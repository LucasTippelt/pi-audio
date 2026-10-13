#!/usr/bin/env node
const WebSocket = require('ws');
const fs = require('fs');
const wss = new WebSocket.Server({ port: 8766 });
console.log('Node.js WS server on port 8766');

wss.on('connection', (ws, req) => {
  const peer = req.socket.remoteAddress + ':' + req.socket.remotePort;
  console.log('Connection from ' + peer);

  const monPath = '/run/pi-audio/monitor-pgm-l.raw';
  let readPos = 0;

  const interval = setInterval(() => {
    try {
      const fd = fs.openSync(monPath, 'r');
      const hdr = Buffer.alloc(64);
      fs.readSync(fd, hdr, 0, 64, 0);
      fs.closeSync(fd);

      const writePos = hdr.readBigUInt64LE(6);
      const writePosNum = Number(writePos);
      const dataSize = 4 * 1024 * 1024;

      let avail = writePosNum >= readPos
        ? writePosNum - readPos
        : dataSize - readPos + writePosNum;

      if (avail < 4800) {
        return;
      }

      const fileOff = 64 + (readPos % dataSize);
      const chunk = Buffer.alloc(4800);
      const fd2 = fs.openSync(monPath, 'r');
      fs.readSync(fd2, chunk, 0, 4800, fileOff);
      fs.closeSync(fd2);
      readPos = (readPos + 4800) % dataSize;

      ws.send(chunk);
    } catch(e) {
      console.log('Error: ' + e.message);
    }
  }, 10);

  ws.on('close', () => {
    clearInterval(interval);
    console.log('Disconnected ' + peer);
  });

  ws.on('error', (e) => {
    console.log('Error ' + peer + ': ' + e.message);
  });
});
