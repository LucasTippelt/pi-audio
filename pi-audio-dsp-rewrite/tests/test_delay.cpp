#include "stages/delay_stage.h"
#include "dsp_core/block_info.h"

#include <cstdio>
#include <cstring>
#include <cmath>

/* ------------------------------------------------------------------ */
/*  DelayStage unit tests                                              */
/* ------------------------------------------------------------------ */

static bool g_passed = true;
static int g_testCount = 0;

#define ASSERT(cond, msg)                                                        \
    do {                                                                         \
        ++g_testCount;                                                           \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s (line %d)\n", msg, __LINE__);       \
            g_passed = false;                                                    \
        }                                                                        \
    } while (0)

#define ASSERT_FLOAT_EQ(a, b, eps, msg)                                          \
    do {                                                                         \
        ++g_testCount;                                                           \
        if (std::fabs((a) - (b)) > (eps)) {                                      \
            std::fprintf(stderr, "[FAIL] %s: got %.8f expected %.8f (line %d)\n", \
                         msg, static_cast<double>(a), static_cast<double>(b), __LINE__); \
            g_passed = false;                                                    \
        }                                                                        \
    } while (0)

/* ------------------------------------------------------------------ */
/*  D1: Zero delay — pass-through                                      */
/* ------------------------------------------------------------------ */
static void test_zero_delay() {
    DelayStage stage;
    stage.setDelay(0);

    float buf[128];
    for (int i = 0; i < 128; ++i) buf[i] = static_cast<float>(i % 17 - 8) / 10.0f;
    float expected[128];
    std::memcpy(expected, buf, sizeof(buf));

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 128, info);

    for (int i = 0; i < 128; ++i) {
        ASSERT_FLOAT_EQ(buf[i], expected[i], 1e-6f, "D1: zero delay pass-through");
    }
}

/* ------------------------------------------------------------------ */
/*  D2: Known delay N=64 samples                                       */
/* ------------------------------------------------------------------ */
static void test_known_delay() {
    DelayStage stage;
    const uint32_t delay = 64;
    stage.setDelay(delay);

    float buf[256];
    for (int i = 0; i < 256; ++i) buf[i] = static_cast<float>(i % 13 - 6) / 8.0f;

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 256, info);

    for (int i = 0; i < static_cast<int>(delay); ++i) {
        ASSERT_FLOAT_EQ(buf[i], 0.0f, 1e-10f, "D2: initial silence");
    }
    for (uint32_t i = delay; i < 256; ++i) {
        float expectedVal = static_cast<float>((static_cast<int>(i - delay) % 13 - 6)) / 8.0f;
        ASSERT_FLOAT_EQ(buf[i], expectedVal, 1e-6f, "D2: delayed output matches input");
    }
}

/* ------------------------------------------------------------------ */
/*  D3: Delay longer than block size (delay=512, block=128)            */
/* ------------------------------------------------------------------ */
static void test_delay_longer_than_block() {
    DelayStage stage;
    const uint32_t delay = 512;
    stage.setDelay(delay);

    float buf[128];
    BlockInfo info{};

    for (int block = 0; block < 10; ++block) {
        for (int i = 0; i < 128; ++i) {
            buf[i] = static_cast<float>((block * 128 + i) % 37 - 18) / 20.0f;
        }
        float* channels[] = {buf};
        stage.process(channels, 1, 128, info);
    }

    for (int i = 0; i < 128; ++i) {
        ASSERT(!std::isnan(buf[i]) && !std::isinf(buf[i]), "D3: no NaN/Inf");
    }
}

/* ------------------------------------------------------------------ */
/*  D4: Max delay at high sample rate (9600 samples @ 192kHz ≈ 50ms)   */
/* ------------------------------------------------------------------ */
static void test_large_delay() {
    DelayStage stage;
    const uint32_t delay = 9600;
    stage.setDelay(delay);

    ASSERT(stage.getDelaySamples() == delay, "D4: delay accepted");

    float buf[128];
    for (int i = 0; i < 128; ++i) buf[i] = 1.0f;

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 128, info);

    for (int i = 0; i < 128; ++i) {
        ASSERT_FLOAT_EQ(buf[i], 0.0f, 1e-10f, "D4: initial silence with large delay");
    }
}

/* ------------------------------------------------------------------ */
/*  D5: Latency declaration matches configured delay                   */
/* ------------------------------------------------------------------ */
static void test_latency_declaration() {
    DelayStage stage;

    ASSERT(stage.latencySamples() == 0, "D5: initial latency is zero");

    stage.setDelay(123);
    ASSERT(stage.latencySamples() == 123, "D5: latency matches configured delay");

    stage.setDelay(4800);
    ASSERT(stage.latencySamples() == 4800, "D5: latency updates with new delay");
}

