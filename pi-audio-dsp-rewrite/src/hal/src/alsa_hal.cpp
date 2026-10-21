#include "hal/alsa_hal.h"
#include "hal/engine.h"
#include "dsp_core/block_info.h"

extern "C" {
#include <alsa/asoundlib.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <vector>
#include <sched.h>
#include <sys/mman.h>
#include <thread>

bool AlsaHal::open(uint32_t sampleRate, uint32_t bufferSize,
                   uint32_t numInputs, uint32_t numOutputs) {
    m_sampleRate = sampleRate;
    m_bufferSize = bufferSize;
    m_numInputs  = numInputs;
    m_numOutputs = numOutputs;

    if (!openStreams())
        return false;

    /* Detect timer pacing for null device. */
    m_timerPaced = (snd_pcm_type(m_pcmPlayback) == SND_PCM_TYPE_NULL);

    /* Set RT priority. */
    struct sched_param sp{};
    sp.sched_priority = rtPriority();
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        /* Non-fatal: continue without RT scheduling. */
    }

    /* Lock current memory pages only — avoid MCL_FUTURE which can exhaust
     * the locked-memory limit and cause pthread_create to fail with EAGAIN
     * when new thread stacks need to be allocated. */
    if (mlockall(MCL_CURRENT) != 0) {
        /* Non-fatal: continue without memory locking. */
    }

    return true;
}

