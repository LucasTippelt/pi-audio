#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  AudioHal abstract base class (A1, A5)                             */
/*  The HAL owns the audio thread and calls Engine::onBlock()          */
/* ------------------------------------------------------------------ */

class Engine; /* forward */

class AudioHal {
public:
    virtual ~AudioHal() = default;

    /* Open and configure the audio backend. Called once before run(). */
    virtual bool open(uint32_t sampleRate, uint32_t bufferSize,
                      uint32_t numInputs, uint32_t numOutputs) = 0;

    /* Main loop: runs in RT thread (or faster-than-RT for SimHal).
     * Calls m_engine->onBlock() each period. */
    virtual void run() = 0;

    virtual void close() = 0;

    /* Set the Engine that receives onBlock() callbacks. */
    void setEngine(Engine* engine) { m_engine = engine; }

    /* B4: Xrun counters for telemetry plumbing. Default to 0 for HALs without xruns. */
    virtual uint32_t xrunUnderrunCount() const { return 0; }
    virtual uint32_t xrunOverrunCount()  const { return 0; }

    /* Actual negotiated channel counts after open(). May differ from requested
     * if the hardware doesn't support the exact count asked for. */
    virtual uint32_t actualNumInputs()  const { return 0; }
    virtual uint32_t actualNumOutputs() const { return 0; }

protected:
    Engine* m_engine = nullptr;

    /* HAL-specific configuration. */
    virtual int rtPriority() const { return 75; }       /* C4: default 75, not max */
    virtual bool isTimerPaced() const { return false; } /* A5: auto-detected for null device */
};
