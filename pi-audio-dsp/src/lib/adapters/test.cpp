#include "adapters/test.h"
#include "core/logger.h"
#include "core/monitor.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <iostream>
#include <string>
#include <random>
#include <cmath>
#include <climits>
#include <thread>
#include <chrono>

/*
 * Test adapter: writes whitenoise to the engine's input ring buffer.
 * The engine reads it, processes through DSP (volume/mute), and outputs
 * to ALSA + per-channel monitor buffers.
 *
 * Input ring buffer: /run/pi-audio/input.raw
 * Format: 64-byte header + mono float32 data
 * Header: magic[4] "PIAI", writePos[8], sampleRate[4], channels[2], seq[4], reserved[40]
 */

static const char* kInputPath = "/run/pi-audio/input.raw";
static const size_t kInputDataSize = 4 * 1024 * 1024; // 4MB

static std::string driverToChannel(const std::string& driver) {
    if (driver == "Woofer_L") return "woofer-l";
    if (driver == "Tweeter_L") return "tweeter-l";
    if (driver == "Woofer_R") return "woofer-r";
    if (driver == "Tweeter_R") return "tweeter-r";
    if (driver == "Mic") return "mic";
    if (driver == "PGM_L") return "pgm-l";
    if (driver == "PGM_R") return "pgm-r";
    return "";
}

static bool isValidDriver(const std::string& driver) {
    return !driverToChannel(driver).empty();
}

static int writeInputRing(const float* data, size_t frames, uint32_t sampleRate) {
    FILE* f = std::fopen(kInputPath, "r+b");
    if (!f) {
        std::cerr << "Cannot open input ring buffer: " << kInputPath << std::endl;
        return 1;
    }

    char hdr[64];
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(hdr, 1, 64, f) < 64 || std::memcmp(hdr, "PIAI", 4) != 0) {
        std::fclose(f);
        std::cerr << "Invalid input ring buffer header" << std::endl;
        return 1;
    }

    uint32_t wL, wH, seq;
    std::memcpy(&wL, hdr + 4, 4);
    std::memcpy(&wH, hdr + 8, 4);
    std::memcpy(&seq, hdr + 20, 4);
    uint64_t writePos = ((uint64_t)wH << 32) | wL;

    size_t totalBytes = frames * sizeof(float);
    size_t pos = writePos;
    std::fseek(f, 64 + (pos % kInputDataSize), SEEK_SET);

    if (pos + totalBytes <= kInputDataSize) {
        std::fwrite(data, 1, totalBytes, f);
    } else {
        size_t first = kInputDataSize - (pos % kInputDataSize);
        std::fwrite(data, 1, first, f);
        std::fseek(f, 64, SEEK_SET);
        std::fwrite(reinterpret_cast<const char*>(data) + first, 1, totalBytes - first, f);
    }

    writePos = (pos + totalBytes) % kInputDataSize;
    seq++;
    uint32_t low = (uint32_t)writePos;
    uint32_t high = (uint32_t)(writePos >> 32);
    std::memcpy(hdr + 4, &low, 4);
    std::memcpy(hdr + 8, &high, 4);
    std::memcpy(hdr + 20, &seq, 4);
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(hdr, 1, 64, f);
    std::fflush(f);
    std::fclose(f);
    return 0;
}

static int playWhitenoise(const std::string& driver, uint32_t sampleRate, int durationMs) {
    std::string channel = driverToChannel(driver);
    if (channel.empty()) {
        std::cerr << "Invalid driver: " << driver << std::endl;
        return 1;
    }

    Logger::getInstance().info("Generating whitenoise for " + driver +
                                  " -> channel " + channel +
                                  " at " + std::to_string(sampleRate) + " Hz for " +
                                  std::to_string(durationMs) + " ms");

    size_t totalFrames = (sampleRate * static_cast<uint64_t>(durationMs)) / 1000;
    size_t bufferSize = 512;

    std::vector<float> buffer(bufferSize);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-0.3f, 0.3f);

    size_t left = totalFrames;
    size_t written = 0;
    int err = 0;
    while (left > 0) {
        size_t frames = (left < bufferSize) ? left : bufferSize;
        for (size_t i = 0; i < frames; ++i) {
            buffer[i] = dist(rng);
        }
        /* Write to engine input ring buffer so engine processes and outputs. */
        if (!err) err = writeInputRing(buffer.data(), frames, sampleRate);
        written += frames;
        left -= frames;
    }

    Logger::getInstance().info("Whitenoise injected for " + driver +
                                  " channel=" + channel +
                                  " (" + std::to_string(written) + " frames)" +
                                  (err ? ", FAILED" : ", OK"));
    std::cout << "{\"ok\":" << (err ? "false" : "true")
              << ",\"driver\":\"" << driver << "\",\"channel\":\"" << channel
              << "\",\"frames\":" << written << "}";
    return err;
}

int testAdapter(const std::string& driver) {
    Logger::getInstance().info("Test adapter invoked for driver: " + driver);

    if (!isValidDriver(driver)) {
        std::cerr << "Invalid driver: " << driver << std::endl;
        return 1;
    }

    return playWhitenoise(driver, 48000, 3000);
}
