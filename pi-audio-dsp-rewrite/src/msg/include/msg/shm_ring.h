#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>

/* ------------------------------------------------------------------ */
/*  Shared memory ring buffer with lifecycle management (E1, E2)      */
/*  Creator owns rings; readers validate before access.               */
/* ------------------------------------------------------------------ */

/* Lifecycle states — published atomically by creator.                */
enum ShmState : uint32_t {
    SHM_STATE_CREATING = 0, /* header being written                  */
    SHM_STATE_READY    = 1, /* data region valid for readers         */
    SHM_STATE_STALE    = 2, /* creator shutting down or resetting    */
};

/* Extended header — uses reserved[20] from legacy layout for new
 * fields. Total size remains 64 bytes (legacy compatible).           */
#pragma pack(push, 1)
struct ShmRingHeader {
    char       magic[4];        /* "PIAI"                              */
    uint16_t   version;         /* header version, currently 2         */
    uint64_t   writePos;        /* byte offset of next write           */
    uint32_t   sampleRate;      /* samples per second                  */
    uint16_t   channels;        /* channel count                       */
    uint32_t   seq;             /* monotonically increasing block seq  */
    uint32_t   generation;      /* E1: incremented on each init()      */
    uint32_t   state;           /* E1: CREATING / READY / STALE        */
    char       name[16];        /* human-readable ring identifier      */
    uint32_t   checksum;        /* E2: XOR over magic+version+gen      */
    uint32_t   reservedPad[3];  /* alignment padding, was reserved[20] */
};
#pragma pack(pop)

static_assert(sizeof(ShmRingHeader) == 64, "header must be 64 bytes");

static const size_t kShmHeaderSize = sizeof(ShmRingHeader);
static const size_t kShmDataSize   = 4UL * 1024 * 1024; /* 4 MB */

/* Compute and return the XOR checksum for critical header fields.
 * Covers magic[4] + version[2] + generation[4].                    */
inline uint32_t computeShmChecksum(const ShmRingHeader* hdr) {
    uint32_t cs = 0;
    /* XOR over magic bytes. */
    const uint8_t* p = reinterpret_cast<const uint8_t*>(hdr->magic);
    for (int i = 0; i < 4; ++i) cs ^= static_cast<uint32_t>(p[i]);
    /* XOR over version bytes. */
    p = reinterpret_cast<const uint8_t*>(&hdr->version);
    for (int i = 0; i < 2; ++i) cs ^= static_cast<uint32_t>(p[i]);
    /* XOR over generation bytes. */
    p = reinterpret_cast<const uint8_t*>(&hdr->generation);
    for (int i = 0; i < 4; ++i) cs ^= static_cast<uint32_t>(p[i]);
    return cs;
}

/* E2: Validate header fields before trusting any data. Returns true
 * only when magic, version, state, bounds, and checksum all pass.    */
inline bool validateShmHeader(const ShmRingHeader* hdr) {
    if (!hdr) return false;

    /* Check magic bytes. */
    if (hdr->magic[0] != 'P' || hdr->magic[1] != 'I' ||
        hdr->magic[2] != 'A' || hdr->magic[3] != 'I') {
        return false;
    }

    /* Version must be >= 1 (forward-compatible). */
    if (hdr->version < 1) return false;

    /* Must be in READY state for normal reads. */
    if (hdr->state != SHM_STATE_READY) return false;

    /* writePos must be within data region. */
    if (hdr->writePos >= kShmDataSize) return false;

    /* E2: Verify XOR checksum over critical fields. */
    uint32_t expected = computeShmChecksum(hdr);
    if (hdr->checksum != expected) return false;

    return true;
}

/* E1: Lifecycle manager — tracks ownership, generation, cleanup.     */
class ShmRingLifecycle {
public:
    ShmRingLifecycle() = default;
    ~ShmRingLifecycle();

    ShmRingLifecycle(const ShmRingLifecycle&) = delete;
    ShmRingLifecycle& operator=(const ShmRingLifecycle&) = delete;

    /* Creator: unlink stale object, create new shm, init header with
     * generation+1, publish READY. Returns 0 on success.              */
    int create(const char* name, uint32_t sampleRate, uint16_t channels);

    /* Reader: validate magic/version/state/generation, return handle. */
    int open(const char* name);

    /* If owner: set STALE, munmap, close fd, unlink.
     * If reader: munmap, close fd only.                              */
    void close();

    /* Increment generation, zero data region, republish READY.        */
    int reset();

    /* Accessors. */
    ShmRingHeader* header() const { return m_hdr; }
    uint8_t* dataPtr() const { return m_data; }
    int fd() const { return m_fd; }
    bool isOwner() const { return m_isOwner; }
    uint32_t generation() const { return m_generation; }

private:
    ShmRingHeader* m_hdr   = nullptr;
    uint8_t*       m_data  = nullptr;
    int            m_fd    = -1;
    bool           m_isOwner = false;
    uint32_t       m_generation = 0;
};
