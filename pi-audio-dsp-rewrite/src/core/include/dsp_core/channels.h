#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  Channel routing abstraction (B3 prep)                             */
/* ------------------------------------------------------------------ */

enum ChannelSource : uint8_t {
    CH_DEVICE = 0, /* sample-synchronous device channel */
    CH_ASYNC   = 1, /* async source with resampler (shm PGM, network) */
};

struct ChannelMap {
    uint8_t sourceIndex;
    ChannelSource sourceType;
    float gain;
};

/* Max channels supported. */
constexpr uint8_t kMaxChannels = 10;
