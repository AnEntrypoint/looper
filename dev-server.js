#!/usr/bin/env node
// dev-server.js — starts tftp, dhcp, and syslog servers; auto-restarts on crash
// Also runs a UDP test controller on port 5555 for network MIDI injection
// Usage: node dev-server.js

const { fork } = require('child_process');
const dgram = require('dgram');
const path = require('path');
const fs = require('fs');

// Load .env if present
try {
  fs.readFileSync(path.join(__dirname, '.env'), 'utf8').split('\n').forEach(line => {
    const m = line.match(/^([^=]+)=(.*)$/);
    if (m && !process.env[m[1]]) process.env[m[1]] = m[2].trim();
  });
} catch(e) {}

const scripts = ['tftp-server.js'];

function startChild(script) {
  const child = fork(path.join(__dirname, script), [], { silent: false });
  child.on('exit', (code) => {
    console.error(`[dev-server] ${script} exited with code ${code} — restarting in 2s`);
    setTimeout(() => startChild(script), 2000);
  });
  return child;
}

for (const script of scripts) startChild(script);

// ── UDP test controller on port 5555 ─────────────────────────────────────────
// Send UDP packets to 192.168.137.1:5555 to inject MIDI into the Pi
// Protocol: text commands, one per packet
//   track <0-3>   — simulate APC col0 press on track N (NoteOn note=N*8, vel=127)
//   mute <0-3>    — simulate APC col1 press on track N
//   stop          — send STOP_ALL button (note=0x51)
//   reboot        — send REBOOT to Pi
//   status        — print current syslog tail

const PI_IP = '192.168.137.100';
const PI_MIDI_PORT = 4445; // we'll open a raw UDP MIDI forwarder on Pi side...
// Actually: inject via the MIDI packet handler directly by sending raw MIDI over UDP to a new listener

// Simpler approach: we forward MIDI NoteOn packets via UDP to the Pi on port 4446
// The Pi kernel.cpp will need a MIDI-over-UDP listener. Instead, use the existing
// reboot socket pattern — add a MIDI UDP listener in kernel.cpp.
//
// For NOW: the test controller sends to tftp-server's internal reboot socket
// and we add a separate MIDI-inject socket in kernel.cpp on port 4446.
// Protocol: raw 3-byte MIDI messages over UDP to 192.168.137.100:4446

const midiSock = dgram.createSocket('udp4');

function sendMidi(status, note, vel) {
  const buf = Buffer.from([status, note, vel]);
  midiSock.send(buf, 0, 3, 4446, PI_IP, (err) => {
    if (err) console.error('[CTRL] MIDI send error:', err.message);
    else console.log(`[CTRL] MIDI -> ${PI_IP}:4446 [${status.toString(16)} ${note.toString(16)} ${vel.toString(16)}]`);
  });
}

function sendReboot() {
  const sock = dgram.createSocket('udp4');
  sock.send(Buffer.from('REBOOT'), 0, 6, 4444, PI_IP, () => {
    sock.close();
    console.log('[CTRL] Sent REBOOT');
  });
}

const ctrl = dgram.createSocket('udp4');
ctrl.on('message', (msg, rinfo) => {
  const cmd = msg.toString().trim();
  console.log(`[CTRL] cmd="${cmd}" from ${rinfo.address}:${rinfo.port}`);

  const m = cmd.match(/^(\w+)\s*(\d*)$/);
  if (!m) return;
  const [, op, arg] = m;
  const n = parseInt(arg) || 0;

  if (op === 'track' && n >= 0 && n <= 3) {
    sendMidi(0x90, n * 8, 0x7F);       // NoteOn col0 of track n
    setTimeout(() => sendMidi(0x80, n * 8, 0x7F), 50); // NoteOff
  } else if (op === 'mute' && n >= 0 && n <= 3) {
    sendMidi(0x90, n * 8 + 1, 0x7F);
    setTimeout(() => sendMidi(0x80, n * 8 + 1, 0x7F), 50);
  } else if (op === 'stop') {
    sendMidi(0x90, 0x51, 0x7F);
    setTimeout(() => sendMidi(0x80, 0x51, 0x7F), 50);
  } else if (op === 'reboot') {
    sendReboot();
  } else {
    console.log('[CTRL] Unknown command. Try: track <0-3>, mute <0-3>, stop, reboot');
  }
});

ctrl.bind(5555, '0.0.0.0', () => {
  console.log('[CTRL] Test controller on UDP:5555');
  console.log('[CTRL] Commands: track <0-3> | mute <0-3> | stop | reboot');
  console.log('[CTRL] Usage: echo "track 0" | nc -u 192.168.137.1 5555');
});

console.log('[dev-server] All servers started. Press Ctrl+C to stop.');
