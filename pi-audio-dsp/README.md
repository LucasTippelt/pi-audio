# Pi Audio DSP Engine

Real-time audio engine for a single-speaker system with 2 outputs (tweeter + woofer), each with independent DSP chains. Stereo PGM input: left channel routes through DSP to both outputs, right channel is a direct output for a future linked speaker. Configurable 2-6 measurement microphones. Built for Raspberry Pi with ALSA support. Uses POSIX shared memory (`shm_open` + `mmap`) for zero-copy inter-process audio ring buffers. Part of the [pi-audio](..) monorepo.

## Requirements

- **CMake** 3.10+
- **GCC** (C++17 support)
- **libasound2-dev** (ALSA development headers)
- **pkg-config**
- `sudo apt install build-essential`

No external dependencies — bundled header-only JSON parser in `src/include/utils/json.hpp`.

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Deploy

Full installation is documented in the [root README](../README.md#quick-install).

### Production

```bash
cd pi-audio-dsp
sudo ./deploy.sh
```

### VM / Development

```bash
mkdir build && cd build && cmake .. && make -j$(nproc)
cd .. && sudo bash redeploy.sh
```

## Architecture

```
pi-audio-dsp/
├── src/
│   ├── bin/              # Executable entry points
│   │   ├── pi-audio-engine.cpp   # Main engine (ALSA playback, DSP, monitoring)
│   │   ├── pi-audio-apply.cpp    # Config apply adapter
│   │   ├── pi-audio-status.cpp   # Status probe adapter
│   │   ├── pi-audio-test.cpp     # Speaker test adapter
│   │   └── pi-audio-measure.cpp  # Room measurement adapter (stub)
│   ├── lib/
│   │   ├── core/         # Core engine modules
│   │   │   ├── engine.cpp        # AudioEngine init/start/stop/config
│   │   │   ├── engine_threads.cpp # Playback, capture, reader thread functions
│   │   │   ├── dsp.cpp           # DSP processing (volume, mute, stages)
│   │   │   ├── ring_buffer_io.cpp # Shared memory ring buffer I/O helpers
│   │   │   ├── config.cpp        # ConfigManager (JSON loading)
│   │   │   ├── logger.cpp        # Logger with rotation (1MB, 3 backups)
│   │   │   ├── monitor.cpp       # Per-channel monitor ring buffer writer
    │   │   │   ├── tcp_relay.cpp     # TCP relay client (pushes to WS server, legacy)
    │   │   │   ├── http_stream.cpp   # Embedded HTTP stream server (port 8767)
    │   │   │   └── websocket_server.cpp # Embedded WebSocket server (port 8765, frame-based)
│   │   └── adapters/     # Web interface adapters
│   │       ├── apply.cpp         # Validates config, writes reload trigger, polls confirmation
│   │       ├── status.cpp        # Reads engine state file
│   │       ├── test.cpp          # Writes whitenoise to shared memory ring buffers
│   │       └── measure.cpp       # Room measurement (stub)
│   └── include/
│       ├── core/         # Headers (engine, config, logger, monitor, shm_ring, tcp_relay, http_stream)
│       └── utils/        # JSON parser
├── overrides/
│   └── 10-vm-user.conf   # VM systemd override
├── CMakeLists.txt
├── deploy.sh             # Production deploy
├── deploy-vm.sh          # VM deploy
├── redeploy.sh           # Quick redeploy
├── pi-audio-engine.service
└── scripts/
    └── gen-version.sh    # Version generation (git + timestamp → C++ header + JSON)
```

## Binaries

| Binary | Purpose |
|---|---|
| `pi-audio-engine` | Main audio engine — captures PGM stereo, reads mic inputs, processes per-output DSP, writes 2-ch ALSA + monitor buffers + HTTP stream + TCP relay |
| `pi-audio-apply` | Apply config — validates, writes reload trigger, polls engine-state.json (up to 5s) |
| `pi-audio-status` | Status probe — reads engine state, reports connection status |
| `pi-audio-measure` | Room measurement (not yet implemented) |
| `pi-audio-test` | Speaker test — writes 3s whitenoise to PGM or mic ring buffers for requested driver |

### Supported Test Drivers

| Driver | Ring Buffer | Description |
|---|---|---|
| `Tweeter` | `shm://pi-audio-test-tweeter` | Per-channel test injection, routes through tweeter DSP only |
| `Woofer` | `shm://pi-audio-test-woofer` | Per-channel test injection, routes through woofer DSP only |
| `PGM_L` | `shm://pi-audio-pgm` | Stereo PGM, left channel only, monitoring only (no speakers) |
| `PGM_R` | `shm://pi-audio-pgm` | Stereo PGM, right channel only, monitoring only (no speakers) |
| `Mic_1`..`Mic_6` | `shm://pi-audio-mic-{1..6}` | Individual mic channels |

Test mode is signalled via `/dev/shm/pi-audio-test-mode` control file. The engine suspends PGM routing during tweeter/woofer tests, and silences physical speakers during PGM tests. The test tool writes all frames in a single call, signals the engine, then waits for consumption before exiting.

## Engine Architecture

### Audio Signal Flow

**Normal mode:**
```
PGM stereo input (ALSA capture or shared memory ring buffer)
    │
    ├──► PGM Left ──► DSP chain ──┬──► Tweeter (output 0)
    │                              └──► Woofer  (output 1)
    │
    └──► PGM Right ──► direct output (linked speaker, future onboard DSP)

Mic inputs (2-6 configurable):
    shm://pi-audio-mic-{1..6} ──► monitor + relay + HTTP stream
```

**Test mode** (engine detects from `/dev/shm/pi-audio-test-mode`):
```
Tweeter test:  shm://pi-audio-test-tweeter ──► tweeter DSP ──► tweeter (woofer silent, PGM suspended)
Woofer test:   shm://pi-audio-test-woofer   ──► woofer DSP  ──► woofer  (tweeter silent, PGM suspended)
PGM_L test:    shm://pi-audio-pgm           ──► monitoring only (no physical speakers)
```

Monitoring always shows actual DSP output for the selected channel.

### Threads

| Thread | Priority | Purpose |
|---|---|---|
| **Playback** | SCHED_FIFO (max) | Real-time audio loop: sleep → read rings → DSP → ALSA write → push relay/monitor/HTTP |
| **Capture** | Normal | ALSA capture → write shared memory ring buffer (background) |
| **PGM Reader** | Normal | Read shared memory → push to in-memory rings (background) |
| **Mic Readers** | Normal | Read shared memory → push to in-memory rings (background) |
| **Relay** | Normal | Read from relay rings → push to TcpRelay (background) |
| **Monitor** | Normal | Read from monitor rings → write shared memory ring buffers (background) |
| **HTTP Accept** | Normal | Accept HTTP connections, spawn client handlers (background) |
| **WS Accept** | Normal | Accept WebSocket connections, handle handshake + frame I/O (background) |

**Key:** The playback thread has **zero file I/O**. All shared memory operations happen in background threads.

### Playback Thread Loop

1. **Sleep** using absolute timing (`clock_nanosleep` with `CLOCK_MONOTONIC` + `TIMER_ABSTIME`)
2. **Check test mode** from `/dev/shm/pi-audio-test-mode` control file
3. **Pop** PGM left/right from in-memory SPSC rings (suspended during tweeter/woofer tests)
4. **Pop** mic data from in-memory SPSC rings
5. **Read test injection** from per-channel shared memory buffers
6. **Route** based on test mode:
   - Normal: PGM L → tweeterBuf + wooferBuf
   - Tweeter test: test injection → tweeterBuf, wooferBuf = silence
   - Woofer test: test injection → wooferBuf, tweeterBuf = silence
   - PGM_L test: both buffers = silence (PGM data available for monitoring)
7. **Process** per-output DSP (volume/mute + extensible stages, double-buffered stage swap)
8. **Interleave** tweeter (ch0) + woofer (ch1) for ALSA
9. **Write** to ALSA PCM (`snd_pcm_writei`)
10. **Push** to relay rings (actual DSP output for tweeter/woofer), monitor rings, HTTP stream server, and WebSocket server

### In-Memory Rings

- **SPSCRing&lt;4096&gt;** — Per-channel single-producer/single-consumer float ring for PGM L/R and mic data
- **RelayChannel&lt;128 slots&gt;** — Lock-free double-buffered ring (32 slots × 512 samples) for TCP relay
- **RelayChannel&lt;128 slots&gt;** — Same structure for monitor file writer

### Real-Time Pacing

The playback thread uses per-cycle absolute timing to self-correct scheduler jitter. Required because null ALSA devices (VMs) return instantly from `snd_pcm_writei()`.

### Shared Memory Ring Buffers

POSIX shared memory objects (`shm_open` + `mmap`), visible at `/dev/shm/`:

| Object | Format | Writer | Reader |
|---|---|---|---|
| `pi-audio-pgm` | Stereo interleaved float32, 4MB | ALSA capture / external | PGM reader thread |
| `pi-audio-mic-{1..6}` | Mono float32, 4MB each | External / test adapter | Mic reader threads |
| `pi-audio-monitor-{channel}` | Mono float32, 4MB each | Monitor thread | Web UI / debug page |
| `pi-audio-test-tweeter` | Mono float32, 4MB, `PIIJ` header | Test adapter | Playback thread |
| `pi-audio-test-woofer` | Mono float32, 4MB, `PIIJ` header | Test adapter | Playback thread |
| `pi-audio-test-mode` | Text ("tweeter", "woofer", "pgm-l") | Test adapter | Playback thread |

On startup, the engine creates all shared memory objects and syncs read positions. External writers (test adapter, external sources) access the same objects via `shm_open()`.

### Monitor Ring Buffers

Per-channel 4MB mono float32 ring buffers with 64-byte `PIAM` header, stored in POSIX shared memory (`/dev/shm/pi-audio-monitor-{channel}`). Channels: `pgm-l`, `pgm-r`, `tweeter`, `woofer`, `mic-1`..`mic-6`.

### HTTP Stream Server (Recommended Monitoring)

Embedded HTTP server on port 8767. Serves real-time audio streams directly to browsers:

- **`GET /stream?ch=&lt;channel&gt;`** — Raw float32LE mono, HTTP chunked transfer encoding
- **`GET /status`** — JSON: sampleRate, BufferSize, volume, muted
- **`GET /`** — Channel list (JSON array)

The playback thread pushes samples directly to connected HTTP clients. No intermediate relay process. Uses `TCP_NODELAY` and 128KB send buffer for low-latency delivery.

### WebSocket Server (Frame-Based Monitoring)

Embedded WebSocket server on port 8765. Provides application-level framing: each 512-frame buffer arrives as a distinct binary frame, bypassing browser TCP receive batching for stable `onmessage` callbacks every ~10.7ms.

- **Subscribe:** Client sends text frame `{"channel":"tweeter"}`
- **Ack:** Server replies `{"ok":true,"sampleRate":48000,"bufferSize":512}`
- **Audio:** Server pushes binary frames — raw mono float32LE, 512 frames per frame (2048 bytes)
- **Unsubscribe:** Client closes connection

Uses Hybi-13 (RFC 6455) handshake with SHA-1. Server→client frames are unmasked binary. Client→server frames are masked text. Non-blocking `send()` with `MSG_DONTWAIT` from playback thread; EAGAIN drops absorbed by client-side buffer.

### TCP Relay (Legacy)

Pushes per-channel mono float32 via TCP to `ws-audio-server-node.js` (port 8766). Non-blocking push from playback thread, dedicated sendLoop thread with condition variable. Sends each chunk immediately (no accumulation). Auto-reconnects on drop.

Protocol: `[1 byte channel ID][4 bytes BE sample count][sampleCount × 4 bytes mono float32]`

Channel control: `E &lt;ch&gt;\n` (enable) / `D &lt;ch&gt;\n` (disable) from WS server.

## Configuration

Engine reads `/etc/pi-audio/config/current.json`. Adapters communicate via JSON stdin/stdout.

### Hot Reload

1. Web UI calls `PI_AUDIO_APPLY_BIN`
2. Apply adapter validates, writes `/run/pi-audio/reload`
3. Engine detects trigger (every 1s), reloads config, removes trigger
4. Apply adapter polls `engine-state.json` for confirmation (up to 5s)

Volume/mute: atomic. Sample rate/buffer size/device: reinitialize.

### Engine State File

Written to `/run/pi-audio/engine-state.json` every second:

```json
{"version":"v2.1.0.28","input":"default","output":"default","sampleRate":48000,"BufferSize":512,"outputChannels":2,"micCount":2,"volume":65,"muted":false,"pgmSuspended":false}
```

## Versioning

Format: `vX.Y.Z.UIO` (e.g. `v2.1.0.0`)

Generated by `scripts/gen-version.sh` at build time:
- **C++**: `version_generated.h` with `#define PI_AUDIO_VERSION`
- **Web**: `pi-audio-web/VERSION.json`

## Runtime Files

| Path | Purpose |
|---|---|
| `/run/pi-audio/status` | Engine running flag |
| `/run/pi-audio/engine-state.json` | Current parameters |
| `/run/pi-audio/reload` | Reload trigger |
| `/dev/shm/pi-audio-pgm` | PGM stereo input ring (4MB, shared memory) |
| `/dev/shm/pi-audio-mic-{1..6}` | Mic input rings (4MB each, shared memory) |
| `/dev/shm/pi-audio-monitor-{channel}` | Monitor rings (4MB each, shared memory) |
| `/dev/shm/pi-audio-test-tweeter` | Tweeter test injection (4MB, shared memory) |
| `/dev/shm/pi-audio-test-woofer` | Woofer test injection (4MB, shared memory) |
| `/dev/shm/pi-audio-test-mode` | Test mode control file |

## Logging

`/var/log/pi-audio/engine.log` — 1 MB max, 3 rotated backups.

## Debugging

```bash
sudo systemctl status pi-audio-engine
sudo journalctl -u pi-audio-engine -f

# Run manually
/usr/local/bin/pi-audio-engine /etc/pi-audio/config/current.json /var/log/pi-audio/engine.log

# State and tests
cat /run/pi-audio/engine-state.json
/usr/local/bin/pi-audio-status
/usr/local/bin/pi-audio-test Tweeter
/usr/local/bin/pi-audio-test Woofer
/usr/local/bin/pi-audio-test PGM_L

# If tests fail with "write failed", clean stale shared memory:
sudo rm -f /dev/shm/pi-audio-test-* /dev/shm/pi-audio-test-mode

# HTTP stream server
curl http://localhost:8767/status

# WebSocket server (port 8765)
wscat -c ws://localhost:8765

# ALSA
aplay -l && arecord -l
```

## VM Testing

Create a null ALSA device:

```bash
cat > ~/.asoundrc << 'EOF'
pcm.!default { type null }
ctl.!default { type hw card 0 }
EOF
```

Use `overrides/10-vm-user.conf` to run as non-root. The VM deploy script installs this automatically.

### VM Permissions

- `/run/pi-audio/` — engine user, group `audio`
- `/var/log/pi-audio/` — engine user, group `audio`
- Config files — `664` (not `660`)
- `www-data` in `audio` group
- Engine user in `audio` group: `sudo usermod -aG audio $USER`
