#!/usr/bin/env node
// syslog-listener.js — listens for UDP syslog from Pi on port 514
const dgram = require('dgram');

const PORT = 514;
const server = dgram.createSocket('udp4');

server.on('message', (msg, rinfo) => {
  const text = msg.toString().replace(/[\x00-\x1f]/g, ' ').trim();
  const ts = new Date().toISOString().substring(11, 23);
  console.log(`[${ts}] ${rinfo.address}: ${text}`);
});

server.on('error', (err) => {
  if (err.code === 'EACCES') {
    console.error('Port 514 requires admin. Run as administrator.');
    process.exit(1);
  }
  console.error('Error:', err.message);
});

server.bind(PORT, '0.0.0.0', () => {
  console.log(`[syslog] Listening on UDP port ${PORT} — waiting for Pi logs...`);
});
