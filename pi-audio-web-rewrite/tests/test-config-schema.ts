import { describe, it, expect } from 'vitest';
import { normalizeState, validPresetName, validMeasurement, stateSignature } from '../src/config/validation';
import { DEFAULT_STATE, ConfigState } from '../src/config/schema';

describe('normalizeState', () => {
  it('returns defaults for empty source', () => {
    const result = normalizeState({});
    expect(result).toEqual(DEFAULT_STATE);
  });

  it('validates sampleRate', () => {
    expect(normalizeState({ sampleRate: 48000 }).sampleRate).toBe(48000);
    expect(normalizeState({ sampleRate: 99999 }).sampleRate).toBe(DEFAULT_STATE.sampleRate);
    expect(normalizeState({ sampleRate: 'invalid' }).sampleRate).toBe(DEFAULT_STATE.sampleRate);
  });

  it('validates BufferSize', () => {
    expect(normalizeState({ BufferSize: 256 }).BufferSize).toBe(256);
    expect(normalizeState({ BufferSize: 999 }).BufferSize).toBe(DEFAULT_STATE.BufferSize);
  });

  it('validates micCount', () => {
    expect(normalizeState({ micCount: 4 }).micCount).toBe(4);
    expect(normalizeState({ micCount: 10 }).micCount).toBe(DEFAULT_STATE.micCount);
  });

  it('clamps volume to 0-100', () => {
    expect(normalizeState({ volume: -10 }).volume).toBe(0);
    expect(normalizeState({ volume: 50 }).volume).toBe(50);
    expect(normalizeState({ volume: 200 }).volume).toBe(100);
  });

  it('validates muted', () => {
    expect(normalizeState({ muted: true }).muted).toBe(true);
    expect(normalizeState({ muted: false }).muted).toBe(false);
  });

  it('validates monitorChannel', () => {
    expect(normalizeState({ monitorChannel: 'pgm-l' }).monitorChannel).toBe('pgm-l');
    expect(normalizeState({ monitorChannel: 'invalid' }).monitorChannel).toBe(DEFAULT_STATE.monitorChannel);
  });

  it('validates input/output PCM names', () => {
    expect(normalizeState({ input: 'default' }).input).toBe('default');
    expect(normalizeState({ input: 'hw:0,0' }).input).toBe('hw:0,0');
    expect(normalizeState({ input: 'a'.repeat(81) }).input).toBe(DEFAULT_STATE.input);
  });

  it('validates crossovers', () => {
    const result = normalizeState({
      crossovers: {
        Woofer: { frequency: 3000, topology: 'Butterworth', slope: 12 },
        Tweeter: { frequency: 5, topology: 'invalid', slope: 99 },
      },
    });
    expect(result.crossovers.Woofer.frequency).toBe(3000);
    expect(result.crossovers.Woofer.topology).toBe('Butterworth');
    expect(result.crossovers.Woofer.slope).toBe(12);
    expect(result.crossovers.Tweeter.frequency).toBe(30);
    expect(result.crossovers.Tweeter.topology).toBe(DEFAULT_STATE.crossovers.Tweeter.topology);
    expect(result.crossovers.Tweeter.slope).toBe(DEFAULT_STATE.crossovers.Tweeter.slope);
  });

  it('validates bands with max 12 per driver', () => {
    const manyBands = Array.from({ length: 20 }, (_, i) => ({ id: `b${i}`, frequency: 100 + i * 100 }));
    const result = normalizeState({ bands: { Woofer: manyBands, Tweeter: [] } });
    expect(result.bands.Woofer.length).toBe(12);
    expect(result.bands.Tweeter.length).toBe(0);
  });

  it('validates band fields', () => {
    const result = normalizeState({
      bands: { Woofer: [{ id: 'test-band', frequency: -1, q: 50, gain: 99 }] },
    });
    expect(result.bands.Woofer[0].frequency).toBe(20);
    expect(result.bands.Woofer[0].q).toBe(18);
    expect(result.bands.Woofer[0].gain).toBe(18);
  });

  it('validates drivers', () => {
    const result = normalizeState({
      drivers: [
        { name: 'Woofer', delayMs: -5, trimDb: 99, phaseInverted: true },
        { name: 'Tweeter', delayMs: 10, trimDb: -30, phaseInverted: false },
      ],
    });
    expect(result.drivers[0].delayMs).toBe(0);
    expect(result.drivers[0].trimDb).toBe(12);
    expect(result.drivers[0].phaseInverted).toBe(true);
    expect(result.drivers[1].delayMs).toBe(10);
    expect(result.drivers[1].trimDb).toBe(-24);
  });

  it('validates limiter', () => {
    const result = normalizeState({
      limiter: { enabled: false, threshold: -99, release: 5 },
    });
    expect(result.limiter.enabled).toBe(false);
    expect(result.limiter.threshold).toBe(-24);
    expect(result.limiter.release).toBe(10);
  });

  it('uses previous state as fallback', () => {
    const prev = normalizeState({ sampleRate: 96000, volume: 80 });
    const result = normalizeState({}, prev);
    expect(result.sampleRate).toBe(96000);
    expect(result.volume).toBe(80);
  });

  it('generates band id when invalid', () => {
    const result = normalizeState({ bands: { Woofer: [{ frequency: 1000 }] } });
    expect(result.bands.Woofer[0].id.length).toBeGreaterThan(0);
    expect(result.bands.Woofer[0].id.length).toBeLessThanOrEqual(32);
  });
});

