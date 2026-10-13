#include "core/engine.h"
#include "core/logger.h"
#include "core/monitor.h"
#include "core/tcp_relay.h"
#include <alsa/asoundlib.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <map>

static const char* kPgmPath = "/run/pi-audio/pgm.raw";
static const char* kMicPaths[4] = {
    "/run/pi-audio/mic-1.raw",
    "/run/pi-audio/mic-2.raw",
    "/run/pi-audio/mic-3.raw",
    "/run/pi-audio/mic-4.raw"
};
static const size_t kInputDataSize = 4 * 1024 * 1024;

/* Persistent monitor state for writeMonitorBuffers. */
struct MonitorPersist {
    struct ChannelState {
        FILE* fp = nullptr;
        uint64_t writePos = 0;
        std::vector<float> accum;
    };
    std::map<std::string, ChannelState> channels;
    bool initialized = false;
    size_t flushThreshold = 9600;

    void open() {
        if (initialized) return;
        std::vector<std::string> chNames = {
            "pgm-l", "pgm-r",
            "woofer-l", "tweeter-l", "woofer-r", "tweeter-r",
            "mic-1", "mic-2", "mic-3", "mic-4"
        };
        for (auto& name : chNames) {
            auto& ch = channels[name];
            ch.fp = std::fopen(monitorChannelPath(name).c_str(), "r+b");
            if (ch.fp) {
                std::fseek(ch.fp, 0, SEEK_SET);
                char hdr[64];
                std::fread(hdr, 1, 64, ch.fp);
                uint32_t wL, wH;
                std::memcpy(&wL, hdr + 6, 4);
                std::memcpy(&wH, hdr + 10, 4);
                ch.writePos = ((uint64_t)wH << 32) | wL;
            }
        }
        initialized = true;
    }

    void close() {
        flush();
        for (auto& pair : channels) {
            if (pair.second.fp) { std::fclose(pair.second.fp); pair.second.fp = nullptr; }
            pair.second.accum.clear();
        }
        initialized = false;
    }

    void writeChannel(FILE* fp, uint64_t& writePos, const float* data, size_t frames) {
        size_t totalBytes = frames * sizeof(float);
        size_t pos = writePos;
        std::fseek(fp, 64 + (pos % kInputDataSize), SEEK_SET);
        if (pos + totalBytes <= kInputDataSize) {
            std::fwrite(data, sizeof(float), frames, fp);
        } else {
            size_t first = (kInputDataSize - (pos % kInputDataSize)) / sizeof(float);
            std::fwrite(data, sizeof(float), first, fp);
            std::fseek(fp, 64, SEEK_SET);
            std::fwrite(data + first, sizeof(float), frames - first, fp);
        }
        writePos = (pos + totalBytes) % kInputDataSize;
        char hdr[64];
        std::fseek(fp, 0, SEEK_SET);
        std::fread(hdr, 1, 64, fp);
        uint32_t low = (uint32_t)writePos;
        uint32_t high = (uint32_t)(writePos >> 32);
        std::memcpy(hdr + 6, &low, 4);
        std::memcpy(hdr + 10, &high, 4);
        uint32_t seq = 0;
        std::memcpy(&seq, hdr + 20, 4);
        seq++;
        std::memcpy(hdr + 20, &seq, 4);
        std::fseek(fp, 0, SEEK_SET);
        std::fwrite(hdr, 1, 64, fp);
        std::fflush(fp);
    }

    void accumulate(const std::string& name, const float* data, size_t frames) {
        auto& ch = channels[name];
        ch.accum.reserve(ch.accum.size() + frames);
        for (size_t i = 0; i < frames; ++i) {
            ch.accum.push_back(data[i]);
        }
    }

