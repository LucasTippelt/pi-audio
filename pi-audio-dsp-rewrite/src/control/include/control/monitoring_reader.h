#pragma once
#include <cstddef>
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  External monitoring reader interface (D1, 1C)                     */
/*  Header-only contract for the Node.js server and any external      */
/*  process that reads shm rings or polls engine-state.json.          */
/* ------------------------------------------------------------------ */

/* Single monitoring contract:
 *   telemetry -> queue -> StateWriter -> JSON file
 *   audio     -> shm rings -> external reader
 * No network syscalls, HTTP servers, or WebSocket handshakes in the
 * audio process. The external monitoring process reads shm rings for
 * audio data and polls engine-state.json for status.                 */

/* Read mono float32 samples from an shm ring by name. Returns the
 * number of frames actually read (0 on failure).                     */
inline int readShmChannel(const char* name, float* buffer, size_t maxFrames) {
#ifdef __linux__
    #include <sys/mman.h>
    #include <fcntl.h>
    #include <unistd.h>

    /* Full path in /dev/shm (POSIX shm default). */
    char path[64];
    int  plen = snprintf(path, sizeof(path), "/pi-audio-%s", name);
    if (plen <= 0 || plen >= static_cast<int>(sizeof(path))) return -1;

    int fd = shm_open(path + 1, O_RDONLY, 0666); /* skip leading '/' */
    if (fd < 0) {
        /* Try without /pi-audio- prefix — direct name. */
        fd = shm_open(name, O_RDONLY, 0666);
        if (fd < 0) return -1;
    }

    /* We need header + data size to mmap. Read header first with a
     * temporary MAP_SHARED mapping of just the header region.       */
    static const size_t kShmHeaderSize = 80; /* see shm_ring.h */
    static const size_t kShmDataSize   = 4UL * 1024 * 1024;

    void* ptr = mmap(nullptr, kShmHeaderSize + kShmDataSize,
                     PROT_READ, MAP_SHARED, fd, 0);
    close(fd); /* fd no longer needed after mmap. */
    if (ptr == MAP_FAILED) return -1;

    /* Validate header — minimal check for external reader. */
    struct __attribute__((packed)) ShmReadHeader {
        char     magic[4];
        uint16_t version;
        uint64_t writePos;
        uint32_t sampleRate;
        uint16_t channels;
        uint32_t seq;
        uint32_t generation;
        uint32_t state;
        char     name[16];
    };

    const ShmReadHeader* hdr = static_cast<const ShmReadHeader*>(ptr);

    if (std::memcmp(hdr->magic, "PIAI", 4) != 0) {
        munmap(const_cast<void*>(ptr), kShmHeaderSize + kShmDataSize);
        return -1;
    }
    if (hdr->state != 1) { /* SHM_STATE_READY */
        munmap(const_cast<void*>(ptr), kShmHeaderSize + kShmDataSize);
        return -1;
    }

    const float* data = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(ptr) + kShmHeaderSize);

    /* Calculate available frames between reader position and writePos. */
    size_t availBytes = hdr->writePos;
    size_t availFrames = availBytes / (sizeof(float) * hdr->channels);
    if (availFrames > maxFrames) availFrames = maxFrames;

    for (size_t i = 0; i < availFrames; ++i) {
        /* Mono mix: average channels if multi-channel. */
        if (hdr->channels == 1) {
            buffer[i] = data[i];
        } else {
            float sum = 0.f;
            for (uint16_t ch = 0; ch < hdr->channels; ++ch) {
                sum += data[i * hdr->channels + ch];
            }
            buffer[i] = sum / hdr->channels;
        }
    }

    munmap(const_cast<void*>(ptr), kShmHeaderSize + kShmDataSize);
    return static_cast<int>(availFrames);
#else
    (void)name; (void)buffer; (void)maxFrames;
    return -1;
#endif
}

/* Read and parse the latest engine-state.json file. Returns true on
 * success, populating the output struct with parsed fields.           */
struct EngineState {
    uint16_t version;
    uint32_t seq;
    uint32_t xrunUnderrun;
    uint32_t xrunOverrun;
    uint8_t  clockState;
    float    outputLevelDb[2];
    float    inputLevelDb[8];
    uint32_t latencyFrames;
    uint64_t sampleIndex;
};

inline bool readStateFile(const char* path, EngineState* out) {
#ifdef __linux__
    #include <cstdio>
    FILE* f = std::fopen(path, "r");
    if (!f) return false;

    /* Minimal JSON field extraction — no library dependency. */
    char buf[2048] = {};
    size_t len = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (len == 0) return false;

    auto extractInt = [](const char* key, const char* json) -> long {
        char search[64];
        snprintf(search, sizeof(search), "\"%s\":", key);
        const char* p = std::strstr(json, search);
        if (!p) return -1;
        p += strlen(search);
        while (*p == ' ') ++p;
        return std::strtol(p, nullptr, 10);
    };

    auto extractFloat = [](const char* key, const char* json) -> float {
        char search[64];
        snprintf(search, sizeof(search), "\"%s\":", key);
        const char* p = std::strstr(json, search);
        if (!p) return -1.f;
        p += strlen(search);
        while (*p == ' ') ++p;
        return static_cast<float>(std::strtod(p, nullptr));
    };

    out->version      = static_cast<uint16_t>(extractInt("version", buf));
    out->seq          = static_cast<uint32_t>(extractInt("seq", buf));
    out->xrunUnderrun = static_cast<uint32_t>(extractInt("xrunUnderrun", buf));
    out->xrunOverrun  = static_cast<uint32_t>(extractInt("xrunOverrun", buf));
    out->clockState   = static_cast<uint8_t>(extractInt("clockState", buf));
    out->latencyFrames = static_cast<uint32_t>(extractInt("latencyFrames", buf));

    long si = extractInt("sampleIndex", buf);
    out->sampleIndex = (si >= 0) ? static_cast<uint64_t>(si) : 0;

    /* Output levels array: "outputLevelDb": [-12.5, -15.3] */
    {
        const char* p = std::strstr(buf, "\"outputLevelDb\"");
        if (p) {
            const char* bracket = std::strchr(p, '[');
            if (bracket) {
                out->outputLevelDb[0] = static_cast<float>(std::strtod(bracket + 1, nullptr));
                const char* comma = std::strchr(bracket, ',');
                if (comma)
                    out->outputLevelDb[1] = static_cast<float>(std::strtod(comma + 1, nullptr));
            }
        }
    }

    return true;
#else
    (void)path; (void)out;
    return false;
#endif
}
