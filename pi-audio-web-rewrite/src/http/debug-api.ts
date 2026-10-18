import fs from 'fs';
import { execFile } from 'child_process';
import { promisify } from 'util';
import { Request, Response } from 'express';
import * as presets from '../config/presets';
import { getStatus } from '../adapters/status-adapter';

const execFileAsync = promisify(execFile);

const kEngineStateFile = '/run/pi-audio/engine-state.json';
const kStatusSentinel = '/run/pi-audio/status';
const kConfigDir = process.env.PI_AUDIO_CONFIG_DIR || '/etc/pi-audio/config';
const kPresetsDir = process.env.PI_AUDIO_PRESETS_DIR || '/etc/pi-audio/presets';
const kCurrentPath = `${kConfigDir}/current.json`;

interface BinaryStatus {
  name: string;
  path: string;
  exists: boolean;
  executable: boolean;
}

interface ServiceStatus {
  name: string;
  active: boolean;
}

export async function handleDebugData(_req: Request, res: Response): Promise<void> {
  try {
    // --- Environment variables ---
    const envVars = [
      'PI_AUDIO_CONFIG_DIR',
      'PI_AUDIO_PRESETS_DIR',
      'PI_AUDIO_CARD',
      'PI_AUDIO_CONTROL',
      'PI_AUDIO_XRUN_FILE',
      'NODE_ENV',
      'HTTP_PORT',
      'WS_PATH',
    ];
    const env: Record<string, string> = {};
    for (const v of envVars) {
      const val = process.env[v];
      if (val !== undefined && val !== '') env[v] = val;
    }

    // --- Version info ---
    let webVersion = 'unknown';
    try {
      const vp = `${process.cwd()}/VERSION.json`;
      if (fs.existsSync(vp)) {
        const vdata = JSON.parse(fs.readFileSync(vp, 'utf8'));
        webVersion = vdata.version || 'unknown';
      }
    } catch {}

    // --- Engine state ---
    let engineState: Record<string, unknown> | null = null;
    try {
      const raw = await fs.promises.readFile(kEngineStateFile, 'utf8');
      engineState = JSON.parse(raw);
    } catch {}

    // --- DSP status (validated) ---
    const dspStatus = await getStatus();

    // --- Current config ---
    let currentConfig: Record<string, unknown> | null = null;
    let rawCurrentConfig = '';
    try {
      const raw = fs.readFileSync(kCurrentPath, 'utf8');
      rawCurrentConfig = raw.trim();
      currentConfig = JSON.parse(raw);
    } catch {}

    // --- Config directory info ---
    const configDirExists = fs.existsSync(kConfigDir);
    const configDirWritable = (() => { try { return fs.accessSync(kConfigDir, fs.constants.W_OK) === undefined; } catch { return false; } })();
    const presetsDirExists = fs.existsSync(kPresetsDir);
    const presetsDirWritable = (() => { try { return fs.accessSync(kPresetsDir, fs.constants.W_OK) === undefined; } catch { return false; } })();

    // --- Preset files ---
    let presetFiles: { name: string; size: number; mtime: string }[] = [];
    if (presetsDirExists) {
      try {
        const entries = fs.readdirSync(kPresetsDir);
        for (const e of entries) {
          if (e.endsWith('.json')) {
            const stat = fs.statSync(`${kPresetsDir}/${e}`);
            presetFiles.push({
              name: e.replace(/\.json$/, ''),
              size: stat.size,
              mtime: stat.mtime.toISOString(),
            });
          }
        }
      } catch {}
    }

    // --- Binary checks ---
    const binariesToCheck: { label: string; path: string }[] = [
      { label: 'Engine', path: '/usr/local/bin/pi-audio-engine' },
    ];
    const binaries: BinaryStatus[] = [];
    for (const b of binariesToCheck) {
      let exists = false;
      let executable = false;
      try {
        const stat = fs.statSync(b.path);
        exists = true;
        executable = (stat.mode & 0o111) !== 0;
      } catch {}
      binaries.push({ name: b.label, path: b.path, exists, executable });
    }

    // --- Service status ---
    const servicesToCheck = ['pi-audio-engine.service', 'pi-audio-web.service', 'pi-audio-ws-server.service'];
    const services: ServiceStatus[] = [];
    for (const svc of servicesToCheck) {
      try {
        const { stdout } = await execFileAsync('systemctl', ['is-active', svc]);
        services.push({ name: svc.replace('.service', ''), active: stdout.trim() === 'active' });
      } catch {
        services.push({ name: svc.replace('.service', ''), active: false });
      }
    }

    // --- ALSA info ---
    let soundCards = '';
    try {
      soundCards = fs.readFileSync('/proc/asound/cards', 'utf8').trim();
    } catch {}

    let alsaDevices = '';
    try {
      alsaDevices = fs.readFileSync('/proc/asound/devices', 'utf8').trim();
    } catch {}

    let amixerControls = '';
    try {
      const { stdout } = await execFileAsync('amixer', ['scontrols'], { timeout: 5000 });
      amixerControls = stdout.trim();
    } catch {}

    // --- Status sentinel ---
    let statusMessage = '';
    try {
      statusMessage = (await fs.promises.readFile(kStatusSentinel, 'utf8')).trim().slice(0, 240);
    } catch {}

    // --- File permissions ---
    const permPaths = [
      kConfigDir, kCurrentPath, kPresetsDir,
      '/var/log/pi-audio', '/run/pi-audio',
      '/usr/local/bin/pi-audio-engine',
    ];
    interface PermInfo { path: string; exists: boolean; perms?: string; owner?: string; group?: string; writable?: boolean }
    const permissions: PermInfo[] = [];
    for (const p of permPaths) {
      try {
        const stat = fs.statSync(p);
        const perms = (stat.mode & 0o7777).toString(8).padStart(4, '0');
        permissions.push({ path: p, exists: true, perms, writable: (stat.mode & 0o200) !== 0 });
      } catch {
        permissions.push({ path: p, exists: false });
      }
    }

    // --- Node.js info ---
    let nodeVersion = '';
    try {
      const { stdout } = await execFileAsync('node', ['--version']);
      nodeVersion = stdout.trim();
    } catch {}

    res.json({
      env,
      webVersion,
      engineState,
      dspStatus: dspStatus.ok ? dspStatus.data : { connected: false, message: dspStatus.error || 'unknown' },
      currentConfig,
      rawCurrentConfig,
      configDirExists,
      configDirWritable,
      presetsDirExists,
      presetsDirWritable,
      presetFiles,
      binaries,
      services,
      soundCards,
      alsaDevices,
      amixerControls,
      statusMessage,
      permissions,
      nodeVersion,
      serverTime: new Date().toISOString(),
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}
