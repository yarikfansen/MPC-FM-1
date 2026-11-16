// Network-first app shell (cache is the offline fallback); bump VERSION when any shell file changes.
const VERSION = 'fm1-phone-5';
const SHELL = ['./', 'index.html', 'app.css', 'manifest.webmanifest', 'icon-192.png', 'icon-512.png',
  'vendor/WebUpdate.js', 'js/app.js', 'js/midi.js', 'js/store.js', 'js/library.js', 'js/editor.js', 'js/keyboard.js', 'js/engine.js', 'js/out.js', 'js/algorithms.js', 'vendor/simulator.wasm'];
self.addEventListener('install', e => e.waitUntil(caches.open(VERSION).then(c => c.addAll(SHELL)).then(() => self.skipWaiting())));
self.addEventListener('activate', e => e.waitUntil(
  caches.keys().then(keys => Promise.all(keys.filter(k => k !== VERSION).map(k => caches.delete(k)))).then(() => self.clients.claim())));
self.addEventListener('fetch', e => {
  if (e.request.method !== 'GET') return;
  e.respondWith(fetch(e.request).then(res => {
    if (res.ok && new URL(e.request.url).origin === location.origin) { const copy = res.clone(); caches.open(VERSION).then(c => c.put(e.request, copy)); }
    return res;
  }).catch(() => caches.match(e.request)));
});
