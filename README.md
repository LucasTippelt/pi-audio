# Pi Audio — Monorepo

Single repository for the Pi Audio DSP engine and web interface.

## Architecture

**Single-speaker system** with 2 outputs (tweeter + woofer), each with independent DSP chains. Stereo PGM input: left channel routes through DSP to both outputs, right channel is a direct output for a future linked speaker (with onboard DSP/room correction). Configurable 2-6 measurement microphone inputs.

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

### 3. Monitoring endpoints

The DSP engine exposes multiple monitoring paths: the stable browser monitor uses the Node.js relay on port 8765 and receives engine data on port 8766, while the lower-latency stream uses the built-in HTTP server on port 8767. The engine also exposes a direct WebSocket endpoint on port 8768 for raw browser access.

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
    SetEnv PI_AUDIO_TEST_BIN_PGM_L /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_PGM_R /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_TWEETER /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_WOOFER /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_MIC_1 /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_MIC_2 /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_MIC_3 /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_MIC_4 /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_MIC_5 /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_MIC_6 /usr/local/bin/pi-audio-test
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
sudo cp /var/www/pi-audio/pi-audio-dsp/pi-audio-engine.service /etc/systemd/system/
cd /var/www/pi-audio/pi-audio-dsp
sudo ./deploy.sh
```

### 10. (Optional) Start the stability monitor relay

The Node.js relay is the stability-focused monitoring option: it accepts the engine TCP stream on port 8766 and exposes a WebSocket browser endpoint on port 8765. The low-latency alternative is the engine's built-in HTTP stream on port 8767.

```bash
sudo cp /var/www/pi-audio/pi-audio-ws-server.service /etc/systemd/system/
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
git fetch origin
git checkout staging
git checkout master
git pull origin <branch>
git push origin <branch>
git checkout master && git merge staging && git push origin master
```

### Team collaboration

**Single developer (Pi as dev + deploy host):** VS Code connects to the Pi via SSH, editing files directly on the device.

**Multiple developers:** Each developer clones the repo locally, works on `staging`, and pushes to GitHub. The Pi pulls from `staging` to deploy.

**VPN:** If the Pi is remote, use WireGuard or Tailscale for secure SSH access.

### Versioning

Version format: `vX.Y.Z.UIO` (e.g. `v2.1.0.0`)

| Component | Meaning |
|---|---|
| `X` | Major release (auto-incremented on push to `master`) |
| `Y` | Staging release (auto-incremented on push to `staging`) |
| `Z` | Minor version (manual bump by user) |
| `UIO` | Build iteration (auto-incremented by `gen-version.sh`) |

Defined in `VERSION` (line 1 = `X.Y.Z`, line 2 = `UIO`, optional).

```bash
./bump-version.sh        # Increment Z by 1
./bump-version.sh 5      # Set Z to 5
```

### Remote AI development (optional)

Set up Kilo Code on the Pi with a local LLM hosted on a workstation:

1. With VS Code connected to the Pi, install **Kilo Code** for the remote host
2. Point Kilo to your workstation's OpenAI-compatible API endpoint: `http://<workstation-ip>:8080/v1`
3. Ensure Node.js/npm is available on the Pi: `sudo apt install -y nodejs npm`
4. Reload VS Code to apply PATH changes

## Structure

```
/var/www/pi-audio/
├── pi-audio-dsp/               # DSP engine (C++, CMake, ALSA)
│   ├── src/
│   │   ├── bin/                # Executable entry points
│   │   ├── lib/
    │   │   │   ├── core/           # Engine, config, logger, monitor, TCP relay, HTTP stream, WebSocket server
│   │   │   └── adapters/       # Web interface adapters (apply, status, test, measure)
│   │   └── include/
    │   │       ├── core/           # Headers (engine, config, logger, monitor, tcp_relay, http_stream, websocket_server)
│   │       └── utils/          # JSON parser (header-only)
│   ├── overrides/              # VM systemd overrides
│   ├── deploy.sh               # Production deploy
│   ├── deploy-vm.sh            # VM deploy
│   ├── redeploy.sh             # Quick redeploy
│   └── pi-audio-engine.service
├── pi-audio-web/               # Web interface (PHP, Apache)
│   ├── api.php                 # REST API
│   ├── index.php               # Main UI
│   ├── debug.php               # System debug page
    │   ├── audio-debug.html        # Real-time audio monitor (relay + direct WS + HTTP stream)
│   ├── ws-audio-server-node.js # Stability monitor relay (Node.js, port 8765)
│   ├── VERSION.json            # Auto-generated version info
│   └── assets/                 # CSS, JS, static assets
├── pi-audio-ws-server.service  # WebSocket relay service (optional)
├── VERSION                     # Version file
├── bump-version.sh             # Version bump helper
└── README.md
```

