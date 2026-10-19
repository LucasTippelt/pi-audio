#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  Telemetry status block (A3)                                       */
/*  Fixed-size, pushed from core to control plane per block           */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)

struct TelemetryBlock {
    uint16_t version;          /* protocol version, currently 1 */
    uint32_t seq;              /* matches command seq for round-trip correlation */
    uint32_t xrunUnderrun;     /* cumulative underrun count */
    uint32_t xrunOverrun;      /* cumulative overrun count */
    uint8_t  clockState;       /* 0=Internal, 1=Locked, 2=Holdover, 3=Unlocked */
    float    outputLevelDb[2]; /* peak meter per output channel */
    float    inputLevelDb[8];  /* peak meter per input channel */
    uint32_t latencyFrames;    /* total pipeline latency in frames */
    uint32_t coreVersion;      /* packed major.minor.patch */
    uint64_t sampleIndex;      /* current processing sample index */
};

#pragma pack(pop)

static_assert(sizeof(TelemetryBlock) <= 128);
