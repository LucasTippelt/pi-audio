# Pi Audio Web Interface

PHP/Apache control surface for the Pi Audio DSP engine. Tuning and preset data are stored as JSON under `/etc/pi-audio/config/` and `/etc/pi-audio/presets/`. Part of the [pi-audio](..) monorepo.

## Files

| File | Purpose |
|---|---|
| `api.php` | REST API — state management, presets, DSP commands |
| `index.php` | Main UI (HTML + JS) |
| `debug.php` | Debug page — env vars, permissions, binary runtime status, monitor ring buffers, engine state vs config |
| `audio-debug.html` | Real-time audio monitor — fetches chunks via WebSocket or chunked HTTP |
| `ws-audio-server-node.js` | WebSocket relay server (Node.js, port 8765) |
| `VERSION.json` | Auto-generated version info |
| `audio-stream.php` | Legacy streaming endpoint (long-lived connection, prone to disconnects) |
| `audio-chunk.php` | Chunked audio endpoint — fixed-size chunks per request, reliable |
| `assets/` | CSS, JavaScript, static assets |

## Deploy on Raspberry Pi

Copy the monorepo to `/var/www/pi-audio`, enable Apache's `auth_basic` and `headers` modules, and allow `.htaccess` overrides for the site directory. Enable PHP for Apache:

```sh
sudo apt install apache2 php libapache2-mod-php alsa-utils
sudo a2enmod auth_basic headers
sudo install -d -o root -g www-data -m 0750 /etc/pi-audio
sudo htpasswd -c /etc/pi-audio/htpasswd pi-audio
sudo chown root:www-data /etc/pi-audio/htpasswd
sudo chmod 0640 /etc/pi-audio/htpasswd
sudo install -d -o www-data -g www-data -m 0770 /etc/pi-audio/config /etc/pi-audio/presets
sudo chown -R www-data:www-data /var/www/pi-audio
```

Save a site configuration at `/etc/apache2/sites-available/pi-audio.conf`, then enable it with `sudo a2ensite pi-audio.conf`, disable the default site with `sudo a2dissite 000-default.conf` and reload Apache:

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
    SetEnv PI_AUDIO_XRUN_FILE /run/pi-audio/xruns
</VirtualHost>
```

Username for web login is `pi-audio`.

### Apache systemd sandbox

Apache's systemd sandbox must permit the PHP worker to inspect ALSA and open the mixer control. Create an override with `sudo systemctl edit apache2`:

```ini
[Service]
ReadWritePaths=/etc/pi-audio/config /etc/pi-audio/presets
PrivateDevices=no
ProcSubset=all
DeviceAllow=/dev/snd/controlC0 rw
```

Then apply it:

```sh
sudo systemctl daemon-reload
sudo systemctl restart apache2
sudo systemctl show apache2 -p PrivateDevices -p DevicePolicy -p DeviceAllow -p ProcSubset
```

These settings reduce Apache's device and `/proc` isolation. Allow only the ALSA control device the service needs, and update `DeviceAllow` if the ALSA card number changes. Keep other sandbox protections enabled.

### ALSA access troubleshooting

Check that Linux sees the card and that the web user can use its mixer:

```sh
cat /proc/asound/cards
sudo -u www-data amixer -c 0 scontrols
sudo -u www-data amixer -c 0 sget Master
sudo -u www-data amixer scontrols
ls -l /dev/snd
id www-data
```

If the `www-data` mixer commands fail, check `/dev/snd` ownership and add `www-data` to the device's group:

```sh
sudo usermod -aG audio www-data
sudo systemctl restart apache2
sudo -u www-data amixer -c 0 scontrols
```

If those commands work from the shell but the web app reports ALSA unavailable, inspect Apache's unit:

```sh
sudo apachectl -M | grep -E 'php|proxy_fcgi'
sudo systemctl show apache2 \
  -p PrivateDevices -p DevicePolicy -p DeviceAllow \
  -p SupplementaryGroups -p ProtectProc -p ProcSubset \
  -p InaccessiblePaths -p RootDirectory
sudo systemctl cat apache2
```

For mod_php, `ProcSubset=all` and `DeviceAllow=/dev/snd/controlC0 rw` in the Apache override are required for mixer detection.

## WebSocket Server Setup

```bash
# Install Node.js (18+)
curl -fsSL https://deb.nodesource.com/setup_18.x | sudo -E bash -
sudo apt install -y nodejs

# Install WebSocket server dependencies
cd /var/www/pi-audio/pi-audio-web
npm install ws

# Install service file
sudo cp /var/www/pi-audio/pi-audio-dsp/pi-audio-ws-server.service /etc/systemd/system/

# Reload systemd and start
sudo systemctl daemon-reload
sudo systemctl enable pi-audio-ws-server
sudo systemctl start pi-audio-ws-server

