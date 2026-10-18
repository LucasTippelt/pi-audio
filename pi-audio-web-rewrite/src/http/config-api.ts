import fs from 'fs';
import { execFile } from 'child_process';
import { promisify } from 'util';
import { Request, Response } from 'express';
import * as presets from '../config/presets';
import { normalizeState, stateSignature, validPresetName } from '../config/validation';
import { ConfigState, PresetVersion, DEFAULT_STATE } from '../config/schema';
import { mixerStatus, mixerCommand } from './mixer-api';
import { applyConfiguration } from '../adapters/apply-adapter';

const execFileAsync = promisify(execFile);
const TEST_SPEAKER_DRIVERS = ['Tweeter', 'Woofer', 'PGM_L', 'PGM_R', 'Mic_1', 'Mic_2', 'Mic_3', 'Mic_4', 'Mic_5', 'Mic_6'];

/* C1: Test mode mapping — matches DSP core m_testMode values. */
const TEST_MODE_MAP: Record<string, number> = {
    Tweeter: 1, Woofer: 2, PGM_L: 3, PGM_R: 3,
};

const kTestCommandPath = '/run/pi-audio/test-command';
const kTestStopPath = '/run/pi-audio/test-stop';

/* Write IPC command file for engine test control thread. */
async function writeTestCommand(mode: number, durationSec: number): Promise<boolean> {
    try {
        const tmp = kTestCommandPath + '.tmp';
        await fs.promises.writeFile(tmp, JSON.stringify({ mode, duration: durationSec }));
        await fs.promises.rename(tmp, kTestCommandPath);
        return true;
    } catch {
        return false;
    }
}

/* Signal engine to stop current test early. */
async function writeTestStop(): Promise<boolean> {
    try {
        await fs.promises.writeFile(kTestStopPath, '1');
        return true;
    } catch {
        return false;
    }
}

const kPcmNameRe = /^[A-Za-z0-9_.:,-=]+$/;
const pcmCache = { inputs: '', outputs: '' };

async function listPcmDevices(utility: 'arecord' | 'aplay'): Promise<string[]> {
  const key = utility === 'arecord' ? 'inputs' : 'outputs';
  try {
    const { stdout } = await execFileAsync(utility, ['-L'], { timeout: 5000 });
    const lines = stdout.split('\n');
    const devices: string[] = ['default'];
    for (const raw of lines) {
      const line = raw.trim();
      if (line && !raw.startsWith(' ') && kPcmNameRe.test(line)) {
        if (!devices.includes(line)) devices.push(line);
      }
    }
    pcmCache[key] = stdout;
    return devices;
  } catch {
    return ['default'];
  }
}

interface SessionData {
  selectedPreset: string;
  presetVersion: PresetVersion;
  currentSource: { name: string; version: PresetVersion; saved: boolean };
  activeConfiguration: { name: string; version: PresetVersion; saved: boolean } | null;
  activeConfigurationSignature: string | null;
  lastLoad: { snapshot: string; at: string } | null;
  lastApplyAttempt: { name: string; version: PresetVersion; success: boolean; at: string } | null;
}

const sessions = new Map<string, SessionData>();

function getSession(req: Request): SessionData {
  const sessionId = req.cookies?.session_id || 'default';
  let session = sessions.get(sessionId);
  if (!session) {
    session = {
      selectedPreset: 'Default',
      presetVersion: 'staging',
      currentSource: { name: 'Default', version: 'staging', saved: true },
      activeConfiguration: null,
      activeConfigurationSignature: null,
      lastLoad: null,
      lastApplyAttempt: null,
    };
    sessions.set(sessionId, session);
  }
  return session;
}

function refreshActiveSaved(session: SessionData, name: string, version: PresetVersion, snapshot: ConfigState): void {
  const active = session.activeConfiguration;
  const signature = session.activeConfigurationSignature;
  if (!active || !signature || active.name !== name || active.version !== version) return;
  active.saved = signature === stateSignature(snapshot, version);
}

