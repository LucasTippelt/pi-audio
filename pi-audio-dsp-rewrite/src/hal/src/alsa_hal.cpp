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
    m_pcmCapture  = nullptr;
    m_pcmPlayback = nullptr;
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
    int err;

    { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
      if (df) { std::fprintf(df, "[hal] openStreams(%u/%u) cap=%s pb=%s\n", m_sampleRate, m_bufferSize, m_captureDevice, m_playbackDevice); std::fflush(df); std::fclose(df); } }

    /* Open capture non-blocking. */
    err = snd_pcm_open(&m_pcmCapture, m_captureDevice, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0) { std::fprintf(stderr, "[hal] capture open failed: %s\n", snd_strerror(err)); return false; }

    /* Open playback non-blocking. */
    err = snd_pcm_open(&m_pcmPlayback, m_playbackDevice, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        std::fprintf(stderr, "[hal] playback open failed: %s\n", snd_strerror(err));
        snd_pcm_close(m_pcmCapture);
        return false;
    }

    /* Set params: S32_LE (hardware native for UR12), interleaved. */
    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(m_pcmPlayback, params);
    snd_pcm_hw_params_set_access(m_pcmPlayback, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(m_pcmPlayback, params, SND_PCM_FORMAT_S32_LE);
    unsigned int playRate = m_sampleRate;
    snd_pcm_hw_params_set_rate_near(m_pcmPlayback, params, &playRate, nullptr);
    snd_pcm_hw_params_set_channels(m_pcmPlayback, params, m_numOutputs);

    /* Set buffer size for ASYNC USB stability. */
    {
        snd_pcm_uframes_t periodSize = m_bufferSize;
        snd_pcm_uframes_t bufferSize = m_bufferSize * 4;
        snd_pcm_hw_params_set_period_size_near(m_pcmPlayback, params, &periodSize, nullptr);
        snd_pcm_hw_params_set_buffer_size_near(m_pcmPlayback, params, &bufferSize);
    }

    err = snd_pcm_hw_params(m_pcmPlayback, params);
    if (err < 0) {
        std::fprintf(stderr, "[hal] playback hw_params failed: %s\n", snd_strerror(err));
        close();
        return false;
    }

    /* Apply same params to capture — use fresh object. */
    {
        snd_pcm_hw_params_t* capParams;
        snd_pcm_hw_params_alloca(&capParams);
        snd_pcm_hw_params_any(m_pcmCapture, capParams);
        snd_pcm_hw_params_set_access(m_pcmCapture, capParams, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(m_pcmCapture, capParams, SND_PCM_FORMAT_S32_LE);
        unsigned int capRate = m_sampleRate;
        snd_pcm_hw_params_set_rate_near(m_pcmCapture, capParams, &capRate, nullptr);
        snd_pcm_hw_params_set_channels(m_pcmCapture, capParams, m_numInputs);

        /* Set buffer size for ASYNC USB stability. */
        {
            snd_pcm_uframes_t periodSize = m_bufferSize;
            snd_pcm_uframes_t bufferSize = m_bufferSize * 4;
            snd_pcm_hw_params_set_period_size_near(m_pcmCapture, capParams, &periodSize, nullptr);
            snd_pcm_hw_params_set_buffer_size_near(m_pcmCapture, capParams, &bufferSize);
        }

        err = snd_pcm_hw_params(m_pcmCapture, capParams);
    }
    if (err < 0) {
        std::fprintf(stderr, "[hal] capture hw_params failed: %s\n", snd_strerror(err));
        close();
        return false;
    }

    /* NO LINK — streams operate independently on ASYNC USB. */

    /* Verify actual negotiated parameters. */
    {
        snd_pcm_hw_params_t* cur;
        snd_pcm_hw_params_alloca(&cur);
        snd_pcm_hw_params_current(m_pcmPlayback, cur);

        unsigned int actualRate = 0;
        snd_pcm_uframes_t actualPeriod = 0, actualBuffer = 0;
        snd_pcm_hw_params_get_rate(cur, &actualRate, nullptr);
        snd_pcm_hw_params_get_period_size(cur, &actualPeriod, NULL);
        snd_pcm_hw_params_get_buffer_size(cur, &actualBuffer);

        if (actualRate != m_sampleRate || actualPeriod != m_bufferSize) {
            std::fprintf(stderr, "[hal] WARNING: ALSA negotiated %u Hz / period=%lu / buffer=%lu "
                                 "(requested %u Hz / period=%u / buffer=%lu)\n",
                         actualRate, (unsigned long)actualPeriod, (unsigned long)actualBuffer,
                         m_sampleRate, (unsigned int)m_bufferSize, (unsigned long)(m_bufferSize * 4));
        } else {
            std::printf("[hal] ALSA params confirmed: %u Hz / period=%lu / buffer=%lu\n",
                        actualRate, (unsigned long)actualPeriod, (unsigned long)actualBuffer);
        }
    }

    /* Get delay for fixed latency offset. */
    snd_pcm_sframes_t delay;
    snd_pcm_delay(m_pcmPlayback, &delay);

    { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
      if (df) { std::fprintf(df, "[hal] openStreams() OK\n"); std::fflush(df); std::fclose(df); } }
    return true;
}

void AlsaHal::recoverUnderrun() {
    (void)0; /* Not used — recovery handled inline in run(). */
}

void AlsaHal::recoverOverrun() {
    (void)0; /* Not used — recovery handled inline in run(). */
}
