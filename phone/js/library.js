// Library screen: four DX7 banks (A-D) x 32 voices kept in IndexedDB.
// A voice is 128 raw VMEM bytes; the name is bytes 118..127, algorithm = byte 110 & 31.
// Edits mark a bank "dirty"; FROM SYNTH / TO SYNTH transfer whole banks (library cmds 1-6, kind 4).
import * as store from './store.js';
import * as midi from './midi.js';

const VOICES = 32, VSIZE = 128, LETTERS = 'ABCD';
const CATS = ['#ff7eb3', '#3ee07a', '#ffd23f', '#2fd8c8', '#4aa3ff', '#c08bff', '#ff9a3c'];
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const wide = () => matchMedia('(orientation:landscape) and (min-width:560px)').matches;

// INIT VOICE as a 155-byte VCED patch, packed to VMEM through FM1Web.
function initVoice() {
  const p = new Uint8Array(156);
  for (let o = 0; o < 6; o++) {
    const j = o * 21;
    p.set([99, 99, 99, 99, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 0, o === 5 ? 99 : 0, 0, 1, 0, 7], j);
  }
  p.set([99, 99, 99, 99, 50, 50, 50, 50, 0, 0, 1, 35, 0, 0, 0, 1, 0, 3, 24], 126);
  p.set(Array.from('INIT VOICE', c => c.charCodeAt(0)), 145);
  p[155] = 63;
  return FM1Web.packVoice(p);
}
const initBank = () => { const v = initVoice(), raw = new Uint8Array(VOICES * VSIZE); for (let i = 0; i < VOICES; i++) raw.set(v, i * VSIZE); return raw; };

const getName = (raw, i) => store.voiceName(raw, i);
const setName = (raw, i, s) => {
  const t = s.toUpperCase().replace(/[^\x20-\x7e]/g, '').slice(0, 10).padEnd(10, ' ');
  for (let k = 0; k < 10; k++) raw[i * VSIZE + 118 + k] = t.charCodeAt(k);
};
const algOf = (raw, i) => raw[i * VSIZE + 110] & 31;
const voiceAt = (raw, i) => raw.slice(i * VSIZE, (i + 1) * VSIZE);

const S = { banks: [], dirty: [false, false, false, false], left: 0, right: 2, active: 'left', sel: [0, 0], undo: [] };
let root, toastTimer, ready = false, busy = false, lastTap = { t: 0, b: -1, i: -1 };
export const hooks = { edit: null };

async function load() {
  const saved = await store.allBanks();
  for (let b = 0; b < 4; b++) {
    const hit = saved.find(x => x.index === b);
    S.banks[b] = hit ? hit.raw : initBank();
    S.dirty[b] = hit ? hit.dirty : false;
  }
  ready = true;
}
const persist = b => store.putBank(b, S.banks[b], S.dirty[b]);

// Run a change over some banks; snapshot them first so the toast can undo.
function mutate(banks, fn, message) {
  S.undo.push(banks.map(b => ({ b, raw: S.banks[b].slice(), dirty: S.dirty[b] })));
  if (S.undo.length > 20) S.undo.shift();
  fn();
  for (const b of banks) { S.dirty[b] = true; persist(b); }
  render();
  toast(message, true);
}
function undo() {
  const snap = S.undo.pop();
  if (!snap) return;
  for (const s of snap) { S.banks[s.b] = s.raw; S.dirty[s.b] = s.dirty; persist(s.b); }
  render(); toast('Отменено');
}

function toast(text, withUndo) {
  clearTimeout(toastTimer);
  root.querySelector('.toast')?.remove();
  const t = document.createElement('div');
  t.className = 'toast';
  t.innerHTML = `<span>${esc(text)}</span>${withUndo ? '<button type="button">ОТМЕНИТЬ</button>' : ''}`;
  t.querySelector('button')?.addEventListener('click', () => { undo(); });
  root.append(t);
  toastTimer = setTimeout(() => t.remove(), 4500);
}

