# Audio Engine Integration Handoff

This document describes the contract between the Pi Audio web application and an external DSP/audio engine. The web application is a configuration editor and adapter launcher; it does not contain or implement the audio engine.

## Responsibilities

The audio engine integration must:

1. Read and validate the configuration sent by the web app.
2. Apply the selected capture and playback PCM devices and DSP settings to the running engine.
3. Report success or a useful failure through the adapter process exit status, stderr, or the documented JSON failure response on stdout.
4. Define how the engine starts initially and recovers after a failed update or restart.

The web UI does not directly reconfigure ALSA PCM streams or DSP processing. **Apply current.json to engine** invokes the configured adapter for the state currently written to `current.json`. A selected input/output, crossover, EQ, driver alignment, or limiter change takes effect in the engine only if the adapter and engine implement it.

## User-triggered adapter calls

The web UI invokes three categories of external adapters that the audio engine integration must provide. All are triggered by explicit user action in the UI:

| UI action | Environment variable | Timeout | Purpose |
| --- | --- | --- | --- |
| **Apply current.json to engine** | `PI_AUDIO_APPLY_BIN` | 30s | Send full configuration to the running engine |
| **Measure Room** | `PI_AUDIO_MEASURE_BIN` | 180s | Run room measurement and auto-configure DSP |
| **Test Speaker** (per driver) | `PI_AUDIO_TEST_BIN_WOOFER_L`, `_TWEETER_L`, `_WOOFER_R`, `_TWEETER_R` | 15s each | Play a test signal to a single driver |

A fourth adapter, `PI_AUDIO_STATUS_BIN`, is polled automatically every three seconds (3s timeout) and is not user-triggered.

All adapters are invoked directly without a shell, as the Apache worker user. They receive JSON on stdin and must exit with status `0` on success or nonzero with a diagnostic on stderr on failure. Keep adapter executables and their parent directories writable only by administrators.

## DSP connection status probe

Configure `PI_AUDIO_STATUS_BIN` in Apache's environment with an administrator-installed executable that checks whether the DSP engine is currently connected/running and reports its active sample rate and buffer size:

```apache
SetEnv PI_AUDIO_STATUS_BIN /usr/local/bin/pi-audio-status
```

The app runs this probe without arguments every three seconds, with a three-second timeout. It must print one JSON object to stdout and exit with status `0`, including when the engine is disconnected:

```json
{"connected":true,"sampleRate":48000,"BufferSize":512}
```

`connected` is a required boolean representing the live engine connection, not merely whether the probe or service process exists. `sampleRate` and `BufferSize` are required integers when connected, and both must be `null` when the engine is disconnected. An optional `message` string provides a concise reason/status detail for the UI tooltip, especially when `connected` is false. Supported rates are exactly `44100`, `48000`, `88200`, `96000`, `176400`, and `192000` Hz; supported buffer sizes are exactly `64`, `128`, `256`, `512`, `1024`, `2048`, and `4096` samples. The probe must report the values actually opened by the engine, not just the requested values. On probe failure, return nonzero and write a diagnostic to stderr; malformed JSON, unsupported values, nonzero exit, or timeout is shown as DSP unavailable. The UI keeps these fields in a loading state until the saved configuration and DSP status have loaded, and highlights a requested value red when it could not be applied or does not match the live engine value. Keep this executable and its parent directories administrator-writable only.

## Apply adapter process contract

Configure `PI_AUDIO_APPLY_BIN` in Apache's environment to the absolute path of an administrator-installed executable. For example:

```apache
SetEnv PI_AUDIO_APPLY_BIN /usr/local/bin/pi-audio-apply
```

Configure speaker tests independently per driver in Apache when those adapters are installed:

```apache
SetEnv PI_AUDIO_TEST_BIN_WOOFER_L /usr/local/bin/pi-audio-test-woofer-l
SetEnv PI_AUDIO_TEST_BIN_TWEETER_L /usr/local/bin/pi-audio-test-tweeter-l
SetEnv PI_AUDIO_TEST_BIN_WOOFER_R /usr/local/bin/pi-audio-test-woofer-r
SetEnv PI_AUDIO_TEST_BIN_TWEETER_R /usr/local/bin/pi-audio-test-tweeter-r
```

