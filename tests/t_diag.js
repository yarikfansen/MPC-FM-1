/* Host test for tools/fm1-diag.html (the diagnostic page; it sends only read-only commands).
 * Usage: node tests/t_diag.js [path/to/fm1-diag.html]
 * Runs the page's whole script with a stub DOM and no Web MIDI (so nothing is polled), then:
 * 1. the page's own self-test must report OK (hand-computed frames, replies 23 / 24, send guard);
 * 2. command 24 (black box, app/blackbox.h bb_read) through the real framing: request frames for parts
 *    0..2, the reply bytes of tests/t_blackbox.c case 8 packed as transfer.c packs them, parsed and decoded;
 * 3. what the page shows for that reply, for a cold start, for old firmware (error status) and a lost part;
 * 4. the send guard; 5. command 25 (F8 screen timing): framing, decoder, shown ms, old firmware.
 */
"use strict";
const fs = require("fs");
const path = require("path");
const file = process.argv[2] || path.join(__dirname, "..", "tools", "fm1-diag.html");
const html = fs.readFileSync(file, "utf8");
const a = html.lastIndexOf("<script>"), b = html.lastIndexOf("</script>");
if (a < 0 || b < a) { console.error("FAIL: script not found"); process.exit(1); }

/* ---- stub DOM: every element remembers text / html / class ---- */
const elements = {};
function element(id) {
  if (!elements[id]) {
    const classes = new Set();
    elements[id] = { id, textContent: "", innerHTML: "", className: "", value: "", style: {}, dataset: {},
      classList: { toggle(c, on) { if (on) classes.add(c); else classes.delete(c); }, has: c => classes.has(c) },
      appendChild() {}, select() {} };
  }
  return elements[id];
}
const documentStub = { getElementById: element, createElement: () => element("_new" + Math.random()), execCommand: () => false };
const navigatorStub = {};           // no requestMIDIAccess: startMidi() stops at once, nothing is sent
const consoleStub = { log() {}, warn() {}, error() {} };
const NAMES = ["selfTest", "buildFrame", "parseFrame", "packStream", "guardedFrame", "decode24", "showBlackBox", "BB_PARTS",
  "ALLOWED_COMMANDS", "decode25", "showUi", "uiMs", "decode23", "record", "csv", "UNIT_KEYS", "UNIT_NAMES", "LOG_FIELDS"];
const page = new Function("document", "navigator", "console", "setInterval",
  html.slice(a + 8, b) + "\nreturn {" + NAMES.join(",") + "};")(documentStub, navigatorStub, consoleStub, () => 0);

let failures = 0, checks = 0;
function ok(cond, what) { checks++; if (!cond) { failures++; if (failures <= 25) console.error("FAIL: " + what); } }
function eq(x, y) { return x.length === y.length && x.every((v, i) => v === y[i]); }

/* 1. the page's self-test */
ok(element("selftest").textContent === "самопроверка: OK", "page self-test: " + element("selftest").textContent);
ok(page.selfTest() === true, "selfTest() returns true");

/* 2. command 24 through the framing. Request: payload one byte, 12-byte frame, only parts 0..2. */
for (let p = 0; p < page.BB_PARTS; ++p) {
  const f = page.guardedFrame(24, 100 + p, [p]);
  const back = page.parseFrame(Uint8Array.from(f));
  ok(f.length === 12 && back && back.cmd === 24 && back.token === 100 + p && eq(back.data, [p]), "request 24 part " + p);
}
/* transfer.c reply: header copied with cmd | 64, then pack(status, data...), checksum, F7 (an independent packer) */
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
/* the bytes tests/t_blackbox.c case 8 checks bb_read against */
const P0 = [1,0,3,3, 0x2c,0x01,0,0, 0xea,0,0,0, 1,0, 0xff,0xff, 1,0, 12,20,88,88,4,1,
            0xc8,0,0,0, 0x32,0,0,0, 0,0, 2,0, 0,0, 7,3,91,91,2,1];
