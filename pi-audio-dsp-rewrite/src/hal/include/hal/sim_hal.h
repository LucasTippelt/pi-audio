#pragma once
#include "hal/hal.h"
#include "dsp_core/drift_controller.h"
#include "dsp_core/fractional_resampler.h"

/* ------------------------------------------------------------------ */
/*  SimHal — file/generator-driven, faster-than-RT (A5)               */
/*  No ALSA dependency. Compiles on any platform including Windows CI. */
/*  Drift compensation pipeline for B3 testing.                        */
/* ------------------------------------------------------------------ */

enum SimInputSource {
    SIM_INPUT_SINE,
    SIM_INPUT_NOISE,
    SIM_INPUT_FILE,
};

class SimHal : public AudioHal {
public:
    bool open(uint32_t sampleRate, uint32_t bufferSize,
              uint32_t numInputs, uint32_t numOutputs) override;
    void run() override;
    void close() override;

    /* Configuration. */
    void setInputSource(SimInputSource src);
    void setClockDriftPpm(int ppm);
    void setInputFile(const char* path);
    void setRunDurationSeconds(double secs);

    /* B4: Xrun counters for telemetry plumbing. */
    uint32_t xrunUnderrunCount() const override { return m_xrunUnderrun; }
    uint32_t xrunOverrunCount()  const override { return m_xrunOverrun; }

    /* Test-only injection methods — simulate xrun conditions for B4 testing. */
    void injectUnderrun();
    void injectOverrun();

    /* Test-only getters for fade state verification. */
    float getFadeLevel() const { return m_fadeLevel; }
    int getFadeStepsRemaining() const { return m_fadeStepsRemaining; }

protected:
    int rtPriority() const override { return 0; }
    bool isTimerPaced() const override { return true; }

private:
    uint32_t m_sampleRate = 48000;
    uint32_t m_bufferSize = 128;
    uint32_t m_numInputs  = 2;
    uint32_t m_numOutputs = 2;

    SimInputSource m_inputSource = SIM_INPUT_SINE;
    int m_clockDriftPpm          = 0;
    const char* m_inputFile      = nullptr;
    double m_runDurationSecs     = 1.0;

    uint64_t m_sampleIndex       = 0;
    bool m_running               = false;

    /* Clock state simulation. */
    uint8_t m_clockState         = 1; /* Locked */

    /* Drift compensation pipeline (B3). */
    static constexpr uint32_t kDriftRingCapacity = 4096;
    float m_driftRing[kDriftRingCapacity];
    uint32_t m_ringWritePos = 0;
    uint32_t m_ringReadPos  = 0;

    DriftController   m_driftCtrl;
    FractionalResampler m_resampler;

    /* B4: Xrun recovery state for SimHal testing. */
    uint32_t m_xrunUnderrun = 0;
    uint32_t m_xrunOverrun  = 0;
    float m_fadeLevel = 1.0f;
    int m_fadeStepsRemaining = 0;
    float m_fadeStartLevel = 1.0f;
    static constexpr int kFadeRestoreSteps = 8;
};
