// Local copy of the four DX7 banks (4096 raw bytes = 32 x 128 B VMEM each) in IndexedDB.
// Works without the synth; `dirty` marks banks not yet sent to the device.
const DB = 'fm1-phone', STORE = 'banks';
const open = () => new Promise((res, rej) => {
  const r = indexedDB.open(DB, 1);
  r.onupgradeneeded = () => r.result.createObjectStore(STORE, { keyPath: 'index' });
  r.onsuccess = () => res(r.result);
  r.onerror = () => rej(r.error);
});
const tx = async (mode, fn) => {
  const db = await open();
  return new Promise((res, rej) => {
    const t = db.transaction(STORE, mode), out = fn(t.objectStore(STORE));
    t.oncomplete = () => { db.close(); res(out.result); };
    t.onerror = t.onabort = () => { db.close(); rej(t.error); };
  });
};
export const getBank = index => tx('readonly', s => s.get(index));
export const putBank = (index, raw, dirty) => {
  if (!(raw instanceof Uint8Array) || raw.length !== 4096) throw new Error('Банк должен быть 4096 байт');
  return tx('readwrite', s => s.put({ index, raw, dirty: !!dirty, at: Date.now() }));
};
export const allBanks = () => tx('readonly', s => s.getAll());
// Voice name = VMEM bytes 118..127 of each 128-byte voice.
export const voiceName = (raw, i) => String.fromCharCode(...raw.slice(i * 128 + 118, i * 128 + 128)).replace(/[^\x20-\x7e]/g, '?').trimEnd();
