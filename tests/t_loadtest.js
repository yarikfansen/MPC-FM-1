/* Host test for tools/fm1-load.html (the pure part between "CORE BEGIN" and "CORE END").
 * Usage: node tests/t_loadtest.js [path/to/fm1-load.html]
 * 1. Sysex framing against an independent model of app/transfer.c (groove_transfer_poll + command 17/18/19/23),
 *    and the older firmware that answers 23 with an error (ALPHA f9b8f4b): new fields stay empty.
 * 2. STOP, page close and error paths always leave zero sounding notes (note on == note off per channel and note)
 *    and send CC 123 on every channel the run used; nothing is sent after the stop.
 * 3. Per-step deltas / render stats / peak flags from a scripted reply stream (+ command 23 maxima, guard trips).
 * 4. Setup text: a run's text is frozen when the run ends; typing later never changes an ended run.
 */
"use strict";
const fs = require("fs");
const path = require("path");
const file = process.argv[2] || path.join(__dirname, "..", "tools", "fm1-load.html");
const html = fs.readFileSync(file, "utf8");
const a = html.indexOf("// ==== CORE BEGIN ===="), b = html.indexOf("// ==== CORE END ====");
if (a < 0 || b < a) { console.error("FAIL: core markers not found"); process.exit(1); }
const NAMES = ["packStream", "unpackStream", "checksumOf", "buildFrame", "parseFrame", "u32", "guardedFrame", "checkOutgoing",
  "decodeSample", "packPoll", "unpackPoll", "buildScenario", "defaultTracks", "defaultKinds", "scenarioMs", "stepEvents",
  "Runner", "summarizeStep", "rawCsv", "summaryCsv", "LOG_FIELDS", "PLAY_MS", "REST_MS", "PREROLL_MS", "ALLOWED_COMMANDS",
  "setupEditTarget", "STEP_PEAKS"];
const core = new Function('"use strict";\n' + html.slice(a, b) + "\nreturn {" + NAMES.join(",") + "};")();

let failures = 0, checks = 0;
function ok(cond, what) { checks++; if (!cond) { failures++; if (failures <= 25) console.error("FAIL: " + what); } }
function eq(x, y) { return x.length === y.length && x.every((v, i) => v === y[i]); }

/* ---------- deterministic random ---------- */
let seed = 12345;
function rnd() { seed = (seed * 1103515245 + 12345) >>> 0; return seed / 4294967296; }
function rint(n) { return Math.floor(rnd() * n); }
function ru32() { const r = rint(4); return r === 0 ? 0 : r === 1 ? 4294967295 : (rint(65536) * 65536 + rint(65536)) >>> 0; }

/* ---------- 1. model of app/transfer.c ---------- */
function put32(out, v) { for (let i = 0; i < 4; ++i) out.push(Math.floor(v / 2 ** (8 * i)) & 255); }
function modelTransfer(m, st) {
  const n = m.length;
  if (n < 6 || n > 192 || m[0] !== 0xf0 || m[n - 1] !== 0xf7) return null;                 // fm1_app_sysex
  if (!(n >= 8 && m[1] === 0x7d && m[2] === 70 && m[3] === 77 && m[4] === 2)) return null;
  for (let i = 1; i + 1 < n; ++i) if (m[i] >= 128) return null;                             // discard
  if (n < 10 || m[5] >= 64) return null;
  let sum = 0; for (let i = 1; i + 1 < n; ++i) sum += m[i];
  if (sum & 127) return null;
  let bits = 0, value = 0; const data = [];
  for (let i = 8; i + 2 < n; ++i) {
    value |= m[i] << bits; bits += 7;
    if (bits >= 8) { data.push(value & 255); value >>= 8; bits -= 8; }
  }
  if (value || bits >= 7) return null;
  const cmd = m[5], out = []; let r = -1;
  if (cmd === 19 && !data.length) { put32(out, st.blocks); put32(out, st.short_intervals); put32(out, st.overruns); put32(out, st.irq_peak_ticks); r = 0; }
  if (cmd === 18 && !data.length) {
    for (const k of ["render_ticks", "engine_peak_ticks", "midi_ticks", "adapted", "overloads", "voice_capacity", "master_gain", "deadline_ticks"]) put32(out, st[k]);
    r = 0;
  }
  if (cmd === 17 && !data.length) {
    for (const k of ["submitted", "completed", "underruns", "midi_dropped"]) put32(out, st[k]);
    out.push(st.samples_status & 255, st.edit_recording & 255); r = 0;
  }
  if (cmd === 23 && !data.length && !st.old) {                                               // audio_output.c load figures
    put32(out, st.shed_count); out.push(st.shedding & 255); put32(out, st.fx_ticks); put32(out, st.block_ticks);
    put32(out, st.engine_call_peak); put32(out, st.micro_d_peak); put32(out, st.mix_peak); r = 0;
    if (!st.v1) {                                                                            // version 2 tail (audit A1), 35 bytes
      out.push(2, 9, 5);
      for (let u = 0; u < 9; ++u) out.push((u * 37 + 1) & 255, u);
      for (let k = 0; k < 5; ++k) out.push(200 + k, 100 + k);
      put32(out, 0x04030201);
    }
  }
  if (r < 0) r = -7;                                                                          // GROOVE_PROJECT_INVALID
  const response = [r < 0 ? 64 - r : r].concat(r ? [] : out);
  const reply = m.slice(0, 8); reply[5] |= 64;
  value = bits = 0;
  for (const x of response) {
    value |= x << bits; bits += 8;
    while (bits >= 7) { reply.push(value & 127); value >>= 7; bits -= 7; }
  }
  if (bits) reply.push(value & 127);
  sum = 0; for (let i = 1; i < reply.length; ++i) sum += reply[i];
  reply.push((0 - sum) & 127, 0xf7);
  return reply;
}
/* Fields of command 23 draw from a second generator, so the draws of the older checks (and the random STOP /
 * send-error points of part 2) stay exactly as they were before command 23 existed. */