function sheet(title, options) {
  closeSheet();
  const o = document.createElement('div');
  o.className = 'overlay';
  o.innerHTML = `<div class="sheet"><div class="h">${esc(title)}</div>${options.map((x, i) =>
    `<button type="button" class="opt${x.danger ? ' danger' : ''}" data-o="${i}">${esc(x.label)}</button>`).join('')}</div>`;
  o.addEventListener('click', e => {
    const b = e.target.closest('[data-o]');
    if (b) { closeSheet(); options[+b.dataset.o].fn(); } else if (e.target === o) closeSheet();
  });
  root.append(o);
}
const closeSheet = () => root.querySelector('.overlay')?.remove();

function askText(title, value, ok) {
  closeSheet();
  const o = document.createElement('div');
  o.className = 'overlay';
  o.innerHTML = `<div class="sheet"><div class="h">${esc(title)}</div>
    <input class="txt" maxlength="10" value="${esc(value)}" autocapitalize="characters" spellcheck="false">
    <div class="two"><button type="button" data-c>ОТМЕНА</button><button type="button" class="go" data-ok>ОК</button></div></div>`;
  const input = o.querySelector('input');
  const done = () => { closeSheet(); ok(input.value); };
  o.addEventListener('click', e => {
    if (e.target.closest('[data-ok]')) done(); else if (e.target === o || e.target.closest('[data-c]')) closeSheet();
  });
  input.addEventListener('keydown', e => { if (e.key === 'Enter') done(); });
  root.append(o);
  input.focus(); input.select();
}

const bankPick = (title, fn, except) => sheet(title, [0, 1, 2, 3].filter(b => b !== except)
  .map(b => ({ label: `Банк ${LETTERS[b]}`, fn: () => fn(b) })));

function voiceMenu(b, i) {
  const raw = S.banks[b], nm = getName(raw, i).trim() || '—', tag = `${LETTERS[b]}${String(i + 1).padStart(2, '0')} · ${nm}`;
  sheet(tag, [
    { label: '✎ Переименовать', fn: () => askText('Имя голоса', nm, v => mutate([b], () => setName(raw, i, v), 'Переименовано')) },
    { label: '⧉ Копировать в… (тот же номер)', fn: () => bankPick('Копировать в', d => mutate([d], () => S.banks[d].set(voiceAt(raw, i), i * VSIZE), `Скопировано в ${LETTERS[d]}${i + 1}`), b) },
    { label: '⇄ Переместить в… (обмен номерами)', fn: () => bankPick('Переместить в', d => mutate([b, d], () => {
      const mine = voiceAt(raw, i), theirs = voiceAt(S.banks[d], i);
      raw.set(theirs, i * VSIZE); S.banks[d].set(mine, i * VSIZE);
    }, `Голос обменян с ${LETTERS[d]}${i + 1}`), b) },
    { label: '⇧ Экспорт голоса .syx', fn: () => exportVoice(b, i) },
    { label: '⇩ Заменить из файла .syx', fn: () => pickFile(f => importInto(f, b, i)) },
    { label: '✕ Очистить слот (INIT VOICE)', danger: true, fn: () => mutate([b], () => raw.set(initVoice(), i * VSIZE), 'Слот очищен') },
  ]);
}

function save(bytes, name) {
  const u = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
  const a = document.createElement('a');
  a.href = u; a.download = name; a.click();
  setTimeout(() => URL.revokeObjectURL(u), 1000);
}
const fname = (b, i) => getName(S.banks[b], i).trim().replace(/[^\w.-]+/g, '_') || 'voice';
function exportVoice(b, i) {
  try {
    const patch = FM1Web.readVoices(FM1Web.bankFile(S.banks[b]))[i];
    save(FM1Web.voiceFile(patch), `${fname(b, i)}.syx`);
  } catch { toast('Не удалось экспортировать голос.'); }
}
function exportBank(b) {
  try { save(FM1Web.bankFile(S.banks[b]), `FM1-bank-${LETTERS[b]}.syx`); }
  catch { toast('Не удалось экспортировать банк.'); }
}

