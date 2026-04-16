# Lanmower's Looper

A bare-metal audio looper running on Raspberry Pi 4, controlled by an Akai APC Key 25. No operating system, no drivers, no latency — just hardware and code.

## What it does

- 5-track loop recording with overdub, quantized to Ableton Link tempo
- Live pitch shifting via keyboard keys (signalsmith-stretch DSP)
- Formant-preserving pitch control
- Tape-style delay with smooth time scrubbing
- Reverb with adjustable decay
- 2-pole SVF lowpass filter with resonance
- 2-pole SVF highpass filter (Butterworth)
- USB audio I/O via Behringer UCA222 (48kHz)
- USB-C OTG audio gadget for direct computer connection
- WiFi Ableton Link sync (joins or creates "ticker" network)
- VU metering on APC pad grid

## Hardware

- Raspberry Pi 4 (runs bare-metal, no Linux)
- Akai APC Key 25 (pads + keyboard + knobs)
- Behringer UCA222 (USB audio interface)
- SD card with firmware

## MIDI CC Map (APC Key 25 Knobs)

| CC | Function |
|----|----------|
| 48 | Reverb amount |
| 49 | Delay amount |
| 50 | Time (reverb decay + delay length, 10ms-1000ms) |
| 51 | Highpass cutoff |
| 52 | Pitch shift (continuous, +/-6 semitones) |
| 53 | Pitch shift formant preservation |
| 54 | Lowpass resonance |
| 55 | Lowpass cutoff |

## Keyboard

APC keyboard keys (MIDI channel 1) set live pitch shift based on distance from middle C (note 60). Press C4 for no shift, E4 for +4 semitones, C3 for -12, etc. Pitch locks on key press.

## Pad Grid

- Column 0: Record/play/stop per track (5 rows = 5 tracks)
- Column 1: Tap to mute, hold to erase track
- Columns 2-7: VU meters per track

## Building

CI builds automatically via GitHub Actions. The build:

1. Clones [circle](https://github.com/rsta2/circle) (bare-metal framework) and [circle-prh](https://github.com/phorton1/circle-prh) (audio extensions)
2. Applies patches from `patches/` directory
3. Compiles for RASPPI=4, AARCH=32
4. Produces `kernel7l.img`

Output goes on the SD card alongside the Pi bootloader files.

## Dev Server

`node dev-server.js` runs TFTP (port 69), DHCP (port 67), and syslog (port 514) for network boot and log capture. Requires admin/root for privileged ports.

## Architecture

```
USB Audio In (UCA222) --> AudioInputUSB --> loopMachine --> AudioOutputUSB --> USB Audio Out
                                              |
                                    pitch shift (signalsmith-stretch)
                                    effects (SVF filters, tape delay, reverb)
                                    loop record/playback (5 tracks, crossfade)
                                              |
                              OTG Audio Gadget (USB-C, side-channel)
```

All audio processing runs in interrupt context on core 0. No OS scheduler, no context switches, no jitter.

## License

Based on [circle](https://github.com/rsta2/circle) by Rene Stange and [circle-prh](https://github.com/phorton1/circle-prh) by Patrick Horton.
