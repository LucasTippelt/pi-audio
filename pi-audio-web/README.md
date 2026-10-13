# Pi Audio Web Interface

Web control panel for the Pi Audio DSP engine. Part of the [pi-audio](..) monorepo.

## Files

| File | Purpose |
|---|---|
| `api.php` | REST API — state management, presets, DSP commands |
| `index.php` | Main UI (HTML + JS) |
| `debug.php` | Debug page — env vars, permissions, binary runtime status, monitor ring buffers, engine state vs config |
| `audio-debug.html` | Real-time audio monitor — fetches chunks via Web Audio API, waveform visualization |
| `audio-stream.php` | Legacy streaming endpoint (long-lived connection, prone to disconnects) |
| `audio-chunk.php` | Chunked audio endpoint — fixed-size chunks per request, reliable |
| `assets/` | CSS, JavaScript, static assets |

## API Endpoints

All endpoints handled by `api.php` via `action` query parameter.

| Action | Method | Description |
|---|---|---|
| `?action=state` | GET | Load current state, presets, ALSA info |
| `?action=metrics` | GET | CPU, temperature, xruns |
| `?action=dsp-status` | GET | DSP engine connection status |
| `save` | POST | Save preset (staging or production) |
| `master` | POST | Update master volume/mute |
| `apply` | POST | Push config to running engine |
| `test-speaker` | POST | Run speaker test |
| `preset-*` | POST | Preset CRUD operations |

## Audio Chunk Endpoint

`audio-chunk.php` returns fixed-size audio chunks from monitor ring buffers. Replaces long-lived streaming for reliability.

| Parameter | Default | Description |
|---|---|---|
| `channel` | `pgm-l` | Monitor channel (pgm-l, pgm-r, woofer-l/r, tweeter-l/r, mic) |
| `duration` | `1.5` | Seconds of audio per chunk |
| `format` | `raw` | `raw` (float32) or `base64` |
| `engineRate` | auto | Engine sample rate from `dsp-status` |

The `audio-debug.html` JS fetches chunks in a loop, queueing them for Web Audio playback.

## Environment Variables

Set in Apache virtual host config (`/etc/apache2/sites-enabled/pi-audio.conf`):

| Variable | Value | Required |
|---|---|---|
| `PI_AUDIO_STATUS_BIN` | `/usr/local/bin/pi-audio-status` | Yes |
| `PI_AUDIO_APPLY_BIN` | `/usr/local/bin/pi-audio-apply` | Yes |
| `PI_AUDIO_MEASURE_BIN` | `/usr/local/bin/pi-audio-measure` | Optional |
| `PI_AUDIO_TEST_BIN_*` | `/usr/local/bin/pi-audio-test` | Optional |

## Configuration Paths

| Path | Purpose |
|---|---|
| `/etc/pi-audio/config/current.json` | Active configuration |
| `/etc/pi-audio/presets/` | Saved presets |
| `/run/pi-audio/status` | Engine status file |
| `/run/pi-audio/engine-state.json` | Engine parameters (written every second) |
| `/run/pi-audio/reload` | Reload trigger |
| `/run/pi-audio/monitor-{channel}.raw` | Per-channel monitor ring buffers (4MB each) |

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
sudo chown -R $USER:audio /run/pi-audio/
sudo chown -R $USER:audio /var/log/pi-audio/
sudo chmod 664 /etc/pi-audio/config/current.json
sudo usermod -aG audio www-data
sudo usermod -aG audio $USER
```

## Debug Page

Access at `http://<ip>/debug.php` (auto-refreshes every 5 seconds):

- Environment variables, PHP/server info
- Configuration files, current config, adapter binaries
- Binary runtime status (PIDs, last run time, success/error)
- Monitor ring buffer headers (magic, sample rate, channels, write position, usage %)
- ALSA devices, DSP connection status
- Engine state vs config comparison
- Systemd service status and journal output
- Saved presets, file permissions

## Audio Debug Page

Access at `http://<ip>/audio-debug.html`:

- Fetches 1.5s audio chunks via `audio-chunk.php`
- Web Audio playback with `AudioBufferSourceNode` scheduling (500ms target queue)
- Canvas waveform visualization with scroll-and-fade animation
- Channel selection (PGM L/R, woofer/tweeter L/R, mic)
- Auto sample rate detection from `dsp-status`
- Metrics: sample rate, queue level, RMS level, buffers queued, drop count

## Console Logging

Browser console logs with `[pi-audio]` prefix:
- `[pi-audio] API error (action):` — Failed API calls
- `[pi-audio] DSP status error:` — DSP status probe failures
