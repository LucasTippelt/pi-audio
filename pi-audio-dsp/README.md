# Pi Audio DSP Engine

Real-time audio engine and DSP pipeline for Raspberry Pi with ALSA support. Part of the [pi-audio](..) monorepo.

## Requirements

- **CMake** 3.10+
- **GCC** (C++17 support)
- **libasound2-dev** (ALSA development headers)
- **pkg-config**
- sudo apt install build-essential

No external JSON library — bundled header-only implementation in `src/include/utils/json.hpp`.

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

Builds the engine, installs binaries to `/usr/local/bin/`, and configures the systemd service.

### VM / Development

```bash
# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Quick redeploy (stops engine, copies binaries, restarts)
cd .. && sudo bash redeploy.sh
```

The `redeploy.sh` script stops the engine first (to avoid "Text file busy" errors), copies binaries, reinstalls the service, fixes permissions, and restarts the engine.

## Architecture

```
pi-audio-dsp/
├── src/
│   ├── bin/              # Executable entry points
│   │   ├── pi-audio-engine.cpp   # Main engine (reads input.raw, writes ALSA + monitors)
│   │   ├── pi-audio-apply.cpp    # Config apply adapter
│   │   ├── pi-audio-status.cpp   # Status probe adapter
│   │   ├── pi-audio-test.cpp     # Speaker test adapter (writes whitenoise to input.raw)
│   │   └── pi-audio-measure.cpp  # Room measurement adapter
│   ├── lib/
│   │   ├── core/         # Engine, config manager, logger, monitor ring buffer, TCP relay
│   │   │   ├── engine.cpp        # AudioEngine class (playback thread, DSP pipeline)
│   │   │   ├── config.cpp        # ConfigManager (JSON config loading)
│   │   │   ├── logger.cpp        # Logger with rotation (1MB max, 3 backups)
│   │   │   ├── monitor.cpp       # Per-channel monitor ring buffer writer
│   │   │   └── tcp_relay.cpp     # TCP relay client (pushes stereo data to WS server)
│   │   └── adapters/     # Web interface adapters
│   │       ├── apply.cpp         # Validates config, writes reload trigger, polls confirmation
│   │       ├── status.cpp        # Reads engine state file
│   │       ├── test.cpp          # Writes whitenoise to input ring buffer
│   │       └── measure.cpp       # Room measurement (stub)
│   └── include/
│       ├── core/         # Headers (engine, config, logger, monitor, tcp_relay)
│       └── utils/        # JSON parser
├── overrides/
│   └── 10-vm-user.conf   # VM systemd override (runs as current user)
├── CMakeLists.txt
├── deploy.sh             # Production deploy script
├── deploy-vm.sh          # VM deploy script
├── redeploy.sh           # Quick redeploy (stops engine, copies binaries, restarts)
├── pi-audio-engine.service
├── pi-audio-ws-server.service  # WebSocket relay server service
└── scripts/
    └── gen-version.sh    # Version generation (git commit + timestamp → C++ header + JSON)
```

## Binaries

| Binary | Purpose |
|---|---|
| `pi-audio-engine` | Main audio engine service — captures PGM stereo, reads mic inputs, processes per-channel DSP, writes 4-ch ALSA + monitor buffers + TCP relay |
| `pi-audio-apply` | Apply configuration adapter — validates config, writes reload trigger, polls engine-state.json for confirmation (up to 5s) |
| `pi-audio-status` | Status probe adapter — reads engine state file and reports connection status |
| `pi-audio-measure` | Room measurement adapter (not yet implemented) |
| `pi-audio-test` | Speaker test adapter — writes 3 seconds of whitenoise to PGM or mic ring buffers for the requested driver |

### Supported Test Drivers

| Driver | Ring Buffer | Description |
|---|---|---|
| `PGM_L` | `pgm.raw` | Stereo PGM, left channel only |
| `PGM_R` | `pgm.raw` | Stereo PGM, right channel only |
| `Woofer_L` | `pgm.raw` | Stereo PGM (routes to left outputs) |
| `Tweeter_L` | `pgm.raw` | Stereo PGM (routes to left outputs) |
| `Woofer_R` | `pgm.raw` | Stereo PGM (routes to right outputs) |
| `Tweeter_R` | `pgm.raw` | Stereo PGM (routes to right outputs) |
| `Mic_1` | `mic-1.raw` | Left driver mic 1 |
| `Mic_2` | `mic-2.raw` | Left driver mic 2 |
| `Mic_3` | `mic-3.raw` | Right driver mic 3 |
| `Mic_4` | `mic-4.raw` | Right driver mic 4 |

