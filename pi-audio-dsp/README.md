# Pi Audio DSP Engine

Real-time audio engine and DSP pipeline for Raspberry Pi with ALSA support. Part of the [pi-audio](..) monorepo.

## Requirements

- **CMake** 3.10+
- **GCC** (C++17 support)
- **libasound2-dev** (ALSA development headers)
- **pkg-config**

No external JSON library — bundled header-only implementation in `src/include/utils/json.hpp`.

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Deploy

### Raspberry Pi (production)

```bash
cd pi-audio-dsp
sudo ./deploy.sh
```

The deploy script builds the engine, installs binaries to `/usr/local/bin/`, configures the systemd service, and sets up Apache.

### VM / Development

```bash
# Build (use temp directory if build/ is owned by root)
mkdir /tmp/pi-audio-build && cd /tmp/pi-audio-build
cmake /var/www/pi-audio/pi-audio-dsp
make -j$(nproc)

# Deploy with VM override (uses current user instead of pi:audio)
sudo bash deploy-vm.sh
```

For subsequent rebuilds:

```bash
cd /tmp/pi-audio-build && make -j$(nproc)
sudo bash redeploy.sh
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
│   │   ├── core/         # Engine, config manager, logger, monitor ring buffer
│   │   │   ├── engine.cpp        # AudioEngine class (playback thread, DSP pipeline)
│   │   │   ├── config.cpp        # ConfigManager (JSON config loading)
│   │   │   ├── logger.cpp        # Logger with rotation (1MB max, 3 backups)
│   │   │   └── monitor.cpp       # Per-channel monitor ring buffer writer
│   │   └── adapters/     # Web interface adapters
│   │       ├── apply.cpp         # Validates config, writes reload trigger, polls confirmation
│   │       ├── status.cpp        # Reads engine state file
│   │       ├── test.cpp          # Writes whitenoise to input ring buffer
│   │       └── measure.cpp       # Room measurement (stub)
│   └── include/
│       ├── core/         # Headers
│       └── utils/        # JSON parser
├── overrides/
│   └── 10-vm-user.conf   # VM systemd override (runs as current user)
├── CMakeLists.txt
├── deploy.sh             # Production deploy script
├── deploy-vm.sh          # VM deploy script
├── redeploy.sh           # Quick redeploy (stops engine, copies binaries, restarts)
└── pi-audio-engine.service
```

## Binaries

| Binary | Purpose |
|---|---|
| `pi-audio-engine` | Main audio engine service — reads input.raw, processes DSP, writes ALSA + per-channel monitor buffers |
| `pi-audio-apply` | Apply configuration adapter — validates config, writes reload trigger, polls engine-state.json for confirmation (up to 5s) |
| `pi-audio-status` | Status probe adapter — reads engine state file and reports connection status |
| `pi-audio-measure` | Room measurement adapter (not yet implemented) |
| `pi-audio-test` | Speaker test adapter — writes 3 seconds of whitenoise to input ring buffer for the requested driver |

### Supported Test Drivers

| Driver | Monitor Channel |
|---|---|
| `PGM_L` | `pgm-l` |
| `PGM_R` | `pgm-r` |
| `Woofer_L` | `woofer-l` |
| `Tweeter_L` | `tweeter-l` |
| `Woofer_R` | `woofer-r` |
| `Tweeter_R` | `tweeter-r` |
| `Mic` | `mic` |

## Engine Architecture

### Playback Thread

The engine runs a single playback thread that loops at real-time pace:

1. **Sleep** for buffer duration (`bufferSize / sampleRate` seconds, e.g. 10.7ms for 512 frames at 48kHz)
2. **Read** mono float32 samples from `input.raw` ring buffer
3. **Convert** mono to stereo (duplicate to both channels)
4. **Process** DSP pipeline (volume/mute + extensible stages)
5. **Write** stereo float32 to ALSA PCM device
6. **Write** left channel to `monitor-pgm-l.raw`, right channel to `monitor-pgm-r.raw`

### Real-Time Pacing

The playback thread uses per-cycle sleep pacing to maintain real-time speed. This is required because null ALSA devices (used in VMs) return instantly from `snd_pcm_writei()`, which would cause the loop to run at CPU speed instead of audio rate.

### Input Ring Buffer

The engine reads from `/run/pi-audio/input.raw` (4MB mono float32 ring buffer). On startup, the engine's internal read position syncs to the current write position to avoid consuming stale data from a previous session.

### Monitor Ring Buffers

The engine creates per-channel monitor buffers on startup (`monitor-pgm-l.raw`, `monitor-pgm-r.raw`). Each is a 4MB mono float32 ring buffer with a 64-byte header. The webapp reads these via `audio-chunk.php` or `audio-stream.php`.

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
{"input":"default","output":"default","sampleRate":48000,"BufferSize":512,"volume":65,"muted":false}
```

The status adapter reads this file and merges it with the connection status. The web UI's debug page compares this state against `current.json` to verify that apply operations took effect.

## Runtime Files

| Path | Purpose |
|---|---|
| `/run/pi-audio/status` | Engine running flag (created on start, removed on stop) |
| `/run/pi-audio/engine-state.json` | Current engine parameters (written every second) |
| `/run/pi-audio/reload` | Reload trigger (written by apply adapter, consumed by engine) |
| `/run/pi-audio/input.raw` | Input ring buffer (4MB mono float32, test writes → engine reads) |
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
