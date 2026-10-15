#include "hal/alsa_hal.h"
#include "hal/engine.h"
#include "dsp_core/block_info.h"

extern "C" {
#include <alsa/asoundlib.h>
}

#include <cstring>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <sched.h>
#include <sys/mman.h>

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

    /* Lock memory to prevent page faults in RT thread. */
    mlockall(MCL_CURRENT | MCL_FUTURE);

    return true;
}

void AlsaHal::run() {
    /* Pre-allocate buffers for interleaved I/O. */
    size_t frameSize = sizeof(float) * std::max(m_numInputs, m_numOutputs);
    float* captureBuf  = new float[m_bufferSize * m_numInputs];
    float* playbackBuf = new float[m_bufferSize * m_numOutputs];

    /* Planar buffers for core. */
    float* capPlanar[10];
    float* playPlanar[10];

    for (uint32_t ch = 0; ch < m_numInputs; ++ch)
        capPlanar[ch] = captureBuf + ch * m_bufferSize;
    for (uint32_t ch = 0; ch < m_numOutputs; ++ch)
        playPlanar[ch] = playbackBuf + ch * m_bufferSize;

    uint64_t sampleIndex = 0;

    while (true) {
        /* Read capture. */
        snd_pcm_sframes_t capFrames = snd_pcm_readi(m_pcmCapture, captureBuf, m_bufferSize);
        if (capFrames < 0) {
            if (snd_pcm_state(m_pcmCapture) == SND_PCM_STATE_XRUN) {
                m_xrunOverrun++;
                recoverOverrun();
                continue;
            }
            break;
        }

        /* Deinterleave capture to planar. */
        for (uint32_t ch = 0; ch < m_numInputs; ++ch) {
            for (uint32_t f = 0; f < static_cast<uint32_t>(capFrames); ++f) {
                capPlanar[ch][f] = captureBuf[f * m_numInputs + ch];
            }
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

        /* Interleave playback from planar. */
        for (uint32_t f = 0; f < static_cast<uint32_t>(capFrames); ++f) {
            for (uint32_t ch = 0; ch < m_numOutputs; ++ch) {
                playbackBuf[f * m_numOutputs + ch] = playPlanar[ch][f] * m_fadeLevel;
            }
        }

        /* Write playback. */
        snd_pcm_sframes_t playFrames = snd_pcm_writei(m_pcmPlayback, playbackBuf, m_bufferSize);
        if (playFrames < 0) {
            if (snd_pcm_state(m_pcmPlayback) == SND_PCM_STATE_XRUN) {
                m_xrunUnderrun++;
                recoverUnderrun();
                continue;
            }
            break;
        }

        /* Restore fade after xrun recovery. */
        if (m_fadeLevel < 1.0f)
            m_fadeLevel = 1.0f;

        sampleIndex += static_cast<uint64_t>(capFrames);
    }

    delete[] captureBuf;
    delete[] playbackBuf;
}

void AlsaHal::close() {
    if (m_pcmCapture)  snd_pcm_close(static_cast<snd_pcm_t*>(m_pcmCapture));
    if (m_pcmPlayback) snd_pcm_close(static_cast<snd_pcm_t*>(m_pcmPlayback));
    m_pcmCapture  = nullptr;
    m_pcmPlayback = nullptr;
    munlockall();
}

bool AlsaHal::isTimerPaced() const { return m_timerPaced; }

bool AlsaHal::openStreams() {
    int err;

    /* Open capture. */
    err = snd_pcm_open(&m_pcmCapture, m_captureDevice, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) return false;

    /* Open playback. */
    err = snd_pcm_open(&m_pcmPlayback, m_playbackDevice, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        snd_pcm_close(static_cast<snd_pcm_t*>(m_pcmCapture));
        return false;
    }

    /* Link streams for sample synchronization. */
    snd_pcm_link(m_pcmCapture, m_pcmPlayback);

    /* Set params: FLOAT_LE, interleaved. */
    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(static_cast<snd_pcm_t*>(m_pcmPlayback), params);
    snd_pcm_hw_params_set_access(static_cast<snd_pcm_t*>(m_pcmPlayback), params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(static_cast<snd_pcm_t*>(m_pcmPlayback), params, SND_PCM_FORMAT_FLOAT_LE);
    snd_pcm_hw_params_set_rate_near(static_cast<snd_pcm_t*>(m_pcmPlayback), params, &m_sampleRate, nullptr);
    snd_pcm_hw_params_set_channels(static_cast<snd_pcm_t*>(m_pcmPlayback), params, m_numOutputs);

    err = snd_pcm_hw_params(static_cast<snd_pcm_t*>(m_pcmPlayback), params);
    if (err < 0) {
        close();
        return false;
    }

    /* Apply same params to capture. */
    snd_pcm_hw_params_any(static_cast<snd_pcm_t*>(m_pcmCapture), params);
    snd_pcm_hw_params_set_access(static_cast<snd_pcm_t*>(m_pcmCapture), params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(static_cast<snd_pcm_t*>(m_pcmCapture), params, SND_PCM_FORMAT_FLOAT_LE);
    snd_pcm_hw_params_set_channels(static_cast<snd_pcm_t*>(m_pcmCapture), params, m_numInputs);
    err = snd_pcm_hw_params(static_cast<snd_pcm_t*>(m_pcmCapture), params);
    if (err < 0) {
        close();
        return false;
    }

    /* Get delay for fixed latency offset. */
    snd_pcm_sframes_t delay;
    snd_pcm_delay(static_cast<snd_pcm_t*>(m_pcmPlayback), &delay);

    return true;
}

void AlsaHal::recoverUnderrun() {
    /* Silence fade + re-prime, never bare snd_pcm_prepare(). */
    m_fadeLevel = 0.0f;
    snd_pcm_sframes_t frames = snd_pcm_avail(static_cast<snd_pcm_t*>(m_pcmPlayback));
    if (frames < 0) frames = m_bufferSize;
    snd_pcm_start(static_cast<snd_pcm_t*>(m_pcmPlayback));

    /* Fade back in over next few blocks. */
    for (int i = 0; i < 4; ++i) {
        float* silence = new float[m_bufferSize * m_numOutputs];
        std::memset(silence, 0, sizeof(float) * m_bufferSize * m_numOutputs);
        snd_pcm_writei(m_pcmPlayback, silence, m_bufferSize);
        delete[] silence;
    }
}

void AlsaHal::recoverOverrun() {
    /* Drop with fade. */
    m_fadeLevel = 0.5f;
    snd_pcm_start(static_cast<snd_pcm_t*>(m_pcmCapture));
}
