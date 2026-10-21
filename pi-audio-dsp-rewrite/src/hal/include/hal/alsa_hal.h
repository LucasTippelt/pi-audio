#pragma once
#include <alsa/asoundlib.h>
#include <atomic>
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

    /* Room correction ALSA device names. Empty string disables RC I/O. */
    void setRcInputDevice(const char* dev) { m_rcInputDevice = dev; }
    void setRcOutputDevice(const char* dev) { m_rcOutputDevice = dev; }

    /* Mic test mode — 0=off, 1=Mic_1 (ch0), 2=Mic_2 (ch1).
     * When active, generates whitenoise on the RC output instead of reading capture. */
    void setRcTestMode(uint8_t mode) { m_rcTestMode = mode; }

    /* CPU affinity mask for RT thread (no-op in original, kept for engine binary). */
    void setCoreMask(uint32_t mask) { (void)mask; }

    /* Signal HAL to reopen streams with new parameters on next cycle.
     * Pass 0 for a parameter to keep the current value unchanged. */
    void requestReopen(uint32_t sampleRate = 0, uint32_t bufferSize = 0);

    /* Capture starvation counter (blocks processed with silence input). */
    uint32_t silenceBlockCount() const { return m_silenceBlocks; }

    /* Callback invoked after a successful runtime reopen so the caller can
     * update StateWriter with the new audio config. Pass nullptr for no-op. */
    using ReopenCallback = void(*)(uint32_t sampleRate, uint32_t bufferSize);
    void setReopenCallback(ReopenCallback cb) { m_reopenCallback = cb; }

    /* B4: Xrun counters for telemetry plumbing. */
    uint32_t xrunUnderrunCount() const override { return m_xrunUnderrun; }
    uint32_t xrunOverrunCount()  const override { return m_xrunOverrun; }

protected:
    int rtPriority() const override { return 75; }
    bool isTimerPaced() const override;

private:
    const char* m_captureDevice = "default";
    const char* m_playbackDevice = "default";

    /* Room correction device names. Empty string = disabled. */
    const char* m_rcInputDevice = "";
    const char* m_rcOutputDevice = "";

    snd_pcm_t* m_pcmCapture   = nullptr;
    snd_pcm_t* m_pcmPlayback  = nullptr;

    /* Room correction PCM handles (loopback routing). */
    snd_pcm_t* m_pcmRcCapture   = nullptr;
    snd_pcm_t* m_pcmRcPlayback  = nullptr;

    /* Mic test mode — 0=off, 1=Mic_1 whitenoise on ch0, 2=Mic_2 whitenoise on ch1. */
    std::atomic<uint8_t> m_rcTestMode{0};
    uint32_t m_rcTestNoiseSeed = 54321u; /* LCG seed for mic test whitenoise */

    uint32_t m_sampleRate = 0;
    uint32_t m_bufferSize = 0;
    uint32_t m_numInputs  = 0;
    uint32_t m_numOutputs = 0;

    bool m_timerPaced = false;

    /* Xrun recovery state with gradual fade-in to prevent clicks. */
    uint32_t m_xrunUnderrun = 0;
    uint32_t m_xrunOverrun  = 0;
    float m_fadeLevel = 1.0f;
    int m_fadeStepsRemaining = 0;
    float m_fadeStartLevel = 1.0f;
    static constexpr int kFadeRestoreSteps = 16; /* ~32ms at 512 frames/48kHz */

    /* Capture starvation counter (for diagnostics). */
    uint32_t m_silenceBlocks = 0;

    /* Runtime reopen support — set by reload thread, consumed by audio loop. */
    std::atomic<bool> m_reopenRequested{false};
    uint32_t m_pendingSampleRate = 0;
    uint32_t m_pendingBufferSize = 0;
    ReopenCallback m_reopenCallback = nullptr;

    bool openStreams();
    void recoverUnderrun();
    void recoverOverrun();
};
