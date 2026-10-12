#include "core/engine.h"
#include "core/logger.h"
#include <alsa/asoundlib.h>
#include <cmath>
#include <cstring>
#include <iostream>

AudioEngine::AudioEngine() {}

AudioEngine::~AudioEngine() {
    stop();
    if (m_pcm) {
        snd_pcm_close(m_pcm);
    }
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
                      2, // channels
                      config.sampleRate,
                      1,
                      config.bufferSize * 1000); // convert to us
    
    m_volume.store(config.volume);
    m_muted.store(config.muted);
    
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
}

void AudioEngine::playbackThread() {
    std::vector<float> buffer(m_config.bufferSize * 2); // stereo
    
    while (m_running.load()) {
        // Process samples
        processSamples(buffer.data(), m_config.bufferSize);
        
        // Write to PCM
        snd_pcm_sframes_t written = snd_pcm_writei(m_pcm, buffer.data(), m_config.bufferSize);
        if (written < 0) {
            snd_pcm_recover(m_pcm, written, 0);
        }
    }
}

void AudioEngine::processSamples(float* buffer, size_t frames) {
    float volumeFactor = m_muted.load() ? 0.0f : (m_volume.load() / 100.0f);
    
    for (size_t i = 0; i < frames * 2; ++i) {
        buffer[i] *= volumeFactor;
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
