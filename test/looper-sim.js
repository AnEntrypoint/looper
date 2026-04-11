#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');

function readSource(name) {
  return fs.readFileSync(path.join(ROOT, name), 'utf-8');
}

function extractDefine(src, name) {
  const re = new RegExp(`#define\\s+${name}\\s+(.+)`);
  const m = src.match(re);
  if (!m) return undefined;
  let val = m[1].replace(/\/\/.*/, '').trim();
  return val;
}

function extractDefineNum(src, name) {
  const val = extractDefine(src, name);
  if (val === undefined) return undefined;
  try { return eval(val); } catch { return parseInt(val); }
}

function loadConstants() {
  const looperH = readSource('Looper.h');
  const commonH = readSource('commonDefines.h');
  const audioH = readSource(
    fs.existsSync(path.join(ROOT, 'circle-prh', 'audio', 'AudioTypes.h'))
      ? 'circle-prh/audio/AudioTypes.h'
      : 'Looper.h'
  );

  const CROSSFADE_BLOCKS = extractDefineNum(looperH, 'CROSSFADE_BLOCKS') || 4;
  const LOOPER_NUM_CHANNELS = extractDefineNum(looperH, 'LOOPER_NUM_CHANNELS') || 2;
  const LOOPER_NUM_TRACKS = extractDefineNum(commonH, 'LOOPER_NUM_TRACKS') || 5;
  const LOOPER_NUM_LAYERS = extractDefineNum(commonH, 'LOOPER_NUM_LAYERS') || 4;

  let AUDIO_SAMPLE_RATE, AUDIO_BLOCK_SAMPLES;
  try {
    const at = fs.readFileSync(path.join(ROOT, 'circle-prh', 'audio', 'AudioTypes.h'), 'utf-8');
    AUDIO_SAMPLE_RATE = extractDefineNum(at, 'AUDIO_SAMPLE_RATE') || 44100;
    AUDIO_BLOCK_SAMPLES = extractDefineNum(at, 'AUDIO_BLOCK_SAMPLES') || 64;
  } catch {
    AUDIO_SAMPLE_RATE = 44100;
    AUDIO_BLOCK_SAMPLES = 64;
  }

  const INTEGRAL_BLOCKS_PER_SECOND = Math.floor((AUDIO_SAMPLE_RATE + AUDIO_BLOCK_SAMPLES - 1) / AUDIO_BLOCK_SAMPLES);

  return {
    CROSSFADE_BLOCKS, LOOPER_NUM_CHANNELS, LOOPER_NUM_TRACKS, LOOPER_NUM_LAYERS,
    AUDIO_SAMPLE_RATE, AUDIO_BLOCK_SAMPLES, INTEGRAL_BLOCKS_PER_SECOND
  };
}

function extractClipStates() {
  const src = readSource('Looper.h');
  const m = src.match(/enum\s+ClipState\s*\{([^}]+)\}/);
  if (!m) return {};
  const states = {};
  let val = 0;
  m[1].split(',').forEach(line => {
    line = line.replace(/\/\/.*/, '').trim();
    if (!line) return;
    const parts = line.split('=');
    const name = parts[0].trim();
    if (parts.length > 1) val = parseInt(parts[1].trim());
    states[name] = val++;
  });
  return states;
}

