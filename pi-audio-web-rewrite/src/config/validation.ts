import crypto from 'crypto';
import {
  ConfigState,
  DEFAULT_STATE,
  VALID_SAMPLE_RATES,
  VALID_BUFFER_SIZES,
  VALID_MIC_COUNTS,
  VALID_TOPOLOGIES,
  VALID_SLOPES,
  VALID_MONITOR_CHANNELS,
  BandConfig,
  MeasurementData,
  MeasurementPoint,
} from './schema';

const PCM_RE = /^[A-Za-z0-9_.:,-=]{1,80}$/;
const PRESET_NAME_RE = /^[A-Za-z0-9][A-Za-z0-9 _-]{0,31}$/;
const BAND_ID_RE = /^[A-Za-z0-9_-]{1,32}$/;

function clamp(val: number, min: number, max: number): number {
  return Math.max(min, Math.min(max, val));
}

function roundTo(val: number, places: number): number {
  const m = Math.pow(10, places);
  return Math.round(val * m) / m;
}

function generateId(): string {
  const bytes = new Uint8Array(5);
  crypto.randomFillSync(bytes);
  return Array.from(bytes).map((b) => b.toString(16).padStart(2, '0')).join('');
}

export function validPresetName(name: unknown): name is string {
  return typeof name === 'string' && PRESET_NAME_RE.test(name);
}

function cryptoHash(input: string): string {
  return crypto.createHash('sha256').update(input).digest('hex');
}

function inArray<T>(arr: readonly unknown[], val: T): boolean {
  return arr.includes(val as any);
}

export function normalizeState(source: Record<string, unknown>, previous?: ConfigState): ConfigState {
  const base = previous ?? DEFAULT_STATE;
  const out = structuredClone(base) as ConfigState;

  out.input = isValidPcmName(source.input) ? (source.input as string) : base.input;
  out.output = isValidPcmName(source.output) ? (source.output as string) : base.output;

  const sr = source.sampleRate as number | undefined;
  out.sampleRate = inArray(VALID_SAMPLE_RATES, sr ?? -1) ? (sr as any) : base.sampleRate;

  const bs = source.BufferSize as number | undefined;
  out.BufferSize = inArray(VALID_BUFFER_SIZES, bs ?? -1) ? (bs as any) : base.BufferSize;

  const micCountVal = source.micCount as number | undefined;
  if (inArray(VALID_MIC_COUNTS, micCountVal ?? -1)) {
    out.micCount = micCountVal!;
  } else {
    out.micCount = base.micCount;
  }

  out.volume = clamp(Math.round((source.volume as number) ?? base.volume), 0, 100);
  out.muted = Boolean(source.muted ?? base.muted);

  const mch = source.monitorChannel as string | undefined;
  out.monitorChannel = inArray(VALID_MONITOR_CHANNELS, mch ?? '') ? (mch as any) : base.monitorChannel;

  if (!isPlainObject(source.crossovers)) {
    out.crossovers = structuredClone(base.crossovers);
  } else {
    const sourceCrossovers = source.crossovers;
    out.crossovers = {} as typeof out.crossovers;
    for (const [name, defaults] of Object.entries(DEFAULT_STATE.crossovers)) {
      const cross = isPlainObject(sourceCrossovers[name]) ? sourceCrossovers[name] : {};
      const fallback = base.crossovers[name] ?? defaults;
      out.crossovers[name] = {
        enabled: Boolean((cross.enabled as boolean) ?? fallback.enabled),
        frequency: clamp(Math.round((cross.frequency as number) ?? fallback.frequency), 30, 20000),
        topology: inArray(VALID_TOPOLOGIES, cross.topology as string)
          ? (cross.topology as any) : fallback.topology,
        slope: inArray(VALID_SLOPES, Math.round((cross.slope as number) ?? 0))
          ? (Math.round((cross.slope as number) ?? 0) as any) : fallback.slope,
      };
    }
  }

  if (!isPlainObject(source.bands)) {
    out.bands = structuredClone(base.bands);
  } else {
    const sourceBands = source.bands;
    out.bands = {} as typeof out.bands;
    for (const driverName of Object.keys(DEFAULT_STATE.bands)) {
      const driverBands = Array.isArray(sourceBands[driverName]) ? sourceBands[driverName] : [];
      out.bands[driverName] = [];
      for (const band of driverBands.slice(0, 12)) {
        if (!isPlainObject(band)) continue;
        const id = BAND_ID_RE.test(String(band.id ?? '')) ? String(band.id) : generateId();
        out.bands[driverName].push({
          id,
          enabled: Boolean((band.enabled as boolean) ?? true),
          frequency: clamp(Math.round((band.frequency as number) ?? 1000), 20, 20000),
          q: roundTo(clamp(Number(band.q ?? 1), 0.1, 18), 2),
          gain: roundTo(clamp(Number(band.gain ?? 0), -18, 18), 1),
        } as BandConfig);
      }
    }
  }

  const drivers = Array.isArray(source.drivers) ? source.drivers : [];
  const driversByName: Record<string, Record<string, unknown>> = {};
  for (const item of drivers) {
    if (isPlainObject(item) && typeof item.name === 'string') {
      driversByName[item.name] = item;
    }
  }
  out.drivers = [];
  for (const driver of DEFAULT_STATE.drivers) {
    const item = driversByName[driver.name] ?? {};
    let existing: Record<string, unknown> | null = null;
    for (const bd of base.drivers) {
      if (bd.name === driver.name) {
        existing = bd as unknown as Record<string, unknown>;
        break;
      }
    }
    out.drivers.push({
      name: driver.name,
      delayMs: roundTo(clamp(Number(item.delayMs ?? existing?.delayMs ?? driver.delayMs), 0, 20), 3),
      trimDb: roundTo(clamp(Number(item.trimDb ?? existing?.trimDb ?? driver.trimDb), -24, 12), 1),
      phaseInverted: typeof item.phaseInverted === 'boolean' ? item.phaseInverted : Boolean(existing?.phaseInverted ?? driver.phaseInverted),
    });
  }

  const limiter = isPlainObject(source.limiter) ? source.limiter : base.limiter as unknown as Record<string, unknown>;
  out.limiter = {
    enabled: Boolean((limiter.enabled as boolean) ?? true),
    threshold: roundTo(clamp(Number(limiter.threshold ?? -1), -24, 0), 1),
    attack: roundTo(clamp(Number(limiter.attack ?? 5), 0.1, 100), 1),
    release: clamp(Math.round((limiter.release as number) ?? 80), 10, 2000),
    ratio: roundTo(clamp(Number(limiter.ratio ?? 4), 1, 20), 1),
  };

  out.measurement = null;
  if (isPlainObject(source.measurement)) {
    const valid = validMeasurement(source.measurement);
    if (valid) out.measurement = valid;
  }

  return out;
}

