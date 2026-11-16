// Voice editor: one DX7 voice as a 156-byte VCED patch (bytes 0..125 = OP6..OP1 x 21, 126..144 global, 145..154 name).
import { ALGORITHMS } from './algorithms.js';
import * as out from './out.js';
import * as library from './library.js';

const CATS = ['#ff7eb3', '#3ee07a', '#ffd23f', '#2fd8c8', '#4aa3ff', '#c08bff'];
const KC = ['--k1', '--k2', '--k3', '--k4'];
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const sgn = v => (v > 0 ? '+' : '') + v;
const CURVE = ['-LIN', '-EXP', '+EXP', '+LIN'], WAVE = ['TRI', 'SAW↓', 'SAW↑', 'SQR', 'SIN', 'S&H'];
const P = (n, i, max, fmt = v => v, op = false) => ({ n, i, max, fmt, op });
const eg = (r, l) => [0, 1, 2, 3].map(k => P('R' + (k + 1), r + k, 99, v => v, r < 126))
  .concat([0, 1, 2, 3].map(k => P('L' + (k + 1), l + k, 99, v => v, r < 126)));
const PAGES = {
  common: ['ОБЩЕЕ', () => [P('CHOIR', 134, 31, v => v + 1), P('FEEDBK', 135, 7), P('TRANSP', 144, 48, v => sgn(v - 24)), P('SYNC', 136, 1, v => (v ? 'ON' : 'OFF'))]],
  op: ['OP', () => [P('LEVEL', 16, 99, v => v, true), P('COARSE', 18, 31, v => v, true), P('FINE', 19, 99, v => v, true), P('DETUNE', 20, 14, v => sgn(v - 7), true),
    P('MODE', 17, 1, v => (v ? 'FIXED' : 'RATIO'), true), P('VELO', 15, 7, v => v, true), P('AMS', 14, 3, v => v, true)]],
  eg: ['EG', () => eg(0, 4)],
  ks: ['SCALE', () => [P('BREAK', 8, 99, v => v, true), P('L DEPTH', 9, 99, v => v, true), P('R DEPTH', 10, 99, v => v, true),
    P('L CURVE', 11, 3, v => CURVE[v], true), P('R CURVE', 12, 3, v => CURVE[v], true), P('RATE SC', 13, 7, v => v, true)]],
  lfo: ['LFO', () => [P('SPEED', 137, 99), P('DELAY', 138, 99), P('PMD', 139, 99), P('AMD', 140, 99), P('WAVE', 142, 5, v => WAVE[v]), P('PMS', 143, 7), P('SYNC', 141, 1, v => (v ? 'ON' : 'OFF'))]],
  peg: ['PITCH', () => eg(126, 130)],
};
const ORDER = ['common', 'op', 'eg', 'ks', 'lfo', 'peg'];

let root, S = null;   // S = { b, i, orig, patch, op (1..6), page }
const base = op => (6 - op) * 21;
const idx = (p, op) => p.i + (p.op ? base(op) : 0);
const nameOf = patch => String.fromCharCode(...patch.slice(145, 155)).replace(/[^\x20-\x7e]/g, '?').trimEnd();
const dirty = () => S.patch.some((v, k) => v !== S.orig[k]);

export function open(b, i) {
  const orig = library.voicePatch(b, i);
  S = { b, i, orig, patch: orig.slice(), op: 1, page: 'common' };
  render();
}
export const current = () => S && [S.b, S.i];
export const patch = () => S?.patch || null;
export const patchName = () => (S ? nameOf(S.patch) : '');

function ratioText() {
  const o = base(S.op), p = S.patch;
  if (p[o + 17]) return `FIXED ${(10 ** (p[o + 18] & 3) * (1 + p[o + 19] / 100)).toFixed(1)} Hz`;
  return 'RATIO ' + ((p[o + 18] || 0.5) * (1 + p[o + 19] / 100)).toFixed(2);
}

