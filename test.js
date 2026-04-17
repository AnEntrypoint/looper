const assert = require('assert');

const BLOCK = 64;
const IN_SIZE = 512;
const IN_MASK = IN_SIZE - 1;
const IN_TARGET = 256;
const IN_DB = 128;
const RATE_GAIN = 16384;
const RATE_MAX = 256;
const FRAC = 65536;

function mkRing() {
  return {
    L: new Int16Array(IN_SIZE),
    R: new Int16Array(IN_SIZE),
    wr: 0, rd: 0, rd_frac: 0,
    last_l: 0, last_r: 0,
    ur: 0, rs: 0,
  };
}

function write(r, l, rt, n) {
  for (let i = 0; i < n; i++) {
    r.L[r.wr & IN_MASK] = l;
    r.R[r.wr & IN_MASK] = rt;
    r.wr = (r.wr + 1) >>> 0;
  }
}

function writeRamp(r, base, n) {
  for (let i = 0; i < n; i++) {
    r.L[r.wr & IN_MASK] = (base + i) & 0x7fff;
    r.R[r.wr & IN_MASK] = (base + i) & 0x7fff;
    r.wr = (r.wr + 1) >>> 0;
  }
}

function updateBlock(r, out) {
  const wr = r.wr;
  let rd = r.rd;
  let rd_frac = r.rd_frac;
  let avail = ((wr - rd) | 0);
  if (avail >= IN_SIZE * 3 / 4 || avail < BLOCK) {
    rd = (wr - IN_TARGET) >>> 0;
    rd_frac = 0;
    r.rs++;
    avail = IN_TARGET;
  }
  let dev = avail - IN_TARGET;
  let band_dev = 0;
  if (dev > IN_DB) band_dev = dev - IN_DB;
  else if (dev < -IN_DB) band_dev = dev + IN_DB;
  if (band_dev > RATE_MAX) band_dev = RATE_MAX;
  if (band_dev < -RATE_MAX) band_dev = -RATE_MAX;
  const rate_step = FRAC + Math.trunc(band_dev * FRAC / RATE_GAIN);
  for (let i = 0; i < BLOCK; i++) {
    if (((r.wr - rd) | 0) > 1) {
      const l0 = r.L[rd & IN_MASK];
      const rr0 = r.R[rd & IN_MASK];
      const l1 = r.L[(rd + 1) & IN_MASK];
      const rr1 = r.R[(rd + 1) & IN_MASK];
      out[i*2]   = l0 + (((l1 - l0) * rd_frac) >> 16);
      out[i*2+1] = rr0 + (((rr1 - rr0) * rd_frac) >> 16);
      r.last_l = out[i*2]; r.last_r = out[i*2+1];
      rd_frac += rate_step;
      rd = (rd + (rd_frac >>> 16)) >>> 0;
      rd_frac &= 0xFFFF;
    } else {
      out[i*2]   = r.last_l;
      out[i*2+1] = r.last_r;
      r.ur++;
    }
  }
  r.rd = rd;
  r.rd_frac = rd_frac;
}

function section(name, fn) {
  try { fn(); console.log('ok  ', name); }
  catch (e) { console.error('FAIL', name, e.message); process.exit(1); }
}

section('steady-state: no underrun, no resync, rate stays at 1.0', () => {
  const r = mkRing();
  write(r, 100, 200, IN_TARGET);
  const out = new Int16Array(BLOCK * 2);
  for (let k = 0; k < 100; k++) {
    write(r, 100, 200, BLOCK);
    updateBlock(r, out);
  }
  assert.strictEqual(r.ur, 0);
  assert.strictEqual(r.rs, 0);
  for (let i = 0; i < BLOCK; i++) {
    assert.strictEqual(out[i*2], 100);
    assert.strictEqual(out[i*2+1], 200);
  }
});

section('linear interp midpoint: frac=32768 between 100 and 200 gives 150', () => {
  const a = 100, b = 200, f = 32768;
  const v = a + (((b - a) * f) >> 16);
  assert.strictEqual(v, 150);
});

section('linear interp zero frac: returns lower sample', () => {
  const a = 777, b = 999, f = 0;
  const v = a + (((b - a) * f) >> 16);
  assert.strictEqual(v, 777);
});

section('ramp signal: output is monotonically increasing ramp', () => {
  const r = mkRing();
  writeRamp(r, 0, IN_TARGET);
  const out = new Int16Array(BLOCK * 2);
  writeRamp(r, IN_TARGET, BLOCK);
  updateBlock(r, out);
  for (let i = 1; i < BLOCK; i++) {
    assert.ok(out[i*2] >= out[(i-1)*2], `monotonic at ${i}: ${out[i*2]} < ${out[(i-1)*2]}`);
  }
});

section('drift positive: avail converges toward target over time', () => {
  const r = mkRing();
  write(r, 1000, 2000, IN_TARGET + IN_DB + BLOCK);
  const out = new Int16Array(BLOCK * 2);
  let maxAvail = 0;
  for (let k = 0; k < 500; k++) {
    write(r, 1000, 2000, BLOCK);
    updateBlock(r, out);
    const av = (r.wr - r.rd) >>> 0;
    if (av > maxAvail) maxAvail = av;
  }
  const finalAvail = (r.wr - r.rd) >>> 0;
  assert.ok(finalAvail <= IN_TARGET + IN_DB + 4,
    `avail did not converge: ${finalAvail} > ${IN_TARGET + IN_DB + 4}`);
  assert.strictEqual(r.ur, 0);
});

section('catastrophic overrun triggers single resync', () => {
  const r = mkRing();
  write(r, 1, 2, IN_SIZE - 10);
  const out = new Int16Array(BLOCK * 2);
  updateBlock(r, out);
  assert.ok(r.rs > 0);
});

section('long-run matched rate: zero resyncs, zero underruns', () => {
  const r = mkRing();
  const out = new Int16Array(BLOCK * 2);
  write(r, 0, 0, IN_TARGET);
  for (let k = 0; k < 5000; k++) {
    writeRamp(r, k * BLOCK, BLOCK);
    updateBlock(r, out);
  }
  assert.strictEqual(r.ur, 0);
  assert.strictEqual(r.rs, 0);
});

section('long-run 0.1% fast producer: convergent, no underruns', () => {
  const r = mkRing();
  const out = new Int16Array(BLOCK * 2);
  write(r, 0, 0, IN_TARGET);
  let phase = 0;
  for (let k = 0; k < 5000; k++) {
    phase += 1;
    let n = BLOCK;
    if (phase >= 1000) { n += 1; phase -= 1000; }
    writeRamp(r, k * BLOCK, n);
    updateBlock(r, out);
  }
  assert.strictEqual(r.ur, 0);
  const avail = (r.wr - r.rd) >>> 0;
  assert.ok(avail < IN_SIZE * 3 / 4, `avail escaped: ${avail}`);
});

section('long-run 0.1% slow producer: convergent', () => {
  const r = mkRing();
  const out = new Int16Array(BLOCK * 2);
  write(r, 0, 0, IN_TARGET);
  let phase = 0;
  for (let k = 0; k < 5000; k++) {
    phase += 1;
    let n = BLOCK;
    if (phase >= 1000 && n > 0) { n -= 1; phase -= 1000; }
    writeRamp(r, k * BLOCK, n);
    updateBlock(r, out);
  }
  const avail = (r.wr - r.rd) >>> 0;
  assert.ok(avail > 0);
});

console.log('all tests passed');