const P1 = [1,1,3,3, 0x2c,0x01,0,0,3,12,20,88,1,0, 0x2c,0x01,0,0,2,12,20,88,1,0, 0x2c,0x01,0,0,1,12,20,88,0xff,0xff,
            0,0,0,0,0,0,0,0,0,0];
const P2 = [1,2,3,3].concat(new Array(40).fill(0));
function received(parts, statuses) {
  return parts.map((d, i) => {
    if (!d) return null;
    const m = reply(24, 7 + i, statuses ? statuses[i] : 0, d);
    ok(m.length <= 96, "reply fits transfer.c's 96-byte cache: " + m.length);
    const p = page.parseFrame(m);
    ok(p && p.cmd === (24 | 64) && p.token === 7 + i, "reply 24/" + i + " framing");
    return p ? { status: p.data[0], d: p.data.slice(1) } : null;
  });
}
const parts = received([P0, P1, P2]);
const bb = page.decode24(parts);
ok(bb && bb.complete && bb.count === 3, "decode24 complete");
ok(bb && bb.now && bb.now.uptime_ms === 300 && bb.now.control_ms === 234 && bb.now.restarts === 1 && bb.now.overruns === 65535 &&
   bb.now.sheds === 1 && bb.now.page === 12 && bb.now.control === 20 && bb.now.cpu === 88 && bb.now.cpu_peak === 88 &&
   bb.now.flags === 4 && bb.now.version === 1, "this session's record");
ok(bb && bb.last && bb.last.uptime_ms === 200 && bb.last.control_ms === 50 && bb.last.restarts === 0 && bb.last.overruns === 2 &&
   bb.last.sheds === 0 && bb.last.page === 7 && bb.last.control === 3 && bb.last.cpu === 91 && bb.last.flags === 2, "previous record");
ok(bb && bb.events.length === 3 && bb.events.map(e => e.kind).join() === "3,2,1" && bb.events[2].count === 65535 &&
   bb.events.every(e => e.uptime_ms === 300 && e.page === 12 && e.control === 20 && e.cpu === 88) &&
   bb.events.map(e => e.index).join() === "0,1,2", "load log");
/* a full log: entries 4..7 come from part 2 */
const full = P2.slice(); full[3] = 8;
for (let k = 0; k < 4; ++k) { const o = 4 + 10 * k; full[o] = 0x10 + k; full[o + 1] = 0x27; full[o + 4] = 1; full[o + 8] = k + 1; }
const bf = page.decode24(received([P0, P1, full]));
ok(bf && bf.events.length === 7 && bf.events[3].index === 4 && bf.events[6].index === 7 && bf.events[6].uptime_ms === 0x2713 &&
   bf.events[6].count === 4, "part 2 entries 4..7");

/* 3. what the page shows */
page.showBlackBox(bb, parts);
const now = element("bb_now").innerHTML, last = element("bb_last").innerHTML, logHtml = element("bb_log").innerHTML;
ok(now.includes("0 с") && now.includes("12") && now.includes("кнопка 4") &&
   now.includes("88 % / 88 %") && now.includes("65") && now.includes("USB подключён"), "this session shown: " + now.slice(0, 200));
ok(last.includes("ручка (энкодер) 3") && last.includes("91 % / 91 %") && last.includes("секвенсор играл") &&
   last.includes("row alarm"), "previous session shown");
ok((logHtml.match(/class='row alarm'/g) || []).length === 3 && logHtml.includes("нагрузка выше 85 %") &&
   logHtml.includes("защита выключила эффекты: 1") && logHtml.includes("потеряны звуковые блоки: 65535"), "log shown");
