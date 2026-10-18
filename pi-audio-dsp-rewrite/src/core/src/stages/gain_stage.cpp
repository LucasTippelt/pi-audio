#define DSP_IN_PROCESS_PATH
#include "stages/gain_stage.h"
#include "dsp_core/block_info.h"
#include "dsp_core/portability.h"

#include <cstdio>

GainStage::GainStage()
    : m_targetGain(1.0f), m_currentGain(1.0f), m_muted(false),
      m_rampStart(1.0f), m_rampEnd(1.0f), m_rampRemaining(0) {}

void GainStage::setGain(float gain) {
    if (gain < 0.f) gain = 0.f;
    if (gain > 1.f) gain = 1.f;
    m_targetGain = gain;
    startRamp(m_currentGain, gain);
    { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
      if (df) { std::fprintf(df, "[gain] setGain input=%.4f target=%.4f current=%.4f rampEnd=%.4f\n",
                gain, m_targetGain, m_currentGain, m_rampEnd); std::fclose(df); } }
}

void GainStage::setMute(bool muted) {
    m_muted = muted;
    float target = muted ? 0.f : m_targetGain;
    startRamp(m_currentGain, target);
}

void GainStage::startRamp(float from, float to) {
    if (from == to) return;
    m_rampStart     = from;
    m_rampEnd       = to;
    m_rampRemaining = kRampSamples;
}

void GainStage::process(float* const channels[], uint32_t numChannels,
                        uint32_t frames, const BlockInfo&) {
    /* Apply gain with ramp interpolation. */
    for (uint32_t ch = 0; ch < numChannels; ++ch) {
        float* buf = channels[ch];

        if (m_rampRemaining > 0) {
            uint32_t rampFrames = (m_rampRemaining < frames) ? m_rampRemaining : frames;
            float step = (m_rampEnd - m_rampStart) / kRampSamples;
            for (uint32_t f = 0; f < rampFrames; ++f) {
                float g = m_rampStart + step * static_cast<float>(kRampSamples - m_rampRemaining + f);
                buf[f] *= g;
            }
            m_currentGain = m_rampEnd;
            m_rampStart = m_rampEnd;
            m_rampRemaining -= rampFrames;

            /* Apply steady gain to remaining frames. */
            for (uint32_t f = rampFrames; f < frames; ++f) {
                buf[f] *= m_currentGain;
            }
        } else {
            for (uint32_t f = 0; f < frames; ++f) {
                buf[f] *= m_currentGain;
            }
        }
    }
}