describe('validPresetName', () => {
  it('accepts valid names', () => {
    expect(validPresetName('My Preset')).toBe(true);
    expect(validPresetName('test-name_1')).toBe(true);
    expect(validPresetName('A')).toBe(true);
  });

  it('rejects invalid names', () => {
    expect(validPresetName('')).toBe(false);
    expect(validPresetName('-invalid')).toBe(false);
    expect(validPresetName('a'.repeat(33))).toBe(false);
    expect(validPresetName(123)).toBe(false);
  });
});

describe('validMeasurement', () => {
  it('accepts valid measurement data', () => {
    const result = validMeasurement({
      responses: {
        left: [{ hz: 100, db: -10 }, { hz: 200, db: -5 }],
        right: [{ hz: 100, db: -12 }, { hz: 200, db: -7 }],
      },
    });
    expect(result).not.toBeNull();
    expect(result!.responses.left.length).toBe(2);
    expect(result!.responses.right.length).toBe(2);
  });

  it('rejects insufficient points', () => {
    const result = validMeasurement({
      responses: {
        left: [{ hz: 100, db: -10 }],
        right: [{ hz: 100, db: -12 }, { hz: 200, db: -7 }],
      },
    });
    expect(result).toBeNull();
  });

  it('rejects out-of-range values', () => {
    const result = validMeasurement({
      responses: {
        left: [{ hz: 5, db: -10 }, { hz: 200, db: -5 }],
        right: [{ hz: 100, db: -12 }, { hz: 200, db: -7 }],
      },
    });
    expect(result).toBeNull();
  });

  it('handles legacy response format', () => {
    const result = validMeasurement({
      response: [{ hz: 100, db: -10 }, { hz: 200, db: -5 }],
    });
    expect(result).not.toBeNull();
    expect(result!.responses.left.length).toBe(2);
    expect(result!.responses.right.length).toBe(2);
  });
});

describe('stateSignature', () => {
  it('produces consistent signatures for staging', () => {
    const state = normalizeState({ sampleRate: 48000, volume: 50 });
    const sig1 = stateSignature(state, 'staging');
    const sig2 = stateSignature(state, 'staging');
    expect(sig1).toBe(sig2);
  });

  it('production signature only considers input/sampleRate/BufferSize', () => {
    const s1 = normalizeState({ sampleRate: 48000, volume: 50 });
    const s2 = normalizeState({ sampleRate: 48000, volume: 90 });
    expect(stateSignature(s1, 'production')).toBe(stateSignature(s2, 'production'));
    expect(stateSignature(s1, 'staging')).not.toBe(stateSignature(s2, 'staging'));
  });
});
