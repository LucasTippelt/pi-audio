#pragma once
#include "dsp_core/stage.h"
#include "dsp_core/channels.h"

/* ------------------------------------------------------------------ */
/*  Single biquad filter — Direct Form II Transposed                  */
/*  Reusable building block for EQ bands and crossover sections       */
/* ------------------------------------------------------------------ */

class BiquadStage : public Stage {
public:
    /* Load pre-normalized coefficients. State is zeroed to avoid clicks. */
    void setCoeffs(float b0, float b1, float b2, float a1, float a2);

    /* Zero all delay states (click-free startup / reconfigure). */
    void reset();

    /* Stage interface */
    void process(float* const channels[], uint32_t numChannels,
                 uint32_t frames, const BlockInfo& info) override;

    uint32_t latencySamples() const override { return 2; }

private:
    float m_b0 = 0.f, m_b1 = 0.f, m_b2 = 0.f;
    float m_a1 = 0.f, m_a2 = 0.f;

    struct ChannelState {
        float d1 = 0.f, d2 = 0.f;
    };
    ChannelState m_state[kMaxChannels];
};
