# Phase 1: Architecture Split & Portability (Sections A1–A5)

## Goal

Extract a portable DSP core with zero OS/file I/O, establish a HAL callback model where the HAL owns the audio thread and calls `Engine::onBlock()`, define a binary message interface replacing JSON in the processing path, enforce strict C++ portability constraints, and set up the HAL test ladder.

## Ownership Model (Resolved)

```
HAL (audio thread) → Engine::onBlock() → drain command queue → core.process(in, out, nframes) → telemetry out
Control plane ←→ command queue / telemetry ←→ Engine ←→ DSP Core
```

The Engine is the HAL-side orchestrator. It owns the DSP core instance and the lock-free command/telemetry queues. The control plane talks only through those queues.

---

## Target File Tree

New code lives in `pi-audio-dsp-rewrite/`. Old `pi-audio-dsp/` is untouched until Phase N migration.

```
pi-audio-dsp-rewrite/
├── CMakeLists.txt
├── src/
│   ├── core/                          # Portable DSP core (A1, A3, A4)
│   │   ├── include/
│   │   │   └── dsp_core/
│   │   │       ├── dsp_core.h         # Core entry point + types
│   │   │       ├── stage.h            # Stage interface + registry
│   │   │       ├── block_info.h       # BlockInfo struct (A1)
│   │   │       ├── channels.h         # Channel routing abstraction (B3 prep)
│   │   │       └── portability.h      # A4 constraint macros + static asserts
│   │   └── src/
│   │       ├── dsp_core.cpp           # Core init, process(), stage invocation
│   │       ├── stages/
│   │       │   ├── gain_stage.h       # Volume/mute with ramp (F1 prep)
│   │       │   ├── gain_stage.cpp
│   │       │   └── null_stage.h       # No-op stage for budget measurement
│   │       └── routing.cpp            # Channel mixing logic extracted from playback thread
│   ├── hal/                           # HAL abstraction + implementations (A1, A5)
│   │   ├── include/
│   │   │   └── hal/
│   │   │       ├── hal.h              # AudioHal abstract base class
│   │   │       ├── engine.h           # Engine: owns core, queues, onBlock() callback
│   │   │       ├── sim_hal.h          # SimHal: file/generator-driven, faster-than-RT (A5)
│   │   │       └── alsa_hal.h         # AlsaHal: ALSA streams, RT thread (A5)
│   │   └── src/
│   │       ├── engine.cpp             # onBlock() implementation, command drain, core dispatch
│   │       ├── sim_hal.cpp            # SimHal with synthetic clock drift injection
│   │       └── alsa_hal.cpp           # ALSA open/link/set_params/writei/xrun recovery
│   ├── msg/                           # Binary message interface (A3)
│   │   ├── include/
│   │   │   └── msg/
│   │   │       ├── command.h          # Command types, versioned header, binary structs
│   │   │       ├── telemetry.h        # Telemetry status block
│   │   │       ├── params.h           # Versioned parameter blocks
│   │   │       └── queue.h            # Lock-free SPSC command/telemetry queues
│   │   └── src/
│   │       └── queue.cpp              # (mostly header-only; compile unit for completeness)
│   ├── control/                       # Control plane stubs (A1)
│   │   ├── include/
│   │   │   └── control/
│   │   │       ├── config_loader.h    # JSON→binary parameter conversion (E3)
│   │   │       └── state_writer.h     # engine-state.json writer
│   │   └── src/
│   │       ├── config_loader.cpp      # Parses JSON, validates, precomputes coefficients
│   │       └── state_writer.cpp       # Periodic state file writer (background thread)
│   └── bin/
│       └── pi-audio-engine.cpp        # New main: wires HAL + Engine + control plane
├── tests/
│   ├── test_dsp_core.cpp              # Unit test: core.process() with SimHal
│   ├── test_command_queue.cpp         # Unit test: lock-free queue correctness
│   └── test_sim_hal.cpp               # Integration: SimHal → Engine → Core pipeline
└── include/                           # Public API umbrella headers
    └── pi_audio_dsp.h                 # Single include for consumers
```

