// Where sound goes: the connected FM-1 (USB MIDI) or the built-in engine. The channel is the
// MIDI input channel of the synth track you want to hear/edit (default 1 = track 1).
import * as midi from './midi.js';
import * as engine from './engine.js';

const simMode = new URLSearchParams(location.search).has('sim');
let channel = 0, pending = null, timer = 0, synced = null;
try { channel = Math.min(15, Math.max(0, +localStorage.getItem('fm1.ch') || 0)); } catch {}
export const getChannel = () => channel;
export const setChannel = c => { channel = c & 15; synced = null; try { localStorage.setItem('fm1.ch', String(channel)); } catch {} };
export const toSynth = () => { const l = midi.getLibrary(); return !!l && !simMode && !l.link?.lost; };

function raw(bytes) {
  if (toSynth()) { try { midi.getLibrary().link.send(Uint8Array.from(bytes)); } catch {} }
  else engine.send(bytes);
}
// Voice (155-byte VCED) as a standard DX7 single-voice dump; RAM only, never flash.
function pushVoice(patch) { raw(FM1Web.voiceFile(patch, channel)); synced = patch.slice(); }
// Send at most ~16 per second while knobs move.
export function voice(patch) {
  pending = patch.slice();
  if (timer) return;
  timer = setTimeout(() => { timer = 0; if (pending) { pushVoice(pending); pending = null; } }, 60);
}
const down = new Set();   // notes still held; a release that arrives while the engine boots cancels the note
export async function noteOn(note, velocity = 100, patch) {
  down.add(note);
  if (!toSynth()) await engine.start();
  if (!down.has(note)) return;
  if (patch && (!synced || synced.some((v, i) => v !== patch[i]))) pushVoice(patch);
  raw([0x90 | channel, note, velocity]);
}
export const noteOff = note => { down.delete(note); raw([0x80 | channel, note, 0]); };
export const panic = () => raw([0xb0 | channel, 123, 0]);
export const bend = v14 => raw([0xe0 | channel, v14 & 127, (v14 >> 7) & 127]);
export const cc = (n, v) => raw([0xb0 | channel, n, v]);
// Create the audio engine early (call from a tap) so the first note is not late.
export const warm = () => { if (!toSynth()) engine.start().catch(() => {}); };
