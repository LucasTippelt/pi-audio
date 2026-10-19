# Pi Audio

Real-time audio DSP engine and web control surface for a single-speaker system with tweeter + woofer outputs, stereo PGM input, and configurable measurement microphones.

## Architecture 

Three-layer split separating portable processing from hardware and control:

| Layer | Role | Constraints |
|---|---|---|
| **DSP Core** | Portable signal processing — `process(in, out, nframes)` + command/telemetry interface | No OS calls, no file I/O, no heap after init, float32 only, no exceptions/RTTI |
| **HAL** | Hardware abstraction — owns audio thread, calls `Engine::onBlock()` per block with planar float buffers and `BlockInfo` | Provides RT scheduling, xrun recovery (fade preserved during recovery), clock-state polling. Swappable per platform |
| **Control Plane** | Config loading (JSON → binary params), telemetry writer, monitoring readers | Runs at normal priority; communicates with core only through command/telemetry queues |

No HTTP, WebSocket, or TCP servers run inside the audio process. All networking is handled by the separate web server.

## Projects

| Project | Description |
|---|---|
| [`pi-audio-dsp-rewrite`](pi-audio-dsp-rewrite/) | DSP engine — portable core, HAL implementations (SimHal, AlsaHal), command/telemetry interface, control plane |
| [`pi-audio-web-rewrite`](pi-audio-web-rewrite/) | Web server — TypeScript/Express REST API + WebSocket audio streaming + vanilla JS frontend |

## Audio Data Flow

### Normal Playback

```
PGM stereo input (ALSA capture or shared memory ring buffer)
    │
    ├──► PGM Left ──► DSP chain ──┬──► Woofer  (output 0)
    │                              └──► Tweeter (output 1)
    │
    └──► PGM Right ──► direct output (linked speaker, future onboard DSP)

Mic inputs (2-6 configurable, shared memory ring buffers):
    shm://pi-audio-mic-{1..6} ──► monitor + web server broadcast
```

### Test Mode

Engine detects active tests via command queue (`CMD_SET_TEST_MODE`). During tests, real-time PGM routing is suspended.

```
Woofer test:   test injection ──► woofer DSP  ──► woofer speaker  (tweeter silent)
Tweeter test:  test injection ──► tweeter DSP ──► tweeter speaker (woofer silent)
PGM_L test:    PGM ring buffer ──► monitoring only (no physical speakers)
```

Test signals are configurable from the web UI with a duration of 1–60 seconds per test (default: 3 s). The frontend enforces a minimum button-block time of `duration + 0.5 s` to prevent overlapping test signals when the HTTP response returns before the hardware signal finishes.

## Crossover Configuration

Per-output crossover filters split the signal between woofer and tweeter. Configured in `current.json` under the `crossovers` object, one entry per driver name (e.g. `Woofer`, `Tweeter`).

| Parameter | Range | Default | Description |
|---|---|---|---|
| `frequency` | 30 – 20 000 Hz | 2500 Hz | Crossover point frequency |
| `topology` | Linkwitz-Riley, Butterworth, Bessel | Linkwitz-Riley | Filter topology |
| `slope` | 12, 18, 24, 36, 48 dB/oct | 24 | Roll-off slope per octave |

**Linkwitz-Riley** is the recommended topology for two-way systems: identical LR filters in phase at the crossover point produce a flat summed magnitude response. Butterworth gives maximally-flat passband with steeper transition. Bessel preserves transient shape (linear phase) at the cost of roll-off steepness.

Coefficients are precomputed on the Linux control plane using Audio EQ Cookbook biquad formulas, then sent to the DSP core as compact binary parameter blocks (`STAGE_PARAM_CROSSOVER`). The core never parses JSON or runs heavy math — it receives ready-to-use `b0/b1/b2/a1/a2` coefficients.

## MCU / DSP Migration Path

The architecture is designed so the DSP core can run on three classes of target platform with minimal code changes:

### Platform Classes

| Class | Audio Backend | Clock Source | Notes |
|---|---|---|---|
| **Embedded Linux SoC** (current) | ALSA PCM, PREEMPT_RT kernel | Converter clock via device pacing | `mlockall`, isolated cores, IRQ affinity |
| **Bare-metal DSP / MCU** | I2S/SAI DMA interrupt | Internal or recovered from AES input | Hardware ASRC for async inputs. No OS. |
| **Hybrid** (product target) | Dedicated chip for audio; Linux Pi or similar for network/UI | Chip-internal clock | shm rings, HTTP/WebSocket servers, JSON config all become Linux-side clients of the DSP |

### What Survives vs Changes

**Survives across all platforms:**
- DSP core `process()` entry point and stage interface (`Stage` / `StageSet`)
- Command/telemetry structs (fixed-size, versioned headers)
- Binary parameter blocks (`ParamBlockHeader`, `StageParamEntry`)
- Drift compensation (PI controller + fractional resampler) — algorithm identical, transport differs

**Changes per platform:**
- **HAL**: AlsaHal replaced by target-specific implementation. Bare-metal HAL runs in I2S/SAI DMA ISR with `open()`/`run()`/`close()` contract unchanged.
- **Transport**: In-process lock-free queue on Pi becomes serialized link (SPI, UART, USB, RPMsg/shared memory, or Ethernet) with checksum. Same command/telemetry structs.
- **Control plane**: Moves entirely to Linux side in hybrid mode. JSON parsing, coefficient precomputation, and state-file writing stay on the Pi/Linux device; only binary parameter blocks cross the link.

### Porting Steps