export async function handleGetState(req: Request, res: Response): Promise<void> {
  try {
    const session = getSession(req);
    presets.ensureDefaultPreset();
    const versions = presets.readPresetVersions(session.selectedPreset);
    const state = fs.existsSync(presets.getCurrentPath()) ? presets.readState() : versions[session.presetVersion];

    if (!fs.existsSync(presets.getCurrentPath())) {
      presets.writeCurrentJson(state);
    }

    if (!session.currentSource) {
      const snapshot = versions[session.presetVersion];
      const matches = session.presetVersion === 'production'
        ? state.input === snapshot.input && state.sampleRate === snapshot.sampleRate && state.BufferSize === snapshot.BufferSize
        : JSON.stringify(state) === JSON.stringify(snapshot);
      session.currentSource = { name: session.selectedPreset, version: session.presetVersion, saved: matches };
    }

    const [inputs, outputs, mixer] = await Promise.all([
      listPcmDevices('arecord'),
      listPcmDevices('aplay'),
      mixerStatus(),
    ]);

    const alsaReady = mixer.target !== null;
    let alsaControl: string | null = null;
    if (alsaReady && mixer.target) {
      alsaControl = `${mixer.target.control}${mixer.target.card !== null ? ` (card ${mixer.target.card})` : ''}`;
    }

    res.json({
      state,
      currentJsonWritten: presets.currentJsonIsValid(),
      inputs,
      outputs,
      presets: presets.presetNames(),
      selectedPreset: session.selectedPreset,
      presetVersion: session.presetVersion,
      currentSource: session.currentSource,
      lastLoad: session.lastLoad,
      activeConfiguration: session.activeConfiguration,
      lastApplyAttempt: session.lastApplyAttempt,
      csrf: '',
      measurementReady: false,
      applyReady: false,
      alsaReady,
      alsaControl,
      alsaMessage: mixer.message,
      serverTime: Math.floor(Date.now() / 1000),
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export function handleSave(req: Request, res: Response): void {
  try {
    const session = getSession(req);
    const body = req.body as Record<string, unknown>;
    const name = (body.presetName as string) || session.selectedPreset;
    const version = (body.presetVersion as PresetVersion) || session.presetVersion;

    if (!validPresetName(name)) {
      res.status(422).json({ error: 'Select a valid preset before saving.' });
      return;
    }
    if (!['staging', 'production'].includes(version)) {
      res.status(422).json({ error: 'Invalid preset version.' });
      return;
    }

    const snapshotSaved = false;
    let state: ConfigState;

    if (version === 'production') {
      const versions = presets.readPresetVersions(name);
      state = normalizeState({
        input: body.input ?? null,
        sampleRate: body.sampleRate ?? null,
        BufferSize: body.BufferSize ?? null,
      }, versions.production);
    } else {
      state = normalizeState(body, presets.readState());
    }

    session.selectedPreset = name;
    session.presetVersion = version;
    session.currentSource = { name, version, saved: false };
    const currentJsonWritten = presets.writeCurrentJson(state);

    res.json({
      state,
      currentSource: session.currentSource,
      activeConfiguration: session.activeConfiguration,
      currentJsonWritten,
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export function handleSaveStaging(req: Request, res: Response): void {
  try {
    const session = getSession(req);
    const body = req.body as Record<string, unknown>;
    const name = (body.presetName as string) || session.selectedPreset;
    const version = (body.presetVersion as PresetVersion) || session.presetVersion;

    if (!validPresetName(name)) {
      res.status(422).json({ error: 'Select a valid preset before saving staging.' });
      return;
    }
    if (version !== 'staging') {
      res.status(409).json({ error: 'Switch to staging before saving the staging snapshot.' });
      return;
    }

    const state = normalizeState(body, presets.readState());
    const currentJsonWritten = presets.writeCurrentJson(state);
    presets.savePresetStaging(name, state);
    refreshActiveSaved(session, name, 'staging', state);

    session.selectedPreset = name;
    session.presetVersion = 'staging';
    session.currentSource = { name, version: 'staging', saved: true };

    res.json({
      state,
      currentSource: session.currentSource,
      activeConfiguration: session.activeConfiguration,
      currentJsonWritten,
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export function handleSaveProduction(req: Request, res: Response): void {
  try {
    const session = getSession(req);
    const body = req.body as Record<string, unknown>;
    const name = (body.presetName as string) || session.selectedPreset;
    const version = (body.presetVersion as PresetVersion) || 'production';

    if (!validPresetName(name)) {
      res.status(422).json({ error: 'Select a valid preset before saving production.' });
      return;
    }
    if (version !== 'production') {
      res.status(409).json({ error: 'Switch to production before saving production settings.' });
      return;
    }

    const versions = presets.readPresetVersions(name);
    const production = normalizeState({
      input: body.input ?? null,
      sampleRate: body.sampleRate ?? null,
      BufferSize: body.BufferSize ?? null,
    }, versions.production);
    versions.production = production;
    presets.savePresetVersions(name, versions);
    refreshActiveSaved(session, name, 'production', production);

    const state = normalizeState(body, presets.readState());
    state.input = production.input;
    state.sampleRate = production.sampleRate;
    state.BufferSize = production.BufferSize;
    const currentJsonWritten = presets.writeCurrentJson(state);

    session.selectedPreset = name;
    session.presetVersion = 'production';
    session.currentSource = { name, version: 'production', saved: true };

    res.json({
      state,
      currentSource: session.currentSource,
      activeConfiguration: session.activeConfiguration,
      currentJsonWritten,
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export async function handleMaster(req: Request, res: Response): Promise<void> {
  try {
    const session = getSession(req);
    const body = req.body as Record<string, unknown>;
    const name = (body.presetName as string) || session.selectedPreset;
    const version = (body.presetVersion as PresetVersion) || 'staging';

    if (!validPresetName(name)) {
      res.status(422).json({ error: 'Select a valid preset before editing volume.' });
      return;
    }
    if (!['staging', 'production'].includes(version)) {
      res.status(422).json({ error: 'Invalid preset version.' });
      return;
    }

    const state = normalizeState(body, presets.readState());
    session.selectedPreset = name;
    session.presetVersion = version;
    session.currentSource = { name, version, saved: false };
    const currentJsonWritten = presets.writeCurrentJson(state);

    const alsa = await mixerStatus();
    const target = alsa.target;
    let alsaApplied = false;
    let alsaMessage = '';

    if (target) {
      const result = await mixerCommand(
        ['sset', target.control, state.muted ? 'mute' : `${state.volume}%`],
        target,
      );
      alsaApplied = result.ok;
      alsaMessage = result.ok ? '' : 'Saved; no ALSA playback volume control was found.';
    }

    res.json({
      state,
      currentSource: session.currentSource,
      currentJsonWritten,
      alsaApplied,
      alsaMessage,
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export async function handleApply(req: Request, res: Response): Promise<void> {
  try {
    const session = getSession(req);
    const body = req.body as Record<string, unknown>;
    const name = (body.presetName as string) || session.selectedPreset;
    const version = (body.presetVersion as PresetVersion) || 'staging';

    if (!validPresetName(name)) {
      res.status(422).json({ error: 'Select a valid preset version before applying.' });
      return;
    }
    if (!['staging', 'production'].includes(version)) {
      res.status(422).json({ error: 'Invalid preset version.' });
      return;
    }

    const state = normalizeState(body, presets.readState());
    session.selectedPreset = name;
    session.presetVersion = version;
    const versions = presets.readPresetVersions(name);
    const saved = stateSignature(state, version) === stateSignature(versions[version], version);
    session.currentSource = { name, version, saved };
    const currentJsonWritten = presets.writeCurrentJson(state);

    const now = new Date().toISOString();
    session.lastLoad = { snapshot: saved ? `${name} ${version}` : 'unsaved current.json', at: now };
    session.lastApplyAttempt = { name, version, success: false, at: now };

    const result = await applyConfiguration(JSON.stringify(state));
    if (!result.ok) {
      res.status(502).json({
        error: `Configuration saved, but ${result.message}`,
        currentJsonWritten,
        lastLoad: session.lastLoad,
        lastApplyAttempt: session.lastApplyAttempt,
      });
      return;
    }

    session.lastApplyAttempt.success = true;
    session.activeConfiguration = { name, version, saved };
    session.activeConfigurationSignature = stateSignature(state, version);

    res.json({
      state,
      currentJsonWritten,
      currentSource: session.currentSource,
      activeConfiguration: session.activeConfiguration,
      lastApplyAttempt: session.lastApplyAttempt,
      lastLoad: session.lastLoad,
      applied: true,
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export function handlePresetSave(req: Request, res: Response): void {
  try {
    const session = getSession(req);
    const body = req.body as Record<string, unknown>;
    const name = String(body.name || '').trim();

    if (!validPresetName(name)) {
      res.status(422).json({ error: 'Use 1-32 letters, numbers, spaces, hyphens or underscores.' });
      return;
    }
    if (name === 'Default') {
      res.status(409).json({ error: 'The protected Default preset cannot be overwritten.' });
      return;
    }

    const state = normalizeState((body.state as Record<string, unknown>) || {}, presets.readState());
    presets.savePresetVersions(name, { staging: state, production: state });
    session.selectedPreset = name;
    session.presetVersion = 'staging';
    const currentJsonWritten = presets.writeCurrentJson(state);
    session.currentSource = { name, version: 'staging', saved: true };

    res.json({
      presets: presets.presetNames(),
      selectedPreset: name,
      presetVersion: 'staging',
      currentSource: session.currentSource,
      currentJsonWritten,
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export function handlePresetSelect(req: Request, res: Response): void {
  try {
    const session = getSession(req);
    const body = req.body as Record<string, unknown>;
    const name = String(body.name || '');
    const version = (body.version as PresetVersion) || 'staging';

    if (!validPresetName(name)) {
      res.status(404).json({ error: 'Preset not found.' });
      return;
    }
    if (!['staging', 'production'].includes(version)) {
      res.status(422).json({ error: 'Invalid preset version.' });
      return;
    }

    const versions = presets.readPresetVersions(name);
    session.selectedPreset = name;
    session.presetVersion = version;
    const state = versions[version];
    const now = new Date().toISOString();
    session.lastLoad = { snapshot: `${name} ${version}`, at: now };
    const currentJsonWritten = presets.writeCurrentJson(state);
    session.currentSource = { name, version, saved: true };

    res.json({
      state,
      selectedPreset: name,
      presetVersion: version,
      lastLoad: session.lastLoad,
      currentSource: session.currentSource,
      currentJsonWritten,
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export function handlePresetPromote(req: Request, res: Response): void {
  try {
    const session = getSession(req);
    const body = req.body as Record<string, unknown>;
    const name = String(body.name || '');

    if (!validPresetName(name)) {
      res.status(404).json({ error: 'Preset not found.' });
      return;
    }
    if ((body.version as string) !== 'staging') {
      res.status(409).json({ error: 'Only staging can be promoted to production.' });
      return;
    }

    const versions = presets.readPresetVersions(name);
    versions.production = structuredClone(versions.staging);
    presets.savePresetVersions(name, versions);
    refreshActiveSaved(session, name, 'production', versions.production);
    session.selectedPreset = name;
    session.presetVersion = 'production';
    const currentJsonWritten = presets.writeCurrentJson(versions.production);
    session.currentSource = { name, version: 'production', saved: true };
    const now = new Date().toISOString();
    session.lastLoad = { snapshot: `${name} Production`, at: now };

    res.json({
      state: versions.production,
      selectedPreset: name,
      presetVersion: 'production',
      currentSource: session.currentSource,
      activeConfiguration: session.activeConfiguration,
      lastLoad: session.lastLoad,
      currentJsonWritten,
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export function handlePresetCopyProductionToStaging(req: Request, res: Response): void {
  try {
    const session = getSession(req);
    const body = req.body as Record<string, unknown>;
    const name = String(body.name || '');

    if (!validPresetName(name)) {
      res.status(404).json({ error: 'Preset not found.' });
      return;
    }
    if ((body.version as string) !== 'production') {
      res.status(409).json({ error: 'Select production before copying it to staging.' });
      return;
    }

    const versions = presets.readPresetVersions(name);
    versions.staging = structuredClone(versions.production);
    presets.savePresetVersions(name, versions);
    refreshActiveSaved(session, name, 'staging', versions.staging);
    session.selectedPreset = name;
    session.presetVersion = 'staging';
    const currentJsonWritten = presets.writeCurrentJson(versions.staging);
    session.currentSource = { name, version: 'staging', saved: true };
    const now = new Date().toISOString();
    session.lastLoad = { snapshot: `${name} Staging`, at: now };

    res.json({
      state: versions.staging,
      selectedPreset: name,
      presetVersion: 'staging',
      currentSource: session.currentSource,
      activeConfiguration: session.activeConfiguration,
      lastLoad: session.lastLoad,
      currentJsonWritten,
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export function handlePresetDelete(req: Request, res: Response): void {
  try {
    const session = getSession(req);
    const body = req.body as Record<string, unknown>;
    const name = String(body.name || '');

    if (!validPresetName(name)) {
      res.status(422).json({ error: 'Invalid preset name.' });
      return;
    }

    presets.deletePreset(name);

    let selectedState: ConfigState | null = null;
    let currentJsonWritten: boolean | null = null;

    if (session.selectedPreset === name) {
      session.selectedPreset = 'Default';
      session.presetVersion = 'staging';
      const versions = presets.readPresetVersions('Default');
      selectedState = versions.staging;
      currentJsonWritten = presets.writeCurrentJson(selectedState);
      session.currentSource = { name: 'Default', version: 'staging', saved: true };
      session.lastLoad = { snapshot: 'Default Staging', at: new Date().toISOString() };
    }

    if (session.activeConfiguration?.name === name) {
      session.activeConfiguration.saved = false;
    }

    const response: Record<string, unknown> = {
      presets: presets.presetNames(),
      selectedPreset: session.selectedPreset,
      presetVersion: session.presetVersion,
      currentSource: session.currentSource,
      activeConfiguration: session.activeConfiguration,
      lastLoad: session.lastLoad,
    };
    if (selectedState) response.state = selectedState;
    if (currentJsonWritten !== null) response.currentJsonWritten = currentJsonWritten;

    res.json(response);
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export function handlePresetDeleteAll(req: Request, res: Response): void {
  try {
    const session = getSession(req);
    presets.ensureDefaultPreset();
    const errors = presets.deleteAllPresets();
    session.selectedPreset = 'Default';
    session.presetVersion = 'staging';
    const versions = presets.readPresetVersions('Default');
    const state = versions.staging;
    if (session.activeConfiguration && session.activeConfiguration.name !== 'Default') {
      session.activeConfiguration.saved = false;
    }
    refreshActiveSaved(session, 'Default', 'staging', state);
    const currentJsonWritten = presets.writeCurrentJson(state);
    session.currentSource = { name: 'Default', version: 'staging', saved: true };
    session.lastLoad = { snapshot: 'Default Staging', at: new Date().toISOString() };

    res.json({
      presets: presets.presetNames(),
      selectedPreset: 'Default',
      presetVersion: 'staging',
      currentSource: session.currentSource,
      activeConfiguration: session.activeConfiguration,
      lastLoad: session.lastLoad,
      deleted: errors.length === 0,
      deleteErrors: errors,
      state,
      currentJsonWritten,
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export function handleRestoreDefaults(req: Request, res: Response): void {
  try {
    const session = getSession(req);
    presets.ensureDefaultPreset();
    const state = normalizeState({}, DEFAULT_STATE);
    const versions = presets.readPresetVersions('Default');
    versions.staging = state;
    presets.savePresetVersions('Default', versions);
    refreshActiveSaved(session, 'Default', 'staging', state);
    session.selectedPreset = 'Default';
    session.presetVersion = 'staging';
    const currentJsonWritten = presets.writeCurrentJson(state);
    session.currentSource = { name: 'Default', version: 'staging', saved: true };
    session.lastLoad = { snapshot: 'Default Staging', at: new Date().toISOString() };

    res.json({
      state,
      currentSource: session.currentSource,
      activeConfiguration: session.activeConfiguration,
      lastApplyAttempt: session.lastApplyAttempt,
      lastLoad: session.lastLoad,
      currentJsonWritten,
      alsaApplied: false,
    });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export async function handleTestSpeaker(req: Request, res: Response): Promise<void> {
  try {
    const body = req.body as Record<string, unknown>;
    const driverName = String(body.driver || '');

    if (!TEST_SPEAKER_DRIVERS.includes(driverName)) {
      res.status(422).json({ error: 'Invalid speaker test target.' });
      return;
    }

    const duration = body.duration ? Number(body.duration) : 3;
    if (isNaN(duration) || duration < 0.5) {
      res.status(422).json({ error: 'Duration must be at least 0.5 seconds.' });
      return;
    }

    const mode = TEST_MODE_MAP[driverName];
    if (mode !== undefined) {
      const ipcOk = await writeTestCommand(mode, duration);
      if (ipcOk) {
        res.json({ tested: driverName, mode: 'ipc', duration });
        return;
      }
    }

    res.status(502).json({ error: `Speaker test for ${driverName} requires a running engine.` });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export async function handleStopTestSpeaker(_req: Request, res: Response): Promise<void> {
  try {
    const ok = await writeTestStop();
    if (!ok) {
      res.status(502).json({ error: 'Failed to send stop command.' });
      return;
    }
    res.json({ stopped: true });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}

export async function handleTestPgm(req: Request, res: Response): Promise<void> {
  try {
    const body = req.body as Record<string, unknown>;
    const duration = body.duration ? Number(body.duration) : 3;
    if (isNaN(duration) || duration < 0.5) {
      res.status(422).json({ error: 'Duration must be at least 0.5 seconds.' });
      return;
    }

    const ipcOk = await writeTestCommand(3, duration);
    if (ipcOk) {
      res.json({ tested: 'PGM_L', mode: 'ipc', duration });
      return;
    }

    res.status(502).json({ error: 'PGM test requires a running engine.' });
  } catch (e: any) {
    res.status(500).json({ error: e.message });
  }
}
