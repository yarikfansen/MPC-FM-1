# Phase V1 checklist (29.10.2026): firmware screens vs `docs/mockups/FM1-v12-screens.html`

Checked in the web simulator (`build/preview`, helper `tour.js` next to it), every id side by side with its mockup. Not flashed, no package built yet (PLAN: package only after the user's "yes").

Done everywhere (C0–C5):
- ONE knob font (`knob()` in `app/screen.c`): name lcd16 white (lcd13 above 6 letters), value lcd24 in the knob colour, line 48x2 with a soft glow, 3 px + lit panel on the knob turned last. TRACKS special font, `knob_icon` (BINAURAL) and `wave_knob` (FM LAB) removed — they used other fonts.
- Saturated palette: knob colours, track colours, white text, F A S L, PLAY triangle, curves amber, bars with an edge.
- ENGINES tiles sit exactly over the knobs (x 4 + 60 col); "LIVE REC" / "STEP REC" alert labels removed.

| id | state | notes |
|---|---|---|
| mx MIXER | matches | 2x4 grid, engine colours, knob font. Knobs are the firmware's (VOLUME BRIGHT SEND STATE), the sketch's TRACK SOUND VOL STATE is illustrative |
| seq SEQ | matches | title SEQ, slot row amber (playing filled, stored framed), cells pink, plate 16+chain |
| stp STEP EDIT | matches | new window: note large, 8 cells of the block, velocity + gate bars; knobs stay CHORD TIE LOCK VALUE |
| fx1–fx3 TRACK/SHARED/DELAY FX | matches | one knob font, bar tracks with an edge |
| lfo LFO | matches | |
| bin BINAURAL | matches | knobs RHYTHM CARRIER TIMER NOISE (P3 = pink 3) in the common font. Open: animation lag fix not drawn in the sketch, not changed |
| b3d 3D SOUND | matches | knobs MODE (X / OFF) WIDTH FEED MIX; head/speakers as coded before |
| mfx0–mfx1 MASTER FX | matches | every curve amber on a dark amber fill |
| arp ARP | matches | window with the mode name, pattern bars (sounding one lit). The cat, MODE/OCTAVES captions removed (not in the sketch); mode on PRESETS, octaves on OCT-/OCT+ as before |
| pre PRESETS | n/a | the firmware has no PRESETS page (PRESET_BROWSER is dead); sounds change in place from the mixer |
| eng ENGINES | matches | 4x2 tiles over the knobs. Knobs are still the mixer's; ENGINE PRESET BANK VOL belongs to phase V2 |
| sav SAVE | restyled | plates SAVE/LOAD, slot plate, window with 16 slots, name, SAVE TO tick. The D10 logic (SONGS, 15 song slots) still waits for E7 |
| sel SEL | matches | five-row list, step plate, action knobs |
| env AMP ENV, DX7 LFO, VOICE RATES/LEVELS | matches | windows with titles, op plates 1..6 + P in the header, ADSR plate |
| ble BLUETOOTH | matches | 4 device rows, plate ON/BT, knobs CONNECT SCAN DISC. Rows with real devices not shown in the simulator (no radio) |
| glb GLOBAL | matches | list of 5 rows (chosen lit), plate with CPU %. Service line CPU/MEM/BAT moved to INFO |
| inf INFO | matches | list FIRMWARE BUILD RAM FREE CPU PEAK LAST RUN PEAK BATTERY KO-FI, plate CPU. FAQ knob kept |
| md0–md4 MICRO D | matches | small captions (DARK, KEY DOWN, ...) removed, "ONE FILTER" |
| dx7 DX7 ALGORITHM | matches | the FM LAB cats page is now the v12 ALGORITHM window (blocks sized to the stack, chosen operator amber, ALGO plate); rate/level pages keep their cats |
| t303 303 FILTER | matches | window, parameter name as title, curve |
| t808 808 | matches | pads in own colours, drum name as title. Pads show icons, the sketch's 3-letter names are illustrative |
| smp SAMPLER REC | matches | 8 pad plates in the header, window REC/ARMED/DONE, meter, SRC/MON, wave |

Not verified: everything on the device; BLE with devices; DX7 ALGORITHM page; Russian strings of the new INFO rows (fall back to English).
