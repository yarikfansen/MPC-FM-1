# FM-1 phone companion (PWA) — design notes

Status 23.10.2026: design phase only (mockups `docs/mockups/FM1-phone-app.html`). No app code until the user agrees.

Target: Chrome on the Android phone, USB-C cable to the FM-1, WebMIDI with SysEx. Installable PWA, works offline.
The app reuses the workbench code: `sdk/WebUpdate.js` (`FM1Web.Library`, DX7 pack/unpack, frames), `sdk/Samples.js`,
and the simulator wasm (`web/engine` + `web/synth`) for on-phone sound.

## SysEx used by the workbench (checked in code)

### 1. Our file protocol (`app/transfer.c`, client `FM1Web.Library` in `sdk/WebUpdate.js`)
Frame: `F0 7D 46 4D 02 cmd tokLo tokHi <packed payload> sum F7`
- `7D` = non-commercial ID, `46 4D` = "FM", `02` = protocol version.
- Payload: 8-bit bytes packed LSB-first into 7-bit stream (`demoFrame`, `pack7`).
- `sum`: sum of bytes from `7D` to `sum` ≡ 0 mod 128.
- Reply: `cmd | 0x40`, same token; first data byte = status (0 ok, 1 empty, 64-n = error -n; 71 = unknown command, 72 = no memory, 73 = old sample bank too big).
- One request at a time, 1 s timeout, 3 retries; exact retry gets the cached reply. Chunks 64 bytes (samples 128 write / 64 read). Transfer cancelled after 10 s silence.
- Firmware accepts it on all MIDI ports (USB, BLE, DIN); replies go back to the same port.

| cmd | payload → reply | use |
|---|---|---|
| 0 | – → ver, slots(16), current slot, project bytes(2), clock mode, 8 engine types | connect / identify |
| 21 | – → tuning flag, tuned project size, sample region | capabilities v1 |
| 22 | – → 1, 128 programs, 4 banks, bank library available, sample limit(4)=102400, flags | capabilities v2 (128-voice library) |
| 1 | kind, target → size(2) | start **read**: kind 0 project, 1 DX7 voice of track (156 B VCED), 2 timbre of track (223 B), 3 tuned project (4264 B), **4 DX7 bank `target`=0..3 (4096 B = 32 × 128 B VMEM)** |
| 2 | offset(2), n≤64 → bytes | read chunk |
| 3 | kind, target, size(2) | start **write** (same kinds) |
| 4 | offset(2), ≤64 bytes | write chunk (in order) |
| 5 | CRC32(4) | commit: kind 4 → `groove_fm_library_write` (flash, staged + journaled, survives power loss); kind 1 → voice into track RAM |
| 6 | – | cancel / free buffer |
| 7 / 8 | slot | save / load project slot |
| 9 | slot → name(12) | project slot name |
| 10 | 12 ASCII | rename current project |
| 11 | – → status, size(4), region(4) | sample bank info |
| 12 / 13 / 14 / 16 | size(4) / offset(4)+≤128 B / – / – | sample bank write: begin, chunk, finish, cancel |
| 15 | offset(4), n≤64 → bytes | sample bank read |
| 17 / 18 / 19 / 20 | – → stats | engine / render / audio stats, master state |

Workbench flow for a bank: `writeBank(i)` = cmd 3 (kind 4) → 64 × cmd 4 → cmd 5 (CRC) → cmd 6, then read back (cmd 1/2) and compare.
`saveVoice(program)` = read the whole bank, replace 128 bytes, write the bank back. 128 programs = banks A–D (`program>>5`).

### 2. Standard Yamaha DX7 single voice (fast audition, no flash write)
`F0 43 0n 00 01 1B <155 B VCED> sum F7` (163 bytes). Firmware imports it into the RAM patch of **every track whose MIDI input channel = n** (`transfer.c`, `message[1]==0x43`). Not saved to flash.
→ live editing from the phone: send this, throttled (~15–20/s), while knobs turn. DX7 parameter-change SysEx (`F0 43 1n …`) and 32-voice bulk (`F0 43 0n 09 …`) are **not** accepted by the firmware; banks go through cmd 3–5 kind 4.

