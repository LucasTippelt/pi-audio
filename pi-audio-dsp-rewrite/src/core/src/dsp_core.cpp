#define DSP_IN_PROCESS_PATH
#include "dsp_core/dsp_core.h"
#include "dsp_core/block_info.h"
#include "dsp_core/stage.h"
#include "dsp_core/channels.h"
#include "dsp_core/portability.h"
#include "stages/gain_stage.h"
#include "msg/telemetry.h"

#include <cmath>
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

    /* B6: Reset clock-state tracking. */
    m_lastClockState = 1; /* Locked */
    m_clockMuteEvents = 0;

    /* Set default routing: PGM_L on input 0, PGM_R on input 1, rest are mics. */
    setRouting(defaultRouting());

    return true;
}

void DspCore::setRouting(const RoutingConfig& rc) {
    m_routing = rc;
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

    /* C1: Test mode generates signals instead of normal PGM routing.
     * All test modes flow through stage chains (crossover, limiter, master gain). */
    if (m_testMode != 0) {
        switch (m_testMode) {
            case 1: { /* Woofer — noise on channel 0 only */
                for (uint32_t f = 0; f < frames; ++f) {
                    m_testNoiseSeed = m_testNoiseSeed * 1664525u + 1013904223u;
                    out[0][f] = static_cast<float>((m_testNoiseSeed >> 16) & 0xFFFF) / 65536.0f * 0.6f - 0.3f;
                }
                break;
            }
            case 2: { /* Tweeter — noise on channel 1 only */
                for (uint32_t f = 0; f < frames; ++f) {
                    m_testNoiseSeed = m_testNoiseSeed * 1664525u + 1013904223u;
                    out[1][f] = static_cast<float>((m_testNoiseSeed >> 16) & 0xFFFF) / 65536.0f * 0.6f - 0.3f;
                }
                break;
            }
            case 3: { /* PGM Test — generate LCG white noise on BOTH outputs (Woofer + Tweeter).
                        * Signal flows through full stage chains: crossover, limiter, master gain.
                        * Self-contained: does not depend on hardware input being connected. */
                if (numOutCh >= 2) {
                    for (uint32_t f = 0; f < frames; ++f) {
                        m_testNoiseSeed = m_testNoiseSeed * 1664525u + 1013904223u;
                        float sample = static_cast<float>((m_testNoiseSeed >> 16) & 0xFFFF) / 65536.0f * 0.6f - 0.3f;
                        out[0][f] = sample;
                        out[1][f] = sample;
                    }
                }
                break;
            }
            /* Modes 4-9: Mic tests — whitenoise injected into capture channels at engine level,
             * flows through SHM rings → RC output. DSP core treats these as inactive (normal routing). */
            default: {
                /* Unknown test mode — treat as inactive, route PGM_L to both outputs. */
                if (m_routing.pgmLIndex < numInCh && numOutCh >= 2) {
                    const float* pgm = in[m_routing.pgmLIndex];
                    std::memcpy(out[0], pgm, frames * sizeof(float));
                    std::memcpy(out[1], pgm, frames * sizeof(float));
                } else if (numInCh > 0 && numOutCh >= 2) {
                    std::memcpy(out[0], in[0], frames * sizeof(float));
                    std::memcpy(out[1], in[0], frames * sizeof(float));
                }
                break;
            }
        }
    } else {
        /* Normal mode: route PGM_L to BOTH local output channels.
         * Each output gets its own DSP chain (Woofer/Tweeter) + master gain/mute.
         *
         * Input routing:
         *   - PGM_L  (input 0 by default): duplicated to out[0] and out[1]
         *   - PGM_R  (input 1): NOT routed locally — sent raw to remote speaker
         *   - Mics   (inputs 2-5): internal only, metered in telemetry
         */
        uint32_t pgmIdx = m_routing.pgmLIndex;
        if (pgmIdx >= numInCh) pgmIdx = 0; /* fallback to input 0 */

        const float* pgm = in[pgmIdx];
        for (uint32_t ch = 0; ch < numOutCh; ++ch) {
            std::memcpy(out[ch], pgm, frames * sizeof(float));
        }
    }

    /* Run stage chains — EQ, crossover, limiter etc. per output channel.
      * Each chain processes only its designated channel to avoid double-processing. */
    for (int c = 0; c < 2; ++c) {
        if (c >= (int)numOutCh) break;
        StageSet& chain = m_chains[c];
        for (uint8_t i = 0; i < chain.m_count; ++i) {
            if (chain.m_active[i]) {
                chain.m_active[i]->process(&out[c], 1, frames, info);
            }
        }
    }

    /* Master gain/mute — applied ONCE to all output channels after chains. */
    if (m_masterGain) {
        m_masterGain->process(out, numOutCh, frames, info);
    }

    /* B6: React to clock-state changes. */
    if (info.clockState == 3) {
        /* Unlocked — mute all outputs immediately. */
        for (uint32_t ch = 0; ch < numOutCh; ++ch) {
            std::memset(out[ch], 0, frames * sizeof(float));
        }
        if (m_lastClockState != 3) {
            m_clockMuteEvents++;
        }
    } else if (info.clockState == 2 && m_lastClockState == 1) {
        /* Transitioned to Holdover — telemetry will reflect state. */
    }

    m_lastClockState = info.clockState;

    /* Update telemetry. */
    m_telemetryCache.sampleIndex = info.sampleIndex + frames;
    m_telemetryCache.clockState  = info.clockState;
    m_telemetryCache.latencyFrames = totalLatencySamples() / m_sampleRate;

    /* Persistent output level meter — RMS per block with exponential decay.
     * Attack: instant (level rises to block RMS immediately).
     * Decay: 0.95/block (~100ms at 48kHz/512) so StateWriter always sees
     * a representative level, not a single-block snapshot. */
    for (uint32_t ch = 0; ch < numOutCh && ch < 2; ++ch) {
        /* Compute block RMS. */
        float sumSq = 0.f;
        for (uint32_t f = 0; f < frames; ++f) {
            float v = out[ch][f];
            sumSq += v * v;
        }
        float rms = std::sqrt(sumSq / static_cast<float>(frames));

        /* Persistent level: attack instantly, decay slowly. */
        if (rms > m_meterLevel[ch]) {
            m_meterLevel[ch] = rms;
        } else {
            m_meterLevel[ch] *= 0.95f; /* ~100ms decay at 48kHz/512 */
        }

        float level = m_meterLevel[ch];
        float db = (level > 1e-10f) ? (20.f * static_cast<float>(std::log10(level))) : -96.f;
        if (db < -96.f) db = -96.f; /* clamp to noise floor */
        m_telemetryCache.outputLevelDb[ch] = db;
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
        m_telemetryCache.inputLevelDb[ch] = (peak > 1e-10f) ? (40.f * static_cast<float>(std::log10(peak))) : -96.f;
    }
}

void DspCore::getTelemetry(TelemetryBlock& out) const {
    std::memcpy(&out, &m_telemetryCache, sizeof(TelemetryBlock));
}

uint32_t DspCore::totalLatencySamples() const {
    uint32_t total = 0;
    for (int c = 0; c < 2; ++c) {
        const StageSet& chain = m_chains[c];
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