function pickFile(fn) {
  const inp = document.createElement('input');
  inp.type = 'file'; inp.accept = '.syx,application/octet-stream';
  inp.onchange = () => { const f = inp.files[0]; if (f) fn(f); };
  inp.click();
}
async function parseSyx(file) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  try {
    const voices = FM1Web.readVoices(bytes);
    return bytes.length === 4104 ? { bank: bytes.slice(6, 4102) } : { voice: FM1Web.packVoice(voices[0]) };
  } catch { return null; }
}
async function importInto(file, b, i) {
  const r = await parseSyx(file);
  if (!r?.voice && !r?.bank) return toast('Это не файл DX7: нужен голос (163 байта) или банк 32 голоса (4104 байта).');
  if (r.bank) return bankPick('Куда записать банк из файла?', d => mutate([d], () => S.banks[d].set(r.bank), `Банк записан в ${LETTERS[d]}`));
  mutate([b], () => S.banks[b].set(r.voice, i * VSIZE), `Голос записан в ${LETTERS[b]}${i + 1}`);
}
function importMenu() {
  const b = S.active === 'left' || !wide() ? S.left : S.right;
  pickFile(f => importInto(f, b, S.sel[S.active === 'right' && wide() ? 1 : 0]));
}

// ---- rendering ----
function paneHtml(side) {
  const b = side === 'left' ? S.left : S.right, raw = S.banks[b], sel = S.sel[side === 'left' ? 0 : 1];
  const tabs = [0, 1, 2, 3].map(k => `<div class="tab${k === b ? ' sel' : ''}" data-tab="${k}" data-side="${side}">${LETTERS[k]}${S.dirty[k] ? '<span class="dot"></span>' : ''}</div>`).join('');
  let rows = '';
  for (let i = 0; i < VOICES; i++) {
    rows += `<div class="v${i === sel && S.active === side ? ' sel' : ''}" data-b="${b}" data-i="${i}" data-side="${side}">
      <span class="n">${String(i + 1).padStart(2, '0')}</span><span class="cat" style="--c:${CATS[algOf(raw, i) % 7]}"></span>
      <span class="nm">${esc(getName(raw, i))}</span><span class="grip">⋮⋮</span></div>`;
  }
  return `<section class="pane" data-side="${side}"><div class="tabs">${tabs}</div><div class="list">${rows}</div></section>`;
}
function render() {
  if (!root || !ready) return;
  const scroll = [...root.querySelectorAll('.list')].map(l => l.scrollTop);
  const two = wide();
  root.querySelector('.panes')?.remove();
  const el = document.createElement('div');
  el.className = 'panes' + (two ? ' two' : '');
  el.innerHTML = paneHtml('left') + (two ? '<div class="swap">⇄</div>' + paneHtml('right') : '');
  root.prepend(el);
  el.querySelectorAll('.list').forEach((l, k) => { l.scrollTop = scroll[k] || 0; });
}

// ---- gestures: tap = select, long press = menu, grip drag = reorder / copy ----
const rowAt = (x, y) => document.elementFromPoint(x, y)?.closest('.v');
function onDown(e) {
  const row = e.target.closest('.v');
  if (!row || e.button) return;
  const b = +row.dataset.b, i = +row.dataset.i, side = row.dataset.side;
  if (e.target.closest('.grip')) return startDrag(e, b, i, side, row);
  const x = e.clientX, y = e.clientY;
  let fired = false;
  const timer = setTimeout(() => { fired = true; select(side, i); voiceMenu(b, i); }, 450);
  const stop = () => { clearTimeout(timer); window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); window.removeEventListener('pointercancel', stop); };
  const move = ev => { if (Math.hypot(ev.clientX - x, ev.clientY - y) > 8) stop(); };
  const up = () => {
    stop();
    if (fired) return;
    const now = Date.now(), twice = lastTap.b === b && lastTap.i === i && now - lastTap.t < 350;
    lastTap = { t: twice ? 0 : now, b, i };
    select(side, i);
    if (twice) hooks.edit?.(b, i);
  };
  window.addEventListener('pointermove', move); window.addEventListener('pointerup', up); window.addEventListener('pointercancel', stop);
}
function select(side, i) { S.active = side; S.sel[side === 'left' ? 0 : 1] = i; render(); }