---

## Task List

### Task 1: Define portability constraints (A4) — Foundation

**Why first**: Every subsequent header must compile under these rules. Establish the constraint macros and static asserts before writing any processing code.

**Files**: `core/include/dsp_core/portability.h`

- Define `DSP_NO_HEAP`, `DSP_FLOAT_ONLY`, `DSP_MAX_BLOCK_SIZE` (e.g., 2048)
- Static assert: `static_assert(std::is_same_v<float, float>, "single precision only")` — guard against accidental double promotion
- Macro: `DSP_STAGE_TIMING(name)` — per-stage cycle timer that logs to a fixed-size ring buffer (no heap, no std::string)
- Compile flag: `-fno-exceptions -fno-rtti -ffast-math` for the core library target
- Ban list enforced by header guards: `#include <string>` and `#include <vector>` must not appear in any `.cpp` under `core/src/` after init

### Task 2: Design binary message interface (A3) — Foundation

**Why second**: The Engine↔Core contract is the structural backbone. Both sides are coded against it.

**Files**: `msg/include/msg/command.h`, `msg/include/msg/telemetry.h`, `msg/include/msg/params.h`, `msg/include/msg/queue.h`

#### Command struct (A3)

```cpp
#pragma pack(push, 1)
struct CmdHeader {
    uint16_t version;       // 1
    uint16_t type;          // enum below
    uint32_t seq;           // monotonically increasing sequence number
    uint64_t applyAtSample; // 0 = immediate at next block boundary
};

enum CmdType : uint16_t {
    CMD_SET_GAIN        = 1,
    CMD_SET_MUTE        = 2,
    CMD_LOAD_PARAMS     = 3,   // versioned parameter block (A3)
    CMD_SCHEDULE        = 4,   // schedule command at sample N
    CMD_ENTER_TEST_MODE = 5,
    CMD_LEAVE_TEST_MODE = 6,
};

struct CmdSetGain {
    CmdHeader header;     // header.type == CMD_SET_GAIN
    float gain;           // linear, 0.0–1.0
};

struct CmdSetMute {
    CmdHeader header;     // header.type == CMD_SET_MUTE
    uint8_t muted;        // 0 or 1
};

struct CmdLoadParams {
    CmdHeader header;     // header.type == CMD_LOAD_PARAMS
    uint32_t paramVersion;// monotonically increasing
    uint32_t payloadSize; // bytes in payload[]
    uint8_t  payload[512];// compact binary parameter block (A3, E3)
};

struct CmdTestMode {
    CmdHeader header;     // CMD_ENTER_TEST_MODE or CMD_LEAVE_TEST_MODE
    uint8_t target;       // 0=tweeter, 1=woofer, 2=pgm-l
};
#pragma pack(pop)

// All command structs are <= 64 bytes (fits in one cache line)
static_assert(sizeof(CmdLoadParams) <= 576);
```

#### Telemetry struct (A3)

```cpp
#pragma pack(push, 1)
struct TelemetryBlock {
    uint16_t version;           // 1
    uint32_t seq;               // matches command seq for round-trip correlation
    uint32_t xrunUnderrun;      // cumulative underrun count (B4)
    uint32_t xrunOverrun;       // cumulative overrun count (B4)
    uint8_t  clockState;        // 0=Internal, 1=Locked, 2=Holdover, 3=Unlocked (B6)
    float    outputLevelDb[2];  // peak meter per output channel
    float    inputLevelDb[8];   // peak meter per input channel (PGM + mics)
    uint32_t latencyFrames;     // total pipeline latency in frames (F4)
    uint32_t coreVersion;       // packed major.minor.patch
    uint64_t sampleIndex;       // current processing sample index
};
#pragma pack(pop)

static_assert(sizeof(TelemetryBlock) <= 128);
```

#### Lock-free SPSC queue (A3 transport)

