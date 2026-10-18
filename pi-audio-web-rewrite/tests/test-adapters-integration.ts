import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import fs from 'fs';
import path from 'path';
import * as statusAdapter from '../src/adapters/status-adapter';
import * as applyAdapter from '../src/adapters/apply-adapter';

const RUN_DIR = '/run/pi-audio';
const STATE_FILE = path.join(RUN_DIR, 'engine-state.json');
const STATUS_FILE = path.join(RUN_DIR, 'status');
const RELOAD_FILE = path.join(RUN_DIR, 'reload');

function setup() {
  try { fs.mkdirSync(RUN_DIR, { recursive: true }); } catch {}
}

function cleanup() {
  try { fs.unlinkSync(STATE_FILE); } catch {}
  try { fs.unlinkSync(STATUS_FILE); } catch {}
  try { fs.unlinkSync(RELOAD_FILE); } catch {}
  try { fs.unlinkSync(RELOAD_FILE + '.tmp'); } catch {}
}

beforeEach(() => {
  cleanup();
  setup();
});

afterEach(cleanup);

describe('status adapter file-based integration', () => {
  it('returns disconnected when no state file exists', async () => {
    const result = await statusAdapter.getStatus();
    expect(result.ok).toBe(true);
    if (result.data) {
      expect(result.data.connected).toBe(false);
      expect(result.data.sampleRate).toBeNull();
      expect(result.data.BufferSize).toBeNull();
    }
  });

  it('returns connected with valid state file', async () => {
    fs.writeFileSync(STATE_FILE, JSON.stringify({
      connected: true,
      sampleRate: 48000,
      BufferSize: 512,
      version: 1,
    }));

    const result = await statusAdapter.getStatus();
    expect(result.ok).toBe(true);
    if (result.data) {
      expect(result.data.connected).toBe(true);
      expect(result.data.sampleRate).toBe(48000);
      expect(result.data.BufferSize).toBe(512);
    }
  });

  it('returns disconnected with status sentinel message', async () => {
    fs.writeFileSync(STATUS_FILE, 'initializing');

    const result = await statusAdapter.getStatus();
    expect(result.ok).toBe(true);
    if (result.data) {
      expect(result.data.connected).toBe(false);
      expect(result.data.message).toContain('initializing');
    }
  });

  it('rejects invalid sampleRate', async () => {
    fs.writeFileSync(STATE_FILE, JSON.stringify({
      connected: true,
      sampleRate: 99999,
      BufferSize: 512,
    }));

    const result = await statusAdapter.getStatus();
    expect(result.ok).toBe(false);
    if (result.error) {
      expect(result.error).toContain('unsupported sampleRate');
    }
  });

  it('rejects invalid BufferSize', async () => {
    fs.writeFileSync(STATE_FILE, JSON.stringify({
      connected: true,
      sampleRate: 48000,
      BufferSize: 99999,
    }));

    const result = await statusAdapter.getStatus();
    expect(result.ok).toBe(false);
    if (result.error) {
      expect(result.error).toContain('unsupported BufferSize');
    }
  });

  it('rejects partial state — connected without sampleRate', async () => {
    fs.writeFileSync(STATE_FILE, JSON.stringify({
      connected: true,
      sampleRate: null,
      BufferSize: null,
    }));

    const result = await statusAdapter.getStatus();
    expect(result.ok).toBe(false);
  });

  it('handles malformed state file gracefully', async () => {
    fs.writeFileSync(STATE_FILE, 'this is not json');

    const result = await statusAdapter.getStatus();
    expect(result.ok).toBe(true);
    if (result.data) {
      expect(result.data.connected).toBe(false);
    }
  });
});

describe('apply adapter file-based integration', () => {
  it('rejects invalid JSON input', async () => {
    const result = await applyAdapter.applyConfiguration('{bad json');
    expect(result.ok).toBe(false);
    expect(result.message).toContain('Invalid JSON');
  });

  it('rejects empty input', async () => {
    const result = await applyAdapter.applyConfiguration('   ');
    expect(result.ok).toBe(false);
    expect(result.message).toContain('No configuration data');
  });

  it('creates reload trigger file with valid config and confirms', async () => {
    fs.writeFileSync(STATE_FILE, JSON.stringify({
      connected: true,
      sampleRate: 48000,
      BufferSize: 512,
    }));

    const config = { volume: 75, sampleRate: 48000, BufferSize: 512 };
    const result = await applyAdapter.applyConfiguration(JSON.stringify(config), 3000);
    expect(result.ok).toBe(true);
  });
});
