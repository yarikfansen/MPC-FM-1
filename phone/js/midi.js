// WebMIDI + SysEx connection to the FM-1. Only FM1Web.Library is used (commands
// 0-6, 21, 22: identify, read/write DX7 banks); firmware update code is never called.
const simMode = new URLSearchParams(location.search).has('sim');
let access = null, library = null;
const listeners = new Set();
export const onChange = fn => listeners.add(fn);
const emit = () => listeners.forEach(fn => fn(state()));

export const state = () => ({ supported: !!navigator.requestMIDIAccess, access: !!access, library, });
export const getLibrary = () => library;

export async function connect() {
  if (!simMode && !navigator.requestMIDIAccess) throw new Error('В этом браузере нет WebMIDI (нужен Chrome на Android).');
  if (!access) {
    // ?sim = dev only: talk to the firmware C code compiled to wasm instead of USB.
    access = simMode ? await (await import('./simmidi.js')).createSimAccess() : await navigator.requestMIDIAccess({ sysex: true });
    access.onstatechange = () => { if (library && !library.link?.lost) return; library = null; emit(); };
  }
  const lib = new FM1Web.Library(access);
  let info;
  try { info = await lib.connect(); }
  catch (e) { throw new Error(friendly(e)); }
  library = Object.assign(lib, { info });
  emit();
  return library;
}

export async function disconnect() {
  if (library) await library.disconnect().catch(() => {});
  library = null; emit();
}

// WebUpdate.js messages are Chinese; map the likely ones to Russian.
export function friendly(e) {
  const m = e.message || String(e);
  if (/读回不一致/.test(m)) return 'Записано, но проверка чтением не совпала. Прочитайте банк заново.';
  if (/已断开/.test(m)) return 'USB отключён, операция не завершена.';
  if (/设备未响应/.test(m)) return 'Синт не отвечает. Проверьте кабель.';
  if (/内存不足/.test(m)) return 'В синте не хватило памяти.';
  if (/固件/.test(m)) return 'Нужна наша прошивка с 128-голосной библиотекой.';
  if (/未找到/.test(m)) return 'FM-1 по USB не найден. Проверьте кабель и подключите синт до нажатия.';
  if (/只连接一台/.test(m)) return 'Подключено несколько FM-1, оставьте один.';
  if (/未支持|设备未响应/.test(m)) return 'Синт не отвечает: нужна наша прошивка с библиотекой проектов.';
  if (e.name === 'SecurityError' || /permission/i.test(m)) return 'Нет разрешения на SysEx. Разрешите доступ к MIDI-устройствам.';
  return /[一-鿿]/.test(m) ? 'Ошибка обмена с синтом.' : m;
}
