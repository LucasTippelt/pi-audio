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

/* Channel name to index mapping */
const CHANNELS = {
  'pgm-l': 0,
  'pgm-r': 1,
  'woofer-l': 2,
  'tweeter-l': 3,
  'woofer-r': 4,
  'tweeter-r': 5,
  'mic-1': 6,
  'mic-2': 7,
  'mic-3': 8,
  'mic-4': 9,
};

/* Reverse map: index to name */
const CH_NAMES = {};
for (const [name, idx] of Object.entries(CHANNELS)) {
  CH_NAMES[idx] = name;
}

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
const wss = new WebSocket.Server({ server: httpServer });
httpServer.listen(WS_PORT, () => {
  console.log('WebSocket server on ws://0.0.0.0:' + WS_PORT);
});

/* Per-channel WebSocket client lists */
const clients = {};
for (const idx of Object.values(CHANNELS)) {
  clients[idx] = [];
}

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

/* Broadcast mono buffer to all clients for a channel */
function broadcast(chIdx, monoBuf) {
  const chClients = clients[chIdx];
  if (!chClients) return;
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

    while (buf.length >= 5) {
      /* Protocol: [1 byte channel ID][4 bytes BE sample count][sampleCount × 4 bytes mono float32] */
      const channelId = buf.readUInt8(0);
      const sampleCount = buf.readUInt32BE(1);
      const msgLen = 5 + sampleCount * 4;
      if (buf.length < msgLen) break;

      buf = buf.slice(1);
      const monoBuf = buf.slice(4, 4 + sampleCount * 4);
      buf = buf.slice(4 + sampleCount * 4);

      tcpBytesTotal += msgLen;
      tcpMsgCount++;

      const chName = CH_NAMES[channelId] || ('ch-' + channelId);
      broadcast(channelId, monoBuf);
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