let seed23 = 777;
function ru32b() {
  seed23 = (seed23 * 1103515245 + 12345) >>> 0; const r = seed23 % 4;
  seed23 = (seed23 * 1103515245 + 12345) >>> 0;
  return r === 0 ? 0 : r === 1 ? 4294967295 : seed23 >>> 0;
}
function rint2(n) { seed23 = (seed23 * 1103515245 + 12345) >>> 0; return Math.floor(seed23 / 4294967296 * n); }
const FIELDS23 = ["shed_count", "shedding", "fx_ticks", "block_ticks", "engine_call_peak", "micro_d_peak", "mix_peak", "st23"];
function randomStats() {
  const s = {};
  for (const k of core.LOG_FIELDS) s[k] = FIELDS23.includes(k) ? ru32b() : ru32();
  s.samples_status = rint(256); s.edit_recording = rint(256); s.shedding = ru32b() & 1;
  return s;
}
{
  // hand vectors from tools/fm1-diag.html
  ok(eq(core.buildFrame(19, 5, []), [0xF0,0x7D,0x46,0x4D,0x02,0x13,0x05,0x00,0x56,0xF7]), "vector: request 19 token 5");
  ok(eq(core.buildFrame(17, 16383, []), [0xF0,0x7D,0x46,0x4D,0x02,0x11,0x7F,0x7F,0x5F,0xF7]), "vector: request 17 token 16383");
  const p = core.parseFrame(Uint8Array.from([0xF0,0x7D,0x46,0x4D,0x02,0x53,0x05,0x00,0x00,0x7E,0x07,0x00,0x11,0xF7]));
  ok(p && p.cmd === 0x53 && p.token === 5 && eq(p.data, [0, 255, 1]), "vector: 3-byte reply");
  let rounds = 0;
  for (let i = 0; i < 200; ++i) {
    const st = randomStats(), reps = {};
    for (const cmd of [17, 18, 19, 23]) {
      const ri = cmd === 23 ? rint2 : rint, token = 1 + ri(16383);
      const req = core.guardedFrame(cmd, token);
      const rep = modelTransfer(req, st);
      ok(rep !== null, "model accepts page request " + cmd + "/" + token);
      if (!rep) continue;
      ok(rep.length <= 96, "reply fits the 96-byte cache");
      const q = core.parseFrame(Uint8Array.from(rep));
      ok(q && q.cmd === (cmd | 64) && q.token === token && q.data[0] === 0, "page parses model reply " + cmd + "/" + token);
      if (q) reps[cmd] = { status: q.data[0], d: q.data.slice(1) };
      rounds++;
      // a corrupted chunk must be rejected by the page
      if (rep.length > 10) {
        const bad = rep.slice(); const at = 8 + ri(rep.length - 10); bad[at] = (bad[at] + 1 + ri(126)) & 127;
        ok(core.parseFrame(Uint8Array.from(bad)) === null, "corrupted reply rejected");
      }
      // a corrupted request must be refused by the model (the page never sends one)
      const breq = req.slice(); breq[8] = (breq[8] + 1) & 127;
      ok(modelTransfer(breq, st) === null, "model refuses bad checksum");
    }
    const s = core.decodeSample(1, reps[17], reps[18], reps[19], reps[23]);
    for (const k of core.LOG_FIELDS) if (!/^st\d/.test(k)) ok(s[k] === st[k], "decoded " + k + " = " + st[k] + " got " + s[k]);
    ok(s.st17 === 0 && s.st18 === 0 && s.st19 === 0 && s.st23 === 0, "status fields");
    const back = core.unpackPoll(JSON.parse(JSON.stringify(core.packPoll(s))));
    for (const k of core.LOG_FIELDS) ok(back[k] === s[k], "stored poll keeps " + k);
  }
  ok(rounds === 800, "800 request/reply rounds");
  // hand-made 23 reply: every field at its own offset (status 0, then 25 bytes)
  {
    const d = [0, 3,0,0,0, 1, 0x10,0x27,0,0, 0x63,0x2a,0x0e,0, 0x40,0x0d,0x03,0, 0xa0,0x86,0x01,0, 0xff,0xff,0xff,0xff];
    const rep = core.buildFrame(23 | 64, 9, d), q = core.parseFrame(Uint8Array.from(rep));
    const s = core.decodeSample(1, null, null, null, { status: q.data[0], d: q.data.slice(1) });
    ok(s.shed_count === 3 && s.shedding === 1 && s.fx_ticks === 10000 && s.block_ticks === 928355 &&
       s.engine_call_peak === 200000 && s.micro_d_peak === 100000 && s.mix_peak === 4294967295, "hand 23 reply " + JSON.stringify(s));
    const short = core.decodeSample(1, null, null, null, { status: 0, d: d.slice(1, 25) });
    ok(short.shed_count === undefined && short.mix_peak === undefined, "a short 23 reply is not decoded");
  }
  // older firmware (ALPHA f9b8f4b): 23 -> error status; 17-19 still decoded, the new fields stay empty
  {
    const st = { blocks: 123456, short_intervals: 1, overruns: 2, irq_peak_ticks: 3, render_ticks: 4, engine_peak_ticks: 5, midi_ticks: 6,
                 adapted: 7, overloads: 8, voice_capacity: 59, master_gain: 9, deadline_ticks: 69659, submitted: 10, completed: 11,
                 underruns: 12, midi_dropped: 13, samples_status: 0, edit_recording: 0, old: true };
    const reps = {};
    for (const cmd of [17, 18, 19, 23]) {
      const q = core.parseFrame(Uint8Array.from(modelTransfer(core.guardedFrame(cmd, 100 + cmd), st)));
      reps[cmd] = { status: q.data[0], d: q.data.slice(1) };
    }
    ok(reps[23].status === 71 && reps[23].d.length === 0, "old firmware: 23 answers status 71 (invalid)");
    const s = core.decodeSample(1, reps[17], reps[18], reps[19], reps[23]);
    ok(s.st23 === 71 && s.blocks === st.blocks && s.voice_capacity === st.voice_capacity, "old firmware: 17-19 decoded");
    for (const k of ["shed_count", "shedding", "fx_ticks", "block_ticks", "engine_call_peak", "micro_d_peak", "mix_peak"])
      ok(s[k] === undefined, "old firmware: " + k + " empty");
    const none = core.decodeSample(1, reps[17], reps[18], reps[19], null);
    ok(none.st23 === "" && none.shed_count === undefined, "no reply to 23: empty");
    // a poll stored by the page before 23 existed (fewer columns) reads back with the new fields empty
    const oldPacked = core.packPoll(s).slice(0, 2 + 21);
    const back = core.unpackPoll(oldPacked);
    ok(back.blocks === s.blocks && back.st19 === s.st19 && back.mix_peak === undefined && back.st23 === undefined, "old stored poll");
  }
  for (let c = 0; c < 128; ++c) {
    let threw = false; try { core.guardedFrame(c, 1); } catch (e) { threw = true; }
    ok(threw !== core.ALLOWED_COMMANDS.includes(c), "send guard for command " + c);
  }
  // outgoing guard
  const allowed = [[0x90, 60, 90], [0x9F, 0, 127], [0x80, 60, 0], [0x8F, 127, 0], [0xB0, 123, 0], [0xBF, 123, 0], core.guardedFrame(19, 3)];
  const refused = [[0x90, 60, 0], [0x80, 60, 64], [0xB0, 7, 100], [0xB0, 123, 1], [0xB0, 120, 0], [0xC0, 5], [0xC0, 5, 0], [0xE0, 0, 64],
    [0xA0, 60, 10], [0xD0, 10, 0], [0xF8], [0xFF], [0xFA], [0x90, 128, 90], [0x90, 60, 128], [0x7F, 60, 90],
    core.buildFrame(3, 1, [0, 0, 0, 16]), core.buildFrame(7, 1, [0]), core.buildFrame(17, 1, [0]), [0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]];
  for (const m of allowed) { let t = false; try { core.checkOutgoing(m); } catch (e) { t = true; } ok(!t, "outgoing allowed " + m.join(" ")); }
  for (const m of refused) { let t = false; try { core.checkOutgoing(m); } catch (e) { t = true; } ok(t, "outgoing refused " + m.join(" ")); }
}