The app invokes this path directly, without a shell, as the Apache worker user. It sends one UTF-8 JSON object to standard input and allows 30 seconds for the adapter to finish.

| Result | Adapter behavior | Web app behavior |
| --- | --- | --- |
| Success | Apply the supplied configuration and exit with status `0`. Optionally print a JSON object such as `{"ok":true}`. | Reports that the configuration was applied after the DSP status probe confirms the requested sample rate and buffer size. |
| Failure | Exit nonzero and write a concise diagnostic to standard error. | Reports that the configuration was saved but apply failed, including the diagnostic when available. |
| Reported failure | Optionally print `{"ok":false,"message":"reason"}` or `{"error":"reason"}` to standard output. | Treats the apply as failed, even if the process exits with status `0`. |
| Timeout | Finish within 30 seconds; otherwise the app terminates the adapter. | Reports an apply failure. |

Other standard output is ignored; keep routine output quiet and use stderr for actionable process errors. The app saves the normalized configuration to `current.json` before starting the adapter. A failed or timed-out adapter does **not** roll back that file, and the running engine may be unchanged or partially changed. The adapter should therefore validate the complete input before changing live state, apply updates safely (preferably as one transaction or controlled reload), and report partial-apply failures clearly.

The adapter executable and all parent directories must be writable only by trusted administrators. Do not allow web users to choose an executable or pass command-line arguments. The adapter runs with Apache's permissions and must be granted only the access needed to communicate with the audio service and devices. Avoid running the DSP engine itself as the Apache worker.

## Configuration JSON

The adapter receives the complete normalized active state, not a patch. Example:

```json
{
  "input": "default",
  "output": "default",
  "sampleRate": 48000,
  "BufferSize": 512,
  "volume": 65,
  "muted": false,
  "crossovers": {
    "Woofer_L": { "frequency": 80, "topology": "Linkwitz-Riley", "slope": 24 },
    "Tweeter_L": { "frequency": 80, "topology": "Linkwitz-Riley", "slope": 24 },
    "Woofer_R": { "frequency": 80, "topology": "Linkwitz-Riley", "slope": 24 },
    "Tweeter_R": { "frequency": 80, "topology": "Linkwitz-Riley", "slope": 24 }
  },
  "bands": {
    "Woofer_L": [
      {
        "id": "woofer-low-1",
        "enabled": true,
        "frequency": 55,
        "q": 3.2,
        "gain": -4.0
      }
    ],
    "Tweeter_L": [],
    "Woofer_R": [],
    "Tweeter_R": []
  },
  "drivers": [
    { "name": "Woofer_L", "delayMs": 0, "trimDb": 0, "phaseInverted": false },
    { "name": "Tweeter_L", "delayMs": 0, "trimDb": 0, "phaseInverted": false },
    { "name": "Woofer_R", "delayMs": 0, "trimDb": 0, "phaseInverted": false },
    { "name": "Tweeter_R", "delayMs": 0, "trimDb": 0, "phaseInverted": false }
  ],
  "limiter": {
    "enabled": true,
    "threshold": -1,
    "release": 80
  },
  "measurement": {
    "measuredAt": "2026-10-10T12:00:00Z",
    "responses": {
      "left": [{ "hz": 20, "db": -4.2 }, { "hz": 20000, "db": -2.0 }],
      "right": [{ "hz": 20, "db": -3.8 }, { "hz": 20000, "db": -2.3 }]
    }
  }
}
```

Fields and application semantics:

| Field | Meaning |
| --- | --- |
| `input` | ALSA capture PCM name from `arecord -L`, or `default`. The engine should open this as its capture source. |
| `output` | ALSA playback PCM name from `aplay -L`, or `default`. The engine should open this as its playback destination. |
| `sampleRate` | Requested stream rate in Hz. The Audio input selector supports exactly `44100`, `48000`, `88200`, `96000`, `176400`, or `192000`; the adapter/engine must validate the selected PCM supports it and fail clearly rather than silently substituting another rate. The DSP status probe reports the rate actually active. |
| `BufferSize` | Requested audio buffer size in samples. The selector supports exactly `64`, `128`, `256`, `512`, `1024`, `2048`, or `4096`; each is below every supported sample rate. The adapter/engine must validate the selected device supports the requested size and report failures clearly. |
| `volume` | Master level from 0 to 100. |
| `muted` | Master mute state. |
| `crossovers` | Four independent crossover objects keyed by `Woofer_L`, `Tweeter_L`, `Woofer_R`, and `Tweeter_R`. A woofer is the low-pass output and a tweeter is the high-pass output. The UI edits each Left/Right pair with the channel selector. |
| `crossovers.<driver>.frequency` | Crossover frequency in Hz, 30–500. |
| `crossovers.<driver>.topology` | `Linkwitz-Riley`, `Butterworth`, or `Bessel`. |
| `crossovers.<driver>.slope` | Slope in dB/octave: 12, 18, 24, 36, or 48. |
| `bands` | Object keyed by `Woofer_L`, `Tweeter_L`, `Woofer_R`, and `Tweeter_R`; each driver has up to 12 independent parametric-EQ bands. Each band has an `id`, `enabled`, center `frequency` in Hz (20–20000), `q` (0.1–18), and `gain` in dB (-18–18). The UI's Left/Right selector displays the selected channel's Woofer and Tweeter PEQ side by side. A legacy flat band array is migrated as a copy to all four drivers. |
| `drivers` | Four output alignment entries (`Woofer_L`, `Tweeter_L`, `Woofer_R`, `Tweeter_R`), with `delayMs` (0–20 ms), `trimDb` (-24–12 dB), and `phaseInverted` (boolean; `true` reverses polarity, a 180° phase flip). Missing `phaseInverted` values default to `false` for older configurations. |
| `limiter` | `enabled`, `threshold` in dBFS (-24–0), and `release` in ms (10–2000). |
| `measurement` | `null` until room calibration data exists; otherwise contains `measuredAt` and independent `responses.left` / `responses.right` arrays of `{hz, db}` points. Both channel curves are always displayed in vertically stacked plots.|
| `roomCorrection` | Optional array of engine-defined room-correction filter parameters. The web app preserves it when saving the active configuration and when saving/loading presets; the engine or adapter validates and interprets its contents. Factory defaults omit this field. |

The API normalizes values before saving or invoking the adapter. Treat these bounds as the supported UI contract, and still validate input in the adapter. Unknown fields may be omitted by future versions; ignore fields the engine does not support or reject them with a clear diagnostic rather than silently claiming all settings were applied.

The UI's crossover + PEQ curve is only a filter visualization: it approximates each driver's PEQ and low/high-pass magnitude response, then combines the driver magnitudes. It does not include driver/acoustic response or crossover phase, and should not be treated as an engine or room measurement.

### Master volume and mute

The web app's master-volume slider and mute button save `volume`/`muted` to `current.json` and attempt to change the detected ALSA playback mixer directly through `amixer`; they do not update the Staging snapshot automatically. Tuning edits do not invoke `PI_AUDIO_APPLY_BIN` automatically. Edits write `current.json` after a short debounce, and the footer reports the pending state until the API verifies the write. Use **Save staging** to persist Staging tuning and **Apply current.json to engine** to apply the working configuration. Coordinate engine software gain/mute with the ALSA mixer behavior to avoid unintended double attenuation or conflicting mute states. If the engine owns master gain instead of ALSA, that is an engine-specific deployment choice; ensure the UI controls are wired to that design.

## Configuration lifecycle and adapter triggers