```cpp
template<typename T, size_t Capacity>
class CmdQueue {
    // Fixed-size ring of T objects. Push from control plane, pop in Engine::onBlock().
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
    T m_buf[Capacity];
public:
    bool push(const T& cmd);       // returns false if full (control plane drops or retries)
    size_t pop(T* out, size_t max);// drains up to max commands
    size_t pending() const;        // non-blocking peek
};
```

### Task 3: Define DSP core interface and BlockInfo (A1, A4)

**Files**: `core/include/dsp_core/dsp_core.h`, `core/include/dsp_core/block_info.h`, `core/include/dsp_core/stage.h`

#### BlockInfo — HAL→Core per-block context (A1)

```cpp
struct BlockInfo {
    uint64_t sampleIndex;  // absolute sample position of first frame in block
    uint64_t hostTimeNs;   // monotonic clock at block start
    uint32_t frames;       // number of frames in this block
    uint8_t  clockState;   // 0=Internal, 1=Locked, 2=Holdover, 3=Unlocked (B6)
};

static_assert(sizeof(BlockInfo) <= 32);
```

#### Stage interface (A4 — no std::function, no heap)

```cpp
struct Stage {
    virtual ~Stage() = default;
    // Process in-place on planar float buffer. No allocation allowed.
    virtual void process(float* const channels[], uint32_t numChannels,
                         uint32_t frames, const BlockInfo& info) = 0;
    // Declared group delay in samples (F4)
    virtual uint32_t latencySamples() const = 0;
};

// Opaque handle for double-buffered stage swap (I2 prep, C3)
struct StageSet {
    // Two slots: active and pending. Swap at block boundary.
    Stage* m_active[8];   // max 8 stages per chain
    Stage* m_pending[8];
    uint8_t m_count;
    void addPending(Stage* s);
    void swap();          // called in Engine::onBlock() before core.process()
};
```

#### Core entry point (A1)

```cpp
class DspCore {
public:
    // Init: allocate internal buffers, precompute coefficients. After this, no heap.
    bool init(uint32_t sampleRate, uint32_t maxBlockSize,
              uint32_t numInputs, uint32_t numOutputs);

    // Main processing entry point (A1)
    // in/out are planar float arrays: channels[ch][frame]
    void process(float* const in[],  uint32_t numInCh,
                 float* const out[], uint32_t numOutCh,
                 uint32_t frames, const BlockInfo& info);

    // Telemetry snapshot (A3)
    void getTelemetry(TelemetryBlock& out) const;

    uint32_t totalLatencySamples() const;  // sum of stage latencies (F4)
};
```

### Task 4: Implement HAL abstraction and Engine (A1, A5)

**Files**: `hal/include/hal/hal.h`, `hal/include/hal/engine.h`, `hal/src/engine.cpp`

#### AudioHal abstract base (A1, A5)

```cpp
class AudioHal {
public:
    virtual ~AudioHal() = default;

    // Open and configure the audio backend. Called once before run().
    virtual bool open(uint32_t sampleRate, uint32_t bufferSize,
                      uint32_t numInputs, uint32_t numOutputs) = 0;

    // Main loop: runs in RT thread (or faster-than-RT for SimHal).
    // Calls m_engine->onBlock() each period.
    virtual void run() = 0;

    virtual void close() = 0;

    // HAL-specific configuration
    virtual int rtPriority() const { return 75; }       // C4: default 75, not max
    virtual bool isTimerPaced() const { return false; }  // A5: auto-detected for null device
};
```

#### Engine — HAL-side orchestrator (A1)