/* ---------- 2. STOP / page close / error paths ---------- */
function fakeSynth(opts) {
  const syn = { ons: new Map(), offs: new Map(), cc: new Map(), sent: 0, log: [], failAt: opts.failAt, failFrom: opts.failFrom };
  syn.send = m => {
    core.checkOutgoing(m);                       // every runner message must pass the page guard
    syn.attempt = (syn.attempt || 0) + 1;
    if (syn.failAt !== undefined && syn.attempt === syn.failAt) throw new Error("port error (test)");
    if (syn.failFrom !== undefined && syn.attempt >= syn.failFrom) throw new Error("port gone (test)");
    syn.sent++; syn.log.push(m);
    const st = m[0] & 0xF0, c = m[0] & 15, k = c * 128 + m[1];
    if (st === 0x90) syn.ons.set(k, (syn.ons.get(k) || 0) + 1);
    else if (st === 0x80) syn.offs.set(k, (syn.offs.get(k) || 0) + 1);
    else if (st === 0xB0) syn.cc.set(c, syn.sent);
  };
  syn.unbalanced = () => {
    const bad = [];
    for (const k of new Set([...syn.ons.keys(), ...syn.offs.keys()]))
      if ((syn.ons.get(k) || 0) !== (syn.offs.get(k) || 0)) bad.push("ch" + ((k >> 7) + 1) + " note " + (k & 127) + " on " + (syn.ons.get(k) || 0) + " off " + (syn.offs.get(k) || 0));
    return bad;
  };
  return syn;
}
function poll(t, i) {
  return { t, blocks: 1000 + i * 172, short_intervals: 0, overruns: i >> 4, irq_peak_ticks: 400000 + i, render_ticks: 5000 + (i % 7) * 1000,
           engine_peak_ticks: 30000, adapted: i >> 3, overloads: 0, voice_capacity: 59, deadline_ticks: 69659, underruns: 0, midi_dropped: 0 };
}
function simulate(steps, opts) {
  const syn = fakeSynth(opts);
  const hooks = {};
  let stepCount = 0;
  if (opts.hookThrowAtStep) hooks.onStep = () => { if (++stepCount === opts.hookThrowAtStep) throw new Error("hook (test)"); };
  if (opts.pollThrowAt) hooks.onPoll = s => { if (s.t >= opts.pollThrowAt && s.t < opts.pollThrowAt + 500) throw new Error("poll hook (test)"); };
  let ended = 0; hooks.onEnd = () => { ended++; };
  const r = new core.Runner(steps, syn.send, hooks);
  r.start(0);
  const total = core.scenarioMs(steps);
  let t = 0, i = 0, sentAtStop = -1;
  for (; t <= total + 1000; t += 5) {
    if (t % 500 === 0) r.addPoll(poll(t, i++));
    if (opts.stopAt !== undefined && t >= opts.stopAt && r.running()) { r.stop(t, opts.how || "test"); }
    r.tick(t);
    if (!r.running() && sentAtStop < 0) sentAtStop = syn.sent;
  }
  for (let k = 0; k < 50; ++k) r.tick(t + k * 5);            // ticks after the end send nothing
  return { r, syn, sentAtStop, ended };
}
function checkClean(res, what) {
  const { r, syn } = res;
  const bad = syn.unbalanced();
  ok(bad.length === 0, what + ": sounding notes left: " + bad.slice(0, 3).join("; "));
  ok(r.ledger.count() === 0, what + ": ledger empty");
  for (const c of r.usedChannels) {
    ok(syn.cc.has(c), what + ": CC 123 on channel " + (c + 1));
    const lastNote = syn.log.reduce((acc, m, i) => ((m[0] & 0xE0) === 0x80 && (m[0] & 15) === c ? i + 1 : acc), 0);
    ok(!syn.cc.has(c) || syn.cc.get(c) >= lastNote, what + ": CC 123 after the last note on channel " + (c + 1));
  }
  ok(!r.running(), what + ": runner stopped");
  ok(res.sentAtStop === syn.sent, what + ": nothing sent after the stop");
  ok(res.ended === 1, what + ": onEnd once (got " + res.ended + ")");
}
{
  const full = core.buildScenario(core.defaultTracks(), core.defaultKinds());
  ok(full.length === 8 * 4 + 3, "default scenario has 35 steps");
  ok(core.scenarioMs(full) === 3000 + 35 * 11000, "default total time");
  ok(full[31].targets[0].ch === 10 && full[31].targets[0].drums, "track 8 = channel 10, drums");
  ok(full[34].targets.length === 8, "all-tracks step has 8 channels");
  // every step's own events are balanced and end before the step ends
  for (const s of full) {
    const ev = core.stepEvents(s, 0);
    const bal = new Map();
    for (const e of ev) { const k = (e.m[0] & 15) * 128 + e.m[1]; bal.set(k, (bal.get(k) || 0) + ((e.m[0] & 0xF0) === 0x90 ? 1 : -1)); }
    ok([...bal.values()].every(v => v === 0), "events balanced: " + s.name);
    ok(ev.every(e => e.t < core.PLAY_MS + core.REST_MS), "events inside the step: " + s.name);
  }
  // completed run
  checkClean(simulate(full, {}), "full run");
  // STOP / page close at many moments (small scenario keeps the test quick, plus some points in the full one)
  const small = core.buildScenario(core.defaultTracks().map((t, i) => Object.assign(t, { on: i === 0 || i === 7 })), core.defaultKinds());
  const smallTotal = core.scenarioMs(small);
  const points = [0, 5, 2995, 3000, 3005, 3047, 3094, 7000, 10995, 11000, 11005, 13995, 14000];
  for (let k = 0; k < 120; ++k) points.push(rint(smallTotal));
  for (const p of points) checkClean(simulate(small, { stopAt: p, how: "STOP" }), "STOP at " + p);
  for (let k = 0; k < 6; ++k) checkClean(simulate(full, { stopAt: rint(core.scenarioMs(full)), how: "page closed" }), "page close (full) #" + k);
  // one send fails (port error) at many message numbers
  for (let k = 0; k < 60; ++k) {
    const failAt = 1 + rint(1500);
    const res = simulate(small, { failAt });
    checkClean(res, "send error at message " + failAt);
    ok(/ошибка/.test(res.r.endHow) || res.r.endHow === "закончен полностью", "error recorded in endHow (" + res.r.endHow + ")");
  }
  // a hook throws (page code error) -> stop
  for (const s of [1, 3, 7]) checkClean(simulate(small, { hookThrowAtStep: s }), "onStep hook error at step " + s);
  checkClean(simulate(small, { pollThrowAt: 9000 }), "onPoll hook error");
  // late page (frozen tab) -> stop, not a burst of notes
  {
    const arpOnly = core.buildScenario(core.defaultTracks(), { arp: true });
    const syn = fakeSynth({}); const r = new core.Runner(arpOnly, syn.send, {});
    r.start(0); r.addPoll(poll(0, 0)); r.tick(3000); r.tick(4000); r.tick(5000);
    ok(syn.ons.size === 1, "late tick: only the first arp note was played");
    ok(r.state === "stopped" && /опоздала/.test(r.endHow), "late tick stops the run");
    ok(syn.unbalanced().length === 0, "late tick: no sounding notes");
  }
  // no reply in the preroll -> no notes at all
  {
    const syn = fakeSynth({}); const r = new core.Runner(small, syn.send, {});
    r.start(0); for (let t = 0; t <= 3000; t += 5) r.tick(t);
    ok(r.state === "stopped" && syn.ons.size === 0, "no reply: stopped without a note");
  }
  // port gone: offs cannot be sent; the page must report it
  {
    const res = simulate(small, { failFrom: 40 });
    ok(res.r.failures > 0 && !res.r.running(), "port gone: failures reported (" + res.r.failures + ")");
    let sounding = 0; for (const [k, v] of res.syn.ons) sounding += v - (res.syn.offs.get(k) || 0);
    ok(res.r.ledger.count() === sounding, "port gone: ledger matches what the synth still holds");
  }
}

