# Pi Audio — Monorepo

Single repository for the Pi Audio DSP engine and web interface.

## Quick Install

Full Raspberry Pi setup from a fresh OS image. Run steps in order.

### 1. System update and dependencies

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y apache2 cmake pkg-config libasound2-dev php libapache2-mod-php alsa-utils nodejs npm git
```

### 2. Clone the repository

```bash
git clone https://github.com/LucasTippelt/pi-audio.git /var/www/pi-audio
cd /var/www/pi-audio
```

### 3. WebSocket server dependency

```bash
cd /var/www/pi-audio/pi-audio-web
npm install ws
```

### 4. Create config directories

```bash
sudo mkdir -p /etc/pi-audio/config /etc/pi-audio/presets
```

### 5. Apache authentication and permissions

Set up Basic Auth and grant `www-data` access to config and project directories:

```bash
sudo a2enmod auth_basic headers
sudo install -d -o root -g www-data -m 0750 /etc/pi-audio
sudo htpasswd -c /etc/pi-audio/htpasswd pi-audio
sudo chown root:www-data /etc/pi-audio/htpasswd
sudo chmod 0640 /etc/pi-audio/htpasswd
sudo install -d -o www-data -g www-data -m 0770 /etc/pi-audio/config /etc/pi-audio/presets
sudo chown -R www-data:www-data /var/www/pi-audio
```

Username for web login is `pi-audio`.

### 6. Apache virtual host

Create `/etc/apache2/sites-available/pi-audio.conf`:

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

Adjust `PI_AUDIO_CARD` to match your ALSA hardware.

### 7. Apache systemd sandbox override

Apache's sandbox blocks device access by default. Create an override so PHP can reach the ALSA mixer:

```bash
sudo systemctl edit apache2
```

Paste:
```ini
[Service]
ReadWritePaths=/etc/pi-audio/config /etc/pi-audio/presets
PrivateDevices=no
ProcSubset=all
DeviceAllow=/dev/snd/controlC0 rw
```

### 8. Audio group and site activation

Add `www-data` to the `audio` group so Apache can access sound devices, then enable the site:

```bash
sudo usermod -aG audio www-data
sudo a2ensite pi-audio.conf
sudo a2dissite 000-default.conf
sudo systemctl daemon-reload
sudo systemctl restart apache2
```

Verify sandbox settings:
```bash
sudo systemctl show apache2 -p PrivateDevices -p DevicePolicy -p DeviceAllow -p ProcSubset
```

### 9. Install systemd services and deploy

Copy the service files, then run the deploy script (builds the DSP engine, installs binaries, and enables services):

```bash
sudo cp /var/www/pi-audio/pi-audio-ws-server.service /etc/systemd/system/
sudo cp /var/www/pi-audio/pi-audio-dsp/pi-audio-engine.service /etc/systemd/system/
cd /var/www/pi-audio/pi-audio-dsp
sudo ./deploy.sh
```

### 10. Start the WebSocket server

```bash
sudo systemctl daemon-reload
sudo systemctl enable pi-audio-ws-server
sudo systemctl start pi-audio-ws-server
```

The web interface is now available at `http://<pi-ip>/`.

## Setup

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

### Versioning

Version format: `vX.Y.Z.UIO` (e.g. `v1.3.0.0`)

| Component | Meaning |
|---|---|
| `X` | Major release (auto-incremented on push to `master`) |
| `Y` | Staging release (auto-incremented on push to `staging`) |
| `Z` | Minor version (manual bump by user) |
| `UIO` | Build iteration (auto-incremented by `gen-version.sh` on each build) |

Defined in `VERSION` (line 1 = `X.Y.Z`, line 2 = `UIO`, optional).

The pre-push hook auto-increments `Y` on staging pushes and `X` on master pushes. To manually bump the minor version (`Z`) and reset `UIO` to 0:

```bash
# Increment Z by 1 (e.g., 1.3.0 → 1.3.1)
./bump-version.sh

# Set Z to a specific value (e.g., 1.3.0 → 1.3.5)
./bump-version.sh 5
```

Commit the VERSION change, then push — the pre-push hook will apply the final bump.

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
├── VERSION                     # Version file (line 1 = X.Y.Z, line 2 = UIO)
├── bump-version.sh             # Helper: bump minor version (Z) and reset UIO to 0
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

1. **Engine (TcpRelay)**: After each DSP processing cycle, per-channel mono float32 data is pushed to a non-blocking TCP client. Channel enable/disable control (`E <ch>\n` / `D <ch>\n`) is sent from WS server to engine over TCP. Only enabled channels are relayed.
2. **Node.js relay server** (`ws-audio-server-node.js`): Accepts TCP connection from engine, broadcasts to WebSocket clients per channel. Pre-allocated 64KB buffer with `copyWithin` replaces `Buffer.concat` to reduce GC pressure.
3. **Browser** (`audio-debug.html`): Connects via WebSocket, 200ms ring buffer with 50ms warmup, plays through Web Audio API (ScriptProcessorNode bufSize=256), waveform visualization uses `copyWithin` to avoid GC storms.

**Protocol** (TCP engine → Node.js): `[1 byte channel ID][4 bytes BE sample count][sampleCount × 4 bytes mono float32]`

**Channel Control** (TCP Node.js → engine): `E <ch>\n` to enable channel, `D <ch>\n` to disable. Only enabled channels are processed and relayed.

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
- **`/audio-debug.html`** — Real-time audio monitor: connects via WebSocket to `ws-audio-server-node.js`, 200ms ring buffer with 50ms warmup, plays through Web Audio API (ScriptProcessorNode bufSize=256), waveform visualization, per-channel selection, auto-detects engine sample rate, callback metrics (drop rate, events, available samples)

## Roadmap

| Phase | Features |
|---|---|
| Phase 1 | ALSA PCM playback, volume/mute, sample rate/buffer size, web interface adapters, monitor ring buffer, audio debug page, WebSocket monitoring pipeline |
| Phase 1.5 | Replace TcpRelay with engine-embedded WebSocket server (Option A, zero-copy) |
| Phase 2 | DSP effects (EQ, crossover, delay, trim, phase inversion) |
| Phase 3 | Room measurement and correction (mic input, RTA, impulse response) |
| Phase 4 | Multi-channel support (4 outputs, stereo PGM + 4 mic inputs), per-channel monitoring |
