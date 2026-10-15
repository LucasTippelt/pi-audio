#define DSP_IN_PROCESS_PATH
#include "dsp_core/dsp_core.h"
#include "dsp_core/block_info.h"
#include "dsp_core/stage.h"
#include "dsp_core/portability.h"
#include "msg/telemetry.h"

#include <cstring>

bool DspCore::init(uint32_t sampleRate, uint32_t maxBlockSize,
                   uint32_t numInputs, uint32_t numOutputs) {
    if (sampleRate == 0 || maxBlockSize == 0 || numInputs == 0 || numOutputs == 0)
        return false;
    if (maxBlockSize > DSP_MAX_BLOCK_SIZE)
        return false;

    m_sampleRate   = sampleRate;
    m_maxBlockSize = maxBlockSize;
    m_numInputs    = numInputs;
    m_numOutputs   = numOutputs;

    std::memset(&m_telemetryCache, 0, sizeof(m_telemetryCache));
    m_telemetryCache.version = 1;

    return true;
}

void DspCore::process(float* const in[], uint32_t numInCh,
                      float* const out[], uint32_t numOutCh,
                      uint32_t frames, const BlockInfo& info) {
    if (frames == 0 || frames > m_maxBlockSize)
        return;

    /* Zero output buffers. */
    for (uint32_t ch = 0; ch < numOutCh; ++ch) {
        std::memset(out[ch], 0, frames * sizeof(float));
    }

    /* Copy input to output as base signal path. */
    uint32_t copyCh = (numInCh < numOutCh) ? numInCh : numOutCh;
    for (uint32_t ch = 0; ch < copyCh; ++ch) {
        std::memcpy(out[ch], in[ch], frames * sizeof(float));
    }

    /* Run stage chains. */
    for (int c = 0; c < 2; ++c) {
        StageSet& chain = m_chains[c];
        for (uint8_t i = 0; i < chain.m_count; ++i) {
            if (chain.m_active[i]) {
                chain.m_active[i]->process(out, numOutCh, frames, info);
            }
        }
    }

    /* Update telemetry. */
    m_telemetryCache.sampleIndex = info.sampleIndex + frames;
    m_telemetryCache.latencyFrames = totalLatencySamples() / m_sampleRate;

    /* Peak meter for output channels. */
    for (uint32_t ch = 0; ch < numOutCh && ch < 2; ++ch) {
        float peak = 0.f;
        for (uint32_t f = 0; f < frames; ++f) {
            float v = out[ch][f];
            if (v < 0.f) v = -v;
            if (v > peak) peak = v;
        }
        m_telemetryCache.outputLevelDb[ch] = (peak > 1e-10f) ? (40.f * std::log10f(peak)) : -96.f;
    }

    /* Peak meter for input channels. */
    uint32_t meterCh = (numInCh < 8) ? numInCh : 8;
    for (uint32_t ch = 0; ch < meterCh; ++ch) {
        float peak = 0.f;
        for (uint32_t f = 0; f < frames; ++f) {
            float v = in[ch][f];
            if (v < 0.f) v = -v;
            if (v > peak) peak = v;
        }
        m_telemetryCache.inputLevelDb[ch] = (peak > 1e-10f) ? (40.f * std::log10f(peak)) : -96.f;
    }
}

void DspCore::getTelemetry(TelemetryBlock& out) const {
    std::memcpy(&out, &m_telemetryCache, sizeof(TelemetryBlock));
}

uint32_t DspCore::totalLatencySamples() const {
    uint32_t total = 0;
    for (int c = 0; c < 2; ++c) {
        StageSet& chain = m_chains[c];
        for (uint8_t i = 0; i < chain.m_count; ++i) {
            if (chain.m_active[i])
                total += chain.m_active[i]->latencySamples();
        }
    }
    return total;
}

StageSet& DspCore::stageChain(uint8_t index) {
    return m_chains[index];
}
