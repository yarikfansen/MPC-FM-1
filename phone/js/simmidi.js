// Dev-only fake WebMIDI access: SysEx goes into the real firmware C code compiled
// to wasm (web/engine/Simulator.c), replies come back through the capture queue.
const USB = 1;
export async function createSimAccess(url = new URL('../vendor/simulator.wasm', import.meta.url)) {
  const res = await fetch(url);
  if (!res.ok) throw new Error('Нет vendor/simulator.wasm (режим ?sim)');
  const c = new WebAssembly.Instance(await WebAssembly.compile(await res.arrayBuffer()), {}).exports;
  c._initialize(); c.sim_boot(1); c.sim_midi_capture(1);
  const listeners = new Set();
  const pump = () => {
    for (let i = 0; i < 12; i++) {
      c.sim_render();
      for (let r; (r = c.sim_midi_output());) {
        if ((r >> 8) !== USB) continue;
        const data = new Uint8Array(c.memory.buffer, c.sim_file(), r & 255).slice();
        setTimeout(() => listeners.forEach(fn => fn({ data })), 0);
      }
    }
  };
  const base = { name: 'FM-1 Simulator', manufacturer: 'sim', state: 'connected', connection: 'open', id: 'sim' };
  const input = { ...base, type: 'input', open: async () => input, close: async () => input,
    addEventListener: (t, fn) => { if (t === 'midimessage') listeners.add(fn); },
    removeEventListener: (t, fn) => listeners.delete(fn) };
  const output = { ...base, type: 'output', open: async () => output, close: async () => output,
    send(bytes) {
      new Uint8Array(c.memory.buffer, c.sim_file(), bytes.length).set(bytes);
      if (c.sim_midi_from(USB, bytes.length) < 0) return;
      pump();
    } };
  return { inputs: new Map([['sim', input]]), outputs: new Map([['sim', output]]), onstatechange: null, sim: c };
}
