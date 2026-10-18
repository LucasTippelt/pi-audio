import fs from 'fs';

const kReloadTrigger = '/run/pi-audio/reload';
const kStateFile = '/run/pi-audio/engine-state.json';
const kConfigPath = '/etc/pi-audio/config/current.json';

export async function applyConfiguration(
  configJson: string,
  timeoutMs = 30_000,
): Promise<{ ok: boolean; message: string }> {
  if (!configJson || !configJson.trim()) {
    return { ok: false, message: 'No configuration data provided.' };
  }

  let config: Record<string, unknown>;
  try {
    config = JSON.parse(configJson);
  } catch {
    return { ok: false, message: 'Invalid JSON in configuration data.' };
  }

  const expectedSampleRate = config.sampleRate !== undefined && config.sampleRate !== null
    ? Number(config.sampleRate) : undefined;
  const expectedBufferSize = config.BufferSize !== undefined && config.BufferSize !== null
    ? Number(config.BufferSize) : undefined;

  try {
    const tmp = kReloadTrigger + '.tmp';
    await fs.promises.writeFile(tmp, kConfigPath);
    await fs.promises.rename(tmp, kReloadTrigger);
  } catch (e: any) {
    return { ok: false, message: `Failed to signal engine reload: ${e.message}` };
  }

  const pollInterval = 100;
  const deadline = Date.now() + timeoutMs;

  while (Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, pollInterval));

    try {
      const raw = await fs.promises.readFile(kStateFile, 'utf8');
      const state = JSON.parse(raw);

      if (state.reloading === true || state.reloading === undefined) {
        /* Engine has picked up the trigger — check values. */
        let matches = true;
        if (expectedSampleRate !== undefined && state.sampleRate !== expectedSampleRate) {
          matches = false;
        }
        if (expectedBufferSize !== undefined && state.BufferSize !== expectedBufferSize) {
          matches = false;
        }

        if (matches || (state.connected === true)) {
          return { ok: true, message: '' };
        }
      }
    } catch {
      /* State file not ready yet — continue polling. */
    }
  }

  return { ok: false, message: `Engine did not confirm reload within ${timeoutMs / 1000}s.` };
}
