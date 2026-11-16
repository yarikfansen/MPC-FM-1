"""Host check for app/micro_d.c (DLL built by tests/run_micro_d.sh).
Renders every preset (a riff, then a chord), writes WAVs next to the DLL and
checks: output finite and below full scale, sound present while notes are
held, silence after the release, the PSG host muted, a rough CPU figure.
With a second DLL (the reference build of the code before the W4 CPU diet)
it first compares new and old: per preset held notes C2 / C3 / C5, a 4-note
chord and a staccato line (2 s each: RMS within 0.5 dB, octave bands
62 Hz..16 kHz within 1 dB where above -50 dB of the total, peak within 1 dB),
KEY mode (the next note after 3 s of silence matches, the new code idles
within 50 ms after the sound has died) and the operation counts per sample.
With a third DLL (the code just before the 2-voice limit, MD_PREV) it checks
the limit (user 10.11): 1 and 2 notes render bit-identical to before in
modes 1-4, a 3rd held note takes the oldest voice, never more than 2 voices
sound.
`t_micro_d.py --make-ref src dst` writes the instrumented reference copy."""
import ctypes, math, os, struct, sys, wave

def make_ref(src, dst):
    """The old micro_d.c with the same test-only counters as the new one."""
    s = open(src, encoding="utf-8").read()
    subs = [
        ("#include <string.h>\n", "#include <string.h>\nunsigned md_test_tanh, md_test_div, md_test_sat, md_test_busy; /* t_micro_d.py */\n"),
        ("float ftanh(float x)\n{\n", "float ftanh(float x)\n{\n    ++md_test_tanh;\n"),
        ("    if (x < -3.0f) return -1.0f;\n    float x2 = x * x;\n", "    if (x < -3.0f) return -1.0f;\n    ++md_test_div;\n    float x2 = x * x;\n"),
        ("if (t < dt) { t /= dt;", "if (t < dt) { ++md_test_div; t /= dt;"),
        ("if (t > 1.0f - dt) { t = (t - 1.0f) / dt;", "if (t > 1.0f - dt) { ++md_test_div; t = (t - 1.0f) / dt;"),
        ("    if (!any) { d->meter *= 0.8f; return; }\n", "    if (!any) { d->meter *= 0.8f; return; }\n    ++md_test_busy;\n"),
        # today's voice limit (user 10.11: 2 voices) in the reference too, so the W4 check compares like with like
        ("VOICES = 4,", "VOICES = 2,"),
        ("mode == MD_UNISON ? VOICES : mode + 1U;", "mode == MD_UNISON || mode + 1U > VOICES ? VOICES : mode + 1U;"),
        ("{-0.12f, -0.04f, 0.04f, 0.12f};", "{-0.08f, 0.08f};"),
    ]
    for old, new in subs:
        if s.count(old) != 1: sys.exit(f"reference: pattern not found once: {old!r} (MD_REF must be the code before W4)")
        s = s.replace(old, new)
    open(dst, "w", encoding="utf-8").write(s)

if sys.argv[1] == "--make-ref":
    make_ref(sys.argv[2], sys.argv[3]); sys.exit(0)

dll = ctypes.CDLL(os.path.abspath(sys.argv[1]))
ref = ctypes.CDLL(os.path.abspath(sys.argv[2])) if len(sys.argv) > 2 else None
prev = ctypes.CDLL(os.path.abspath(sys.argv[3])) if len(sys.argv) > 3 else None
out_dir = os.path.dirname(sys.argv[1])
FR, FS = 128, 44100
buf = (ctypes.c_int32 * (FR * 2))()
for d in (dll, ref):
    if d is None: continue
    d.md_name.restype = ctypes.c_char_p
    d.md_cycles.restype = ctypes.c_uint64
T = 2
fails = 0
assert dll.md_attach(T) == 0
assert dll.md_host_volume(T) == 0, "PSG host not muted"

