#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  Binary command interface (A3)                                     */
/*  Fixed-size, versioned structs for engine-to-core communication    */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)

struct CmdHeader {
    uint16_t version;       /* protocol version, currently 1 */
    uint16_t type;          /* CmdType enum */
    uint32_t seq;           /* monotonically increasing sequence number */
    uint64_t applyAtSample; /* 0 = immediate at next block boundary */
};

enum CmdType : uint16_t {
    CMD_SET_GAIN        = 1,
    CMD_SET_MUTE        = 2,
    CMD_LOAD_PARAMS     = 3,
    CMD_SCHEDULE        = 4,
    CMD_ENTER_TEST_MODE = 5,
    CMD_LEAVE_TEST_MODE = 6,
    CMD_SET_TEST_MODE   = 7,
};

struct CmdSetGain {
    CmdHeader header;
    float gain; /* linear, 0.0–1.0 */
};

struct CmdSetMute {
    CmdHeader header;
    uint8_t muted; /* 0 or 1 */
};

/* Generic control command for real-time gain/mute routing.
 * Fits in one cache line, union shares payload space. */
struct CmdGeneric {
    CmdHeader header;
    union {
        float gain;       /* for CMD_SET_GAIN */
        uint8_t muted;    /* for CMD_SET_MUTE */
    };
};

struct CmdLoadParams {
    CmdHeader header;
    uint32_t paramVersion; /* monotonically increasing */
    uint32_t payloadSize;  /* bytes in payload[] */
    uint8_t  payload[2048];/* compact binary parameter block */
};

struct CmdTestMode {
    CmdHeader header; /* CMD_ENTER_TEST_MODE or CMD_LEAVE_TEST_MODE */
    uint8_t target;   /* 0=woofer, 1=tweeter, 2=pgm-l */
};

/* C1: Set test mode via command queue — atomic update at block boundary. */
struct CmdSetTestMode {
    CmdHeader header; /* CMD_SET_TEST_MODE */
    uint8_t mode;     /* 0=none, 1=woofer-noise, 2=tweeter-noise, 3=pgm-test (PGM_L to both chains) */
};

#pragma pack(pop)

static_assert(sizeof(CmdSetGain) <= 64);
static_assert(sizeof(CmdSetMute) <= 64);
static_assert(sizeof(CmdGeneric) <= 64);
static_assert(sizeof(CmdLoadParams) <= 2120);
static_assert(sizeof(CmdTestMode) <= 64);
static_assert(sizeof(CmdSetTestMode) <= 64);
