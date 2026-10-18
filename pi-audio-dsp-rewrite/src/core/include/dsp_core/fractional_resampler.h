#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  FractionalResampler — polyphase FIR fractional-delay filter (B3)   */
/*  Kaiser-windowed sinc, 32 taps × 64 phases.                        */
/*  Behaves as fractional-delay filter for near-unity clock ratios.    */
/*  No heap allocation after init. No libm at runtime.                 */
/* ------------------------------------------------------------------ */

#define FR_RESAMPLER_TAPS   32
#define FR_RESAMPLER_PHASES 64
#define FR_HISTORY_SIZE     (FR_RESAMPLER_TAPS + 16)

class FractionalResampler {
public:
    FractionalResampler();

    /* Process a block. ratio = sourceRate/destRate (near 1.0).
     * nIn input samples produce up to nOut output samples. */
    uint32_t process(const float* in, uint32_t nIn,
                     float* out, uint32_t nOut, float ratio);

    /* Reset history buffer to zero. */
    void reset();

private:
    float m_history[FR_HISTORY_SIZE];  /* Ring buffer of recent input samples */
    uint32_t m_histWrite;              /* Write position in history ring */
    float m_phaseAccum;                /* Fractional phase accumulator */

};
