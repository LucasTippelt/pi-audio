#define DSP_IN_PROCESS_PATH
#include "stages/crossover_stage.h"
#include "dsp_core/block_info.h"
#include "dsp_core/portability.h"
#include <cstring>

bool CrossoverStage::configure(const float* biquadCoeffs, uint8_t numBiquads,
                               bool hasFirstOrder, float firstOrderCoef) {
    if (numBiquads > kMaxBiquads) return false;

    m_numBiquads = numBiquads;
    m_hasFirstOrder = hasFirstOrder;

    for (uint8_t i = 0; i < kMaxBiquads; ++i) {
        if (i < numBiquads && biquadCoeffs) {
            const float* src = &biquadCoeffs[i * 5];
            m_biquads[i].b0 = src[0];
            m_biquads[i].b1 = src[1];
            m_biquads[i].b2 = src[2];
            m_biquads[i].a1 = src[3];
            m_biquads[i].a2 = src[4];
        } else {
            m_biquads[i].b0 = 0.f;
            m_biquads[i].b1 = 0.f;
            m_biquads[i].b2 = 0.f;
            m_biquads[i].a1 = 0.f;
            m_biquads[i].a2 = 0.f;
        }
    }

    if (hasFirstOrder) {
        m_firstOrder.coef = firstOrderCoef;
    }

    reset();
    return true;
}

void CrossoverStage::reset() {
    for (uint8_t i = 0; i < kMaxBiquads; ++i) {
        std::memset(m_biquads[i].d1, 0, kMaxChannels * sizeof(float));
        std::memset(m_biquads[i].d2, 0, kMaxChannels * sizeof(float));
    }
    std::memset(m_firstOrder.y_prev, 0, kMaxChannels * sizeof(float));
    std::memset(m_firstOrder.x_prev, 0, kMaxChannels * sizeof(float));
}

void CrossoverStage::disable() {
    /* Reset to pass-through: no biquads, no first-order section. */
    m_numBiquads = 0;
    m_hasFirstOrder = false;
    reset();
}

void CrossoverStage::processChannel(float* buffer, uint32_t frames, uint32_t chIdx) {
    /* First-order section (if present).                              */
    /* Convention: positive coef = lowpass, negative coef = highpass. */
    if (m_hasFirstOrder && frames > 0) {
        float alpha = m_firstOrder.coef;
        bool isLowpass = alpha >= 0.f;

        if (isLowpass) {
            float y_prev = m_firstOrder.y_prev[chIdx];
            for (uint32_t f = 0; f < frames; ++f) {
                float x = buffer[f];
                float y = alpha * x + (1.f - alpha) * y_prev;
                buffer[f] = y;
                y_prev = y;
            }
            m_firstOrder.y_prev[chIdx] = y_prev;
        } else {
            float a = -alpha;
            float yp = m_firstOrder.y_prev[chIdx];
            float xp = m_firstOrder.x_prev[chIdx];
            for (uint32_t f = 0; f < frames; ++f) {
                float x = buffer[f];
                float y = yp + a * (x - xp);
                buffer[f] = y;
                yp = y;
                xp = x;
            }
            m_firstOrder.y_prev[chIdx] = yp;
            m_firstOrder.x_prev[chIdx] = xp;
        }
    }

    /* Cascade biquad sections — Direct Form II Transposed. */
    for (uint8_t i = 0; i < m_numBiquads; ++i) {
        auto& bq = m_biquads[i];
        float d1 = bq.d1[chIdx];
        float d2 = bq.d2[chIdx];
        for (uint32_t f = 0; f < frames; ++f) {
            float x = buffer[f];
            float y = bq.b0 * x + d1;
            d1 = bq.b1 * x - bq.a1 * y + d2;
            d2 = bq.b2 * x - bq.a2 * y;
            buffer[f] = y;
        }
        bq.d1[chIdx] = d1;
        bq.d2[chIdx] = d2;
    }
}

void CrossoverStage::process(float* const channels[], uint32_t numChannels,
                              uint32_t frames, const BlockInfo&) {
    for (uint32_t ch = 0; ch < numChannels; ++ch) {
        processChannel(channels[ch], frames, ch);
    }
}
