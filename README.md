# MPC-FM-1

# HI, DEARS! <3 Welcome to try my own view on portable synth FW! 
# Its try to combine features of MPC + Electron + Roland 303 under HUGE Effects in FM-1

# I need Yours Opinion about.

 
Custom firmware for the **M-VAVE FM-1** pocket synth.

> **Beta.** Tested on the author's FM-1. Back up your projects and samples before you install, and read [Install and recovery](docs/INSTALL_AND_RECOVERY.md) first. No warranty.

## What we aim for

- **Quality sound.** Every engine, filter and effect should sound good on its own, at any setting.
- **A wide range of functions.** Sequencing, variations, song chains, sampling and modulation, all on the device.
- **Simple, intuitive control.** The same knob does the same kind of job on every page. The screen shows the thing you turn, large and clear, so you can play without a manual.

## What is new

Based on the official open release FM1 Open V14 by M-VAVE ([fm1_open](https://github.com/m-vave/fm1_open)); its notes are kept in [docs/UPSTREAM-FM1-OPEN-V14.md](docs/UPSTREAM-FM1-OPEN-V14.md).

**Screens and control**
- New look on every page: larger text, a picture of the parameter you turn, clear colours per knob.
- One filter picture for all four 303 filter knobs; ENV MOD and DECAY animate the curve.
- Sequencer: **SELECT** moves along the row, **KNOB 1** moves between rows. The playing step is red, the chosen step is a white pulsing frame.
- **HOME** goes one level up (sub page → its page → mixer); hold HOME for the mixer.
- The **GLOBAL** menu turns round in a circle.
- On **808 and SAMPLER** pages SELECT picks the pad, ALGORITHM turns the pages.
- **Live-safe:** turning through sounds loads only the one you stop on; autosave never interrupts a playing sequence.

**Sequencer**
- **VARIATION** page (tap SEQ on the grid): MUTATE (notes jump an octave or a fifth each loop), HUMAN (velocities wander), RATCHET (notes repeat 2–4 times inside a step), CHANCE (how often notes play). The written pattern is kept; it comes back when you stop or save.
- **CHAIN** (hold SEQ): up to 16 projects in a row, each played **x1 to x16** times — a whole song.

**Sampler**
- **Record samples on the device** from USB audio, Bluetooth audio or the synth's own output (MIX), with a threshold, length and monitor switch.
- **CHOP** the recording into 1, 2, 4 or 8 slices, evenly or at the hits (TRANSIENT), move each start, and save the slices onto pads.
- The REC page shows how long this pad can record and, if it cannot, why.
- **Sample length per step** on the grid (KNOB 3 on a SAMPLER track).

**Modulation and effects**
- **Two global LFOs** (PRESETS switches LFO 1 / LFO 2), each to a master effect or a track parameter.
- **Master FX:** ACID / SEM / LADDER / COMB / DJ filters, drive, chorus, phaser, ring modulator with 8 carriers (incl. pitch tracking and frequency shift), reverb, EQ, delay.

**Sounds**
- More presets in the built-in library: new 303 basses and leads, PSG chip sounds, plucked sounds and 808 kits.

**Safety**
- **SAFE mode:** hold **OCT−** during the first 1.5 s after power on. The synth starts without loading projects, so a bad project cannot block start-up.

## Download and install

**Current version: v0.1 beta** — download from [release/v0.1-beta](release/v0.1-beta):
[install page](https://github.com/yarikfansen/MPC-FM-1/raw/main/release/v0.1-beta/MPC-FM-1-install-v0.1-beta.html) ·
[firmware .fwsc](https://github.com/yarikfansen/MPC-FM-1/raw/main/release/v0.1-beta/MPC-FM-1-v0.1-beta.fwsc) ·
[SHA256SUMS](release/v0.1-beta/SHA256SUMS.txt)

The same files are listed on the **Releases** page. Each release has:
- `MPC-FM-1-install-<version>.html` — the install page (open it in Google Chrome),
- `MPC-FM-1-<version>.fwsc` — the firmware file,
- `SHA256SUMS.txt` — checksums,
- the source code of that release.

### Install in short

1. Open the install page in **Google Chrome**, allow MIDI.
2. **Connect device** → choose the FM-1 → tick the backup box → **Start update**.
3. **The first try usually stops** with *"Timed out waiting for reconnection"* while the synth screen is **black**. This is normal: the synth is already in M-VAVE's loader (it has no display) and the page did not find it in time. Nothing is broken.
4. **Do not close the page and do not unplug the synth.** Press **Connect device** again, choose the FM-1, press **Start update** again on the **same page**. The second try writes the firmware and the synth restarts with *MPC FM-1* on the screen.
5. If the page refuses to start the update, open **Recovery & force update**, enable force flash and start again.

Full steps and recovery: [docs/INSTALL_AND_RECOVERY.md](docs/INSTALL_AND_RECOVERY.md). How to play: [docs/USER_GUIDE.md](docs/USER_GUIDE.md).

## Feedback, reviews, bugs

Your experience shapes the next version. Please use **Issues**:
- [Feedback or review](../../issues/new?template=feedback.md) — what you like, what is hard, what you miss.
- [Bug report](../../issues/new?template=bug_report.md) — what you did, what happened, firmware version, a short video if you can.

## Support the project

[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-support%20MPC--FM--1-ff5e5b?logo=ko-fi&logoColor=white)](https://ko-fi.com/yarikfansen)

MPC-FM-1 is free and open and will stay so. If it makes your FM-1 more fun, a donation on **Ko-fi** helps to spend more time on it — new engines, sounds and functions, and testing on real hardware:

**https://ko-fi.com/yarikfansen**

Donations are completely optional. Reviews, bug reports and ideas in Issues help just as much.

## Build from source

Windows, the JieLi pi32 toolchain and, for the browser simulator, WASI SDK. `build.bat` builds the firmware; `build.bat --web` builds the simulator; `build.bat --package` builds the install page. Tests: `tests/run_*.sh`.

## License

- Application code (`app/`, tools, web): **GPL-3.0** with the FM1 SDK linking exception — see [LICENSE.md](LICENSE.md).
- The M-VAVE engine library (`sdk/lib/libfm1_engine.a`) is proprietary; M-VAVE allows redistributing it in binary form for FM-1 devices — see [THIRD_PARTY.md](THIRD_PARTY.md). Keep both files with every copy.
- The SEA5 sample codec is MIT (Dani Biro); see THIRD_PARTY.md.

M-VAVE and FM-1 are trademarks of their owners. This project is not affiliated with or endorsed by M-VAVE.
