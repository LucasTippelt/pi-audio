# Pi Audio — Monorepo

Single repository for the Pi Audio DSP engine and web interface.

## Setup

### Clone

```bash
git clone https://github.com/LucasTippelt/pi-audio.git /var/www/pi-audio
cd /var/www/pi-audio
```

### VS Code SSH

1. Install the **Remote - SSH** extension in VS Code
2. Open SSH config: `Ctrl+Shift+P` → **Remote-SSH: Open Configuration File**
3. Add the host entry:
   ```
   Host pi-audio
       HostName <pi-ip-or-hostname>
       User <username>
       IdentityFile ~/.ssh/<private-key>
   ```
4. Connect: `Ctrl+Shift+P` → **Remote-SSH: Connect to Host** → select `pi-audio`
5. Open the workspace: `File` → `Open Folder` → `/var/www/pi-audio`

### Branch switching

Two branches are used:

| Branch | Purpose |
|---|---|
| `master` | Stable, production-ready code |
| `staging` | Experimental features, pre-production testing |

```bash
# Switch branches
git fetch origin
git checkout staging
git checkout master

# Pull / push
git pull origin <branch>
git push origin <branch>

# Merge staging into master (when ready)
git checkout master
git merge staging
git push origin master
```

### Team collaboration

**Single developer (Pi as dev + deploy host):** VS Code connects to the Pi via SSH, editing files directly on the device.

**Multiple developers:** Each developer clones the repo locally, works on `staging`, and pushes to GitHub. The Pi pulls from `staging` to deploy, avoiding concurrent editing conflicts.

**VPN:** If the Pi is remote, use WireGuard or Tailscale for secure SSH access without exposing router ports.

### Remote AI development (optional)

Set up Kilo Code on the Pi with a local LLM hosted on a workstation (e.g., GPU machine):

1. With VS Code connected to the Pi, install **Kilo Code** for the remote host
2. Point Kilo to your workstation's OpenAI-compatible API endpoint:
   ```
   http://<workstation-ip>:8080/v1
   ```
3. Ensure Node.js/npm is available on the Pi for extension tool dependencies:
   ```bash
   sudo apt update && sudo apt install -y nodejs npm
   ```
4. Reload VS Code (`Developer: Reload Window`) to apply PATH changes

## Structure

```
/var/www/pi-audio/
├── pi-audio-dsp/               # DSP engine (C++, CMake, ALSA)
│   ├── src/
│   │   ├── bin/                # Executable entry points
│   │   ├── lib/
│   │   │   ├── core/           # Engine, config, logger, monitor ring buffers
│   │   │   └── adapters/       # Web interface adapters (apply, status, test, measure)
│   │   └── include/
│   │       ├── core/           # Headers (engine, config, logger, monitor)
│   │       └── utils/          # JSON parser (header-only)
│   ├── overrides/              # VM systemd overrides
│   ├── deploy.sh               # Production deploy
│   ├── deploy-vm.sh            # VM deploy
│   ├── redeploy.sh             # Quick redeploy (stops engine, copies binaries, restarts)
│   └── pi-audio-engine.service
├── pi-audio-web/               # Web interface (PHP, Apache)
│   ├── api.php                 # REST API
│   ├── index.php               # Main UI
│   ├── debug.php               # System debug page
│   ├── audio-debug.html        # Real-time audio monitor
│   ├── audio-stream.php        # Legacy streaming endpoint (long-lived connection)
│   ├── audio-chunk.php         # Chunked audio endpoint (fixed-size requests)
│   └── assets/                 # CSS, JS, static assets
└── README.md
```

## Deploy

### Production (Raspberry Pi)

```bash
cd /var/www/pi-audio/pi-audio-dsp
sudo ./deploy.sh
```

Builds the DSP engine, installs binaries, configures Apache, and sets up the systemd service.

### VM / Development

```bash
# Build (use temp directory if build/ is owned by root)
mkdir /tmp/pi-audio-build && cd /tmp/pi-audio-build
cmake /var/www/pi-audio/pi-audio-dsp
make -j$(nproc)

# Deploy
sudo bash /var/www/pi-audio/pi-audio-dsp/redeploy.sh
```

The redeploy script stops the engine, copies binaries from `/tmp/pi-audio-build/`, reinstalls the service, fixes permissions, and restarts the engine.

## Services

- **pi-audio-engine** — Main audio engine (systemd service)
- **Apache** — Web interface on port 80

## Architecture

### Audio Data Flow

