# Pi Audio DSP Rewrite

Portable real-time audio DSP engine with hardware abstraction layer, command/telemetry message interface, and control plane. Designed for Raspberry Pi (ALSA) with a migration path to bare-metal DSP/MCU targets.

## Requirements

- **CMake** 3.16+
- **GCC** with C++17 support
- **libasound2-dev** (ALSA development headers, Linux only)
- `sudo apt install build-essential cmake libasound2-dev pkg-config`

No external runtime dependencies — bundled header-only JSON parser in control plane. DSP core has zero library dependencies.

## Build

### Quick Start (Interactive)

Run `build.sh` for an interactive prompt that lets you choose diagnostic logging with arrow keys and Enter:

```bash
./build.sh
```

The script will ask whether to enable diagnostic file logging (`/run/pi-audio/diag.log`). Default is **OFF** (production mode). Use ↑↓ arrows to select, Enter to confirm.

### Manual Build

```bash
mkdir build && cd build
cmake .. -DPI_AUDIO_DIAG_LOG=OFF
make -j$(nproc)
```

### Diagnostic Logging Option

| Setting | Flag | Effect |
|---|---|---|
| **Production** (default) | `-DPI_AUDIO_DIAG_LOG=OFF` | No file I/O in audio path. Lower CPU, no diag.log writes |
| **Debug** | `-DPI_AUDIO_DIAG_LOG=ON` | Writes diagnostic traces to `/run/pi-audio/diag.log`. Useful for troubleshooting test mode, gain changes, and config reloads |

When enabled, the following components log to `diag.log`:
- DSP core: per-block peak levels during test mode
- Gain stage: gain change events with ramp state
- Engine control: command drain (test mode, gain, mute)
- Main binary: test control activation/stop, config reload volume/mute

**Note:** Diagnostic logging adds file I/O in the real-time audio path. Keep it disabled for production use to minimize CPU load and avoid blocking the RT thread on disk writes.

### Libraries

| Target | Description |
|---|---|
| `dsp_core` | Portable processing library (no OS deps, no ALSA) |
| `hal_sim` | Simulated HAL — sine/noise generator, drift injection, xrun simulation |
| `hal_alsa` | ALSA HAL — non-blocking independent streams, avail_update pacing, S32_LE format (Linux only) |
| `msg_lib` | Lock-free SPSC queue, SHM ring buffers, command/telemetry structs |
| `control_plane` | ConfigLoader (JSON → binary params), StateWriter, MonitoringReader |

### Binary

`pi-audio-engine` — links `hal_sim` + `hal_alsa` on Linux. Selects backend via `--sim` flag or `PI_AUDIO_SIM_HAL` env var.

```bash
./build/pi-audio-engine --config /etc/pi-audio/config/current.json
./build/pi-audio-engine --sim --rate 48000 --buffer 128
```

### DSP Core Compile Flags

The core is compiled with strict real-time constraints: `-fno-exceptions -fno-rtti -ffast-math -Wdouble-promotion`. No heap allocation after `init()`, no std::string or std::vector in the processing path, single-precision float only.

### Compiler Notes

**g++ 15**: Stricter about `void*` to `snd_pcm_t*` conversions in ALSA HAL. The `hal_alsa` target uses `-fpermissive` flag to allow legacy cast patterns. Header types use `snd_pcm_t*` directly (not `void*`) for type safety.

## Deploy

```bash
# Build (interactive prompt for diagnostic logging)
./build.sh

# Deploy binary and restart service
sudo deploy-pi-audio  # copies build/pi-audio-engine to /usr/local/bin/, restarts systemd service
```

### Systemd Service

Managed by `pi-audio-engine.service` with drop-in override at `/etc/systemd/system/pi-audio-engine.service.d/override.conf`:

| Setting | Value | Purpose |
|---|---|---|
| `RuntimeDirectory` | `pi-audio` | Creates `/run/pi-audio/` for IPC (engine-state.json, test-command) |
| `RuntimeDirectoryMode` | `0777` | World-writable so web server (www-data) can write speaker test commands |
| `LimitMEMLOCK` | `67108864` (64MB) | Allows mlockall() for RT thread without EAGAIN on pthread_create |
| `RestartSec` | `10s` | Delay between restart attempts to avoid rapid crash loops |

### Buffer Size Recommendation

Use `--buffer 512` for ASYNC USB audio interfaces (UR12, etc.). The larger buffer provides USB packet timing slack and reduces XRUN frequency. Default is 128 frames if not specified.

## Architecture

