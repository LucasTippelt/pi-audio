#include "adapters/test.h"
#include "core/logger.h"
#include "core/shm_ring.h"
#include "core/test_inject.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <random>
#include <thread>
#include <chrono>

static std::string driverToChannel(const std::string& driver) {
    if (driver == "Tweeter") return "tweeter";
    if (driver == "Woofer") return "woofer";
    if (driver == "PGM_L") return "pgm-l";
    if (driver == "PGM_R") return "pgm-r";
    for (int i = 1; i <= 6; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "Mic_%d", i);
        if (driver == name) {
            char ch[16];
            snprintf(ch, sizeof(ch), "mic-%d", i);
            return ch;
        }
    }
    return "";
}

static int writePgmShmTest(const float* stereoData, size_t frames) {
    auto& mgr = shmRingManager();
    if (mgr.m_ptrs.empty()) {
        std::vector<std::string> names;
        names.push_back("pi-audio-pgm");
        if (!mgr.openExisting(names)) return 1;
    }
    if (!mgr.m_ptrs[0]) return 1;

    ShmRingHeader* hdr = static_cast<ShmRingHeader*>(mgr.m_ptrs[0]);
    if (std::memcmp(hdr->magic, "PIAI", 4) != 0) return 1;

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
    return 0;
}

static int writeMicShmTest(int idx, const float* data, size_t frames) {
    auto& mgr = shmRingManager();
    size_t slot = 1 + idx;

    if (mgr.m_ptrs.empty() || slot >= mgr.m_ptrs.size() || !mgr.m_ptrs[slot]) {
        std::vector<std::string> names;
        names.push_back("pi-audio-pgm");
        for (int i = 1; i <= 6; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "pi-audio-mic-%d", i);
            names.push_back(name);
        }
        if (!mgr.openExisting(names)) return 1;
    }

    if (!mgr.m_ptrs[slot]) return 1;

    ShmRingHeader* hdr = static_cast<ShmRingHeader*>(mgr.m_ptrs[slot]);
    if (std::memcmp(hdr->magic, "PIAI", 4) != 0) return 1;

    size_t totalBytes = frames * sizeof(float);
    size_t pos = hdr->writePos;
    uint8_t* base = static_cast<uint8_t*>(mgr.m_ptrs[slot]) + kShmHeaderSize;

    if (pos + totalBytes <= mgr.mDataSize) {
        std::memcpy(base + pos, data, totalBytes);
    } else {
        size_t first = mgr.mDataSize - pos;
        std::memcpy(base + pos, data, first);
        std::memcpy(base, reinterpret_cast<const uint8_t*>(data) + first, totalBytes - first);
    }
    hdr->writePos = (pos + totalBytes) % mgr.mDataSize;
    hdr->seq++;
    return 0;
}

int testAdapter(const std::string& driver) {
    Logger::getInstance().info("Test adapter invoked for driver: " + driver);
    std::string channel = driverToChannel(driver);
    if (channel.empty()) {
        std::cerr << "Invalid driver: " << driver << std::endl;
        return 1;
    }

    uint32_t sampleRate = 48000;
    int durationMs = 3000;
    size_t totalFrames = (sampleRate * static_cast<uint64_t>(durationMs)) / 1000;
    size_t bufferSize = 512;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-0.3f, 0.3f);

    /* Mic tests: write to mic ring buffer */
    if (driver.find("Mic_") == 0) {
        int idx = std::stoi(driver.substr(4)) - 1;
        if (idx < 0 || idx >= 6) return 1;
        std::vector<float> buffer(bufferSize);
        size_t left = totalFrames;
        int err = 0;
        while (left > 0) {
            size_t frames = (left < bufferSize) ? left : bufferSize;
            for (size_t i = 0; i < frames; ++i) buffer[i] = dist(rng);
            if (!err) err = writeMicShmTest(idx, buffer.data(), frames);
            left -= frames;
        }
        std::cout << "{\"ok\":" << (err ? "false" : "true") << ",\"driver\":\"" << driver << "\"}";
        return err;
    }

    /* PGM tests: write to PGM stereo ring buffer (feeds both outputs) */
    if (driver == "PGM_L" || driver == "PGM_R") {
        bool rightOnly = (driver == "PGM_R");
        std::vector<float> stereoBuf(bufferSize * 2);
        size_t left = totalFrames;
        int err = 0;
        while (left > 0) {
            size_t frames = (left < bufferSize) ? left : bufferSize;
            for (size_t i = 0; i < frames; ++i) {
                float val = dist(rng);
                stereoBuf[i * 2] = rightOnly ? 0.0f : val;
                stereoBuf[i * 2 + 1] = rightOnly ? val : 0.0f;
            }
            if (!err) err = writePgmShmTest(stereoBuf.data(), frames);
            left -= frames;
        }
        std::cout << "{\"ok\":" << (err ? "false" : "true") << ",\"driver\":\"" << driver << "\",\"channel\":\"" << channel << "\"}";
        return err;
    }

    /* Tweeter/Woofer tests: write to per-channel test injection buffer */
    {
        std::string shmName = (driver == "Tweeter") ? "pi-audio-test-tweeter" : "pi-audio-test-woofer";
        std::vector<float> buffer(bufferSize);
        size_t left = totalFrames;
        int err = 0;
        while (left > 0) {
            size_t frames = (left < bufferSize) ? left : bufferSize;
            for (size_t i = 0; i < frames; ++i) buffer[i] = dist(rng);
            if (!err) err = writeTestInject(shmName, buffer.data(), frames);
            left -= frames;
        }
        std::cout << "{\"ok\":" << (err ? "false" : "true") << ",\"driver\":\"" << driver << "\",\"channel\":\"" << channel << "\"}";
        return err;
    }
}
