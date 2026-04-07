#!/usr/bin/env node
// syslog-listener.js — listens for UDP syslog from Pi on port 514
const dgram = require('dgram');
const fs = require('fs');
const path = require('path');

const PORT = 514;
const LOG_FILE = path.join(__dirname, 'syslog.log');

// Parse RFC3164 syslog: <PRI>TIMESTAMP HOSTNAME TAG: MSG
function parseSyslog(raw) {
  let msg = raw;
  // Strip <PRI>
  const priMatch = msg.match(/^<(\d+)>(.*)/s);
  if (priMatch) msg = priMatch[2];
  // Strip leading timestamp (MMM DD HH:MM:SS or ISO)
  msg = msg.replace(/^\w{3}\s+\d+\s+\d+:\d+:\d+\s+\S+\s+/, '');
  // Strip any remaining non-printable chars
  msg = msg.replace(/[^\x20-\x7e\n]/g, '');
  return msg.trim();
}

const server = dgram.createSocket('udp4');

server.on('message', (msg, rinfo) => {
  const text = parseSyslog(msg.toString());
  if (!text) return;
  const ts = new Date().toISOString().substring(11, 23);
  const line = `[${ts}] ${text}\n`;
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
