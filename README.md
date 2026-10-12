# Pi Audio — Monorepo

Single repository for the Pi Audio DSP engine and web interface.

## Setup

### Clone

```bash
git clone https://github.com/LucasTippelt/pi-audio.git
cd pi-audio
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
| `testing` | Experimental features, pre-production testing |

```bash
# Switch branches
git fetch origin
git checkout testing
git checkout master

# Pull / push
git pull origin <branch>
git push origin <branch>

# Merge testing into master (when ready)
git checkout master
git merge testing
git push origin master
```

### Team collaboration

**Single developer (Pi as dev + deploy host):** VS Code connects to the Pi via SSH, editing files directly on the device.

**Multiple developers:** Each developer clones the repo locally, works on `testing`, and pushes to GitHub. The Pi pulls from `testing` to deploy, avoiding concurrent editing conflicts.

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
│   ├── overrides/              # VM systemd overrides
│   ├── deploy.sh               # Production deploy
│   ├── deploy-vm.sh            # VM deploy
│   ├── redeploy.sh             # Quick redeploy
│   └── pi-audio-engine.service
├── pi-audio-web/               # Web interface (PHP, Apache)
│   ├── api.php
│   ├── index.php
│   ├── debug.php
│   └── assets/
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
cd /var/www/pi-audio/pi-audio-dsp
sudo bash deploy-vm.sh
```

Uses the current user instead of `pi:audio`, installs the VM systemd override, and configures shared runtime directories.

## Services

- **pi-audio-engine** — Main audio engine (systemd service)
- **Apache** — Web interface on port 80

## Engine Communication

The engine and web UI communicate through files in `/run/pi-audio/`:

- **`status`** — Engine running flag
- **`engine-state.json`** — Current engine parameters (written every second)
- **`reload`** — Reload trigger (written by apply adapter, consumed by engine)

## Config

- `/etc/pi-audio/config/current.json` — Active configuration
- `/etc/pi-audio/presets/` — Saved presets

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
```

## Roadmap

| Phase | Features |
|---|---|
| Phase 1 | ALSA PCM playback, volume/mute, sample rate/buffer size, web interface adapters |
| Phase 2 | DSP effects (EQ, crossover, delay) |
| Phase 3 | Room measurement and correction |
| Phase 4 | Multi-channel support (4 outputs, 6 inputs) |