/* ---------- 3b. command 23 in the step summary ---------- */
{
  const B = 1000000, step = { idx: 1, name: "x", targets: [{ ch: 1 }] };
  const Q = (t, o) => Object.assign({ t, blocks: t, block_ticks: B }, o);
  const polls = [
    Q(1000, { shed_count: 4, engine_call_peak: 900000, micro_d_peak: 900000, mix_peak: 900000 }),   // before the step: not counted
    Q(3000, { shed_count: 4, engine_call_peak: 100000, micro_d_peak: 50000, mix_peak: 200000 }),    // = step start: not counted
    Q(3500, { shed_count: 4, engine_call_peak: 300000, micro_d_peak: 60000, mix_peak: 250000 }),
    Q(4000, { shed_count: 5, engine_call_peak: 450000, micro_d_peak: 40000, mix_peak: 300000 }),
    Q(9000, { shed_count: 7, engine_call_peak: 200000, micro_d_peak: 120000, mix_peak: 450000 }),
    Q(13500, { shed_count: 7, engine_call_peak: 10000, micro_d_peak: 130000, mix_peak: 20000 }),     // in the pause: counted
    Q(14500, { shed_count: 9, engine_call_peak: 990000, micro_d_peak: 990000, mix_peak: 990000 }),  // after the step
  ];
  const r = core.summarizeStep(polls, step, 3000, 11000, 14000);
  ok(r.shed_count === 3, "guard trips in the step: 3, got " + r.shed_count);
  ok(r.engine_call_peak_max === 450000 && r.micro_d_peak_max === 130000 && r.mix_peak_max === 450000,
     "step maxima " + [r.engine_call_peak_max, r.micro_d_peak_max, r.mix_peak_max]);
  ok(r.engine_call_peak_max_pct === 45 && r.micro_d_peak_max_pct === 13 && r.mix_peak_max_pct === 45 && r.block_ticks === B, "step maxima in %");
  const sum = core.summaryCsv([{ id: 1, started: 0, setup: "", tracks: [], steps: [r], raw: [], end: null }]).split("\n");
  const head = sum[0].split(","), row = sum.find(l => /^1,1,/.test(l)).split(",");
  for (const k of ["shed_count", "engine_call_peak_max", "engine_call_peak_max_pct", "micro_d_peak_max", "micro_d_peak_max_pct", "mix_peak_max", "mix_peak_max_pct"])
    ok(head.includes(k) && row[head.indexOf(k)] === String(r[k]), "summary CSV column " + k);
  const raw = core.rawCsv([{ id: 1, started: 0, setup: "", tracks: [], steps: [], raw: polls.map(core.packPoll), end: null }]).split("\n");
  const rh = raw[0].split(",");
  for (const k of ["shed_count", "shedding", "fx_ticks", "block_ticks", "engine_call_peak", "micro_d_peak", "mix_peak", "st23"])
    ok(rh.includes(k), "raw CSV column " + k);
  ok(raw[6].split(",")[rh.indexOf("mix_peak")] === "250000", "raw CSV mix_peak value");
  // a synth restart inside the step: no trip delta
  const rs = core.summarizeStep(polls.map((p, i) => Object.assign({}, p, { blocks: i >= 4 ? i : p.blocks })), step, 3000, 11000, 14000);
  ok(rs.shed_count === "", "restart: no trip delta");
}