# Check status
sudo systemctl status pi-audio-ws-server
sudo journalctl -u pi-audio-ws-server -f
```

## DSP Integration

`current.json` is atomically replaced after updates. Its fields are `input`, `output`, `sampleRate`, `volume`, `muted`, `crossovers` (independent `frequency`, `topology`, and `slope` settings for each driver), `bands` (per-driver PEQ arrays), `drivers` (`name`, `delayMs`, `trimDb`, `phaseInverted`), `limiter` (`enabled`, `threshold`, `release`), optional `measurement` data, and optional `roomCorrection` parameters.

### Applying configuration to the audio engine

The **Apply to engine** button saves the current configuration and invokes an administrator-installed adapter. Configure `PI_AUDIO_APPLY_BIN` in Apache's environment. The path must name an executable accessible to Apache.

The app invokes the adapter directly (without a shell), sends the normalized active configuration as JSON on stdin, and allows up to 30 seconds for it to finish. The adapter should exit with status `0` on success. On failure, exit nonzero and write a diagnostic to stderr.

After setting or changing the adapter, reload Apache:

```sh
sudo systemctl reload apache2
sudo -u www-data test -x /usr/local/bin/pi-audio-apply
```

Capture input selection, master volume/mute changes, and preset loads invoke the adapter automatically. Other tuning changes require **Apply to engine**.

The header's **DSP connection** indicator requires `PI_AUDIO_STATUS_BIN`; it polls the configured status adapter every three seconds.

### Testing a speaker

Configure the corresponding Apache environment variable for each adapter. Each adapter receives a JSON object on stdin containing its selected `driver` and current playback `output`. It must emit a short, low-level test signal only on that selected speaker.

```sh
sudo install -o root -g root -m 0755 pi-audio-test /usr/local/bin/pi-audio-test
sudo -u www-data test -x /usr/local/bin/pi-audio-test
sudo systemctl reload apache2
```

### Room measurement

Set `PI_AUDIO_MEASURE_BIN` in Apache's environment to an executable DSP/measurement adapter. The adapter receives the current configuration as JSON on stdin, runs its calibrated measurement workflow, and writes JSON to stdout with `responses`, and optional `autoconfiguration` (`crossovers`, `bands`, `drivers`).

### Live metrics

Metrics use sampled CPU counters from `/proc/stat`, `/sys/class/thermal/thermal_zone0/temp`, and the integer in `/run/pi-audio/xruns`. Set `PI_AUDIO_XRUN_FILE` to override the path.

To verify CPU monitoring, install `stress-ng` and run a short load test:

```sh
sudo apt install stress-ng
stress-ng --cpu 0 --timeout 30s --metrics-brief
```

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

## Environment Variables

Set in Apache virtual host config (`/etc/apache2/sites-enabled/pi-audio.conf`):

| Variable | Value | Required |
|---|---|---|
| `PI_AUDIO_STATUS_BIN` | `/usr/local/bin/pi-audio-status` | Yes |
| `PI_AUDIO_APPLY_BIN` | `/usr/local/bin/pi-audio-apply` | Yes |
| `PI_AUDIO_MEASURE_BIN` | `/usr/local/bin/pi-audio-measure` | Optional |
| `PI_AUDIO_TEST_BIN_*` | `/usr/local/bin/pi-audio-test` | Optional |
| `PI_AUDIO_CONFIG_DIR` | `/etc/pi-audio/config` | Optional |
| `PI_AUDIO_PRESETS_DIR` | `/etc/pi-audio/presets` | Optional |
| `PI_AUDIO_CARD` | `0` | Optional |
| `PI_AUDIO_CONTROL` | `PCM` | Optional |
| `PI_AUDIO_XRUN_FILE` | `/run/pi-audio/xruns` | Optional |

## Configuration Paths

| Path | Purpose |
|---|---|
| `/etc/pi-audio/config/current.json` | Active configuration |
| `/etc/pi-audio/presets/` | Saved presets |
| `/run/pi-audio/status` | Engine status file |
| `/run/pi-audio/engine-state.json` | Engine parameters (written every second) |
| `/run/pi-audio/reload` | Reload trigger |
| `/run/pi-audio/monitor-{channel}.raw` | Per-channel monitor ring buffers (4MB each) |

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

- WebSocket connection to `ws-audio-server-node.js` (port 8765)
- 200ms ring buffer (9,600 samples) with 50ms warmup threshold
- Web Audio playback via ScriptProcessorNode (bufSize=256) for low-latency callback
- Canvas waveform visualization with `copyWithin` to avoid GC storms
- Per-channel selection (PGM L/R, woofer/tweeter L/R, mic) with channel enable/disable control
- Auto sample rate detection from `dsp-status`
- Metrics: sample rate, queue level, RMS level, buffers queued, drop count, callback drop rate, events, available samples
- WebSocket rate logging (5s intervals)

## Security

Keep this service on a trusted LAN, use a strong Basic Auth password, and use HTTPS if credentials or controls cross an untrusted network. Do not expose Apache directly to the public internet.

## Console Logging

Browser console logs with `[pi-audio]` prefix:
- `[pi-audio] API error (action):` — Failed API calls
- `[pi-audio] DSP status error:` — DSP status probe failures