import { describe, it, expect } from 'vitest';
import * as statusAdapter from '../src/adapters/status-adapter';
import * as applyAdapter from '../src/adapters/apply-adapter';

describe('status adapter', () => {
  it('returns disconnected when state file not accessible', async () => {
    const result = await statusAdapter.getStatus();
    expect(result.ok).toBe(true);
    if (result.data) {
      expect(result.data.connected).toBe(false);
      expect(result.data.sampleRate).toBeNull();
      expect(result.data.BufferSize).toBeNull();
    }
  });
});

describe('apply adapter', () => {
  it('rejects empty configuration', async () => {
    const result = await applyAdapter.applyConfiguration('');
    expect(result.ok).toBe(false);
    expect(result.message).toContain('No configuration data');
  });

  it('rejects invalid JSON', async () => {
    const result = await applyAdapter.applyConfiguration('not json at all');
    expect(result.ok).toBe(false);
    expect(result.message).toContain('Invalid JSON');
  });
});
