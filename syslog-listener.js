#!/usr/bin/env node
// syslog-listener.js — listens for UDP syslog from Pi on port 514
const dgram = require('dgram');
const fs = require('fs');
const path = require('path');

const PORT = 514;
const LOG_FILE = path.join(__dirname, 'syslog.log');

const server = dgram.createSocket('udp4');

server.on('message', (msg, rinfo) => {
  const text = msg.toString().replace(/[\x00-\x1f]/g, ' ').trim();
  const ts = new Date().toISOString().substring(11, 23);
  const line = `[${ts}] ${rinfo.address}: ${text}\n`;
  process.stdout.write(line);
  fs.appendFileSync(LOG_FILE, line);
});

server.on('error', (err) => {
  if (err.code === 'EACCES') {
    console.error('Port 514 requires admin. Run as administrator.');
    process.exit(1);
  }
  console.error('Error:', err.message);
});

server.bind(PORT, '0.0.0.0', () => {
  const msg = `[syslog] Listening on UDP port ${PORT} — waiting for Pi logs...\n`;
  process.stdout.write(msg);
  fs.appendFileSync(LOG_FILE, msg);
});
