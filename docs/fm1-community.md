# M-VAVE FM-1: community research notes

Compiled 2026-10-22 from WebFetch summaries (not full page reads). Facts marked "user claim" or "OP spec" are not verified.

## 1. Sources

| Source | URL | Accessible | Date range seen |
|---|---|---|---|
| Hacker News thread | https://news.ycombinator.com/item?id=49994065 | Yes | Comments span about 2 weeks; newest about 6 days old at fetch time (approx. mid-Oct 2026, inferred) |
| Elektronauts "Other Gear" | https://www.elektronauts.com/t/m-vave-fm-1/252170 | Yes (first page only; "next page" exists) | June 18 to June 22, 2026 |
| Gearspace | https://gearspace.com/threads/m-vave-fm-1.1465371/ | No (HTTP 403) | n/a |
| KVR Audio forum | https://www.kvraudio.com/forum/viewtopic.php?t=632127 | Yes (some attachments need permission) | July 30 to Oct 4, 2026 |
| r/MVaveFM1 (old.reddit) | https://old.reddit.com/r/MVaveFM1/ | No (WebFetch refused the domain) | n/a |
| r/MVaveFM1 top JSON | https://www.reddit.com/r/MVaveFM1/top/.json?t=all&limit=25 | No (WebFetch refused the domain) | n/a |

Reddit could not be read with the tools available here. A browser session or manual check would be needed.

## 2. Hardware facts

From the Elektronauts original post (OP spec, unverified):
- 6-operator FM, 32 algorithms, 128 factory presets; up to 12 voices in poly mode.
- 27-key silicone keyboard, piano layout.
- Colour TFT display.
- Effects: filter, reverb, delay, distortion, chorus, phaser.
- Arpeggiator (7 modes); 16-step sequencer with 16 sequences/patterns.
- MIDI over USB, Bluetooth BLE, and 3.5 mm MIDI IN. Built-in speaker, 3.5 mm stereo headphone out (speaker mutes on plug-in).
- USB-C charging; 2000 mAh battery, about 12 h (vendor claim).
- Size about 161.5 x 96.5 x 28.6 mm, about 251 g. KVR user gives 6.5 x 4 x 1.3 in.
- Imports standard FM SysEx banks.

From users:
- Price about $60-80. Elektronauts: about $68-75 on AliExpress. KVR: about $80 on Amazon; warned against reseller markups. One report says it sold out.
- Encoders resemble the Sonicware SmplTrek type, with no push function. Knobs labelled 1-4.
- Keys are small and membrane-like; durability concerns.
- 5-pin DIN to TRS MIDI IN worked with an MPK Mini IV. USB MIDI and USB audio work with a driver (confirmed in Audacity, not FL Studio).
- Bluetooth audio works; Bluetooth MIDI was difficult for at least one user.
- Chip (user claim, HN): a JieLi-family part; a commenter cites the AC79 devkit and says a public devkit exists.
- Firmware loads over MIDI SysEx on a USB-MIDI class device (HN).

## 3. Official firmware history

- Versions seen in readable sources: v9, v14, v15 (KVR). V12, V13, V16 are not mentioned in any readable source.
- v9 and v14 (KVR): single-parameter SysEx editing from Dexed or other DX7 editors; sequencer rests; single-preset receive (overwrites a slot on save); on-the-fly velocity adjustment.
- v15 (KVR): sequencer step scrolling (hold REC, use Select); simple pattern chaining. Answers an earlier wish.
- Update method: SysEx; a WebUSB installer exists in community projects (sloop-fm1, HN). A Chinese-language-only updater is also mentioned (KVR).
- Users often cannot tell which version they have installed (KVR).
- Bricking and recovery (KVR):
  - Overwriting factory banks cannot be reversed through normal means.
  - One user restored the factory banks with the Chinese updater, which downgraded firmware. They captured and decoded the updater's SysEx traffic.
  - That poster rebuilt the factory set as four standard DX7 banks, restorable with Dexed or any SysEx tool, and published a GitHub repo with banks, a decoder script, a SysEx protocol doc, and voice provenance. Claims not independently verified.
- No official unbricking tool is named. The community repo above is the only recovery path mentioned.
- The device cannot export its own patches via SysEx (KVR poster); no factory SysEx has been extracted from firmware.

## 4. Common complaints and bugs

- Keys: small, silicone or membrane, hard to play, durability concern.
- Sound: described as samey (FM and Karplus-Strong); compared to late-1990s/early-2000s virtual analog (HN).
- Sampling: limited; simple recording and pitching only (HN).
- Sequencer: no step scrolling until v15; ties awkward to enter; one user reports a one-step gap between patterns (no fix shown).
- Controls: too few; fiddly (Elektronauts).
- Bluetooth MIDI: difficult for at least one user.
- Documentation: poor; a roughly 85-page unofficial manual was made by a community member.
- Trust: distrust of Chinese-hosted downloads.
- UI: a good UI is wanted (Elektronauts).
- Sound and battery noise: not mentioned in any readable source.
- Battery: only the vendor 2000 mAh / 12 h spec; no user reports.

## 5. Most wanted features and UI wishes

- SD card sampler, or a "1.5" revision with SD storage (HN).
- Load firmware from USB storage instead of SysEx-only (HN).
- More DSP power or a hardware mod (HN).
- Export of device patches via SysEx (KVR).
- A switch to turn pattern and patch linking off (KVR, not found at last report).
- Sequencer step scrolling (delivered in v15).
- An Elektron-style or otherwise better UI (Elektronauts).
- Open SDKs from makers in general (HN).

## 6. Custom and open firmware efforts

- BaudGirl "FM-1+VA" (KVR): virtual analog firmware; sequencer grows to 64 steps; chords up to 9 notes (user claim). Often named as the most prominent custom build (HN).
- Felucca (hugelton): alternative firmware with a web-based editor (HN); KVR user says they have not tried it.
- Other projects named in HN: fm1-x0x, groove-os, sloop-fm1 (WebUSB installer), fm1-fomni, aroum/fm1-custom-fw.
- Reverse engineering: AL-255/FM-1-RE (the subject of the HN post).
- Editors and lists: fm1-editor.com (firmware list and editor); a community "awesome-fm-1" list; Dexed recommended for editing (KVR).
- Web-based emulators let people try firmware without hardware (HN).
- Melodee: not mentioned in any readable source.
- JieLi: AC79 devkit cited as a user claim (HN); no SDK, toolchain, or flashing details found in readable sources.
- Gearspace and Reddit were not readable, so their firmware content is missing.

## 7. Ideas for a custom UI firmware

- Keep SysEx and WebUSB flashing; document a safe backup of factory banks before any flash.
- Build on the DX7 SysEx compatibility; the decoder and banks repo is a useful base for recovery.
- Add a clear, screen-based UI for the 4 knobs and small keys; consider menu paging and shortcuts.
- Expose step scrolling, pattern linking toggle, and SysEx patch export as first-class features.
- Support USB storage or SD-card loading for banks and samples to avoid SysEx-only workflows.
- Verify JieLi toolchain and SDK details before planning a build; the AC79 devkit claim is unconfirmed.
- Keep a version and rollback path; firmware downgrades have been used to recover.
- Consider Bluetooth MIDI stability as a test item.
