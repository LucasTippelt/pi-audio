import fs from 'fs';

const SHM_PREFIX = '/dev/shm/pi-audio-';
const HEADER_SIZE = 80;
const DATA_SIZE = 4 * 1024 * 1024; // 4MB data region
const MAGIC = Buffer.from('PIAI');

interface ShmHeader {
  magic: string;
  version: number;
  writePos: number;
  sampleRate: number;
  channels: number;
  seq: number;
  generation: number;
  state: number;
  name: string;
}

function parseHeader(buf: Buffer): ShmHeader | null {
  if (buf.length < HEADER_SIZE) return null;

  const magic = buf.toString('ascii', 0, 4);
  if (magic !== 'PIAI') return null;

  return {
    magic,
    version: buf.readUInt16LE(4),
    writePos: Number(buf.readBigUInt64LE(6)),
    sampleRate: buf.readUInt32LE(14),
    channels: buf.readUInt16LE(18),
    seq: buf.readUInt32LE(20),
    generation: buf.readUInt32LE(24),
    state: buf.readUInt32LE(28),
    name: buf.toString('ascii', 32, 48).replace(/\0/g, ''),
  };
}

export interface ShmReadResult {
  samples: Float32Array;
  sampleRate: number;
  channels: number;
  frames: number;
}

/**
 * Read mono float32 samples from an shm ring by channel name.
 * Returns the number of frames actually read (0 on failure).
 */
export function readShmChannel(name: string, maxFrames: number): ShmReadResult | null {
  const filePath = SHM_PREFIX + name;

  let fd: number | null = null;
  try {
    fd = fs.openSync(filePath, 'r');
    const buf = Buffer.allocUnsafe(HEADER_SIZE + Math.min(maxFrames * 4, DATA_SIZE));
    const bytesRead = fs.readSync(fd, buf, 0, buf.length, 0);

    if (bytesRead < HEADER_SIZE) {
      return null;
    }

    const header = parseHeader(buf.slice(0, HEADER_SIZE));
    if (!header || header.state !== 1) {
      return null;
    }

    const channels = Math.max(1, header.channels);
    const bytesPerFrame = sizeofFloat32 * channels;
    const availFrames = Math.floor(header.writePos / bytesPerFrame);
    const framesToRead = Math.min(availFrames, maxFrames);

    if (framesToRead === 0) {
      return null;
    }

    // Read data region starting after header
    const dataBuf = Buffer.allocUnsafe(framesToRead * bytesPerFrame);
    fs.readSync(fd, dataBuf, 0, dataBuf.length, HEADER_SIZE);

    // Convert to mono Float32Array by averaging channels if multi-channel
    const result = new Float32Array(framesToRead);
    for (let i = 0; i < framesToRead; i++) {
      if (channels === 1) {
        result[i] = dataBuf.readFloatLE(i * sizeofFloat32);
      } else {
        let sum = 0;
        for (let ch = 0; ch < channels; ch++) {
          sum += dataBuf.readFloatLE((i * bytesPerFrame) + (ch * sizeofFloat32));
        }
        result[i] = sum / channels;
      }
    }

    return {
      samples: result,
      sampleRate: header.sampleRate,
      channels: header.channels,
      frames: framesToRead,
    };
  } catch {
    return null;
  } finally {
    if (fd !== null) {
      try { fs.closeSync(fd); } catch {}
    }
  }
}

const sizeofFloat32 = 4;
