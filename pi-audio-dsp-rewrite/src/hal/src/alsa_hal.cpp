#include "hal/alsa_hal.h"
#include "hal/engine.h"
#include "dsp_core/block_info.h"

extern "C" {
#include <alsa/asoundlib.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <csignal>
#include <cstdlib>
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
    /* Pre-allocate buffers: int32 for ALSA I/O, float for DSP core. */
    size_t frameSize = sizeof(int32_t) * std::max(m_numInputs, m_numOutputs);
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

    /* S32_LE to float scale. */
    constexpr float s32Scale = 1.0f / (float)(1u << 31);

    uint64_t sampleIndex = 0;

    /* Activate capture with blocking reads (retry for USB warmup). */
    {
        snd_pcm_nonblock(m_pcmCapture, 0);
        for (int i = 0; i < 5; ++i) {
            int32_t buf[4096] = {};
            snd_pcm_sframes_t r = snd_pcm_readi(m_pcmCapture, buf, m_bufferSize * 2);
            if (r > 0) break; /* Got data — capture is active. */
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        snd_pcm_nonblock(m_pcmCapture, 1);
    }

    /* Activate playback with blocking write. */
    {
        int32_t silence[4096] = {};
        snd_pcm_nonblock(m_pcmPlayback, 0);
        snd_pcm_writei(m_pcmPlayback, silence, m_bufferSize * 2);
        snd_pcm_nonblock(m_pcmPlayback, 1);
    }

    while (true) {
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
                snd_pcm_prepare(m_pcmCapture);
                snd_pcm_start(m_pcmCapture);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                /* Fall through to process silence — keeps telemetry flowing. */
            } else {
                break;
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
                    snd_pcm_prepare(m_pcmCapture);
                    snd_pcm_start(m_pcmCapture);
                    /* Fall through to process silence. */
                } else {
                    break;
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
        for (uint32_t f = 0; f < static_cast<uint32_t>(capFrames); ++f) {
            for (uint32_t ch = 0; ch < m_numOutputs; ++ch) {
                float sample = playPlanar[ch][f] * m_fadeLevel;
                if (sample > 1.0f) sample = 1.0f;
                else if (sample < -1.0f) sample = -1.0f;
                alsaPlayback[f * m_numOutputs + ch] = static_cast<int32_t>(sample * (float)(1u << 31));
            }
        }

        /* Check playback availability before writing. */
        snd_pcm_sframes_t availPlay = snd_pcm_avail_update(m_pcmPlayback);
        if (availPlay < 0) {
            /* Error state — try to recover with prepare+start. */
            int err = snd_pcm_state(m_pcmPlayback);
            if (err == SND_PCM_STATE_XRUN || err == SND_PCM_STATE_DRAINING || err == SND_PCM_STATE_PREPARED) {
                m_xrunUnderrun++;
                snd_pcm_prepare(m_pcmPlayback);
                snd_pcm_start(m_pcmPlayback);
                sampleIndex += static_cast<uint64_t>(capFrames);
                continue;
            }
            break;
        }
        if (availPlay < static_cast<snd_pcm_sframes_t>(m_bufferSize)) {
            /* Not enough space — skip this block's output to avoid underrun. */
            sampleIndex += static_cast<uint64_t>(capFrames);
            continue;
        }

        /* Write playback (S32_LE). */
        snd_pcm_sframes_t playFrames = snd_pcm_writei(m_pcmPlayback, alsaPlayback, m_bufferSize);
        if (playFrames < 0) {
            int err = snd_pcm_state(m_pcmPlayback);
            if (err == SND_PCM_STATE_XRUN || err == SND_PCM_STATE_DRAINING || err == SND_PCM_STATE_PREPARED) {
                m_xrunUnderrun++;
                snd_pcm_prepare(m_pcmPlayback);
                snd_pcm_start(m_pcmPlayback);
                continue;
            }
            break;
        }

        /* Restore fade after xrun recovery. */
        if (m_fadeLevel < 1.0f)
            m_fadeLevel = 1.0f;

        sampleIndex += static_cast<uint64_t>(capFrames);
    }

    delete[] alsaCapture;
    delete[] alsaPlayback;
    delete[] dspCapture;
    delete[] dspPlayback;
}

void AlsaHal::close() {
    if (m_pcmCapture)  snd_pcm_close(m_pcmCapture);
    if (m_pcmPlayback) snd_pcm_close(m_pcmPlayback);
    m_pcmCapture  = nullptr;
    m_pcmPlayback = nullptr;
    munlockall();
}

bool AlsaHal::isTimerPaced() const { return m_timerPaced; }

bool AlsaHal::openStreams() {
    int err;

    /* Open capture non-blocking. */
    err = snd_pcm_open(&m_pcmCapture, m_captureDevice, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0) return false;

    /* Open playback non-blocking. */
    err = snd_pcm_open(&m_pcmPlayback, m_playbackDevice, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
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
        close();
        return false;
    }

    /* NO LINK — streams operate independently on ASYNC USB. */

    /* Get delay for fixed latency offset. */
    snd_pcm_sframes_t delay;
    snd_pcm_delay(m_pcmPlayback, &delay);

    return true;
}

void AlsaHal::recoverUnderrun() {
    (void)0; /* Not used — recovery handled inline in run(). */
}

void AlsaHal::recoverOverrun() {
    (void)0; /* Not used — recovery handled inline in run(). */
}
