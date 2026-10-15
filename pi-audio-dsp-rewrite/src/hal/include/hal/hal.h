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

protected:
    Engine* m_engine = nullptr;

    /* HAL-specific configuration. */
    virtual int rtPriority() const { return 75; }       /* C4: default 75, not max */
    virtual bool isTimerPaced() const { return false; } /* A5: auto-detected for null device */
};
