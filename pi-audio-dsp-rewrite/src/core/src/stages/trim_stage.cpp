#define DSP_IN_PROCESS_PATH
#include "stages/trim_stage.h"
#include "dsp_core/block_info.h"
#include "dsp_core/portability.h"

#include <cmath>

void TrimStage::setGain(float gain) {
    m_gain = gain;
}

void TrimStage::setTrimDb(float db) {
    m_gain = std::expf(db * 0.1151292546497023f);
}

void TrimStage::setPhaseInvert(bool invert) {
    if (invert) {
        m_gain = -std::fabs(m_gain);
    } else {
        m_gain = std::fabs(m_gain);
    }
}

float TrimStage::getGain() const {
    return m_gain;
}

void TrimStage::process(float* const channels[], uint32_t numChannels,
                        uint32_t frames, const BlockInfo&) {
    for (uint32_t ch = 0; ch < numChannels; ++ch) {
        float* buf = channels[ch];
        for (uint32_t f = 0; f < frames; ++f) {
            buf[f] *= m_gain;
        }
    }
}
