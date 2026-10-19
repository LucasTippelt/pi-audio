import { describe, it, expect, beforeEach } from 'vitest';
import fs from 'fs';
import path from 'path';
import * as presets from '../src/config/presets';
import { ConfigState } from '../src/config/schema';

const TEST_DIR = '/tmp/pi-audio-test-presets';

function setupTestDirs() {
  const configDir = path.join(TEST_DIR, 'config');
  const presetsDir = path.join(TEST_DIR, 'presets');
  fs.mkdirSync(configDir, { recursive: true });
  fs.mkdirSync(presetsDir, { recursive: true });
  process.env.PI_AUDIO_CONFIG_DIR = configDir;
  process.env.PI_AUDIO_PRESETS_DIR = presetsDir;
}

function cleanup() {
  try { fs.rmSync(TEST_DIR, { recursive: true, force: true }); } catch {}
  delete process.env.PI_AUDIO_CONFIG_DIR;
  delete process.env.PI_AUDIO_PRESETS_DIR;
}

beforeEach(() => {
  cleanup();
  setupTestDirs();
});

describe('preset management', () => {
  it('creates Default preset on ensureDefaultPreset', () => {
    const result = presets.ensureDefaultPreset();
    expect(result.selected).toBe('Default');
    expect(result.version).toBe('staging');
    expect(result.names).toContain('Default');
  });

  it('saves and loads preset versions', () => {
    presets.ensureDefaultPreset();
    const state: ConfigState = {
      ...JSON.parse(JSON.stringify(presets.readState())),
      volume: 80,
    };
    presets.savePresetStaging('Default', state);
    const versions = presets.readPresetVersions('Default');
    expect(versions.staging.volume).toBe(80);
  });

  it('lists preset names', () => {
    presets.ensureDefaultPreset();
    const names = presets.presetNames();
    expect(names).toContain('Default');
  });

  it('deletes non-Default preset', () => {
    presets.ensureDefaultPreset();
    const testState: ConfigState = presets.readState();
    presets.savePresetVersions('Test Preset', { staging: testState, production: testState });
    expect(presets.presetNames()).toContain('Test Preset');
    presets.deletePreset('Test Preset');
    expect(presets.presetNames()).not.toContain('Test Preset');
  });

  it('protects Default preset from deletion', () => {
    presets.ensureDefaultPreset();
    expect(() => presets.deletePreset('Default')).toThrow(/protected/);
  });

  it('reads state from current.json', () => {
    presets.ensureDefaultPreset();
    const state = presets.readState();
    expect(state.volume).toBe(65);
    expect(state.sampleRate).toBe(48000);
  });

  it('writes and verifies current.json', () => {
    presets.ensureDefaultPreset();
    const state = presets.readState();
    state.volume = 90;
    presets.writeCurrentJson(state);
    expect(presets.currentJsonIsValid()).toBe(true);
    const reloaded = presets.readState();
    expect(reloaded.volume).toBe(90);
  });

  it('handles deleteAllPresets', () => {
    presets.ensureDefaultPreset();
    const testState: ConfigState = presets.readState();
    presets.savePresetVersions('A Preset', { staging: testState, production: testState });
    presets.savePresetVersions('B Preset', { staging: testState, production: testState });
    expect(presets.presetNames().length).toBe(3);
    const errors = presets.deleteAllPresets();
    expect(errors.length).toBe(0);
    expect(presets.presetNames()).toContain('Default');
  });

  it('converts legacy preset format', () => {
    const configDir = process.env.PI_AUDIO_CONFIG_DIR!;
    const presetsDir = process.env.PI_AUDIO_PRESETS_DIR!;
    const legacyPath = path.join(presetsDir, 'Legacy.json');
    fs.writeFileSync(legacyPath, JSON.stringify({ volume: 75, sampleRate: 48000 }));
    const versions = presets.readPresetVersions('Legacy');
    expect(versions.staging.volume).toBe(75);
    expect(versions.production.volume).toBe(75);
  });

  it('throws for non-existent preset', () => {
    presets.ensureDefaultPreset();
    expect(() => presets.readPresetVersions('NonExistent')).toThrow(/not found/);
  });
});
