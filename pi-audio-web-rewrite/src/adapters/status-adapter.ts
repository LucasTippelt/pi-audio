import fs from 'fs';

interface StatusResponse {
  connected: boolean;
  sampleRate: number | null;
  BufferSize: number | null;
  message: string;
}

const VALID_SAMPLE_RATES = [44100, 48000, 88200, 96000, 176400, 192000];
const VALID_BUFFER_SIZES = [64, 128, 256, 512, 1024, 2048, 4096];

const kStatusSentinel = '/run/pi-audio/status';
const kStateFile = '/run/pi-audio/engine-state.json';

export async function getStatus(): Promise<{
  ok: boolean;
  data?: StatusResponse;
  error?: string;
}> {
  let statusMessage = '';
  try {
    statusMessage = (await fs.promises.readFile(kStatusSentinel, 'utf8')).trim().slice(0, 240);
  } catch {
    /* Sentinel not present — engine may be starting or stopped. */
  }

  let parsed: Record<string, unknown>;
  try {
    const raw = await fs.promises.readFile(kStateFile, 'utf8');
    parsed = JSON.parse(raw);
  } catch {
    return {
      ok: true,
      data: {
        connected: false,
        sampleRate: null,
        BufferSize: null,
        message: statusMessage || 'Engine state file not found',
      },
    };
  }

  if (typeof parsed.connected !== 'boolean') {
    return { ok: false, error: 'Engine state missing boolean connected field.' };
  }
  if (!('sampleRate' in parsed) || !('BufferSize' in parsed)) {
    return { ok: false, error: 'Engine state must include sampleRate and BufferSize fields.' };
  }

  const sampleRate = parsed.sampleRate === null ? null : Number(parsed.sampleRate);
  if (sampleRate !== null && (!Number.isInteger(sampleRate) || !VALID_SAMPLE_RATES.includes(sampleRate))) {
    return { ok: false, error: 'Engine state returned an unsupported sampleRate.' };
  }

  const bufferSize = parsed.BufferSize === null ? null : Number(parsed.BufferSize);
  if (bufferSize !== null && (!Number.isInteger(bufferSize) || !VALID_BUFFER_SIZES.includes(bufferSize))) {
    return { ok: false, error: 'Engine state returned an unsupported BufferSize.' };
  }

  if (parsed.connected === true && sampleRate === null) {
    return { ok: false, error: 'Engine state must report sampleRate when connected.' };
  }
  if (parsed.connected === false && sampleRate !== null) {
    return { ok: false, error: 'Engine state must report a null sampleRate when disconnected.' };
  }

  const message = statusMessage || (typeof parsed.message === 'string' ? String(parsed.message).slice(0, 240) : '');

  return {
    ok: true,
    data: {
      connected: parsed.connected as boolean,
      sampleRate,
      BufferSize: bufferSize,
      message,
    },
  };
}
