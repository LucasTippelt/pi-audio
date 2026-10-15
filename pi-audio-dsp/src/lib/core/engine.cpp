#include "core/engine.h"
#include "core/logger.h"
#include "core/tcp_relay.h"
#include "core/monitor_writer.h"
#include "core/ring_buffer_io.h"
#include "core/shm_ring.h"
#include "core/test_inject.h"
#include <alsa/asoundlib.h>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <iostream>
#include <utility>

AudioEngine::AudioEngine() {}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::initialize(const EngineConfig& config) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_config = config;
    int err;

    std::string captureDev = config.capture;
    std::string playbackDev = config.playback;
    if (captureDev == "default" || captureDev.empty()) captureDev = "plughw:0,0";
    if (playbackDev == "default" || playbackDev.empty()) playbackDev = "plughw:0,0";

    err = snd_pcm_open(&m_pcmCapture, captureDev.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        Logger::getInstance().warning("Cannot open capture '" + captureDev + "': " +
            std::string(snd_strerror(err)) + " - PGM will use ring buffer only");
        m_pcmCapture = nullptr;
    } else {
        err = snd_pcm_set_params(m_pcmCapture, SND_PCM_FORMAT_FLOAT_LE,
            SND_PCM_ACCESS_RW_INTERLEAVED, 2, config.sampleRate, 1, config.bufferSize * 1000);
        if (err < 0) {
            Logger::getInstance().error("Capture params: " + std::string(snd_strerror(err)));
            snd_pcm_close(m_pcmCapture);
            m_pcmCapture = nullptr;
        } else {
            Logger::getInstance().info("Capture: " + captureDev + " (stereo, " +
                std::to_string(config.sampleRate) + " Hz)");
        }
    }

    err = snd_pcm_open(&m_pcmPlayback, playbackDev.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        Logger::getInstance().error("Playback open: " + std::string(snd_strerror(err)));
        return false;
    }
    err = snd_pcm_set_params(m_pcmPlayback, SND_PCM_FORMAT_FLOAT_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED, config.outputChannels, config.sampleRate,
        1, config.bufferSize * 1000);
    if (err < 0) {
        Logger::getInstance().error("Playback params (" + std::to_string(config.outputChannels) +
            "ch): " + std::string(snd_strerror(err)));
        snd_pcm_close(m_pcmPlayback);
        m_pcmPlayback = nullptr;
        return false;
    }

    m_outputChannels = config.outputChannels;
    m_micCount = (config.micCount >= 2 && config.micCount <= 6) ? config.micCount : 2;
    m_volume.store(config.volume);
    m_muted.store(config.muted);

    size_t bs = config.bufferSize;
    m_outputBuf.resize(bs * config.outputChannels);
    m_pgmLeft.resize(bs);
    m_pgmRight.resize(bs);
    m_tweeterBuf.resize(bs);
    m_wooferBuf.resize(bs);
    for (int i = 0; i < 6; ++i) {
        m_micBuf[i].resize(bs);
    }

    createPgmShm(config.sampleRate, m_micCount);

    monitorWriter().shutdown();
    std::vector<std::string> monitorChannels = {
        "pgm-l", "pgm-r", "tweeter", "woofer"
    };
    for (int i = 0; i < m_micCount; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "mic-%d", i + 1);
        monitorChannels.push_back(name);
    }
    for (auto& ch : monitorChannels) {
        std::string shmName = "pi-audio-monitor-" + ch;
        shm_unlink(shmName.c_str());
        int fd = shm_open(shmName.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd >= 0) {
            ftruncate(fd, kShmHeaderSize + kShmDataSize);
            void* ptr = mmap(nullptr, kShmHeaderSize + kShmDataSize,
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (ptr != MAP_FAILED) {
                auto* hdr = static_cast<ShmRingHeader*>(ptr);
                std::memset(hdr, 0, kShmHeaderSize);
                std::memcpy(hdr->magic, "PIAI", 4);
                hdr->version = 1;
                hdr->sampleRate = config.sampleRate;
                hdr->channels = 1;
                std::strncpy(hdr->name, ch.c_str(), sizeof(hdr->name) - 1);
                std::memset(static_cast<uint8_t*>(ptr) + kShmHeaderSize, 0, kShmDataSize);
                munmap(ptr, kShmHeaderSize + kShmDataSize);
            }
            fchmod(fd, 0666);
            close(fd);
        }
    }

    /* Pre-create test injection shared memory buffers so the playback
     * thread doesn't log ENOENT on every buffer cycle after reboot. */
    for (const char* testName : {"pi-audio-test-tweeter", "pi-audio-test-woofer"}) {
        mode_t oldUmask = umask(0);
        int fd = shm_open(testName, O_CREAT | O_RDWR, 0666);
        umask(oldUmask);
        if (fd >= 0) {
            ftruncate(fd, kTestInjectHeaderSize + kTestInjectDataSize);
            void* ptr = mmap(nullptr, kTestInjectHeaderSize + kTestInjectDataSize,
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (ptr != MAP_FAILED) {
                auto* hdr = static_cast<TestInjectHeader*>(ptr);
                std::memset(hdr, 0, kTestInjectHeaderSize);
                std::memcpy(hdr->magic, "PIIJ", 4);
                munmap(ptr, kTestInjectHeaderSize + kTestInjectDataSize);
            }
            fchmod(fd, 0666);
            close(fd);
        }
    }

    tcpRelay().disconnect();
    tcpRelay().connect("127.0.0.1", 8766);

    m_httpServer.stop();
    m_httpServer.setBufferSize(config.bufferSize);
    m_httpServer.setStatusCallback([this]() -> std::string {
        auto cfg = getCurrentConfig();
        return "{\"sampleRate\":" + std::to_string(cfg.sampleRate) +
            ",\"BufferSize\":" + std::to_string(cfg.bufferSize) +
            ",\"volume\":" + std::to_string(getVolume()) +
            ",\"muted\":" + (isMuted() ? "true" : "false") + "}";
    });
    if (!m_httpServer.start("0.0.0.0", 8767)) {
        Logger::getInstance().warning("HTTP stream server failed to start on :8767");
    }

    m_wsServer.stop();
    m_wsServer.setSampleRate(config.sampleRate);
    m_wsServer.setBufferSize(config.bufferSize);
    m_wsServer.setConfigCallback([this]() -> std::pair<int, int> {
        auto cfg = getCurrentConfig();
        return {cfg.sampleRate, cfg.bufferSize};
    });
    if (!m_wsServer.start("0.0.0.0", 8768)) {
        Logger::getInstance().warning("WebSocket server failed to start on :8768");
    }

    Logger::getInstance().info("Engine initialized: " +
        std::to_string(config.sampleRate) + " Hz, " +
        std::to_string(config.bufferSize) + " frames, " +
        std::to_string(config.outputChannels) + " outputs, " +
        std::to_string(m_micCount) + " mics");
    return true;
}

void AudioEngine::start() {
    if (m_running.exchange(true)) return;
    if (m_pcmCapture)
        m_captureThread = std::thread(&AudioEngine::captureThread, this);
    m_pgmReaderThread = std::thread(&AudioEngine::pgmReaderThread, this);
    for (int i = 0; i < m_micCount; ++i)
        m_micReaderThreads[i] = std::thread(&AudioEngine::micReaderThread, this, i);
    m_relayRunning.store(true);
    m_relayThread = std::thread(&AudioEngine::relayThread, this);
    m_monitorRunning.store(true);
    m_monitorThread = std::thread(&AudioEngine::monitorThread, this);
    m_playbackThread = std::thread(&AudioEngine::playbackThread, this);
}

void AudioEngine::stop() {
    if (!m_running.exchange(false)) return;
    m_relayRunning.store(false);
    m_monitorRunning.store(false);
    if (m_pcmPlayback) { snd_pcm_close(m_pcmPlayback); m_pcmPlayback = nullptr; }
    if (m_pcmCapture) { snd_pcm_close(m_pcmCapture); m_pcmCapture = nullptr; }
    tcpRelay().disconnect();
    m_httpServer.stop();
    m_wsServer.stop();
    if (m_playbackThread.joinable()) m_playbackThread.join();
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_pgmReaderThread.joinable()) m_pgmReaderThread.join();
    for (int i = 0; i < m_micCount; ++i)
        if (m_micReaderThreads[i].joinable()) m_micReaderThreads[i].join();
    if (m_relayThread.joinable()) m_relayThread.join();
    if (m_monitorThread.joinable()) m_monitorThread.join();
    monitorWriter().shutdown();
}