function verifySourceIntegrity() {
  const clipSrc = readSource('loopClip.cpp');
  const clipUpdateSrc = readSource('loopClipUpdate.cpp');
  const clipStateSrc = readSource('loopClipState.cpp');
  const machineSrc = readSource('loopMachine.cpp');

  const checks = [];

  if (clipSrc.includes('m_masterPhase + CROSSFADE_BLOCKS'))
    checks.push({ ok: false, msg: 'loopClip.cpp: offset still has +CROSSFADE_BLOCKS' });
  else
    checks.push({ ok: true, msg: 'loopClip.cpp: offset = masterPhase (no +CB)' });

  if (clipSrc.includes('+ CROSSFADE_BLOCKS + 1'))
    checks.push({ ok: false, msg: 'loopClip.cpp: offset has +CB+1 (should be plain masterPhase)' });

  if (clipUpdateSrc.includes('m_state != CS_LOOPING'))
    checks.push({ ok: false, msg: 'loopClipUpdate.cpp: hard-lock still skips CS_LOOPING' });
  else
    checks.push({ ok: true, msg: 'loopClipUpdate.cpp: hard-lock applies in all play states' });

  if (clipUpdateSrc.includes('crossfade_start = m_num_blocks'))
    checks.push({ ok: true, msg: 'loopClipUpdate.cpp: crossfade reads tail region' });
  else
    checks.push({ ok: false, msg: 'loopClipUpdate.cpp: crossfade_start may not point to tail' });

  if (machineSrc.includes('m_masterPhase % m_masterLoopBlocks == 0'))
    checks.push({ ok: true, msg: 'loopMachine.cpp: phrase boundary uses modular arithmetic' });
  else
    checks.push({ ok: false, msg: 'loopMachine.cpp: phrase boundary check may be wrong' });

  if (machineSrc.includes('m_masterPhase++') && !machineSrc.includes('% m_masterLoopBlocks;'))
    checks.push({ ok: true, msg: 'loopMachine.cpp: masterPhase is monotonic (no wrap)' });

  return checks;
}

// ========== STATE MACHINE SIMULATION ==========

const CS = { IDLE: 0, RECORDING: 1, RECORDING_MAIN: 2, RECORDING_TAIL: 3,
             FINISHING: 4, RECORDED: 5, PLAYING: 6, LOOPING: 7, STOPPING: 8 };

class ClipSim {
  constructor(C) {
    this.C = C;
    this.init();
  }
  init() {
    this.state = CS.IDLE;
    this.num_blocks = 0; this.max_blocks = 0;
    this.play_block = 0; this.record_block = 0;
    this.crossfade_start = 0; this.crossfade_offset = 0;
    this.recordStartPhaseOffset = 0;
    this.quantizeTarget = 0; this.quantizeWillPlay = false;
  }
}

class MachineSim {
  constructor(C, bpm) {
    this.C = C;
    this.masterPhase = 0;
    this.masterLoopBlocks = Math.round((C.INTEGRAL_BLOCKS_PER_SECOND * 60 * 4) / bpm);
    this.running = 0;
    this.clip = new ClipSim(C);
    this.trackPending = 0;
    this.log = [];
    this.tick = 0;
  }

  emit(msg) { this.log.push(`t${this.tick} mp=${this.masterPhase}: ${msg}`); }

  pressRecord() {
    if (this.masterLoopBlocks > 0) this.trackPending = 1;
    else this._startRecording();
  }

  pressPlay() {
    const c = this.clip;
    if (c.state === CS.RECORDING || c.state === CS.RECORDING_MAIN) {
      if (c.state === CS.RECORDING && c.record_block === 0) { this._stopImmediate(); return; }
      const target = this._calcQuantizeTarget();
      if (target <= c.record_block)
        this._startEndingRecording(target, true);
      else {
        c.quantizeTarget = target;
        c.quantizeWillPlay = true;
        this.emit(`quantize deferred target=${target}`);
      }
    } else if (c.state === CS.RECORDED) {
      this._startPlaying();
    }
  }

  pressStop() {
    const c = this.clip;
    if (c.state === CS.RECORDING_MAIN) {
      const target = this._calcQuantizeTarget();
      if (target <= c.record_block)
        this._startEndingRecording(target, false);
      else {
        c.quantizeTarget = target;
        c.quantizeWillPlay = false;
      }
    } else if (c.state === CS.PLAYING) {
      c.state = CS.STOPPING;
      c.crossfade_start = c.play_block;
      c.crossfade_offset = 0;
      c.play_block = 0;
    }
  }

