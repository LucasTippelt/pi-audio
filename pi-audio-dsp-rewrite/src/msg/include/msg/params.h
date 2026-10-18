#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  Versioned parameter blocks (A3, E3)                               */
/*  Produced by config_loader from JSON, consumed via CmdLoadParams   */
/* ------------------------------------------------------------------ */

struct ParamBlockHeader {
    uint32_t version;   /* monotonically increasing param set version */
    uint32_t size;      /* bytes following this header */
    uint16_t stageCount;/* number of stage configs in payload */
};

enum StageParamType : uint16_t {
    STAGE_PARAM_GAIN      = 1,
    STAGE_PARAM_IIR_COEF  = 2,
    STAGE_PARAM_CROSSOVER = 3,
    STAGE_PARAM_DELAY     = 4,
    STAGE_PARAM_TRIM      = 5,
};

struct StageParamEntry {
    uint16_t type;      /* StageParamType */
    uint16_t stageSlot; /* target slot index (0-7) */
    uint32_t dataLen;   /* bytes in data[] */
    uint8_t  data[256]; /* compact binary coefficients */
};

static_assert(sizeof(StageParamEntry) <= 264);