void AudioEngine::setVolume(int volume) { m_volume.store(volume); }
void AudioEngine::setMuted(bool muted) { m_muted.store(muted); }

void AudioEngine::setPgmSuspended(bool suspended) {
    m_pgmSuspended.store(suspended);
    Logger::getInstance().info("PGM " + std::string(suspended ? "suspended" : "resumed"));
}

void AudioEngine::setMonitorChannel(uint8_t channelId) {
    m_monitorChannel.store(channelId);
    Logger::getInstance().info("Monitor channel set to " + std::to_string(channelId));
}

EngineConfig AudioEngine::getCurrentConfig() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    EngineConfig cfg = m_config;
    cfg.volume = m_volume.load();
    cfg.muted = m_muted.load();
    return cfg;
}

void AudioEngine::addDspStageTweeter(DspStage stage) {
    std::lock_guard<std::mutex> lock(m_stagesTweeterMutex);
    m_stagesTweeterNext.push_back(std::move(stage));
    std::swap(m_stagesTweeterActive, m_stagesTweeterNext);
    m_stagesTweeterNext.clear();
}

void AudioEngine::addDspStageWoofer(DspStage stage) {
    std::lock_guard<std::mutex> lock(m_stagesWooferMutex);
    m_stagesWooferNext.push_back(std::move(stage));
    std::swap(m_stagesWooferActive, m_stagesWooferNext);
    m_stagesWooferNext.clear();
}
