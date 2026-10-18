#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  Channel routing abstraction (B3 prep)                             */
/*                                                                    */
/*  Input roles for the master speaker unit:                          */
/*    - PGM_L  (input 0): main program, routed to BOTH local outputs  */
/*      through per-output DSP chains + master gain/mute              */
/*    - PGM_R  (input 1): right channel, NOT routed locally;          */
/*      forwarded raw to remote speaker for its own processing        */
/*    - Mic    (inputs 2-5): measurement mics, internal only;         */
/*      metered in telemetry, never sent to physical outputs           */
/* ------------------------------------------------------------------ */

enum ChannelSource : uint8_t {
    CH_DEVICE = 0, /* sample-synchronous device channel */
    CH_ASYNC   = 1, /* async source with resampler (shm PGM, network) */
};

enum InputRole : uint8_t {
    INPUT_PGM_L     = 0, /* Program Left — routed to local outputs through DSP */
    INPUT_PGM_R     = 1, /* Program Right — NOT routed locally (remote speaker) */
    INPUT_MIC       = 2, /* Measurement mic — internal only, metered in telemetry */
};

struct ChannelMap {
    uint8_t sourceIndex;
    ChannelSource sourceType;
    float gain;
};

/* Routing configuration: maps each input to its role.
 * Set by control plane from config.json or defaults. */
struct RoutingConfig {
    InputRole inputRoles[8];  /* role per input channel index */
    uint8_t numInputs;        /* total active inputs */
    uint8_t pgmLIndex;        /* which physical input carries PGM_L (default 0) */
};

/* Default routing: UR12-style with 6 inputs. */
inline RoutingConfig defaultRouting() {
    RoutingConfig rc{};
    rc.inputRoles[0] = INPUT_PGM_L;
    rc.inputRoles[1] = INPUT_PGM_R;
    for (int i = 2; i < 8; ++i) rc.inputRoles[i] = INPUT_MIC;
    rc.numInputs = 6;
    rc.pgmLIndex = 0;
    return rc;
}

/* Max channels supported. */
constexpr uint8_t kMaxChannels = 10;

/* Routing functions (implemented in routing.cpp). */

/* Generic channel mixer: each ChannelMap entry routes one input to one output. */
void routeChannels(const float* const inputs[], uint32_t numInputs,
                   float* const outputs[], uint32_t numOutputs,
                   const ChannelMap maps[], uint32_t mapCount,
                   uint32_t frames);

/* Role-based routing: PGM_L duplicated to all outputs, others silent. */
void routeByRole(const float* const inputs[], uint32_t numInputs,
                 float* const outputs[], uint32_t numOutputs,
                 const RoutingConfig& routing,
                 uint32_t frames);