# ---- 2-voice limit (user 10.11) against the code just before it ----
if prev is not None:
    V = 6 # track: slot 1 in both builds
    prev.md_name.restype = ctypes.c_char_p
    notes4 = (ctypes.c_uint8 * 4)()
    def voices(d):
        m = d.md_voices(V, notes4)
        return m, sorted(notes4[i] for i in range(4) if m >> i & 1)
    def run(d, events, blocks):
        """events: (block, note, velocity); the left channel"""
        out = []
        for b in range(blocks):
            for when, n, v in events:
                if when == b: d.md_note(V, n, v)
            d.md_render(buf); out.extend(buf[0::2])
        return out
    mirror = [d for d in (dll, prev, ref) if d is not None]
    prev.md_attach(T) # T holds slot 0 in every build: V gets slot 1 (its own noise seed) everywhere
    if ref is not None: ref.md_attach(T)
    for d in mirror: d.md_attach(V)
    same = 0
    one = [(0, 48, 100), (150, 48, 0)]
    two = [(0, 48, 100), (40, 55, 90), (150, 48, 0), (170, 55, 0)]
    for p in range(dll.md_presets()):
        for mode in (0, 1, 2, 3):
            for ev in ((one,) if mode == 0 else (one, two)):
                for d in mirror: # from a clean voice state (no tail of the run before)
                    d.md_detach(V); d.md_attach(V); d.md_preset(V, p); d.md_set(V, 19, mode)
                a = run(dll, ev, 400); b = run(prev, ev, 400)
                if ref is not None: run(ref, ev, 400) # keeps the W4 reference's slot history (noise, LFO) in step
                if a != b:
                    print(f"2 voices: preset {p} mode {mode} {len(ev) // 2} note(s) differ from before FAIL"); fails += 1
                else: same += 1
    print(f"2 voices: {same} renders of 1 / 2 notes bit-identical to before")
    # a 3rd held note takes the oldest voice (modes 2-4, which had up to 4 voices); never more than 2 sound (also ALL 1)
    for mode in (1, 2, 3, 4):
        for d in mirror: d.md_detach(V); d.md_attach(V); d.md_preset(V, 10); d.md_set(V, 19, mode)
        ev = [(0, 48, 100), (5, 52, 100), (10, 55, 100), (15, 59, 100), (20, 62, 100)] + [(60, n, 0) for n in (48, 52, 55, 59, 62)]
        held, most = [], 0
        for b in range(400):
            for d in mirror:
                for when, n, v in ev:
                    if when == b: d.md_note(V, n, v)
                d.md_render(buf)
            m, ns = voices(dll)
            most = max(most, bin(m).count("1"))
            if b in (12, 17, 22): held.append(ns)
        want = [[52, 55], [55, 59], [59, 62]] if mode != 4 else [[55, 55], [59, 59], [62, 62]]
        ok = most <= 2 and held == want
        print(f"2 voices: mode {mode} most voices {most}, held {held} {'ok' if ok else 'FAIL'}"); fails += not ok
    for d in mirror: d.md_detach(V)

# ---- new against old (reference build) ----
C = 5 # comparison track: slot 1 in both builds (T holds slot 0)

def play_blocks(d, events, seconds):
    """events (time s, note, velocity), sorted; returns the left channel and per-block busy flags"""
    blocks = int(math.ceil(seconds * FS / FR))
    left = (ctypes.c_int32 * (blocks * FR))(); busy = (ctypes.c_uint8 * blocks)()
    pos = 0
    def run(to):
        nonlocal pos
        if to > pos: d.md_play(ctypes.byref(left, pos * FR * 4), to - pos, ctypes.byref(busy, pos)); pos = to
    for when, note, vel in events:
        run(min(blocks, int(math.ceil(when * FS / FR - 1e-9))))
        d.md_note(C, note, vel)
    run(blocks)
    return left, busy

def analyse(d, left, start, n):
    r = (ctypes.c_double * 11)()
    d.md_analyse(ctypes.byref(left, start * 4), n, r)
    return list(r)

def db(a, b, k=10):
    if a <= 0 and b <= 0: return 0.0
    if a <= 0 or b <= 0: return float("inf")
    return k * math.log10(a / b)

worst = {"rms": 0.0, "band": 0.0, "peak": 0.0}
def compare(a, b):
    """a new, b reference analyses: (rms dB, worst band dB, peak dB)"""
    rms = db(a[0], b[0]); pk = db(a[1], b[1], 20); band = 0.0
    for i in range(8):
        if b[2 + i] > b[10] * 1e-5 or a[2 + i] > a[10] * 1e-5:
            x = db(a[2 + i], b[2 + i])
            if abs(x) > abs(band): band = x
    for key, v in (("rms", rms), ("band", band), ("peak", pk)):
        if abs(v) > abs(worst[key]): worst[key] = v
    ok = abs(rms) <= 0.5 and abs(band) <= 1.0 and abs(pk) <= 1.0
    if not ok: print("   bands dB re total, new / old: " + " ".join(f"{db(a[2 + i], a[10]):.0f}/{db(b[2 + i], b[10]):.0f}" for i in range(8)))
    return rms, band, pk, ok

def ops(d):
    o = (ctypes.c_uint * 4)(); d.md_ops(o); return list(o)

