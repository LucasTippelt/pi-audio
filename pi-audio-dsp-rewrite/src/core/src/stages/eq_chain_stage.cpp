#define DSP_IN_PROCESS_PATH
#include "stages/eq_chain_stage.h"
#include "dsp_core/block_info.h"
#include "dsp_core/portability.h"

#include <cstring>

void EqChainStage::configure(const float* coeffs, uint32_t numBands, const bool* enabled) {
    m_bandCount = (numBands > kMaxBands) ? kMaxBands : static_cast<uint8_t>(numBands);
    reset();

    for (uint8_t i = 0; i < m_bandCount; ++i) {
        if (coeffs) {
            m_bands[i].b0 = coeffs[i * 5 + 0];
            m_bands[i].b1 = coeffs[i * 5 + 1];
            m_bands[i].b2 = coeffs[i * 5 + 2];
            m_bands[i].a1 = coeffs[i * 5 + 3];
            m_bands[i].a2 = coeffs[i * 5 + 4];
        } else {
            m_bands[i].b0 = 1.f;
            m_bands[i].b1 = 0.f;
            m_bands[i].b2 = 0.f;
            m_bands[i].a1 = 0.f;
            m_bands[i].a2 = 0.f;
        }
        if (enabled) {
            m_bands[i].enabled = enabled[i];
        } else {
            m_bands[i].enabled = true;
        }
    }
}

void EqChainStage::reset() {
    for (uint8_t i = 0; i < kMaxBands; ++i) {
        for (uint8_t ch = 0; ch < kMaxChannels; ++ch) {
            m_bands[i].d1[ch] = 0.f;
            m_bands[i].d2[ch] = 0.f;
        }
    }
}

void EqChainStage::process(float* const channels[], uint32_t numChannels,
                           uint32_t frames, const BlockInfo&) {
    if (m_bandCount == 0) return;

    for (uint32_t ch = 0; ch < numChannels; ++ch) {
        float* buf = channels[ch];

        /* Cascade through each enabled band sequentially. */
        for (uint8_t bi = 0; bi < m_bandCount; ++bi) {
            Band& band = m_bands[bi];
            if (!band.enabled) continue;

            /* Skip pass-through bands (b0=1, rest=0). */
            if (band.b1 == 0.f && band.b2 == 0.f && band.a1 == 0.f && band.a2 == 0.f) {
                float diff = band.b0 - 1.f;
                if (diff >= -1e-6f && diff <= 1e-6f) continue;
            }

            float b0 = band.b0;
            float b1 = band.b1;
            float b2 = band.b2;
            float a1 = band.a1;
            float a2 = band.a2;
            float d1 = band.d1[ch];
            float d2 = band.d2[ch];

            for (uint32_t f = 0; f < frames; ++f) {
                float x = buf[f];
                float y = b0 * x + d1;
                d1 = b1 * x - a1 * y + d2;
                d2 = b2 * x - a2 * y;
                buf[f] = y;
            }

            band.d1[ch] = d1;
            band.d2[ch] = d2;
        }
    }
}
