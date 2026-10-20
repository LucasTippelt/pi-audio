#pragma once
#include "dsp_core/stage.h"

/* ------------------------------------------------------------------ */
/*  Trim stage — fixed linear gain with phase inversion support       */
/*  No ramp, no smoothing: applied at init or block-boundary reload.  */
/* ------------------------------------------------------------------ */

class TrimStage : public Stage {
public:
    TrimStage() = default;

    void setGain(float gain);
    void setTrimDb(float db);
    void setPhaseInvert(bool invert);
    float getGain() const;

    void process(float* const channels[], uint32_t numChannels,
                 uint32_t frames, const BlockInfo& info) override;

    uint32_t latencySamples() const override { return 0; }

private:
    float m_gain = 1.0f;
};
