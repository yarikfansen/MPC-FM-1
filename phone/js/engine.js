// Built-in sound: the firmware's own audio code (vendor/simulator.wasm) in an AudioWorklet.
// It takes MIDI/SysEx bytes exactly like the synth does (USB source, DX7 on MIDI channel of track 1).
const WORKLET = `
class FM1 extends AudioWorkletProcessor {
  constructor(o) {
    super();
    const c = this.c = new WebAssembly.Instance(new WebAssembly.Module(o.processorOptions.wasm), {}).exports;
    c._initialize(); c.sim_boot(1);
    this.bf = c.sim_block_frames(); this.pos = this.bf;
    this.port.onmessage = e => {
      const d = e.data; if (!d.length || d.length > 1024) return;
      new Uint8Array(c.memory.buffer, c.sim_file(), d.length).set(d); c.sim_midi_from(1, d.length);
    };
    this.port.postMessage('ready');
  }
  process(_, outputs) {
    const l = outputs[0][0], r = outputs[0][1], c = this.c;
    for (let k = 0; k < l.length; k++) {
      if (this.pos === this.bf) { this.buf = new Float32Array(c.memory.buffer, c.sim_render(), this.bf * 2); this.pos = 0; }
      l[k] = this.buf[this.pos * 2]; if (r) r[k] = this.buf[this.pos * 2 + 1]; this.pos++;
    }
    return true;
  }
}
registerProcessor('fm1-engine', FM1);`;

let ctx = null, node = null, starting = null, volume = null, meter = null, meterData = null;
export const ready = () => !!node;

// Must be called from a user gesture (tap) the first time.
export function start() {
  if (node) { ctx.state !== 'running' && ctx.resume(); return Promise.resolve(); }
  return starting ||= (async () => {
    const wasm = await (await fetch(new URL('../vendor/simulator.wasm', import.meta.url))).arrayBuffer();
    ctx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
    await ctx.resume();
    await ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([WORKLET], { type: 'text/javascript' })));
    const n = new AudioWorkletNode(ctx, 'fm1-engine', { numberOfInputs: 0, outputChannelCount: [2], processorOptions: { wasm } });
    await new Promise((res, rej) => { n.onprocessorerror = () => rej(new Error('Звуковой движок не запустился')); n.port.onmessage = () => res(); });
    volume = ctx.createGain(); volume.gain.value = 0.8;
    meter = ctx.createAnalyser(); meter.fftSize = 512; meterData = new Float32Array(512);
    n.connect(volume).connect(meter).connect(ctx.destination);
    node = n;
  })().finally(() => { starting = null; });
}
// Peak of the last ~12 ms of output (0..1); handy for a level meter and for tests.
export function level() { if (!meter) return 0; meter.getFloatTimeDomainData(meterData); return meterData.reduce((m, v) => Math.max(m, Math.abs(v)), 0); }
export const send = bytes => { if (node) node.port.postMessage(Uint8Array.from(bytes)); };
