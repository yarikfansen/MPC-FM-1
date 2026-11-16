# Installing the phone app on the Pixel (Chrome)

WebMIDI with SysEx and PWA install need a *secure context*: HTTPS or `localhost`. A plain `http://<PC-IP>` is not one.
Checked 13.11 on the PC: service worker caches the whole app (offline-ready), manifest and icons load. Not yet tried on the phone.

## Option A — no internet, no cable to the PC (recommended for the first test)
1. PC and phone on the same Wi-Fi. On the PC, in `<repo>\phone`:
   `python -m http.server 8767 --bind 0.0.0.0` (allow the Windows firewall prompt for private networks). PC address: `ipconfig` (IPv4).
2. On the phone, Chrome → `chrome://flags/#unsafely-treat-insecure-origin-as-secure` → Enabled, text field: `http://<PC-IP>:8767` → Relaunch.
3. Open `http://<PC-IP>:8767/` → menu ⋮ → "Install app" / "Add to Home screen".
4. After the first load the app works offline from the cache; the PC server can be stopped. Updates need the server again (the service worker fetches new files first, falls back to the cache).
5. Plug the FM-1 into the Pixel (USB-C; the phone port is free once the PC cable is out), open the app, tap "нет синта" → allow "control and reprogram MIDI devices".

## Option B — HTTPS hosting (GitHub Pages, publishes the app publicly)
Needs a public repo or Pages enabled; the app contains the firmware wasm (GPL, repo already is). Not done without the user's decision.

## Option C — adb reverse (`adb reverse tcp:8767 tcp:8767`, open `http://localhost:8767`)
Needs adb (not installed here) and USB debugging; the single USB-C port conflicts with the synth cable, so wireless debugging only. More work than A.

## First on-phone checklist
Connect FM-1 → pill turns green; FROM SYNTH reads 4 banks; edit + TO SYNTH; keyboard plays the built-in sound with the synth unplugged, and the synth when connected (check MIDI channel pill, bend/mod/hold behaviour). Note the latency of the built-in sound by ear.
