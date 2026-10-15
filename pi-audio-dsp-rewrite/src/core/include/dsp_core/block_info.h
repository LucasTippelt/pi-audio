#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  BlockInfo — HAL-to-Core per-block context (A1)                    */
/* ------------------------------------------------------------------ */

struct BlockInfo {
    uint64_t sampleIndex; /* absolute sample position of first frame in block */
    uint64_t hostTimeNs;  /* monotonic clock at block start */
    uint32_t frames;      /* number of frames in this block */
    uint8_t  clockState;  /* 0=Internal, 1=Locked, 2=Holdover, 3=Unlocked */
};

static_assert(sizeof(BlockInfo) <= 32);