const cold = P0.slice(); cold[2] = 1; cold[3] = 0;
const pc = received([cold, P2.map((v, i) => i === 1 ? 1 : v), P2]);
page.showBlackBox(page.decode24(pc), pc);
ok(element("bb_last").innerHTML.includes("был выключен") && element("bb_log").innerHTML.includes("перегрузок не было"), "cold start shown");
const old = received([[], [], []], [71, 71, 71]);       // older firmware: "invalid" for an unknown command
ok(page.decode24(old) === null, "old firmware decodes nothing");
page.showBlackBox(null, old);
ok(["bb_now", "bb_last", "bb_log"].every(id => element(id).innerHTML.includes("старая прошивка")), "old firmware shown");
const lost = received([P0, null, P2]);
const bl = page.decode24(lost);
ok(bl && !bl.complete && bl.events.length === 0, "a lost part is reported");
page.showBlackBox(bl, lost);
ok(element("bb_log").innerHTML.includes("неполные"), "a lost part is shown");
/* the page escapes nothing from the synth into markup: numbers only */
ok(!/<script/i.test(now + last + logHtml), "no markup from data");

/* 4. the guard: only read-only commands, 24 only with one byte 0..2 */
for (let c = 0; c < 128; ++c) {
  let threw = false;
  try { page.guardedFrame(c, 1, c === 24 ? [1] : []); } catch (e) { threw = true; }
  ok(threw !== page.ALLOWED_COMMANDS.includes(c), "guard command " + c);
}
ok([17, 18, 19, 23, 24, 25].every(c => page.ALLOWED_COMMANDS.includes(c)) && page.ALLOWED_COMMANDS.every(c => c >= 17 && c <= 25),
   "allowed list is read-only commands only");

/* 5. command 25 (F8 screen timing, app/ui_meter.h): request without payload, the reply bytes of
 *    tests/t_ui_meter.c case 2 through the framing, decoded and shown in ms */
{
  const f = page.guardedFrame(25, 9, []);
  ok(f.length === 10 && f[5] === 25, "request 25");
  let threw = false; try { page.guardedFrame(25, 9, [0]); } catch (e) { threw = true; }
  ok(threw, "25 refuses a payload");
  const D = [1, 0x45,0x23,0x01,0, 0,0,0,0,
             3,0,0,0, 0x90,0x01,0,0, 0xfa,0,0,0, 1,0,
             2,0,0,0, 0x71,0x11,0x01,0, 0x70,0x11,0x01,0, 2,0,
             1,0,0,0, 4,3,2,1, 4,3,2,1, 6,0];
  const m = reply(25, 300, 0, D);
  ok(m.length <= 96, "reply 25 fits the reply cache: " + m.length);
  const p = page.parseFrame(m);
  ok(p && p.cmd === (25 | 64) && p.token === 300 && p.data.length === 52, "reply 25 framing");
  const u = page.decode25({ status: p.data[0], d: p.data.slice(1) });
  ok(u && u.block_ticks === 0x12345 && u.period === 0 && u.draw.count === 3 && u.draw.max === 400 && u.draw.avg === 250 &&
     u.draw.max_ms === 1 && u.compose.count === 2 && u.compose.max === 70001 && u.compose.avg === 70000 && u.compose.max_ms === 2 &&
     u.flush.count === 1 && u.flush.max === 0x01020304 && u.flush.avg === 0x01020304 && u.flush.max_ms === 6, "decode25");
  const slow = D.slice(); slow[49] = 0x2c; slow[50] = 0x01;      // flush max 300 ms (two bytes)
  ok(page.decode25({ status: 0, d: slow }).flush.max_ms === 300, "max ms above 255");
  const other = D.slice(); other[0] = 2;                          // a later layout: not read as this one
  ok(page.decode25({ status: 0, d: other }) === null, "unknown layout version");
  /* ms: one block = 128 / 44118 s = 2.9013 ms; at 928 419 ticks per block, 320 000 ticks = 1.00 ms */
  ok(Math.abs(page.uiMs(320000, 928419) - 1.0000) < 0.001 && page.uiMs(5, 0) === undefined, "ticks to ms");
  const real = { block_ticks: 928419, period: 0,
    draw: { count: 30, max: 640000, avg: 320000, max_ms: 2 }, compose: { count: 0, max: 0, avg: 0, max_ms: 0 },
    flush: { count: 100, max: 1920000, avg: 960000, max_ms: 6 } };
  page.showUi(real);
  const h = element("ui_rows").innerHTML;
  ok(h.includes("макс 2.00 мс / сред 1.00 мс") && h.includes("30 раз за секунду") && h.includes("0 <small>не было") &&
     h.includes("макс 6.00 мс / сред 3.00 мс") && h.includes("100 опросов за секунду") && h.includes("макс по часам 6 мс"),
     "screen timing shown: " + h.slice(0, 300));
  page.showUi(page.decode25({ status: 71, d: [] }));
  ok(element("ui_rows").innerHTML.includes("старая прошивка"), "old firmware: screen timing");
}

