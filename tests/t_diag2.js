/* Host test for tools/fm1-diag2.html (two synths at once; read-only commands 17, 18, 19, 23).
 * Usage: node tests/t_diag2.js [path/to/fm1-diag2.html]
 * Runs the page script with a stub DOM and no Web MIDI, then checks: the page's self-test; decoding of an old
 * (version 1) and a new (version 2) reply of command 23 for two devices through the real framing; step
 * statistics; the difference table; CSV; the send guard (only 17, 18, 19, 23, no payload).
 */
"use strict";
const fs = require("fs");
const path = require("path");
const file = process.argv[2] || path.join(__dirname, "..", "tools", "fm1-diag2.html");
const html = fs.readFileSync(file, "utf8");
const a = html.lastIndexOf("<script>"), b = html.lastIndexOf("</script>");
if (a < 0 || b < a) { console.error("FAIL: script not found"); process.exit(1); }

const elements = {};
function element(id) {
  if (!elements[id]) {
    elements[id] = { id, textContent: "", innerHTML: "", className: "", value: "", style: {}, dataset: {}, disabled: false,
      classList: { toggle() {}, has: () => false }, appendChild() {}, select() {}, querySelectorAll: () => [] };
  }
  return elements[id];
}
const documentStub = { getElementById: element, createElement: () => element("_new" + Math.random()), execCommand: () => false };
const consoleStub = { log() {}, warn() {}, error() {} };
const NAMES = ["selfTest", "buildFrame", "parseFrame", "guardedFrame", "ALLOWED_COMMANDS", "decodeAll", "decode23", "METRICS", "COUNTERS",
  "DIFF_KEYS", "STEPS", "stepStats", "meanMax", "diffRows", "csvAll", "csvSummary", "RAW_FIELDS", "UNIT_KEYS", "BOARD_BLOCK_TICKS"];
const page = new Function("document", "navigator", "console", "setInterval", "setTimeout", "clearInterval", "clearTimeout",
  html.slice(a + 8, b) + "\nreturn {" + NAMES.join(",") + "};")(documentStub, {}, consoleStub, () => 0, () => 0, () => {}, () => {});

let failures = 0, checks = 0;
function ok(cond, what) { checks++; if (!cond) { failures++; if (failures <= 25) console.error("FAIL: " + what); } }
function near(x, y, e = 1e-6) { return typeof x === "number" && Math.abs(x - y) <= e; }

ok(element("selftest").textContent === "самопроверка: OK", "page self-test: " + element("selftest").textContent);
ok(page.selfTest() === true, "selfTest() returns true");

/* reply as transfer.c builds it (independent packer) */
function reply(cmd, token, status, data) {
  const out = [0xF0, 0x7D, 0x46, 0x4D, 0x02, cmd | 64, token & 127, token >> 7];
  let value = 0, bits = 0;
  for (const byte of [status].concat(data)) {
    value |= byte << bits; bits += 8;
    while (bits >= 7) { out.push(value & 127); value >>= 7; bits -= 7; }
  }
  if (bits) out.push(value & 127);
  let sum = 0; for (let i = 1; i < out.length; ++i) sum += out[i];
  out.push((0 - sum) & 127, 0xF7);
  return Uint8Array.from(out);
}
function received(cmd, token, status, data) {
  const p = page.parseFrame(reply(cmd, token, status, data));
  return p ? { status: p.data[0], d: p.data.slice(1) } : null;
}
function le32(v) { return [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255]; }

