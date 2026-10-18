#include "msg/shm_ring.h"

#ifdef __linux__
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <cstdio>
#else
/* Stub for non-Linux builds — shm not available. */
#endif

ShmRingLifecycle::~ShmRingLifecycle() {
    close();
}

int ShmRingLifecycle::create(const char* name, uint32_t sampleRate, uint16_t channels) {
#ifdef __linux__
    /* Close any existing mapping first. */
    if (m_hdr) {
        munmap(m_hdr, kShmHeaderSize + kShmDataSize);
        m_hdr = nullptr;
        m_data = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }

    /* Build shm object name — use /pi-audio- prefix for namespace. */
    char shmName[64];
    int len = snprintf(shmName, sizeof(shmName), "/pi-audio-%s", name);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(shmName)) {
        return -1; /* name too long */
    }

    /* Unlink stale object. */
    shm_unlink(shmName);

    /* Create new shared memory object. */
    m_fd = shm_open(shmName, O_CREAT | O_RDWR, 0666);
    if (m_fd < 0) {
        std::fprintf(stderr, "[shm] failed to create %s: errno=%d\n", shmName, errno);
        return -1;
    }

    /* Set size. */
    if (ftruncate(m_fd, static_cast<off_t>(kShmHeaderSize + kShmDataSize)) < 0) {
        std::fprintf(stderr, "[shm] ftruncate failed for %s\n", shmName);
        ::close(m_fd);
        m_fd = -1;
        return -1;
    }

    /* Map into address space. */
    void* ptr = mmap(nullptr, kShmHeaderSize + kShmDataSize,
                     PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
    if (ptr == MAP_FAILED) {
        std::fprintf(stderr, "[shm] mmap failed for %s\n", shmName);
        ::close(m_fd);
        m_fd = -1;
        return -1;
    }

    /* Increment generation. */
    ++m_generation;

    /* Initialize header — start in CREATING state. */
    std::memset(ptr, 0, kShmHeaderSize + kShmDataSize);
    m_hdr = static_cast<ShmRingHeader*>(ptr);
    m_data = static_cast<uint8_t*>(ptr) + kShmHeaderSize;

    std::memcpy(m_hdr->magic, "PIAI", 4);
    m_hdr->version    = 2;
    m_hdr->writePos   = 0;
    m_hdr->sampleRate = sampleRate;
    m_hdr->channels   = channels;
    m_hdr->seq        = 0;
    m_hdr->generation = m_generation;
    m_hdr->state      = SHM_STATE_CREATING;
    std::strncpy(m_hdr->name, name, sizeof(m_hdr->name) - 1);
    m_hdr->name[sizeof(m_hdr->name) - 1] = '\0';

    /* Compute checksum over critical fields. */
    m_hdr->checksum = computeShmChecksum(m_hdr);

    /* Memory barrier before publishing READY state. */
    std::atomic_thread_fence(std::memory_order_release);

    /* Publish as READY — readers will now see valid data. */
    m_hdr->state = SHM_STATE_READY;

    fchmod(m_fd, 0666);
    m_isOwner = true;

    return 0;
#else
    (void)name; (void)sampleRate; (void)channels;
    return -1;
#endif
}

int ShmRingLifecycle::open(const char* name) {
#ifdef __linux__
    /* Close any existing mapping. */
    if (m_hdr) {
        munmap(m_hdr, kShmHeaderSize + kShmDataSize);
        m_hdr = nullptr;
        m_data = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }

    /* Try with prefix first, then bare name. */
    char shmName[64];
    int len = snprintf(shmName, sizeof(shmName), "/pi-audio-%s", name);
    const char* tryName = (len > 0 && static_cast<size_t>(len) < sizeof(shmName))
                          ? shmName : name;

    m_fd = shm_open(tryName + (tryName[0] == '/' ? 0 : 0), O_RDONLY, 0666);
    if (m_fd < 0) {
        /* Try bare name as fallback. */
        m_fd = shm_open(name, O_RDONLY, 0666);
    }
    if (m_fd < 0) {
        std::fprintf(stderr, "[shm] failed to open %s: errno=%d\n", name, errno);
        return -1;
    }

    void* ptr = mmap(nullptr, kShmHeaderSize + kShmDataSize,
                     PROT_READ, MAP_SHARED, m_fd, 0);
    if (ptr == MAP_FAILED) {
        std::fprintf(stderr, "[shm] mmap (read) failed for %s\n", name);
        ::close(m_fd);
        m_fd = -1;
        return -1;
    }

    m_hdr = static_cast<ShmRingHeader*>(ptr);
    m_data = static_cast<uint8_t*>(ptr) + kShmHeaderSize;

    /* E2: Validate header before trusting data. */
    if (!validateShmHeader(m_hdr)) {
        std::fprintf(stderr, "[shm] header validation failed for %s\n", name);
        munmap(m_hdr, kShmHeaderSize + kShmDataSize);
        ::close(m_fd);
        m_hdr = nullptr;
        m_data = nullptr;
        m_fd = -1;
        return -1;
    }

    m_generation = m_hdr->generation;
    m_isOwner = false;

    return 0;
#else
    (void)name;
    return -1;
#endif
}

void ShmRingLifecycle::close() {
#ifdef __linux__
    if (m_hdr) {
        if (m_isOwner) {
            /* Set STALE before cleanup so readers detect shutdown. */
            m_hdr->state = SHM_STATE_STALE;
            std::atomic_thread_fence(std::memory_order_release);

            /* Unlink the shm object. */
            char shmName[64];
            if (m_hdr->name[0]) {
                snprintf(shmName, sizeof(shmName), "/pi-audio-%s", m_hdr->name);
                shm_unlink(shmName);
            }
        }
        munmap(m_hdr, kShmHeaderSize + kShmDataSize);
        m_hdr = nullptr;
        m_data = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
#else
    m_hdr = nullptr;
    m_data = nullptr;
    m_fd = -1;
#endif
    m_isOwner = false;
}

int ShmRingLifecycle::reset() {
#ifdef __linux__
    if (!m_hdr || !m_isOwner) return -1;

    /* Transition to CREATING, increment generation, zero data. */
    m_hdr->state = SHM_STATE_CREATING;
    ++m_generation;
    m_hdr->generation = m_generation;
    m_hdr->writePos   = 0;
    m_hdr->seq        = 0;

    /* Zero the data region. */
    std::memset(m_data, 0, kShmDataSize);

    /* Recompute checksum with new generation. */
    m_hdr->checksum = computeShmChecksum(m_hdr);

    /* Memory barrier before republishing READY. */
    std::atomic_thread_fence(std::memory_order_release);
    m_hdr->state = SHM_STATE_READY;

    return 0;
#else
    return -1;
#endif
}
