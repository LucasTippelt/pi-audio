#ifndef PI_AUDIO_ENGINE_H
#define PI_AUDIO_ENGINE_H

#include <alsa/asoundlib.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

struct EngineConfig {
    std::string input = "default";
    std::string output = "default";
    int sampleRate = 48000;
    int bufferSize = 512;
    int volume = 65;
    bool muted = false;
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

    EngineConfig getCurrentConfig() const;

private:
    void playbackThread();
    void processSamples(float* buffer, size_t frames);

    snd_pcm_t* m_pcm = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_volume{65};
    std::atomic<bool> m_muted{false};
    std::thread m_thread;
    EngineConfig m_config;
    mutable std::mutex m_configMutex;
};

#endif
