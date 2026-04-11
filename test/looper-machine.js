#!/usr/bin/env node
'use strict';

const { CS, ClipSim } = require('./looper-sim');

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

module.exports = { MachineSim };
