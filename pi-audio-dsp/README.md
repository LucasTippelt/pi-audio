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

## Architecture

```
pi-audio-dsp/
├── src/
│   ├── bin/              # Executable entry points
│   ├── lib/
│   │   ├── core/         # Engine, config manager, logger
│   │   └── adapters/     # Web interface adapters
│   └── include/
│       ├── core/         # Headers
│       └── utils/        # JSON parser
├── overrides/
│   └── 10-vm-user.conf   # VM systemd override (runs as current user)
├── CMakeLists.txt
├── deploy.sh             # Production deploy script
├── deploy-vm.sh          # VM deploy script
├── redeploy.sh           # Quick redeploy (binaries + restart)
└── pi-audio-engine.service
```

## Binaries

| Binary | Purpose |
|---|---|
| `pi-audio-engine` | Main audio engine service |
| `pi-audio-apply` | Apply configuration adapter — writes reload trigger |
| `pi-audio-status` | Status probe adapter — reads engine state file |
| `pi-audio-measure` | Room measurement adapter |
| `pi-audio-test` | Speaker test adapter |

## Configuration

Engine reads from `/etc/pi-audio/config/current.json`. Adapters communicate via JSON stdin/stdout.

### Hot Reload (Trigger File)

The engine does not poll the config file. Instead, the apply adapter signals the engine to reload by writing a trigger file:

1. Web UI calls `PI_AUDIO_APPLY_BIN` with new config
2. Apply adapter validates config, writes `/run/pi-audio/reload`
3. Engine detects trigger file in its main loop (checked every second)
4. Engine reloads config from `current.json`, applies changes, removes trigger

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

## Logging

Logs written to `/var/log/pi-audio/engine.log`.

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
