#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  DSP Core entry point (A1)                                         */
/*  Portable, zero OS/file I/O, no heap after init                    */
/* ------------------------------------------------------------------ */

struct BlockInfo;
struct TelemetryBlock;
struct StageSet;

class DspCore {
public:
    /* Init: allocate internal buffers, precompute coefficients. After this, no heap. */
    bool init(uint32_t sampleRate, uint32_t maxBlockSize,
              uint32_t numInputs, uint32_t numOutputs);

    /* Main processing entry point (A1).
     * in/out are planar float arrays: channels[ch][frame] */
    void process(float* const in[],  uint32_t numInCh,
                 float* const out[], uint32_t numOutCh,
                 uint32_t frames, const BlockInfo& info);

    /* Telemetry snapshot (A3). */
    void getTelemetry(TelemetryBlock& out) const;

    /* Sum of stage latencies (F4). */
    uint32_t totalLatencySamples() const;

    /* Stage chain access for Engine. */
    StageSet& stageChain(uint8_t index);

private:
    uint32_t m_sampleRate     = 0;
    uint32_t m_maxBlockSize   = 0;
    uint32_t m_numInputs      = 0;
    uint32_t m_numOutputs     = 0;

    StageSet m_chains[2]; /* tweeter chain, woofer chain */

    mutable TelemetryBlock m_telemetryCache{};
};
