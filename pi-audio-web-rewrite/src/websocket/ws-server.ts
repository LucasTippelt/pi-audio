import { WebSocketServer, WebSocket } from 'ws';
import http from 'http';
import fs from 'fs';
import path from 'path';
import { IncomingMessage } from 'http';

export const CHANNELS: Record<string, number> = {
  'pgm-l': 0,
  'pgm-r': 1,
  'woofer': 2,
  'tweeter': 3,
  'mic-1': 4,
  'mic-2': 5,
  'mic-3': 6,
  'mic-4': 7,
  'mic-5': 8,
  'mic-6': 9,
};

export const CH_NAMES: Record<number, string> = {};
for (const [name, idx] of Object.entries(CHANNELS)) {
  CH_NAMES[idx] = name;
}

const PING_INTERVAL = 20_000; // 20s
const PING_TIMEOUT = 5_000; // 5s

let versionInfo = { version: 'unknown' };
try {
  const vp = path.join(process.cwd(), 'VERSION.json');
  if (fs.existsSync(vp)) versionInfo = JSON.parse(fs.readFileSync(vp, 'utf8'));
} catch {}

export interface WSServerOptions {
  httpServer?: http.Server;
  port?: number;
  path?: string;
  onBroadcast?: (chIdx: number, monoBuf: Buffer) => void;
}

export class WSServer {
  private wss: WebSocketServer;
  private clients: Map<number, Set<WebSocket>> = new Map();
  private pingTimers: Map<WebSocket, ReturnType<typeof setInterval>> = new Map();
  private pongDeadlines: Map<WebSocket, number> = new Map();

  constructor(options?: WSServerOptions) {
    if (options?.httpServer) {
      this.wss = new WebSocketServer({ server: options.httpServer, path: options.path || '/' });
    } else {
      const port = options?.port || 8765;
      const standalone = http.createServer((req, res) => {
        if (req.url === '/version') {
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify(versionInfo));
        } else if (req.url === '/health') {
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ status: 'ok', wsPort: port }));
        } else {
          res.writeHead(404);
          res.end();
        }
      });
      this.wss = new WebSocketServer({ server: standalone, port });
    }

    // Initialize client sets for all channels
    for (const idx of Object.values(CHANNELS)) {
      this.clients.set(idx, new Set());
    }

    if (process.env.NODE_ENV !== 'production') {
      console.log(`pi-audio-ws-server v${versionInfo.version}`);
    }

    this.wss.on('connection', (ws, req) => this.handleConnection(ws, req));
  }

  private handleConnection(ws: WebSocket, req: IncomingMessage): void {
    const url = new URL(req.url || '/', 'http://localhost');
    const ch = url.searchParams.get('ch') || 'pgm-l';
    const chIdx = CHANNELS[ch] != null ? CHANNELS[ch] : 0;
    const peer = `${req.socket.remoteAddress}:${req.socket.remotePort}`;

    if (process.env.NODE_ENV !== 'production') {
      console.log(`ws-connect ${peer} ch=${ch} idx=${chIdx}`);
    }

    // Get engine settings from engine-state.json instead of HTTP fetch
    let settings: { sampleRate: number; bufferSize: number } | null = null;
    try {
      const statePath = process.env.PI_AUDIO_STATE_FILE || '/run/pi-audio/engine-state.json';
      if (fs.existsSync(statePath)) {
        const data = JSON.parse(fs.readFileSync(statePath, 'utf8'));
        settings = {
          sampleRate: Number(data.sampleRate) || 48000,
          bufferSize: Number(data.BufferSize) || 512,
        };
      }
    } catch (e: any) {
      if (process.env.NODE_ENV !== 'production') {
        console.log(`[CONFIG] engine state unavailable: ${e.message}`);
      }
    }

    if (!settings && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ ok: false, error: 'Engine status unavailable' }));
      ws.close(1011, 'Engine status unavailable');
      return;
    }

    if (ws.readyState !== WebSocket.OPEN) return;

    // Register client for channel
    const chClients = this.clients.get(chIdx);
    if (!chClients) {
      this.clients.set(chIdx, new Set());
    }
    this.clients.get(chIdx)!.add(ws);

    // Send settings
    ws.send(JSON.stringify({ ok: true, ...(settings || { sampleRate: 48000, bufferSize: 512 }) }));

    // Ping/pong keepalive
    const pingTimer = setInterval(() => {
      if (ws.readyState === WebSocket.OPEN) {
        this.pongDeadlines.set(ws, Date.now() + PING_TIMEOUT);
        ws.ping();
      } else {
        clearInterval(pingTimer);
      }
    }, PING_INTERVAL);
    this.pingTimers.set(ws, pingTimer);

    ws.on('pong', () => {
      this.pongDeadlines.set(ws, Date.now() + PING_TIMEOUT);
    });

    ws.on('close', () => {
      clearInterval(pingTimer);
      this.pingTimers.delete(ws);
      this.pongDeadlines.delete(ws);
      const idx = this.clients.get(chIdx);
      if (idx) idx.delete(ws);
      console.log(`ws-disconnect ${peer}`);
    });

    ws.on('error', (e) => {
      clearInterval(pingTimer);
      this.pingTimers.delete(ws);
      this.pongDeadlines.delete(ws);
      const idx = this.clients.get(chIdx);
      if (idx) idx.delete(ws);
      console.log(`ws-error ${peer}: ${e.message}`);
    });

    // Check for stale connections periodically
    setInterval(() => {
      if (Date.now() > (this.pongDeadlines.get(ws) || 0)) {
        ws.close(1008, 'Ping timeout');
      }
    }, PING_TIMEOUT).unref();
  }

  /** Broadcast mono float32 buffer to all clients subscribed to a channel */
  public broadcast(chIdx: number, monoBuf: Buffer): void {
    const chClients = this.clients.get(chIdx);
    if (!chClients || chClients.size === 0) return;

    for (const ws of chClients) {
      if (ws.readyState === WebSocket.OPEN) {
        try {
          ws.send(monoBuf);
        } catch (e: any) {
          console.log(`[BROADCAST] send error on ch${chIdx}: ${e.message}`);
          chClients.delete(ws);
        }
      } else {
        chClients.delete(ws);
      }
    }
  }

  /** Get client count for a channel */
  public getClientCount(chIdx: number): number {
    return this.clients.get(chIdx)?.size || 0;
  }

  /** Close the WebSocket server */
  public close(): void {
    for (const [ws, timer] of this.pingTimers) {
      clearInterval(timer);
      ws.close();
    }
    this.wss.close();
  }
}