## Engine Architecture

### Audio Signal Flow

```
PGM stereo input (ALSA capture or pgm.raw ring buffer)
    │
    ├─► Mono_L ──► woofer-l   (output 0, per-channel DSP)
    │              └─► tweeter-l  (output 1, per-channel DSP)
    │
    └─► Mono_R ──► woofer-r   (output 2, per-channel DSP)
                   └─► tweeter-r  (output 3, per-channel DSP)

Mic inputs (measurement only, ring buffers):
    mic-1.raw ──► monitor + TCP relay
    mic-2.raw ──► monitor + TCP relay
    mic-3.raw ──► monitor + TCP relay
    mic-4.raw ──► monitor + TCP relay

PGM is suspended (replaced with silence) when a test or measurement is running.
```

### Capture Thread

When an ALSA capture device is configured, a dedicated capture thread reads stereo PGM from the device and writes it to the `pgm.raw` ring buffer. If no capture device is available, PGM data must be written to `pgm.raw` by an external process.

### Playback Thread

The engine runs a playback thread that loops at real-time pace:

1. **Sleep** using absolute timing (`clock_nanosleep` with `CLOCK_MONOTONIC` + `TIMER_ABSTIME`) to self-correct scheduler jitter
2. **Read** stereo float32 from `pgm.raw` ring buffer (skip if PGM suspended)
3. **Split** stereo into Mono_L and Mono_R
4. **Read** mono float32 from `mic-1..4.raw` ring buffers
5. **Route** Mono_L to outputs 0+1, Mono_R to outputs 2+3
6. **Process** per-channel DSP pipeline (volume/mute + extensible stages)
7. **Write** interleaved 4-channel float32 to ALSA PCM device
8. **Write** per-channel monitor buffers (`monitor-{channel}.raw`)
9. **Push** per-channel mono float32 to TcpRelay → TCP 127.0.0.1:8766 (disabled channels skipped)

### Real-Time Pacing

Both threads use per-cycle sleep pacing to maintain real-time speed. This is required because null ALSA devices (used in VMs) return instantly from `snd_pcm_writei()`, which would cause the loop to run at CPU speed instead of audio rate.

### Ring Buffers

| File | Format | Writer | Reader |
|---|---|---|---|
| `pgm.raw` | Stereo interleaved float32, 4MB | ALSA capture / external | Engine playback |
| `mic-{1..4}.raw` | Mono float32, 4MB each | External / test adapter | Engine playback |
| `monitor-{channel}.raw` | Mono float32, 4MB each | Engine | Web UI / WS server |

On startup, the engine creates all ring buffer files and syncs read positions to avoid consuming stale data.

### Monitor Ring Buffers

The engine creates per-channel monitor buffers on startup. Each is a 4MB mono float32 ring buffer with a 64-byte header. Channels: `pgm-l`, `pgm-r`, `woofer-l`, `tweeter-l`, `woofer-r`, `tweeter-r`, `mic-1`..`mic-4`.

### TCP Relay (Real-Time Monitoring)

The engine pushes per-channel mono float32 data via TCP to `ws-audio-server-node.js` (port 8766). The TcpRelay module:

- Buffers data from the playback thread (non-blocking push, never stalls audio processing)
- Sends batches from a dedicated sendLoop thread using a condition variable (woken on new data)
- Merged single-packet send replaces 3 separate `send()` calls per cycle
- Protocol: `[1 byte channel ID][4 bytes BE sample count][sampleCount × 4 bytes mono float32]`
- Channel enable/disable control: WS server sends `E <ch>\n` / `D <ch>\n` over TCP to enable/disable channels
- Only enabled channels are swapped in sendLoop and pushed in pushChannel
- Auto-reconnects if the TCP connection drops

The Node.js relay server broadcasts to WebSocket clients per channel (port 8765).

## Configuration

Engine reads from `/etc/pi-audio/config/current.json`. Adapters communicate via JSON stdin/stdout.

