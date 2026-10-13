#!/usr/bin/env node
const WebSocket = require('ws');
const net = require('net');
const http = require('http');
const fs = require('fs');
const path = require('path');

const WS_PORT = 8765;
const TCP_PORT = 8766;
const SAMPLE_RATE = 48000;
const PING_INTERVAL = 20000;
const PING_TIMEOUT = 5000;

/* Load version info */
let versionInfo = { version: 'unknown' };
try {
  const vp = path.join(__dirname, 'VERSION.json');
  if (fs.existsSync(vp)) versionInfo = JSON.parse(fs.readFileSync(vp, 'utf8'));
} catch(e) {}
console.log('pi-audio-ws-server v' + versionInfo.version + ' (built ' + versionInfo.buildTime + ')');

const CHANNELS = {
  'pgm-l': 0,
  'pgm-r': 1,
};

/* HTTP server for version endpoint */
const httpServer = http.createServer((req, res) => {
  if (req.url === '/version') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(versionInfo));
  } else if (req.url === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'ok', tcpPort: TCP_PORT, wsPort: WS_PORT }));
  } else {
    res.writeHead(404);
    res.end();
  }
});

/* WebSocket server: relay to browser clients */
const wss = new WebSocket.Server({ server: httpServer, port: WS_PORT });
console.log('WebSocket server on ws://0.0.0.0:' + WS_PORT);

/* Per-channel WebSocket client lists */
const clients = { 0: [], 1: [] };

wss.on('connection', (ws, req) => {
  const url = new URL(req.url, 'http://localhost');
  const ch = url.searchParams.get('ch') || 'pgm-l';
  const chIdx = CHANNELS[ch] != null ? CHANNELS[ch] : 0;
  const peer = req.socket.remoteAddress + ':' + req.socket.remotePort;
  let framesSent = 0;
  let bytesSent = 0;
  let pongDeadline = Date.now() + PING_TIMEOUT;

  console.log('ws-connect ' + peer + ' ch=' + ch + ' idx=' + chIdx);
  clients[chIdx].push(ws);

  const pingTimer = setInterval(() => {
    if (ws.readyState === WebSocket.OPEN) {
      pongDeadline = Date.now() + PING_TIMEOUT;
      ws.ping();
    } else {
      clearInterval(pingTimer);
    }
  }, PING_INTERVAL);

  ws.on('pong', () => {
    pongDeadline = Date.now() + PING_TIMEOUT;
  });

  ws.on('close', () => {
    clearInterval(pingTimer);
    const idx = clients[chIdx].indexOf(ws);
    if (idx >= 0) clients[chIdx].splice(idx, 1);
    console.log('ws-disconnect ' + peer + ' f=' + framesSent + ' b=' + bytesSent);
  });

  ws.on('error', (e) => {
    clearInterval(pingTimer);
    const idx = clients[chIdx].indexOf(ws);
    if (idx >= 0) clients[chIdx].splice(idx, 1);
    console.log('ws-error ' + peer + ': ' + e.message);
  });

  ws._addStats = (b) => { framesSent++; bytesSent += b; };
});

/* Split interleaved stereo float32 into two mono buffers */
function splitStereo(stereo, sampleCount) {
  const lBuf = Buffer.allocUnsafe(sampleCount * 4);
  const rBuf = Buffer.allocUnsafe(sampleCount * 4);
  for (let i = 0; i < sampleCount; i++) {
    stereo.copy(lBuf, i * 4, i * 8, i * 8 + 4);
    stereo.copy(rBuf, i * 4, i * 8 + 4, i * 8 + 8);
  }
  return [lBuf, rBuf];
}

/* Broadcast mono buffer to all clients for a channel */
function broadcast(chIdx, monoBuf) {
  const chClients = clients[chIdx];
  for (let i = chClients.length - 1; i >= 0; i--) {
    const ws = chClients[i];
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(monoBuf);
      ws._addStats(monoBuf.length);
    } else {
      chClients.splice(i, 1);
    }
  }
}

/* TCP server: accept engine connection */
const tcpServer = net.createServer((socket) => {
  const peer = socket.remoteAddress + ':' + socket.remotePort;
  console.log('tcp-connect ' + peer);

  let buf = Buffer.allocUnsafe(0);
  let tcpBytesTotal = 0;
  let tcpMsgCount = 0;
  const tcpStart = Date.now();
  const tcpTimer = setInterval(() => {
    const elapsed = ((Date.now() - tcpStart) / 1000).toFixed(1);
    const kb = (tcpBytesTotal / 1024).toFixed(0);
    console.log('[TCP] ' + kb + 'kB / ' + elapsed + 's = ' + (tcpBytesTotal / (Date.now() - tcpStart) * 1000 / 1024).toFixed(1) + 'kB/s, msgs=' + tcpMsgCount);
  }, 5000).unref();

  socket.on('data', (data) => {
    buf = Buffer.concat([buf, data]);

    while (buf.length >= 4) {
      const sampleCount = buf.readUInt32BE(0);
      const msgLen = 4 + sampleCount * 8;
      if (buf.length < msgLen) break;

      buf = buf.slice(4);
      const stereo = buf.slice(0, msgLen - 4);
      buf = buf.slice(msgLen - 4);

      tcpBytesTotal += msgLen;
      tcpMsgCount++;

      const [lBuf, rBuf] = splitStereo(stereo, sampleCount);
      broadcast(0, lBuf);
      broadcast(1, rBuf);
    }
  });

  socket.on('close', () => {
    clearInterval(tcpTimer);
    console.log('tcp-disconnect ' + peer);
  });

  socket.on('error', (e) => {
    console.log('tcp-error ' + peer + ': ' + e.message);
  });
});

tcpServer.listen(TCP_PORT, () => {
  console.log('TCP engine relay on 0.0.0.0:' + TCP_PORT);
});