if ref:
    assert ref.md_attach(T) == 0
    for d in (dll, ref): assert d.md_attach(C) == 0
    stacc = []
    for i, n in enumerate((36, 48, 43, 39, 55, 60, 51, 46)): stacc += [(0.25 * i, n, 105), (0.25 * i + 0.12, n, 0)]
    cases = [("C2", [(0, 36, 100), (2.0, 36, 0)]), ("C3", [(0, 48, 100), (2.0, 48, 0)]), ("C5", [(0, 72, 100), (2.0, 72, 0)]),
             ("chord", [(0, n, 100) for n in (48, 52, 55, 59)] + [(2.0, n, 0) for n in (48, 52, 55, 59)]), ("staccato", stacc)]
    print("new against old (dB new - old: RMS / worst band / peak)")
    for p in range(dll.md_presets()):
        name = dll.md_name(p).decode()
        for d in (dll, ref): d.md_preset(C, p)
        line = []
        for cname, ev in cases:
            res = []
            for d in (dll, ref):
                left, _ = play_blocks(d, ev, 2.6) # 2 s compared, then 0.6 s of release
                res.append(analyse(d, left, 0, 2 * FS))
            rms, band, pk, ok = compare(res[0], res[1])
            fails += not ok
            line.append(f"{cname} {rms:+.2f}/{band:+.2f}/{pk:+.2f}{'' if ok else ' FAIL'}")
        print(f"{p:2} {name:10} " + "  ".join(line))
    # KEY mode: note, release, 3 s of silence, the same note again
    print("KEY mode: second note after 3 s (dB), idle after release (new / old busy ms after the sound died)")
    rel, on2 = int(math.ceil(0.5 * FS / FR)), int(math.ceil(3.5 * FS / FR))
    for p in range(dll.md_presets()):
        name = dll.md_name(p).decode()
        runs = []
        for d in (dll, ref):
            d.md_preset(C, p); d.md_set(C, 7, 1)
            runs.append(play_blocks(d, [(0, 48, 100), (0.5, 48, 0), (3.5, 48, 100), (3.7, 48, 0)], 4.5))
        (nl, nb), (rl, rb) = runs
        rms, band, pk, ok = compare(analyse(dll, nl, on2 * FR, FS // 10), analyse(ref, rl, on2 * FR, FS // 10))
        died = rel * FR # first sample after the release from which the old output stays 0
        for i in range(on2 * FR - 1, rel * FR - 1, -1):
            if rl[i]: died = i + 1; break
        limit = int(math.ceil((died + 0.05 * FS) / FR))
        new_busy = sum(nb[limit:on2]); old_busy = sum(rb[limit:on2])
        idle = rel # first block from which the new code stays idle until the second note
        for b in range(on2 - 1, rel - 1, -1):
            if nb[b]: idle = b + 1; break
        # what the idle code still plays (the DC blocker's tail) against the old output, after it went idle
        cut = max((abs(nl[i] - rl[i]) for i in range(idle * FR, on2 * FR)), default=0)
        tail = max((abs(x) for x in rl[idle * FR:on2 * FR]), default=0)
        idle_ok = new_busy == 0 and cut <= 64
        fails += not ok or not idle_ok
        print(f"{p:2} {name:10} {rms:+.2f}/{band:+.2f}/{pk:+.2f}{'' if ok else ' FAIL'}  full render ends {(idle - rel) * FR * 1000 // FS} ms "
              f"after release, sound dies {(died - rel * FR) * 1000 // FS} ms; busy after died+50ms: new {new_busy * FR * 1000 // FS} / "
              f"old {old_busy * FR * 1000 // FS} ms; after that new-old {cut} LSB (old {tail}){'' if idle_ok else ' FAIL'}")
    # operation counts per rendered sample, default preset, PARA 4, THICK 14 (two oscillators per note)
    print("operations per sample (default preset): ftanh calls / float divisions on the sample path / inner saturations")
    cyc = {}
    for label, d in (("old", ref), ("new", dll)):
        d.md_preset(C, 0)
        row = []
        for notes in ((48,), (48, 52, 55, 59)):
            for n in notes: d.md_note(C, n, 100)
            play_blocks(d, [], 20 * FR / FS)
            a = ops(d); play_blocks(d, [], 200 * FR / FS); b = ops(d)
            per = [(b[i] - a[i]) / (200 * FR) for i in range(3)]
            row.append(f"{len(notes)} note(s): {per[0]:.2f} / {per[1]:.2f} / {per[2]:.2f}")
            if len(notes) == 4:
                big = (ctypes.c_int32 * (FR * 2))()
                cyc[label] = d.md_cycles(400, big) / (400 * FR)
            for n in notes: d.md_note(C, n, 0)
            play_blocks(d, [], 6.0)
        print(f"  {label}: " + "   ".join(row))
    print("  plus block-level divisions (by reading, default preset): old 14 per block + 2 per sub-block + 1 per voice "
          "per sub-block (1 note 0.30, 4 notes 0.48 per sample); new 13 per block + 1 per sub-block (0.16 per sample)")
    print(f"  x86 host cycles per frame, 4 notes: old {cyc['old']:.0f}, new {cyc['new']:.0f} ({100 * (1 - cyc['new'] / cyc['old']):.0f} % less; the device differs)")
    print(f"largest differences: RMS {worst['rms']:+.2f} dB, band {worst['band']:+.2f} dB, peak {worst['peak']:+.2f} dB")
    for d in (dll, ref): d.md_detach(C)

def render(seconds):
    out = []
    for _ in range(int(seconds * FS / FR)):
        dll.md_render(buf)
        out.extend(buf[0::2])
    return out

def play(events, total):
    """events: (time s, note, velocity) sorted"""
    out = []
    for when, note, vel in events + [(total, None, None)]:
        while len(out) < when * FS: # whole blocks, counted in rendered samples
            dll.md_render(buf); out.extend(buf[0::2])
        if note is not None:
            dll.md_note(T, note, vel)
    return out

riff = [(0.00, 36, 110), (0.20, 36, 0), (0.25, 48, 100), (0.45, 48, 0), (0.50, 43, 120), (0.70, 43, 0),
        (0.75, 39, 90), (1.10, 39, 0)]
chord = [(1.5, 48, 100), (1.5, 52, 100), (1.5, 55, 100), (2.5, 52, 0), (2.6, 48, 0), (2.6, 55, 0)]
for p in range(dll.md_presets()):
    name = dll.md_name(p).decode()
    dll.md_preset(T, p)
    s = play(riff + chord, 9.0)
    peak = max(abs(x) for x in s) / 8388608.0
    held = s[int(1.5 * FS):int(1.7 * FS)] # chord onset (presets with SUSTAIN 0 fade later)
    rms = max(abs(x) for x in held) / 8388608.0
    riff_peak = max(abs(x) for x in s[:int(1.2 * FS)]) / 8388608.0
    tail = max(abs(x) for x in s[int(8.7 * FS):]) / 8388608.0
    ok = peak < 0.98 and riff_peak > 0.05 and rms > 0.05 and tail < 1e-3
    fails += not ok
    print(f"{p:2} {name:10} peak {peak:5.2f}  riff {riff_peak:4.2f}  chord peak {rms:5.3f}  tail {tail:.5f}  {'ok' if ok else 'FAIL'}")
    with wave.open(os.path.join(out_dir, f"micro_d_{p:02}_{name.replace(' ', '_')}.wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS)
        w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, x >> 8))) for x in s))