```
pi-audio-dsp-rewrite/
├── src/
│   ├── bin/
│   │   └── pi-audio-engine.cpp       # CLI: constructs Engine, selects HAL, starts threads
│   ├── core/                           # Portable DSP core (no OS deps)
│   │   ├── include/dsp_core/
│   │   │   ├── block_info.h           # Per-block context (sample index, host time, frames, clock state)
│   │   │   ├── channels.h             # Channel routing abstraction
│   │   │   ├── dsp_core.h             # Main DSP engine: init(), process(), getTelemetry()
│   │   │   ├── drift_controller.h     # PI clock-drift controller (Kp/Ki, slew-limited)
│   │   │   ├── fractional_resampler.h # Polyphase FIR resampler (32 taps × 64 phases)
│   │   │   ├── portability.h          # RT constraints + timing ring buffer
│   │   │   └── stage.h                # Stage interface + double-buffered StageSet swap
│   │   └── src/
│   │       ├── dsp_core.cpp           # Core process() implementation
│   │       ├── drift_controller.cpp   # PI controller logic with anti-windup
│   │       ├── fractional_resampler.cpp # Kaiser-windowed sinc coefficients, history ring
│   │       ├── routing.cpp            # Channel mixing utility
│   │       └── stages/
│   │           ├── gain_stage.{h,cpp} # Per-channel gain with 64-sample ramp (click-free)
│   │           └── null_stage.h       # No-op stage for cycle budget measurement
│   ├── hal/                            # Hardware Abstraction Layer
│   │   ├── include/hal/
│   │   │   ├── alsa_hal.h             # ALSA-based HAL (Linux)
│   │   │   ├── engine.h               # Engine orchestrator: onBlock(), command drain, telemetry push
│   │   │   ├── hal.h                  # AudioHal abstract base: open(), run(), close()
│   │   │   └── sim_hal.h              # Simulated HAL (no hardware)
│   │   └── src/
│   │       ├── alsa_hal.cpp           # Non-blocking independent streams, avail_update pacing, S32_LE format, snd_pcm_start recovery
│   │       ├── engine.cpp             # onBlock() callback, drainCommands(), pushTelemetry()
│   │       └── sim_hal.cpp            # Sine/noise generator + drift simulation + clock transitions
│   ├── msg/                            # Message passing / IPC
│   │   ├── include/msg/
│   │   │   ├── command.h              # Binary command structs (gain, mute, params, test mode)
│   │   │   ├── params.h               # Versioned parameter blocks with stage param types
│   │   │   ├── queue.h                # Lock-free SPSC ring queue template (cache-line aligned)
│   │   │   ├── shm_ring.h             # Shared memory ring buffer + lifecycle management
│   │   │   └── telemetry.h            # Telemetry status block struct
│   │   └── src/
│   │       ├── queue.cpp              # Compile unit stub (header-only template)
│   │       └── shm_ring.cpp           # POSIX shm create/open/close/reset with generation tracking
│   └── control/                        # Control plane
│       ├── include/control/
│       │   ├── config_loader.h        # JSON-to-binary parameter conversion + biquad precompute
│       │   ├── monitoring_reader.h    # External reader contract (header-only, no network syscalls)
│       │   └── state_writer.h         # Periodic engine-state.json writer
│       └── src/
│           ├── config_loader.cpp      # Full JSON parser + biquad coefficient precompute
│           └── state_writer.cpp       # Background thread writing telemetry JSON
├── tests/                              # 12 test binaries (custom ASSERT, CTest integration)
├── CMakeLists.txt
└── run.sh                              # Interactive build/test/run script
```

## HAL Implementations

### SimHal

Simulated audio backend with zero hardware dependencies. Compiles on any platform.

- **Input sources**: sine wave, white noise, or file-based
- **Clock drift injection**: configurable PPM offset (e.g. +30 ppm) through `DriftController` + `FractionalResampler` pipeline
- **Simulated clock state transitions**: Locked → Holdover → Unlocked events for testing mute/fade behavior
- **Xrun injection**: methods to trigger underrun/overrun and verify fade recovery over 8 blocks

Use in CI to unit-test the DSP core, drift controller, xrun fades, and lock-loss behavior without hardware.

### AlsaHal

Full ALSA implementation with real-time optimizations:

