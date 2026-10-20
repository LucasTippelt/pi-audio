#define DSP_IN_PROCESS_PATH
#include "stages/delay_stage.h"
#include "dsp_core/block_info.h"

#include <cstring>

DelayStage::DelayStage()
    : m_buffer{}, m_scratch{}, m_writePos(0), m_delaySamples(0), m_initialized(false) {}

void DelayStage::setDelay(uint32_t samples) {
    if (samples >= kMaxDelay) return;
    m_delaySamples = samples;
}

void DelayStage::reset() {
    std::memset(m_buffer, 0, sizeof(m_buffer));
    m_writePos = 0;
    m_initialized = false;
}

uint32_t DelayStage::getDelaySamples() const {
    return m_delaySamples;
}

void DelayStage::process(float* const channels[], uint32_t numChannels,
                         uint32_t frames, const BlockInfo&) {
    if (m_delaySamples == 0) return;

    for (uint32_t ch = 0; ch < numChannels; ++ch) {
        float* buf = channels[ch];

        if (!m_initialized) {
            std::memset(m_buffer, 0, sizeof(m_buffer));
            m_writePos = m_delaySamples & (kMaxDelay - 1);
            m_initialized = true;
        }

        uint32_t mask = kMaxDelay - 1;

        if (m_delaySamples < frames) {
            std::memcpy(m_scratch, buf, frames * sizeof(float));
            for (uint32_t f = 0; f < frames; ++f) {
                uint32_t rp = (m_writePos + f - m_delaySamples) & mask;
                buf[f] = m_buffer[rp];
                m_buffer[(m_writePos + f) & mask] = m_scratch[f];
            }
        } else {
            for (uint32_t f = 0; f < frames; ++f) {
                uint32_t rp = (m_writePos + f - m_delaySamples) & mask;
                float input = buf[f];
                buf[f] = m_buffer[rp];
                m_buffer[(m_writePos + f) & mask] = input;
            }
        }

        m_writePos = (m_writePos + frames) & mask;
    }
}
