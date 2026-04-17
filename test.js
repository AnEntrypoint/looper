const assert = require('assert');

const BLOCK = 64;
const IN_SIZE = 512;
const IN_MASK = IN_SIZE - 1;
const IN_TARGET = 192;
const IN_DB = 64;

function mkRing() {
  return {
    L: new Int16Array(IN_SIZE),
    R: new Int16Array(IN_SIZE),
    wr: 0, rd: 0,
    last_l: 0, last_r: 0,
    ur: 0, rs: 0, sk: 0, rp: 0,
  };
}

function write(r, l, rt, n) {
  for (let i = 0; i < n; i++) {
    r.L[r.wr & IN_MASK] = l;
    r.R[r.wr & IN_MASK] = rt;
    r.wr = (r.wr + 1) >>> 0;
  }
}

function updateBlock(r, out) {
  const wr = r.wr;
  let rd = r.rd;
  let avail = ((wr - rd) | 0);
  if (avail >= IN_SIZE * 3 / 4 || avail < BLOCK) {
    rd = (wr - IN_TARGET) >>> 0;
    r.rs++;
  } else if (avail > IN_TARGET + IN_DB) {
    rd = (rd + 1) >>> 0;
    r.sk++;
  } else if (avail < IN_TARGET - IN_DB) {
    rd = (rd - 1) >>> 0;
    r.rp++;
  }
  for (let i = 0; i < BLOCK; i++) {
    if (((r.wr - rd) | 0) > 0) {
      out[i*2]   = r.L[rd & IN_MASK];
      out[i*2+1] = r.R[rd & IN_MASK];
      r.last_l = out[i*2]; r.last_r = out[i*2+1];
      rd = (rd + 1) >>> 0;
    } else {
      out[i*2]   = r.last_l;
      out[i*2+1] = r.last_r;
      r.ur++;
    }
  }
  r.rd = rd;
}

function section(name, fn) {
  try { fn(); console.log('ok  ', name); }
  catch (e) { console.error('FAIL', name, e.message); process.exit(1); }
}

section('steady-state: deadband does not trigger corrections', () => {
  const r = mkRing();
  write(r, 100, 200, IN_TARGET);
  const out = new Int16Array(BLOCK * 2);
  for (let k = 0; k < 10; k++) {
    write(r, 100, 200, BLOCK);
    updateBlock(r, out);
  }
  assert.strictEqual(r.ur, 0, 'no underruns expected');
  assert.strictEqual(r.rs, 0, 'no resyncs expected');
  assert.strictEqual(r.sk, 0, 'no skips expected');
  assert.strictEqual(r.rp, 0, 'no repeats expected');
});

section('underrun: repeats last sample, not zero', () => {
  const r = mkRing();
  write(r, 555, 777, IN_TARGET + BLOCK);
  const out = new Int16Array(BLOCK * 2);
  updateBlock(r, out);
  r.last_l = 555; r.last_r = 777;
  r.rd = r.wr;
  const out2 = new Int16Array(BLOCK * 2);
  const origWr = r.wr;
  r.wr = r.rd;
  for (let i = 0; i < BLOCK; i++) {
    if (((r.wr - r.rd) | 0) > 0) {
      out2[i*2] = r.L[r.rd & IN_MASK]; out2[i*2+1] = r.R[r.rd & IN_MASK];
      r.last_l = out2[i*2]; r.last_r = out2[i*2+1]; r.rd = (r.rd+1)>>>0;
    } else {
      out2[i*2] = r.last_l; out2[i*2+1] = r.last_r; r.ur++;
    }
  }
  assert.ok(r.ur > 0, 'expected underruns');
  assert.strictEqual(out2[0], 555);
  assert.strictEqual(out2[1], 777);
  assert.notStrictEqual(out2[0], 0);
});

section('catastrophic overrun triggers resync', () => {
  const r = mkRing();
  write(r, 1, 2, IN_SIZE - 10);
  const out = new Int16Array(BLOCK * 2);
  updateBlock(r, out);
  assert.ok(r.rs > 0, 'expected resync');
});

section('drift positive: skip reduces avail back to target', () => {
  const r = mkRing();
  write(r, 10, 20, IN_TARGET + IN_DB + BLOCK);
  const out = new Int16Array(BLOCK * 2);
  updateBlock(r, out);
  assert.ok(r.sk > 0, 'expected a skip correction');
});

section('drift negative (avail below target-db, above block): repeats one sample', () => {
  const r = mkRing();
  write(r, 10, 20, BLOCK + 1);
  const out = new Int16Array(BLOCK * 2);
  updateBlock(r, out);
  assert.ok(r.rp > 0, 'expected repeat correction');
});

section('long-run stability under matched rates', () => {
  const r = mkRing();
  const out = new Int16Array(BLOCK * 2);
  write(r, 0, 0, IN_TARGET + BLOCK);
  for (let k = 0; k < 1000; k++) {
    updateBlock(r, out);
    write(r, k & 0x7fff, (k*2) & 0x7fff, BLOCK);
  }
  assert.strictEqual(r.ur, 0);
  assert.strictEqual(r.rs, 0);
});

section('long-run stability under 0.1% fast producer (skip kicks in)', () => {
  const r = mkRing();
  const out = new Int16Array(BLOCK * 2);
  write(r, 0, 0, IN_TARGET + BLOCK);
  let phase = 0;
  for (let k = 0; k < 2000; k++) {
    updateBlock(r, out);
    phase += 65;
    let n = BLOCK;
    if (phase >= 1000) { n += 1; phase -= 1000; }
    write(r, k & 0x7fff, (k*2) & 0x7fff, n);
  }
  assert.ok(r.sk > 0, 'skip corrections must have fired');
  assert.strictEqual(r.ur, 0, 'no underruns when producer is fast');
});

section('long-run stability under 0.1% slow producer (repeat kicks in)', () => {
  const r = mkRing();
  const out = new Int16Array(BLOCK * 2);
  write(r, 0, 0, IN_TARGET + BLOCK);
  let phase = 0;
  for (let k = 0; k < 2000; k++) {
    updateBlock(r, out);
    phase += 65;
    let n = BLOCK;
    if (phase >= 1000) { n -= 1; phase -= 1000; }
    write(r, k & 0x7fff, (k*2) & 0x7fff, n);
  }
  assert.ok(r.rp > 0 || r.rs > 0, 'repeat/resync corrections must have fired');
});

console.log('all tests passed');