  _stopImmediate() {
    this.clip.init();
    this.running = 0;
  }

  _calcQuantizeTarget() {
    const M = this.masterLoopBlocks;
    if (M === 0) return this.clip.record_block;
    const CB2 = this.C.CROSSFADE_BLOCKS * 2;
    const rb = this.clip.record_block;
    const candidates = [M>>3, M>>2, M>>1, M, M*2, M*4, M*8];
    let best = M;
    let bestDist = rb > M ? rb - M : M - rb;
    for (const c of candidates) {
      if (c < CB2) continue;
      const dist = rb > c ? rb - c : c - rb;
      if (dist < bestDist) { best = c; bestDist = dist; }
    }
    return best;
  }

  _startRecording() {
    const c = this.clip;
    c.play_block = 0; c.record_block = 0;
    c.crossfade_start = 0; c.crossfade_offset = 0;
    c.num_blocks = 0; c.max_blocks = 999999;
    c.recordStartPhaseOffset = this.masterPhase;
    c.state = CS.RECORDING;
    this.running++;
    this.emit(`startRecording offset=${c.recordStartPhaseOffset}`);
  }

  _startEndingRecording(trim, willPlay) {
    const c = this.clip;
    const CB = this.C.CROSSFADE_BLOCKS;
    c.num_blocks = trim > 0 ? trim : c.record_block;
    c.max_blocks = c.num_blocks + CB;
    c.state = willPlay ? CS.RECORDING_TAIL : CS.FINISHING;
    this.emit(`endRecording nb=${c.num_blocks} max=${c.max_blocks} willPlay=${willPlay}`);
  }

  _finishRecording() {
    const c = this.clip;
    const willPlay = (c.state === CS.RECORDING_TAIL);
    c.state = CS.RECORDED;
    this.running--;
    this.emit(`finishRecording willPlay=${willPlay}`);
    if (willPlay) this._startPlaying();
  }

  _startPlaying() {
    const c = this.clip;
    const M = this.masterLoopBlocks;
    if (M > 0 && c.num_blocks > 0)
      c.play_block = (((this.masterPhase - c.recordStartPhaseOffset) % c.num_blocks) + c.num_blocks * 2) % c.num_blocks;
    else
      c.play_block = 0;
    c.crossfade_start = 0; c.crossfade_offset = 0;
    c.state = CS.PLAYING;
    this.running++;
    this.emit(`startPlaying pb=${c.play_block}`);
  }

  update() {
    const c = this.clip, CB = this.C.CROSSFADE_BLOCKS;

    if (this.trackPending === 1) {
      if (this.masterLoopBlocks > 0 && this.masterPhase % this.masterLoopBlocks === 0) {
        this.trackPending = 0;
        this._startRecording();
      }
    }

    if (this.running <= 0) return;
    this.masterPhase++;

    const isRec = c.state >= CS.RECORDING && c.state <= CS.FINISHING;
    const isPlay = c.state === CS.PLAYING || c.state === CS.LOOPING;

    if (isRec) {
      c.record_block++;
      if (c.state === CS.RECORDING && c.record_block >= CB)
        c.state = CS.RECORDING_MAIN;
      if (c.state === CS.RECORDING_MAIN && c.quantizeTarget > 0 && c.record_block >= c.quantizeTarget) {
        const trim = c.quantizeTarget, play = c.quantizeWillPlay;
        c.quantizeTarget = 0; c.quantizeWillPlay = false;
        this._startEndingRecording(trim, play);
      } else if ((c.state === CS.RECORDING_TAIL || c.state === CS.FINISHING) && c.record_block >= c.max_blocks) {
        this._finishRecording();
      }
    }

    if (c.state === CS.LOOPING || c.state === CS.STOPPING) {
      c.crossfade_offset++;
      if (c.crossfade_offset === CB) {
        if (c.state === CS.LOOPING) {
          c.state = CS.PLAYING;
          c.crossfade_start = 0; c.crossfade_offset = 0;
          this.running--;
        } else {
          c.state = CS.RECORDED;
          c.crossfade_start = 0; c.crossfade_offset = 0;
          this.running--;
        }
      }
    }

    if (isPlay || c.state === CS.PLAYING) {
      const M = this.masterLoopBlocks;
      if (M > 0 && c.num_blocks > 0) {
        const next = (((this.masterPhase - c.recordStartPhaseOffset) % c.num_blocks) + c.num_blocks * 2) % c.num_blocks;
        const wrapped = (next === 0) && (c.play_block > 0);
        if (wrapped && c.state === CS.PLAYING) {
          c.state = CS.LOOPING;
          c.crossfade_start = c.num_blocks;
          c.crossfade_offset = 0;
          this.running++;
          this.emit(`crossfade start=${c.num_blocks}`);
        }
        c.play_block = next;
      } else {
        c.play_block++;
        if (c.play_block === c.num_blocks) {
          c.state = CS.LOOPING;
          c.crossfade_start = c.play_block;
          c.crossfade_offset = 0;
          this.running++;
        }
      }
    }
  }
}

