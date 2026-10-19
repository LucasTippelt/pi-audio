import express from 'express';
import http from 'http';
import path from 'path';
import fs from 'fs';
import { createRouter } from './http/router';
import { WSServer } from './websocket/ws-server';
import { Broadcaster } from './websocket/broadcaster';

const PORT = parseInt(process.env.HTTP_PORT || '80', 10);
const WS_PATH = process.env.WS_PATH || '/ws';

let versionInfo = { version: 'unknown' };
try {
  const vp = path.join(process.cwd(), 'VERSION.json');
  if (fs.existsSync(vp)) versionInfo = JSON.parse(fs.readFileSync(vp, 'utf8'));
} catch {}

const app = express();
app.use(express.json({ limit: '1mb' }));

// Static frontend files — serve from src/ so HTML is always fresh without copy step.
const projectRoot = path.resolve(__dirname, '..');
const frontendDir = fs.existsSync(path.join(projectRoot, 'src', 'frontend'))
  ? path.join(projectRoot, 'src', 'frontend')
  : path.join(__dirname, 'frontend');

if (fs.existsSync(frontendDir)) {
  // Try src assets first, fall back to dist assets.
  const assetsSrc = path.join(projectRoot, 'src', 'frontend', 'assets');
  const assetsDist = path.join(__dirname, 'frontend', 'assets');
  app.use('/assets', express.static(fs.existsSync(assetsSrc) ? assetsSrc : assetsDist));

  function serveHtml(name: string) {
    return (_req: any, res: any) => {
      const p = path.join(frontendDir, name);
      if (fs.existsSync(p)) res.sendFile(p);
      else res.status(404).json({ error: `${name} not found` });
    };
  }
  app.get('/', serveHtml('index.html'));
  app.get('/debug', serveHtml('debug.html'));
  app.get('/monitoring', serveHtml('monitoring.html'));
}

// API routes
app.use(createRouter());

// Version endpoint
app.get('/version', (_req, res) => {
  res.json(versionInfo);
});

const server = http.createServer(app);

// WebSocket server attached to same HTTP server
const wsServer = new WSServer({ httpServer: server, path: WS_PATH });

// Start SHM broadcaster for real-time audio monitoring
const broadcaster = new Broadcaster();
broadcaster.start(wsServer);

server.listen(PORT, () => {
  console.log(`pi-audio-web v${versionInfo.version} listening on :${PORT}`);
});

process.on('SIGTERM', () => {
  console.log('SIGTERM received, shutting down...');
  broadcaster.stop();
  wsServer.close();
  server.close(() => {
    process.exit(0);
  });
});

process.on('SIGINT', () => {
  console.log('SIGINT received, shutting down...');
  broadcaster.stop();
  wsServer.close();
  server.close(() => {
    process.exit(0);
  });
});

export default app;