- **Non-blocking independent streams** (`SND_PCM_NONBLOCK`) — capture and playback operate independently without `snd_pcm_link()`. Required for ASYNC USB audio interfaces (e.g., Steinberg UR12) where linked streams cause continuous XRUN on capture because the USB packet interval (125μs) doesn't align with buffer read timing.
- **S32_LE interleaved format** — hardware-native integer format for UR12 and similar devices. Float↔int32 conversion done in DSP loop with clamping to [-1.0, 1.0] range before writei.
- **Availability-based pacing**: `snd_pcm_avail_update()` checks buffer availability before each read/write instead of blocking I/O. Capture waits for sufficient data; playback skips output when buffer is full (prevents underrun on ASYNC USB endpoints).
- **Stream activation**: capture warms up with retrying blocking reads (~100ms USB device startup delay); playback activated via silence write during initialization.
- **RT scheduling**: `SCHED_FIFO` at priority 75 with CPU affinity pinning (`setCoreMask()`)
- **Memory locking** via `mlockall(MCL_CURRENT)` — locks existing pages only (NOT `MCL_FUTURE` which exhausts the locked memory limit and causes EAGAIN on `pthread_create`)
- **Pre-allocated I/O buffers** — no heap allocation on RT path
- **Xrun recovery**: uses `snd_pcm_prepare()` + `snd_pcm_start()` sequence for XRUN/DRAINING/PREPARED states (not `snd_pcm_recover()` which returns EBUSY on ASYNC USB devices). Bare `snd_pcm_start()` alone doesn't fully reinitialize the stream after persistent XRUN, causing the engine to loop without producing telemetry. Capture recovery falls through to process silence (keeping telemetry flowing) instead of continuing the loop. Fade level (`m_fadeLevel`) is NOT reset during xrun recovery — it stays at 1.0f and only ramps down when explicitly muted. This fixes a critical bug where ~374 xruns/s kept fade near zero permanently, causing complete muting.
- **Timer pacing safety-net**: auto-detected via `snd_pcm_type() == SND_PCM_TYPE_NULL`; switches to timer pacing if `writei` returns in under half a period (catches `plug:`-wrapped null devices)

#### ASYNC USB Device Notes

USB audio interfaces with ASYNC endpoints (no implicit feedback) require special handling:
- **No stream linking**: `snd_pcm_link()` before hw_params fails with EBADF on plughw; after hw_params succeeds but causes continuous capture XRUN
- **Larger buffers**: Use `--buffer 512` or higher for USB packet timing slack (default 128 is too small for ASYNC devices)
- **Non-blocking I/O**: Blocking readi/writei can hang indefinitely on ASYNC endpoints when stream enters DRAINING state
- **snd_pcm_prepare() + snd_pcm_start() recovery**: `snd_pcm_recover()` returns EBUSY (-16) and cannot handle DRAINING state; use explicit prepare+start sequence instead of bare start

## DSP Core

Portable processing engine with zero OS dependencies. Entry point: `process(float* const channels[], uint32_t numChannels, uint32_t frames)`.

- **`init()`** — Allocates internal buffers; after this, no heap usage
- **`process()`** — Main entry point with planar float arrays (`channels[ch][frame]`)
  - Copies input to output as base signal path
  - Runs stage chains (tweeter chain + woofer chain) via StageSet
  - Reacts to clock state changes (mutes on Unlocked)
  - Updates telemetry cache with peak meters and latency
- **`getTelemetry()`** — Returns cached `TelemetryBlock` (xruns, clock state, levels, latency, sample index)
- **`totalLatencySamples()`** — Sum of declared stage latencies

### Stage Interface

No `std::function`, no heap allocation. Fixed maximum block size.

```cpp
struct Stage {
    virtual void process(float* const channels[], uint32_t numChannels,
                         uint32_t frames, const BlockInfo& info) = 0;
    virtual uint32_t latencySamples() const = 0;
};
```

**StageSet**: Double-buffered stage swap. `m_active[]` holds currently running stages, `m_pending[]` holds stages queued for next block boundary. `swap()` atomically replaces active with pending at block start. Maximum 8 stages per chain.

### Implemented Stages

| Stage | Description |
|---|---|
| **GainStage** | Per-channel gain (0.0–1.0 linear) with 64-sample ramp to avoid clicks; supports mute |
| **NullStage** | No-op stage for cycle budget measurement and latency baseline |

## Command / Telemetry Interface

### Lock-Free SPSC Queue

Template class with cache-line aligned head/tail pointers (64-byte alignment). `push()` returns false if full. `pop()` drains up to max items, returns count. `pending()` provides non-blocking peek at item count.

### Command Types

Fixed-size packed structs with versioned header (type, sequence number, optional apply-at-sample N):

