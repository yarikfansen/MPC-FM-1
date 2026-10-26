# Install and recovery

> Beta firmware, no warranty. Back up projects and samples first; the install rewrites the program, and a failed write can leave the synth unable to start until you recover it (below).

## You need

- A computer with **Google Chrome** (it can talk to the synth over MIDI).
- A USB **data** cable, plugged directly into the computer (no hub).
- A charged FM-1.
- Other apps that use MIDI closed.

## Install

1. Download the release files from **Releases** and check them against `SHA256SUMS.txt` if you like.
2. Open `MPC-FM-1-install-<version>.html` in Chrome. Allow MIDI when Chrome asks (if you said no, allow it in the site settings).
3. Press **Connect device** and choose the FM-1.
4. Tick that you have a backup, press **Start update** and wait. Do not unplug or switch off while it writes; the synth restarts by itself.
5. If the page says the update cannot start (for example because another custom build is installed), open **Recovery & force update**, enable force flash and start again.
6. If the page says *"Timed out waiting for reconnection"* and the synth screen is black: the synth is already in M-VAVE's loader (it has no display) and the page did not find it in time. This is common on the first try. Keep the page open, press **Connect device** and **Start update** again with the **same page** — the second try finishes.

## If something goes wrong

**The synth starts but hangs or resets with a project** — **SAFE mode**: switch on and hold **OCT−** during the first 1.5 s. The synth starts without loading projects; the screen explains how to continue. Power off and on for a normal start.

**The synth does not start (black screen)** — **USB download mode**: hold **OCT−** and **OCT+** together while switching on, for about 3 s. Then install the official firmware with M-VAVE's **M-UPGRADE** updater (Windows) and the official file **FM-1_015.fwsc** (included in this repository as `sdk/firmware/FM-1_015.fwsc`, Apache-2.0). After that you can install this firmware again with the install page.

**Nothing helps** — the community tool **FM-1 Transporter** (Seeed XIAO RP2040, three wires to D+, D−, GND; never VBUS) can read and write the flash directly: https://github.com/kurogedelic/FM-1-transporter. Read and save a full backup first; never write below address `0x4000` (bootloader).

## Back to the official firmware

Use M-VAVE's updater with the official V15 file, as in "black screen" above (download mode is not needed if the synth starts normally).

## Report

Please open an Issue with the firmware version, what you did and what happened — see the README.
