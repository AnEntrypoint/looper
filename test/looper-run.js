#!/usr/bin/env node
'use strict';

const { loadConstants, extractClipStates, verifySourceIntegrity, CS } = require('./looper-sim');
const { MachineSim } = require('./looper-machine');

function runTests() {
  const C = loadConstants();
  console.log('Constants loaded from source:');
  console.log(JSON.stringify(C, null, 2));

  console.log('\n--- Source integrity checks ---');
  const checks = verifySourceIntegrity();
  checks.forEach(c => console.log(`  ${c.ok ? 'PASS' : 'FAIL'} ${c.msg}`));
  const allChecks = checks.every(c => c.ok);

  console.log('\n--- Phase alignment tests ---');
  const bpms = [60, 90, 105, 120, 140, 180];
  const scenarios = [];

  for (const bpm of bpms) {
    const M = Math.round((C.INTEGRAL_BLOCKS_PER_SECOND * 60 * 4) / bpm);
    const half = M >> 1;

    {
      const m = new MachineSim(C, bpm);
      m.pressRecord();
      for (let t = 0; t < 50; t++) { m.tick = t; m.update(); }
      m.pressPlay();
      for (let t = 50; t < M * 3; t++) { m.tick = t; m.update(); if (m.clip.state === CS.PLAYING) break; }
      const nb = m.clip.num_blocks;
      while ((m.masterPhase - m.clip.recordStartPhaseOffset) % nb !== 0) { m.tick++; m.update(); }
      scenarios.push({ name: `${bpm}BPM short`, ok: m.clip.play_block === 0, pb: m.clip.play_block, M, nb });
    }

    {
      const m = new MachineSim(C, bpm);
      m.pressRecord();
      const overshoot = M + 10;
      for (let t = 0; t < overshoot; t++) { m.tick = t; m.update(); }
      m.pressPlay();
      for (let t = overshoot; t < overshoot + M * 3; t++) { m.tick = t; m.update(); if (m.clip.state === CS.PLAYING) break; }
      while (m.masterPhase % M !== 0) { m.tick++; m.update(); }
      const pb = (((m.masterPhase - m.clip.recordStartPhaseOffset) % m.clip.num_blocks) + m.clip.num_blocks * 2) % m.clip.num_blocks;
      scenarios.push({ name: `${bpm}BPM overshoot`, ok: pb === 0, pb, M, nb: m.clip.num_blocks });
    }

    {
      const m = new MachineSim(C, bpm);
      m.pressRecord();
      for (let t = 0; t < 50; t++) { m.tick = t; m.update(); }
      m.pressPlay();
      for (let t = 50; t < M * 3; t++) { m.tick = t; m.update(); if (m.clip.state === CS.PLAYING) break; }
      for (let t = 0; t < 1000 * m.clip.num_blocks; t++) { m.tick++; m.update(); }
      const nb = m.clip.num_blocks;
      while ((m.masterPhase - m.clip.recordStartPhaseOffset) % nb !== 0) { m.tick++; m.update(); }
      scenarios.push({ name: `${bpm}BPM drift-1000`, ok: m.clip.play_block === 0, pb: m.clip.play_block, M, nb });
    }

    {
      const m = new MachineSim(C, bpm);
      m.pressRecord();
      for (let t = 0; t < half + 10; t++) { m.tick = t; m.update(); }
      m.pressPlay();
      let i = 0; while (m.clip.state !== CS.PLAYING && i++ < M * 4) { m.tick++; m.update(); }
      const nb = m.clip.num_blocks;
      const ro = m.clip.recordStartPhaseOffset;
      while (((m.masterPhase - ro) % nb + nb) % nb !== 0) { m.tick++; m.update(); }
      scenarios.push({ name: `${bpm}BPM sub-phrase`, ok: m.clip.play_block === 0 && nb === half, pb: m.clip.play_block, M, nb });
    }

    {
      const m = new MachineSim(C, bpm);
      m.pressRecord();
      for (let t = 0; t < M * 2 + 10; t++) { m.tick = t; m.update(); }
      m.pressPlay();
      let i = 0; while (m.clip.state !== CS.PLAYING && i++ < M * 6) { m.tick++; m.update(); }
      const nb = m.clip.num_blocks;
      const ro = m.clip.recordStartPhaseOffset;
      while (((m.masterPhase - ro) % nb + nb) % nb !== 0) { m.tick++; m.update(); }
      scenarios.push({ name: `${bpm}BPM multi-phrase`, ok: m.clip.play_block === 0 && nb === M * 2, pb: m.clip.play_block, M, nb });
    }

    {
      const m = new MachineSim(C, bpm);
      m.pressRecord();
      for (let t = 0; t < half + 10; t++) { m.tick = t; m.update(); }
      m.pressStop();
      let i = 0; while (m.clip.state !== CS.RECORDED && i++ < M * 3) { m.tick++; m.update(); }
      scenarios.push({ name: `${bpm}BPM stop-quantize`, ok: m.clip.state === CS.RECORDED && m.clip.num_blocks === half, pb: 0, M, nb: m.clip.num_blocks });
    }

    {
      const m = new MachineSim(C, bpm);
      m.pressRecord();
      for (let t = 0; t < half - 5; t++) { m.tick = t; m.update(); }
      m.pressPlay();
      const deferred = m.clip.quantizeTarget === half && m.clip.quantizeWillPlay;
      let i = 0; while (m.clip.state !== CS.PLAYING && i++ < M * 4) { m.tick++; m.update(); }
      const nb = m.clip.num_blocks;
      const ro = m.clip.recordStartPhaseOffset;
      while (((m.masterPhase - ro) % nb + nb) % nb !== 0) { m.tick++; m.update(); }
      scenarios.push({ name: `${bpm}BPM deferred`, ok: deferred && m.clip.play_block === 0 && nb === half, pb: m.clip.play_block, M, nb });
    }

    {
      const m = new MachineSim(C, bpm);
      m.masterPhase = M - 5;
      m.running = 1;
      m.pressRecord();
      const pendingOk = m.trackPending === 1 && m.clip.state === CS.IDLE;
      let i = 0; while (m.trackPending !== 0 && i++ < 20) { m.tick++; m.update(); }
      const ro = m.clip.recordStartPhaseOffset;
      scenarios.push({ name: `${bpm}BPM latch`, ok: pendingOk && m.clip.state >= CS.RECORDING && ro % M === 0, pb: 0, M, nb: ro });
    }
  }

  let allPass = allChecks;
  scenarios.forEach(s => {
    console.log(`  ${s.ok ? 'PASS' : 'FAIL'} ${s.name}: M=${s.M} nb=${s.nb} pb=${s.pb}`);
    if (!s.ok) allPass = false;
  });

  console.log(`\n${allPass ? 'ALL TESTS PASSED' : 'SOME TESTS FAILED'}`);
  return allPass;
}

if (require.main === module) {
  const ok = runTests();
  process.exit(ok ? 0 : 1);
}

module.exports = { loadConstants, extractClipStates, verifySourceIntegrity, MachineSim, CS, runTests };