1. **Validate core on target silicon** — Run DSP core with a trivial HAL on an eval board. Confirms ISR latency, DMA jitter, cache effects, and real cycle counts (Pi hides inefficiencies a Cortex-M7 won't forgive).
2. **Implement target HAL** — Provide `open()`/`run()`/`close()` for the I2S/SAI DMA backend. The `Engine::onBlock()` callback contract is identical to AlsaHal.
3. **Wire command transport** — Serialize command queue structs over chosen link (SPI, UART, etc.). Add checksum. Linux side sends binary parameter blocks; target applies at block boundaries via StageSet swap.
4. **Validate cycle budget** — Log per-stage timing against the target's budget. The Pi build is a faithful stand-in for algorithm validation but not cycle-count accuracy.

## Deployment

### Systemd Services

Two services manage the system:

| Service | Description | Key Settings |
|---|---|---|
| `pi-audio-engine` | DSP engine (C++) | `RuntimeDirectory=pi-audio`, `LimitMEMLOCK=64M`, buffer 512 for USB devices |
| `pi-audio-web` | Web server (Node.js) | Runs as `www-data`, port 80, `ProtectSystem=strict` with `ReadWritePaths` |

Engine override at `/etc/systemd/system/pi-audio-engine.service.d/override.conf`:
```ini
[Service]
ExecStart=
ExecStart=/usr/local/bin/pi-audio-engine /etc/pi-audio/config/current.json /var/log/pi-audio/engine.log --rate 48000 --buffer 512 --capture-dev hw:CARD=UR12,DEV=0 --playback-dev hw:CARD=UR12,DEV=0
RestartSec=10
LimitMEMLOCK=67108864
```

### Rebuild and Deploy

```bash
# Engine (C++)
cd pi-audio-dsp-rewrite/build && cmake .. && make -j$(nproc)
sudo deploy-pi-audio  # copies binary + restarts engine service

# Web server (TypeScript)
cd pi-audio-web-rewrite && npx tsc
sudo systemctl restart pi-audio-web
```

### After Service File Changes

Any change to `.service` files or drop-in overrides requires:
```bash
sudo systemctl daemon-reload && sudo systemctl restart pi-audio-engine pi-audio-web
```
Both services must be restarted — `ReadWritePaths` sandbox mounts only take effect at service start.

## Troubleshooting

### Muted output despite engine running

If `engine-state.json` shows zero xruns but output levels are near silence (-160 dB or lower):

- **Check fade level**: The DSP core uses a fade multiplier (`m_fadeLevel`) that starts at 1.0f. If xrun recovery resets this to 0.0f and xruns occur frequently (~374/s on ASYNC USB), the output stays muted. Fix: ensure `recoverUnderrun()` does NOT reset `m_fadeLevel = 0.0f`.
- **Check ALSA device**: Use `hw:CARD=UR12,DEV=0` (native hardware) instead of `plughw:` for ASYNC USB devices. The plughw wrapper adds format conversion that conflicts with stream linking.

### Speaker tests fail with "requires a running engine"

- Ensure `/run/pi-audio/` has mode 0777 (set via `RuntimeDirectoryMode=0777` in systemd service)
- Ensure `www-data` is in the `audio` group: `sudo usermod -aG audio www-data`
- **Restart web server** after any permission or service file change so it picks up new sandbox mounts (`ReadWritePaths`)

### Engine crashes with "Resource temporarily unavailable" on thread creation

- **Cause**: `mlockall(MCL_CURRENT | MCL_FUTURE)` exhausts the locked memory limit (default 64MB). When `pthread_create` allocates thread stacks, the kernel cannot lock new pages and returns EAGAIN.
- **Fix**: Engine uses `mlockall(MCL_CURRENT)` only — locks existing pages but allows future allocations without locking. Set `LimitMEMLOCK=67108864` (64MB) in systemd service for safety margin.

### XRUN recovery loop — engine stuck, no telemetry produced

- **Cause**: Bare `snd_pcm_start()` after XRUN doesn't fully reinitialize the stream on ASYNC USB devices. The `continue` statement skips `onBlock()`, so no telemetry is ever written and `engine-state.json` stays stale.
- **Fix**: Recovery uses `snd_pcm_prepare()` + `snd_pcm_start()` sequence for proper stream reinitialization. Capture recovery falls through to process silence (keeping telemetry flowing) instead of continuing the loop.

### Engine fails to open HAL on plughw device

ASYNC USB interfaces (UR12, etc.) cannot use linked streams (`snd_pcm_link`). The engine uses non-blocking independent streams with `avail_update()` pacing. If using an older binary that still links streams, switch to `hw:` device name or rebuild current code.

## Structure

```
pi-audio/
├── pi-audio-dsp-rewrite/       # DSP engine (C++, CMake)
│   ├── src/
│   │   ├── bin/                # Main entry point
│   │   ├── core/               # Portable DSP core (no OS deps)
│   │   ├── hal/                # Hardware abstraction (currently SimHal, AlsaHal)
│   │   ├── msg/                # Command queue, telemetry, SHM rings
│   │   └── control/            # Config loader, state writer
│   └── tests/                  # 12 test binaries
├── pi-audio-web-rewrite/       # Web server (TypeScript, Node.js)
│   ├── src/
│   │   ├── http/               # REST API endpoints
│   │   ├── websocket/          # WebSocket audio streaming + SHM reader
│   │   ├── config/             # Schema, validation, presets
│   │   ├── adapters/           # External binary adapters (status, apply, test)
│   │   └── frontend/           # Vanilla JS SPA
│   └── tests/                  # Vitest test suite
├── legacy/                     # Superseded code (kept for reference)
│   ├── pi-audio-dsp-legacy/    # Old DSP engine
│   └── pi-audio-web-legacy/    # Old PHP/Apache web interface
└── VERSION                     # Shared version string
```