| Type | ID | Struct | Purpose |
|---|---|---|---|
| `CMD_SET_GAIN` | 1 | `CmdSetGain` | Set gain 0.0–1.0 linear |
| `CMD_SET_MUTE` | 2 | `CmdSetMute` | Mute/unmute flag |
| `CMD_LOAD_PARAMS` | 3 | `CmdLoadParams` | Binary parameter block (up to 2KB payload) |
| `CMD_SCHEDULE` | 4 | — | Scheduled command at sample N |
| `CMD_ENTER_TEST_MODE` | 5 | `CmdTestMode` | Enter diagnostic mode |
| `CMD_LEAVE_TEST_MODE` | 6 | `CmdTestMode` | Leave diagnostic mode |
| `CMD_SET_TEST_MODE` | 7 | `CmdSetTestMode` | Atomic test mode update |

### Telemetry Block

Pushed from core to control plane per block:

- Version, sequence number, xrun counters (underrun/overrun)
- Clock state (Internal / Locked / Holdover / Unlocked)
- Output level dB (2 channels), input level dB (8 channels)
- Latency in frames, sample index, core version

### Binary Parameter Blocks

Versioned parameter format (`ParamBlockHeader` + `StageParamEntry[]`). Stage param types:

| Type | ID | Description |
|---|---|---|
| `STAGE_PARAM_GAIN` | 1 | Gain value |
| `STAGE_PARAM_IIR_COEF` | 2 | Biquad coefficients (b0/b1/b2/a1/a2) |
| `STAGE_PARAM_CROSSOVER` | 3 | Crossover filter coefficients |
| `STAGE_PARAM_DELAY` | 4 | Delay line configuration |
| `STAGE_PARAM_TRIM` | 5 | Level adjustment in dB |

## Crossover Processing

Crossover filters are implemented as DSP stages using precomputed biquad coefficients. The workflow:

1. **Config**: Web UI sets crossover parameters per output (frequency, topology, slope) in JSON config
2. **Precompute**: `ConfigLoader::precomputeBiquadCoeffs()` converts frequency + Q + gain to `b0/b1/b2/a1/a2` using Audio EQ Cookbook formulas. Runs on Linux control plane at normal priority — no libm calls in RT path.
3. **Send**: Coefficients packaged as `STAGE_PARAM_CROSSOVER` binary parameter block, pushed via command queue
4. **Apply**: Core applies coefficients at next block boundary through StageSet swap — zero glitches

Supported topologies map to standard biquad designs:
- **Linkwitz-Riley**: Cascaded Butterworth sections, in-phase summation at crossover point (flat combined magnitude)
- **Butterworth**: Maximally-flat passband, steeper transition than LR for same order
- **Bessel**: Linear phase (preserves transient shape), gentler roll-off

Slope determines filter order: 12 dB/oct = 2nd order, 18 dB/oct = 3rd, 24 dB/oct = 4th, etc. Higher slopes use cascaded biquad stages within the StageSet.

## Drift Compensation

For sources on a different clock (shm PGM in VM, external console with unrelated clock):

- **DriftController**: PI controller estimates source/destination clock ratio from ring-buffer fill level. Kp=0.01, Ki=0.0001 (slow convergence ~5s for +30ppm step). Slew-limited to 50 ppm/block max change with anti-windup clamping.
- **FractionalResampler**: Polyphase FIR fractional-delay filter. Kaiser-windowed sinc, 32 taps × 64 phases (precomputed coefficients). History ring buffer of 48 samples. Behaves as fractional-delay filter for near-unity clock ratios. No heap allocation after init, no libm at runtime.

## Shared Memory IPC

POSIX shared memory ring buffers (`shm_open` + `mmap`) with lifecycle management:

- **States**: CREATING → READY → STALE (atomic transitions)
- **Header** (64 bytes): Magic "PIAI", version, write position, sample rate, channels, sequence, generation counter, XOR checksum
- **Data region**: 4 MB per ring
- **ShmRingLifecycle**: Creator/reader ownership model with generation tracking and header validation

## Control Plane

### ConfigLoader

JSON-to-binary parameter conversion. Custom minimal JSON parser (no library dependency). Full schema: audio format, input/output gains, filter bands (biquad), drivers (delay/trim/phase), limiter settings, measurement points. Precomputes biquad coefficients using Audio EQ Cookbook formulas. Validates all parameters with structured error codes (`CONFIG_ERR_SAMPLE_RATE`, `CONFIG_ERR_FREQ_NYQUIST`, etc.).

**Case-insensitive field matching**: accepts both `"BufferSize"` (web UI schema) and `"bufferSize"` (legacy format) to prevent silent buffer size mismatches between config files.

### StateWriter

Background thread writing `engine-state.json` at configurable interval (default 1000ms). Internal ring buffer for telemetry ingestion. Outputs JSON with version, seq, xrun counts, clock state, levels, latency, sample index.