// Algorithm picture: positions from the firmware table, links drawn to the nearest operator one row down.
function labHtml() {
  const lay = ALGORITHMS[S.patch[134] & 31], pts = [];
  let cmin = 15, cmax = 0, rmax = 0;
  for (let k = 0; k < 6; k++) {
    const c = lay[k * 2] >> 4, r = lay[k * 2] & 15;
    pts.push({ c, r, fb: lay[k * 2 + 1] & 15 });
    cmin = Math.min(cmin, c); cmax = Math.max(cmax, c); rmax = Math.max(rmax, r);
  }
  const X = c => (cmax === cmin ? 50 : 12 + (c - cmin) / (cmax - cmin) * 76), Y = r => 78 - (rmax ? r / rmax * 62 : 0);
  let lines = '';
  pts.forEach((p, k) => {
    if (!p.r) return;
    const t = pts.filter((q, j) => j !== k && q.r === p.r - 1).sort((a, b) => Math.abs(a.c - p.c) - Math.abs(b.c - p.c))[0] || pts.find(q => !q.r);
    lines += `<line x1="${X(p.c)}%" y1="${Y(p.r) + 8}%" x2="${X(t.c)}%" y2="${Y(t.r) - 4}%" stroke="#2fd8c8" stroke-width="2" opacity=".6"/>`;
  });
  const cats = pts.map((p, k) => `<div class="op${k + 1 === S.op ? ' sel' : ''}${p.fb ? ' fb' : ''}" data-op="${k + 1}" style="--c:${CATS[k]};left:calc(${X(p.c)}% - 19px);top:calc(${Y(p.r)}% - 15px)">${k + 1}</div>`).join('');
  return `<svg>${lines}</svg>${cats}`;
}

function knobHtml(p, k) {
  const v = S.patch[idx(p, S.op)], deg = v / p.max * 270;
  return `<div class="kn" data-k="${k}"><div class="dial" style="--c:var(${KC[k % 4]});--p:${deg}deg" data-v="${esc(p.fmt(v))}"></div><div class="name" style="--c:var(${KC[k % 4]})">${p.n}</div></div>`;
}

function miniKeys() {
  let h = '';
  for (let k = 0; k < 14; k++) h += `<span class="wk" data-n="${60 + [0, 2, 4, 5, 7, 9, 11][k % 7] + 12 * Math.floor(k / 7)}"></span>`;
  [[0, 61], [1, 63], [3, 66], [4, 68], [5, 70], [7, 73], [8, 75], [10, 78], [11, 80], [12, 82]]
    .forEach(([k, n]) => { h += `<span class="bk" data-n="${n}" style="left:calc(${(k + 1) / 14 * 100}% - 3.2%)"></span>`; });
  return h;
}

function render() {
  if (!root) return;
  if (!S) { root.innerHTML = '<div class="card"><h2>РЕДАКТОР</h2><div class="dim">Выберите голос в библиотеке: двойной тап по строке.</div></div>'; return; }
  const params = PAGES[S.page][1](), showOp = ['op', 'eg', 'ks'].includes(S.page);
  root.innerHTML = `<div class="etop"><span class="pill">${'ABCD'[S.b]}${String(S.i + 1).padStart(2, '0')}</span>
    <button class="ename" type="button" data-a="name">${esc(nameOf(S.patch)) || '—'}</button>
    <button class="pill" type="button" data-a="ch">CH ${out.getChannel() + 1}</button>
    <span class="pill${dirty() ? ' warn' : ''}" id="edirty">${dirty() ? '● ИЗМ.' : 'ок'}</span></div>
    <div class="pages">${ORDER.map(k => `<button type="button" class="pg${k === S.page ? ' sel' : ''}" data-page="${k}">${PAGES[k][0]}</button>`).join('')}</div>
    <div class="eb"><div class="lab" id="lab">${labHtml()}</div>
    <div class="ek"><div class="sub">${showOp ? `OP${S.op}${S.page === 'op' ? ' · ' + ratioText() : ''}` : ''}</div>
    <div class="knobs">${params.map(knobHtml).join('')}</div></div></div>
    <div class="mini" id="mini">${miniKeys()}</div>
    <div class="bar"><button type="button" data-a="reset"${dirty() ? '' : ' disabled'}>СБРОС</button><button type="button" class="go" data-a="save"${dirty() ? '' : ' disabled'}>СОХРАНИТЬ В БАНК</button></div>`;
}

