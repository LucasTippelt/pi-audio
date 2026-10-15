#pragma once
#include "hal/hal.h"

/* ------------------------------------------------------------------ */
/*  SimHal — file/generator-driven, faster-than-RT (A5)               */
/*  No ALSA dependency. Compiles on any platform including Windows CI. */
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
};