    void flush() {
        for (auto& pair : channels) {
            auto& ch = pair.second;
            if (ch.fp && !ch.accum.empty()) {
                writeChannel(ch.fp, ch.writePos, ch.accum.data(), ch.accum.size());
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

static MonitorPersist& monitorPersist() {
    static MonitorPersist inst;
    return inst;
}

static TcpRelay& tcpRelay() {
    static TcpRelay inst;
    return inst;
}

/* --- PGM stereo ring buffer file I/O --- */

static bool createPgmFile() {
    std::remove(kPgmPath);
    FILE* f = std::fopen(kPgmPath, "wb");
    if (!f) return false;
    char hdr[64] = {0};
    std::memcpy(hdr, "PIAI", 4);
    uint32_t sr = 48000;
    uint16_t ch = 2;
    std::memcpy(hdr + 14, &sr, 4);
    std::memcpy(hdr + 18, &ch, 2);
    std::fwrite(hdr, 1, 64, f);
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

/* Read interleaved stereo frames from PGM ring buffer file. */
static size_t readPgmFile(float* buffer, size_t maxFrames) {
    static size_t s_readPos = (size_t)-1;
    FILE* f = std::fopen(kPgmPath, "rb");
    if (!f) return 0;
    char hdr[64] = {0};
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(hdr, 1, 64, f) < 64 || std::memcmp(hdr, "PIAI", 4) != 0) {
        std::fclose(f);
        return 0;
    }
    uint32_t wL, wH;
    std::memcpy(&wL, hdr + 4, 4);
    std::memcpy(&wH, hdr + 8, 4);
    uint64_t writePos = ((uint64_t)wH << 32) | wL;
    if (s_readPos == (size_t)-1) {
        s_readPos = writePos;
    }
    size_t readPos = s_readPos;
    size_t avail;
    if (writePos >= readPos) {
        avail = writePos - readPos;
    } else {
        avail = kInputDataSize - readPos + writePos;
    }
    size_t needed = sizeof(float) * maxFrames * 2;
    if (avail < needed) {
        std::fclose(f);
        return 0;
    }
    size_t pos = readPos;
    size_t frames = 0;
    while (frames < maxFrames) {
        size_t segEnd = kInputDataSize - (pos % kInputDataSize);
        size_t toRead = std::min(maxFrames - frames, segEnd / sizeof(float) / 2) * 2;
        std::fseek(f, 64 + (pos % kInputDataSize), SEEK_SET);
        size_t read = std::fread(buffer + frames * 2, sizeof(float), toRead, f);
        if (read == 0) break;
        pos += read * sizeof(float);
        frames += read / 2;
    }
    s_readPos = pos % kInputDataSize;
    std::fclose(f);
    return frames;
}

/* Read mono frames from a mic ring buffer file. */
static size_t readMicFile(const char* path, float* buffer, size_t maxFrames) {
    static std::map<std::string, size_t> s_readPos;
    auto it = s_readPos.find(path);
    if (it == s_readPos.end()) {
        s_readPos[path] = (size_t)-1;
        it = s_readPos.find(path);
    }
    size_t& readPos = it->second;

    FILE* f = std::fopen(path, "rb");
    if (!f) return 0;
    char hdr[64] = {0};
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(hdr, 1, 64, f) < 64 || std::memcmp(hdr, "PIAI", 4) != 0) {
        std::fclose(f);
        return 0;
    }
    uint32_t wL, wH;
    std::memcpy(&wL, hdr + 4, 4);
    std::memcpy(&wH, hdr + 8, 4);
    uint64_t writePos = ((uint64_t)wH << 32) | wL;
    if (readPos == (size_t)-1) {
        readPos = writePos;
    }
    size_t avail;
    if (writePos >= readPos) {
        avail = writePos - readPos;
    } else {
        avail = kInputDataSize - readPos + writePos;
    }
    size_t needed = sizeof(float) * maxFrames;
    if (avail < needed) {
        std::fclose(f);
        return 0;
    }
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
    readPos = pos % kInputDataSize;
    std::fclose(f);
    return frames;
}

/* --- Mic ring buffer file creation --- */
static void createMicFiles() {
    for (int i = 0; i < 4; ++i) {
        std::remove(kMicPaths[i]);
        FILE* f = std::fopen(kMicPaths[i], "wb");
        if (!f) continue;
        char hdr[64] = {0};
        std::memcpy(hdr, "PIAI", 4);
        uint32_t sr = 48000;
        uint16_t ch = 1;
        char name[16];
        snprintf(name, sizeof(name), "mic-%d", i + 1);
        std::memcpy(hdr + 14, &sr, 4);
        std::memcpy(hdr + 18, &ch, 2);
        std::memcpy(hdr + 24, name, strlen(name));
        std::fwrite(hdr, 1, 64, f);
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
        Logger::getInstance().info("Mic buffer created: " + std::string(kMicPaths[i]));
    }
}

/* --- Engine implementation --- */

AudioEngine::AudioEngine() {}

AudioEngine::~AudioEngine() {
    stop();
    if (m_pcmCapture) {
        snd_pcm_close(m_pcmCapture);
    }
    if (m_pcmPlayback) {
        snd_pcm_close(m_pcmPlayback);
    }
}

bool AudioEngine::initialize(const EngineConfig& config) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_config = config;

    int err;

    /* Open capture device for stereo PGM input. */
    err = snd_pcm_open(&m_pcmCapture, config.capture.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        Logger::getInstance().warning("Cannot open capture device '" +
            config.capture + "': " + std::string(snd_strerror(err)) +
            " — PGM will use ring buffer only");
        m_pcmCapture = nullptr;
    } else {
        err = snd_pcm_set_params(m_pcmCapture,
            SND_PCM_FORMAT_FLOAT_LE,
            SND_PCM_ACCESS_RW_INTERLEAVED,
            2,
            config.sampleRate,
            1,
            config.bufferSize * 1000);
        if (err < 0) {
            Logger::getInstance().error("Failed to set capture params: " +
                std::string(snd_strerror(err)));
            snd_pcm_close(m_pcmCapture);
            m_pcmCapture = nullptr;
        } else {
            Logger::getInstance().info("Capture device opened: " + config.capture +
                " (stereo, " + std::to_string(config.sampleRate) + " Hz)");
        }
    }

    /* Open playback device for 4-channel output. */
    err = snd_pcm_open(&m_pcmPlayback, config.playback.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        Logger::getInstance().error("Failed to open PCM device: " + std::string(snd_strerror(err)));
        return false;
    }

    err = snd_pcm_set_params(m_pcmPlayback,
        SND_PCM_FORMAT_FLOAT_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        config.outputChannels,
        config.sampleRate,
        1,
        config.bufferSize * 1000);
    if (err < 0) {
        Logger::getInstance().error("Failed to set playback params (" +
            std::to_string(config.outputChannels) + "ch): " + std::string(snd_strerror(err)));
        snd_pcm_close(m_pcmPlayback);
        m_pcmPlayback = nullptr;
        return false;
    }

    m_outputChannels = config.outputChannels;
    m_volume.store(config.volume);
    m_muted.store(config.muted);

    /* Create PGM stereo ring buffer file. */
    if (!createPgmFile()) {
        Logger::getInstance().warning("Cannot create PGM buffer " + std::string(kPgmPath));
    } else {
        Logger::getInstance().info("PGM buffer created: " + std::string(kPgmPath));
    }

    /* Create mic ring buffer files. */
    createMicFiles();

    /* Create per-channel monitor ring buffers. */
    monitorPersist().close();
    std::vector<std::string> monitorChannels = {
        "pgm-l", "pgm-r", "woofer-l", "tweeter-l", "woofer-r", "tweeter-r",
        "mic-1", "mic-2", "mic-3", "mic-4"
    };
    for (auto& ch : monitorChannels) {
        monitorInit(ch, config.sampleRate);
        monitorClose();
    }

    /* Connect TCP relay. */
    tcpRelay().disconnect();
    if (!tcpRelay().connect("127.0.0.1", 8766)) {
        Logger::getInstance().warning("TcpRelay: initial connect failed (will retry on first push)");
    }

    Logger::getInstance().info("Engine initialized: " +
        std::to_string(config.sampleRate) + " Hz, " +
        std::to_string(config.bufferSize) + " frames, " +
        std::to_string(config.outputChannels) + " outputs");

    return true;
}

void AudioEngine::start() {
    if (m_running.exchange(true)) return;

    /* Start capture thread if capture device is available. */
    if (m_pcmCapture) {
        m_captureThread = std::thread(&AudioEngine::captureThread, this);
    }

    m_playbackThread = std::thread(&AudioEngine::playbackThread, this);
}

void AudioEngine::stop() {
    if (!m_running.exchange(false)) return;
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
    if (m_playbackThread.joinable()) {
        m_playbackThread.join();
    }
    monitorPersist().close();
    tcpRelay().disconnect();
}

/* Write stereo frames to PGM ring buffer file. */
static void writePgmFile(const float* stereoData, size_t frames) {
    FILE* f = std::fopen(kPgmPath, "r+b");
    if (!f) return;
    char hdr[64];
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(hdr, 1, 64, f) < 64 || std::memcmp(hdr, "PIAI", 4) != 0) {
        std::fclose(f);
        return;
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
}

/* Capture thread: read stereo PGM from ALSA capture device, write to ring buffer file. */
void AudioEngine::captureThread() {
    Logger::getInstance().info("Capture thread started");
    while (m_running.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(
            static_cast<int64_t>(m_config.bufferSize * 1000000LL / m_config.sampleRate)));

        std::vector<float> captureBuf(m_config.bufferSize * 2, 0.0f);
        snd_pcm_sframes_t frames = snd_pcm_readi(m_pcmCapture, captureBuf.data(), m_config.bufferSize);

        if (frames < 0) {
            snd_pcm_recover(m_pcmCapture, frames, 0);
            frames = 0;
        }

        if (frames > 0) {
            writePgmFile(captureBuf.data(), (size_t)frames);
        }
    }
}

/* Playback thread: read PGM stereo, split L/R, apply per-channel DSP, output to ALSA. */
void AudioEngine::playbackThread() {
    int channels = m_outputChannels;
    size_t bs = m_config.bufferSize;
    std::vector<float> outputBuf(bs * channels, 0.0f);
    std::vector<float> pgmStereo(bs * 2, 0.0f);
    std::vector<float> pgmLeft(bs, 0.0f);
    std::vector<float> pgmRight(bs, 0.0f);
    std::vector<float> micBuf[4];
    for (int i = 0; i < 4; ++i) {
        micBuf[i].resize(bs, 0.0f);
    }
    /* Per-channel mono buffers for DSP processing. */
    std::vector<float> chBuf[4];
    for (int i = 0; i < 4; ++i) {
        chBuf[i].resize(bs);
    }

    /* Initialize absolute timing: start one period from now */
    struct timespec nextTs;
    clock_gettime(CLOCK_MONOTONIC, &nextTs);
    long periodNs = (long)(bs * 1000000000LL / m_config.sampleRate);
    nextTs.tv_nsec += periodNs % 1000000000;
    if (nextTs.tv_nsec >= 1000000000) { nextTs.tv_sec++; nextTs.tv_nsec -= 1000000000; }

    while (m_running.load()) {
        /* Absolute timing: sleep until next period, so jitter doesn't compound */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec < nextTs.tv_sec || (now.tv_sec == nextTs.tv_sec && now.tv_nsec <= nextTs.tv_nsec)) {
            struct timespec rem = {0, 0};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextTs, &rem);
        }
        /* Advance nextTs by one period */
        nextTs.tv_nsec += periodNs % 1000000000;
        if (nextTs.tv_nsec >= 1000000000) { nextTs.tv_sec++; nextTs.tv_nsec -= 1000000000; }

