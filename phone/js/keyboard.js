// Full-screen multitouch keyboard. Velocity comes from where the finger lands on the key
// (top = soft, bottom = loud). Left strips: PITCH (springs back) and MOD (CC1).
import * as out from './out.js';
import * as midi from './midi.js';
import * as editor from './editor.js';

const WHITE = [0, 2, 4, 5, 7, 9, 11], BLACK_AFTER = [0, 1, 3, 4, 5];
const wide = () => matchMedia('(orientation:landscape) and (min-width:560px)').matches;
let root, oct = 3, hold = false;
const touches = new Map();   // pointerId -> { note, key }
const lit = new Map();       // note -> number of fingers on it

const keyCount = () => (wide() ? 14 : 8);
const baseNote = () => 12 * (oct + 1);
const noteOf = k => baseNote() + WHITE[k % 7] + 12 * Math.floor(k / 7);

function keysHtml() {
  const n = keyCount();
  let h = '';
  for (let k = 0; k < n; k++) h += `<div class="wk" data-n="${noteOf(k)}">${k % 7 === 0 ? `<span class="c">C${oct + Math.floor(k / 7)}</span>` : ''}</div>`;
  for (let k = 0; k < n - 1; k++) {
    if (BLACK_AFTER.includes(k % 7)) h += `<div class="bk" data-n="${noteOf(k) + 1}" style="left:calc(${(k + 1) / n * 100}% - ${100 / n * 0.3}%);width:${100 / n * 0.6}%"></div>`;
  }
  return h;
}
function targetText() {
  const name = editor.patch() ? ' · ' + editor.patchName() : '';
  return (out.toSynth() ? `→ FM-1 · канал ${out.getChannel() + 1}` : 'встроенный звук') + name;
}
export function render() {
  if (!root) return;
  releaseAll();
  root.innerHTML = `<div class="kbtop"><span class="pill ${out.toSynth() ? 'on' : 'warn'}" data-a="ch">${targetText()}</span>
    <span class="sp"></span><button type="button" data-a="dn">◀</button><span class="pill">OCT ${oct}</span><button type="button" data-a="up">▶</button>
    <button type="button" data-a="hold" class="${hold ? 'go' : ''}">HOLD</button><button type="button" data-a="off">СТОП</button><button type="button" data-a="full">⤢</button></div>
    <div class="strip"><div class="wheel" data-w="bend" style="--c:var(--k4)"><i></i></div><div class="wheel" data-w="mod" style="--c:var(--k1)"><i></i></div>
    <div class="keys">${keysHtml()}</div></div>
    <div class="hint">Поверните телефон горизонтально: две октавы.</div>`;
}

function light(note, on) {
  const c = (lit.get(note) || 0) + (on ? 1 : -1);
  if (c > 0) lit.set(note, c); else lit.delete(note);
  root.querySelector(`[data-n="${note}"]`)?.classList.toggle('hit', c > 0);
}
function press(id, key, y) {
  const r = key.getBoundingClientRect(), note = +key.dataset.n;
  const vel = Math.round(30 + 97 * Math.min(1, Math.max(0, (y - r.top) / r.height)));
  touches.set(id, { note, key });
  light(note, true);
  out.noteOn(note, vel, editor.patch()).catch(() => {});
}
function lift(id) {
  const t = touches.get(id);
  if (!t) return;
  touches.delete(id); light(t.note, false); out.noteOff(t.note);
}
function releaseAll() { for (const id of [...touches.keys()]) lift(id); lit.clear(); }

function onDown(e) {
  const w = e.target.closest('[data-w]');
  if (w) return wheel(e, w);
  const key = e.target.closest('.wk,.bk');
  if (!key) return;
  e.preventDefault();
  press(e.pointerId, key, e.clientY);
  // Sliding a finger onto another key plays it (glissando).
  const move = ev => {
    if (ev.pointerId !== e.pointerId) return;
    const under = document.elementFromPoint(ev.clientX, ev.clientY)?.closest('.wk,.bk'), cur = touches.get(ev.pointerId);
    if (under && cur && under !== cur.key) { lift(ev.pointerId); press(ev.pointerId, under, ev.clientY); }
  };
  const up = ev => {
    if (ev.pointerId !== e.pointerId) return;
    window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); window.removeEventListener('pointercancel', up);
    lift(e.pointerId);
  };
  window.addEventListener('pointermove', move); window.addEventListener('pointerup', up); window.addEventListener('pointercancel', up);
}

// PITCH springs back to the centre on release; MOD stays where it was left.
function wheel(e, el) {
  e.preventDefault();
  const kind = el.dataset.w, r = el.getBoundingClientRect(), knob = el.querySelector('i');
  const set = y => {
    const f = 1 - Math.min(1, Math.max(0, (y - r.top) / r.height));   // 0 bottom .. 1 top
    knob.style.bottom = `calc(${f * 100}% - ${f * 16}px)`;
    if (kind === 'bend') out.bend(Math.round(f * 16383)); else out.cc(1, Math.round(f * 127));
  };
  set(e.clientY);
  const move = ev => { if (ev.pointerId === e.pointerId) set(ev.clientY); };
  const up = ev => {
    if (ev.pointerId !== e.pointerId) return;
    window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); window.removeEventListener('pointercancel', up);
    if (kind === 'bend') { knob.style.bottom = 'calc(50% - 8px)'; out.bend(8192); }
  };
  window.addEventListener('pointermove', move); window.addEventListener('pointerup', up); window.addEventListener('pointercancel', up);
}

export function mount(el, onFull) {
  root = el; el.className = 'keyboard';
  el.addEventListener('pointerdown', onDown);
  el.addEventListener('contextmenu', e => e.preventDefault());
  el.addEventListener('click', e => {
    const a = e.target.closest('[data-a]')?.dataset.a;
    if (a === 'dn' && oct > 0) { oct--; render(); }
    if (a === 'up' && oct < 7) { oct++; render(); }
    if (a === 'hold') { hold = !hold; out.cc(64, hold ? 127 : 0); render(); }
    if (a === 'off') { hold = false; out.cc(64, 0); out.panic(); render(); }
    if (a === 'ch') { out.setChannel(out.getChannel() + 1); render(); }
    if (a === 'full') onFull?.();
  });
  matchMedia('(orientation:landscape)').addEventListener('change', render);
  midi.onChange(render);
  render();
}
export const release = releaseAll;
