#include "core/engine.h"
#include "core/logger.h"
#include "core/monitor.h"
#include <alsa/asoundlib.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <iostream>
#include <chrono>
#include <thread>

static const char* kInputPath = "/run/pi-audio/input.raw";
static const size_t kInputDataSize = 4 * 1024 * 1024;

/* Persistent monitor state for writeMonitorBuffers. */
struct MonitorPersist {
    FILE* fpL = nullptr;
    FILE* fpR = nullptr;
    uint64_t writePosL = 0;
    uint64_t writePosR = 0;
    bool initialized = false;
    std::vector<float> monoBuf;

    void open() {
        if (initialized) return;
        if (fpL) std::fclose(fpL);
        if (fpR) std::fclose(fpR);
        fpL = nullptr;
        fpR = nullptr;
        writePosL = 0;
        writePosR = 0;

        fpL = std::fopen(monitorChannelPath("pgm-l").c_str(), "r+b");
        fpR = std::fopen(monitorChannelPath("pgm-r").c_str(), "r+b");
        if (fpL) {
            std::fseek(fpL, 0, SEEK_SET);
            char hdr[64];
            std::fread(hdr, 1, 64, fpL);
            uint32_t wL, wH;
            std::memcpy(&wL, hdr + 6, 4);
            std::memcpy(&wH, hdr + 10, 4);
            writePosL = ((uint64_t)wH << 32) | wL;
        }
        if (fpR) {
            std::fseek(fpR, 0, SEEK_SET);
            char hdr[64];
            std::fread(hdr, 1, 64, fpR);
            uint32_t wL, wH;
            std::memcpy(&wL, hdr + 6, 4);
            std::memcpy(&wH, hdr + 10, 4);
            writePosR = ((uint64_t)wH << 32) | wL;
        }
        initialized = true;
    }

    void close() {
        if (fpL) { std::fclose(fpL); fpL = nullptr; }
        if (fpR) { std::fclose(fpR); fpR = nullptr; }
        initialized = false;
    }
};

static MonitorPersist& monitorPersist() {
    static MonitorPersist inst;
    return inst;
}

AudioEngine::AudioEngine() {
    m_inputBuf.resize(kInputBufSize / sizeof(float), 0.0f);
}

AudioEngine::~AudioEngine() {
    stop();
    if (m_pcm) {
        snd_pcm_close(m_pcm);
    }
}

static bool createInputFile() {
    std::remove(kInputPath);
    FILE* f = std::fopen(kInputPath, "wb");
    if (!f) return false;

    /* Write header: magic[4] "PIAI", writePos[8], sampleRate[4], channels[2], seq[4], reserved[40] */
    char hdr[64] = {0};
    std::memcpy(hdr, "PIAI", 4);
    uint32_t sr = 48000;
    uint16_t ch = 1;
    std::memcpy(hdr + 14, &sr, 4);
    std::memcpy(hdr + 18, &ch, 2);
    std::fwrite(hdr, 1, 64, f);

    /* Pre-allocate data area. */
    std::vector<char> zero(65536, 0);
    size_t left = kInputDataSize;
    while (left > 0) {
        size_t chunk = (zero.size() < left) ? zero.size() : left;
        std::fwrite(zero.data(), 1, chunk, f);
        left -= chunk;
    }
    std::fflush(f);
    fchmod(fileno(f), 0666);
    std::fclose(f);
    return true;
}

bool AudioEngine::initialize(const EngineConfig& config) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_config = config;

    int err;

    err = snd_pcm_open(&m_pcm, config.output.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        Logger::getInstance().error("Failed to open PCM device: " + std::string(snd_strerror(err)));
        return false;
    }

    snd_pcm_set_params(m_pcm,
                      SND_PCM_FORMAT_FLOAT_LE,
                      SND_PCM_ACCESS_RW_INTERLEAVED,
                      2,
                      config.sampleRate,
                      1,
                      config.bufferSize * 1000);

    m_volume.store(config.volume);
    m_muted.store(config.muted);

    /* Create input ring buffer file. */
    if (!createInputFile()) {
        Logger::getInstance().warning("Cannot create input buffer " + std::string(kInputPath));
    } else {
        Logger::getInstance().info("Input buffer created: " + std::string(kInputPath));
    }

    /* Create PGM monitor ring buffers if they don't exist. */
    monitorPersist().close();
    monitorInit("pgm-l", config.sampleRate);
    monitorClose();
    monitorInit("pgm-r", config.sampleRate);
    monitorClose();

    Logger::getInstance().info("Engine initialized: " +
                                std::to_string(config.sampleRate) + " Hz, " +
                                std::to_string(config.bufferSize) + " frames, " +
                                "volume=" + std::to_string(config.volume) +
                                ", muted=" + std::to_string(config.muted));

    return true;
}

void AudioEngine::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread(&AudioEngine::playbackThread, this);
}

