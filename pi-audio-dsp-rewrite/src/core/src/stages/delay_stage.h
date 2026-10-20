#pragma once
#include "dsp_core/stage.h"
#include "dsp_core/portability.h"

/* ------------------------------------------------------------------ */
/*  Delay stage — circular buffer delay line for driver alignment     */
/*  Fixed-size buffer, no heap allocation after construction.         */
/* ------------------------------------------------------------------ */

class DelayStage : public Stage {
public:
    static constexpr uint32_t kMaxDelay = 32768;

    DelayStage();

    void setDelay(uint32_t samples);
    void reset();
    uint32_t getDelaySamples() const;

    void process(float* const channels[], uint32_t numChannels,
                 uint32_t frames, const BlockInfo& info) override;

    uint32_t latencySamples() const override { return m_delaySamples; }

private:
    float m_buffer[kMaxDelay];
    float m_scratch[DSP_MAX_BLOCK_SIZE];
    uint32_t m_writePos = 0;
    uint32_t m_delaySamples = 0;
    bool m_initialized = false;
};
