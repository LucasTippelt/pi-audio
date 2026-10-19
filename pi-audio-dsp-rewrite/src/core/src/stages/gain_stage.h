#pragma once
#include "dsp_core/stage.h"

/* ------------------------------------------------------------------ */
/*  Gain stage with ramp/smoothing to avoid clicks (F1)               */
/* ------------------------------------------------------------------ */

class GainStage : public Stage {
public:
    GainStage();

    void setGain(float gain);
    void setMute(bool muted);

    void process(float* const channels[], uint32_t numChannels,
                 uint32_t frames, const BlockInfo& info) override;

    uint32_t latencySamples() const override { return 0; }

private:
    float m_targetGain   = 1.0f;
    float m_currentGain  = 1.0f;
    bool  m_muted        = false;

    /* Ramp: interpolate over N samples when gain changes. */
    static constexpr uint32_t kRampSamples = 64;
    float m_rampStart     = 1.0f;
    float m_rampEnd       = 1.0f;
    uint32_t m_rampRemaining = 0;

    void startRamp(float from, float to);
};
