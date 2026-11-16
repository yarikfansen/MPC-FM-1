import * as midi from './midi.js';
import * as library from './library.js';
import * as editor from './editor.js';
import * as keyboard from './keyboard.js';
import * as out from './out.js';

const $ = id => document.getElementById(id);
const conn = $('conn'), title = $('title');
const titles = { library: 'БАНКИ DX7', editor: 'РЕДАКТОР', keys: 'КЛАВИАТУРА' };
let current = 'library', busy = false;

function pill() {
  const lib = midi.getLibrary();
  conn.textContent = busy ? 'связь…' : lib ? 'FM-1 ✓' : 'нет синта';
  conn.className = 'pill' + (lib ? ' on' : busy ? ' warn' : '');
}

function show(name) {
  current = name;
  title.textContent = titles[name];
  document.querySelectorAll('#nav button').forEach(b => b.classList.toggle('sel', b.dataset.s === name));
  for (const s of ['library', 'editor', 'keys']) $('s-' + s).hidden = s !== name;
  if (name === 'keys') { out.warm(); keyboard.render(); }
  else { keyboard.release(); $('app').classList.remove('kbfull'); }
  pill();
}

conn.addEventListener('click', async () => {
  if (busy) return;
  if (midi.getLibrary()) return midi.disconnect();
  busy = true; pill();
  try { await midi.connect(); }
  catch (e) { alert(e.message); }
  busy = false; pill();
});
$('nav').addEventListener('click', e => {
  const s = e.target.closest('button')?.dataset.s;
  if (s === 'editor' && !editor.current()) editor.open(...library.selection());
  if (s) show(s);
});
midi.onChange(pill);
library.mount($('s-library'));
editor.mount($('s-editor'));
keyboard.mount($('s-keys'), () => {
  const on = $('app').classList.toggle('kbfull');
  if (on) { document.documentElement.requestFullscreen?.().catch(() => {}); screen.orientation?.lock?.('landscape').catch(() => {}); }
  else if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
});
library.hooks.edit = (b, i) => { editor.open(b, i); show('editor'); };
show('library');
if ('serviceWorker' in navigator && location.protocol !== 'file:') navigator.serviceWorker.register('sw.js').catch(() => {});
