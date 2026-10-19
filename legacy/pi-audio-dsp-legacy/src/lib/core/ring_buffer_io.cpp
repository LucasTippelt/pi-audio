#include "core/ring_buffer_io.h"
#include "core/shm_ring.h"
#include <cstdio>

static size_t s_readPosPgm = (size_t)-1;

bool createPgmShm(uint32_t sampleRate, int micCount) {
    std::vector<std::string> names;
    names.push_back("pi-audio-pgm");
    for (int i = 0; i < micCount && i < 6; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "pi-audio-mic-%d", i + 1);
        names.push_back(name);
    }
    shmRingManager().init(names, sampleRate, 2);
    s_readPosPgm = (size_t)-1;
    return shmRingManager().m_fds[0] >= 0;
}

void writePgmShm(const float* stereoData, size_t frames) {
    auto& mgr = shmRingManager();
    if (mgr.m_ptrs.empty() || !mgr.m_ptrs[0]) return;
    ShmRingHeader* hdr = static_cast<ShmRingHeader*>(mgr.m_ptrs[0]);
    if (std::memcmp(hdr->magic, "PIAI", 4) != 0) return;

    size_t totalBytes = frames * 2 * sizeof(float);
    size_t pos = hdr->writePos;
    uint8_t* base = static_cast<uint8_t*>(mgr.m_ptrs[0]) + kShmHeaderSize;

    if (pos + totalBytes <= mgr.mDataSize) {
        std::memcpy(base + pos, stereoData, totalBytes);
    } else {
        size_t first = mgr.mDataSize - pos;
        std::memcpy(base + pos, stereoData, first);
        std::memcpy(base, reinterpret_cast<const uint8_t*>(stereoData) + first, totalBytes - first);
    }

    hdr->writePos = (pos + totalBytes) % mgr.mDataSize;
    hdr->seq++;
}

size_t readPgmShm(float* buffer, size_t maxFrames) {
    auto& mgr = shmRingManager();
    if (mgr.m_ptrs.empty() || !mgr.m_ptrs[0]) return 0;
    ShmRingHeader* hdr = static_cast<ShmRingHeader*>(mgr.m_ptrs[0]);
    if (std::memcmp(hdr->magic, "PIAI", 4) != 0) return 0;

    size_t writePos = hdr->writePos;
    if (s_readPosPgm == (size_t)-1) s_readPosPgm = writePos;
    size_t readPos = s_readPosPgm;
    size_t avail = (writePos >= readPos) ? (writePos - readPos) : (mgr.mDataSize - readPos + writePos);
    size_t needed = sizeof(float) * maxFrames * 2;
    if (avail < needed) return 0;

    uint8_t* base = static_cast<uint8_t*>(mgr.m_ptrs[0]) + kShmHeaderSize;
    size_t pos = readPos;
    size_t frames = 0;
    while (frames < maxFrames) {
        size_t segEnd = mgr.mDataSize - (pos % mgr.mDataSize);
        size_t toRead = std::min(maxFrames - frames, segEnd / sizeof(float) / 2) * 2;
        std::memcpy(buffer + frames * 2, base + pos, toRead * sizeof(float));
        pos += toRead * sizeof(float);
        frames += toRead / 2;
    }
    s_readPosPgm = pos % mgr.mDataSize;
    return frames;
}

size_t readMicShm(int idx, float* buffer, size_t maxFrames) {
    auto& mgr = shmRingManager();
    size_t slot = 1 + idx;
    if (mgr.m_ptrs.empty() || slot >= mgr.m_ptrs.size() || !mgr.m_ptrs[slot]) return 0;
    ShmRingHeader* hdr = static_cast<ShmRingHeader*>(mgr.m_ptrs[slot]);
    if (std::memcmp(hdr->magic, "PIAI", 4) != 0) return 0;

    static std::vector<size_t> s_readPos;
    if (s_readPos.size() <= idx) s_readPos.resize(mgr.m_ptrs.size(), (size_t)-1);
    size_t& readPos = s_readPos[slot];

    size_t writePos = hdr->writePos;
    if (readPos == (size_t)-1) readPos = writePos;
    size_t avail = (writePos >= readPos) ? (writePos - readPos) : (mgr.mDataSize - readPos + writePos);
    size_t needed = sizeof(float) * maxFrames;
    if (avail < needed) return 0;

    uint8_t* base = static_cast<uint8_t*>(mgr.m_ptrs[slot]) + kShmHeaderSize;
    size_t pos = readPos;
    size_t frames = 0;
    while (frames < maxFrames) {
        size_t segEnd = mgr.mDataSize - (pos % mgr.mDataSize);
        size_t toRead = std::min(maxFrames - frames, segEnd / sizeof(float));
        std::memcpy(buffer + frames, base + pos, toRead * sizeof(float));
        pos += toRead * sizeof(float);
        frames += toRead;
    }
    readPos = pos % mgr.mDataSize;
    return frames;
}