```cpp
class Engine {
public:
    Engine();
    ~Engine();

    // Called by HAL once per audio block (A1).
    // This is the ONLY entry point from the HAL into the Engine.
    void onBlock(float* const in[],  uint32_t numInCh,
                 float* const out[], uint32_t numOutCh,
                 const BlockInfo& info);

    // Control plane registers its command/telemetry queues here.
    void setCommandQueue(CmdQueue<CmdLoadParams, 16>* cmdQ);
    void setTelemetryQueue(CmdQueue<TelemetryBlock, 8>* telQ);

private:
    DspCore m_core;
    StageSet m_stageSets[2]; // tweeter chain, woofer chain
    CmdQueue<CmdLoadParams, 16>* m_cmdQueue = nullptr;
    CmdQueue<TelemetryBlock, 8>* m_telQueue = nullptr;

    void drainCommands();     // apply pending commands at block boundary
    void pushTelemetry(const BlockInfo& info);
};
```

#### Engine::onBlock() flow (A1)

```
HAL wakes (device-driven or timer)
  → HAL prepares planar float buffers + BlockInfo
  → Engine::onBlock(in, out, info)
      → drainCommands() — pop from command queue, apply at block boundary
      → stage swap if pending stages exist (C3: no allocation on RT thread)
      → m_core.process(in, out, frames, info)
      → pushTelemetry(info) — write to telemetry queue
  → HAL writes output to device (or advances virtual clock for SimHal)
```

### Task 5: Implement AlsaHal (A5)

**Files**: `hal/include/hal/alsa_hal.h`, `hal/src/alsa_hal.cpp`

- Opens capture and playback streams with `snd_pcm_open()` (B2: same card, linked with `snd_pcm_link()`)
- Sets format to FLOAT_LE, interleaved access
- Main loop: `snd_pcm_readi()` → deinterleave to planar → `Engine::onBlock()` → interleave output → `snd_pcm_writei()`
- Xrun recovery: underrun → silence fade + re-prime (B4), never bare `snd_pcm_prepare()`; overrun → drop with fade (B4)
- Uses `snd_pcm_delay()` for fixed latency offset (B2)
- RT priority via `sched_setscheduler()` with priority 75, not max (C4)
- Timer pacing auto-detected: `snd_pcm_type() == SND_PCM_TYPE_NULL` (A5)
- Safety net: switches to timer pacing if `writei` returns in under half a period repeatedly

### Task 6: Implement SimHal (A5)

**Files**: `hal/include/hal/sim_hal.h`, `hal/src/sim_hal.cpp`

- No ALSA dependency. Compiles on any platform including Windows CI.
- Runs faster than real time from files or generated signals
- Injects synthetic clock offsets (+30 ppm), lock/unlock/holdover events (B6)
- Configurable: input source (file, sine wave, white noise), sample rate, buffer size
- Used in CI to unit-test DSP core, drift controller, xrun fades, and lock-loss behavior

### Task 7: Implement initial DSP stages (A4, F1 prep)

**Files**: `core/src/stages/gain_stage.h`, `core/src/stages/gain_stage.cpp`

- Gain stage with ramp/smoothing to avoid clicks (F1): linear interpolation over N samples when gain changes
- Mute as gain=0 with same ramp behavior
- No heap allocation, no std::string/vector in process path
- Declares latency of 0 samples (F4)

### Task 8: Control plane stubs (A1, E3)

**Files**: `control/include/control/config_loader.h`, `control/src/config_loader.cpp`

- Parses JSON config on the Linux side (E3)
- Validates parameters, precomputes filter coefficients
- Outputs versioned binary parameter blocks matching `CmdLoadParams::payload[]` format (A3)
- Does NOT send JSON to the core — only compact binary

### Task 9: New main binary wiring (A1)

**Files**: `src/bin/pi-audio-engine.cpp`

- Constructs HAL (AlsaHal or SimHal based on config/environment)
- Constructs Engine, registers command/telemetry queues
- Control plane thread: loads JSON config → converts to binary params → pushes commands
- Background state writer thread: polls telemetry queue → writes engine-state.json
- No file I/O, no network syscalls from the RT thread (C1, C2)

### Task 10: CMake build configuration

**Files**: `pi-audio-dsp-rewrite/CMakeLists.txt`

