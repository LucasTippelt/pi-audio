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

    /* Initialize drift compensation pipeline (B3). */
    std::memset(m_driftRing, 0, sizeof(m_driftRing));
    m_ringWritePos = kDriftRingCapacity / 2;
    m_ringReadPos  = 0;
    m_driftCtrl.reset();
    m_resampler.reset();

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

    /* Virtual fill level for DriftController — starts at half capacity. */
    uint32_t virtualFill = kDriftRingCapacity / 2;

    while (m_running && m_sampleIndex < totalFrames) {
        uint32_t frames = static_cast<uint32_t>(
            std::min(static_cast<double>(m_bufferSize),
                     static_cast<double>(totalFrames - m_sampleIndex)));

        /* Generate raw input. */
        for (uint32_t ch = 0; ch < m_numInputs; ++ch) {
            float* raw = capPlanar[ch];
            for (uint32_t f = 0; f < frames; ++f) {
                switch (m_inputSource) {
                    case SIM_INPUT_SINE: {
                        sinePhase += (sineFreq / m_sampleRate) * driftFactor;
                        if (sinePhase >= 1.0) sinePhase -= 1.0;
                        raw[f] = std::sin(2.0 * M_PI * sinePhase) * 0.5f;
                        break;
                    }
                    case SIM_INPUT_NOISE: {
                        raw[f] = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f);
                        break;
                    }
                    default: {
                        raw[f] = 0.f;
                        break;
                    }
                }
            }

            /* Push raw input into drift ring (simulates async capture buffer write). */
            uint32_t pushCount = frames;
            if (pushCount > kDriftRingCapacity)
                pushCount = kDriftRingCapacity;
            for (uint32_t f = 0; f < pushCount; ++f) {
                m_driftRing[m_ringWritePos] = raw[f];
                m_ringWritePos = (m_ringWritePos + 1) % kDriftRingCapacity;
            }

            /* Read from drift ring at nominal rate. */
            uint32_t readCount = frames;
            if (readCount > kDriftRingCapacity)
                readCount = kDriftRingCapacity;
            for (uint32_t f = 0; f < readCount; ++f) {
                raw[f] = m_driftRing[m_ringReadPos];
                m_ringReadPos = (m_ringReadPos + 1) % kDriftRingCapacity;
            }
        }

        /* Update virtual fill level to simulate clock drift effect on ring buffer. */
        if (m_clockDriftPpm != 0) {
            int32_t fillDelta = m_clockDriftPpm / 100;
            if (fillDelta < 0) fillDelta = -fillDelta;
            if (fillDelta == 0) fillDelta = 1;
            virtualFill += static_cast<uint32_t>(fillDelta);
        }
        if (virtualFill >= kDriftRingCapacity - 1)
            virtualFill = kDriftRingCapacity - 1;
        if (virtualFill < 1)
            virtualFill = 1;

        /* Feed fill level to DriftController — estimates compensation ratio. */
        m_driftCtrl.update(virtualFill, kDriftRingCapacity);
        float ratio = m_driftCtrl.getRatio();

        /* Apply FractionalResampler per channel for drift compensation. */
        if (m_clockDriftPpm != 0) {
            for (uint32_t ch = 0; ch < m_numInputs; ++ch) {
                float compensated[512];
                uint32_t outCount = m_resampler.process(capPlanar[ch], frames,
                                                         compensated, sizeof(compensated) / sizeof(float),
                                                         ratio);
                if (outCount > m_bufferSize) outCount = m_bufferSize;
                std::memcpy(capPlanar[ch], compensated, outCount * sizeof(float));
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

        /* B4: Gradual fade restore — ramp over multiple blocks after xrun. */
        if (m_fadeStepsRemaining > 0) {
            float targetLevel = 1.0f;
            float stepSize = (targetLevel - m_fadeStartLevel) / static_cast<float>(kFadeRestoreSteps);
            m_fadeLevel = m_fadeStartLevel + stepSize * static_cast<float>(kFadeRestoreSteps - m_fadeStepsRemaining);
            if (m_fadeLevel > 1.0f) m_fadeLevel = 1.0f;

            /* Apply fade to playback output. */
            for (uint32_t ch = 0; ch < m_numOutputs; ++ch) {
                for (uint32_t f = 0; f < frames; ++f) {
                    playPlanar[ch][f] *= m_fadeLevel;
                }
            }

            m_fadeStepsRemaining--;
        } else if (m_fadeLevel < 1.0f) {
            /* Fade fully restored — reset. */
            m_fadeLevel = 1.0f;
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

/* B4: Xrun injection for testing. */
void SimHal::injectUnderrun() {
    m_xrunUnderrun++;
    m_fadeLevel = 0.0f;
    m_fadeStartLevel = 0.0f;
    m_fadeStepsRemaining = kFadeRestoreSteps;
}

void SimHal::injectOverrun() {
    m_xrunOverrun++;
    m_fadeLevel = 0.5f;
    m_fadeStartLevel = 0.5f;
    m_fadeStepsRemaining = kFadeRestoreSteps;
}
