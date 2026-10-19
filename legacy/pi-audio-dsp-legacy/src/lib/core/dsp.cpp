#include "core/engine.h"

void AudioEngine::processDspTweeter(float* buffer, size_t frames) {
    float vol = m_muted.load() ? 0.0f : (m_volume.load() / 100.0f);
    for (size_t i = 0; i < frames; ++i)
        buffer[i] *= vol;
    for (auto& stage : m_stagesTweeterActive)
        stage(buffer, frames);
}

void AudioEngine::processDspWoofer(float* buffer, size_t frames) {
    float vol = m_muted.load() ? 0.0f : (m_volume.load() / 100.0f);
    for (size_t i = 0; i < frames; ++i)
        buffer[i] *= vol;
    for (auto& stage : m_stagesWooferActive)
        stage(buffer, frames);
}
