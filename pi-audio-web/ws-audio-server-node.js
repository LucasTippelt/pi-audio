#!/usr/bin/env node
const WebSocket = require('ws');
const net = require('net');
const http = require('http');
const fs = require('fs');
const path = require('path');

const WS_PORT = 8765;
const TCP_PORT = 8766;
const PING_INTERVAL = 20000;
const PING_TIMEOUT = 5000;

async function getEngineSettings() {
  const response = await fetch('http://127.0.0.1:8767/status', {
    signal: AbortSignal.timeout(2000),
  });
  if (!response.ok) throw new Error('Engine status returned HTTP ' + response.status);

  const status = await response.json();
  const sampleRate = Number(status.sampleRate);
  const bufferSize = Number(status.BufferSize);
  if (!Number.isInteger(sampleRate) || sampleRate <= 0 ||
      !Number.isInteger(bufferSize) || bufferSize <= 0) {
    throw new Error('Engine status has invalid sample rate or buffer size');
  }
  return {sampleRate, bufferSize};
}

/* Load version info */
let versionInfo = { version: 'unknown' };
try {
  const vp = path.join(__dirname, 'VERSION.json');
  if (fs.existsSync(vp)) versionInfo = JSON.parse(fs.readFileSync(vp, 'utf8'));
} catch(e) {}
console.log('pi-audio-ws-server v' + versionInfo.version + ' (built ' + versionInfo.buildTime + ')');

/* Channel name to index mapping - single speaker system */
const CHANNELS = {
    'pgm-l': 0,
    'pgm-r': 1,
    'tweeter': 2,
    'woofer': 3,
    'mic-1': 4,
    'mic-2': 5,
    'mic-3': 6,
    'mic-4': 7,
    'mic-5': 8,
    'mic-6': 9,
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

/* Track which channels have active clients */
const activeChannels = new Set();

wss.on('connection', async (ws, req) => {
  const url = new URL(req.url, 'http://localhost');
  const ch = url.searchParams.get('ch') || 'pgm-l';
  const chIdx = CHANNELS[ch] != null ? CHANNELS[ch] : 0;
  const peer = req.socket.remoteAddress + ':' + req.socket.remotePort;
  let framesSent = 0;
  let bytesSent = 0;
  let pongDeadline = Date.now() + PING_TIMEOUT;

  console.log('ws-connect ' + peer + ' ch=' + ch + ' idx=' + chIdx);
  let settings;
  try {
    settings = await getEngineSettings();
  } catch (error) {
    console.log('[CONFIG] engine status unavailable: ' + error.message);
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ok: false, error: 'Engine status unavailable'}));
      ws.close(1011, 'Engine status unavailable');
    }
    return;
  }
  if (ws.readyState !== WebSocket.OPEN) return;

  clients[chIdx].push(ws);
  activeChannels.add(chIdx);
  ws.send(JSON.stringify({ok: true, ...settings}));
  // Tell engine to enable this channel
  if (engineSocket && engineSocket.writable) {
    try {
      var cmd = 'E ' + chIdx + '\n';
      engineSocket.write(cmd);
      console.log('[CTRL] sent to engine: ' + cmd.trim());
    } catch(e) { console.log('[CTRL] send failed: ' + e.message); }
  } else {
    console.log('[CTRL] engineSocket unavailable: ws=' + (ws ? 'open' : 'closed') + ' engine=' + (engineSocket ? (engineSocket.writable ? 'writable' : 'closed') : 'null'));
  }

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
    // If no more clients for this channel, disable it
    if (clients[chIdx].length === 0) {
      activeChannels.delete(chIdx);
      if (engineSocket && engineSocket.writable) {
        try { engineSocket.write('D ' + chIdx + '\n'); } catch(e) {}
      }
    }
  });

  ws.on('error', (e) => {
    clearInterval(pingTimer);
    const idx = clients[chIdx].indexOf(ws);
    if (idx >= 0) clients[chIdx].splice(idx, 1);
    console.log('ws-error ' + peer + ': ' + e.message);
  });

  ws._addStats = (b) => { framesSent++; bytesSent += b; };
});

/* Per-channel rate tracking */
const rateStats = {};
for (const idx of Object.values(CHANNELS)) {
  rateStats[idx] = { tcpSamples: 0, tcpMsgs: 0, wsSamples: 0, wsMsgs: 0, skipped: 0 };
}

/* Broadcast mono buffer to all clients for a channel */
function broadcast(chIdx, monoBuf) {
  const chClients = clients[chIdx];
  if (!chClients) return;
  const samples = monoBuf.length / 4;
  for (let i = chClients.length - 1; i >= 0; i--) {
    const ws = chClients[i];
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(monoBuf);
      ws._addStats(monoBuf.length);
      rateStats[chIdx].wsSamples += samples;
      rateStats[chIdx].wsMsgs++;
    } else {
      chClients.splice(i, 1);
    }
  }
}

