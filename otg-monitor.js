
const { execSync, spawnSync } = require('child_process');
const { SerialPort } = require('serialport');

function clearPhantom() {
  try {
    // Remove any existing instance of our device to clear stale driver state
    const r = spawnSync('pnputil', ['/remove-device', 'USB\\VID_2E8A&PID_000A\\6&de15991&0&1'], 
      { encoding: 'utf8', timeout: 5000 });
    if (r.stdout.includes('successfully')) console.log('[otg] Cleared stale device entry');
  } catch(e) {}
}

function tryOpen() {
  const port = new SerialPort({ path: 'COM11', baudRate: 115200, autoOpen: false });
  port.open((err) => {
    if (err) {
      if (err.message.includes('not found') || err.message.includes('cannot find')) {
        console.log('[otg] Stale handle detected, clearing...');
        clearPhantom();
        setTimeout(tryOpen, 3000);
        return;
      }
      console.log('[otg] Open error:', err.message);
      setTimeout(tryOpen, 3000);
      return;
    }
    console.log('[otg] COM11 opened!');
    port.on('data', (d) => process.stdout.write(d.toString()));
    port.on('close', () => { console.log('[otg] Port closed, retrying...'); setTimeout(tryOpen, 2000); });
    port.on('error', (e) => console.log('[otg] Error:', e.message));
  });
}

console.log('[otg] Waiting for COM11...');
clearPhantom();
setTimeout(tryOpen, 2000);
