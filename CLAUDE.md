# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Real-time beat-mode time-stretching for Pure Data (Pd), inspired by Ableton Live's Beat Mode. No FFT, no phase vocoder — purely temporal domain via transient-preserving looping. Deliberately lightweight on CPU.

**Two-component architecture:**
1. **Python analyzer** (`python-analyzer/analyze.py`): offline onset detection via librosa → produces `.ana` files
2. **Pure Data external** (`timestretch_beatmode~.c`): loads WAV + `.ana` at runtime, performs real-time time-stretch

## Build

Requires Pure Data development headers and the `pd-lib-builder` submodule.

```bash
# First-time setup
git submodule update --init

# Build the external
make

# Output: timestretch_beatmode~.pd_darwin (macOS) or .pd_linux
```

The compiled binary must be placed where Pd can find it (same directory as the help patch or in Pd's search path).

## Python Analyzer

```bash
cd python-analyzer

# Create and activate venv (first time)
python3 -m venv analyzer_env
source analyzer_env/bin/activate
pip install librosa soundfile

# Analyze an audio file (outputs <name>.ana and <name>.json)
python analyze.py <file.wav>
```

The `.ana` file format is a plain-text file read by the C external:
```
# sample_rate 48000
# tempo 128
12345
24690
...
```

The analyzer hardcodes 48 kHz output — all audio files must match this sample rate.

## Algorithm: How the DSP Works

The perform routine (`timestretch_beatmode_tilde_perform`) implements this per output sample:

1. Convert output timeline position (`pos_global`) to source position: `pos_audio = pos_global * tempo_ratio`
2. Determine which transient segment we're in (from the `.ana` transient array)
3. **Attack phase** (first 50% of the segment in output time): play source audio 1:1 — preserves transient character
4. **Sustain phase** (second 50%): loop the second half of the source segment with Hann fade-in/out — stretches sustain

The `tempo_ratio = tempo_target / tempo_original` controls the time-stretch amount.

Key state variables in `t_timestretch_beatmode_tilde`:
- `pos_global`: output timeline counter (samples)
- `current_transient`: index into the transients array
- `in_sustain`: whether we're in attack or sustain mode
- `sustain_pos`: position within the loop (wraps modulo loop length)

## Known Limitations (see IMPROVE.md for full roadmap)

- No crossfade between attack→sustain transition → audible clicks
- No crossfade at loop cycle boundaries → clicks on loop wrap
- Loop point is arbitrary 50% mark, no zero-crossing alignment
- No interpolation for fractional sample positions (nearest-neighbor cast)
- `pos_global` is `int` → position drift on long files (fix: use `double` or `unsigned long`)
- Help patch (`timestretch_beatmode~-help.pd`) has an unresolved Git merge conflict

## Pure Data External Conventions

- Object name: `timestretch_beatmode~` (tilde = audio-rate object)
- Inlets: none (control via messages)
- Outlets: `[signal~]` audio out, `[bang]` for mode changes, `[list]` for debug info
- Message: `play <wavfile> <target_tempo>` — loads files and starts playback

The `.ana` file path is derived from the `.wav` path by replacing the extension.

## File Layout

```
timestretch_beatmode~.c       — single-file Pure Data external
timestretch_beatmode~-help.pd — help patch (has merge conflict, needs fixing)
Makefile                      — uses pd-lib-builder
python-analyzer/
  analyze.py                  — onset detection script
  *.ana / *.json              — pre-computed analysis files for test audio
IMPROVE.md                    — prioritized improvement roadmap (read before making changes)
```
