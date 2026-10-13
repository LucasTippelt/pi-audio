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
 * Test adapter: writes whitenoise to ring buffers.
 *
 * For driver outputs (PGM_L, PGM_R, Woofer_*, Tweeter_*):
 *   Writes stereo whitenoise to PGM ring buffer -> engine processes through DSP.
 *
 * For mic inputs (Mic_1, Mic_2, Mic_3, Mic_4):
 *   Writes mono whitenoise to the corresponding mic ring buffer.
 */

static const char* kPgmPath = "/run/pi-audio/pgm.raw";
static const char* kMicPaths[4] = {
    "/run/pi-audio/mic-1.raw",
    "/run/pi-audio/mic-2.raw",
    "/run/pi-audio/mic-3.raw",
    "/run/pi-audio/mic-4.raw"
};
static const size_t kInputDataSize = 4 * 1024 * 1024;

static std::string driverToChannel(const std::string& driver) {
    if (driver == "Woofer_L") return "woofer-l";
    if (driver == "Tweeter_L") return "tweeter-l";
    if (driver == "Woofer_R") return "woofer-r";
    if (driver == "Tweeter_R") return "tweeter-r";
    if (driver == "Mic_1") return "mic-1";
    if (driver == "Mic_2") return "mic-2";
    if (driver == "Mic_3") return "mic-3";
    if (driver == "Mic_4") return "mic-4";
    if (driver == "PGM_L") return "pgm-l";
    if (driver == "PGM_R") return "pgm-r";
    return "";
}

static bool isValidDriver(const std::string& driver) {
    return !driverToChannel(driver).empty();
}

static bool isMicInput(const std::string& driver) {
    return driver == "Mic_1" || driver == "Mic_2" ||
           driver == "Mic_3" || driver == "Mic_4";
}

static int micIndex(const std::string& driver) {
    if (driver == "Mic_1") return 0;
    if (driver == "Mic_2") return 1;
    if (driver == "Mic_3") return 2;
    if (driver == "Mic_4") return 3;
    return -1;
}

/* Write mono data to a ring buffer file. */
static int writeMonoRing(const char* path, const float* data, size_t frames) {
    FILE* f = std::fopen(path, "r+b");
    if (!f) {
        std::cerr << "Cannot open ring buffer: " << path << std::endl;
        return 1;
    }
    char hdr[64];
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(hdr, 1, 64, f) < 64 || std::memcmp(hdr, "PIAI", 4) != 0) {
        std::fclose(f);
        std::cerr << "Invalid ring buffer header: " << path << std::endl;
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
        std::fwrite(data, sizeof(float), frames, f);
    } else {
        size_t first = (kInputDataSize - (pos % kInputDataSize)) / sizeof(float);
        std::fwrite(data, sizeof(float), first, f);
        std::fseek(f, 64, SEEK_SET);
        std::fwrite(data + first, sizeof(float), frames - first, f);
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

/* Write stereo data to PGM ring buffer file. */
static int writeStereoRing(const float* stereoData, size_t frames) {
    FILE* f = std::fopen(kPgmPath, "r+b");
    if (!f) {
        std::cerr << "Cannot open PGM ring buffer: " << kPgmPath << std::endl;
        return 1;
    }
    char hdr[64];
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(hdr, 1, 64, f) < 64 || std::memcmp(hdr, "PIAI", 4) != 0) {
        std::fclose(f);
        std::cerr << "Invalid PGM ring buffer header" << std::endl;
        return 1;
    }
    uint32_t wL, wH, seq;
    std::memcpy(&wL, hdr + 4, 4);
    std::memcpy(&wH, hdr + 8, 4);
    std::memcpy(&seq, hdr + 20, 4);
    uint64_t writePos = ((uint64_t)wH << 32) | wL;
    size_t totalBytes = frames * 2 * sizeof(float);
    size_t pos = writePos;
    std::fseek(f, 64 + (pos % kInputDataSize), SEEK_SET);
    if (pos + totalBytes <= kInputDataSize) {
        std::fwrite(stereoData, sizeof(float), frames * 2, f);
    } else {
        size_t first = (kInputDataSize - (pos % kInputDataSize)) / sizeof(float);
        std::fwrite(stereoData, sizeof(float), first, f);
        std::fseek(f, 64, SEEK_SET);
        std::fwrite(stereoData + first, sizeof(float), frames * 2 - first, f);
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
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-0.3f, 0.3f);

    if (isMicInput(driver)) {
        /* Mic inputs: write mono to individual mic ring buffer. */
        int idx = micIndex(driver);
        if (idx < 0) return 1;
        std::vector<float> buffer(bufferSize);
        size_t left = totalFrames;
        size_t written = 0;
        int err = 0;
        while (left > 0) {
            size_t frames = (left < bufferSize) ? left : bufferSize;
            for (size_t i = 0; i < frames; ++i) {
                buffer[i] = dist(rng);
            }
            if (!err) err = writeMonoRing(kMicPaths[idx], buffer.data(), frames);
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

    /* PGM and driver outputs: write stereo to PGM ring buffer. */
    bool leftOnly = (driver == "PGM_L" || driver == "Woofer_L" || driver == "Tweeter_L");
    bool rightOnly = (driver == "PGM_R" || driver == "Woofer_R" || driver == "Tweeter_R");
    std::vector<float> stereoBuf(bufferSize * 2);
    size_t left = totalFrames;
    size_t written = 0;
    int err = 0;
    while (left > 0) {
        size_t frames = (left < bufferSize) ? left : bufferSize;
        for (size_t i = 0; i < frames; ++i) {
            float val = dist(rng);
            stereoBuf[i * 2] = leftOnly ? val : 0.0f;
            stereoBuf[i * 2 + 1] = rightOnly ? val : 0.0f;
            if (!leftOnly && !rightOnly) {
                stereoBuf[i * 2] = val;
                stereoBuf[i * 2 + 1] = val;
            }
        }
        if (!err) err = writeStereoRing(stereoBuf.data(), frames);
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