/* ---------- 4. setup text of a run is frozen when the run ends ---------- */
{
  ok(core.setupEditTarget([]) === null, "no run: typing changes no run");
  const ended = { id: 1, setup: "old layout", end: { t: 1, how: "x" } };
  ok(core.setupEditTarget([ended]) === null, "ended run: typing does not change it");
  const going = { id: 2, setup: "new", end: null };
  ok(core.setupEditTarget([ended, going]) === going, "a run in progress follows the text");
  // the page glue uses it, and the end hook writes the text before marking the end
  const glue = html.slice(b);
  const h0 = glue.indexOf('$("setup").oninput = () => {'), handler = h0 < 0 ? "" : glue.slice(h0, glue.indexOf("\n};", h0));
  ok(/setupEditTarget\(runs\)/.test(handler), "setup oninput goes through setupEditTarget");
  ok(!/runs\[/.test(handler) && !/\blast\b/.test(handler), "setup oninput does not touch the last run directly");
  ok(/onEnd: r => \{\s*run\.setup = \$\("setup"\)\.value;[^\n]*\n\s*run\.end = /.test(glue), "end hook: setup snapshot, then end");
}

/* ---------- 3. per-step deltas from a scripted reply stream ---------- */
{
  const tracks = core.defaultTracks().map((t, i) => Object.assign(t, { on: i === 0 }));
  const steps = core.buildScenario(tracks, { one: true, chord4: true });
  ok(steps.length === 2, "two steps");
  const D = 69659;
  const P = (t, o) => Object.assign({ t, deadline_ticks: D, short_intervals: 0, overloads: 0, midi_dropped: 0 }, o);
  const stream = [
    P(1000,  { blocks: 100, overruns: 5, adapted: 10, irq_peak_ticks: 500000, engine_peak_ticks: 30000, render_ticks: 3000, underruns: 0, voice_capacity: 59 }),
    P(2500,  { blocks: 200, overruns: 5, adapted: 10, irq_peak_ticks: 500000, engine_peak_ticks: 30000, render_ticks: 3500, underruns: 0, voice_capacity: 59 }),
    P(3000,  { blocks: 300, overruns: 6, adapted: 11, irq_peak_ticks: 500000, engine_peak_ticks: 30000, render_ticks: 3400, underruns: 1, voice_capacity: 59 }),
    P(5000,  { blocks: 400, overruns: 6, adapted: 15, irq_peak_ticks: 600000, engine_peak_ticks: 30000, render_ticks: 20000, underruns: 1, voice_capacity: 53 }),
    P(8000,  { blocks: 500, overruns: 8, adapted: 20, irq_peak_ticks: 700000, engine_peak_ticks: 31000, render_ticks: 30000, underruns: 1, voice_capacity: 50 }),
    P(11000, { blocks: 600, overruns: 9, adapted: 20, irq_peak_ticks: 700000, engine_peak_ticks: 31000, render_ticks: 10000, underruns: 1, voice_capacity: 59 }),
    { t: 13000, blocks: 700, overruns: 9, short_intervals: 0, irq_peak_ticks: 700000 },          // 17 and 18 timed out
    P(14000, { blocks: 800, overruns: 9, adapted: 21, irq_peak_ticks: 700000, engine_peak_ticks: 31000, render_ticks: 2000, underruns: 3, voice_capacity: 59 }),
    P(17000, { blocks: 900, overruns: 9, adapted: 21, irq_peak_ticks: 700000, engine_peak_ticks: 31000, render_ticks: 5000, underruns: 3, voice_capacity: 59 }),
    P(20000, { blocks: 50,  overruns: 0, adapted: 0,  irq_peak_ticks: 300000, engine_peak_ticks: 9000,  render_ticks: 4000, underruns: 0, voice_capacity: 59 }),
    P(25000, { blocks: 150, overruns: 0, adapted: 0,  irq_peak_ticks: 300000, engine_peak_ticks: 9000,  render_ticks: 1000, underruns: 0, voice_capacity: 59 }),
  ];
  const syn = fakeSynth({}); const got = [];
  const r = new core.Runner(steps, syn.send, { onStep: s => got.push(s) });
  r.start(0);
  let k = 0;
  for (let t = 0; t <= 26000; t += 5) {
    while (k < stream.length && stream[k].t <= t) r.addPoll(Object.assign({}, stream[k++]));
    r.tick(t);
  }
  ok(got.length === 2, "two summaries");
  const s1 = got[0] || {}, s2 = got[1] || {};
  ok(s1.start === 3000 && s1.end === 14000 && s1.play_ms === 8000, "step 1 window " + s1.start + ".." + s1.end);
  ok(s1.overruns === 3, "step 1 overruns delta 3, got " + s1.overruns);
  ok(s1.adapted === 10, "step 1 adapted delta 10, got " + s1.adapted);
  ok(s1.underruns === 2, "step 1 underruns delta 2, got " + s1.underruns);
  ok(s1.short_intervals === 0 && s1.overloads === 0 && s1.midi_dropped === 0, "step 1 zero deltas");
  ok(s1.render_min === 10000 && s1.render_max === 30000 && s1.render_mean === 20000, "step 1 render min/max/mean " + [s1.render_min, s1.render_mean, s1.render_max]);
  ok(s1.render_mean_pct === 28.7 && s1.render_max_pct === 43.1, "step 1 render % " + s1.render_mean_pct + " / " + s1.render_max_pct);
  ok(s1.irq_peak_rose === "yes" && s1.irq_peak_before === 500000 && s1.irq_peak_after === 700000 && s1.irq_peak_after_pct === 75.4, "step 1 irq peak rose to 700000 (75.4 %)");
  ok(s1.engine_peak_rose === "yes" && s1.engine_peak_after === 31000, "step 1 engine peak rose");
  ok(s1.voice_capacity_min === 50 && s1.polls === 3 && s1.note === "", "step 1 capacity / polls / note");
  ok(s2.overruns === "" && s2.adapted === "" && s2.irq_peak_rose === "" && /перезапуст/.test(s2.note), "step 2 restart detected: " + s2.note);
  ok(s2.render_min === 4000 && s2.render_max === 5000, "step 2 render over play polls only");
  ok(s1.shed_count === "" && s1.engine_call_peak_max === "" && s1.mix_peak_max_pct === "" && s1.block_ticks === "",
     "no command 23 in the stream (old firmware): empty columns");
  // the raw log labels
  const labels = r.polls.map(p => p.t + ":" + p.step).join(" ");
  ok(/3000:до начала/.test(labels) && /5000:1 / .test(labels + " ") && /13000:1 пауза/.test(labels) && /17000:2 /.test(labels + " ") && /25000:2 пауза/.test(labels), "step labels: " + labels);
  // a peak that did not rise
  const s3 = core.summarizeStep(stream.slice(0, 8), { idx: 9, name: "x", targets: [{ ch: 1 }] }, 11000, 13000, 14000);
  ok(s3.irq_peak_rose === "no" && s3.engine_peak_rose === "no" && s3.overruns === 0, "flat peaks reported as not risen");
  // stopped in the middle of step 1
  const syn2 = fakeSynth({}); const got2 = [];
  const r2 = new core.Runner(steps, syn2.send, { onStep: s => got2.push(s) });
  r2.start(0); k = 0;
  for (let t = 0; t <= 9000; t += 5) { while (k < stream.length && stream[k].t <= t) r2.addPoll(Object.assign({}, stream[k++])); r2.tick(t); }
  r2.stop(9000, "нажата кнопка СТОП");
  ok(got2.length === 1 && got2[0].overruns === 2 && got2[0].play_ms === 6000 && /остановлен/.test(got2[0].note), "stopped step summary " + JSON.stringify(got2[0]));
  // CSV: same columns as the diag page + run + step; setup text in both files
  const run = { id: 7, started: 0, setup: "1 DX7, reverb on\n8 808", tracks, steps: got, raw: r.polls.map(core.packPoll), end: { t: 26000, how: "x" } };
  const raw = core.rawCsv([run]).split("\n"), sum = core.summaryCsv([run]).split("\n");
  ok(raw[0] === "time,unix_ms," + core.LOG_FIELDS.join(",") + ",run,step", "raw CSV header");
  ok(raw.some(l => l.startsWith("# setup: 1 DX7, reverb on / 8 808")) && sum.some(l => l.startsWith("# setup: 1 DX7, reverb on / 8 808")), "setup in both files");
  ok(raw.filter(l => /^\d/.test(l)).length === stream.length, "raw CSV rows");
  ok(sum.filter(l => /^7,/.test(l)).length === 2, "summary CSV rows");
  ok(raw.filter(l => /^\d/.test(l)).every(l => l.split(",").length === 2 + core.LOG_FIELDS.length + 2), "raw CSV column count");
}

if (failures) { console.error("t_loadtest: " + failures + " of " + checks + " checks FAILED"); process.exit(1); }
console.log("t_loadtest: all " + checks + " checks passed");