// ========== TEST SUITE ==========

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

    // Test 1: short recording (quantize deferred, fires during recording)
    {
      const m = new MachineSim(C, bpm);
      m.pressRecord();
      for (let t = 0; t < 50; t++) { m.tick = t; m.update(); }
      m.pressPlay();
      for (let t = 50; t < M * 3; t++) { m.tick = t; m.update(); if (m.clip.state === CS.PLAYING) break; }
      const nb = m.clip.num_blocks;
      while ((m.masterPhase - m.clip.recordStartPhaseOffset) % nb !== 0) { m.tick++; m.update(); }
      const pb = m.clip.play_block;
      const ok = pb === 0;
      scenarios.push({ name: `${bpm}BPM short`, ok, pb, M, nb });
    }

    // Test 2: overshoot recording (user presses play after boundary)
    {
      const m = new MachineSim(C, bpm);
      m.pressRecord();
      const overshoot = M + 10;
      for (let t = 0; t < overshoot; t++) { m.tick = t; m.update(); }
      m.pressPlay();
      for (let t = overshoot; t < overshoot + M * 3; t++) { m.tick = t; m.update(); if (m.clip.state === CS.PLAYING) break; }
      while (m.masterPhase % M !== 0) { m.tick++; m.update(); }
      const pb = (((m.masterPhase - m.clip.recordStartPhaseOffset) % m.clip.num_blocks) + m.clip.num_blocks * 2) % m.clip.num_blocks;
      const ok = pb === 0;
      scenarios.push({ name: `${bpm}BPM overshoot`, ok, pb, M, nb: m.clip.num_blocks });
    }

    // Test 3: drift over 1000 loops
    {
      const m = new MachineSim(C, bpm);
      m.pressRecord();
      for (let t = 0; t < 50; t++) { m.tick = t; m.update(); }
      m.pressPlay();
      for (let t = 50; t < M * 3; t++) { m.tick = t; m.update(); if (m.clip.state === CS.PLAYING) break; }
      for (let t = 0; t < 1000 * m.clip.num_blocks; t++) { m.tick++; m.update(); }
      const nb = m.clip.num_blocks;
      while ((m.masterPhase - m.clip.recordStartPhaseOffset) % nb !== 0) { m.tick++; m.update(); }
      const pb = m.clip.play_block;
      const ok = pb === 0;
      scenarios.push({ name: `${bpm}BPM drift-1000`, ok, pb, M, nb });
    }
  }

  let allPass = allChecks;
  scenarios.forEach(s => {
    console.log(`  ${s.ok ? 'PASS' : 'FAIL'} ${s.name}: M=${s.M} nb=${s.nb} pb_at_phrase=${s.pb}`);
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
