#ifndef PI_AUDIO_ENGINE_H
#define PI_AUDIO_ENGINE_H

#include <alsa/asoundlib.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <functional>
#include <cstdint>
#include "core/http_stream.h"
#include "core/websocket_server.h"

struct EngineConfig {
    std::string capture = "default";
    std::string playback = "default";
    int sampleRate = 48000;
    int bufferSize = 512;
    int outputChannels = 2;
    int micCount = 2;
    int volume = 65;
    bool muted = false;
};

using DspStage = std::function<void(float* buffer, size_t frames)>;

template<size_t Cap>
class SPSCRing {
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
    float m_buf[Cap];
public:
    bool push(const float* data, size_t count) {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t tail = m_tail.load(std::memory_order_acquire);
        size_t free = (tail >= head) ? (Cap - tail + head) : (head - tail);
        if (count > free) return false;
        for (size_t i = 0; i < count; ++i) {
            size_t idx = (head + i) % Cap;
            m_buf[idx] = data[i];
        }
        m_head.store((head + count) % Cap, std::memory_order_release);
        return true;
    }
    size_t pop(float* out, size_t maxCount) {
        size_t tail = m_tail.load(std::memory_order_relaxed);
        size_t head = m_head.load(std::memory_order_acquire);
        size_t avail = (head >= tail) ? (head - tail) : (Cap - tail + head);
        if (avail == 0) return 0;
        size_t take = (avail < maxCount) ? avail : maxCount;
        for (size_t i = 0; i < take; ++i) {
            size_t idx = (tail + i) % Cap;
            out[i] = m_buf[idx];
        }
        m_tail.store((tail + take) % Cap, std::memory_order_release);
        return take;
    }
};

struct RelayChannel {
    static constexpr size_t CAP = 128;
    struct Slot {
        uint64_t seq;
        float samples[512];
    };
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
    Slot m_buf[CAP];

    void push(const float* data, size_t frames) {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t tail = m_tail.load(std::memory_order_acquire);
        size_t next = (head + 1) % CAP;
        if (next == tail) return;
        size_t slot = head;
        for (size_t i = 0; i < frames; ++i) m_buf[slot].samples[i] = data[i];
        m_buf[slot].seq = head + 1;
        m_head.store(next, std::memory_order_release);
    }

    const float* tryRead(size_t& outFrames) {
        size_t tail = m_tail.load(std::memory_order_relaxed);
        size_t head = m_head.load(std::memory_order_acquire);
        if (tail == head) return nullptr;
        outFrames = 512;
        m_tail.store((tail + 1) % CAP, std::memory_order_release);
        return m_buf[tail].samples;
    }
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool initialize(const EngineConfig& config);
    void start();
    void stop();
    bool isRunning() const { return m_running.load(); }

    void setVolume(int volume);
    void setMuted(bool muted);
    int getVolume() const { return m_volume.load(); }
    bool isMuted() const { return m_muted.load(); }

    void setPgmSuspended(bool suspended);
    bool isPgmSuspended() const { return m_pgmSuspended.load(); }

    EngineConfig getCurrentConfig() const;
    int getOutputChannels() const { return m_outputChannels; }

    void addDspStageTweeter(DspStage stage);
    void addDspStageWoofer(DspStage stage);

    void setMonitorChannel(uint8_t channelId);
    uint8_t getMonitorChannel() const { return m_monitorChannel.load(); }

    HttpStreamServer& httpServer() { return m_httpServer; }
    WebSocketServer& wsServer() { return m_wsServer; }

private:
    void playbackThread();
    void captureThread();
    void pgmReaderThread();
    void micReaderThread(int idx);
    void relayThread();
    void monitorThread();
    void processDspTweeter(float* buffer, size_t frames);
    void processDspWoofer(float* buffer, size_t frames);

    snd_pcm_t* m_pcmCapture = nullptr;
    snd_pcm_t* m_pcmPlayback = nullptr;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_pgmSuspended{false};
    std::atomic<int> m_volume{65};
    std::atomic<bool> m_muted{false};
    std::atomic<uint8_t> m_monitorChannel{0};
    int m_outputChannels = 2;
    int m_micCount = 2;
    EngineConfig m_config;
    mutable std::mutex m_configMutex;

    std::thread m_captureThread;
    std::thread m_playbackThread;
    std::thread m_pgmReaderThread;
    std::thread m_micReaderThreads[6];
    std::thread m_relayThread;
    std::thread m_monitorThread;
    std::atomic<bool> m_relayRunning{false};
    std::atomic<bool> m_monitorRunning{false};

    std::vector<DspStage> m_stagesTweeterActive;
    std::vector<DspStage> m_stagesTweeterNext;
    std::mutex m_stagesTweeterMutex;

    std::vector<DspStage> m_stagesWooferActive;
    std::vector<DspStage> m_stagesWooferNext;
    std::mutex m_stagesWooferMutex;

    static constexpr size_t RING_CAP = 4096;
    SPSCRing<RING_CAP> m_pgmLeftRing;
    SPSCRing<RING_CAP> m_pgmRightRing;
    SPSCRing<RING_CAP> m_micRing[6];

    std::vector<float> m_outputBuf;
    std::vector<float> m_pgmLeft;
    std::vector<float> m_pgmRight;
    std::vector<float> m_tweeterBuf;
    std::vector<float> m_wooferBuf;
    std::vector<float> m_micBuf[6];

    RelayChannel m_relayCh[10];
    RelayChannel m_monitorCh[10];

    HttpStreamServer m_httpServer;
    WebSocketServer m_wsServer;
};

#endif