void AudioEngine::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    monitorPersist().close();
}

/* Read mono samples from input ring buffer file.
 * Tracks read position internally to avoid re-reading consumed data. */
static size_t readInputFile(float* buffer, size_t maxFrames) {
    static size_t s_readPos = (size_t)-1; /* -1 means not yet initialized */
    static uint32_t s_lastSeq = 0;

    FILE* f = std::fopen(kInputPath, "rb");
    if (!f) return 0;

    char hdr[64] = {0};
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(hdr, 1, 64, f) < 64 || std::memcmp(hdr, "PIAI", 4) != 0) {
        std::fclose(f);
        return 0;
    }

    uint32_t wL, wH, seq;
    std::memcpy(&wL, hdr + 4, 4);
    std::memcpy(&wH, hdr + 8, 4);
    std::memcpy(&seq, hdr + 20, 4);
    uint64_t writePos = ((uint64_t)wH << 32) | wL;

    /* On first call (or after engine restart), sync readPos to current writePos
     * so we don't consume stale data left over from a previous session. */
    if (s_readPos == (size_t)-1) {
        s_readPos = writePos;
    }

    /* Calculate available frames between readPos and writePos. */
    size_t readPos = s_readPos;
    size_t avail;
    if (writePos >= readPos) {
        avail = writePos - readPos;
    } else {
        avail = kInputDataSize - readPos + writePos;
    }

    if (avail < sizeof(float) * maxFrames) {
        std::fclose(f);
        if (avail == 0 && s_readPos != (size_t)-1) {
            /* Periodic debug: log positions when buffer is empty */
            static size_t debugCount = 0;
            if (++debugCount % 1000 == 0) {
                Logger::getInstance().debug("readInputFile: empty readPos=" +
                    std::to_string(s_readPos) + " writePos=" +
                    std::to_string(writePos) + " avail=" + std::to_string(avail));
            }
        }
        return 0;
    }

    /* Read up to maxFrames from readPos. */
    size_t pos = readPos;
    size_t frames = 0;
    while (frames < maxFrames) {
        size_t segEnd = kInputDataSize - (pos % kInputDataSize);
        size_t toRead = std::min(maxFrames - frames, segEnd / sizeof(float));

        std::fseek(f, 64 + (pos % kInputDataSize), SEEK_SET);
        size_t read = std::fread(buffer + frames, sizeof(float), toRead, f);
        if (read == 0) break;

        pos += read * sizeof(float);
        frames += read;
    }

    s_readPos = pos % kInputDataSize;
    Logger::getInstance().debug("readInputFile: read " + std::to_string(frames) +
        " frames, readPos=" + std::to_string(s_readPos) + " writePos=" +
        std::to_string(writePos));
    s_lastSeq = seq;
    std::fclose(f);
    return frames;
}

/* Write processed stereo audio to PGM monitor buffers.
 * Persistent file handles to avoid open/close I/O every buffer period.
 * Phase 1: writes to pgm-l and pgm-r only.
 * Phase 2: will split by crossover (woofer/tweeter per side). */
static void writeMonitorBuffers(const float* stereo, size_t frames) {
    MonitorPersist& mp = monitorPersist();
    if (!mp.initialized) {
        mp.open();
        Logger::getInstance().info("writeMonitorBuffers: persistent handles opened fpL=" +
            std::string(mp.fpL ? "yes" : "NO") + " fpR=" + std::string(mp.fpR ? "yes" : "NO"));
    }

    const size_t kDataSize = 4 * 1024 * 1024;
    const size_t kHeaderSize = 64;

    if (mp.monoBuf.size() < frames) mp.monoBuf.resize(frames);

    auto writeMono = [&](FILE* fp, size_t chOffset, uint64_t& writePos) {
        if (!fp) return;
        for (size_t i = 0; i < frames; ++i) mp.monoBuf[i] = stereo[i * 2 + chOffset];

        size_t totalBytes = frames * sizeof(float);
        size_t pos = writePos;
        std::fseek(fp, static_cast<long>(kHeaderSize + pos), SEEK_SET);

        if (pos + totalBytes <= kDataSize) {
            std::fwrite(mp.monoBuf.data(), 1, totalBytes, fp);
        } else {
            size_t first = kDataSize - pos;
            std::fwrite(mp.monoBuf.data(), 1, first, fp);
            std::fseek(fp, static_cast<long>(kHeaderSize), SEEK_SET);
            std::fwrite(mp.monoBuf.data() + (first / sizeof(float)), 1, totalBytes - first, fp);
        }
        writePos = (pos + totalBytes) % kDataSize;

        std::fseek(fp, 0, SEEK_SET);
        char hdr[64];
        std::fread(hdr, 1, 64, fp);
        uint32_t wLo = (uint32_t)(writePos & 0xFFFFFFFF);
        uint32_t wHi = (uint32_t)(writePos >> 32);
        std::memcpy(hdr + 6, &wLo, 4);
        std::memcpy(hdr + 10, &wHi, 4);
        uint32_t seq = 0;
        std::memcpy(&seq, hdr + 20, 4);
        seq++;
        std::memcpy(hdr + 20, &seq, 4);
        std::fseek(fp, 0, SEEK_SET);
        std::fwrite(hdr, 1, 64, fp);
        std::fflush(fp);
    };

    writeMono(mp.fpL, 0, mp.writePosL);
    writeMono(mp.fpR, 1, mp.writePosR);

    static size_t monDebugCount = 0;
    if (++monDebugCount % 100 == 0) {
        Logger::getInstance().debug("writeMonitorBuffers: writePosL=" +
            std::to_string(mp.writePosL) + " writePosR=" + std::to_string(mp.writePosR) +
            " fpL=" + std::string(mp.fpL ? "ok" : "null") +
            " fpR=" + std::string(mp.fpR ? "ok" : "null"));
    }
}