### MonitoringReader

Header-only contract for external processes: `readShmChannel()` reads mono float32 from SHM ring by name; `readStateFile()` parses engine-state.json. No network syscalls in the audio process.

## MCU / DSP Migration Path

The architecture separates portable processing (DSP core) from hardware access (HAL) and control (config/networking), enabling migration from ALSA on Raspberry Pi to a dedicated DSP/MCU chip with minimal code changes.

### Current vs Target

| Aspect | Current (Pi + ALSA) | Target (Dedicated DSP/MCU) |
|---|---|---|
| Audio backend | AlsaHal: ALSA PCM streams, `snd_pcm_writei` pacing | DMA/ISR HAL: I2S/SAI DMA interrupt, hardware period callback |
| Clock source | Converter clock via device pacing | Internal oscillator or recovered from AES input |
| Async inputs | Software resampler (PI controller + polyphase FIR) | Hardware ASRC on the chip |
| Command transport | In-process lock-free SPSC queue | Serialized over SPI, UART, USB, RPMsg, shared memory, or Ethernet with checksum |
| Control plane | Co-located: JSON parsing, coefficient precompute, state writer in same process | Linux side (Pi): all control logic. DSP receives only binary parameter blocks |
| Monitoring | SHM rings + web server reads directly | Audio data returned over link to Linux side; monitoring runs on Pi |

### What Survives

- **DSP core**: `process()` entry point, stage interface (`Stage` / `StageSet`), drift compensation algorithm — unchanged
- **Command/telemetry structs**: Fixed-size, versioned headers — same layout, different transport
- **Binary parameter blocks**: `ParamBlockHeader`, `StageParamEntry` with `STAGE_PARAM_CROSSOVER` etc. — identical format across link
- **Portability rules**: No exceptions, no RTTI, no heap after init, float32 only — enforced by compile flags

### What Changes

- **HAL implementation**: New DMA/ISR backend replaces AlsaHal. Same `AudioHal` interface (`open()` / `run()` / `close()`) but different internals: ISR-driven block processing instead of RT thread with `clock_nanosleep`.
- **Command transport**: In-process queue becomes serialized link protocol. Same structs, added checksum and framing for the wire.
- **Control plane location**: Moves entirely to Linux side in hybrid mode. Pi parses JSON, precomputes coefficients, manages presets — sends only binary blocks to DSP.

### Porting Steps

1. **Validate core on target silicon** — Run DSP core with a trivial HAL on an eval board. The Pi validates algorithms and protocol but not ISR latency, DMA jitter, cache effects, or real cycle counts.
2. **Implement target HAL** — Provide `open()` / `run()` / `close()` for the I2S/SAI DMA backend. Register ISR callback that calls `Engine::onBlock()` with planar float buffers and `BlockInfo`. Handle hardware ASRC clock state reporting.
3. **Wire command transport** — Serialize command queue structs over chosen link (SPI, UART, etc.). Add checksum. Linux side sends binary parameter blocks; target applies at block boundaries via StageSet swap.
4. **Validate cycle budget** — Log per-stage timing against the target's budget. A Pi 4 hides inefficiencies that a Cortex-M7 or dedicated DSP won't forgive. Confirm hardware FPU availability for float32 operations.

## Test Suite

12 test binaries using custom ASSERT macro (no external framework), CTest integration:

| Test | Scope |
|---|---|
| `test_dsp_core` | Core init, process pass-through, telemetry, latency sum |
| `test_command_queue` | SPSC queue push/pop, full detection, concurrent stress (10K items) |
| `test_sim_hal` | SimHal open/run/close, basic operation |
| `test_sim_hal_extended` | Noise input, clock transitions, multiple PPM offsets |
| `test_drift_compensation` | PI controller convergence, THD+N measurement via DFT |
| `test_clock_state` | Clock state reaction (mute on Unlocked) |
| `test_xrun_recovery_sim` | Xrun injection + fade recovery in SimHal |
| `test_xrun_recovery_alsa` | Xrun recovery with real ALSA (Linux-only) |
| `test_rt_thread_hygiene` | RT thread constraints verification (no file I/O, no network, no allocation) |
| `test_monitoring_networking` | StateWriter, telemetry queue plumbing |
| `test_shm_ipc_config` | SHM lifecycle, header validation, config loader error codes |
| `test_measurement_timing` | Per-stage timing measurement |
| `test_architecture_separation` | HAL/DSP/control separation verification |
| `test_dsp_quality` | DSP output quality metrics (denormal handling, click-free ramps) |
