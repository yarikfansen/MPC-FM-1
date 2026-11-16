#!/usr/bin/env python3
"""Host prototype of the binaural engine (listening test only, not firmware).
Mirrors the planned firmware DSP: uint32 phase accumulators, parabola sine,
LFSR + one-pole noise, crossfeed (delay + low-pass). Writes 16-bit stereo WAVs
at 44.1 kHz to the output folder. Usage: python tools/binaural_proto.py [outdir] [seconds]"""
import math, struct, sys, wave, os

FS = 44100
PRESETS = {  # name: (beat Hz, carrier Hz, noise level 0..1)
    "delta": (2.0, 200.0, 0.30),
    "theta": (6.0, 220.0, 0.25),
    "alpha": (10.0, 250.0, 0.20),
    "beta":  (18.0, 300.0, 0.15),
    "focus": (40.0, 300.0, 0.15),
}

def sine(ph):  # ph uint32 -> -1..1, parabola + one correction (cheap in int)
    x = (ph & 0xFFFFFFFF) / 2**32 * 2.0 - 1.0
    y = 4.0 * x * (1.0 - abs(x))
    y = 0.225 * (y * abs(y) - y) + y
    return y

def render(beat, carrier, noise, secs, fade=2.0):
    n = int(FS * secs)
    inc_l = int(carrier / FS * 2**32)
    inc_r = int((carrier + beat) / FS * 2**32)
    pl = pr = 0
    lfsr = 0xACE1BEEF
    b0 = b1 = b2 = 0.0  # Paul Kellet-style cheap pink filter
    nl = nr = 0.0
    out = []
    for i in range(n):
        g = min(1.0, i / (fade * FS), (n - i) / (fade * FS))  # fade in / out
        pl = (pl + inc_l) & 0xFFFFFFFF; pr = (pr + inc_r) & 0xFFFFFFFF
        tone_l = sine(pl) * 0.30; tone_r = sine(pr) * 0.30
        if noise:  # one noise stream per ear (decorrelated), 22 kHz update
            if i & 1 == 0:
                for ch in (0, 1):
                    lfsr ^= (lfsr << 13) & 0xFFFFFFFF; lfsr ^= lfsr >> 17; lfsr ^= (lfsr << 5) & 0xFFFFFFFF
                    w = (lfsr / 2**31 - 1.0)
                    if ch == 0: b0 = 0.98 * b0 + 0.18 * w; nl = b0
                    else: b1 = 0.98 * b1 + 0.18 * w; nr = b1
            tone_l += nl * noise * 0.5; tone_r += nr * noise * 0.5
        out.append((tone_l * g, tone_r * g))
    return out

def crossfeed(buf, delay=14, amt=0.28, lp=0.18):
    """B1: each ear also hears the other channel, delayed ~0.3 ms and low-passed."""
    dl = [0.0] * delay; dr = [0.0] * delay
    fl = fr = 0.0; res = []
    for i, (l, r) in enumerate(buf):
        k = i % delay
        fl += lp * (dr[k] - fl); fr += lp * (dl[k] - fr)  # delayed other ear, low-passed
        dl[k] = l; dr[k] = r
        norm = 1.0 / (1.0 + amt)
        res.append(((l + amt * fl) * norm, (r + amt * fr) * norm))
    return res

def save(path, buf):
    with wave.open(path, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(FS)
        w.writeframes(b"".join(struct.pack("<hh", int(max(-1, min(1, l)) * 32000), int(max(-1, min(1, r)) * 32000)) for l, r in buf))

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "build/binaural"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    os.makedirs(out, exist_ok=True)
    for name, (beat, car, nz) in PRESETS.items():
        save(os.path.join(out, f"A_{name}_{beat:g}Hz.wav"), render(beat, car, nz, secs))
    # B1 demo: a panned stereo test tone, raw vs crossfeed
    demo = [((math.sin(2*math.pi*330*i/FS) * (0.4 if (i//FS) % 2 == 0 else 0.0)),
             (math.sin(2*math.pi*495*i/FS) * (0.0 if (i//FS) % 2 == 0 else 0.4))) for i in range(int(FS * 6))]
    save(os.path.join(out, "B1_hardpan_raw.wav"), demo)
    save(os.path.join(out, "B1_hardpan_crossfeed.wav"), crossfeed(demo))
    print("done:", os.listdir(out))
