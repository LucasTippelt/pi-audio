import fs from 'fs';
import path from 'path';
import { ConfigState, PresetFile, DEFAULT_STATE } from './schema';
import { normalizeState } from './validation';

export function getConfigDir(): string { return process.env.PI_AUDIO_CONFIG_DIR || '/etc/pi-audio/config'; }
export function getPresetsDir(): string { return process.env.PI_AUDIO_PRESETS_DIR || '/etc/pi-audio/presets'; }
export function getCurrentPath(): string { return path.join(getConfigDir(), 'current.json'); }

function presetPath(name: string): string {
  return path.join(getPresetsDir(), `${name}.json`);
}

function findPresetPath(name: string): string | null {
  try {
    const files = fs.readdirSync(getPresetsDir()).filter((f) => f.endsWith('.json'));
    for (const file of files) {
      if (path.basename(file, '.json') === name) return path.join(getPresetsDir(), file);
    }
  } catch {
    // Directory doesn't exist yet
  }
  return null;
}

export function ensureDirectory(dirPath: string): void {
  if (!fs.existsSync(dirPath)) {
    fs.mkdirSync(dirPath, { recursive: true, mode: 0o770 });
  }
  try {
    fs.accessSync(dirPath, fs.constants.W_OK);
  } catch {
    throw new Error(`Cannot write to ${dirPath}.`);
  }
}

export function readState(): ConfigState {
  const current = getCurrentPath();
  if (!fs.existsSync(current)) return structuredClone(DEFAULT_STATE);
  try {
    const data = JSON.parse(fs.readFileSync(current, 'utf8'));
    if (typeof data === 'object' && data !== null && !Array.isArray(data)) {
      return normalizeState(data as Record<string, unknown>, DEFAULT_STATE);
    }
  } catch {
    // Invalid JSON falls through to default
  }
  return structuredClone(DEFAULT_STATE);
}

export function currentJsonIsValid(): boolean {
  const current = getCurrentPath();
  try {
    const contents = fs.readFileSync(current, 'utf8');
    return typeof JSON.parse(contents) === 'object';
  } catch {
    return false;
  }
}

function writeJson(filePath: string, data: unknown): void {
  const directory = path.dirname(filePath);
  ensureDirectory(directory);
  const json = JSON.stringify(data, null, 2) + '\n';
  const tempPath = `${filePath}.tmp.${Date.now()}`;
  fs.writeFileSync(tempPath, json, { mode: 0o664 });
  try {
    fs.renameSync(tempPath, filePath);
    const written = fs.readFileSync(filePath, 'utf8');
    if (written !== json) {
      throw new Error(`Could not verify that ${filePath} was written.`);
    }
  } catch {
    try { fs.unlinkSync(tempPath); } catch {}
    throw new Error(`Could not write ${filePath}.`);
  }
}

export function writeCurrentJson(state: ConfigState): boolean {
  writeJson(getCurrentPath(), state);
  return true;
}

export function readPresetVersions(name: string): PresetFile {
  const filePath = findPresetPath(name);
  if (!filePath) throw new Error('Preset not found.');

  const saved = JSON.parse(fs.readFileSync(filePath, 'utf8'));
  if (typeof saved !== 'object' || saved === null) throw new Error('Preset file is not valid JSON.');

  if (!('staging' in saved) && !('production' in saved)) {
    const legacy = normalizeState(saved as Record<string, unknown>, DEFAULT_STATE);
    const versions: PresetFile = { staging: legacy, production: legacy };
    writeJson(filePath, versions);
    return versions;
  }

  if (typeof saved.staging !== 'object' || typeof saved.production !== 'object') {
    throw new Error('Preset must contain staging and production configurations.');
  }

  return {
    staging: normalizeState(saved.staging as Record<string, unknown>, DEFAULT_STATE),
    production: normalizeState(saved.production as Record<string, unknown>, DEFAULT_STATE),
  };
}

export function presetNames(): string[] {
  const names: string[] = [];
  try {
    for (const file of fs.readdirSync(getPresetsDir()).filter((f) => f.endsWith('.json'))) {
      names.push(path.basename(file, '.json'));
    }
  } catch {
    // Directory doesn't exist yet
  }

  if (!names.includes('Default') && findPresetPath('Default') === null) {
    names.push('Default');
  }

  return names.sort((a, b) => {
    const ai = a.toLowerCase();
    const bi = b.toLowerCase();
    if (ai < bi) return -1;
    if (ai > bi) return 1;
    return a.localeCompare(b);
  });
}

export function savePresetVersions(name: string, versions: PresetFile): void {
  writeJson(presetPath(name), versions);
}

export function savePresetStaging(name: string, state: ConfigState): void {
  const versions = readPresetVersions(name);
  versions.staging = state;
  savePresetVersions(name, versions);
}

export function ensureDefaultPreset(): { names: string[]; selected: string; version: 'staging' | 'production' } {
  ensureDirectory(getPresetsDir());

  if (findPresetPath('Default') === null) {
    const initial = fs.existsSync(getCurrentPath()) ? readState() : normalizeState(DEFAULT_STATE as unknown as Record<string, unknown>, DEFAULT_STATE);
    savePresetVersions('Default', { staging: initial, production: initial });
  }

  const names = presetNames();
  return { names, selected: 'Default', version: 'staging' };
}

export function deletePreset(name: string): boolean {
  if (name === 'Default') throw new Error('The protected Default preset cannot be deleted.');
  const filePath = findPresetPath(name);
  if (!filePath) return false;
  fs.unlinkSync(filePath);
  return true;
}

export function deleteAllPresets(): string[] {
  ensureDirectory(getPresetsDir());
  const errors: string[] = [];
  try {
    for (const file of fs.readdirSync(getPresetsDir()).filter((f) => f.endsWith('.json'))) {
      if (file === 'Default.json') continue;
      const filePath = path.join(getPresetsDir(), file);
      try { fs.unlinkSync(filePath); } catch (e: any) { errors.push(file + ': ' + e.message); }
    }
  } catch (e: any) {
    errors.push(e.message);
  }
  return errors;
}
