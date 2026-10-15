#include "hal/sim_hal.h"
#include "hal/engine.h"
#include "dsp_core/block_info.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

bool SimHal::open(uint32_t sampleRate, uint32_t bufferSize,
                  uint32_t numInputs, uint32_t numOutputs) {
    m_sampleRate = sampleRate;
    m_bufferSize = bufferSize;
    m_numInputs  = numInputs;
    m_numOutputs = numOutputs;
    return true;
}

void SimHal::run() {
    m_running = true;

    float* capPlanar[10]   = {};
    float* playPlanar[10]  = {};

    /* Allocate planar buffers. */
    for (uint32_t ch = 0; ch < m_numInputs; ++ch)
        capPlanar[ch] = new float[m_bufferSize];
    for (uint32_t ch = 0; ch < m_numOutputs; ++ch)
        playPlanar[ch] = new float[m_bufferSize];

    uint64_t totalFrames   = static_cast<uint64_t>(m_sampleRate * m_runDurationSecs);
    double sinePhase       = 0.0;
    double sineFreq         = 440.0; /* A4 */
    double driftFactor      = 1.0 + (static_cast<double>(m_clockDriftPpm) / 1e6);

    while (m_running && m_sampleIndex < totalFrames) {
        uint32_t frames = static_cast<uint32_t>(
            std::min(static_cast<double>(m_bufferSize),
                     static_cast<double>(totalFrames - m_sampleIndex)));

        /* Generate input. */
        for (uint32_t ch = 0; ch < m_numInputs; ++ch) {
            for (uint32_t f = 0; f < frames; ++f) {
                switch (m_inputSource) {
                    case SIM_INPUT_SINE: {
                        sinePhase += (sineFreq / m_sampleRate) * driftFactor;
                        if (sinePhase >= 1.0) sinePhase -= 1.0;
                        capPlanar[ch][f] = std::sin(2.0 * M_PI * sinePhase) * 0.5f;
                        break;
                    }
                    case SIM_INPUT_NOISE: {
                        capPlanar[ch][f] = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f);
                        break;
                    }
                    default: {
                        capPlanar[ch][f] = 0.f;
                        break;
                    }
                }
            }
        }

        /* Simulate clock state transitions. */
        if (m_sampleIndex > totalFrames / 4 && m_sampleIndex < totalFrames / 4 + m_sampleRate) {
            m_clockState = 2; /* Holdover */
        } else if (m_sampleIndex > totalFrames / 2 && m_sampleIndex < totalFrames / 2 + m_sampleRate) {
            m_clockState = 3; /* Unlocked */
        } else {
            m_clockState = 1; /* Locked */
        }

        /* Prepare BlockInfo. */
        BlockInfo info{};
        info.sampleIndex = m_sampleIndex;
        info.hostTimeNs  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
        info.frames      = frames;
        info.clockState  = m_clockState;

        /* Call Engine. */
        if (m_engine) {
            m_engine->onBlock(capPlanar, m_numInputs, playPlanar, m_numOutputs, info);
        }

        m_sampleIndex += frames;

        /* Small sleep to avoid consuming 100% CPU in faster-than-RT mode. */
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    /* Cleanup. */
    for (uint32_t ch = 0; ch < m_numInputs; ++ch)
        delete[] capPlanar[ch];
    for (uint32_t ch = 0; ch < m_numOutputs; ++ch)
        delete[] playPlanar[ch];
}

void SimHal::close() { m_running = false; }

void SimHal::setInputSource(SimInputSource src) { m_inputSource = src; }
void SimHal::setClockDriftPpm(int ppm) { m_clockDriftPpm = ppm; }
void SimHal::setInputFile(const char* path) { m_inputFile = path; }
void SimHal::setRunDurationSeconds(double secs) { m_runDurationSecs = secs; }
