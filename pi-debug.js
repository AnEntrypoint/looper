#!/usr/bin/env node
const dgram = require('dgram');
const PI_IP = process.env.PI_IP || '192.168.137.100';
const PORT = 4445;
const sock = dgram.createSocket('udp4');
sock.send('STATUS', PORT, PI_IP, err => {
	if (err) { console.error('send failed:', err.message); sock.close(); return; }
});
let closed = false;
sock.on('message', (msg, rinfo) => {
	console.log('[pi]', msg.toString());
	if (!closed) { closed = true; sock.close(); }
});
setTimeout(() => {
	if (!closed) { closed = true; console.log('timeout — no reply'); sock.close(); }
}, 3000);