void AlsaHal::run() {
    /* S32_LE to float scale. */
    constexpr float s32Scale = 1.0f / (float)(1u << 31);

    uint64_t sampleIndex = 0;

    /* Outer loop: supports runtime reopen when parameters change. */
    while (!m_reopenRequested.load()) {
        /* Pre-allocate buffers for current buffer size. */
        int32_t* alsaCapture  = new int32_t[m_bufferSize * m_numInputs];
        int32_t* alsaPlayback = new int32_t[m_bufferSize * m_numOutputs];

        /* Planar buffers for core (float). */
        float* capPlanar[10];
        float* playPlanar[10];
        float* dspCapture   = new float[m_bufferSize * m_numInputs];
        float* dspPlayback  = new float[m_bufferSize * m_numOutputs];

        for (uint32_t ch = 0; ch < m_numInputs; ++ch)
            capPlanar[ch] = dspCapture + ch * m_bufferSize;
        for (uint32_t ch = 0; ch < m_numOutputs; ++ch)
            playPlanar[ch] = dspPlayback + ch * m_bufferSize;

        /* Activate capture with blocking reads (retry for USB warmup).
         * At high sample rates + large buffers, ALSA needs time to fill initial buffer. */
        {
            constexpr uint32_t kMaxPrimeFrames = 8192;
            uint32_t primeFrames = m_bufferSize * 2;
            if (primeFrames > kMaxPrimeFrames) primeFrames = kMaxPrimeFrames;
            std::vector<int32_t> capBuf(primeFrames * m_numInputs, 0);
            snd_pcm_nonblock(m_pcmCapture, 0);
            for (int i = 0; i < 20; ++i) {
                snd_pcm_sframes_t r = snd_pcm_readi(m_pcmCapture, capBuf.data(), primeFrames);
                if (r > 0) break; /* Got data — capture is active. */
                /* On error/xrun, try to recover. */
                int err = snd_pcm_state(m_pcmCapture);
                if (err == SND_PCM_STATE_XRUN || err == SND_PCM_STATE_DRAINING || err == SND_PCM_STATE_PREPARED) {
                    snd_pcm_prepare(m_pcmCapture);
                    snd_pcm_start(m_pcmCapture);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            snd_pcm_nonblock(m_pcmCapture, 1);
            { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
              if (df) { std::fprintf(df, "[hal] capture activated\n"); std::fflush(df); std::fclose(df); } }
        }

        /* Activate playback with blocking write (silence fill to prime buffer). */
        {
            constexpr uint32_t kMaxPrimeFrames = 8192;
            uint32_t primeFrames = m_bufferSize * 2;
            if (primeFrames > kMaxPrimeFrames) primeFrames = kMaxPrimeFrames;
            std::vector<int32_t> silence(primeFrames * m_numOutputs, 0);
            snd_pcm_nonblock(m_pcmPlayback, 0);
            snd_pcm_writei(m_pcmPlayback, silence.data(), primeFrames);
            snd_pcm_nonblock(m_pcmPlayback, 1);
        }

        /* Inner audio processing loop. */
        while (true) {
            /* Check for reopen request from reload thread. */
            if (m_reopenRequested.load()) break;

            /* Wait until BOTH capture has data AND playback has space.
             * On ASYNC USB devices with separate streams, running ahead of
             * playback causes buffer fills and dropped blocks (playSKIP).
             * Gate on both sides so we never produce a block we can't deliver. */
            {
                uint32_t waitMs = 0;
                while (waitMs < 50) {
                    snd_pcm_sframes_t ac = snd_pcm_avail_update(m_pcmCapture);
                    snd_pcm_sframes_t ap = snd_pcm_avail_update(m_pcmPlayback);
                    if (ac >= static_cast<snd_pcm_sframes_t>(m_bufferSize) &&
                        ap >= static_cast<snd_pcm_sframes_t>(m_bufferSize)) break;
                    if (ac < 0 || ap < 0) break; /* Error — handle below. */
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    waitMs++;
                }
            }

            /* Check capture availability with timeout.
             * On ASYNC USB devices (UR12), capture can stall indefinitely.
             * After waiting too long, fall through to process with silence so
             * speaker tests and DSP processing continue uninterrupted. */
             snd_pcm_sframes_t availCap = snd_pcm_avail_update(m_pcmCapture);
            if (availCap < 0) {
                /* Error state — try to recover with prepare+start sequence. */
                int err = snd_pcm_state(m_pcmCapture);
                if (err == SND_PCM_STATE_XRUN || err == SND_PCM_STATE_DRAINING || err == SND_PCM_STATE_PREPARED) {
                    m_xrunOverrun++;
                    /* Partial fade for capture xrun — output data is still valid but input gap. */
                    if (m_fadeStepsRemaining <= 0) {
                        m_fadeLevel = 0.5f;
                        m_fadeStartLevel = 0.5f;
                        m_fadeStepsRemaining = kFadeRestoreSteps / 2;
                    }
#ifdef PI_AUDIO_DIAG_LOG
                    static uint32_t lastXrunLog = 0;
                    if (m_xrunOverrun - lastXrunLog > 10) {
                        FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                        if (df) { std::fprintf(df, "[hal] capXRUN total=%u state=%d fade=%.3f\n",
                                  (unsigned)m_xrunOverrun, err, m_fadeLevel); std::fclose(df); }
                        lastXrunLog = m_xrunOverrun;
                    }
#endif
                    snd_pcm_prepare(m_pcmCapture);
                    snd_pcm_start(m_pcmCapture);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    /* Fall through to process silence — keeps telemetry flowing. */
                } else {
                    goto cleanup_buffers;
                }
            }

            uint32_t capFrames = 0;
            if (availCap >= static_cast<snd_pcm_sframes_t>(m_bufferSize)) {
                /* Capture data available — read it. */
                snd_pcm_sframes_t r = snd_pcm_readi(m_pcmCapture, alsaCapture, m_bufferSize);
                if (r < 0) {
                    int err = snd_pcm_state(m_pcmCapture);
                    if (err == SND_PCM_STATE_XRUN || err == SND_PCM_STATE_DRAINING || err == SND_PCM_STATE_PREPARED) {
                        m_xrunOverrun++;
                        /* Partial fade for capture xrun — output data is still valid but input gap. */
                        if (m_fadeStepsRemaining <= 0) {
                            m_fadeLevel = 0.5f;
                            m_fadeStartLevel = 0.5f;
                            m_fadeStepsRemaining = kFadeRestoreSteps / 2;
                        }
                        snd_pcm_prepare(m_pcmCapture);
                        snd_pcm_start(m_pcmCapture);
                        /* Fall through to process silence. */
                    } else {
                        goto cleanup_buffers;
                    }
                }
                capFrames = static_cast<uint32_t>(r);

                /* Deinterleave + convert S32_LE to float planar. */
                for (uint32_t ch = 0; ch < m_numInputs; ++ch) {
                    for (uint32_t f = 0; f < capFrames; ++f) {
                        capPlanar[ch][f] = alsaCapture[f * m_numInputs + ch] * s32Scale;
                    }
                }
            } else {
                /* No capture data — wait with timeout then fall through with silence. */
                uint32_t waitMs = 0;
                while (waitMs < 50) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    availCap = snd_pcm_avail_update(m_pcmCapture);
                    if (availCap >= static_cast<snd_pcm_sframes_t>(m_bufferSize)) break;
                    waitMs++;
                }
                /* Re-check after timeout. */
                if (availCap >= static_cast<snd_pcm_sframes_t>(m_bufferSize)) {
                    snd_pcm_sframes_t r = snd_pcm_readi(m_pcmCapture, alsaCapture, m_bufferSize);
                    if (r > 0) {
                        capFrames = static_cast<uint32_t>(r);
                        for (uint32_t ch = 0; ch < m_numInputs; ++ch) {
                            for (uint32_t f = 0; f < capFrames; ++f) {
                                capPlanar[ch][f] = alsaCapture[f * m_numInputs + ch] * s32Scale;
                            }
                        }
                    }
                }
                /* If still no data, process with silence (capFrames stays 0). */
            }

            if (capFrames == 0) {
                /* No capture data — fill with silence and use buffer size for processing. */
                m_silenceBlocks++;
#ifdef PI_AUDIO_DIAG_LOG
                /* Only log first occurrence per reopen cycle to avoid flooding. */
                if (m_silenceBlocks <= 3) {
                    FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                    if (df) { std::fprintf(df, "[hal] silenceBlock total=%u\n", (unsigned)m_silenceBlocks); std::fclose(df); }
                }
#endif
                for (uint32_t ch = 0; ch < m_numInputs; ++ch) {
                    std::memset(capPlanar[ch], 0, m_bufferSize * sizeof(float));
                }
                capFrames = m_bufferSize;
            }

            /* Prepare BlockInfo. */
            BlockInfo info{};
            info.sampleIndex = sampleIndex;
            info.hostTimeNs  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch()).count();
            info.frames      = static_cast<uint32_t>(capFrames);
            info.clockState  = 1; /* Locked */

            /* Call Engine. */
            if (m_engine) {
                m_engine->onBlock(capPlanar, m_numInputs, playPlanar, m_numOutputs, info);
            }

            /* --- Room Correction I/O (NONBLOCK, drop on failure). --- */
            if (m_pcmRcPlayback) {
                int32_t* rcOutBuf = new int32_t[m_bufferSize * 2];

                uint8_t testMode = m_rcTestMode.load();
                bool hasData = false;

                if (testMode != 0) {
                    /* Mic test mode: generate whitenoise on the target channel. */
                    uint32_t noiseCh = (testMode == 1) ? 0 : 1; /* Mic_1=ch0, Mic_2=ch1 */
                    for (uint32_t f = 0; f < m_bufferSize; ++f) {
                        m_rcTestNoiseSeed = m_rcTestNoiseSeed * 1664525u + 1013904223u;
                        float sample = static_cast<float>((m_rcTestNoiseSeed >> 16) & 0xFFFF) / 65536.0f * 0.6f - 0.3f;
                        int32_t val = static_cast<int32_t>(sample * 8388608.0f); /* scale to ~24-bit */
                        rcOutBuf[f * 2 + noiseCh] = val;
                        rcOutBuf[f * 2 + (1 - noiseCh)] = 0;
                    }
                    hasData = true;
                } else if (m_pcmRcCapture) {
                    /* Normal mode: read from RC capture device and pass through. */
                    snd_pcm_sframes_t rcAvail = snd_pcm_avail_update(m_pcmRcCapture);
                    if (rcAvail >= static_cast<snd_pcm_sframes_t>(m_bufferSize)) {
                        snd_pcm_sframes_t r = snd_pcm_readi(m_pcmRcCapture, rcOutBuf, m_bufferSize);
                        if (r > 0) {
                            hasData = true;
                        } else {
                            int err = snd_pcm_state(m_pcmRcCapture);
                            if (err == SND_PCM_STATE_XRUN || err == SND_PCM_STATE_DRAINING || err == SND_PCM_STATE_PREPARED) {
                                snd_pcm_prepare(m_pcmRcCapture);
                                snd_pcm_start(m_pcmRcCapture);
                            }
                        }
                    }
                }

                /* Write to RC playback device (loopback/SoundCard) — NONBLOCK. */
                if (hasData) {
                    snd_pcm_sframes_t rcPlayAvail = snd_pcm_avail_update(m_pcmRcPlayback);
                    if (rcPlayAvail >= static_cast<snd_pcm_sframes_t>(m_bufferSize)) {
                        snd_pcm_sframes_t pw = snd_pcm_writei(m_pcmRcPlayback, rcOutBuf, m_bufferSize);
                        if (pw < 0) {
                            int err = snd_pcm_state(m_pcmRcPlayback);
                            if (err == SND_PCM_STATE_XRUN || err == SND_PCM_STATE_DRAINING || err == SND_PCM_STATE_PREPARED) {
                                snd_pcm_prepare(m_pcmRcPlayback);
                                snd_pcm_start(m_pcmRcPlayback);
                            }
                        }
                    }
                }

                delete[] rcOutBuf;
            }

            /* Interleave + convert float to S32_LE with fade. */
            {
                uint32_t clipCount[2] = {0, 0}; /* per-channel clip counts */
                uint32_t nanCount = 0;
                for (uint32_t f = 0; f < static_cast<uint32_t>(capFrames); ++f) {
                    for (uint32_t ch = 0; ch < m_numOutputs; ++ch) {
                        float sample = playPlanar[ch][f] * m_fadeLevel;
                        if (std::isnan(sample) || std::isinf(sample)) {
                            nanCount++;
                            sample = 0.f;
                        } else if (sample > 1.0f) {
                            clipCount[ch < 2 ? ch : 1]++;
                            sample = 1.0f;
                        } else if (sample < -1.0f) {
                            clipCount[ch < 2 ? ch : 1]++;
                            sample = -1.0f;
                        }
                        alsaPlayback[f * m_numOutputs + ch] = static_cast<int32_t>(sample * (float)(1u << 31));
                    }
                }
#ifdef PI_AUDIO_DIAG_LOG
                uint32_t totalClips = clipCount[0] + clipCount[1];
                if (totalClips > 0 || nanCount > 0) {
                    FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                    if (df) { std::fprintf(df, "[hal] OUTPUT: clips=[%u %u] nans=%u frames=%u\n",
                              (unsigned)clipCount[0], (unsigned)clipCount[1], (unsigned)nanCount, (unsigned)capFrames); std::fclose(df); }
                }
#endif
            }

            /* Check playback availability before writing. */
            snd_pcm_sframes_t availPlay = snd_pcm_avail_update(m_pcmPlayback);
            if (availPlay < 0) {
                /* Error state — try to recover with prepare+start. */
                int err = snd_pcm_state(m_pcmPlayback);
                if (err == SND_PCM_STATE_XRUN || err == SND_PCM_STATE_DRAINING || err == SND_PCM_STATE_PREPARED) {
                    m_xrunUnderrun++;
                    /* Fade out to prevent click on silence gap. */
                    m_fadeLevel = 0.0f;
                    m_fadeStartLevel = 0.0f;
                    m_fadeStepsRemaining = kFadeRestoreSteps;
#ifdef PI_AUDIO_DIAG_LOG
                    static uint32_t lastPlayXrunLog = 0;
                    if (m_xrunUnderrun - lastPlayXrunLog > 10) {
                        FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                        if (df) { std::fprintf(df, "[hal] playXRUN total=%u state=%d fadeOut\n",
                                  (unsigned)m_xrunUnderrun, err); std::fclose(df); }
                        lastPlayXrunLog = m_xrunUnderrun;
                    }
#endif
                    snd_pcm_prepare(m_pcmPlayback);
                    snd_pcm_start(m_pcmPlayback);
                    sampleIndex += static_cast<uint64_t>(capFrames);
                    continue;
                }
                goto cleanup_buffers;
            }
            if (availPlay < static_cast<snd_pcm_sframes_t>(m_bufferSize)) {
                /* Not enough space — skip this block's output to avoid underrun. */
#ifdef PI_AUDIO_DIAG_LOG
                static uint32_t lastSkipLog = 0;
                static uint32_t skipCount = 0;
                skipCount++;
                if (skipCount - lastSkipLog > 100) {
                    FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                    if (df) { std::fprintf(df, "[hal] playSKIP total=%u avail=%ld\n", (unsigned)skipCount, (long)availPlay); std::fclose(df); }
                    lastSkipLog = skipCount;
                }
#endif
                sampleIndex += static_cast<uint64_t>(capFrames);
                continue;
            }

            /* Write playback (S32_LE). */
            snd_pcm_sframes_t playFrames = snd_pcm_writei(m_pcmPlayback, alsaPlayback, m_bufferSize);
            if (playFrames < 0) {
                int err = snd_pcm_state(m_pcmPlayback);
                if (err == SND_PCM_STATE_XRUN || err == SND_PCM_STATE_DRAINING || err == SND_PCM_STATE_PREPARED) {
                    m_xrunUnderrun++;
                    /* Fade out to prevent click on silence gap. */
                    m_fadeLevel = 0.0f;
                    m_fadeStartLevel = 0.0f;
                    m_fadeStepsRemaining = kFadeRestoreSteps;
                    snd_pcm_prepare(m_pcmPlayback);
                    snd_pcm_start(m_pcmPlayback);
                    continue;
                }
                goto cleanup_buffers;
            }

            /* Gradual fade-in after xrun recovery to prevent clicks. */
            if (m_fadeStepsRemaining > 0) {
                float targetLevel = 1.0f;
                float stepSize = (targetLevel - m_fadeStartLevel) / static_cast<float>(kFadeRestoreSteps);
                m_fadeLevel = m_fadeStartLevel + stepSize * static_cast<float>(kFadeRestoreSteps - m_fadeStepsRemaining);
                if (m_fadeLevel > 1.0f) m_fadeLevel = 1.0f;
                m_fadeStepsRemaining--;
            } else if (m_fadeLevel < 1.0f) {
                /* Fade fully restored — reset. */
                m_fadeLevel = 1.0f;
            }

            sampleIndex += static_cast<uint64_t>(capFrames);
        }

    cleanup_buffers:
        delete[] alsaCapture;
        delete[] alsaPlayback;
        delete[] dspCapture;
        delete[] dspPlayback;

        /* If reopen requested, apply new parameters and restart. */
        if (m_reopenRequested.load()) {
            uint32_t oldRate = m_sampleRate;
            uint32_t oldSize = m_bufferSize;

            uint32_t newRate = m_pendingSampleRate;
            uint32_t newSize = m_pendingBufferSize;
            if (newRate == 0) newRate = oldRate;
            if (newSize == 0) newSize = oldSize;

            { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
              if (df) { std::fprintf(df, "[hal] Reopening: %u/%u -> %u/%u\n", oldRate, oldSize, newRate, newSize); std::fflush(df); std::fclose(df); } }

            /* Close existing streams. */
            close();

            { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
              if (df) { std::fprintf(df, "[hal] close() done\n"); std::fflush(df); std::fclose(df); } }

            /* Update parameters and reopen. */
            m_sampleRate = newRate;
            m_bufferSize = newSize;
            m_fadeLevel = 1.0f; /* Reset fade so audio isn't quiet after reopen. */

            { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
              if (df) { std::fprintf(df, "[hal] calling openStreams()\n"); std::fflush(df); std::fclose(df); } }

            if (!openStreams()) {
                { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                  if (df) { std::fprintf(df, "[hal] Reopen failed at %u/%u, falling back to %u/%u\n", newRate, newSize, oldRate, oldSize); std::fflush(df); std::fclose(df); } }
                m_sampleRate = oldRate;
                m_bufferSize = oldSize;
                if (!openStreams()) {
                    { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                      if (df) { std::fprintf(df, "[hal] Fallback reopen also failed — stopping\n"); std::fflush(df); std::fclose(df); } }
                    break;
                }
            }

            { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
              if (df) { std::fprintf(df, "[hal] openStreams() succeeded\n"); std::fflush(df); std::fclose(df); } }

            /* Reset reopen flag and clear pending params. */
            m_reopenRequested.store(false);
            m_pendingSampleRate = 0;
            m_pendingBufferSize = 0;

            /* Notify caller that reopen completed with actual parameters. */
            if (m_reopenCallback) {
                m_reopenCallback(m_sampleRate, m_bufferSize);
            }

            { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
              if (df) { std::fprintf(df, "[hal] Reopen complete: %u/%u\n", m_sampleRate, m_bufferSize); std::fflush(df); std::fclose(df); } }
        }
    }
}