### 3. Other
- MMC `F0 7F xx 06 cmd F7` → transport start/stop.
- Firmware update uses a different protocol (`frame()`, `F0 00 59 …`/`F0 22 24 35 7F F7`). The phone app must **not** contain it (rule: never flash).

### File formats (already in `WebUpdate.js`)
- `.syx` single voice: 163 B (`voiceFile`), bank: 4104 B = `F0 43 0n 09 20 00 <4096> sum F7` (`bankFile`). `readVoices` reads both.
- Voice name = VCED bytes 145..154 (10 ASCII), in VMEM 118..127.
- Projects `.fm1`, timbres `.fm1t` — out of scope for v1.

### Samples (`sdk/Samples.js`)
Bank: 512 B header ("FM1S", 32 kHz) + up to 8 slots; audio = SEA5 codec, 228 B per 320 frames (≈5.7 bit/sample, ≈22.8 KB/s mono).
Encoder is in the simulator wasm (`sim_sample_encode`). Device capacity now **102 400 B ≈ 4.4 s total** for all slots.
Trade-off: each extra 4 KB sector of USER area ≈ +0.18 s of sound, taken from flash shared with program headroom (≥16 KB must stay free); the old 524 KB region gave ≈ 22 s. Phone side can help by: mono, trim tight, lower pitch range (play back pitched), normalise before encode.

## Plan of the app (v1 = priorities 1+2)
1. **Library**: banks A–D × 32, read all 4 (≈ 4 × 66 round trips, ~2–4 s), list with names, drag to reorder, long-press = rename / copy / move to bank, drop .syx (voice or bank) into a slot, export voice/bank, "send bank" button only for banks changed (dot marker). Local copy kept in IndexedDB (works without the synth).
2. **Voice editor**: pages COMMON (algorithm/CHOIR, feedback, transpose, name), OP1–6 (level, ratio/fixed, detune, EG rates/levels, kbd scaling, velocity), LFO, PITCH EG. 4 big knobs per row in K1–K4 colours, operator cats as in FM LAB. Sound: wasm DX7 on the phone, or live single-voice SysEx to the synth. "Save to slot" writes the bank.
3. **Keyboard**: full screen landscape, 2 octaves (swipe = octave), velocity by touch height (top soft, bottom loud), multi-touch, pitch/mod strip. Output: USB MIDI channel of the track, else the built-in engine.
Later: 4. sample import (file picker / screen recording audio, trim, normalise, preview, encode, upload); 5. our sequencer + arpeggiator.

## Risks / checks
- **WebMIDI SysEx on Android Chrome**: supported, needs a secure context (HTTPS or installed PWA) and a permission prompt "control and reprogram MIDI devices". Must test that the FM-1 shows up as a USB MIDI port on the Pixel (class-compliant USB MIDI — should work; USB-C OTG is fine on Pixel). Chrome on Android also needs the device connected **before** `requestMIDIAccess` sometimes; handle `statechange`.
- **Latency**: USB MIDI from Chrome ~3–10 ms, fine for the keyboard. Built-in engine via AudioWorklet on Android: typically 20–40 ms output latency — fine for preview, mediocre for playing. Touch-to-sound adds ~10–20 ms.
- **Throughput**: one 64-byte chunk per round trip; bank ≈ 66 round trips. If Android USB MIDI adds per-message latency, reads get slower (still seconds, not minutes). Live audition SysEx must be throttled so it never starves note messages.
- **Flash wear / safety**: bank writes go to flash through the journaled path; app writes only on explicit "Save", never per knob turn. The app never touches firmware update commands.
- **Sample memory**: 100 KB ≈ 4.4 s total — the import UI must show seconds left before encoding.
- **iPad / iPhone**: Safari has no WebMIDI → needs a native app (Swift, CoreMIDI) built on a Mac with Xcode and an Apple developer account. Not planned now.
- **BLE MIDI**: firmware answers on BLE too; on Android it appears in Chrome only after pairing in the OS/an app. USB first.
- **Screen recorder audio**: Pixel recorder saves MP4/AAC; decode with WebAudio `decodeAudioData` (works for AAC in Chrome). Copyright of captured audio is the user's responsibility.
- Not yet checked on the device (rule: nothing with the device until the Transporter dump).

