#!/usr/bin/env node
const WebSocket = require('ws');
const fs = require('fs');

const PORT = 8765;
const HEADER_SIZE = 64;
const DATA_SIZE = 4 * 1024 * 1024;
const POLL_INTERVAL = 10;
const FRAME_SIZE = 4800;

const CHANNELS = {
  'pgm-l': '/run/pi-audio/monitor-pgm-l.raw',
  'pgm-r': '/run/pi-audio/monitor-pgm-r.raw',
  'woofer-l': '/run/pi-audio/monitor-woofer-l.raw',
  'woofer-r': '/run/pi-audio/monitor-woofer-r.raw',
  'tweeter-l': '/run/pi-audio/monitor-tweeter-l.raw',
  'tweeter-r': '/run/pi-audio/monitor-tweeter-r.raw',
  'mic': '/run/pi-audio/monitor-mic.raw',
};

const wss = new WebSocket.Server({ port: PORT });
console.log('WebSocket audio server on ws://0.0.0.0:' + PORT);

wss.on('connection', (ws, req) => {
  const url = new URL(req.url, 'http://localhost');
  const ch = url.searchParams.get('ch') || 'pgm-l';
  const monitorPath = CHANNELS[ch] || CHANNELS['pgm-l'];
  const peer = req.socket.remoteAddress + ':' + req.socket.remotePort;
  let readPos = 0;
  let framesSent = 0;
  let bytesSent = 0;

  console.log('connect ' + peer + ' ch=' + ch);

  const interval = setInterval(() => {
    try {
      const fd = fs.openSync(monitorPath, 'r');
      const hdr = Buffer.alloc(HEADER_SIZE);
      fs.readSync(fd, hdr, 0, HEADER_SIZE, 0);
      fs.closeSync(fd);

      if (hdr.toString('ascii', 0, 4) !== 'PIAM') return;

      const writePos = Number(hdr.readBigUInt64LE(6));
      let avail = writePos >= readPos
        ? writePos - readPos
        : DATA_SIZE - readPos + writePos;

      if (avail < 4) return;

      const toRead = Math.min(avail, FRAME_SIZE);
      const aligned = Math.floor(toRead / 4) * 4;
      if (aligned < 4) return;

      const fileOff = HEADER_SIZE + (readPos % DATA_SIZE);
      const chunk = Buffer.alloc(aligned);
      const fd2 = fs.openSync(monitorPath, 'r');
      const r = fs.readSync(fd2, chunk, 0, aligned, fileOff);
      fs.closeSync(fd2);

      if (r > 0) {
        ws.send(chunk.slice(0, r));
        readPos = (readPos + r) % DATA_SIZE;
        framesSent++;
        bytesSent += r;
      }
    } catch (e) {}
  }, POLL_INTERVAL);

  ws.on('close', () => {
    clearInterval(interval);
    console.log('disconnect ' + peer + ' f=' + framesSent + ' b=' + bytesSent);
  });

  ws.on('error', (e) => {
    clearInterval(interval);
    console.log('error ' + peer + ': ' + e.message);
  });
});
