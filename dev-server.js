#!/usr/bin/env node
// dev-server.js — starts tftp, dhcp, and syslog servers in one process
// Usage: node dev-server.js

const { fork } = require('child_process');
const path = require('path');

const scripts = ['tftp-server.js', 'dhcp-server.js', 'syslog-listener.js'];

for (const script of scripts) {
  const child = fork(path.join(__dirname, script), [], { silent: false });
  child.on('exit', (code) => {
    console.error(`[dev-server] ${script} exited with code ${code}`);
  });
}

console.log('[dev-server] All servers started. Press Ctrl+C to stop.');
