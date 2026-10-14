# Pi Audio Web Interface

PHP/Apache control surface for the Pi Audio DSP engine. Single-speaker system with tweeter/woofer outputs, configurable 2-6 mic inputs. Tuning and preset data stored as JSON under `/etc/pi-audio/config/` and `/etc/pi-audio/presets/`. Part of the [pi-audio](..) monorepo.

## Files

| File | Purpose |
|---|---|
| `api.php` | REST API — state management, presets, DSP commands |
| `index.php` | Main UI (HTML + JS) |
| `debug.php` | Debug page — env vars, permissions, binary status, per-channel monitor ring buffers (shared memory), engine state vs config |
| `audio-debug.html` | Real-time audio monitor — HTTP stream from engine (port 8767), gapless scheduling |
| `ws-audio-server-node.js` | WebSocket relay server (Node.js, port 8765, legacy/optional) |
| `VERSION.json` | Auto-generated version info |
| `assets/` | CSS, JavaScript, static assets |

## Deploy on Raspberry Pi

See the [root README](../README.md#quick-install) for full installation instructions.

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

## DSP Integration

`current.json` is atomically replaced after updates. Its fields are `input`, `output`, `sampleRate`, `BufferSize`, `micCount`, `volume`, `muted`, `crossovers` (per-output: `frequency`, `topology`, `slope`), `bands` (per-output PEQ arrays), `drivers` (`name`, `delayMs`, `trimDb`, `phaseInverted`), `limiter` (`enabled`, `threshold`, `release`), `monitorChannel`, optional `measurement`.

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

```sh
sudo install -o root -g root -m 0755 pi-audio-test /usr/local/bin/pi-audio-test
sudo systemctl reload apache2
```

Supported drivers: `Tweeter`, `Woofer`, `PGM_L`, `PGM_R`, `Mic_1`..`Mic_6`.

**Test mode routing:**
- **Tweeter test**: signal plays through tweeter DSP only, woofer silent, PGM suspended
- **Woofer test**: signal plays through woofer DSP only, tweeter silent, PGM suspended
- **PGM_L test**: no physical speakers, PGM monitoring shows raw input
- **PGM_R test**: no physical speakers, PGM monitoring shows raw input

The engine detects test mode via `/dev/shm/pi-audio-test-mode` control file. The test tool signals the engine before writing data and clears the mode after consumption.

Each driver requires a matching `PI_AUDIO_TEST_BIN_<DRIVER>` environment variable in Apache config (e.g. `PI_AUDIO_TEST_BIN_TWEETER`). The `test-pgm` action uses `PI_AUDIO_TEST_BIN_PGM_L` or falls back to `/usr/local/bin/pi-audio-test`.

**Troubleshooting:** If tests fail with `"write failed"`, stale shared memory from a previous `sudo` run may block creation. Clean with: `sudo rm -f /dev/shm/pi-audio-test-* /dev/shm/pi-audio-test-mode`

### Room measurement

Set `PI_AUDIO_MEASURE_BIN` in Apache's environment to an executable measurement adapter (not yet implemented).

### Live metrics

Metrics use sampled CPU counters from `/proc/stat`, `/sys/class/thermal/thermal_zone0/temp`, and the integer in `/run/pi-audio/xruns`. Set `PI_AUDIO_XRUN_FILE` to override the path.

### Shared memory ring buffers

The engine uses POSIX shared memory (`shm_open` + `mmap`) instead of file-based ring buffers for all audio data transfer. Ring buffers are visible at `/dev/shm/pi-audio-*`. External writers (test adapter, external sources) access the same shared memory objects.

Test injection uses per-channel shared memory buffers (`pi-audio-test-tweeter`, `pi-audio-test-woofer`) with a 12-byte `PIIJ` header (`magic[4]`, `frameCount[4]`, `readPos[4]`) followed by raw float32 data. The engine reads frames and advances `readPos`, resetting both to 0 when fully consumed. A control file (`pi-audio-test-mode`) signals the active test type to the engine.

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
| `test-speaker` | POST | Run speaker test (tweeter/woofer/mic) |
| `test-pgm` | POST | Run PGM_L input test (monitoring only, no speakers) |
| `preset-*` | POST | Preset CRUD operations |

## HTTP Stream Monitoring (Recommended)

The engine's built-in HTTP stream server (port 8767) serves real-time audio directly to the browser. No Node.js relay required.

| Endpoint | Description |
|---|---|
| `GET /stream?ch=&lt;channel&gt;` | Raw float32LE mono, HTTP chunked transfer encoding |
| `GET /status` | JSON: sampleRate, BufferSize, volume, muted |
| `GET /` | Channel list (JSON array) |

Channels: `pgm-l`, `pgm-r`, `tweeter`, `woofer`, `mic-1`..`mic-6`.

## WebSocket Monitoring (Legacy)

The TCP relay (port 8766) + Node.js WebSocket server (port 8765) path still works but is deprecated.

## Environment Variables

Set in Apache virtual host config (`/etc/apache2/sites-enabled/pi-audio.conf`):

| Variable | Value | Required |
|---|---|---|
| `PI_AUDIO_STATUS_BIN` | `/usr/local/bin/pi-audio-status` | Yes |
| `PI_AUDIO_APPLY_BIN` | `/usr/local/bin/pi-audio-apply` | Yes |
| `PI_AUDIO_TEST_BIN_PGM_L` | `/usr/local/bin/pi-audio-test` | For PGM test |
| `PI_AUDIO_TEST_BIN_PGM_R` | `/usr/local/bin/pi-audio-test` | For PGM R test |
| `PI_AUDIO_TEST_BIN_TWEETER` | `/usr/local/bin/pi-audio-test` | For Tweeter test |
| `PI_AUDIO_TEST_BIN_WOOFER` | `/usr/local/bin/pi-audio-test` | For Woofer test |
| `PI_AUDIO_TEST_BIN_MIC_1`..`MIC_6` | `/usr/local/bin/pi-audio-test` | For mic tests |
| `PI_AUDIO_MEASURE_BIN` | `/usr/local/bin/pi-audio-measure` | Optional |
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
| `/dev/shm/pi-audio-monitor-{channel}` | Per-channel monitor ring buffers (4MB each, POSIX shared memory) |
| `/dev/shm/pi-audio-test-tweeter` | Tweeter test injection buffer (4MB, POSIX shared memory) |
| `/dev/shm/pi-audio-test-woofer` | Woofer test injection buffer (4MB, POSIX shared memory) |
| `/dev/shm/pi-audio-test-mode` | Test mode control file |

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
- Monitor ring buffer headers (shared memory: magic, sample rate, channels, write position, usage %)
- ALSA devices, DSP connection status
- Engine state vs config comparison
- Systemd service status and journal output
- Saved presets, file permissions

## Audio Debug Page

Access at `http://<ip>/audio-debug.html`:

- HTTP stream from engine (port 8767) via `fetch()` + `ReadableStream`
- Gapless audio playback using `AudioBufferSourceNode` scheduling with overlap
- Canvas waveform visualization
- Per-channel selection (PGM L/R, tweeter, woofer, mic-1..mic-6)
- Auto sample rate detection with AudioContext recreation on change
- Metrics: sample rate, queue level, RMS level, buffers queued, drop count, events

### Test Buttons

The web UI provides test buttons for tweeter, woofer, and PGM input. A 1-second cooldown prevents overlapping tests. All test buttons are disabled while any test is running.

## Security

Keep this service on a trusted LAN, use a strong Basic Auth password, and use HTTPS if credentials or controls cross an untrusted network. Do not expose Apache directly to the public internet.

## Console Logging

Browser console logs with `[pi-audio]` prefix:
- `[pi-audio] API error (action):` — Failed API calls
- `[pi-audio] DSP status error:` — DSP status probe failures