- Separate targets:
  - `dsp_core` — portable library, compiled with `-fno-exceptions -fno-rtti`, no ALSA dependency
  - `hal_sim` — SimHal implementation, no ALSA dependency
  - `hal_alsa` — AlsaHal implementation, links ALSA
  - `msg_lib` — message queue headers (header-only) + compile unit
  - `control_plane` — JSON parser, config loader, state writer
  - `pi-audio-engine` — main binary, links all targets
- Test targets: `test_dsp_core`, `test_command_queue`, `test_sim_hal`

### Task 11: Unit tests (A5)

**Files**: `tests/test_dsp_core.cpp`, `tests/test_command_queue.cpp`, `tests/test_sim_hal.cpp`

- `test_dsp_core`: instantiate DspCore with SimHal-generated buffers, verify process() output, check telemetry
- `test_command_queue`: stress-test lock-free queue under concurrent push/pop
- `test_sim_hal`: full pipeline test — SimHal → Engine → Core, inject clock drift, verify behavior

---

## Key Constraints (A4)

| Constraint | Enforcement |
|---|---|
| No heap after init | Static asserts + code review; core library compiled separately |
| No exceptions/RTTI | CMake flags: `-fno-exceptions -fno-rtti` on `dsp_core` target |
| No std::string/vector in process path | Header guard in `portability.h`; compile fails if included |
| Single-precision float only | `-Wdouble-promotion` warning as error; static assert on types |
| Fixed max block size | Template parameter + static assert on buffer sizes |
| Per-stage timing budget | `DSP_STAGE_TIMING()` macro logs to fixed-size ring buffer |

## Data Flow Summary

```
┌─────────────┐     JSON      ┌──────────────────┐
│  Control    │ ────────────► │  Config Loader   │ (E3: parse, validate, precompute)
│  Plane      │               │  (Linux side)    │
│  (config,   │               └────────┬─────────┘
│   reload,   │                        │ binary params
│   state)    │               ┌────────▼─────────┐
│             │ ◄──────────── │ Telemetry Queue  │
└─────────────┘     status    └────────┬─────────┘
                                       ▲
                          ┌────────────┴─────────┐
                          │   Command Queue      │ (A3: lock-free SPSC)
                          └────────────┬─────────┘
                                       │
                          ┌────────────▼─────────┐     onBlock()    ┌──────────┐
                          │       Engine         │ ◄────────────── │  HAL     │ (A1, A5)
                          │  - drain commands    │                 │  owns    │
                          │  - stage swap        │   planar float  │  audio   │
                          │  - dispatch to core  │   buffers +     │  thread  │
                          └────────────┬─────────┘   BlockInfo     └──────────┘
                                       │
                          ┌────────────▼─────────┐
                          │      DSP Core        │ (A1, A4)
                          │  - process(in,out,N) │
                          │  - zero OS calls     │
                          │  - no heap after init│
                          └──────────────────────┘
```

## Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Lock-free queue loses commands under load | Queue size is conservative (16 cmds, 8 telemetry). Control plane retries on full. Commands have sequence numbers for gap detection. |
| SimHal doesn't catch real-time issues | A5 test ladder: SimHal → AlsaHal+null → AlsaHal+real → target silicon. Each rung catches different classes of bugs. |
| Portability constraints too strict for development | Constraints apply to `dsp_core` target only. HAL and control plane can use full C++. |
| Binary param format drifts from JSON config | Config loader unit tests verify round-trip: JSON → binary → reconstructed values match within tolerance. |

## Validation Plan

1. **Compile check**: `dsp_core` builds with `-fno-exceptions -fno-rtti` and no ALSA headers
2. **SimHal test**: Run `test_sim_hal` — verify core processes blocks, telemetry is populated, commands are applied at block boundaries
3. **Queue stress test**: Concurrent push/pop on command queue for 1M iterations, zero lost/duplicated commands
4. **AlsaHal+null test**: Run engine with `default:none` ALSA device, verify timer pacing activates and blocks process correctly
5. **Portability audit**: Grep `dsp_core/` source for banned includes (`<string>`, `<vector>` in `.cpp` files outside init)