void AlsaHal::close() {
    if (m_pcmCapture)  snd_pcm_close(m_pcmCapture);
    if (m_pcmPlayback) snd_pcm_close(m_pcmPlayback);
    if (m_pcmRcCapture)  snd_pcm_close(m_pcmRcCapture);
    if (m_pcmRcPlayback) snd_pcm_close(m_pcmRcPlayback);
    m_pcmCapture  = nullptr;
    m_pcmPlayback = nullptr;
    m_pcmRcCapture  = nullptr;
    m_pcmRcPlayback = nullptr;
    munlockall();
}

void AlsaHal::requestReopen(uint32_t sampleRate, uint32_t bufferSize) {
    /* Store pending parameters (0 means keep current). */
    if (sampleRate > 0) m_pendingSampleRate = sampleRate;
    if (bufferSize > 0) m_pendingBufferSize = bufferSize;
    m_reopenRequested.store(true);
}

bool AlsaHal::isTimerPaced() const { return m_timerPaced; }

bool AlsaHal::openStreams() {
    uint32_t reqInputs  = m_numInputs;
    uint32_t reqOutputs = m_numOutputs;

    { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
      if (df) { std::fprintf(df, "[hal] openStreams(%u/%u) cap=%s pb=%s\n", m_sampleRate, m_bufferSize, m_captureDevice, m_playbackDevice); std::fflush(df); std::fclose(df); } }

    /* --- Open capture with device fallback (requested → "default"). --- */
    {
        unsigned int ch = 0;
        snd_pcm_format_t fmt = SND_PCM_FORMAT_UNKNOWN;

        if (!openStreamWithFallback(&m_pcmCapture, m_captureDevice, "default",
                                     SND_PCM_STREAM_CAPTURE)) {
            std::fprintf(stderr, "[hal] capture open failed (all devices tried)\n");
            return false;
        }
        /* Try hw formats first. If none work, fall back to plug: for auto-conversion. */
        if (!configureStreamWithFallback(m_pcmCapture, m_sampleRate, m_bufferSize,
                                          reqInputs, &fmt, &ch)) {
            std::fprintf(stderr, "[hal] capture hw_params failed — trying plug: converter\n");
            snd_pcm_close(m_pcmCapture);
            /* Reopen with plug: prefix for automatic format conversion. */
            char devName[256];
            (void)std::snprintf(devName, sizeof(devName), "plug:%s", m_captureDevice);
            if (snd_pcm_open(&m_pcmCapture, devName, SND_PCM_STREAM_CAPTURE, 0) < 0) {
                std::fprintf(stderr, "[hal] capture plug: open failed\n");
                return false;
            }
            /* On plug device, just set S32_LE directly — it converts. */
            if (!configureStreamWithFallback(m_pcmCapture, m_sampleRate, m_bufferSize,
                                              reqInputs, &fmt, &ch)) {
                std::fprintf(stderr, "[hal] capture hw_params failed (even with plug:)\n");
                close();
                return false;
            }
        }
        if (fmt != SND_PCM_FORMAT_S32_LE) {
            std::fprintf(stderr, "[hal] capture format conversion: %s → S32_LE\n",
                          snd_pcm_format_name(fmt));
        }
        m_captureFmt = fmt;
        m_numInputs  = ch;
    }

    /* --- Open playback with device fallback. --- */
    {
        unsigned int ch = 0;
        snd_pcm_format_t fmt = SND_PCM_FORMAT_UNKNOWN;

        if (!openStreamWithFallback(&m_pcmPlayback, m_playbackDevice, "default",
                                     SND_PCM_STREAM_PLAYBACK)) {
            std::fprintf(stderr, "[hal] playback open failed (all devices tried)\n");
            close();
            return false;
        }
        /* Try hw formats first. If none work, fall back to plug: for auto-conversion. */
        if (!configureStreamWithFallback(m_pcmPlayback, m_sampleRate, m_bufferSize,
                                          reqOutputs, &fmt, &ch)) {
            std::fprintf(stderr, "[hal] playback hw_params failed — trying plug: converter\n");
            snd_pcm_close(m_pcmPlayback);
            /* Reopen with plug: prefix for automatic format conversion. */
            char devName[256];
            (void)std::snprintf(devName, sizeof(devName), "plug:%s", m_playbackDevice);
            if (snd_pcm_open(&m_pcmPlayback, devName, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
                std::fprintf(stderr, "[hal] playback plug: open failed\n");
                close();
                return false;
            }
            /* On plug device, just set S32_LE directly — it converts. */
            if (!configureStreamWithFallback(m_pcmPlayback, m_sampleRate, m_bufferSize,
                                              reqOutputs, &fmt, &ch)) {
                std::fprintf(stderr, "[hal] playback hw_params failed (even with plug:)\n");
                close();
                return false;
            }
        }
        if (fmt != SND_PCM_FORMAT_S32_LE) {
            std::fprintf(stderr, "[hal] playback format conversion: %s → S32_LE\n",
                          snd_pcm_format_name(fmt));
        }
        m_playbackFmt = fmt;
        m_numOutputs  = ch;
    }

    /* Log negotiated parameters. */
    {
        auto logStream = [](const char* label, snd_pcm_t* pcm, uint32_t reqCh,
                            uint32_t sampleRate, uint32_t bufferSize) {
            snd_pcm_hw_params_t* cur;
            snd_pcm_hw_params_alloca(&cur);
            if (snd_pcm_hw_params_current(pcm, cur) < 0) return;

            unsigned int rate = 0, ch = 0;
            snd_pcm_uframes_t period = 0, buf = 0;
            snd_pcm_format_t fmt = SND_PCM_FORMAT_UNKNOWN;
            snd_pcm_hw_params_get_rate(cur, &rate, nullptr);
            snd_pcm_hw_params_get_channels(cur, &ch);
            snd_pcm_hw_params_get_period_size(cur, &period, NULL);
            snd_pcm_hw_params_get_buffer_size(cur, &buf);
            snd_pcm_hw_params_get_format(cur, &fmt);

            if (ch != reqCh) {
                std::fprintf(stderr, "[hal] WARNING: %s channels negotiated %u (requested %u)\n",
                              label, ch, reqCh);
            }
            if (fmt != SND_PCM_FORMAT_S32_LE) {
                std::fprintf(stderr, "[hal] WARNING: %s format is %s (expected S32_LE — conversion active)\n",
                              label, snd_pcm_format_name(fmt));
            }
            std::printf("[hal] ALSA %s confirmed: %u Hz / %u ch / period=%lu / buffer=%lu / %s\n",
                        label, rate, ch, (unsigned long)period, (unsigned long)buf,
                        snd_pcm_format_name(fmt));
        };

        logStream("playback", m_pcmPlayback, reqOutputs, m_sampleRate, m_bufferSize);
        logStream("capture",  m_pcmCapture,  reqInputs,  m_sampleRate, m_bufferSize);

        snd_pcm_sframes_t delay;
        snd_pcm_delay(m_pcmPlayback, &delay);
    }

    /* NO LINK — streams operate independently on ASYNC USB. */

    /* --- Open Room Correction PCM pair (if devices are set). --- */
    if (m_rcInputDevice && m_rcInputDevice[0] != '\0') {
        int rcErr = snd_pcm_open(&m_pcmRcCapture, m_rcInputDevice, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
        if (rcErr < 0) {
            std::fprintf(stderr, "[hal] RC capture open failed (%s): %s — disabling RC input\n",
                          m_rcInputDevice, snd_strerror(rcErr));
            m_pcmRcCapture = nullptr;
        } else {
            snd_pcm_hw_params_t* rcParams;
            snd_pcm_hw_params_alloca(&rcParams);
            snd_pcm_hw_params_any(m_pcmRcCapture, rcParams);
            snd_pcm_hw_params_set_access(m_pcmRcCapture, rcParams, SND_PCM_ACCESS_RW_INTERLEAVED);
            snd_pcm_hw_params_set_format(m_pcmRcCapture, rcParams, SND_PCM_FORMAT_S32_LE);
            unsigned int rcRate = m_sampleRate;
            snd_pcm_hw_params_set_rate_near(m_pcmRcCapture, rcParams, &rcRate, nullptr);
            snd_pcm_hw_params_set_channels(m_pcmRcCapture, rcParams, 2);
            {
                snd_pcm_uframes_t periodSize = m_bufferSize;
                snd_pcm_uframes_t bufferSize = m_bufferSize * 4;
                snd_pcm_hw_params_set_period_size_near(m_pcmRcCapture, rcParams, &periodSize, nullptr);
                snd_pcm_hw_params_set_buffer_size_near(m_pcmRcCapture, rcParams, &bufferSize);
            }
            rcErr = snd_pcm_hw_params(m_pcmRcCapture, rcParams);
            if (rcErr < 0) {
                std::fprintf(stderr, "[hal] RC capture hw_params failed: %s — disabling RC input\n", snd_strerror(rcErr));
                snd_pcm_close(m_pcmRcCapture);
                m_pcmRcCapture = nullptr;
            } else {
                /* Prime RC capture with blocking read. */
                std::vector<int32_t> rcPrime(m_bufferSize * 2, 0);
                snd_pcm_nonblock(m_pcmRcCapture, 0);
                for (int i = 0; i < 10; ++i) {
                    snd_pcm_sframes_t r = snd_pcm_readi(m_pcmRcCapture, rcPrime.data(), m_bufferSize * 2);
                    if (r > 0) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                snd_pcm_nonblock(m_pcmRcCapture, 1);
            }
        }
    }

    if (m_rcOutputDevice && m_rcOutputDevice[0] != '\0') {
        int rcErr = snd_pcm_open(&m_pcmRcPlayback, m_rcOutputDevice, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
        if (rcErr < 0) {
            std::fprintf(stderr, "[hal] RC playback open failed (%s): %s — disabling RC output\n",
                          m_rcOutputDevice, snd_strerror(rcErr));
            m_pcmRcPlayback = nullptr;
        } else {
            snd_pcm_hw_params_t* rcParams;
            snd_pcm_hw_params_alloca(&rcParams);
            snd_pcm_hw_params_any(m_pcmRcPlayback, rcParams);
            snd_pcm_hw_params_set_access(m_pcmRcPlayback, rcParams, SND_PCM_ACCESS_RW_INTERLEAVED);
            snd_pcm_hw_params_set_format(m_pcmRcPlayback, rcParams, SND_PCM_FORMAT_S32_LE);
            unsigned int rcRate = m_sampleRate;
            snd_pcm_hw_params_set_rate_near(m_pcmRcPlayback, rcParams, &rcRate, nullptr);
            snd_pcm_hw_params_set_channels(m_pcmRcPlayback, rcParams, 2);
            {
                snd_pcm_uframes_t periodSize = m_bufferSize;
                snd_pcm_uframes_t bufferSize = m_bufferSize * 4;
                snd_pcm_hw_params_set_period_size_near(m_pcmRcPlayback, rcParams, &periodSize, nullptr);
                snd_pcm_hw_params_set_buffer_size_near(m_pcmRcPlayback, rcParams, &bufferSize);
            }
            rcErr = snd_pcm_hw_params(m_pcmRcPlayback, rcParams);
            if (rcErr < 0) {
                std::fprintf(stderr, "[hal] RC playback hw_params failed: %s — disabling RC output\n", snd_strerror(rcErr));
                snd_pcm_close(m_pcmRcPlayback);
                m_pcmRcPlayback = nullptr;
            } else {
                /* Prime RC playback with silence. */
                std::vector<int32_t> rcSilence(m_bufferSize * 2, 0);
                snd_pcm_nonblock(m_pcmRcPlayback, 0);
                snd_pcm_writei(m_pcmRcPlayback, rcSilence.data(), m_bufferSize * 2);
                snd_pcm_nonblock(m_pcmRcPlayback, 1);
            }
        }
    }

    { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
      if (df) { std::fprintf(df, "[hal] openStreams() OK\n"); std::fflush(df); std::fclose(df); } }
    return true;
}

/* Open a single PCM stream with device fallback.
 * Tries requestedDevice first, then fallbackDevice (typically "default"). */
bool AlsaHal::openStreamWithFallback(snd_pcm_t** pcm, const char* requestedDevice,
                                      const char* fallbackDevice, snd_pcm_stream_t stream) {
    int err = snd_pcm_open(pcm, requestedDevice, stream, SND_PCM_NONBLOCK);
    if (err >= 0) return true;

    /* Requested device failed — try fallback. */
    std::fprintf(stderr, "[hal] %s open failed for '%s': %s — trying '%s'\n",
                  stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                  requestedDevice, snd_strerror(err), fallbackDevice);

    err = snd_pcm_open(pcm, fallbackDevice, stream, SND_PCM_NONBLOCK);
    if (err >= 0) {
        std::fprintf(stderr, "[hal] %s opened fallback device '%s'\n",
                      stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                      fallbackDevice);
        return true;
    }
    std::fprintf(stderr, "[hal] %s open failed for '%s': %s\n",
                  stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                  fallbackDevice, snd_strerror(err));
    return false;
}

/* Configure hw_params with format + channel fallback.
 * Tries formats: S32_LE → S24_3LE → S24_LE → S16_LE
 * For each format, tries channels from maxChannels down to 1. */
bool AlsaHal::configureStreamWithFallback(snd_pcm_t* pcm, uint32_t sampleRate,
                                           uint32_t bufferSize, uint32_t maxChannels,
                                           snd_pcm_format_t* outFormat, unsigned int* outChannels) {
    /* Format priority list — prefer formats that match our internal S32_LE processing. */
    static const snd_pcm_format_t fmtOrder[] = {
        SND_PCM_FORMAT_S32_LE,
        SND_PCM_FORMAT_S24_3LE,
        SND_PCM_FORMAT_S24_LE,
        SND_PCM_FORMAT_S16_LE,
    };
    static constexpr int kNumFormats = sizeof(fmtOrder) / sizeof(fmtOrder[0]);

    for (int fi = 0; fi < kNumFormats; ++fi) {
        for (unsigned int tryCh = maxChannels; tryCh >= 1; --tryCh) {
            snd_pcm_hw_params_t* params;
            snd_pcm_hw_params_alloca(&params);
            snd_pcm_hw_params_any(pcm, params);
            snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
            snd_pcm_hw_params_set_format(pcm, params, fmtOrder[fi]);

            unsigned int rate = sampleRate;
            snd_pcm_hw_params_set_rate_near(pcm, params, &rate, nullptr);
            snd_pcm_hw_params_set_channels(pcm, params, tryCh);

            {
                snd_pcm_uframes_t periodSize = bufferSize;
                snd_pcm_uframes_t bufSize = bufferSize * 4;
                snd_pcm_hw_params_set_period_size_near(pcm, params, &periodSize, nullptr);
                snd_pcm_hw_params_set_buffer_size_near(pcm, params, &bufSize);
            }

            int err = snd_pcm_hw_params(pcm, params);
            if (err >= 0) {
                *outFormat  = fmtOrder[fi];
                *outChannels = tryCh;
                return true;
            }
        }
    }
    return false;
}

void AlsaHal::recoverUnderrun() {
    (void)0; /* Not used — recovery handled inline in run(). */
}

void AlsaHal::recoverOverrun() {
    (void)0; /* Not used — recovery handled inline in run(). */
}
