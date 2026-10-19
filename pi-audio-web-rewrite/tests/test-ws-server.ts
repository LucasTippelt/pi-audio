import { describe, it, expect } from 'vitest';
import { CHANNELS, CH_NAMES } from '../src/websocket/ws-server';

describe('WebSocket channel mapping', () => {
  it('has correct channel indices', () => {
    expect(CHANNELS['pgm-l']).toBe(0);
    expect(CHANNELS['pgm-r']).toBe(1);
    expect(CHANNELS['tweeter']).toBe(2);
    expect(CHANNELS['woofer']).toBe(3);
    expect(CHANNELS['mic-1']).toBe(4);
    expect(CHANNELS['mic-6']).toBe(9);
  });

  it('has reverse mapping', () => {
    expect(CH_NAMES[0]).toBe('pgm-l');
    expect(CH_NAMES[2]).toBe('tweeter');
    expect(CH_NAMES[4]).toBe('mic-1');
  });

  it('has 10 channels total', () => {
    expect(Object.keys(CHANNELS).length).toBe(10);
  });
});
