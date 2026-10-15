#include "core/engine.h"
#include "core/logger.h"
#include "core/tcp_relay.h"
#include "core/monitor_writer.h"
#include "core/ring_buffer_io.h"
#include "core/test_inject.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

void AudioEngine::captureThread() {
    Logger::getInstance().info("Capture thread started (background)");
    std::vector<float> captureBuf(m_config.bufferSize * 2, 0.0f);
    while (m_running.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(
            static_cast<int64_t>(m_config.bufferSize * 1000000LL / m_config.sampleRate)));
        snd_pcm_sframes_t frames = snd_pcm_readi(m_pcmCapture, captureBuf.data(), m_config.bufferSize);
        if (frames < 0) {
            snd_pcm_recover(m_pcmCapture, frames, 0);
            frames = 0;
        }
        if (frames > 0)
            writePgmShm(captureBuf.data(), (size_t)frames);
    }
}

void AudioEngine::pgmReaderThread() {
    size_t bs = m_config.bufferSize;
    std::vector<float> stereoBuf(bs * 2);
    std::vector<float> leftBuf(bs), rightBuf(bs);
    while (m_running.load()) {
        size_t frames = readPgmShm(stereoBuf.data(), bs);
        if (frames > 0) {
            for (size_t i = 0; i < frames; ++i) {
                leftBuf[i] = stereoBuf[i * 2];
                rightBuf[i] = stereoBuf[i * 2 + 1];
            }
            m_pgmLeftRing.push(leftBuf.data(), frames);
            m_pgmRightRing.push(rightBuf.data(), frames);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(
            static_cast<int64_t>(bs * 500000LL / m_config.sampleRate)));
    }
}

void AudioEngine::micReaderThread(int idx) {
    size_t bs = m_config.bufferSize;
    std::vector<float> buf(bs);
    while (m_running.load()) {
        size_t frames = readMicShm(idx, buf.data(), bs);
        if (frames > 0)
            m_micRing[idx].push(buf.data(), frames);
        std::this_thread::sleep_for(std::chrono::microseconds(
            static_cast<int64_t>(bs * 500000LL / m_config.sampleRate)));
    }
}

