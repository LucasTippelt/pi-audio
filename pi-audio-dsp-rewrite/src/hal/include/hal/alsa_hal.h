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