/* ------------------------------------------------------------------ */
/*  D6: Buffer boundary wrap-around correctness                        */
/* ------------------------------------------------------------------ */
static void test_buffer_wrap() {
    DelayStage stage;
    const uint32_t delay = 100;
    stage.setDelay(delay);

    float buf[128];
    BlockInfo info{};

    /* Process enough blocks to wrap past the buffer boundary.
     * Total frames needed: kMaxDelay + some margin. */
    const uint32_t totalFrames = DelayStage::kMaxDelay + 500;
    const uint32_t blockSize = 128;
    uint32_t processed = 0;

    while (processed < totalFrames) {
        uint32_t remaining = totalFrames - processed;
        uint32_t frames = (remaining < blockSize) ? remaining : blockSize;

        for (uint32_t i = 0; i < frames; ++i) {
            buf[i] = static_cast<float>((processed + i) % 41 - 20) / 25.0f;
        }

        float* channels[] = {buf};
        stage.process(channels, 1, frames, info);

        for (uint32_t i = 0; i < frames; ++i) {
            ASSERT(!std::isnan(buf[i]) && !std::isinf(buf[i]), "D6: no corruption after wrap");
        }

        processed += frames;
    }
}

/* ------------------------------------------------------------------ */
/*  D7: Small delay less than block size (delay=32, block=128)         */
/* ------------------------------------------------------------------ */
static void test_small_delay_overlap() {
    DelayStage stage;
    const uint32_t delay = 32;
    stage.setDelay(delay);

    float buf[128];
    BlockInfo info{};

    for (int block = 0; block < 5; ++block) {
        for (int i = 0; i < 128; ++i) {
            buf[i] = static_cast<float>((block * 128 + i) % 29 - 14) / 15.0f;
        }

        float* channels[] = {buf};
        stage.process(channels, 1, 128, info);

        for (int i = 0; i < 128; ++i) {
            ASSERT(!std::isnan(buf[i]) && !std::isinf(buf[i]), "D7: no corruption with small delay");
        }
    }
}

/* ------------------------------------------------------------------ */
/*  D8: Multi-block continuity — ramp signal                           */
/* ------------------------------------------------------------------ */
static void test_multi_block_continuity() {
    DelayStage stage;
    const uint32_t delay = 64;
    stage.setDelay(delay);

    float buf[128];
    BlockInfo info{};

    /* Feed a continuous ramp across many blocks. */
    const int numBlocks = 50;
    for (int block = 0; block < numBlocks; ++block) {
        uint32_t baseSample = static_cast<uint32_t>(block * 128);

        for (int i = 0; i < 128; ++i) {
            buf[i] = static_cast<float>((baseSample + i) % 67 - 33) / 40.0f;
        }

        float* channels[] = {buf};
        stage.process(channels, 1, 128, info);

        /* After initial delay period, output should match input shifted by delay. */
        if (baseSample >= delay) {
            for (int i = 0; i < 128; ++i) {
                uint32_t srcIdx = baseSample + i - delay;
                float expectedVal = static_cast<float>((srcIdx % 67 - 33)) / 40.0f;
                ASSERT_FLOAT_EQ(buf[i], expectedVal, 1e-5f, "D8: multi-block continuity");
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  D9: Reset clears state                                             */
/* ------------------------------------------------------------------ */
static void test_reset() {
    DelayStage stage;
    const uint32_t delay = 64;
    stage.setDelay(delay);

    float buf[128];
    BlockInfo info{};

    /* Feed some data. */
    for (int i = 0; i < 128; ++i) buf[i] = 1.0f;
    float* channels[] = {buf};
    stage.process(channels, 1, 128, info);

    /* Reset and verify next block starts with silence for delay period. */
    stage.reset();

    for (int i = 0; i < 128; ++i) buf[i] = 0.5f;
    stage.process(channels, 1, 128, info);

    /* First block after reset should have zeros for the delay period. */
    int silenceFrames = (delay < 128) ? static_cast<int>(delay) : 128;
    for (int i = 0; i < silenceFrames; ++i) {
        ASSERT_FLOAT_EQ(buf[i], 0.0f, 1e-10f, "D9: reset produces initial silence");
    }
}

/* ------------------------------------------------------------------ */
/*  D10: Silence through with any delay                                */
/* ------------------------------------------------------------------ */
static void test_silence_through() {
    uint32_t delays[] = {0, 1, 64, 512, 9600};

    for (uint32_t delay : delays) {
        DelayStage stage;
        stage.setDelay(delay);

        float buf[128]{};
        BlockInfo info{};

        /* Process several blocks of silence. */
        for (int block = 0; block < 20; ++block) {
            float* channels[] = {buf};
            stage.process(channels, 1, 128, info);

            for (int i = 0; i < 128; ++i) {
                ASSERT_FLOAT_EQ(buf[i], 0.0f, 1e-10f, "D10: silence stays silent");
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */
int main() {
    test_zero_delay();
    test_known_delay();
    test_delay_longer_than_block();
    test_large_delay();
    test_latency_declaration();
    test_buffer_wrap();
    test_small_delay_overlap();
    test_multi_block_continuity();
    test_reset();
    test_silence_through();

    if (g_passed) {
        std::fprintf(stderr, "[PASS] All %d assertions passed.\n", g_testCount);
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some assertions failed (%d total).\n", g_testCount);
        return 1;
    }
}