void AudioEngine::relayThread() {
    Logger::getInstance().info("Relay thread started (background)");
    while (m_relayRunning.load()) {
        bool gotData = false;
        for (int ch = 0; ch < 10; ++ch) {
            size_t frames = 0;
            const float* data = m_relayCh[ch].tryRead(frames);
            if (data) {
                tcpRelay().pushChannel((uint8_t)ch, data, frames);
                gotData = true;
            }
        }
        if (!gotData)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void AudioEngine::monitorThread() {
    Logger::getInstance().info("Monitor thread started (background)");
    monitorWriter().open();
    static const char* names[10] = {
        "pgm-l", "pgm-r", "tweeter", "woofer",
        "mic-1", "mic-2", "mic-3", "mic-4", "mic-5", "mic-6"
    };
    while (m_monitorRunning.load()) {
        bool got = false;
        for (int ch = 0; ch < 10; ++ch) {
            while (true) {
                size_t frames = 0;
                const float* data = m_monitorCh[ch].tryRead(frames);
                if (!data) break;
                monitorWriter().accumulate(names[ch], data, frames);
                got = true;
            }
        }
        if (got && monitorWriter().shouldFlush())
            monitorWriter().flush();
        if (!got)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    monitorWriter().flush();
}

void AudioEngine::playbackThread() {
    struct sched_param sp;
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &sp) != 0) {
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
            std::cerr << "[ENGINE] WARNING: SCHED_FIFO failed, running at normal priority" << std::endl;
        } else {
            std::cerr << "[ENGINE] Playback: SCHED_FIFO priority " << sp.sched_priority << std::endl;
        }
    } else {
        std::cerr << "[ENGINE] Playback: SCHED_FIFO priority " << sp.sched_priority << std::endl;
    }

    int channels = m_outputChannels;
    size_t bs = m_config.bufferSize;
    int micCount = m_micCount;

    auto& outputBuf = m_outputBuf;
    auto& pgmLeft = m_pgmLeft;
    auto& pgmRight = m_pgmRight;
    auto& tweeterBuf = m_tweeterBuf;
    auto& wooferBuf = m_wooferBuf;

    struct timespec nextTs;
    clock_gettime(CLOCK_MONOTONIC, &nextTs);
    long periodNs = (long)(bs * 1000000000LL / m_config.sampleRate);
    nextTs.tv_nsec += periodNs % 1000000000;
    if (nextTs.tv_nsec >= 1000000000) { nextTs.tv_sec++; nextTs.tv_nsec -= 1000000000; }

    while (m_running.load()) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec < nextTs.tv_sec ||
            (now.tv_sec == nextTs.tv_sec && now.tv_nsec <= nextTs.tv_nsec)) {
            struct timespec rem = {0, 0};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextTs, &rem);
        }
        nextTs.tv_nsec += periodNs % 1000000000;
        if (nextTs.tv_nsec >= 1000000000) { nextTs.tv_sec++; nextTs.tv_nsec -= 1000000000; }

        std::fill(outputBuf.begin(), outputBuf.end(), 0.0f);
        std::fill(pgmLeft.begin(), pgmLeft.end(), 0.0f);
        std::fill(pgmRight.begin(), pgmRight.end(), 0.0f);

        /* Routing: check test mode FIRST */
        static std::string lastMode;
        std::string curMode = getTestMode();
        if (curMode != lastMode) {
            lastMode = curMode;
            Logger::getInstance().info("[TEST-MODE] switched to: " + (curMode.empty() ? "none" : curMode));
        }
        bool testActive = !curMode.empty();

        if (!m_pgmSuspended.load() && !(testActive && curMode == "tweeter") && !(testActive && curMode == "woofer")) {
            size_t l = m_pgmLeftRing.pop(pgmLeft.data(), bs);
            size_t r = m_pgmRightRing.pop(pgmRight.data(), bs);
            size_t minF = (l < r) ? l : r;
            if (minF < bs) {
                std::fill(pgmLeft.begin() + minF, pgmLeft.end(), 0.0f);
                std::fill(pgmRight.begin() + minF, pgmRight.end(), 0.0f);
            }
        }

        for (int i = 0; i < micCount; ++i) {
            std::fill(m_micBuf[i].begin(), m_micBuf[i].end(), 0.0f);
            m_micRing[i].pop(m_micBuf[i].data(), bs);
        }

        /* Test injection: per-channel, read before mixing */
        static std::vector<float> testBufTweeter(bs), testBufWoofer(bs);
        static int testLogCounter = 0;
        size_t tFrames = 0, wFrames = 0;
        {
            std::fill(testBufTweeter.begin(), testBufTweeter.end(), 0.0f);
            tFrames = readTestInject("pi-audio-test-tweeter", testBufTweeter.data(), bs);
        }
        {
            std::fill(testBufWoofer.begin(), testBufWoofer.end(), 0.0f);
            wFrames = readTestInject("pi-audio-test-woofer", testBufWoofer.data(), bs);
        }
        testLogCounter++;
        if (testLogCounter % 50 == 0) {
            Logger::getInstance().info("[TEST-INJECT] tweeter=" + std::to_string(tFrames) + "f woofer=" + std::to_string(wFrames) + "f");
        }

        if (testActive && curMode == "tweeter") {
            /* Tweeter test: only tweeter gets signal, woofer silent, PGM suspended */
            for (size_t i = 0; i < bs; ++i) {
                tweeterBuf[i] = testBufTweeter[i];
                wooferBuf[i] = 0.0f;
            }
        } else if (testActive && curMode == "woofer") {
            /* Woofer test: only woofer gets signal, tweeter silent, PGM suspended */
            for (size_t i = 0; i < bs; ++i) {
                tweeterBuf[i] = 0.0f;
                wooferBuf[i] = testBufWoofer[i];
            }
        } else if (testActive && curMode == "pgm-l") {
            /* PGM_L test: no physical speakers, PGM monitoring shows raw input */
            for (size_t i = 0; i < bs; ++i) {
                tweeterBuf[i] = 0.0f;
                wooferBuf[i] = 0.0f;
            }
        } else {
            /* Normal: PGM L -> tweeter + woofer */
            for (size_t i = 0; i < bs; ++i) {
                tweeterBuf[i] = pgmLeft[i];
                wooferBuf[i] = pgmLeft[i];
            }
        }

        /* Per-output DSP: NO locks (double-buffered stages) */
        processDspTweeter(tweeterBuf.data(), bs);
        processDspWoofer(wooferBuf.data(), bs);

        /* Interleave for ALSA: tweeter on ch0, woofer on ch1 */
        if (channels >= 2) {
            for (size_t i = 0; i < bs; ++i) {
                outputBuf[i * 2 + 0] = tweeterBuf[i];
                outputBuf[i * 2 + 1] = wooferBuf[i];
            }
        }

        snd_pcm_sframes_t written = snd_pcm_writei(m_pcmPlayback, outputBuf.data(), bs);
        if (written < 0) {
            if (written == -EPIPE) {
                snd_pcm_prepare(m_pcmPlayback);
            } else {
                snd_pcm_recover(m_pcmPlayback, written, 0);
            }
        }

        /* Push to relay: ch0=pgm-l, ch1=pgm-r, ch2=tweeter-output, ch3=woofer-output, ch4..=mics */
        m_relayCh[CH_PGM_L].push(pgmLeft.data(), bs);
        m_relayCh[CH_PGM_R].push(pgmRight.data(), bs);
        m_relayCh[CH_TWEETER].push(tweeterBuf.data(), bs);
        m_relayCh[CH_WOOFER].push(wooferBuf.data(), bs);
        for (int i = 0; i < micCount; ++i)
            m_relayCh[CH_MIC_BASE + i].push(m_micBuf[i].data(), bs);

        /* Push to monitor double-buffers */
        m_monitorCh[CH_PGM_L].push(pgmLeft.data(), bs);
        m_monitorCh[CH_PGM_R].push(pgmRight.data(), bs);
        m_monitorCh[CH_TWEETER].push(tweeterBuf.data(), bs);
        m_monitorCh[CH_WOOFER].push(wooferBuf.data(), bs);
        for (int i = 0; i < micCount; ++i)
            m_monitorCh[CH_MIC_BASE + i].push(m_micBuf[i].data(), bs);

        /* Push to HTTP stream server (direct to browser, no Node.js) */
        m_httpServer.pushSamples(CH_PGM_L, pgmLeft.data(), bs);
        m_httpServer.pushSamples(CH_PGM_R, pgmRight.data(), bs);
        m_httpServer.pushSamples(CH_TWEETER, tweeterBuf.data(), bs);
        m_httpServer.pushSamples(CH_WOOFER, wooferBuf.data(), bs);
        for (int i = 0; i < micCount; ++i)
            m_httpServer.pushSamples(CH_MIC_BASE + i, m_micBuf[i].data(), bs);

        /* Push to WebSocket server (frame-based, no TCP batching) */
        m_wsServer.pushAudio(CH_PGM_L, pgmLeft.data(), bs);
        m_wsServer.pushAudio(CH_PGM_R, pgmRight.data(), bs);
        m_wsServer.pushAudio(CH_TWEETER, tweeterBuf.data(), bs);
        m_wsServer.pushAudio(CH_WOOFER, wooferBuf.data(), bs);
        for (int i = 0; i < micCount; ++i)
            m_wsServer.pushAudio(CH_MIC_BASE + i, m_micBuf[i].data(), bs);

        /* Periodic playback stats: log push timing every 1000 frames (~10.7s) */
        static int statsCounter = 0;
        static struct timespec statsLastTs = {0, 0};
        statsCounter++;
        if (statsCounter % 1000 == 0) {
            struct timespec tsNow;
            clock_gettime(CLOCK_MONOTONIC, &tsNow);
            double elapsed = 0;
            if (statsLastTs.tv_sec > 0) {
                elapsed = (tsNow.tv_sec - statsLastTs.tv_sec) * 1000.0 +
                          (tsNow.tv_nsec - statsLastTs.tv_nsec) / 1000000.0;
            }
            double expected = 1000.0 * bs * 1000.0 / m_config.sampleRate;
            double jitter = elapsed - expected;
            std::cerr << "[PLAYBACK-STATS] 1000 frames: elapsed=" <<
                static_cast<int>(elapsed) << "ms expected=" <<
                static_cast<int>(expected) << "ms jitter=" <<
                static_cast<int>(jitter) << "ms" << std::endl;
            statsLastTs = tsNow;
        }
    }
}
