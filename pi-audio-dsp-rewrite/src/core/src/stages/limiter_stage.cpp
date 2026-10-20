#define DSP_IN_PROCESS_PATH
#include "stages/limiter_stage.h"
#include "dsp_core/block_info.h"
#include "dsp_core/portability.h"

#include <cmath>

void LimiterStage::configure(float thresholdDb, float attackMs,
                              float releaseMs, float ratio, uint32_t sampleRate) {
    /* Convert threshold from dB to linear. */
    m_threshold = std::expf(thresholdDb * 0.1151292546497023f);

    /* Precompute attack/release coefficients. */
    m_attackCoeff   = 1.0f - std::expf(-1.0f / (attackMs * sampleRate / 1000.0f));
    m_releaseCoeff  = 1.0f - std::expf(-1.0f / (releaseMs * sampleRate / 1000.0f));

    /* Clamp coefficients to valid range [0, 1]. */
    if (m_attackCoeff < 0.f)   m_attackCoeff = 0.f;
    if (m_attackCoeff > 1.f)   m_attackCoeff = 1.f;
    if (m_releaseCoeff < 0.f)  m_releaseCoeff = 0.f;
    if (m_releaseCoeff > 1.f)  m_releaseCoeff = 1.f;

    /* Set ratio and precompute exponent. */
    m_ratio = ratio;
    m_exponent = 1.0f - 1.0f / ratio;

    reset();
}

void LimiterStage::reset() {
    for (uint32_t ch = 0; ch < kMaxChannels; ++ch) {
        m_envelope[ch] = 0.f;
    }
}

void LimiterStage::disable() {
    /* Restore pass-through defaults: threshold above full-scale, no compression. */
    m_threshold     = 2.0f;       /* linear — well above full-scale */
    m_attackCoeff   = 0.0f;       /* inactive */
    m_releaseCoeff  = 0.0f;       /* inactive */
    m_ratio         = 1.0f;       /* 1:1 = no compression */
    m_exponent      = 0.0f;       /* 1 - 1/1 = 0 */
    reset();                      /* zero envelope states */
}

void LimiterStage::process(float* const channels[], uint32_t numChannels,
                            uint32_t frames, const BlockInfo&) {
    for (uint32_t ch = 0; ch < numChannels; ++ch) {
        float* buf = channels[ch];
        float& env = m_envelope[ch];

        for (uint32_t f = 0; f < frames; ++f) {
            float input = buf[f];
            float absVal = input >= 0.f ? input : -input;

            /* Envelope tracking. */
            if (absVal > env) {
                env += m_attackCoeff * (absVal - env);
            } else {
                env -= m_releaseCoeff * (env - absVal);
            }

            /* Gain calculation. */
            float gain = 1.0f;
            if (env > m_threshold) {
                gain = std::powf(m_threshold / env, m_exponent);
            }

            buf[f] = input * gain;
        }
    }
}