/* TCP server: accept engine connection */
let engineSocket = null;
const tcpServer = net.createServer((socket) => {
  const peer = socket.remoteAddress + ':' + socket.remotePort;
  console.log('tcp-connect ' + peer);
  engineSocket = socket;
  for (const chIdx of activeChannels) {
    socket.write('E ' + chIdx + '\n');
    console.log('[CTRL] restored channel ' + chIdx + ' after engine reconnect');
  }
  socket.on('error', (e) => { console.log('tcp-error ' + peer + ': ' + e.message); });

  let buf = Buffer.allocUnsafe(65536);
  let bufLen = 0;
  let tcpBytesTotal = 0;
  let tcpMsgCount = 0;
  const tcpStart = Date.now();
  const tcpTimer = setInterval(() => {
    const elapsed = ((Date.now() - tcpStart) / 1000).toFixed(1);
    const kb = (tcpBytesTotal / 1024).toFixed(0);
    console.log('[TCP] ' + kb + 'kB / ' + elapsed + 's = ' + (tcpBytesTotal / (Date.now() - tcpStart) * 1000 / 1024).toFixed(1) + 'kB/s, msgs=' + tcpMsgCount);
  }, 5000).unref();

  /* Per-channel rate logger: 2-second windows */
  let rateLastTime = Date.now();
  const rateTimer = setInterval(() => {
    const now = Date.now();
    const dt = Math.max(0.001, (now - rateLastTime) / 1000);
    rateLastTime = now;
    let line = '[RATE]';
    for (const idx of Object.values(CHANNELS)) {
      const s = rateStats[idx];
      if (s.tcpSamples === 0 && s.wsSamples === 0) continue;
      const tcpRate = (s.tcpSamples / dt).toFixed(0);
      const wsRate = (s.wsSamples / dt).toFixed(0);
      const pct = s.tcpSamples > 0 ? (s.wsSamples / s.tcpSamples * 100).toFixed(0) : '0';
      line += ' ch' + idx + ' tcp=' + tcpRate + '/s(' + s.tcpMsgs + 'm) ws=' + wsRate + '/s(' + s.wsMsgs + 'm) skip=' + s.skipped + ' send=' + pct + '%';
      s.tcpSamples = 0; s.tcpMsgs = 0; s.wsSamples = 0; s.wsMsgs = 0; s.skipped = 0;
    }
    console.log(line);
  }, 5000).unref();

  socket.on('data', (data) => {
    if (bufLen + data.length > buf.length) {
      buf = Buffer.concat([buf, Buffer.allocUnsafe(data.length)]);
    }
    data.copy(buf, bufLen);
    bufLen += data.length;

    while (bufLen >= 5) {
      /* Protocol: [1 byte channel ID][4 bytes BE sample count][sampleCount x 4 bytes mono float32] */
      const channelId = buf.readUInt8(0);
      const sampleCount = buf.readUInt32BE(1);
      const msgLen = 5 + sampleCount * 4;
      if (bufLen < msgLen) break;

      tcpBytesTotal += msgLen;
      tcpMsgCount++;
      rateStats[channelId].tcpSamples += sampleCount;
      rateStats[channelId].tcpMsgs++;

      // Only copy and broadcast if clients exist for this channel
      const chClients = clients[channelId];
      if (chClients && chClients.length > 0) {
        const monoBuf = Buffer.allocUnsafe(msgLen - 5);
        buf.copy(monoBuf, 0, 5, msgLen);
        broadcast(channelId, monoBuf);
        if (tcpMsgCount <= 5) console.log('[TCP] ch=' + channelId + ' samples=' + sampleCount + ' clients=' + chClients.length);
       } else {
        rateStats[channelId].skipped++;
        if (tcpMsgCount <= 5) console.log('[TCP] ch=' + channelId + ' samples=' + sampleCount + ' clients=0 SKIPPED');
      }

      buf.copyWithin(0, msgLen, bufLen);
      bufLen -= msgLen;
    }
  });

  socket.on('close', () => {
    clearInterval(tcpTimer);
    clearInterval(rateTimer);
    if (engineSocket === socket) {
      engineSocket = null;
      console.log('tcp-disconnect ' + peer + ' — engine lost, waiting for reconnect');
      for (const chIdx of Object.values(CHANNELS)) {
        for (const ws of clients[chIdx]) {
          if (ws.readyState === WebSocket.OPEN) {
            try { ws.send(JSON.stringify({ type: 'engine-disconnected' })); } catch(e) {}
          }
        }
      }
    }
  });

  socket.on('error', (e) => {
    console.log('tcp-error ' + peer + ': ' + e.message);
  });
});

tcpServer.listen(TCP_PORT, () => {
  console.log('TCP engine relay on 0.0.0.0:' + TCP_PORT);
});
