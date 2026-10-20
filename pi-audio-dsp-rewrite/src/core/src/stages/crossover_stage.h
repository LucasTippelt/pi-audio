#pragma once
#include "dsp_core/stage.h"
#include "dsp_core/channels.h"
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  Crossover stage — cascaded biquads + optional first-order section */
/*  One instance per output channel (woofer LP, tweeter HP)           */
/* ------------------------------------------------------------------ */

class CrossoverStage : public Stage {
public:
    /* Configure from precomputed coefficients. State is zeroed to     */
    /* avoid clicks. Call during init or at block-boundary reloads.   */
    bool configure(const float* biquadCoeffs, uint8_t numBiquads,
                   bool hasFirstOrder, float firstOrderCoef);

    /* Zero all delay states (click-free reconfigure). */
    void reset();

    /* Reset to pass-through mode (no biquads, no first-order section).
     * Called when crossover is disabled in config. */
    void disable();

    /* Stage interface */
    void process(float* const channels[], uint32_t numChannels,
                 uint32_t frames, const BlockInfo& info) override;

    uint32_t latencySamples() const override {
        return static_cast<uint32_t>(m_numBiquads * 2 + (m_hasFirstOrder ? 1 : 0));
    }

private:
    static constexpr uint8_t kMaxBiquads = 4;

    struct BiquadState {
        float b0 = 0.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
        float d1[kMaxChannels] = {};
        float d2[kMaxChannels] = {};
    };
    BiquadState m_biquads[kMaxBiquads];
    uint8_t m_numBiquads = 0;

    struct FirstOrderState {
        float coef = 0.f;
        float y_prev[kMaxChannels] = {};
        float x_prev[kMaxChannels] = {};
    };
    FirstOrderState m_firstOrder;
    bool m_hasFirstOrder = false;

    /* Process a single channel through the full filter chain in-place. */
    void processChannel(float* buffer, uint32_t frames, uint32_t chIdx);
};

/* P5: Compile-time size audit — must fit in L1 cache. */
static_assert(sizeof(CrossoverStage) <= 1024, "CrossoverStage too large for L1 cache");
