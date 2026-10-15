#ifndef PI_AUDIO_MONITOR_WRITER_H
#define PI_AUDIO_MONITOR_WRITER_H

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "core/shm_ring.h"

struct MonitorWriter {
    struct ChannelState {
        int fd = -1;
        uint8_t* base = nullptr;
        uint64_t writePos = 0;
        std::vector<float> accum;
    };
    std::map<std::string, ChannelState> channels;
    bool initialized = false;
    size_t flushThreshold = 9600;

    void open() {
        if (initialized) return;
        std::vector<std::string> names = {
            "pgm-l", "pgm-r", "tweeter", "woofer",
            "mic-1", "mic-2", "mic-3", "mic-4", "mic-5", "mic-6"
        };
        for (auto& name : names) {
            auto& ch = channels[name];
            std::string shmName = "pi-audio-monitor-" + name;
            ch.fd = shm_open(shmName.c_str(), O_RDWR, 0);
            if (ch.fd < 0) continue;
            void* ptr = mmap(nullptr, kShmHeaderSize + kShmDataSize,
                             PROT_READ | PROT_WRITE, MAP_SHARED, ch.fd, 0);
            if (ptr == MAP_FAILED) { ::close(ch.fd); ch.fd = -1; continue; }
            ch.base = static_cast<uint8_t*>(ptr) + kShmHeaderSize;
            ShmRingHeader* hdr = static_cast<ShmRingHeader*>(ptr);
            ch.writePos = hdr->writePos;
        }
        initialized = true;
    }

    void shutdown() {
        flush();
        for (auto& pair : channels) {
            auto& ch = pair.second;
            if (ch.base) munmap(ch.base - kShmHeaderSize, kShmHeaderSize + kShmDataSize);
            if (ch.fd >= 0) ::close(ch.fd);
            ch.fd = -1;
            ch.base = nullptr;
            ch.accum.clear();
        }
        initialized = false;
    }

    void writeChannel(uint8_t* base, uint64_t& writePos, const float* data, size_t frames) {
        size_t totalBytes = frames * sizeof(float);
        size_t pos = writePos;
        if (pos + totalBytes <= kShmDataSize) {
            std::memcpy(base + pos, data, totalBytes);
        } else {
            size_t first = kShmDataSize - pos;
            std::memcpy(base + pos, data, first);
            std::memcpy(base, reinterpret_cast<const uint8_t*>(data) + first, totalBytes - first);
        }
        writePos = (pos + totalBytes) % kShmDataSize;
        ShmRingHeader* hdr = reinterpret_cast<ShmRingHeader*>(
            const_cast<uint8_t*>(base) - kShmHeaderSize);
        hdr->writePos = writePos;
        hdr->seq++;
    }

    void accumulate(const std::string& name, const float* data, size_t frames) {
        auto& ch = channels[name];
        ch.accum.insert(ch.accum.end(), data, data + frames);
    }

    void flush() {
        for (auto& pair : channels) {
            auto& ch = pair.second;
            if (ch.base && !ch.accum.empty()) {
                writeChannel(ch.base, ch.writePos, ch.accum.data(), ch.accum.size());
                ch.accum.clear();
            }
        }
    }

    bool shouldFlush() {
        for (auto& pair : channels) {
            if (pair.second.accum.size() >= flushThreshold) return true;
        }
        return false;
    }
};

inline MonitorWriter& monitorWriter() {
    static MonitorWriter inst;
    return inst;
}

#endif
