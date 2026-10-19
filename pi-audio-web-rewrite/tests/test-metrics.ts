import { describe, it, expect } from 'vitest';
import * as metricsApi from '../src/http/metrics-api';

describe('metrics API', () => {
  it('returns valid metrics structure', async () => {
    const result = await metricsApi.getMetrics();
    expect(result).toHaveProperty('cpu');
    expect(result).toHaveProperty('cpuSource');
    expect(result).toHaveProperty('cpuMessage');
    expect(result).toHaveProperty('temperature');
    expect(result).toHaveProperty('xruns');
  });

  it('cpu is null or between 0-100', async () => {
    const result = await metricsApi.getMetrics();
    if (result.cpu !== null) {
      expect(result.cpu).toBeGreaterThanOrEqual(0);
      expect(result.cpu).toBeLessThanOrEqual(100);
    }
  });

  it('temperature is null or a finite number', async () => {
    const result = await metricsApi.getMetrics();
    if (result.temperature !== null) {
      expect(Number.isFinite(result.temperature)).toBe(true);
    }
  });

  it('xruns is null or non-negative integer', async () => {
    const result = await metricsApi.getMetrics();
    if (result.xruns !== null) {
      expect(result.xruns).toBeGreaterThanOrEqual(0);
      expect(Number.isInteger(result.xruns)).toBe(true);
    }
  });

  it('cpuSource matches cpu value', async () => {
    const result = await metricsApi.getMetrics();
    if (result.cpu !== null) {
      expect(['proc-stat', 'loadavg']).toContain(result.cpuSource!);
    } else {
      expect(result.cpuMessage).not.toBe('');
    }
  });
});
