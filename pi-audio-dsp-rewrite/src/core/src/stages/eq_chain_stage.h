#pragma once
#include "dsp_core/stage.h"
#include "dsp_core/channels.h"

/* ------------------------------------------------------------------ */
/*  Cascaded EQ chain — up to 12 peaking biquads, DFII-T             */
/*  Single stage slot per output chain (EQ before crossover).         */
/* ------------------------------------------------------------------ */

class EqChainStage : public Stage {
public:
    static constexpr uint8_t kMaxBands = 12;

    struct Band {
        float b0, b1, b2, a1, a2;
        bool enabled;
        /* DFII-T delay states per channel */
        float d1[kMaxChannels], d2[kMaxChannels];
    };

    /* Load coefficients and enabled flags. Delay states zeroed. */
    void configure(const float* coeffs, uint32_t numBands, const bool* enabled);

    /* Zero all delay states (click-free reconfigure). */
    void reset();

    /* Stage interface */
    void process(float* const channels[], uint32_t numChannels,
                 uint32_t frames, const BlockInfo& info) override;

    uint32_t latencySamples() const override { return m_bandCount * 2; }

private:
    Band m_bands[kMaxBands];
    uint8_t m_bandCount = 0;
};