export function validMeasurement(measurement: Record<string, unknown>): MeasurementData | null {
  let responses = measurement.responses as Record<string, unknown> | undefined;
  if (!responses || !isPlainObject(responses)) {
    responses = {};
  }
  if (!responses.left && !responses.right && Array.isArray((measurement as any).response)) {
    responses = { left: (measurement as any).response, right: (measurement as any).response };
  }

  const validated: Record<string, MeasurementPoint[]> = {};
  for (const channel of ['left', 'right']) {
    if (!Array.isArray(responses[channel])) return null;
    const points: MeasurementPoint[] = [];
    for (const point of (responses[channel] as unknown[]).slice(0, 1000)) {
      if (!isPlainObject(point)) continue;
      const hz = Number((point as any).hz);
      const db = Number((point as any).db);
      if (!Number.isFinite(hz) || !Number.isFinite(db)) continue;
      if (hz < 10 || hz > 40000 || db < -60 || db > 20) continue;
      points.push({ hz, db });
    }
    points.sort((a, b) => a.hz - b.hz);
    if (points.length < 2) return null;
    validated[channel] = points;
  }

  return {
    responses: validated as MeasurementData['responses'],
    measuredAt: String(measurement.measuredAt ?? new Date().toISOString()),
  };
}

export function stateSignature(state: ConfigState, version: 'staging' | 'production'): string {
  if (version === 'production') {
    const subset = JSON.stringify({
      input: state.input,
      sampleRate: state.sampleRate,
      BufferSize: state.BufferSize,
    });
    return cryptoHash(subset);
  }
  return cryptoHash(JSON.stringify(state));
}

function isValidPcmName(val: unknown): val is string {
  return typeof val === 'string' && PCM_RE.test(val);
}

function isPlainObject(val: unknown): val is Record<string, unknown> {
  return typeof val === 'object' && val !== null && !Array.isArray(val);
}