```
Test adapter (pi-audio-test)
    │ writes whitenoise
    ▼
input.raw (4MB ring buffer)
    │ engine reads
    ▼
AudioEngine playback thread
    │ mono-to-stereo, DSP (volume/mute/stages)
    ├──► ALSA PCM (actual speakers)
    ├──► monitor-pgm-l.raw (left program output)
    └──► monitor-pgm-r.raw (right program output)
              │
              ▼ webapp reads
       audio-chunk.php / audio-stream.php
              │
              ▼
       audio-debug.html (Web Audio API playback + waveform)
```

### Engine Communication

The engine and web UI communicate through files in `/run/pi-audio/`:

| File | Purpose |
|---|---|
| `status` | Engine running flag (created on start, removed on stop) |
| `engine-state.json` | Current engine parameters (written every second) |
| `reload` | Reload trigger (written by apply adapter, consumed by engine) |
| `input.raw` | Input ring buffer (4MB mono float32, test writes → engine reads) |
| `monitor-{channel}.raw` | Per-channel monitor ring buffers (4MB mono float32, engine writes → webapp reads) |

### Ring Buffer Format

All ring buffers use a 64-byte header followed by raw float32 data:

**Input buffer (`PIAI` magic):**
- `magic[4]` = "PIAI"
- `version[2]`
- `writePos[8]` — byte offset of next write (0..dataSize-1)
- `sampleRate[4]` — Hz
- `channels[2]` — always 1 (mono)
- `seq[4]` — monotonically increasing write sequence counter
- `channelName[16]` — null-terminated identifier
- `reserved[20]`

**Monitor buffers (`PIAM` magic):** Same layout, magic "PIAM".

Supported monitor channels: `pgm-l`, `pgm-r`, `woofer-l`, `tweeter-l`, `woofer-r`, `tweeter-r`, `mic`.

## Config

- `/etc/pi-audio/config/current.json` — Active configuration
- `/etc/pi-audio/presets/` — Saved presets

### Hot Reload (Trigger File)

The engine does not poll the config file. Instead, the apply adapter signals the engine to reload:

1. Web UI calls `PI_AUDIO_APPLY_BIN` with new config
2. Apply adapter validates config, writes `/run/pi-audio/reload`
3. Engine detects trigger file in its main loop (checked every second)
4. Engine reloads config from `current.json`, applies changes, removes trigger
5. Apply adapter polls `/run/pi-audio/engine-state.json` for up to 5 seconds, confirming the engine reports the new parameters

Volume and mute changes are applied atomically without restart. Sample rate, buffer size, or device changes require the engine to stop and reinitialize.

## Logging

Logs written to `/var/log/pi-audio/engine.log` with automatic rotation:
- **Max size:** 1 MB per file
- **Backups:** 3 rotated files (`engine.log.1`, `.2`, `.3`)
- Older backups are overwritten on rotation

## Dependencies

```bash
sudo apt install cmake pkg-config libasound2-dev
```

## Debugging

```bash
# Service status
sudo systemctl status pi-audio-engine
sudo journalctl -u pi-audio-engine -f

# Run engine manually (foreground)
sudo systemctl stop pi-audio-engine
/usr/local/bin/pi-audio-engine /etc/pi-audio/config/current.json /var/log/pi-audio/engine.log

# Verify installed binaries
ls -la /usr/local/bin/pi-audio-*

# ALSA devices
aplay -l
arecord -l

# Engine state
cat /run/pi-audio/engine-state.json
/usr/local/bin/pi-audio-status

# Monitor ring buffers
ls -la /run/pi-audio/monitor-*.raw

# Speaker test
/usr/local/bin/pi-audio-test PGM_L
```

### Debug Pages

- **`/debug.php`** — Full system debug: environment, permissions, binary runtime status (running PIDs, last run time, success/error), monitor ring buffer header, engine state vs config comparison, auto-refreshes every 5 seconds
- **`/audio-debug.html`** — Real-time audio monitor: fetches fixed-size audio chunks via `audio-chunk.php`, plays through Web Audio API, waveform visualization, channel selection, auto-detects engine sample rate

## Roadmap

| Phase | Features |
|---|---|
| Phase 1 | ALSA PCM playback, volume/mute, sample rate/buffer size, web interface adapters, monitor ring buffer, audio debug page |
| Phase 2 | DSP effects (EQ, crossover, delay, trim, phase inversion) |
| Phase 3 | Room measurement and correction (mic input, RTA, impulse response) |
| Phase 4 | Multi-channel support (4 outputs, 6 inputs), per-channel monitoring |
