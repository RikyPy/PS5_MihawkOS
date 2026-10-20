// SPDX-License-Identifier: GPL-3.0-or-later
// Shared by tools/measure-prototype-*.mjs: a Chrome DevTools session on a headless Chromium (started with
// --remote-debugging-port --window-size=1920,1080 --force-device-scale-factor=1) and its screenshots, decoded.
import { inflateSync } from 'node:zlib';

export async function connect(port = '9333') {
  const version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
  const ws = new WebSocket(version.webSocketDebuggerUrl);
  await new Promise((r) => ws.addEventListener('open', r));
  let id = 0; const waiting = new Map();
  ws.addEventListener('message', (e) => { const m = JSON.parse(e.data); if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); } });
  const send = (method, params = {}, sessionId) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params, sessionId })); });
  return { browser: version.Browser, send, close: () => ws.close() };
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* A 1920 x 1080 page at device scale 1, navigated and settled; evaluate() returns values, shot() a decoded image. */
export async function page(session, url) {
  const { send } = session;
  const { result: { targetId } } = await send('Target.createTarget', { url: 'about:blank' });
  const { result: { sessionId: s } } = await send('Target.attachToTarget', { targetId, flatten: true });
  await send('Emulation.setDeviceMetricsOverride', { width: 1920, height: 1080, deviceScaleFactor: 1, mobile: false }, s);
  await send('Page.enable', {}, s);
  await send('Page.navigate', { url }, s);
  await sleep(3500);
  // The prototype simulates the display's brightness with a dark veil over everything (2.4% at its default); a
  // screen has none, so every measurement is taken at 100.
  await send('Runtime.evaluate', { expression: "window.Shell && (Shell.set('brightness', 100), Shell.set('nightLight', false), Shell.set('animations', false))" }, s);
  await sleep(300);
  // PROTOTYPE_SETUP: script that puts the page in the state to measure (the console's: no notifications, Wi-Fi off).
  if (process.env.PROTOTYPE_SETUP) { await send('Runtime.evaluate', { expression: process.env.PROTOTYPE_SETUP, awaitPromise: true }, s); await sleep(500); }
  return {
    evaluate: async (expression) => (await send('Runtime.evaluate', { returnByValue: true, awaitPromise: true, expression }, s)).result.result?.value,
    mouse: (type, x, y) => send('Input.dispatchMouseEvent', { type, x, y, button: type === 'mouseMoved' ? 'none' : 'left', buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1 }, s),
    shot: async () => { await sleep(400); return decode(Buffer.from((await send('Page.captureScreenshot', { format: 'png' }, s)).result.data, 'base64')); },
    // clip {x, y, width, height}: only that part of the page (far less to encode and decode than 1920 x 1080).
    png: async (clip) => { await sleep(400); return Buffer.from((await send('Page.captureScreenshot', { format: 'png', ...(clip ? { clip: { ...clip, scale: 1 } } : {}) }, s)).result.data, 'base64'); },
    close: () => send('Target.closeTarget', { targetId }),
  };
}

/* RGB of a PNG screenshot (8-bit RGBA or RGB, non-interlaced, as Chromium writes them). */
export function decode(png) {
  let pos = 8, width = 0, height = 0, channels = 0; const parts = [];
  while (pos < png.length) {
    const length = png.readUInt32BE(pos), type = png.toString('latin1', pos + 4, pos + 8), data = png.subarray(pos + 8, pos + 8 + length);
    if (type === 'IHDR') { width = data.readUInt32BE(0); height = data.readUInt32BE(4); channels = { 2: 3, 6: 4 }[data[9]]; if (data[8] !== 8 || !channels || data[12]) throw new Error('unexpected PNG'); }
    if (type === 'IDAT') parts.push(data);
    pos += 12 + length;
  }
  const raw = inflateSync(Buffer.concat(parts)), stride = width * channels, pixels = Buffer.alloc(height * stride);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)], line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const a = x >= channels ? pixels[y * stride + x - channels] : 0, b = y ? pixels[(y - 1) * stride + x] : 0;
      const c = x >= channels && y ? pixels[(y - 1) * stride + x - channels] : 0;
      const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
      const predictor = [0, a, b, (a + b) >> 1, pa <= pb && pa <= pc ? a : pb <= pc ? b : c][filter];
      pixels[y * stride + x] = (line[x] + predictor) & 255;
    }
  }
  return { width, at: (x, y) => [0, 1, 2].map((k) => pixels[y * stride + x * channels + k]) };
}
