import { describe, it, expect, beforeEach } from 'vitest';
import fs from 'fs';
import path from 'path';
import http from 'http';

const TEST_DIR = '/tmp/pi-audio-test-http';
const CONFIG_DIR = path.join(TEST_DIR, 'config');
const PRESETS_DIR = path.join(TEST_DIR, 'presets');

function setupTestDirs() {
  fs.mkdirSync(CONFIG_DIR, { recursive: true });
  fs.mkdirSync(PRESETS_DIR, { recursive: true });
  process.env.PI_AUDIO_CONFIG_DIR = CONFIG_DIR;
  process.env.PI_AUDIO_PRESETS_DIR = PRESETS_DIR;
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

function request(method: string, actionPath: string, body?: Record<string, unknown>): Promise<any> {
  return new Promise((resolve, reject) => {
    const url = new URL(`http://127.0.0.1:${process.env.TEST_PORT || '9876'}${actionPath}`);
    const postData = body ? JSON.stringify(body) : undefined;
    const options = {
      hostname: url.hostname,
      port: url.port,
      path: url.pathname + url.search,
      method,
      headers: { 'Content-Type': 'application/json' },
    };
    const req = http.request(options, (res) => {
      let data = '';
      res.on('data', (chunk) => data += chunk);
      res.on('end', () => {
        try {
          resolve(JSON.parse(data));
        } catch {
          reject(new Error(`Invalid JSON response: ${data}`));
        }
      });
    });
    req.on('error', reject);
    if (postData) req.write(postData);
    req.end();
  });
}

describe.skipIf(!process.env.TEST_PORT)('HTTP API endpoints', () => {
  it('GET /api/state returns default state after init', async () => {
    const result = await request('GET', '/api/state');
    expect(result.state).toBeDefined();
    expect(result.state.volume).toBe(65);
    expect(result.presets).toContain('Default');
  });

  it('POST /api/save saves state', async () => {
    await request('GET', '/api/state');
    const result = await request('POST', '/api/save', { volume: 80, sampleRate: 48000 });
    expect(result.state.volume).toBe(80);
    expect(result.currentJsonWritten).toBe(true);
  });

  it('POST /api/preset-save creates new preset', async () => {
    await request('GET', '/api/state');
    const result = await request('POST', '/api/preset-save', { name: 'Test Preset' });
    expect(result.selectedPreset).toBe('Test Preset');
    expect(result.presets).toContain('Test Preset');
  });

  it('POST /api/restore-defaults resets to defaults', async () => {
    await request('GET', '/api/state');
    const result = await request('POST', '/api/restore-defaults');
    expect(result.state.volume).toBe(65);
    expect(result.selectedPreset).toBe('Default');
  });

  it('GET /health returns ok', async () => {
    const result = await request('GET', '/health');
    expect(result.status).toBe('ok');
  });
});