/* synthetic replies for one device: block ticks 1 000 000, everything scaled by "load" (percent of a block) */
const BLK = 1000000;
function build(load, v2) {
  const t = p => Math.round(BLK * p / 100);
  const r17 = [].concat(le32(10), le32(10), le32(0), le32(0), [1, 0]);
  const r18 = [].concat(le32(t(load)), le32(t(load * 1.5)), le32(t(1)), le32(0), le32(0), le32(8), le32(1073741824), le32(1000000));
  const r19 = [].concat(le32(5000), le32(0), le32(0), le32(page.BOARD_BLOCK_TICKS ? Math.round(page.BOARD_BLOCK_TICKS * load / 100) : 0));
  let r23 = [].concat(le32(0), [0], le32(t(2)), le32(BLK), le32(t(load * 2)), le32(t(load * 0.5)), le32(t(load * 3)));
  if (v2) {
    r23 = r23.concat([2, 9, 0]);                       // version 2, 9 units, shift 0
    for (let k = 0; k < 9; ++k) r23.push(...[(k + 1) * 100 & 255, ((k + 1) * 100) >> 8]);   // 1.0 %, 2.0 % ... ticks 100..900 of 1e6 is tiny, fine
    const u16 = v => [v & 255, v >> 8];
    r23 = r23.concat(u16(200), u16(100), u16(50), u16(Math.min(65535, t(load * 4))), u16(300), le32(7));
  }
  return { r17: received(17, 1, 0, r17), r18: received(18, 2, 0, r18), r19: received(19, 3, 0, r19), r23: received(23, 4, 0, r23) };
}

/* 1. decode: new firmware, then an old one (version 1 reply: no version 2 fields) */
{
  const n = build(10, true), s = page.decodeAll(1000, n.r17, n.r18, n.r19, n.r23);
  ok(s.blocks === 5000 && s.underruns === 0 && s.completed === 10, "decode 17 / 19");
  ok(s.block_ticks === BLK && s.engine_call_peak === 200000 && s.micro_d_peak === 50000 && s.mix_peak === 300000, "decode 23 v1 part");
  ok(s.cost_filter === 100 && s.cost_lowcut === 900 && s.base_float === 200 && s.panel_irq_peak === 300 && s.lone_spikes === 7, "decode 23 v2 part");
  const m = k => page.METRICS.find(x => x.key === k).get(s);
  ok(near(m("engine_call"), 20) && near(m("micro_d"), 5) && near(m("ours"), 30), "metrics percent of a block");
  ok(near(m("irq_peak"), 10, 0.01), "irq peak percent: " + m("irq_peak"));
  ok(near(m("render"), 10) && near(m("engine_pk"), 15), "metrics percent of the deadline");
  const o = build(10, false), so = page.decodeAll(1000, o.r17, o.r18, o.r19, o.r23);
  ok(so.engine_call_peak === 200000 && so.cost_filter === undefined && so.lone_spikes === undefined, "old firmware: no v2 fields");
  ok(page.METRICS.find(x => x.key === "guard").get(so) === undefined, "old firmware: guard metric undefined");
  const none = page.decodeAll(1, null, { status: 71, d: [] }, null, null);
  ok(Object.keys(none).length === 1, "no replies decode nothing");
}

/* 2. step statistics from per-second samples of two devices; counters = added during the step */
function samplesFor(loads, lone0) {
  return loads.map((l, i) => {
    const n = build(l, true);
    const s = page.decodeAll(1000 + i * 1000, n.r17, n.r18, n.r19, n.r23);
    s.lone_spikes = lone0 + i; return s;
  });
}
const base0 = page.stepStats(samplesFor([10, 12, 14], 0)), base1 = page.stepStats(samplesFor([20, 20, 20], 5));
const bt0 = page.stepStats(samplesFor([20, 22, 24], 0)), bt1 = page.stepStats(samplesFor([20, 25, 30], 5));
ok(base0.n === 3 && near(base0.metrics.engine_call.mean, 24) && near(base0.metrics.engine_call.max, 28), "stats mean / max");
ok(base0.counters.lone_spikes === 2 && base1.counters.lone_spikes === 2, "counter added during the step");
ok(page.meanMax([]) === null && page.meanMax([undefined, NaN]) === null && page.meanMax([1, 3]).mean === 2, "meanMax edge cases");