function startDrag(e, b, i, side, row) {
  e.preventDefault();
  const ghost = document.createElement('div');
  ghost.className = 'v drag ghost';
  ghost.textContent = getName(S.banks[b], i);
  document.body.append(ghost);
  row.classList.add('lift');
  let over = null;
  const place = ev => {
    ghost.style.left = `${ev.clientX - 20}px`; ghost.style.top = `${ev.clientY - 17}px`;
    const list = rowAt(ev.clientX, ev.clientY)?.closest('.list');
    if (list) { const r = list.getBoundingClientRect(); if (ev.clientY < r.top + 40) list.scrollTop -= 12; else if (ev.clientY > r.bottom - 40) list.scrollTop += 12; }
    ghost.style.display = 'none'; const t = rowAt(ev.clientX, ev.clientY); ghost.style.display = '';
    over?.classList.remove('over'); over = t || null; over?.classList.add('over');
  };
  const end = ev => {
    window.removeEventListener('pointermove', place); window.removeEventListener('pointerup', end); window.removeEventListener('pointercancel', end);
    ghost.remove(); row.classList.remove('lift'); over?.classList.remove('over');
    if (ev.type === 'pointercancel' || !over) return;
    const tb = +over.dataset.b, ti = +over.dataset.i;
    if (tb === b && ti === i) return;
    if (tb === b) mutate([b], () => {
      const list = Array.from({ length: VOICES }, (_, k) => voiceAt(S.banks[b], k));
      const [m] = list.splice(i, 1); list.splice(ti, 0, m);
      list.forEach((v, k) => S.banks[b].set(v, k * VSIZE));
    }, `Голос ${i + 1} перенесён на ${ti + 1}`);
    else mutate([tb], () => S.banks[tb].set(voiceAt(S.banks[b], i), ti * VSIZE), `Скопировано в ${LETTERS[tb]}${ti + 1}`);
  };
  window.addEventListener('pointermove', place); window.addEventListener('pointerup', end); window.addEventListener('pointercancel', end);
  place(e);
}

