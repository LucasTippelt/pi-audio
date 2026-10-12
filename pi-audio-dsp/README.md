# Pi Audio DSP Engine

A real-time audio engine and DSP pipeline for Raspberry Pi with ALSA support.

## Requirements

- **CMake** 3.10+
- **GCC** (C++17 support)
- **libasound2-dev** (ALSA development headers)
- **pkg-config**

No external JSON library is required — a bundled header-only JSON implementation is included in `src/include/utils/json.hpp`.

## Quick Start

### Clone on Raspberry Pi

```bash
sudo mkdir -p /opt/pi-audio-dsp
sudo git clone https://github.com/LucasTippelt/pi-audio-dsp/ /opt/pi-audio-dsp
sudo chown -R pi:pi /opt/pi-audio-dsp
cd /opt/pi-audio-dsp
```

### VS Code Remote Setup

1. Install the **Remote - SSH** extension in VS Code on your workstation.
2. Connect to your Pi: `Ctrl+Shift+P` → `Remote-SSH: Connect to Host`.
3. Open folder: `/opt/pi-audio-dsp`.

### Remote AI Development (Optional)

Set up Kilo Code on the Pi with a local LLM hosted on a workstation (e.g., Windows with a dedicated GPU).

**Prerequisites:** A local LLM running an OpenAI-compatible API server on your workstation.

1. With VS Code connected to the Pi, install **Kilo Code** for the remote host (click **Install in SSH: [Your-Pi-Name]**).
2. In Kilo Code settings on the Pi, set the Custom OpenAI-Compatible Base URL to your workstation's LLM endpoint:
   ```
   http://<workstation-ip>:8080/v1
   ```
3. Install Node.js and npm on the Pi so extension tool dependencies (`npx`) are available:
   ```bash
   sudo apt update && sudo apt install -y nodejs npm
   ```
4. Reload VS Code (**Developer: Reload Window**) to apply PATH changes.

### Branch Management

The project uses two branches:

- **`main`** — Stable, production-ready code deployed to the Pi.
- **`staging`** — Active development branch for testing changes before merging.

**Create and push the staging branch (first time only):**
```bash
git checkout -b staging
git push -u origin staging
```

**Switch between branches:**
```bash
# From main → staging
git checkout staging
git pull

# From staging → main
git checkout main
git pull
```

**Merge staging into main (when ready):**
```bash
git checkout main
git merge staging
git push origin main
```

### Team Collaboration

**Current setup:** VS Code connects to the Pi via SSH, editing files directly on the device. The Pi is both the development and deployment host.

**Commit workflow (from the Pi):**
```bash
# Always pull before committing to avoid conflicts
git checkout staging
git pull
# ... make changes in VS Code ...
git add -A
git commit -m "describe change"
git push origin staging
```


**Multiple Developers:** Each developer clones the repo on their own machine, works locally, and pushes to GitHub. The Pi then pulls from `staging` to deploy — eliminating concurrent editing conflicts entirely.

**VPN:** If the Pi is at a remote location, a VPN tunnel (e.g., WireGuard, Tailscale) gives secure SSH access for debugging for all team members without exposing ports on your router.

### Debugging on the Pi (SSH Session)

**Build debugging:**
```bash
# Clean rebuild to catch stale artifacts
rm -rf build && mkdir build && cd build
cmake .. 2>&1 | tee cmake.log
make 2>&1 | tee build.log

# Check CMake output for missing dependencies or config errors
cat cmake.log
```

**Verify binaries exist and are executable:**
```bash
ls -la /usr/local/bin/pi-audio-*
file /usr/local/bin/pi-audio-engine
```

**Run engine manually (foreground) to see output:**
```bash
# Stop the service first
sudo systemctl stop pi-audio-engine

# Run manually to see stdout/stderr
sudo /usr/local/bin/pi-audio-engine
```

**Check service status and logs:**
```bash
sudo systemctl status pi-audio-engine
sudo journalctl -u pi-audio-engine -f   # live log stream
sudo journalctl -u pi-audio-engine --since "10 minutes ago"
```

**Check audio device availability:**
```bash
aplay -l    # list playback devices
arecord -l  # list capture devices
```

**VS Code integration:** Use the integrated terminal in your SSH session for all of the above. The **CMake Tools** extension (installed on the remote) provides build task integration and error squiggles in source files.

```bash
# Install dependencies
sudo apt install -y cmake pkg-config libasound2-dev

# Build
mkdir build && cd build
cmake ..
make
sudo make install

# Deploy
sudo ../deploy.sh
sudo systemctl start pi-audio-engine
```

## Architecture

The project is structured into three main components:

- **Core Library**: Audio engine, configuration management, and logging
- **Adapters**: Web interface integration binaries
- **Binaries**: Executable programs

## Project Layout

```
/opt/pi-audio-dsp/          # DSP engine source repo
/var/www/pi-audio/          # Web app (separate repo)
/etc/pi-audio/config/       # Shared configuration
/usr/local/bin/pi-audio-*   # Installed binaries
```

## Binaries

- `pi-audio-engine`: Main audio engine service
- `pi-audio-apply`: Apply configuration adapter
- `pi-audio-status`: Status probe adapter
- `pi-audio-measure`: Room measurement adapter
- `pi-audio-test`: Speaker test adapter

## Configuration

The engine reads configuration from `/etc/pi-audio/config/current.json`. See `AUDIO_ENGINE_INTEGRATION.md` for the complete configuration schema.

## Logging

Logs are written to `/var/log/pi-audio/` by default. The log level can be adjusted in the source code.

## Phase 1 Features

- ALSA PCM playback
- Volume and mute control
- Sample rate and buffer size configuration
- Web interface integration via adapters
- Debug logging

## Future Phases

- Phase 2: DSP effects (EQ, crossover, delay, etc.)
- Phase 3: Room measurement and correction
- Phase 4: Multi-channel support (4 outputs, 6 inputs)
