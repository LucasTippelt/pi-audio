# Pi Audio — Monorepo

Single repository for the Pi Audio DSP engine and web interface.

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
