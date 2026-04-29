#!/usr/bin/env node
// reboot.js — send REBOOT command to Pi via UDP
const dgram = require('dgram');
const PI_IP = '192.168.137.100';
const PI_PORT = 4444;

const sock = dgram.createSocket('udp4');
sock.send('REBOOT', PI_PORT, PI_IP, (err) => {
  if (err) console.error('Error:', err.message);
  else console.log(`Sent REBOOT to ${PI_IP}:${PI_PORT}`);
  sock.close();
});
