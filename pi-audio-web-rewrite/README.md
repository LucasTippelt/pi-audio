# Pi Audio Web Rewrite

TypeScript web server replacing the legacy PHP/Apache interface. Unified HTTP REST API and WebSocket audio streaming on a single port, with a vanilla JS frontend for DSP configuration, preset management, and real-time monitoring.

## Tech Stack

| Layer | Technology |
|---|---|
| Language | TypeScript 5.7, ES2022 target, CommonJS modules, strict mode |
| Runtime | Node.js (production), ts-node (development) |
| HTTP Framework | Express 4.21 |
| WebSocket | ws 8.22 |
| Test Runner | Vitest 3.0 with supertest for HTTP integration |
| Build | `tsc` (plain TypeScript compiler, no bundler) |
| Frontend | Vanilla JS + CSS, zero frameworks, Canvas API for charts |

## Requirements

- **Node.js** 18+
- **npm**

```bash
npm ci
npm run build
```

## Run

```bash
# Production (compiled)
npm start

# Development (hot-reload with ts-node)
npm run dev

# Tests
npm test
```

Server listens on port 8767 by default. Frontend served at `/`, API at `/api/`, WebSocket at `/ws`.

## Architecture

```
pi-audio-web-rewrite/
├── src/
│   ├── server.ts                   # Main entry: Express app + WS server + graceful shutdown
│   ├── http/                       # REST API layer
│   │   ├── router.ts               # Central Express router with GET and POST action dispatch
│   │   ├── config-api.ts           # Config CRUD, preset management, apply logic (609 lines)
│   │   ├── mixer-api.ts            # ALSA amixer integration (volume control via subprocess)
│   │   └── metrics-api.ts          # CPU/temperature/xrun system metrics from /proc
│   ├── websocket/                  # Real-time audio streaming layer
│   │   ├── ws-server.ts            # WebSocket server with channel subscription + ping/pong
│   │   ├── broadcaster.ts          # SHM ring reader → WS broadcast loop (~50Hz)
│   │   └── shm-reader.ts           # /dev/shm binary ring buffer parser (PIAI format)
│   ├── config/                     # Configuration management layer
│   │   ├── schema.ts               # TypeScript types + DEFAULT_STATE definition
│   │   ├── validation.ts           # normalizeState(), stateSignature(), validators
│   │   └── presets.ts              # Preset file I/O (staging/production versions)
│   ├── adapters/                   # External binary adapter layer
│   │   ├── status-adapter.ts       # Probes DSP engine connection/status via pi-audio-status
│   │   ├── apply-adapter.ts        # Sends config JSON to DSP engine via pi-audio-apply stdin
│   │   └── test-adapter.ts         # Runs speaker/driver tests via pi-audio-test
│   └── frontend/                   # Static web UI (served as static files)
│       ├── index.html              # Single-page app shell with accessibility attributes
│       └── assets/
│           ├── style.css           # CSS custom properties, responsive breakpoints
│           └── app.js              # Vanilla JS client (~1100 lines, no framework)
├── deploy/
│   └── pi-audio-web.service        # systemd unit file for production
├── mock-adapters/                  # Bash scripts simulating DSP binaries (for testing)
│   ├── pi-audio-status             # Mock status probe
│   ├── pi-audio-apply              # Mock config applier
│   └── pi-audio-test               # Mock speaker test simulator
├── native/                         # Reserved for N-API modules (e.g., SHM reader addon)
├── tests/                          # Vitest test suite
├── deploy.sh                       # Full deployment pipeline
├── setup-mocks.sh                  # Installs mock adapters for testing without hardware
├── package.json
├── tsconfig.json
└── vitest.config.ts
```

## API Endpoints

### GET Endpoints

| Endpoint | Description | Response |
|---|---|---|
| `GET /api/state` | Full DSP configuration state, presets list, session info | `{ state: ConfigState, presets: [...], selectedPreset, version }` |
| `GET /api/metrics` | System metrics: CPU load, temperature, audio xruns | `{ cpu: number, tempC: number, xruns: number }` |
| `GET /api/dsp-status` | DSP engine connection status via adapter binary | `{ connected: bool, sampleRate, BufferSize, message? }` |
| `GET /health` | Health check | `{ "status": "ok" }` |
| `GET /version` | Version info from VERSION.json | version object |

### POST Endpoints

All POST actions routed through `/api/:action`:

| Action | Description |
|---|---|
| `save` | Save current state to `current.json` |
| `save-staging` | Persist staging snapshot for selected preset |
| `save-production` | Persist production settings (input/sampleRate/BufferSize only) |
| `master` | Update master volume + ALSA mixer control via amixer subprocess |
| `apply` | Push configuration to DSP engine via apply adapter binary |
| `test-speaker` | Run speaker/driver test signal (Tweeter, Woofer, Mic_1..Mic_6). Body: `{ driver, duration }`. Duration is 1–60 s (default 3). |
| `test-pgm`     | Run PGM output test. Body: `{ duration }`. Duration is 1–60 s (default 3). Frontend enforces minimum button-block of `duration + 0.5 s`. |
| `preset-save` | Create new preset from current state |
| `preset-select` | Load a specific preset version into working memory |
| `preset-promote` | Copy staging → production for a preset |
| `preset-copy-production-to-staging` | Reverse: copy production → staging |
| `preset-delete` | Delete a single user preset (Default is protected) |
| `preset-delete-all` | Delete all presets except Default |
| `restore-defaults` | Reset to factory DEFAULT_STATE |

### Debug Page

`GET /debug` — Diagnostic page with auto-refresh (5s interval) showing:

- **DSP Connection Status**: Connected state, sample rate, buffer size from engine-state.json
- **Services**: Active/inactive status of pi-audio-engine and pi-audio-web systemd services
- **Binaries**: Existence and executable permissions of engine binary
- **Output Levels (Live)**: Real-time L/R level meters from engine telemetry
- **Engine Telemetry**: Sequence number, XRUN counters, clock state, latency frames, sample index
- **Engine State vs Config**: Side-by-side comparison of config file values vs running engine state with match indicators. Includes detailed DSP parameter rows for crossover (enabled, frequency, topology, slope per driver), limiter (threshold, attack, release, ratio), and drivers (delay in ms, trim dB, phase inversion)
- **Current Configuration**: Full current.json contents rendered as table
- **Raw JSON Panels**: Raw engine-state.json and current.json side by side
- **ALSA**: Sound card list and mixer controls from `aplay -l` and `amixer scontrols`
- **Presets**: File listing with sizes and modification times
- **File Permissions**: Writability check for key config paths
- **Build Versions**: Web server version, engine version, version match status, Node.js version

## WebSocket Audio Streaming

Subscribe to real-time audio channels via WebSocket:

```
ws://host:8767/ws?ch=<channel>
```

### Channels

| Index | Channel Name | Description |
|---|---|---|
| 0 | `pgm-l` | PGM program input, left channel |
| 1 | `pgm-r` | PGM program input, right channel |
| 2 | `woofer` | Woofer output after DSP processing |
| 3 | `tweeter` | Tweeter output after DSP processing |
| 4–9 | `mic-1` .. `mic-6` | Measurement microphone inputs |

### Protocol

- **Subscribe**: Connect with `?ch=<channel>` query parameter
- **Ack**: Server sends JSON `{ "sampleRate": 48000, "BufferSize": 512 }` from engine-state.json
- **Audio**: Server pushes binary frames — raw mono float32LE samples
- **Keepalive**: Ping/pong every 20s, 5s timeout for disconnect

### Broadcaster

Periodic broadcast loop runs every 20ms (~50Hz). Reads SHM rings only for channels with active subscribers. Converts Float32Array to Buffer, sends binary WebSocket frames. Tracks per-channel rate statistics (samples/sec, messages, skip count).

## Configuration Schema

Full TypeScript type definitions for the audio configuration domain:

### ConfigState

```typescript
interface ConfigState {
  input: string;           // Input device name
  output: string;          // Output device name
  sampleRate: number;      // 44100, 48000, 96000
  BufferSize: number;      // Power of 2: 64–2048
  micCount: number;        // 2–6 microphones
  volume: number;          // 0–100
  muted: boolean;
  crossovers: Record<string, CrossoverConfig>;
  bands: Record<string, BandConfig[]>;
  drivers: DriverConfig[];
  limiter: LimiterConfig;
  monitorChannel: MonitorChannel;
  measurement?: MeasurementData;
}
```

### CrossoverConfig

Per-output crossover filter settings. One entry per driver name (e.g. `Woofer`, `Tweeter`).

```typescript
interface CrossoverConfig {
  enabled: boolean;     // Enable/disable crossover for this driver
  frequency: number;    // 30 – 20000 Hz
  topology: Topology;   // 'Linkwitz-Riley' | 'Butterworth' | 'Bessel'
  slope: number;        // 12 | 18 | 24 | 36 | 48 (dB/octave)
}
```

**Default**: `{ frequency: 2500, topology: 'Linkwitz-Riley', slope: 24 }` for both Woofer and Tweeter.

### BandConfig

Parametric EQ band. Maximum 12 bands per driver.

```typescript
interface BandConfig {
  id: string;
  enabled: boolean;
  frequency: number;    // 20 – 20000 Hz
  Q: number;            // 0.1 – 18
  gain: number;         // -18 to +18 dB
}
```

### DriverConfig

Per-output driver settings.

