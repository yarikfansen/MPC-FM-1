# User guide

A short map of the firmware. The rule everywhere: **SELECT** chooses *where* (track, row position, page, pad, effect), **KNOB 1–4** change *what you see* at the bottom of the screen, in the same colours as the bars above the knob names. The screen shows the parameter you turn, large.

## Buttons

| Button | Tap | Hold |
|---|---|---|
| HOME | one level up (sub page → its page → mixer) | mixer |
| SEQ | step grid; on the grid: VARIATION page | CHAIN (song) |
| EDIT | sound editor of the track (on the grid: step details) | advanced editor (DX7: FM LAB) |
| FX | MASTER FX; again: back | |
| LFO | GLOBAL LFO | |
| GLO | global settings | |
| ARP | arpeggiator page | |
| SEL | track operations (copy, paste, clear, undo…) | |
| SAVE | save / load projects; confirms on several pages | |
| PLAY / REC | transport / record notes (in the sampler editor: record a sample) | |

**HOME + PLAY** stops everything (panic).

## Mixer (home)

- SELECT: track. PRESETS: engine of the track (DX7, 303, PSG, PLUCK, 808, SAMPLER). ALGORITHM: sound from the library — the name changes at once, the sound loads when the knob rests, so turning through the list does not disturb playing.
- KNOB 1–4: volume, filter, effect send, state (on / mute / solo). Changes are saved automatically 3 s later, but never while the sequence plays (writing memory would interrupt the sound): then they are saved when you stop.
- Changing the engine (PRESETS) allocates memory and can click; avoid it while performing.
- The red bar above the steps is the playing position.

## Sequencer

- SELECT: along the row. KNOB 1: between the four rows. KNOB 2: velocity. KNOB 3: gate (SAMPLER tracks: **LENGTH** of the pad on this step, see below). KNOB 4: note (drums: layer).
- Keys on the grid: a melodic track writes the played note into the chosen step; a drum or sampler track toggles the current layer (pad) on that step.
- **SAMPLER step LENGTH:** KNOB 3 shortens the chosen pad on the chosen step only (dim = the pad's own length). It is a parameter lock: one per step, 16 per project.
- PRESETS: length 16 / 32 / 48 / 64. ALGORITHM: recording mode STEP or LIVE (the lamp next to REC).
- Red frame = playing step. White pulsing frame = chosen step.
- EDIT: step details (probability, chord, tie, parameter locks).

### VARIATION (tap SEQ on the grid)

Per track, live while the sequence plays; the written pattern is kept and returns when you stop or save.

| Knob | What it does |
|---|---|
| MUTATE 0–100 % | each loop some notes jump an octave or a fifth (drums: a hit moves to the next free step) |
| HUMAN 0–100 % | each loop the velocities wander |
| RATCHET OFF / 2X / 3X / 4X | about one note in three repeats inside its step |
| CHANCE 100 %…13 % | how often the notes of the track play (written into the steps) |

### CHAIN (hold SEQ)

16 fields = a song. SELECT: along the row, KNOB 1: between rows. KNOB 2: project in the field. KNOB 3: repeats x1–x16. KNOB 4: chain on/off. SAVE: jump to the chosen field now.
Red frame = playing field, white pulsing frame = chosen field.

## Sound editors

EDIT on a track. SELECT turns the pages; each knob shows its own picture. On **808 and SAMPLER** SELECT picks the drum / pad and **ALGORITHM** turns the pages. The 303 filter page shows one curve: CUTOFF moves the corner, RESO the peak, ENV MOD how far each note pushes the curve, DECAY how fast it falls back (both animate).

## Sampler: record and chop

1. Choose a SAMPLER track, press EDIT, then **REC** → SAMPLER REC.
2. SELECT: pad. KNOB 1: SOURCE — USB (audio from the computer), BT (Bluetooth audio) or MIX (the synth's own sound). KNOB 2: THRESH — recording starts when the sound is louder. KNOB 3: LENGTH up to 1.5 s. KNOB 4: MONITOR — hear the input or not.
   The time shown ("0.0 / 1.2 S") is what this pad can really take: the smallest of LENGTH, free RAM and free sample memory. If it cannot record, the box says why: **SAMPLE MEMORY FULL** (record over a pad with a longer sound) or **RAM FULL** (put lighter engines than DX7 / PLUCK on unused tracks).
3. **REC** arms (yellow ARMED); recording starts at the threshold; REC again stops early.
4. CHOP opens: SELECT chooses a slice, KNOB 1 moves its start (10 ms steps), KNOB 2: 1 / 2 / 4 / 8 slices, KNOB 3: EVEN or TRANSIENT (cuts at the hits).
5. **SAVE** writes the slices onto the pads from the chosen pad on, replacing what was there (a few seconds, with a progress bar). **Keep the power on** while "SAVING …% – POWER ON" is shown. If it says SAVE FAILED, the take is kept: press SAVE again. REC: record again. EDIT: cancel.

All eight pads share about 100 KB (≈ 4.5 s in total).

## GLOBAL LFO

PRESETS switches LFO 1 / LFO 2. KNOB 1: target (master effect or a track parameter), KNOB 2: rate (synced or Hz), KNOB 3: depth (±), KNOB 4: wave. DEPTH 0 = off.

## MASTER FX

SELECT: effect (white = on, dim = off). KNOB 1–4: its parameters (usually TYPE, two controls, MIX). The picture behind the name shows the response or the live movement.

## Global settings (GLO)

SELECT turns the round list: output, BPM, swing, click, count-in, clock, low cut, Bluetooth, MIDI routing, track role, scope. KNOB 1 changes the value; SAVE opens the pages marked ">". The bottom line shows CPU load now / peak.

## Saving

SAVE opens projects: KNOB 1 slot, KNOB 4 SAVE or LOAD, the SAVE button does it. Loading a project from another slot asks for a second SAVE.
