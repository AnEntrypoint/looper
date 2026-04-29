#!/usr/bin/env node
// syslog-listener.js — listens for UDP syslog from Pi on port 514
const dgram = require('dgram');
const fs = require('fs');
const path = require('path');

const PORT = 514;
const LOG_FILE = path.join(__dirname, 'syslog.log');

// Circle CSysLogDaemon format: <PRI>1 - HOSTNAME APPNAME - - - MESSAGE
function parseSyslog(raw) {
  let msg = raw.toString();
  const pri = msg.match(/^<\d+>(.*)/s);
  if (pri) msg = pri[1];
  const circle = msg.match(/^\d+\s+-\s+\S+\s+(\S+)\s+-\s+-\s+-\s+(.*)/s);
  if (circle) msg = circle[1] + ': ' + circle[2];
  else msg = msg.replace(/^\w{3}\s+\d+\s+\d+:\d+:\d+\s+\S+\s+/, '');
  return msg.replace(/[^\x20-\x7e\n]/g, '').trim();
}

const server = dgram.createSocket('udp4');

server.on('message', (msg, rinfo) => {
  const text = parseSyslog(msg.toString());
  if (!text) return;
  if (text.startsWith('icmp:')) return; // filter ICMP unreachable noise
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