## Deploy

### Production (Raspberry Pi)

```bash
cd /var/www/pi-audio/pi-audio-dsp
sudo ./deploy.sh
```

### VM / Development

```bash
cd pi-audio-dsp/build && cmake .. && make -j$(nproc)
sudo bash pi-audio-dsp/redeploy.sh
```

## Audio Data Flow

### Normal Playback

```
PGM stereo input (ALSA capture or shared memory ring buffer)
    │
    ├──► PGM Left ──► DSP chain ──┬──► Tweeter (output 0)
    │                              └──► Woofer  (output 1)
    │
    └──► PGM Right ──► direct output (linked speaker, future onboard DSP)

Mic inputs (2-6 configurable, shared memory ring buffers):
    shm://pi-audio-mic-{1..6} ──► monitor + relay
```

### Test Mode

The engine detects active tests via `/dev/shm/pi-audio-test-mode` control file. During tests, real-time PGM routing is suspended.

```
Tweeter test:  test injection ──► tweeter DSP ──► tweeter speaker (woofer silent)
Woofer test:   test injection ──► woofer DSP  ──► woofer speaker  (tweeter silent)
PGM_L test:    PGM ring buffer ──► monitoring only (no physical speakers)
```

Monitoring channels always show actual DSP output:
- Tweeter monitoring: tweeter output after DSP
- Woofer monitoring: woofer output after DSP
- PGM_L monitoring: raw PGM_L input

Test adapter (pi-audio-test)
    │ writes whitenoise to shared memory, signals engine via control file
    ▼
AudioEngine playback thread
    │ per-output DSP (volume/mute/stages)
    ├──► ALSA PCM (2-channel: tweeter + woofer)
    ├──► shm://pi-audio-monitor-{channel} (per-channel ring buffers)
    ├──► TcpRelay → TCP 127.0.0.1:8766 → ws-audio-server-node.js (stability monitor relay)
    ├──► HttpStreamServer → HTTP :8767 → browser (low-latency stream)
    └──► WebSocketServer  → WS  :8768 → browser (direct engine endpoint)