```typescript
interface DriverConfig {
  name: string;         // e.g. 'Woofer', 'Tweeter'
  delayMs: number;      // 0 – 20 ms
  trimDb: number;       // -24 to +12 dB
  phaseInverted: boolean;
}
```

### LimiterConfig

Output protection limiter.

```typescript
interface LimiterConfig {
  enabled: boolean;
  threshold: number;    // -24 to 0 dBFS
  attack: number;       // 0.1 – 100 ms
  release: number;      // 10 – 2000 ms
  ratio: number;        // 1 – 20 (:1)
}
```

## Crossover Configuration

Crossovers split the audio signal between woofer and tweeter at a chosen frequency point. Each output (driver) has its own crossover configuration entry.

### Topologies

| Topology | Characteristics | Use Case |
|---|---|---|
| **Linkwitz-Riley** | Cascaded Butterworth sections; in-phase summation at crossover produces flat combined magnitude | Default for two-way systems — coherent woofer+tweeter summation |
| **Butterworth** | Maximally-flat passband, steeper transition than LR for same order | When individual driver response matters more than summed response |
| **Bessel** | Linear phase (preserves transient shape), gentler roll-off | Transient-critical applications where phase coherence is priority |

### Slopes

Slope determines filter steepness: 12 dB/oct = 2nd order, 18 dB/oct = 3rd, 24 dB/oct = 4th, 36 dB/oct = 6th, 48 dB/oct = 8th. Higher slopes provide stronger driver isolation but introduce more phase shift (except Bessel).

### Frequency Range

Crossover frequency is clamped to 30–20 kHz by `normalizeState()`. The default 2500 Hz crossover point suits typical tweeter/woofer pairings. Lower frequencies (e.g., 800–1500 Hz) for soft-dome tweeters, higher (3–5 kHz) for compression drivers.

### Crossover Bypass

Each driver's crossover can be toggled on/off independently via the `enabled` field. When disabled, the driver receives full-bandwidth signal with no filter applied. The frontend shows a toggle switch per crossover card and visually dims bypassed cards (48% opacity).

### Frontend Visualization

The frontend renders a combined woofer+tweeter frequency response curve on a Canvas with logarithmic X-axis. The curve computes:
- Individual driver crossover filter response per topology formula (skipped when crossover is disabled)
- PEQ band contributions summed in dB
- Combined magnitude (for Linkwitz-Riley pairs: coherent summation; others: energy sum)

## Adapter Layer

Three thin adapters spawn external DSP binaries via `child_process.spawn()`. All configured through environment variables, making them swappable between real binaries and mock scripts.

| Adapter | Env Var | Behavior |
|---|---|---|
| **status-adapter** | `PI_AUDIO_STATUS_BIN` | Runs binary, validates JSON response (`connected`, `sampleRate`, `BufferSize`) |
| **apply-adapter** | `PI_AUDIO_APPLY_BIN` | Pipes config JSON to stdin, reads result. 30s timeout. Exits 0 on success. |
| **test-adapter** | `PI_AUDIO_TEST_BIN_<DRIVER>` | Spawns binary with `[driverName, durationSeconds]` arguments. Timeout is `max(10 s, duration + 2 s buffer)`. |

Mock adapters in `mock-adapters/` simulate DSP binaries for testing without hardware. Install with `setup-mocks.sh`.

## Preset System

Dual-version preset workflow: each preset has independent staging and production configurations.

- **Staging**: Fully editable — all tuning parameters (crossovers, bands, drivers, limiter)
- **Production**: Locked tuning — only input device, sample rate, and buffer size remain mutable
- **Promote**: Copy staging → production for a preset
- **Copy back**: Reverse direction for re-tuning from production baseline

Each preset is a JSON file with `{ staging: ConfigState, production: ConfigState }`. Atomic writes via temp file + rename pattern. Legacy format (single state object) auto-converts on load. The "Default" preset cannot be deleted.

## SHM Ring Reader

Reads audio data from POSIX shared memory ring buffers at `/dev/shm/pi-audio-{channel}`:

- **PIAI header** (80 bytes): magic, version, writePos, sampleRate, channels, seq, generation, state, name
- Converts multi-channel interleaved data to mono by averaging channels
- Respects `maxFrames` limit per read
- Used by broadcaster for WebSocket audio streaming

## Frontend

Single-page vanilla JS application with no build step:

- **Debounced auto-save**: 80ms throttle after changes via `queueSave()`
- **Revision-based save conflict detection**: `saveRevision` vs `savedRevision` tracking
- **Canvas frequency response charts**: Logarithmic X-axis, combined woofer+tweeter crossover + PEQ curves computed mathematically
- **Real-time metrics polling**: CPU bar, temperature gradient, xrun counter (every 3s)
- **DSP status polling**: Sample rate/buffer size mismatch detection against engine-state.json
- **Preset management UI**: Staging/production workflow with promote/copy controls
- **Room measurement flow**: Overlay dialog for frequency response capture
- **Test duration inputs**: Inline number input next to each test button (PGM, per-driver). Accepts 1–60 s (default 3 when empty). Frontend enforces minimum button-block time of `duration + 0.5 s` so buttons stay disabled while the hardware signal is still playing

CSS uses custom properties for theming (`--paper`, `--surface`, `--ink`, `--olive`, `--coral`), responsive breakpoints at 1050px, 760px, 380px. DM Mono + Manrope fonts from Google Fonts.

## Deploy

### Production

```bash
cd pi-audio-web-rewrite
sudo ./deploy.sh
```

Deploy script pipeline:
1. `npm ci` + `tsc` build
2. Install systemd service file to `/etc/systemd/system/`
3. Stop legacy PHP web server if running
4. Restart new service, verify health endpoint

### systemd Service

Installed at `/etc/systemd/system/pi-audio-web.service`:
- Runs as `www-data`, port 80 (via `CAP_NET_BIND_SERVICE`)
- Security: `NoNewPrivileges`, `ProtectSystem=strict`, `PrivateTmp`
- `ReadWritePaths`: `/var/www/pi-audio /etc/pi-audio /dev/shm /run/pi-audio`
- Depends on `pi-audio-engine.service`

**Important**: After any service file change, run `sudo systemctl daemon-reload && sudo systemctl restart pi-audio-web`. The `ReadWritePaths` sandbox mounts only take effect at service start — an old process keeps stale read-only mount namespaces.

### IPC Permissions

The web server communicates with the DSP engine via file-based IPC in `/run/pi-audio/`. For speaker tests (`test-speaker`, `test-pgm`) to work:

1. **Engine service** creates `/run/pi-audio/` via systemd `RuntimeDirectory=pi-audio` with `RuntimeDirectoryMode=0777` (must be world-writable for www-data access)
2. **www-data user** must be in the `audio` group: `sudo usermod -aG audio www-data`
3. After adding www-data to audio group, restart the web server so it picks up new group membership

After any service file change (dependency updates, ReadWritePaths changes), run:
```bash
sudo systemctl daemon-reload && sudo systemctl restart pi-audio-engine pi-audio-web
```

Without these permissions, speaker tests return `"Speaker test for X requires a running engine"` even when the engine is active. The `ProtectSystem=strict` sandbox makes `/run/` read-only by default; `ReadWritePaths=/run/pi-audio` creates a writable bind mount but only at service start time.

### Environment Variables

Set in systemd service or Apache config:

| Variable | Purpose |
|---|---|
| `PI_AUDIO_STATUS_BIN` | Path to status probe binary |
| `PI_AUDIO_APPLY_BIN` | Path to config apply binary |
| `PI_AUDIO_TEST_BIN_TWEETER` | Test binary for Tweeter driver |
| `PI_AUDIO_TEST_BIN_WOOFER` | Test binary for Woofer driver |
| `PI_AUDIO_TEST_BIN_MIC_1`..`MIC_6` | Test binaries for mic channels |
| `PI_AUDIO_CONFIG_DIR` | Config directory (default: `/etc/pi-audio/config`) |
| `PI_AUDIO_PRESETS_DIR` | Presets directory (default: `/etc/pi-audio/presets`) |
| `PI_AUDIO_XRUN_FILE` | Xrun counter file path (default: `/run/pi-audio/xruns`) |

## Test Suite

8 test files using Vitest 3.0 with supertest for HTTP integration:

| File | Scope | Approach |
|---|---|---|
| `test-config-schema.ts` | Validation logic | Unit tests for normalizeState clamping, preset names, measurement validation, signature consistency |
| `test-adapters.ts` | Adapter error paths | Verifies graceful errors when binaries are not configured |
| `test-adapters-integration.ts` | Full adapter flow | Copies mock binaries to temp dir, sets env vars, tests status/apply/test with real file I/O |
| `test-metrics.ts` | Metrics API | Validates response structure and value ranges |
| `test-http-api.ts` | HTTP endpoints | Direct HTTP requests against running server (skipped unless TEST_PORT set) |
| `test-e2e.ts` | Full integration | Uses supertest to spin up Express app, tests health, config CRUD, presets, DSP status, error handling |
| `test-shm-reader.ts` | SHM parsing | Creates synthetic ring buffer files in /dev/shm, validates readShmChannel output |
| `test-presets.ts` | Preset management | Tests CRUD operations, Default protection, legacy format conversion, atomic writes |
| `test-ws-server.ts` | Channel mapping | Verifies CHANNELS/CH_NAMES constants and bidirectional lookup |
