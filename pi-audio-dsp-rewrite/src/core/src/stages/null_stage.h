#pragma once
#include "dsp_core/stage.h"

/* ------------------------------------------------------------------ */
/*  No-op stage for budget measurement                                */
/* ------------------------------------------------------------------ */

class NullStage : public Stage {
public:
    void process(float* const, uint32_t, uint32_t, const BlockInfo&) override {}
    uint32_t latencySamples() const override { return 0; }
};