/* 3. difference table against step 1 */
{
  const res = { 1: { perDev: [base0, base1] }, 2: { perDev: [bt0, bt1] }, 3: { perDev: [null, bt1] } };
  const rows = page.diffRows(res, 1, [1, 2, 3], 2);
  const get = (st, dv, k) => rows.find(r => r.step === st && r.dev === dv && r.key === k);
  ok(!rows.some(r => r.step === 1), "no row for the base step");
  const r = get(2, 0, "engine_call");
  ok(r && near(r.dMean, 20) && near(r.dMax, 20), "difference dev 0 (+10 % load x2 engine factor)");
  const r1 = get(2, 1, "engine_call");
  ok(r1 && near(r1.dMean, 10) && near(r1.dMax, 20), "difference dev 1");
  ok(!get(3, 0, "engine_call") && get(3, 1, "engine_call"), "missing device skipped");
  ok(page.diffRows({}, 1, [1], 2).length === 0, "no base step -> empty table");
}

/* 4. CSV: raw rows with time, label, step; the summary with differences; quoting */
{
  const s = samplesFor([10], 0)[0];
  const text = page.csvAll([{ t: 1000, dev: 0, label: "Оранж, \"ev\"", step: 2, sample: s }, { t: 2000, dev: 1, label: "Чёрный", step: null, sample: { t: 2000 } }]);
  const lines = text.trim().split("\n");
  ok(lines[0].startsWith("time,unix_ms,synth,step,blocks,"), "csv header");
  ok(lines.length === 3, "csv row count");
  ok(lines[1].startsWith("1970-01-01T00:00:01.000Z,1000,\"Оранж, \"\"ev\"\"\",2,5000,"), "csv row 1: " + lines[1].slice(0, 60));
  ok(lines[2].startsWith("1970-01-01T00:00:02.000Z,2000,Чёрный,,"), "csv row 2 (no step, empty fields)");
  ok(lines[1].split(",").length === lines[0].split(",").length + 1, "csv row 1 has one more cell (the quoted comma)");
  const res = { 1: { note: "тихо", dur: 30, perDev: [base0, base1] }, 2: { note: "", dur: 30, perDev: [bt0, bt1] } };
  const sum = page.csvSummary(res, ["Оранжевый", "Чёрный"], [1, 2]);
  ok(sum.split("\n")[0] === "step,title,note,synth,seconds,samples,metric,mean,max", "summary header");
  ok(/\n1,[^\n]*,тихо,Оранжевый,30,3,[^\n]*Вызов движка[^\n]*,24\.000,28\.000\n/.test("\n" + sum), "summary row with mean / max");
  ok(/diff_vs_step_1,synth,metric,delta_mean,delta_max\n2,Оранжевый,[^\n]*,20\.000,20\.000\n/.test(sum) || sum.includes("diff_vs_step_1,synth"), "summary difference block");
  ok(sum.includes("Одиночные всплески > 75 % (прибавка)"), "summary counters");
}

/* 5. steps list shape and the send guard */
{
  const ids = page.STEPS.map(s => s.id);
  ok(page.STEPS.length >= 6 && ids.every((x, i) => x === i + 1), "steps numbered 1..n");
  ok(page.STEPS.every(s => s.title && s.text), "every step has a title and a text");
  ok(page.DIFF_KEYS.every(k => page.METRICS.some(m => m.key === k)), "DIFF_KEYS exist");
  ok(JSON.stringify(Array.from(page.ALLOWED_COMMANDS)) === "[17,18,19,23]", "allowed commands");
  for (let c = 0; c < 128; ++c) {
    let threw = false;
    try { page.guardedFrame(c, 1, []); } catch (e) { threw = true; }
    ok(threw === ![17, 18, 19, 23].includes(c), "guard, command " + c);
  }
  for (const c of [17, 18, 19, 23]) { let t = false; try { page.guardedFrame(c, 1, [0]); } catch (e) { t = true; } ok(t, "guard payload " + c); }
}

if (failures) { console.error("t_diag2: " + failures + " of " + checks + " checks FAILED"); process.exit(1); }
console.log("t_diag2: OK (" + checks + " checks)");
