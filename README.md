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
│   ├── audio-debug.html        # Real-time audio monitor (WebSocket)
│   ├── ws-audio-server-node.js # WebSocket relay server (Node.js)
│   ├── VERSION.json            # Auto-generated version info
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
cd pi-audio-dsp/build
cmake ..
make -j$(nproc)

# Deploy
sudo bash pi-audio-dsp/redeploy.sh
```

The redeploy script stops the engine, copies binaries from `pi-audio-dsp/build/`, reinstalls the service, fixes permissions, and restarts all services (engine, Apache, WebSocket server).

## Services

- **pi-audio-engine** — Main audio engine (systemd service)
- **pi-audio-ws-server** — WebSocket relay server for real-time audio monitoring (systemd service)
- **Apache** — Web interface on port 80

### Apache Config

`/etc/apache2/sites-available/pi-audio.conf`:

```apache
<VirtualHost *:80>
    DocumentRoot /var/www/pi-audio/pi-audio-web
    <Directory /var/www/pi-audio/pi-audio-web>
        AllowOverride All
        Require all granted
    </Directory>
    SetEnv PI_AUDIO_STATUS_BIN /usr/local/bin/pi-audio-status
    SetEnv PI_AUDIO_APPLY_BIN /usr/local/bin/pi-audio-apply
    SetEnv PI_AUDIO_MEASURE_BIN /usr/local/bin/pi-audio-measure
    SetEnv PI_AUDIO_TEST_BIN_WOOFER_L /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_WOOFER_R /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_TWEETER_L /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_TWEETER_R /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_CONFIG_DIR /etc/pi-audio/config
    SetEnv PI_AUDIO_PRESETS_DIR /etc/pi-audio/presets
    SetEnv PI_AUDIO_CARD 0
    SetEnv PI_AUDIO_CONTROL PCM
    SetEnv PI_AUDIO_XRUN_FILE /run/pi-audio/xrun
</VirtualHost>
```

Adjust `PI_AUDIO_CARD` and `PI_AUDIO_CONTROL` to match your `.asoundrc` / ALSA hardware.

## Architecture

### Audio Data Flow

```
PGM stereo input (ALSA capture or pgm.raw ring buffer)
    │
    ├─► Mono_L ──► woofer-l   (output 0, per-channel DSP)
    │              └─► tweeter-l  (output 1, per-channel DSP)
    │
    └─► Mono_R ──► woofer-r   (output 2, per-channel DSP)
                   └─► tweeter-r  (output 3, per-channel DSP)

Mic inputs (measurement, ring buffers):
    mic-1.raw ──► monitor + TCP relay
    mic-2.raw ──► monitor + TCP relay
    mic-3.raw ──► monitor + TCP relay
    mic-4.raw ──► monitor + TCP relay

PGM is suspended (replaced with silence) when a test or measurement is running.

Test adapter (pi-audio-test)
    │ writes whitenoise to pgm.raw or mic-*.raw
    ▼
AudioEngine playback thread
    │ per-channel DSP (volume/mute/stages)
    ├──► ALSA PCM (4-channel output)
    ├──► monitor-{channel}.raw (per-channel ring buffers)
    └──► TcpRelay → TCP 127.0.0.1:8766 → ws-audio-server-node.js
               │ pushes per-channel mono float32 at real-time pace
               │
               ▼ WebSocket relay (port 8765)
         audio-debug.html (Web Audio API playback + waveform)
