#define DSP_IN_PROCESS_PATH
#include "stages/biquad_stage.h"
#include "dsp_core/block_info.h"
#include "dsp_core/portability.h"

void BiquadStage::setCoeffs(float b0, float b1, float b2, float a1, float a2) {
    m_b0 = b0;
    m_b1 = b1;
    m_b2 = b2;
    m_a1 = a1;
    m_a2 = a2;
    reset();
}

void BiquadStage::reset() {
    for (auto& st : m_state) {
        st.d1 = 0.f;
        st.d2 = 0.f;
    }
}

void BiquadStage::process(float* const channels[], uint32_t numChannels,
                          uint32_t frames, const BlockInfo&) {
    for (uint32_t ch = 0; ch < numChannels; ++ch) {
        float* buf = channels[ch];
        auto& st = m_state[ch];
        for (uint32_t f = 0; f < frames; ++f) {
            float x = buf[f];
            float y = m_b0 * x + st.d1;
            st.d1 = m_b1 * x - m_a1 * y + st.d2;
            st.d2 = m_b2 * x - m_a2 * y;
            buf[f] = y;
        }
    }
}
