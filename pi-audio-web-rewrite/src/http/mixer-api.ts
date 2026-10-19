import { spawn } from 'child_process';

interface MixerTarget {
  card: string | null;
  control: string;
}

interface MixerStatusResult {
  target: MixerTarget | null;
  message: string;
}

const MIXER_PRIORITY: Record<string, number> = {
  Master: 0,
  PCM: 1,
  Speaker: 2,
  Headphone: 3,
};

let mixerCache: { checkedAt: number; status: MixerStatusResult } | null = null;
const CACHE_TTL_MS = 600_000; // 10 minutes

function runCommand(command: string[], input?: string): Promise<{ ok: boolean; output: string; error: string }> {
  return new Promise((resolve) => {
    const child = spawn(command[0], command.slice(1), { timeout: 10_000 });
    let stdout = '';
    let stderr = '';

    if (input && child.stdin) {
      child.stdin.write(input);
      child.stdin.end();
    }

    child.stdout.on('data', (chunk: Buffer) => { stdout += chunk.toString(); });
    child.stderr.on('data', (chunk: Buffer) => { stderr += chunk.toString(); });
    child.on('close', (code) => {
      resolve({ ok: code === 0, output: stdout.trim(), error: stderr.trim() });
    });
    child.on('error', () => {
      resolve({ ok: false, output: '', error: 'Command unavailable' });
    });
  });
}

async function getCards(): Promise<string[]> {
  const configuredCard = process.env.PI_AUDIO_CARD;
  if (configuredCard) return [configuredCard];

  try {
    const result = await runCommand(['cat', '/proc/asound/cards']);
    if (!result.ok) return ['0'];
    const cards: string[] = [];
    for (const line of result.output.split('\n')) {
      const match = line.match(/^\s*(\d+)\s+\[/);
      if (match) cards.push(match[1]);
    }
    return cards.length > 0 ? cards : ['0'];
  } catch {
    return ['0'];
  }
}

async function probeMixer(): Promise<MixerStatusResult> {
  const cards = await getCards();
  const configuredControl = process.env.PI_AUDIO_CONTROL;
  const devices: Array<{ card: string | null; label: string }> = cards.map((card) => ({
    card,
    label: `card ${card}`,
  }));

  if (!process.env.PI_AUDIO_CARD) {
    devices.push({ card: null, label: 'default device' });
  }

  const errors: string[] = [];

  for (const device of devices) {
    const cmd = ['amixer'];
    if (device.card !== null) cmd.push('-c', device.card);

    const scontrolsResult = await runCommand([...cmd, 'scontrols']);
    if (!scontrolsResult.ok) {
      errors.push(`${device.label}: ${scontrolsResult.error || scontrolsResult.output}`);
      continue;
    }

    const matches = scontrolsResult.output.matchAll(/Simple mixer control '([^']+)'/g);
    let available = [...new Set([...matches].map((m) => m[1]))];

    if (configuredControl) {
      available = available.filter((c) => c === configuredControl);
    } else {
      available.sort((a, b) => (MIXER_PRIORITY[a] ?? 4) - (MIXER_PRIORITY[b] ?? 4));
    }

    for (const control of available) {
      const details = await runCommand([...cmd, 'sget', control]);
      if (!details.ok) continue;

      if (!configuredControl && !/Playback channels:|\b(?:pvolume|pswitch)\b/i.test(details.output)) {
        continue;
      }

      return { target: { card: device.card, control }, message: '' };
    }
  }

  const errorText = errors.join('\n');
  let message: string;
  if (errorText.includes('Command unavailable')) {
    message = 'amixer is unavailable. Install alsa-utils and ensure it is on PATH.';
  } else if (/Invalid card number|Permission denied|cannot find card/i.test(errorText)) {
    message = 'Cannot open the ALSA card. Grant access to /dev/snd (audio group) and check PrivateDevices.';
  } else if (configuredControl) {
    message = `ALSA control "${configuredControl}" was not found; check PI_AUDIO_CONTROL.`;
  } else {
    message = 'No ALSA playback mixer was found; check the VM sound device or ALSA permissions.';
  }

  const detail = errorText.replace(/\s+/g, ' ').trim();
  if (detail) message += ` Probe result: ${detail.slice(0, 240)}`;

  return { target: null, message };
}

export async function mixerStatus(): Promise<MixerStatusResult> {
  if (mixerCache && Date.now() - mixerCache.checkedAt < CACHE_TTL_MS) {
    return mixerCache.status;
  }
  const status = await probeMixer();
  mixerCache = { checkedAt: Date.now(), status };
  return status;
}

export async function mixerCommand(
  arguments_: string[],
  target?: MixerTarget | null,
): Promise<{ ok: boolean; output: string; error: string }> {
  if (!target) return { ok: false, output: '', error: 'No ALSA playback volume control found' };

  const cmd = ['amixer'];
  if (target.card !== null) cmd.push('-c', target.card);
  return runCommand([...cmd, ...arguments_]);
}

export function clearMixerCache(): void {
  mixerCache = null;
}
