// SPDX-License-Identifier: GPL-3.0-or-later
// Where the approved prototype places each state's windows (prototypes/windows11-shell, ?fresh&<theme>&open=<state>),
// for the shell scorecard (tests/shell-scorecard-probe.c places the native windows there): every visible
// section.window's app, title and border box at 1920 x 1080. Needs a headless Chromium with --remote-debugging-port
// (tools/pw_prototype_capture.mjs) and the prototype served at PROTOTYPE_URL.
//   node tools/measure-prototype-windows.mjs explorer notepad ... > out.json
import { connect, page } from './pw_prototype_capture.mjs';

const base = process.env.PROTOTYPE_URL || 'http://127.0.0.1:8778/prototypes/windows11-shell/index.html';
const session = await connect(process.env.CDP_PORT || '9333');
const out = {};
for (const state of process.argv.slice(2)) {
  const p = await page(session, `${base}?fresh&light&open=${state}`);
  out[state] = await p.evaluate(`[...document.querySelectorAll('section.window')].filter((w) => w.checkVisibility())
    .map((w) => { const r = w.getBoundingClientRect(); return { app: w.dataset.app, title: w.getAttribute('aria-label'),
      rect: [r.left, r.top, r.width, r.height] }; })`);
  await p.close();
}
console.log(JSON.stringify(out, null, 1));
session.close();