# CPU: PARA 4 with detune, four notes held
dll.md_preset(T, 3); dll.md_set(T, 4, 40); dll.md_set(T, 19, 3)
for n in (48, 52, 55, 59): dll.md_note(T, n, 100)
cyc = dll.md_cycles(200, buf) / (200 * FR)
print(f"cost: {cyc:.0f} x86 cycles per frame (PARA 4, detune on)")
for n in (48, 52, 55, 59): dll.md_note(T, n, 0)
# save / load: the extras record carries the MICRO D tracks
dll.md_preset(T, 5); dll.md_set(T, 8, 77)
st = (ctypes.c_uint8 * 46)(); dll.md_state_get(st)
saved = bytes(st)
dll.md_detach(T)
if dll.md_owns(T) or dll.md_host_volume(T) == 0: print("detach failed"); fails += 1
blank = (ctypes.c_uint8 * 46)(*([0xff] * 46)); dll.md_state_put(blank)
if dll.md_owns(T): print("0xff record still owns a track"); fails += 1
dll.md_state_put((ctypes.c_uint8 * 46)(*saved))
if not dll.md_owns(T) or dll.md_value(T, 8) != 77: print("state round trip failed"); fails += 1
else: print("state round trip ok")
# Mixer pan (user 09.11: plain channel switch): 2 = left only, 1 = right only, 0 = both.
if not dll.md_owns(T): dll.md_attach(T)
dll.md_preset(T, 0)
for side, quiet in ((0, 1), (127, 0), (32, 2), (96, 3), (64, None)):
    dll.md_pan(T, side); dll.md_note(T, 57, 100)
    l = r = 0
    for _ in range(40):
        dll.md_render(buf)
        l = max(l, max(abs(x) for x in buf[0::2])); r = max(r, max(abs(x) for x in buf[1::2]))
    dll.md_note(T, 57, 0); render(1.0)
    if quiet is None: ok = l > 1000 and r > 1000 and abs(l - r) <= 1
    elif quiet >= 2: # half way to a side: the far channel is at about half level
        near, far = (l, r) if quiet == 2 else (r, l)
        ok = near > 1000 and 0.45 * near < far < 0.56 * near
    else: ok = (r if quiet else l) == 0 and (l if quiet else r) > 1000
    print(f"pan side {side}: L peak {l} R peak {r} {'ok' if ok else 'FAIL'}"); fails += not ok
dll.md_pan(T, 64)
print("micro d: all checks passed" if not fails else f"micro d: {fails} preset(s) failed")
sys.exit(1 if fails else 0)
