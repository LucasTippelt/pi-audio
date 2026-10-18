import { WSServer, CHANNELS } from './ws-server';
import { readShmChannel } from './shm-reader';

const DEFAULT_SAMPLE_COUNT = 512; // Samples per broadcast cycle
const BROADCAST_INTERVAL_MS = 20; // ~50Hz update rate (48kHz / 512 samples ≈ 10ms, but we use 20ms for safety)

interface RateStats {
  tcpSamples: number;
  tcpMsgs: number;
  wsSamples: number;
  wsMsgs: number;
  skipped: number;
}

export class Broadcaster {
  private wsServer: WSServer | null = null;
  private broadcastTimer: ReturnType<typeof setInterval> | null = null;
  private rateStats: Map<number, RateStats> = new Map();
  private running = false;
  private sampleCount: number;

  constructor(sampleCount?: number) {
    this.sampleCount = sampleCount || DEFAULT_SAMPLE_COUNT;

    // Initialize rate stats for all channels
    for (const idx of Object.values(CHANNELS)) {
      this.rateStats.set(idx, { tcpSamples: 0, tcpMsgs: 0, wsSamples: 0, wsMsgs: 0, skipped: 0 });
    }
  }

  /** Start broadcasting from SHM rings to WebSocket clients */
  public start(wsServer?: WSServer): void {
    if (this.running) return;
    this.running = true;

    if (wsServer) {
      this.wsServer = wsServer;
    }

    console.log(`[BROADCAST] Starting broadcaster with ${this.sampleCount} samples per cycle`);

    // Start periodic broadcast loop
    this.broadcastTimer = setInterval(() => this.broadcastCycle(), BROADCAST_INTERVAL_MS);

    // Log rate stats every 2 seconds
    const rateLogInterval = setInterval(() => this.logRateStats(), 2000).unref();
  }

  private broadcastCycle(): void {
    for (const [name, idx] of Object.entries(CHANNELS)) {
      if (!this.wsServer) continue;

      // Skip channels with no clients
      if (this.wsServer.getClientCount(idx) === 0) continue;

      const result = readShmChannel(name, this.sampleCount);
      if (!result || result.frames === 0) {
        const stats = this.rateStats.get(idx);
        if (stats) stats.skipped++;
        continue;
      }

      // Convert Float32Array to Buffer for WebSocket binary send
      const monoBuf = Buffer.from(result.samples.buffer, result.samples.byteOffset, result.samples.byteLength);

      // Update rate stats
      const stats = this.rateStats.get(idx);
      if (stats) {
        stats.tcpSamples += result.frames;
        stats.tcpMsgs++;
        stats.wsSamples += result.frames;
        stats.wsMsgs++;
      }

      // Broadcast to WebSocket clients
      this.wsServer.broadcast(idx, monoBuf);
    }
  }

  private logRateStats(): void {
    let line = '[RATE]';
    for (const [name, idx] of Object.entries(CHANNELS)) {
      const stats = this.rateStats.get(idx);
      if (!stats || (stats.tcpSamples === 0 && stats.wsSamples === 0)) continue;

      const dt = BROADCAST_INTERVAL_MS / 1000; // seconds per cycle
      const tcpRate = Math.round(stats.tcpSamples / dt);
      const wsRate = Math.round(stats.wsSamples / dt);
      const pct = stats.tcpSamples > 0 ? Math.round((stats.wsSamples / stats.tcpSamples) * 100) : 0;

      line += ` ch${idx} tcp=${tcpRate}/s(${stats.tcpMsgs}m) ws=${wsRate}/s(${stats.wsMsgs}m) skip=${stats.skipped} send=${pct}%`;

      // Reset stats
      stats.tcpSamples = 0;
      stats.tcpMsgs = 0;
      stats.wsSamples = 0;
      stats.wsMsgs = 0;
      stats.skipped = 0;
    }
    console.log(line);
  }

  /** Stop broadcasting */
  public stop(): void {
    this.running = false;
    if (this.broadcastTimer) {
      clearInterval(this.broadcastTimer);
      this.broadcastTimer = null;
    }
    console.log('[BROADCAST] Stopped');
  }

  /** Check if broadcaster is running */
  public isRunning(): boolean {
    return this.running;
  }
}
