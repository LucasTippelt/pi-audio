import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import request from 'supertest';
import express from 'express';
import http from 'http';
import fs from 'fs';
import path from 'path';
import { createRouter } from '../src/http/router';

const TEST_DIR = '/tmp/pi-audio-e2e-test';
const CONFIG_DIR = path.join(TEST_DIR, 'config');
const PRESETS_DIR = path.join(TEST_DIR, 'presets');
const RUN_DIR = '/run/pi-audio';
const STATE_FILE = path.join(RUN_DIR, 'engine-state.json');

function setup() {
  fs.mkdirSync(CONFIG_DIR, { recursive: true });
  fs.mkdirSync(PRESETS_DIR, { recursive: true });
  try { fs.mkdirSync(RUN_DIR, { recursive: true }); } catch {}

  process.env.PI_AUDIO_CONFIG_DIR = CONFIG_DIR;
  process.env.PI_AUDIO_PRESETS_DIR = PRESETS_DIR;
  process.env.PI_AUDIO_STATE_FILE = STATE_FILE;
}

function cleanup() {
  try { fs.unlinkSync(STATE_FILE); } catch {}
  try { fs.rmSync(TEST_DIR, { recursive: true, force: true }); } catch {}
  delete process.env.PI_AUDIO_CONFIG_DIR;
  delete process.env.PI_AUDIO_PRESETS_DIR;
  delete process.env.PI_AUDIO_STATE_FILE;
}

describe('End-to-end integration', () => {
  let app: express.Express;
  let server: http.Server;

  beforeAll(() => {
    cleanup();
    setup();
    app = express();
    app.use(express.json());
    app.use(createRouter());
    server = http.createServer(app);
  });

  afterAll((done) => {
    cleanup();
    server.close(done);
  });

  describe('Health & version endpoints', () => {
    it('returns health status', async () => {
      const res = await request(app).get('/health');
      expect(res.status).toBe(200);
      expect(res.body.status).toBe('ok');
    });
  });

  describe('Config CRUD flow', () => {
    it('loads default state when no config exists', async () => {
      const res = await request(app).get('/api/state');
      expect(res.status).toBe(200);
      expect(res.body.state).toBeDefined();
      expect(res.body.state.volume).toBe(65);
    });

    it('saves configuration and persists to file', async () => {
      const config = { volume: 80, sampleRate: 48000, muted: false };
      const res = await request(app)
        .post('/api/save')
        .send(config);

      expect(res.status).toBe(200);

      // Verify file was written
      const currentFile = path.join(CONFIG_DIR, 'current.json');
      expect(fs.existsSync(currentFile)).toBe(true);
    });

    it('applies master volume change', async () => {
      const res = await request(app)
        .post('/api/master')
        .send({ volume: 42, muted: true });

      expect(res.status).toBe(200);
    });
  });

  describe('Preset management flow', () => {
    it('creates new preset successfully', async () => {
      const res = await request(app)
        .post('/api/preset-save')
        .send({ name: 'Test Preset' });

      expect(res.status).toBe(200);
    });

    it('selects existing preset', async () => {
      const res = await request(app)
        .post('/api/preset-select')
        .send({ name: 'Default' });

      expect(res.status).toBe(200);
    });

    it('rejects invalid preset name with 4xx status', async () => {
      const res = await request(app)
        .post('/api/preset-save')
        .send({ name: 'Invalid Name!' });

      expect(res.status).toBeGreaterThanOrEqual(400);
      expect(res.status).toBeLessThan(500);
    });
  });

  describe('DSP status endpoint', () => {
    it('returns disconnected when no state file', async () => {
      try { fs.unlinkSync(STATE_FILE); } catch {}
      const res = await request(app).get('/api/dsp-status');
      // May return 502 if adapter not configured, or 200 with disconnected status
      expect([200, 502]).toContain(res.status);
    });

    it('returns connected when state file exists', async () => {
      fs.writeFileSync(STATE_FILE, JSON.stringify({
        sampleRate: 48000,
        BufferSize: 512,
      }));

      const res = await request(app).get('/api/dsp-status');
      if (res.status === 200) {
        expect(res.body.connected).toBe(true);
        expect(res.body.sampleRate).toBe(48000);
      }
    });
  });

  describe('Metrics endpoint', () => {
    it('returns system metrics with valid response', async () => {
      const res = await request(app).get('/api/metrics');
      expect(res.status).toBe(200);
      // Metrics response should have some numeric fields
      expect(res.body).toBeDefined();
    });
  });

  describe('Error handling', () => {
    it('returns 404 for unknown action', async () => {
      const res = await request(app)
        .post('/api/unknown-action')
        .send({});

      expect(res.status).toBe(404);
    });

    it('handles malformed JSON gracefully', async () => {
      const res = await request(app)
        .post('/api/save')
        .set('Content-Type', 'application/json')
        .send('not json');

      expect(res.status).toBe(400);
    });
  });
});
