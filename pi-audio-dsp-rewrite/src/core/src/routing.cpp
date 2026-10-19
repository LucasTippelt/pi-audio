#define DSP_IN_PROCESS_PATH
#include "dsp_core/channels.h"
#include "dsp_core/portability.h"

#include <cstring>

/* Channel mixing logic extracted from playback thread. */
/* Routes input channels to output channels with per-channel gain. */

void routeChannels(const float* const inputs[], uint32_t numInputs,
                   float* const outputs[], uint32_t numOutputs,
                   const ChannelMap maps[], uint32_t mapCount,
                   uint32_t frames) {
    /* Zero outputs. */
    for (uint32_t ch = 0; ch < numOutputs; ++ch) {
        std::memset(outputs[ch], 0, frames * sizeof(float));
    }

    /* Mix mapped channels. Each map entry specifies a source input and gain.
     * The destination output channel is derived from the map index (0-based). */
    for (uint32_t m = 0; m < mapCount; ++m) {
        const ChannelMap& map = maps[m];
        if (map.sourceIndex >= numInputs || map.gain == 0.f)
            continue;

        const float* src = inputs[map.sourceIndex];
        uint8_t dstCh = static_cast<uint8_t>(m); /* map index -> output channel */
        if (dstCh >= numOutputs)
            continue;

        float* dst = outputs[dstCh];
        for (uint32_t f = 0; f < frames; ++f) {
            dst[f] += src[f] * map.gain;
        }
    }
}

/* Route inputs to outputs using the RoutingConfig input roles.
 * PGM_L is duplicated to all output channels.
 * PGM_R and Mic inputs are NOT routed to local outputs. */
void routeByRole(const float* const inputs[], uint32_t numInputs,
                 float* const outputs[], uint32_t numOutputs,
                 const RoutingConfig& routing,
                 uint32_t frames) {
    /* Zero outputs. */
    for (uint32_t ch = 0; ch < numOutputs; ++ch) {
        std::memset(outputs[ch], 0, frames * sizeof(float));
    }

    /* Find PGM_L input and duplicate to all output channels. */
    uint32_t pgmIdx = routing.pgmLIndex;
    if (pgmIdx >= numInputs) return; /* no valid source */

    const float* pgm = inputs[pgmIdx];
    for (uint32_t ch = 0; ch < numOutputs; ++ch) {
        std::memcpy(outputs[ch], pgm, frames * sizeof(float));
    }
}
