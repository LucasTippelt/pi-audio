#pragma once
#include "dsp_core/stage.h"
#include "dsp_core/channels.h"

/* ------------------------------------------------------------------ */
/*  Limiter stage — per-output peak limiter with attack/release       */
/*  envelope follower and compression ratio.                          */
/*  No heap allocation after construction.                            */
/* ------------------------------------------------------------------ */

class LimiterStage : public Stage {
public:
    LimiterStage() = default;

    /* Configure from parameters. Sample rate needed for coefficient computation. */
    void configure(float thresholdDb, float attackMs, float releaseMs,
                   float ratio, uint32_t sampleRate);

    /* Zero envelope states (for reconfigure). */
    void reset();

    /* Reset to pass-through defaults (threshold above full-scale, ratio 1:1).
     * Called when limiter is disabled in config so stale parameters are cleared. */
    void disable();

    void process(float* const channels[], uint32_t numChannels,
                 uint32_t frames, const BlockInfo& info) override;

    uint32_t latencySamples() const override { return 0; }

private:
    float m_threshold     = 2.0f;       /* linear threshold — above full-scale (disabled at startup) */
    float m_attackCoeff   = 0.0f;       /* attack smoothing coefficient (inactive until configure())  */
    float m_releaseCoeff  = 0.0f;       /* release smoothing coefficient (inactive until configure()) */
    float m_ratio         = 1.0f;       /* compression ratio — 1:1 = no compression at startup        */
    float m_exponent      = 0.0f;       /* precomputed: 1 - 1/ratio (=0 when ratio=1, no compression)*/
    float m_envelope[kMaxChannels] = {}; /* per-channel envelope follower     */
};