function refresh() {
  const params = PAGES[S.page][1]();
  root.querySelectorAll('.kn').forEach(el => {
    const p = params[+el.dataset.k], v = S.patch[idx(p, S.op)], d = el.querySelector('.dial');
    d.style.setProperty('--p', `${v / p.max * 270}deg`); d.dataset.v = p.fmt(v);
  });
  const dd = dirty(), e = root.querySelector('#edirty');
  e.textContent = dd ? '● ИЗМ.' : 'ок'; e.classList.toggle('warn', dd);
  root.querySelectorAll('.bar button').forEach(b => { b.disabled = !dd; });
  root.querySelector('.ename').textContent = nameOf(S.patch) || '—';
  if (S.page === 'op') root.querySelector('.sub').textContent = `OP${S.op} · ${ratioText()}`;
  if (S.page === 'common') root.querySelector('#lab').innerHTML = labHtml();
  out.voice(S.patch);
}

function setParam(p, v) {
  v = Math.max(0, Math.min(p.max, Math.round(v)));
  const at = idx(p, S.op);
  if (S.patch[at] === v) return;
  S.patch[at] = v;
  refresh();
}

function setName(text) {
  const t = text.toUpperCase().replace(/[^\x20-\x7e]/g, '').slice(0, 10).padEnd(10, ' ');
  for (let k = 0; k < 10; k++) S.patch[145 + k] = t.charCodeAt(k);
  refresh();
}

// Drag a knob up/down: full range over 150 px. Double tap = type a number.
function onKnobDown(e) {
  const el = e.target.closest('.kn');
  if (!el) return;
  e.preventDefault();
  const p = PAGES[S.page][1]()[+el.dataset.k], y0 = e.clientY, v0 = S.patch[idx(p, S.op)];
  let moved = false;
  const move = ev => { const dy = y0 - ev.clientY; if (Math.abs(dy) > 3) moved = true; if (moved) setParam(p, v0 + dy / 150 * p.max); };
  const up = () => {
    window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); window.removeEventListener('pointercancel', up);
    if (moved) return;
    const now = Date.now();
    if (el._tap && now - el._tap < 350) {
      el._tap = 0;
      const r = prompt(`${p.n} (0–${p.max})`, String(S.patch[idx(p, S.op)]));
      if (r !== null && r.trim() !== '' && !isNaN(+r)) setParam(p, +r);
    } else el._tap = now;
  };
  window.addEventListener('pointermove', move); window.addEventListener('pointerup', up); window.addEventListener('pointercancel', up);
}

// Preview keys: velocity grows from the top of the key to the bottom.
function onKeyDown(e) {
  const k = e.target.closest('.wk,.bk');
  if (!k) return;
  e.preventDefault();
  const r = k.getBoundingClientRect(), n = +k.dataset.n;
  const vel = Math.round(40 + 87 * Math.min(1, Math.max(0, (e.clientY - r.top) / r.height)));
  k.classList.add('hit');
  out.noteOn(n, vel, S.patch).catch(() => {});
  const up = () => { window.removeEventListener('pointerup', up); window.removeEventListener('pointercancel', up); k.classList.remove('hit'); out.noteOff(n); };
  window.addEventListener('pointerup', up); window.addEventListener('pointercancel', up);
}

export function mount(el) {
  root = el; el.className = 'editor'; render();
  el.addEventListener('pointerdown', e => { if (!S) return; onKnobDown(e); onKeyDown(e); });
  el.addEventListener('click', e => {
    if (!S) return;
    const page = e.target.closest('[data-page]')?.dataset.page, op = e.target.closest('[data-op]')?.dataset.op, a = e.target.closest('[data-a]')?.dataset.a;
    if (page) { S.page = page; render(); }
    if (op) { S.op = +op; if (['common', 'lfo', 'peg'].includes(S.page)) S.page = 'op'; render(); }
    if (a === 'name') { const r = prompt('Имя голоса (до 10 символов)', nameOf(S.patch)); if (r !== null) setName(r); }
    if (a === 'ch') { out.setChannel(out.getChannel() + 1); render(); }
    if (a === 'reset') { S.patch = S.orig.slice(); render(); out.voice(S.patch); }
    if (a === 'save') {
      try { library.commitVoice(S.b, S.i, S.patch); S.orig = S.patch.slice(); render(); }
      catch { alert('Не удалось сохранить: параметр вне допустимых границ.'); }
    }
  });
}
