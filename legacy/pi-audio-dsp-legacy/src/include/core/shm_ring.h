#ifndef PI_AUDIO_SHM_RING_H
#define PI_AUDIO_SHM_RING_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#pragma pack(push, 1)
struct ShmRingHeader {
    char    magic[4];       /* "PIAI" */
    uint16_t version;       /* 1 */
    uint64_t writePos;      /* byte offset of next write (0..dataSize-1) */
    uint32_t sampleRate;
    uint16_t channels;
    uint32_t seq;
    char    name[16];
    char    reserved[20];
};
#pragma pack(pop)

static const size_t kShmHeaderSize = sizeof(ShmRingHeader);
static const size_t kShmDataSize   = 4 * 1024 * 1024;

class ShmRingManager {
public:
    std::vector<int>   m_fds;
    std::vector<void*> m_ptrs;
    std::vector<uint64_t> m_writePos;
    std::vector<uint32_t> m_sampleRates;
    size_t mDataSize = kShmDataSize;

    void init(const std::vector<std::string>& names, uint32_t sampleRate, uint16_t channels = 1) {
        size_t n = names.size();
        m_fds.resize(n, -1);
        m_ptrs.resize(n, nullptr);
        m_writePos.resize(n, 0);
        m_sampleRates.resize(n, sampleRate);

        for (size_t i = 0; i < n; ++i) {
            shm_unlink(names[i].c_str());
            int fd = shm_open(names[i].c_str(), O_CREAT | O_RDWR, 0666);
            if (fd < 0) continue;
            ftruncate(fd, kShmHeaderSize + mDataSize);
            void* ptr = mmap(nullptr, kShmHeaderSize + mDataSize,
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (ptr == MAP_FAILED) { close(fd); fd = -1; continue; }

            ShmRingHeader* hdr = static_cast<ShmRingHeader*>(ptr);
            std::memset(hdr, 0, kShmHeaderSize);
            std::memcpy(hdr->magic, "PIAI", 4);
            hdr->version = 1;
            hdr->writePos = 0;
            hdr->sampleRate = sampleRate;
            hdr->channels = channels;
            hdr->seq = 0;
            std::strncpy(hdr->name, names[i].c_str(), sizeof(hdr->name) - 1);
            std::memset(static_cast<uint8_t*>(ptr) + kShmHeaderSize, 0, mDataSize);
            fchmod(fd, 0666);

            m_fds[i] = fd;
            m_ptrs[i] = ptr;
        }
    }

    bool openExisting(const std::vector<std::string>& names) {
        size_t n = names.size();
        m_fds.resize(n, -1);
        m_ptrs.resize(n, nullptr);
        m_writePos.resize(n, 0);
        m_sampleRates.resize(n, 0);

        for (size_t i = 0; i < n; ++i) {
            int fd = shm_open(names[i].c_str(), O_RDWR, 0666);
            if (fd < 0) { cleanup(); return false; }
            void* ptr = mmap(nullptr, kShmHeaderSize + mDataSize,
                              PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (ptr == MAP_FAILED) { close(fd); cleanup(); return false; }
            ShmRingHeader* hdr = static_cast<ShmRingHeader*>(ptr);
            if (std::memcmp(hdr->magic, "PIAI", 4) != 0) {
                munmap(ptr, kShmHeaderSize + mDataSize);
                close(fd);
                cleanup();
                return false;
            }
            m_sampleRates[i] = hdr->sampleRate;
            m_fds[i] = fd;
            m_ptrs[i] = ptr;
        }
        return true;
    }

    void cleanup() {
        for (size_t i = 0; i < m_ptrs.size(); ++i) {
            if (m_ptrs[i]) munmap(m_ptrs[i], kShmHeaderSize + mDataSize);
            if (m_fds[i] >= 0) close(m_fds[i]);
        }
        m_ptrs.clear();
        m_fds.clear();
        m_writePos.clear();
        m_sampleRates.clear();
    }
};

inline ShmRingManager& shmRingManager() {
    static ShmRingManager inst;
    return inst;
}

inline void createShmRings(const std::vector<std::string>& names,
                            uint32_t sampleRate, uint16_t channels = 1) {
    shmRingManager().init(names, sampleRate, channels);
}

#endif
