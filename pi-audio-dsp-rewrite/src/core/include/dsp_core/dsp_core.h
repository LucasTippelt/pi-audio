#pragma once
#include <cstdint>
#include "dsp_core/stage.h"
#include "dsp_core/channels.h"
#include "msg/telemetry.h"

/* ------------------------------------------------------------------ */
/*  DSP Core entry point (A1)                                         */
/*  Portable, zero OS/file I/O, no heap after init                    */
/* ------------------------------------------------------------------ */

struct BlockInfo;
class GainStage; /* forward — master volume/mute applied once after chains */

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

    /* Register master gain stage (applied once to all outputs after chains). */
    void setMasterGain(GainStage* g) { m_masterGain = g; }
    /* C1: Test mode — set by Engine, read during processing.

     * 0=none, 1=woofer, 2=tweeter, 3=pgm-l
     * 4-9=mic tests (whitenoise injected into capture ch at engine level) */
    void setTestMode(uint8_t mode) { m_testMode = mode; }
    uint8_t getTestMode() const { return m_testMode; }

    /* Set routing configuration (input roles). Defaults to PGM_L on input 0. */
    void setRouting(const RoutingConfig& rc);
    const RoutingConfig& routing() const { return m_routing; }

    /* Get sample rate for stage configuration. */
    uint32_t getSampleRate() const { return m_sampleRate; }

private:
    uint32_t m_sampleRate     = 0;
    uint32_t m_maxBlockSize   = 0;
    uint32_t m_numInputs      = 0;
    uint32_t m_numOutputs     = 0;

    StageSet m_chains[2]; /* woofer chain, tweeter chain */

    mutable TelemetryBlock m_telemetryCache{};

    /* B6: Clock-state tracking for transition detection. */
    uint8_t m_lastClockState = 1; /* Default: Locked */
    uint32_t m_clockMuteEvents = 0; /* Count of mute events due to Unlocked state */

    /* C1: Test mode — 0=none, 1=woofer, 2=tweeter, 3=pgm-l. */
    uint8_t m_testMode = 0;

    /* LCG seed for deterministic white noise generation during tests. */
    uint32_t m_testNoiseSeed = 12345u;

    /* Persistent output level meter — exponential decay so StateWriter
     * always reads a representative level, not a single-block snapshot. */
    float m_meterLevel[2]{0.f, 0.f};

    /* Master gain/mute stage — applied once to all outputs after chains. */
    GainStage* m_masterGain = nullptr;

    /* Routing configuration: maps input roles (PGM_L, PGM_R, Mic). */
    RoutingConfig m_routing{};
};