## Build status
- Code in `phone/` (static PWA, no build; dev server config `fm1-phone` port 8767). `phone/vendor/WebUpdate.js` is a copy of `sdk/WebUpdate.js` — re-copy when the SDK file changes.
- Step 1 (ca2c76a): shell, WebMIDI connect (read-only commands), IndexedDB bank store.
- Step 2: library on .syx files — tabs A–D, drag reorder by grip, cross-bank drag copy in landscape, long-press menu (rename / copy / swap / export / import / clear), undo toast, import voice 163 B / bank 4104 B, export. Send to / read from synth buttons are disabled until step 3.
- Step 3: FROM SYNTH reads banks A–D (`Library.readBank`), TO SYNTH writes only dirty banks after an explicit confirm (`Library.writeBank` = upload + read-back compare). Buttons are disabled without a connection.
- Dev test without a unit: serve the repo root (`fm1-phone` launch config) and open `/phone/index.html?sim`. `phone/js/simmidi.js` fakes WebMIDI over `build/web/simulator.wasm`, i.e. the real firmware transfer code. Checked 13.11: connect, read 4 banks (~1.3 s), rename + write bank A (~0.8 s), device read-back matches. Not yet tried on a real FM-1 or a Pixel.
- Step 4: editor + built-in sound. Double-tap a voice row (or the EDITOR tab) opens it; pages ОБЩЕЕ / OP / EG / SCALE / LFO / PITCH, FM LAB cats pick the operator, knobs drag vertically (double tap = type a value), mini keyboard (velocity = touch height), SAVE writes the voice into the local bank (then TO SYNTH). Sound: `vendor/simulator.wasm` (the firmware audio code, copy of `build/web/simulator.wasm` + `.json`; re-copy after firmware/web rebuilds) in an AudioWorklet (`js/engine.js`). `js/out.js` routes notes and live voice edits (standard `F0 43 0n 00 01 1B` dump, RAM only, ≤16/s, never flash) to the connected FM-1 on a chosen MIDI channel (CH pill, default 1) or to the engine; in `?sim` mode it always uses the engine.
- Checked in the browser 13.11: factory PIANO 1 plays (peak 0.18), algorithm change saved and written through the firmware wasm, read-back matches. Algorithm picture links are approximate (nearest operator one row down), not the exact DX7 wiring. Not checked: real synth channel mapping, Pixel audio latency.
- Step 5: keyboard (`js/keyboard.js`): landscape 14 white keys (2 octaves), portrait 8; multitouch, glissando by sliding, velocity 30..127 from touch height (top soft), PITCH strip (springs back, 14-bit bend) and MOD strip (CC1), OCT ◀ ▶, HOLD (CC64), СТОП (all notes off), ⤢ full screen + landscape lock attempt. Plays the voice open in the editor (sent as the single-voice dump before the first note); target pill shows FM-1 channel or built-in sound. `out.noteOn` cancels a note released while the engine is still booting. Checked in the browser: 3 simultaneous notes, velocity bytes 35/122 for top/bottom touches, pitch spring-back. Engine shows a short level blip (~0.05) right after start, then silence. Not checked on Pixel (touch latency, fullscreen/orientation lock, real synth mapping of bend/mod/hold).
- Step 6: install notes in docs/phone-install.md (offline cache verified on PC; phone test pending the user).
