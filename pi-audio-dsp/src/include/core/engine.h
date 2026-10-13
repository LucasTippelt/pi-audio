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
 * Pi Audio DSP Engine
 *
 * Audio architecture:
 *   Stereo PGM input  --split--> Mono_L ──┐
 *                                      ├--> woofer-l  (output 0)
 *                                      └--> tweeter-l (output 1)
 *
 *   Stereo PGM input  --split--> Mono_R ──┐
 *                                      ├--> woofer-r  (output 2)
 *                                      └--> tweeter-r (output 3)
 *
 *   Mic inputs (measurement, 4 mono channels):
 *     mic-1, mic-2 -> left side measurement
 *     mic-3, mic-4 -> right side measurement
 *
 *   PGM is suspended (replaced with silence) during test/measurement.
 *
 * Audio flow:
 *   [PGM stereo capture or ring buffer] -> [split L/R]
 *   [mic-1..4 ring buffers]              -> [monitor only]
 *   [L/R mix with per-channel DSP]       -> [4-ch ALSA + monitor buffers]
 */

struct EngineConfig {
    std::string capture = "default";
    std::string playback = "default";
    int sampleRate = 48000;
    int bufferSize = 512;
    int outputChannels = 4;
    int volume = 65;
    bool muted = false;
};

/* DSP processing stage: processes frames in-place per channel. */
using DspStage = std::function<void(float* buffer, size_t frames)>;

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool initialize(const EngineConfig& config);
    void start();
    void stop();
    bool isRunning() const { return m_running.load(); }

    /* Global volume/mute control (applied to all outputs). */
    void setVolume(int volume);
    void setMuted(bool muted);
    int getVolume() const { return m_volume.load(); }
    bool isMuted() const { return m_muted.load(); }

    /* PGM suspension: when true, PGM input is replaced with silence. */
    void setPgmSuspended(bool suspended);
    bool isPgmSuspended() const { return m_pgmSuspended.load(); }

    EngineConfig getCurrentConfig() const;
    int getOutputChannels() const { return m_outputChannels; }

    /* Add a DSP stage per output channel. */
    void addDspStage(DspStage stage);

private:
    void captureThread();
    void playbackThread();
    void processDsp(float* channelBuffer, size_t frames, int channel);

    snd_pcm_t* m_pcmCapture = nullptr;
    snd_pcm_t* m_pcmPlayback = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_pgmSuspended{false};
    std::thread m_captureThread;
    std::thread m_playbackThread;
    EngineConfig m_config;
    mutable std::mutex m_configMutex;
    int m_outputChannels = 4;

    /* Global volume/mute (applied to all outputs). */
    std::atomic<int> m_volume{65};
    std::atomic<bool> m_muted{false};

    /* DSP pipeline: per-channel stages. */
    std::vector<DspStage> m_stages;
    std::mutex m_stagesMutex;


};

#endif