```

### WebSocket Monitoring (Direct Engine Endpoint)

The engine embeds a minimal WebSocket server on port 8768 for direct browser access with application-level framing. Each 512-frame buffer arrives as a distinct binary frame, bypassing browser TCP receive batching for stable `onmessage` callbacks every ~10.7ms.

- **Subscribe:** Client sends text frame `{"channel":"tweeter"}`
- **Ack:** Server replies `{"ok":true,"sampleRate":48000,"bufferSize":512}`
- **Audio:** Server pushes binary frames — raw mono float32LE, 512 frames per frame (2048 bytes)
- **Unsubscribe:** Client closes connection

**Channels:** `pgm-l`, `pgm-r`, `tweeter`, `woofer`, `mic-1`..`mic-6`

### HTTP Stream Monitoring (Low-Latency Option)

The engine embeds a minimal HTTP server on port 8767 that serves real-time audio streams directly to the browser. This is the low-latency monitoring path, while the Node.js relay on port 8765 provides the stability-oriented monitor.

- **`GET /stream?ch=<channel>`** — Continuous raw float32LE mono audio stream with HTTP chunked transfer encoding
- **`GET /status`** — JSON with sampleRate, BufferSize, volume, muted
- **`GET /`** — Channel list

The browser uses `fetch()` + `ReadableStream` to consume the stream, then schedules chunks with `AudioBufferSourceNode` for gapless playback. No Node.js relay required.

**Channels:** `pgm-l`, `pgm-r`, `tweeter`, `woofer`, `mic-1`..`mic-6`

### TCP Relay (Stability Monitor Path)

The TCP relay (port 8766) + Node.js WebSocket server (port 8765) path is the stability-oriented monitor path. It remains a first-class option alongside the lower-latency HTTP stream on port 8767.

**Protocol** (TCP engine → Node.js): `[1 byte channel ID][4 bytes BE sample count][sampleCount × 4 bytes mono float32]`

**Channel Control** (TCP Node.js → engine): `E <ch>\n` to enable, `D <ch>\n` to disable.

### Engine Communication

The engine and web UI communicate through files in `/run/pi-audio/` and POSIX shared memory:

| File | Purpose |
|---|---|
| `status` | Engine running flag |
| `engine-state.json` | Current engine parameters (written every second) |
| `reload` | Reload trigger (written by apply adapter) |

Shared memory ring buffers (visible at `/dev/shm/`):

| Object | Purpose |
|---|---|
| `pi-audio-pgm` | PGM stereo input ring buffer (4MB interleaved stereo float32) |
| `pi-audio-mic-{1..6}` | Mic mono input ring buffers (4MB mono float32 each) |
| `pi-audio-monitor-{channel}` | Per-channel monitor ring buffers (4MB mono float32) |
| `pi-audio-test-tweeter` | Tweeter test injection buffer (4MB mono float32) |
| `pi-audio-test-woofer` | Woofer test injection buffer (4MB mono float32) |
| `pi-audio-test-mode` | Test mode control file (contains "tweeter", "woofer", or "pgm-l") |

### Ring Buffer Format

All ring buffers use a 64-byte header followed by raw float32 data, shared via `shm_open()` + `mmap()`:

**Input buffer (`PIAI` magic):** `magic[4]`, `version[2]`, `writePos[8]`, `sampleRate[4]`, `channels[2]`, `seq[4]`, `channelName[16]`, `reserved[20]`

**Monitor buffers (`PIAM` magic):** Same layout.

Supported monitor channels: `pgm-l`, `pgm-r`, `tweeter`, `woofer`, `mic-1`..`mic-6`.

## Config

- `/etc/pi-audio/config/current.json` — Active configuration
- `/etc/pi-audio/presets/` — Saved presets

### Hot Reload (Trigger File)

1. Web UI calls `PI_AUDIO_APPLY_BIN` with new config
2. Apply adapter validates config, writes `/run/pi-audio/reload`
3. Engine detects trigger file (checked every second)
4. Engine reloads config, applies changes, removes trigger
5. Apply adapter polls `engine-state.json` for confirmation (up to 5s)

Volume/mute: atomic, no restart. Sample rate/buffer size/device: requires reinitialize.

## Logging

`/var/log/pi-audio/engine.log` — 1 MB max, 3 rotated backups.

## Debugging

```bash
sudo systemctl status pi-audio-engine
sudo journalctl -u pi-audio-engine -f

# Run engine manually
sudo systemctl stop pi-audio-engine
/usr/local/bin/pi-audio-engine /etc/pi-audio/config/current.json /var/log/pi-audio/engine.log

# Engine state
cat /run/pi-audio/engine-state.json
/usr/local/bin/pi-audio-status

# Speaker test (runs test, waits for engine to consume, then exits)
/usr/local/bin/pi-audio-test Tweeter
/usr/local/bin/pi-audio-test Woofer
/usr/local/bin/pi-audio-test PGM_L
/usr/local/bin/pi-audio-test Mic_1

# If tests fail with "write failed", clean stale shared memory:
sudo rm -f /dev/shm/pi-audio-test-* /dev/shm/pi-audio-test-mode

# HTTP stream server health
curl http://localhost:8767/status
curl http://localhost:8767/

# WebSocket server (port 8768)
wscat -c ws://localhost:8768

# Monitor ring buffers
ls -la /dev/shm/pi-audio-monitor-*

# ALSA devices
aplay -l && arecord -l
```

### Debug Pages

- **`/debug.php`** — Full system debug: versions, environment, permissions, binary status, per-channel monitor ring buffers (shared memory), engine state vs config, auto-refreshes every 5s
- **`/audio-debug.html`** — Real-time audio monitor: Node.js stability WebSocket on port 8765, direct engine WebSocket on port 8768, or low-latency HTTP stream on port 8767, with gapless AudioBufferSourceNode scheduling, waveform visualization, per-channel selection, and auto sample rate detection with AudioContext recreation

## Roadmap

| Phase | Features |
|---|---|
| Phase 1 | ALSA PCM, volume/mute, sample rate/buffer size, web UI, monitor ring buffers, HTTP stream, WebSocket monitoring |
| Phase 2 | DSP effects (EQ, crossover, delay, trim, phase inversion) |
| Phase 3 | Room measurement and correction (mic input, RTA, impulse response) |
| Phase 4 | PGM right channel hardware output (dedicated DAC channel for linked speaker with onboard DSP/room correction) |
| Phase 5 | Test mode isolation (per-channel test injection with suspended PGM routing, dedicated monitoring) |