        std::fill(outputBuf.begin(), outputBuf.end(), 0.0f);
        std::fill(pgmLeft.begin(), pgmLeft.end(), 0.0f);
        std::fill(pgmRight.begin(), pgmRight.end(), 0.0f);

        /* Read PGM stereo from ring buffer file. */
        if (!m_pgmSuspended.load()) {
            size_t pgmFrames = readPgmFile(pgmStereo.data(), bs);
            if (pgmFrames > 0) {
                for (size_t i = 0; i < pgmFrames; ++i) {
                    pgmLeft[i] = pgmStereo[i * 2];
                    pgmRight[i] = pgmStereo[i * 2 + 1];
                }
            }
        }

        /* Read mic inputs from ring buffer files. */
        for (int i = 0; i < 4; ++i) {
            std::fill(micBuf[i].begin(), micBuf[i].end(), 0.0f);
            readMicFile(kMicPaths[i], micBuf[i].data(), bs);
        }

        /* Route: PGM split into L/R, each drives 2 outputs.
         *   chBuf[0] (woofer-l)  = pgmLeft
         *   chBuf[1] (tweeter-l) = pgmLeft
         *   chBuf[2] (woofer-r)  = pgmRight
         *   chBuf[3] (tweeter-r) = pgmRight
         */
        for (size_t i = 0; i < bs; ++i) {
            chBuf[0][i] = pgmLeft[i];
            chBuf[1][i] = pgmLeft[i];
            chBuf[2][i] = pgmRight[i];
            chBuf[3][i] = pgmRight[i];
        }