- The app ensures a protected **Default** preset exists, with independent staging and production snapshots. Existing legacy one-state presets are migrated to identical staging and production snapshots when accessed.
- Ordinary setting edits are saved to `/etc/pi-audio/config/current.json` (or `PI_AUDIO_CONFIG_DIR`) after a short debounce. They do not update the Staging snapshot; use **Save staging** to persist the current tuning to Staging. Production input, sample rate, and buffer size edits are persisted to the selected Production snapshot. Saving alone does not invoke the engine adapter.
- The preset action button reads **Save staging** in Staging view and **Save production** in Production view. Saving Production persists its configurable input, sample rate, and buffer size; it does not apply those values to the engine.
- Production tuning is a read-only snapshot. Input, sample rate, and buffer size remain configurable there. Select a preset's staging version to edit tuning; **Promote to production** copies staging to production only after the user explicitly approves it.
- **Copy production to staging** replaces the selected preset's staging snapshot with its production snapshot and writes it to `current.json`. It does not invoke the adapter.
- Switching preset versions loads the selected snapshot into the UI and writes it to `current.json`; switching does not invoke `PI_AUDIO_APPLY_BIN`.
- **Apply current.json to engine** is the only UI action that invokes `PI_AUDIO_APPLY_BIN`, sending the state currently written to `current.json`. Its description identifies the selected snapshot or unsaved state; the header displays load time/source on a separate line and records failed apply attempts in **Active** (while retaining the last successful state when one exists). **Viewing** identifies the selected saved snapshot or unsaved `current.json`.
- Changing capture input, requested sample rate, or requested buffer size does not invoke `PI_AUDIO_APPLY_BIN`; only **Apply current.json to engine** sends configuration to the engine.
- Changing capture input, requested sample rate, or requested buffer size writes `current.json` and does not invoke the adapter. When Production is selected, those three fields are also saved to the Production snapshot; use **Save production** to explicitly save them. In Staging, press **Save staging** to update the snapshot.
- The `PI_AUDIO_STATUS_BIN` probe independently reports live connection state and the actual active sample rate and buffer size to the header and controls; it is polled every three seconds and does not apply configuration.
- Changing volume or mute writes `current.json` and attempts to apply the ALSA mixer setting; it does not update Staging or invoke the engine adapter.
- Selecting a staging or production version loads that snapshot in the UI and writes it to `current.json`. Only staging tuning can be edited; input, sample rate, and buffer size can also be changed in production.
- Saving a new preset creates its staging and production snapshots from the current configuration.
- Only the exact preset name `Default` is protected; a lowercase `default` remains a regular user preset. **Delete all except Default** removes all other presets and preserves the protected Default preset.
- **Restore to Factory Default** resets Default staging and `current.json` to the built-in configuration (without optional `roomCorrection` data) and attempts to reset ALSA master volume/mute. It does not invoke the adapter or delete user presets; use **Apply current.json to engine** to apply the reset configuration.
- Changing playback output, crossover settings, room measurement, or other DSP settings writes `current.json` but does **not** automatically update Staging or invoke the adapter; use **Save staging** to save the snapshot and **Apply current.json to engine** to test/use it.
- On audio-engine startup or restart, the engine/service should load the latest `current.json` independently. The adapter is not a substitute for the service's boot-time configuration loading.

The engine or adapter should report unsupported PCM names, device-open failures, invalid DSP capabilities, and communication errors. Do not signal success until the configuration has been accepted by the running engine.

## Measurement adapter

Configure `PI_AUDIO_MEASURE_BIN` in Apache's environment with an administrator-installed executable that performs room measurement and returns autoconfiguration:

```apache
SetEnv PI_AUDIO_MEASURE_BIN /usr/local/bin/pi-audio-measure
```

The app invokes this adapter directly, without a shell, as the Apache worker user. It sends the current normalized configuration as one UTF-8 JSON object to standard input and allows 180 seconds (3 minutes) for the adapter to finish.

The adapter must run its calibrated measurement workflow using the capture and playback devices specified in the configuration, then write a single JSON object to stdout:

```json
{
  "measuredAt": "2026-10-10T12:00:00Z",
  "responses": {
    "left": [{"hz": 20, "db": -4.2}, {"hz": 100, "db": 1.1}, {"hz": 20000, "db": -2.0}],
    "right": [{"hz": 20, "db": -3.8}, {"hz": 100, "db": 0.9}, {"hz": 20000, "db": -2.3}]
  },
  "autoconfiguration": {
    "crossovers": {
      "Woofer_L": {"frequency": 80, "topology": "Linkwitz-Riley", "slope": 24},
      "Tweeter_L": {"frequency": 80, "topology": "Linkwitz-Riley", "slope": 24},
      "Woofer_R": {"frequency": 82, "topology": "Linkwitz-Riley", "slope": 24},
      "Tweeter_R": {"frequency": 82, "topology": "Linkwitz-Riley", "slope": 24}
    },
    "bands": {
      "Woofer_L": [{"id": "room-wl", "enabled": true, "frequency": 55, "q": 3.2, "gain": -4.0}],
      "Tweeter_L": [],
      "Woofer_R": [],
      "Tweeter_R": []
    },
    "drivers": [
      {"name": "Woofer_L", "delayMs": 0.15, "trimDb": -1, "phaseInverted": false},
      {"name": "Tweeter_L", "delayMs": 0.35, "trimDb": -2, "phaseInverted": false},
      {"name": "Woofer_R", "delayMs": 0.17, "trimDb": -1, "phaseInverted": false},
      {"name": "Tweeter_R", "delayMs": 0.37, "trimDb": -2, "phaseInverted": false}
    ]
  }
}
```