void AudioEngine::playbackThread() {
    size_t channels = 2;
    std::vector<float> buffer(m_config.bufferSize * channels);

    while (m_running.load()) {
        /* Pace at real-time: sleep for buffer duration before processing.
         * This ensures the loop runs at exactly sampleRate frames/second
         * regardless of how fast ALSA accepts writes (null device issue). */
        std::this_thread::sleep_for(std::chrono::microseconds(
            static_cast<int64_t>(m_config.bufferSize * 1000000LL / m_config.sampleRate)));

        /* Read from input ring buffer file; if empty, use silence. */
        size_t inputFrames = readInputFile(buffer.data(), m_config.bufferSize);
        if (inputFrames == 0) {
            std::memset(buffer.data(), 0, buffer.size() * sizeof(float));
            inputFrames = m_config.bufferSize;
        }

        /* Mono-to-stereo: duplicate input to both channels. */
        {
            std::vector<float> mono(inputFrames);
            for (size_t i = 0; i < inputFrames; ++i) {
                mono[i] = buffer[i];
            }
            for (size_t i = 0; i < inputFrames; ++i) {
                buffer[i * 2]     = mono[i];
                buffer[i * 2 + 1] = mono[i];
            }
        }

        /* Run DSP pipeline (volume/mute + added stages). */
        processDsp(buffer.data(), inputFrames);

        /* Write to ALSA. */
        snd_pcm_sframes_t written = snd_pcm_writei(m_pcm, buffer.data(), inputFrames);
        if (written < 0) {
            snd_pcm_recover(m_pcm, written, 0);
        }

        /* Write processed audio to PGM monitor buffers. */
        writeMonitorBuffers(buffer.data(), inputFrames);
    }
}

void AudioEngine::processDsp(float* buffer, size_t frames) {
    /* Phase 1: volume/mute. */
    float volumeFactor = m_muted.load() ? 0.0f : (m_volume.load() / 100.0f);
    for (size_t i = 0; i < frames * 2; ++i) {
        buffer[i] *= volumeFactor;
    }

    /* Phase 2: additional DSP stages (EQ, delay, trim, etc.). */
    std::lock_guard<std::mutex> lock(m_stagesMutex);
    for (auto& stage : m_stages) {
        stage(buffer, frames);
    }
}

void AudioEngine::setVolume(int volume) {
    m_volume.store(volume);
}

void AudioEngine::setMuted(bool muted) {
    m_muted.store(muted);
}

EngineConfig AudioEngine::getCurrentConfig() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_config;
}

void AudioEngine::addDspStage(DspStage stage) {
    std::lock_guard<std::mutex> lock(m_stagesMutex);
    m_stages.push_back(std::move(stage));
}

bool AudioEngine::inputRead(float* buffer, size_t frames) {
    std::lock_guard<std::mutex> lock(m_inputMutex);
    size_t bufFrames = m_inputBuf.size();
    size_t avail = (m_inputWritePos >= m_inputReadPos)
        ? (m_inputWritePos - m_inputReadPos)
        : (bufFrames - m_inputReadPos + m_inputWritePos);

    if (avail < frames) return false;

    for (size_t i = 0; i < frames; ++i) {
        size_t idx = (m_inputReadPos + i) % bufFrames;
        buffer[i] = m_inputBuf[idx];
        m_inputBuf[idx] = 0.0f;
    }
    m_inputReadPos = (m_inputReadPos + frames) % bufFrames;
    return true;
}

void AudioEngine::inputWrite(const float* data, size_t frames) {
    std::lock_guard<std::mutex> lock(m_inputMutex);
    size_t bufFrames = m_inputBuf.size();

    for (size_t i = 0; i < frames; ++i) {
        size_t next = (m_inputWritePos + i) % bufFrames;
        if (next == m_inputReadPos) return;
        m_inputBuf[next] = data[i];
    }
    m_inputWritePos = (m_inputWritePos + frames) % bufFrames;
}