        /* Per-channel DSP (volume/mute + stages). */
        for (int ch = 0; ch < 4; ++ch) {
            processDsp(chBuf[ch].data(), bs, ch);
        }

        /* Interleave channels for ALSA output. */
        if (channels >= 4) {
            for (size_t i = 0; i < bs; ++i) {
                outputBuf[i * 4 + 0] = chBuf[0][i];
                outputBuf[i * 4 + 1] = chBuf[1][i];
                outputBuf[i * 4 + 2] = chBuf[2][i];
                outputBuf[i * 4 + 3] = chBuf[3][i];
            }
        } else {
            for (size_t i = 0; i < bs; ++i) {
                outputBuf[i * 2 + 0] = chBuf[0][i];
                outputBuf[i * 2 + 1] = chBuf[2][i];
            }
        }

        /* Write to ALSA. */
        snd_pcm_sframes_t written = snd_pcm_writei(m_pcmPlayback, outputBuf.data(), bs);
        if (written < 0) {
            snd_pcm_recover(m_pcmPlayback, written, 0);
        }

        /* Write monitor buffers. */
        {
            MonitorPersist& mp = monitorPersist();
            if (!mp.initialized) mp.open();
            mp.accumulate("pgm-l", pgmLeft.data(), bs);
            mp.accumulate("pgm-r", pgmRight.data(), bs);
            mp.accumulate("woofer-l", chBuf[0].data(), bs);
            mp.accumulate("tweeter-l", chBuf[1].data(), bs);
            mp.accumulate("woofer-r", chBuf[2].data(), bs);
            mp.accumulate("tweeter-r", chBuf[3].data(), bs);
            mp.accumulate("mic-1", micBuf[0].data(), bs);
            mp.accumulate("mic-2", micBuf[1].data(), bs);
            mp.accumulate("mic-3", micBuf[2].data(), bs);
            mp.accumulate("mic-4", micBuf[3].data(), bs);
            if (mp.shouldFlush()) mp.flush();
        }

