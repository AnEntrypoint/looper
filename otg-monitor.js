#!/usr/bin/env node
const { SerialPort } = require('serialport');
const readline = require('readline');

const TARGET_PORT = process.argv[2] || null;
const BAUD = 115200;

async function findCDCPort() {
	const ports = await SerialPort.list();
	if (TARGET_PORT) {
		const found = ports.find(p => p.path === TARGET_PORT);
		if (!found) throw new Error('Port ' + TARGET_PORT + ' not found');
		return TARGET_PORT;
	}
	const cdc = ports.find(p =>
		(p.manufacturer || '').toLowerCase().includes('linux') ||
		(p.friendlyName || '').toLowerCase().includes('cdc') ||
		(p.pnpId || '').toLowerCase().includes('vid_04d8') ||
		(p.pnpId || '').toLowerCase().includes('acm')
	);
	if (cdc) return cdc.path;
	if (ports.length === 1) return ports[0].path;
	console.error('Available ports:');
	ports.forEach(p => console.error(' ', p.path, p.manufacturer || '', p.friendlyName || ''));
	console.error('Specify port as: node otg-monitor.js <COM_PORT>');
	process.exit(1);
}

async function main() {
	let portPath;
	try {
		portPath = await findCDCPort();
	} catch (e) {
		console.error('Error:', e.message);
		try {
			const { execSync } = require('child_process');
			execSync('npm install serialport', { stdio: 'inherit' });
			portPath = await findCDCPort();
		} catch (e2) {
			console.error('Install serialport: npm install serialport');
			process.exit(1);
		}
	}

	console.log('Connecting to', portPath, 'at', BAUD, 'baud...');

	const port = new SerialPort({ path: portPath, baudRate: BAUD });

	port.on('open', () => console.log('Connected. Press r to reboot.'));
	port.on('data', buf => process.stdout.write(buf.toString('utf8')));
	port.on('error', e => console.error('Port error:', e.message));
	port.on('close', () => { console.log('\nDisconnected.'); process.exit(0); });

	readline.emitKeypressEvents(process.stdin);
	if (process.stdin.isTTY) process.stdin.setRawMode(true);
	process.stdin.on('keypress', (str, key) => {
		if (key && key.ctrl && key.name === 'c') process.exit(0);
		if (str === 'r' || str === 'R') {
			console.log('\n[Sending reboot command]');
			port.write(Buffer.from([0x52]));
		}
	});
}

main();
