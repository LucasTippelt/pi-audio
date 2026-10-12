# Pi Audio Web Interface

Web control panel for the Pi Audio DSP engine. Part of the [pi-audio](..) monorepo.

## Files

| File | Purpose |
|---|---|
| `api.php` | REST API — state management, presets, DSP commands |
| `index.php` | Main UI (HTML + JS) |
| `debug.php` | Debug page — env vars, permissions, engine state comparison, auto-refresh |
| `assets/` | CSS, JavaScript, static assets |

## API Endpoints

All endpoints are handled by `api.php` via the `action` query parameter.

| Action | Method | Description |
|---|---|---|
| `?action=state` | GET | Load current state, presets, ALSA info |
| `?action=metrics` | GET | CPU, temperature, xruns |
| `?action=dsp-status` | GET | DSP engine connection status (reads engine state file) |
| `save` | POST | Save preset (staging or production) |
| `master` | POST | Update master volume/mute |
| `apply` | POST | Push config to running engine (writes reload trigger) |
| `test-speaker` | POST | Run speaker test |
| `preset-*` | POST | Preset CRUD operations |

## Environment Variables

Set in Apache virtual host config (`/etc/apache2/sites-enabled/pi-audio.conf`):

| Variable | Value | Required |
|---|---|---|
| `PI_AUDIO_STATUS_BIN` | `/usr/local/bin/pi-audio-status` | Yes |
| `PI_AUDIO_APPLY_BIN` | `/usr/local/bin/pi-audio-apply` | Yes |
| `PI_AUDIO_MEASURE_BIN` | `/usr/local/bin/pi-audio-measure` | Optional |
| `PI_AUDIO_TEST_BIN_WOOFER_L` | `/usr/local/bin/pi-audio-test` | Optional |
| `PI_AUDIO_TEST_BIN_WOOFER_R` | `/usr/local/bin/pi-audio-test` | Optional |
| `PI_AUDIO_TEST_BIN_TWEETER_L` | `/usr/local/bin/pi-audio-test` | Optional |
| `PI_AUDIO_TEST_BIN_TWEETER_R` | `/usr/local/bin/pi-audio-test` | Optional |

## Configuration Paths

| Path | Purpose |
|---|---|
| `/etc/pi-audio/config/current.json` | Active configuration |
| `/etc/pi-audio/presets/` | Saved presets |
| `/run/pi-audio/status` | Engine status file (created by engine) |
| `/run/pi-audio/engine-state.json` | Engine parameters (written every second) |
| `/run/pi-audio/reload` | Reload trigger (written by apply adapter) |

## Apache Setup

```apache
<VirtualHost *:80>
    DocumentRoot /var/www/pi-audio/pi-audio-web
    <Directory /var/www/pi-audio/pi-audio-web>
        AllowOverride All
        Require all granted
    </Directory>
    SetEnv PI_AUDIO_STATUS_BIN /usr/local/bin/pi-audio-status
    SetEnv PI_AUDIO_APPLY_BIN /usr/local/bin/pi-audio-apply
    # ... other adapter paths ...
</VirtualHost>
```

## Permissions

### Production

```bash
sudo usermod -aG audio www-data
sudo chown -R pi:audio /etc/pi-audio/
sudo chmod -R 775 /etc/pi-audio/
```

### VM / Development

```bash
# Engine user must own runtime directories
sudo chown -R $USER:audio /run/pi-audio/
sudo chown -R $USER:audio /var/log/pi-audio/

# Config files must be readable by engine user (664, not 660)
sudo chmod 664 /etc/pi-audio/config/current.json
sudo find /etc/pi-audio/presets -name "*.json" -exec chmod 664 {} \;

# www-data must be in audio group for shared file access
sudo usermod -aG audio www-data
```

## Debug Page

Access at `http://<ip>/debug.php`:

- **Environment variables** — Apache env var configuration
- **PHP / Server info** — PHP version, user, permissions
- **Configuration files** — Paths, writability, status
- **Current config** — Parsed `current.json` contents
- **Adapter binaries** — Existence and executability
- **ALSA devices** — Sound cards, devices, mixer controls
- **DSP connection status** — Live engine connection and parameters
- **Engine state vs config** — Compares `current.json` against actual engine state with match indicators
- **Systemd service** — Service status and journal output
- **Presets** — List of saved presets
- **File permissions** — Detailed permissions for all paths

The debug page auto-refreshes every 5 seconds with a progress bar.

## Console Logging

The web app logs errors to the browser console with `[pi-audio]` prefix. Open DevTools (F12) Console to see:

- `[pi-audio] API error (action):` — Failed API calls with error details
- `[pi-audio] DSP status error:` — DSP status probe failures
- `[pi-audio] DSP status response:` — DSP status probe responses
