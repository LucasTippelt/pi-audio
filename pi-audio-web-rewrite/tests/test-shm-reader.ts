import { describe, it, expect } from 'vitest';
import fs from 'fs';
import path from 'path';
import { readShmChannel } from '../src/websocket/shm-reader';

const TEST_SHM_DIR = '/tmp/pi-audio-test-shm';

function createTestShm(name: string, samples: Float32Array): void {
  const filePath = path.join('/dev/shm', `pi-audio-${name}`);
  const headerSize = 80;
  const dataSize = samples.length * 4; // mono float32

  const buf = Buffer.allocUnsafe(headerSize + dataSize);
  buf.fill(0);

  // Write magic "PIAI"
  buf.write('PIAI', 0, 'ascii');

  // Version (uint16 LE)
  buf.writeUInt16LE(1, 4);

  // WritePos (uint64 LE) - bytes written in data region
  const writePos = BigInt(dataSize);
  buf.writeBigUInt64LE(writePos, 6);

  // SampleRate (uint32 LE)
  buf.writeUInt32LE(48000, 14);

  // Channels (uint16 LE) - mono
  buf.writeUInt16LE(1, 18);

  // Seq (uint32 LE)
  buf.writeUInt32LE(1, 20);

  // Generation (uint32 LE)
  buf.writeUInt32LE(1, 24);

  // State (uint32 LE) - 1 = ready
  buf.writeUInt32LE(1, 28);

  // Name (16 bytes)
  buf.write(name, 32, 16, 'ascii');

  // Write sample data after header
  const floatBuf = Buffer.from(samples.buffer, samples.byteOffset, samples.byteLength);
  floatBuf.copy(buf, headerSize);

  fs.writeFileSync(filePath, buf);
}

describe('SHM ring reader', () => {
  it('returns null for non-existent channel', () => {
    const result = readShmChannel('nonexistent-test-ch', 100);
    expect(result).toBeNull();
  });

  it('reads mono samples from valid shm ring', () => {
    const testSamples = new Float32Array([0.5, -0.3, 0.8, -0.1, 0.2]);
    createTestShm('test-mono-ch', testSamples);

    try {
      const result = readShmChannel('test-mono-ch', 10);
      expect(result).not.toBeNull();
      if (result) {
        expect(result.frames).toBe(5);
        expect(result.sampleRate).toBe(48000);
        expect(result.channels).toBe(1);
        expect(result.samples[0]).toBeCloseTo(0.5, 5);
        expect(result.samples[2]).toBeCloseTo(0.8, 5);
      }
    } finally {
      try { fs.unlinkSync('/dev/shm/pi-audio-test-mono-ch'); } catch {}
    }
  });

  it('respects maxFrames limit', () => {
    const testSamples = new Float32Array(100).fill(0.5);
    createTestShm('test-limit-ch', testSamples);

    try {
      const result = readShmChannel('test-limit-ch', 10);
      expect(result).not.toBeNull();
      if (result) {
        expect(result.frames).toBe(10);
      }
    } finally {
      try { fs.unlinkSync('/dev/shm/pi-audio-test-limit-ch'); } catch {}
    }
  });

  it('returns null for invalid magic', () => {
    const filePath = '/dev/shm/pi-audio-test-invalid-ch';
    fs.writeFileSync(filePath, Buffer.alloc(100)); // No valid header

    try {
      const result = readShmChannel('test-invalid-ch', 10);
      expect(result).toBeNull();
    } finally {
      try { fs.unlinkSync(filePath); } catch {}
    }
  });

  it('returns null for state != 1', () => {
    const filePath = '/dev/shm/pi-audio-test-state-ch';
    const buf = Buffer.allocUnsafe(80 + 4 * 10);
    buf.fill(0);
    buf.write('PIAI', 0, 'ascii');
    buf.writeUInt32LE(0, 28); // state = 0 (not ready)

    fs.writeFileSync(filePath, buf);

    try {
      const result = readShmChannel('test-state-ch', 10);
      expect(result).toBeNull();
    } finally {
      try { fs.unlinkSync(filePath); } catch {}
    }
  });
});
