#pragma once
#include <alsa/asoundlib.h>
#include "hal/hal.h"

/* ------------------------------------------------------------------ */
/*  AlsaHal — ALSA streams, RT thread (A5)                            */
/* ------------------------------------------------------------------ */

class AlsaHal : public AudioHal {
public:
    bool open(uint32_t sampleRate, uint32_t bufferSize,
              uint32_t numInputs, uint32_t numOutputs) override;
    void run() override;
    void close() override;

    /* Set ALSA device names. */
    void setCaptureDevice(const char* dev) { m_captureDevice = dev; }
    void setPlaybackDevice(const char* dev) { m_playbackDevice = dev; }

    /* CPU affinity mask for RT thread (no-op in original, kept for engine binary). */
    void setCoreMask(uint32_t mask) { (void)mask; }

    /* Signal HAL to reopen streams on next cycle (no-op in original, kept for engine binary). */
    void requestReopen() {}

protected:
    int rtPriority() const override { return 75; }
    bool isTimerPaced() const override;

private:
    const char* m_captureDevice = "default";
    const char* m_playbackDevice = "default";

    snd_pcm_t* m_pcmCapture   = nullptr;
    snd_pcm_t* m_pcmPlayback  = nullptr;

    uint32_t m_sampleRate = 0;
    uint32_t m_bufferSize = 0;
    uint32_t m_numInputs  = 0;
    uint32_t m_numOutputs = 0;

    bool m_timerPaced = false;

    /* Xrun recovery state. */
    uint32_t m_xrunUnderrun = 0;
    uint32_t m_xrunOverrun  = 0;
    float m_fadeLevel = 1.0f;

    bool openStreams();
    void recoverUnderrun();
    void recoverOverrun();
};
