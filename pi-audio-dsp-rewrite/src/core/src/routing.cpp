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

    /* Mix mapped channels. */
    for (uint32_t m = 0; m < mapCount; ++m) {
        const ChannelMap& map = maps[m];
        if (map.sourceIndex >= numInputs || map.gain == 0.f)
            continue;

        const float* src = inputs[map.sourceIndex];
        /* Find target output channel from map. For now, sourceIndex maps to same output index. */
        uint8_t dstCh = map.sourceIndex;
        if (dstCh >= numOutputs)
            continue;

        float* dst = outputs[dstCh];
        for (uint32_t f = 0; f < frames; ++f) {
            dst[f] += src[f] * map.gain;
        }
    }
}