**Response contract:**

| Field | Required | Description |
| --- | --- | --- |
| `responses.left` | Yes | Array of `{hz, db}` points (at least 2, hz: 10–40000, db: -60–20) |
| `responses.right` | Yes | Array of `{hz, db}` points (at least 2, hz: 10–40000, db: -60–20) |
| `autoconfiguration` | Yes | Object containing at least one of: `crossovers`, `bands`, or `drivers` |
| `autoconfiguration.crossovers` | No | Same schema as the configuration JSON crossovers object |
| `autoconfiguration.bands` | No | Object keyed by driver name; same band schema as configuration JSON |
| `autoconfiguration.drivers` | No | Array of driver entries; same schema as configuration JSON |
| `measuredAt` | No | ISO 8601 timestamp; defaults to server time if omitted |

If the adapter exits nonzero, times out, returns invalid JSON, or omits `responses` and `autoconfiguration`, the web app reports a measurement failure. The UI shows a progress overlay during measurement, reports success or failure in the footer status bar and toast notification, and auto-dismisses the overlay on success.

On success, the app normalizes the returned autoconfiguration, merges it with the current state, writes the updated state to `current.json`, and renders the measurement response curves on the frequency response chart.

## Speaker-test adapter

Configure these Apache environment variables with administrator-controlled executables to enable their corresponding **Test** buttons: `PI_AUDIO_TEST_BIN_WOOFER_L`, `PI_AUDIO_TEST_BIN_TWEETER_L`, `PI_AUDIO_TEST_BIN_WOOFER_R`, and `PI_AUDIO_TEST_BIN_TWEETER_R`. Each adapter is invoked directly as the Apache worker, with a 15-second timeout. The selected adapter receives one JSON object on stdin; it does not receive arbitrary signal parameters:

```json
{ "driver": "Woofer_L", "output": "default" }
```

The `driver` is one of `Woofer_L`, `Tweeter_L`, `Woofer_R`, or `Tweeter_R`; `output` is the active playback PCM name. Each adapter is responsible for safely routing its test signal to only the requested driver on that output. It must enforce a conservative level and short duration itself, stop playback before exiting, return status `0` only on success, and provide a concise stderr diagnostic on failure. The app returns a failure on nonzero exit or timeout and never trusts adapter stdout as a result payload. Do not expose user-controlled frequency, level, or duration parameters.

Keep the adapter and its parent directories writable only by administrators. Grant its Apache process only the specific service/device access it needs; do not run the DSP engine with Apache privileges.

## Deployment and checks

Install the adapter outside the web root, make it executable by Apache, and keep ownership administrator-controlled. Set the environment variable in the Apache virtual host, reload Apache, and verify the path is executable:

```sh
sudo install -o root -g root -m 0755 pi-audio-apply /usr/local/bin/pi-audio-apply
sudo -u www-data test -x /usr/local/bin/pi-audio-apply
sudo systemctl reload apache2
```

For the confirmed Ubuntu VM ALSA setup, Apache also needs the ALSA control device and full `/proc` view. Review the sandbox settings and commands in [README.md](./README.md#apache-systemd-and-alsa-access). The DSP engine's own service account needs the separate PCM device permissions required for capture and playback.

Before wiring the adapter to the production engine, test it with a known-good configuration and verify:

1. Valid JSON arrives intact on stdin.
2. A successful apply exits `0` only after the engine accepts the new configuration.
3. A rejected configuration exits nonzero and writes a useful stderr message.
4. Failure and timeout do not leave the engine in an unsafe or misleading state.
5. Engine startup reads the saved configuration, including selected input/output devices.
