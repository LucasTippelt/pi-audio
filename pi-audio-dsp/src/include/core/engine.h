#ifndef PI_AUDIO_ENGINE_H
#define PI_AUDIO_ENGINE_H

#include <alsa/asoundlib.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <functional>

/*
 * Lightweight, extendable DSP engine.
 *
 * Phase 1: volume, mute, sample rate, buffer size
 * Phase 2: EQ, delay, trim, phase, limiter, room correction
 *
 * Audio flow:
 *   [input ring buffer or silence] -> [DSP pipeline] -> [ALSA + monitor buffers]
 *
 * The test adapter writes whitenoise to the input ring buffer.
 * The engine reads it, processes through DSP, and outputs to ALSA + monitor.
 */

struct EngineConfig {
    std::string input = "default";
    std::string output = "default";
    int sampleRate = 48000;
    int bufferSize = 512;
    int volume = 65;
    bool muted = false;
};

/* DSP processing stage: processes frames in-place. Extendable for Phase 2. */
using DspStage = std::function<void(float* buffer, size_t frames)>;

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

    EngineConfig getCurrentConfig() const;

    /* Add a DSP stage to the processing pipeline (called before start() or during runtime). */
    void addDspStage(DspStage stage);

    /* Input ring buffer for test adapter to inject audio. */
    /* Returns true if data was consumed, false if buffer was empty. */
    bool inputRead(float* buffer, size_t frames);
    void inputWrite(const float* data, size_t frames);

private:
    void playbackThread();

    /* Run DSP pipeline on buffer (volume/mute + any added stages). */
    void processDsp(float* buffer, size_t frames);

    snd_pcm_t* m_pcm = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_volume{65};
    std::atomic<bool> m_muted{false};
    std::thread m_thread;
    EngineConfig m_config;
    mutable std::mutex m_configMutex;

    /* DSP pipeline: volume/mute is implicit, additional stages are appended. */
    std::vector<DspStage> m_stages;
    std::mutex m_stagesMutex;

    /* Input ring buffer (mono float32, shared with test adapter). */
    static constexpr size_t kInputBufSize = 48000 * 2 * 4; // 2s at 48kHz mono
    std::vector<float> m_inputBuf;
    size_t m_inputReadPos = 0;
    size_t m_inputWritePos = 0;
    std::mutex m_inputMutex;
};

#endif
