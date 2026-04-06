#!/usr/bin/env node
// dev-server.js — starts tftp, dhcp, and syslog servers in one process
// Usage: node dev-server.js

const { fork } = require('child_process');
const path = require('path');
const fs = require('fs');

// Load .env if present
try {
  fs.readFileSync(path.join(__dirname, '.env'), 'utf8').split('\n').forEach(line => {
    const m = line.match(/^([^=]+)=(.*)$/);
    if (m && !process.env[m[1]]) process.env[m[1]] = m[2].trim();
  });
} catch(e) {}

const scripts = ['tftp-server.js', 'dhcp-server.js', 'syslog-listener.js'];

for (const script of scripts) {
  const child = fork(path.join(__dirname, script), [], { silent: false });
  child.on('exit', (code) => {
    console.error(`[dev-server] ${script} exited with code ${code}`);
  });
}

console.log('[dev-server] All servers started. Press Ctrl+C to stop.');
