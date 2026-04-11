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

  if (clipStateSrc.includes('u32 candidates[]'))
    checks.push({ ok: true, msg: 'loopClipState.cpp: _calcQuantizeTarget uses 7-candidate array' });
  else
    checks.push({ ok: false, msg: 'loopClipState.cpp: _calcQuantizeTarget missing candidates[] array' });

  const modPat = '% m_num_blocks + m_num_blocks) % m_num_blocks';
  if (clipUpdateSrc.includes(modPat))
    checks.push({ ok: true, msg: 'loopClipUpdate.cpp: play_block uses canonical modulo pattern' });
  else
    checks.push({ ok: false, msg: 'loopClipUpdate.cpp: play_block modulo pattern missing' });

  return checks;
}

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

module.exports = { loadConstants, extractClipStates, verifySourceIntegrity, CS, ClipSim };