```

### WebSocket Monitoring Pipeline

Real-time audio monitoring uses a direct TCP + WebSocket pipeline instead of file-based polling:

1. **Engine (TcpRelay)**: After each DSP processing cycle, stereo float32 data is pushed to a non-blocking TCP client that buffers and sends data to `127.0.0.1:8766`
2. **Node.js relay server** (`ws-audio-server-node.js`): Accepts TCP connection from engine, splits stereo into L/R mono, broadcasts to WebSocket clients per channel
3. **Browser** (`audio-debug.html`): Connects via WebSocket, accumulates a startup buffer (~300ms), then schedules audio through Web Audio API for smooth playback

**Protocol** (TCP engine → Node.js): `[4 bytes BE sample count][sampleCount × 8 bytes interleaved stereo float32]`

**Fallback**: Engine still writes monitor ring buffer files for backward compatibility with `audio-chunk.php` and `audio-stream.php`.

### Engine Communication

The engine and web UI communicate through files in `/run/pi-audio/` and a TCP relay:

| File | Purpose |
|---|---|
| `status` | Engine running flag (created on start, removed on stop) |
| `engine-state.json` | Current engine parameters (written every second) |
| `reload` | Reload trigger (written by apply adapter, consumed by engine) |
| `pgm.raw` | PGM stereo input ring buffer (4MB interleaved stereo float32) |
| `mic-{1..4}.raw` | Mic mono input ring buffers (4MB mono float32 each) |
| `monitor-{channel}.raw` | Per-channel monitor ring buffers (4MB mono float32, engine writes → webapp reads, fallback) |

**TCP Relay** (port 8766): Engine pushes per-channel mono float32 monitor data to Node.js relay server in real-time. Relay broadcasts to WebSocket clients per channel (port 8765).

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

Supported monitor channels: `pgm-l`, `pgm-r`, `woofer-l`, `tweeter-l`, `woofer-r`, `tweeter-r`, `mic-1`, `mic-2`, `mic-3`, `mic-4`.

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

### DSP Engine

```bash
sudo apt install cmake pkg-config libasound2-dev
```

### WebSocket Monitoring Server

```bash
# Install Node.js (18+)
curl -fsSL https://deb.nodesource.com/setup_18.x | sudo -E bash -
sudo apt install -y nodejs

# Install WebSocket server dependencies
cd /var/www/pi-audio/pi-audio-web
npm install ws
```

### Setup WebSocket Server Service

```bash
# Install service file
sudo cp /var/www/pi-audio/pi-audio-dsp/pi-audio-ws-server.service /etc/systemd/system/

# Create VM override (runs as current user instead of root:audio)
sudo mkdir -p /etc/systemd/system/pi-audio-ws-server.service.d
sudo cp /var/www/pi-audio/pi-audio-dsp/overrides/10-vm-user.conf /etc/systemd/system/pi-audio-ws-server.service.d/

# Reload systemd and start
sudo systemctl daemon-reload
sudo systemctl enable pi-audio-ws-server
sudo systemctl start pi-audio-ws-server

# Check status
sudo systemctl status pi-audio-ws-server
sudo journalctl -u pi-audio-ws-server -f
```

## Debugging

```bash
# Engine service status
sudo systemctl status pi-audio-engine
sudo journalctl -u pi-audio-engine -f

# WebSocket server status
sudo systemctl status pi-audio-ws-server
sudo journalctl -u pi-audio-ws-server -f

# Run engine manually (foreground)
sudo systemctl stop pi-audio-engine
/usr/local/bin/pi-audio-engine /etc/pi-audio/config/current.json /var/log/pi-audio/engine.log

# Run WebSocket server manually (foreground)
sudo systemctl stop pi-audio-ws-server
cd /var/www/pi-audio/pi-audio-web && node ws-audio-server-node.js

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

# WebSocket server health check
curl http://localhost:8765/health
curl http://localhost:8765/version
```

### Debug Pages

- **`/debug.php`** — Full system debug: build versions, environment, permissions, binary runtime status (running PIDs, last run time, success/error), monitor ring buffer header, engine state vs config comparison, auto-refreshes every 5 seconds
- **`/audio-debug.html`** — Real-time audio monitor: connects via WebSocket to `ws-audio-server-node.js`, accumulates startup buffer (~300ms), plays through Web Audio API, waveform visualization, channel selection (pgm-l/pgm-r), auto-detects engine sample rate

## Roadmap

| Phase | Features |
|---|---|
| Phase 1 | ALSA PCM playback, volume/mute, sample rate/buffer size, web interface adapters, monitor ring buffer, audio debug page, WebSocket monitoring pipeline |
| Phase 1.5 | Replace TcpRelay with engine-embedded WebSocket server (Option A, zero-copy) |
| Phase 2 | DSP effects (EQ, crossover, delay, trim, phase inversion) |
| Phase 3 | Room measurement and correction (mic input, RTA, impulse response) |
| Phase 4 | Multi-channel support (4 outputs, stereo PGM + 4 mic inputs), per-channel monitoring |