// ---- device transfer ----
function setBusy(text) {
  busy = !!text;
  root.querySelector('.toast.busy')?.remove();
  root.querySelectorAll('.bar button').forEach(b => { b.disabled = busy || (b.dataset.a === 'from' || b.dataset.a === 'to') && !midi.getLibrary(); });
  if (!text) return;
  clearTimeout(toastTimer);
  root.querySelector('.toast')?.remove();
  const t = document.createElement('div');
  t.className = 'toast busy';
  t.textContent = text;
  root.append(t);
}
const needLib = () => {
  const lib = midi.getLibrary();
  if (!lib) toast('Сначала подключите синт: плашка вверху экрана.');
  else if (lib.programs !== 128 || !lib.bankAvailable) toast('Нужна наша прошивка с библиотекой банков A–D.');
  else return lib;
};
function fromSynth() {
  if (busy || !needLib()) return;
  if (S.dirty.some(Boolean)) {
    sheet('Заменить изменения в телефоне?', [
      { label: 'Да, прочитать все банки из синта', danger: true, fn: doRead },
      { label: 'Отмена', fn: () => {} }]);
  } else doRead();
}
async function doRead() {
  const lib = needLib();
  if (!lib) return;
  const before = [0, 1, 2, 3].map(b => ({ b, raw: S.banks[b].slice(), dirty: S.dirty[b] }));
  try {
    for (let b = 0; b < 4; b++) {
      setBusy(`Чтение банка ${LETTERS[b]} из синта… ${b + 1}/4`);
      const raw = await lib.readBank(b);
      S.banks[b] = raw; S.dirty[b] = false; await persist(b);
    }
    S.undo.push(before);
    setBusy(''); render(); toast('Прочитано: банки A–D из синта', true);
  } catch (e) {
    for (const s of before) { S.banks[s.b] = s.raw; S.dirty[s.b] = s.dirty; persist(s.b); }
    setBusy(''); render(); toast(midi.friendly(e));
  }
}
function toSynth() {
  if (busy || !needLib()) return;
  const list = [0, 1, 2, 3].filter(b => S.dirty[b]);
  if (!list.length) return toast('Нет изменённых банков: нечего отправлять.');
  const names = list.map(b => LETTERS[b]).join(', ');
  sheet(`Записать банки ${names} во флеш синта?`, [
    { label: `Да, перезаписать ${names} в синте`, danger: true, fn: () => doWrite(list) },
    { label: 'Отмена', fn: () => {} }]);
}
async function doWrite(list) {
  const lib = needLib();
  if (!lib) return;
  let done = 0;
  try {
    for (const b of list) {
      setBusy(`Запись банка ${LETTERS[b]} в синт… ${done + 1}/${list.length}`);
      await lib.writeBank(b, S.banks[b]);   // uploads, then reads back and compares
      S.dirty[b] = false; await persist(b); done++;
    }
    setBusy(''); render(); toast(`Записано и проверено: ${list.map(b => LETTERS[b]).join(', ')}`);
  } catch (e) {
    setBusy(''); render(); toast(`${midi.friendly(e)} Записано банков: ${done} из ${list.length}.`);
  }
}

// ---- mount ----
export async function mount(el) {
  root = el;
  el.className = 'library';
  el.innerHTML = `<div class="bar"><button type="button" data-a="imp">⇩ ИМПОРТ</button><button type="button" data-a="exp">⇧ ЭКСПОРТ</button>
    <button type="button" data-a="from">⟲ С СИНТА</button><button type="button" class="go" data-a="to">➜ В СИНТ</button></div>`;
  await load();
  render();
  setBusy('');
  midi.onChange(() => setBusy(''));
  el.addEventListener('pointerdown', onDown);
  el.addEventListener('contextmenu', e => { if (e.target.closest('.v')) e.preventDefault(); });
  el.addEventListener('click', e => {
    const tab = e.target.closest('[data-tab]');
    if (tab) { const side = tab.dataset.side; S[side] = +tab.dataset.tab; S.active = side; render(); return; }
    const a = e.target.closest('[data-a]')?.dataset.a;
    if (a === 'imp') importMenu();
    if (a === 'exp') exportBank(S.active === 'right' && wide() ? S.right : S.left);
    if (a === 'from') fromSynth();
    if (a === 'to') toSynth();
  });
  matchMedia('(orientation:landscape)').addEventListener('change', () => { S.active = 'left'; render(); });
}
export const voicePatch = (b, i) => FM1Web.readVoices(FM1Web.bankFile(S.banks[b]))[i];
export function commitVoice(b, i, patch) {
  const raw = FM1Web.packVoice(patch);
  mutate([b], () => S.banks[b].set(raw, i * VSIZE), `Голос сохранён в ${LETTERS[b]}${i + 1}`);
}
export const selection = () => {
  const side = S.active === 'right' && wide() ? 'right' : 'left';
  return [side === 'right' ? S.right : S.left, S.sel[side === 'right' ? 1 : 0]];
};
export const anyDirty = () => S.dirty.some(Boolean);
