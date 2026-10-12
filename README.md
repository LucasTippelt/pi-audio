# Pi Audio — Monorepo

Single repository for the Pi Audio DSP engine and web interface.

## Structure

```
/var/www/pi-audio/
├── pi-audio-dsp/       # DSP engine (C++, CMake, ALSA)
│   ├── src/
│   ├── CMakeLists.txt
│   ├── deploy.sh
│   └── pi-audio-engine.service
├── pi-audio-web/       # Web interface (PHP, Apache)
│   ├── api.php
│   ├── index.php
│   ├── debug.php
│   └── assets/
└── README.md
```

## Deploy

```bash
cd /var/www/pi-audio/pi-audio-dsp
sudo ./deploy.sh
```

This builds the DSP engine, installs binaries, configures Apache, and sets up the systemd service.

## Services

- **pi-audio-engine** — Main audio engine (systemd service)
- **Apache** — Web interface on port 80

## Config

- `/etc/pi-audio/config/current.json` — Active configuration
- `/etc/pi-audio/presets/` — Saved presets