        /* Push to TCP relay. */
        tcpRelay().pushChannel(CH_PGM_L, pgmLeft.data(), bs);
        tcpRelay().pushChannel(CH_PGM_R, pgmRight.data(), bs);
        tcpRelay().pushChannel(CH_WOOFER_L, chBuf[0].data(), bs);
        tcpRelay().pushChannel(CH_TWEETER_L, chBuf[1].data(), bs);
        tcpRelay().pushChannel(CH_WOOFER_R, chBuf[2].data(), bs);
        tcpRelay().pushChannel(CH_TWEETER_R, chBuf[3].data(), bs);
        tcpRelay().pushChannel(CH_MIC_1, micBuf[0].data(), bs);
        tcpRelay().pushChannel(CH_MIC_2, micBuf[1].data(), bs);
        tcpRelay().pushChannel(CH_MIC_3, micBuf[2].data(), bs);
        tcpRelay().pushChannel(CH_MIC_4, micBuf[3].data(), bs);
    }
}

void AudioEngine::processDsp(float* buffer, size_t frames, int channel) {
    float volumeFactor = m_muted.load() ? 0.0f : (m_volume.load() / 100.0f);
    for (size_t i = 0; i < frames; ++i) {
        buffer[i] *= volumeFactor;
    }
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

void AudioEngine::setPgmSuspended(bool suspended) {
    m_pgmSuspended.store(suspended);
    Logger::getInstance().info("PGM " + std::string(suspended ? "suspended" : "resumed"));
}

EngineConfig AudioEngine::getCurrentConfig() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    EngineConfig cfg = m_config;
    cfg.volume = m_volume.load();
    cfg.muted = m_muted.load();
    return cfg;
}

void AudioEngine::addDspStage(DspStage stage) {
    std::lock_guard<std::mutex> lock(m_stagesMutex);
    m_stages.push_back(std::move(stage));
}