/* 6. command 23 version 2 (audit A1: guard costs per unit, guard_now_peak, lone_spikes, panel_irq_peak).
 *    The reply as app/transfer.c builds it (an independent packer here), through the framing; shown in
 *    percent of a block with the unit names; in the CSV; a version 1 reply (older firmware) still reads. */
{
  const B = 928355, SH = 5;
  function p32(out, v) { for (let i = 0; i < 4; ++i) out.push(Math.floor(v / 2 ** (8 * i)) & 255); }
  function p16s(out, ticks) {                    // transfer.c put16s: >> 5, never 0 for a non-zero cost, saturated
    let v = Math.floor(ticks / 32); if (!v && ticks) v = 1; if (v > 65535) v = 65535;
    out.push(v & 255, v >> 8);
  }
  const L = { shed_count: 7, shedding: 0, fx_ticks: 150000, block_ticks: B, engine_peak: 46418, micro_d_peak: 3000, mix_peak: 190000,
    unit_cost: [64985, 9284, 0, 31, 4640, 92836, 18567, 13925, 928], base_float: 27851, base_dry: 9280, clear_cost: 37134,
    guard_now_peak: 742684, panel_irq_peak: 3000000, lone_spikes: 70000 };
  function reply23(l, v2) {
    const d = [];
    p32(d, l.shed_count); d.push(l.shedding); p32(d, l.fx_ticks); p32(d, l.block_ticks);
    p32(d, l.engine_peak); p32(d, l.micro_d_peak); p32(d, l.mix_peak);
    if (!v2) return d;
    d.push(2, l.unit_cost.length, SH);
    for (const c of l.unit_cost) p16s(d, c);
    for (const k of ["base_float", "base_dry", "clear_cost", "guard_now_peak", "panel_irq_peak"]) p16s(d, l[k]);
    p32(d, l.lone_spikes);
    return d;
  }
  const d2 = reply23(L, true);
  ok(d2.length === 60 && d2.length <= 64, "reply 23 v2 is 60 bytes (transfer.c response[65]): " + d2.length);
  const m = reply(23, 55, 0, d2);
  ok(m.length <= 96, "reply 23 v2 fits the 96-byte reply cache: " + m.length);
  const p = page.parseFrame(m);
  ok(p && p.cmd === (23 | 64) && p.data.length === 61, "reply 23 v2 framing");
  const s = { t: 1000, blocks: 10 }; page.decode23(s, { status: p.data[0], d: p.data.slice(1) });
  ok(s.shed_count === 7 && s.fx_ticks === 150000 && s.block_ticks === B && s.engine_call_peak === 46418 && s.micro_d_peak === 3000 &&
     s.mix_peak === 190000, "v2: the version 1 fields at their old offsets");
  ok(page.UNIT_KEYS.length === 9 && page.UNIT_NAMES.length === 9, "nine units named");
  ok(page.UNIT_KEYS.every((k, u) => s[k] === (L.unit_cost[u] === 31 ? 32 : Math.floor(L.unit_cost[u] / 32) * 32)),
     "unit costs to 32 ticks, a non-zero cost never 0: " + page.UNIT_KEYS.map(k => s[k]).join());
  ok(s.base_float === 27840 && s.base_dry === 9280 && s.clear_cost === 37120 && s.guard_now_peak === 742656 &&
     s.panel_irq_peak === 65535 * 32 && s.lone_spikes === 70000, "bases, peaks, lone spikes");
  page.record(s);
  const units = element("v_units").innerHTML;
  ok(units.includes("FILTER 7.0 %") && units.includes("CHORUS нет замера") && units.includes("REVERB 10.0 %") &&
     units.includes("EQ 2.0 %") && units.includes("COMP 1.5 %") && units.includes("LOW CUT 0.1 %") && units.includes("DRIVE 1.0 %") &&
     units.includes("RING 0.5 %") && units.includes("PHASER 0.0 %"), "unit costs shown in % with names: " + units);
  ok(element("v_bases").innerHTML === "3.0 % / 1.0 % / 4.0 %", "bases shown: " + element("v_bases").innerHTML);
  ok(element("v_gnow").innerHTML.startsWith("80.0 %"), "guard peak shown: " + element("v_gnow").innerHTML);
  ok(element("v_panel").innerHTML.startsWith("≥ 225.9 %"), "saturated panel peak marked: " + element("v_panel").innerHTML);
  ok(element("v_lone").innerHTML.includes("+0 за секунду"), "lone spikes, first poll");
  const s2 = { t: 2000, blocks: 20 }; const L2 = Object.assign({}, L, { lone_spikes: 70003 });
  page.decode23(s2, { status: 0, d: reply23(L2, true) }); page.record(s2);
  ok(element("v_lone").innerHTML.includes("+3 за секунду"), "lone spikes delta: " + element("v_lone").innerHTML);
  /* older firmware: the 25-byte reply */
  const s1 = { t: 3000, blocks: 30 }; page.decode23(s1, { status: 0, d: reply23(L, false) });
  ok(s1.shed_count === 7 && s1.mix_peak === 190000 && s1.cost_filter === undefined && s1.lone_spikes === undefined &&
     s1.guard_now_peak === undefined, "version 1 reply: old fields read, new ones absent");
  page.record(s1);
  ok(["v_units", "v_bases", "v_gnow", "v_lone", "v_panel"].every(id => element(id).innerHTML.includes("старая прошивка")) &&
     !element("v_mixpeak").innerHTML.includes("старая прошивка"), "version 1 reply shown");
  /* a later version with one more unit and more bytes at the end: the known fields keep their meaning */
  const L3 = Object.assign({}, L, { unit_cost: L.unit_cost.concat([640]) });
  const d3 = reply23(L3, true); d3[25] = 3; d3.push(9, 9, 9);
  const s3 = {}; page.decode23(s3, { status: 0, d: d3 });
  ok(s3.cost_lowcut === 928 && s3.base_float === 27840 && s3.lone_spikes === 70000, "a longer later version still reads");
  /* CSV: the new columns after the old ones (old column positions unchanged), values in ticks */
  const lines = page.csv().trim().split("\n"), head = lines[0].split(",");
  ok(head.indexOf("st25") < head.indexOf("cost_filter") && head[head.length - 1] === "lone_spikes" &&
     head.indexOf("shed_count") === 2 + page.LOG_FIELDS.indexOf("shed_count"), "CSV header");
  ok(lines.length === 4 && lines[1].split(",")[head.indexOf("cost_reverb")] === "92832" &&
     lines[1].split(",")[head.indexOf("guard_now_peak")] === "742656" && lines[2].split(",")[head.indexOf("lone_spikes")] === "70003" &&
     lines[3].split(",")[head.indexOf("cost_reverb")] === "" && lines[3].split(",")[head.indexOf("mix_peak")] === "190000", "CSV rows");
}

if (failures) { console.error("diag: " + failures + " of " + checks + " checks FAILED"); process.exit(1); }
console.log("diag: all " + checks + " checks passed");