### Hot Reload (Trigger File)

The engine does not poll the config file. Instead, the apply adapter signals the engine to reload by writing a trigger file:

1. Web UI calls `PI_AUDIO_APPLY_BIN` with new config
2. Apply adapter validates config, writes `/run/pi-audio/reload`
3. Engine detects trigger file in its main loop (checked every second)
4. Engine reloads config from `current.json`, applies changes, removes trigger
5. Apply adapter polls `/run/pi-audio/engine-state.json` for up to 5 seconds, confirming the engine reports the new sampleRate and BufferSize

Volume and mute changes are applied atomically without restart. Sample rate, buffer size, or device changes require the engine to stop and reinitialize.

### Engine State File

The engine writes its current parameters to `/run/pi-audio/engine-state.json` every second:

```json
{"version":"v1.2.3.0","input":"default","output":"default","sampleRate":48000,"BufferSize":512,"outputChannels":4,"volume":65,"muted":false,"pgmSuspended":false}
```

The status adapter reads this file and merges it with the connection status. The web UI's debug page compares this state against `current.json` to verify that apply operations took effect.

## Versioning

Version format: `vX.Y.Z.UIO` (e.g. `v1.2.3.0`)

| Component | Meaning |
|---|---|
| `X` | Major release (main branch) |
| `Y` | Staging release |
| `Z` | Local build |
| `UIO` | Debug iteration |

Defined in `/var/www/pi-audio/VERSION` (line 1 = `X.Y.Z`, line 2 = `UIO`, optional).

Generated at build time by `scripts/gen-version.sh`:
- **C++ engine**: `version_generated.h` with `#define PI_AUDIO_VERSION "v1.2.3.0"`
- **Web/WS server**: `pi-audio-web/VERSION.json` with full metadata

The engine logs its version on startup and includes it in `engine-state.json`. The debug page shows all component versions and flags mismatches.

## Runtime Files

| Path | Purpose |
|---|---|
| `/run/pi-audio/status` | Engine running flag (created on start, removed on stop) |
| `/run/pi-audio/engine-state.json` | Current engine parameters (written every second) |
| `/run/pi-audio/reload` | Reload trigger (written by apply adapter, consumed by engine) |
| `/run/pi-audio/pgm.raw` | PGM stereo input ring buffer (4MB interleaved stereo float32) |
| `/run/pi-audio/mic-{1..4}.raw` | Mic mono input ring buffers (4MB mono float32 each) |
| `/run/pi-audio/monitor-{channel}.raw` | Per-channel monitor ring buffers (4MB mono float32, engine writes → webapp reads) |

## Logging

Logs written to `/var/log/pi-audio/engine.log` with automatic rotation:
- **Max size:** 1 MB per file
- **Backups:** 3 rotated files (`engine.log.1`, `.2`, `.3`)

## Debugging

```bash
# Service status
sudo systemctl status pi-audio-engine
sudo journalctl -u pi-audio-engine -f

# Run engine manually
sudo systemctl stop pi-audio-engine
/usr/local/bin/pi-audio-engine /etc/pi-audio/config/current.json /var/log/pi-audio/engine.log

# Check engine state
cat /run/pi-audio/engine-state.json

# Test status adapter
/usr/local/bin/pi-audio-status

# Speaker test (writes whitenoise to input buffer)
/usr/local/bin/pi-audio-test PGM_L

# ALSA devices
aplay -l
arecord -l
```

## VM Testing

VMs often lack functional audio hardware. Create a null ALSA device:

```bash
cat > ~/.asoundrc << 'EOF'
pcm.!default {
    type null
}
ctl.!default {
    type hw
    card 0
}
EOF
```

Use the systemd override in `overrides/10-vm-user.conf` to run as a non-root user. The VM deploy script (`deploy-vm.sh`) installs this override automatically.

### VM Permissions

For the engine (running as the current user) and Apache (`www-data`) to share runtime files:

- `/run/pi-audio/` must be owned by the engine user with group `audio`
- `/var/log/pi-audio/` must be owned by the engine user with group `audio`
- Config files must be `664` (not `660`) so the engine can read them
- `www-data` must be in the `audio` group
- Engine user must be in the `audio` group (for directory write access):
  ```bash
  sudo usermod -aG audio $USER
  ```
