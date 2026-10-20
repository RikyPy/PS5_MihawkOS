// Source of the cursor artwork in this folder: original vector drawings of a Windows-style
// pointer scheme (white fill, 1 px black outline, soft pointer shadow, 32x32 at 100 % scale).
// Run `node make-cursors.mjs` here to rewrite every .svg, provenance.json and the generated
// block of css/cursors.css (the --cursor-* variables and the ring keyframes, as data: URLs:
// Chrome resolves a relative url() inside var() against the stylesheet or inline style that
// uses it, so file URLs would break in css/apps/ and in style attributes). The output is
// committed, so the POC itself has no build step. Coordinates sit on half pixels so the 1 px
// outlines land on whole pixels.
import { readFileSync, writeFileSync } from 'node:fs';

const FRAMES = 12; // busy and working rings: one frame per 30 degrees, swapped by css/cursors.css
const OUTLINE = '#000000', FILL = '#ffffff';
const RING_TRACK = '#a9d6fa', RING_ARC = '#0a6ccf', RING_EDGE = '#0b3159';

const svg = (body, defs = '') =>
  `<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32">${defs}${body}</svg>\n`;
const SHADOW = '<defs><filter id="s" x="-25%" y="-25%" width="150%" height="150%"><feDropShadow dx="1" dy="1.5" stdDeviation="0.9" flood-color="#000" flood-opacity="0.32"/></filter></defs>';
const shape = (d, extra = '', join = 'round') => `<path d="${d}" fill="${FILL}" stroke="${OUTLINE}" stroke-width="1" stroke-linejoin="${join}"${extra}/>`;
const lines = (d) => `<path d="${d}" fill="none" stroke="${OUTLINE}" stroke-width="1"/>`;
const shadowed = (body) => svg(`<g filter="url(#s)">${body}</g>`, SHADOW);

const ARROW = 'M1.5 1.5 V17.5 L5 14 L7.75 20.25 L10.25 19.25 L7.75 12.5 H12.5 Z';
const NS = 'M16.5 5 L21 9.5 H17.5 V23.5 H21 L16.5 28 L12 23.5 H15.5 V9.5 H12 Z';

/* A ring of `r` (track width `w`) centred on (cx, cy): light track, dark edges, and a comet
   arc whose tail fades over 150 degrees, turned `turn` degrees clockwise. */
function ring(cx, cy, r, w, turn) {
  const pt = (a, rr) => [cx + rr * Math.sin(a * Math.PI / 180), cy - rr * Math.cos(a * Math.PI / 180)].map((v) => +v.toFixed(1)).join(' ');
  let arcs = '';
  const steps = 8, span = 150;
  for (let i = 0; i < steps; i++) {
    const a0 = turn - span + (span / steps) * i, a1 = a0 + span / steps + 0.6;
    arcs += `<path d="M${pt(a0, r)} A${r} ${r} 0 0 1 ${pt(a1, r)}" fill="none" stroke="${RING_ARC}" stroke-width="${w}" stroke-opacity="${((i + 1) / steps).toFixed(2)}"/>`;
  }
  return `<circle cx="${cx}" cy="${cy}" r="${r}" fill="none" stroke="${RING_TRACK}" stroke-width="${w}"/>${arcs}`
    + `<circle cx="${cx}" cy="${cy}" r="${r + w / 2}" fill="none" stroke="${RING_EDGE}" stroke-width="1"/>`
    + `<circle cx="${cx}" cy="${cy}" r="${r - w / 2}" fill="none" stroke="${RING_EDGE}" stroke-width="1"/>`;
}

/* A small white badge box at the arrow's lower right with a black mark in it (OLE drag cursors). */
const badge = (mark) => `<rect x="14.5" y="18.5" width="11" height="11" rx="1.5" fill="${FILL}" stroke="${OUTLINE}"/>`
  + `<path d="${mark}" fill="none" stroke="${OUTLINE}" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>`;

const files = {
  arrow: { hot: [1, 1], fallback: 'default', svg: shadowed(shape(ARROW)) },
  help: {
    hot: [1, 1], fallback: 'help',
    svg: shadowed(shape(ARROW)
      + `<path d="M14.25 6.25 A2.5 2.5 0 1 1 18 8.4 C17.1 8.9 16.75 9.4 16.75 10.25 V10.75" fill="none" stroke="${OUTLINE}" stroke-width="3.6" stroke-linecap="round" stroke-linejoin="round"/>`
      + `<circle cx="16.75" cy="13.75" r="1.8" fill="${OUTLINE}"/>`
      + `<path d="M14.25 6.25 A2.5 2.5 0 1 1 18 8.4 C17.1 8.9 16.75 9.4 16.75 10.25 V10.75" fill="none" stroke="${FILL}" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>`
      + `<circle cx="16.75" cy="13.75" r="0.8" fill="${FILL}"/>`),
  },
  link: {
    hot: [8, 1], fallback: 'pointer',
    svg: shadowed(shape('M6.5 15 V3.5 A2 2 0 0 1 10.5 3.5 V10.5 A1.5 1.5 0 0 1 13.5 10.5 V11.5 A1.5 1.5 0 0 1 16.5 11.5 V12.5 A1.5 1.5 0 0 1 19.5 12.5 V17.5 Q19.5 20.5 17.5 21.75 V24.5 H9.5 V22 C8.5 21 3.5 16.75 2.6 15.6 A1.5 1.5 0 0 1 4.75 13.5 Z')
      + lines('M10.5 10.5 V13 M13.5 11.5 V13.5 M16.5 12.5 V14.5')),
  },
  text: { hot: [16, 16], fallback: 'text', svg: svg(shape('M13.5 6.5 H19.5 V8.5 H17.5 V23.5 H19.5 V25.5 H13.5 V23.5 H15.5 V8.5 H13.5 Z', '', 'miter')) },
  precision: { hot: [16, 16], fallback: 'crosshair', svg: svg(shape('M15.5 5.5 H17.5 V15.5 H27.5 V17.5 H17.5 V27.5 H15.5 V17.5 H5.5 V15.5 H15.5 Z', '', 'miter')) },
  unavailable: {
    hot: [16, 16], fallback: 'not-allowed',
    svg: shadowed(`<circle cx="16" cy="16" r="8" fill="none" stroke="${OUTLINE}" stroke-width="4"/>`
      + `<path d="M10.6 10.6 L21.4 21.4" stroke="${OUTLINE}" stroke-width="4"/>`
      + `<circle cx="16" cy="16" r="8" fill="none" stroke="${FILL}" stroke-width="2"/>`
      + `<path d="M10.4 10.4 L21.6 21.6" stroke="${FILL}" stroke-width="2"/>`),
  },
  'resize-ns': { hot: [16, 16], fallback: 'ns-resize', svg: shadowed(shape(NS)) },
  'resize-ew': { hot: [16, 16], fallback: 'ew-resize', svg: shadowed(shape(NS, ' transform="rotate(90 16.5 16.5)"')) },
  'resize-nwse': { hot: [16, 16], fallback: 'nwse-resize', svg: shadowed(shape(NS, ' transform="rotate(-45 16.5 16.5)"')) },
  'resize-nesw': { hot: [16, 16], fallback: 'nesw-resize', svg: shadowed(shape(NS, ' transform="rotate(45 16.5 16.5)"')) },
  move: {
    hot: [16, 16], fallback: 'move',
    svg: shadowed(shape('M16.5 4.5 L20.5 8.5 H17.5 V15.5 H24.5 V12.5 L28.5 16.5 L24.5 20.5 V17.5 H17.5 V24.5 H20.5 L16.5 28.5 L12.5 24.5 H15.5 V17.5 H8.5 V20.5 L4.5 16.5 L8.5 12.5 V15.5 H15.5 V8.5 H12.5 Z')),
  },
  split: {
    hot: [16, 16], fallback: 'col-resize',
    svg: shadowed(shape('M14.5 7.5 H18.5 V15.5 H23.5 V12.5 L27.5 16.5 L23.5 20.5 V17.5 H18.5 V25.5 H14.5 V17.5 H9.5 V20.5 L5.5 16.5 L9.5 12.5 V15.5 H14.5 Z') + lines('M16.5 8 V25')),
  },
  'drag-move': { hot: [1, 1], fallback: 'default', svg: shadowed(shape(ARROW) + badge('M17.5 24 H22.5 M20.5 21.75 L22.75 24 L20.5 26.25')) },
  'drag-copy': { hot: [1, 1], fallback: 'copy', svg: shadowed(shape(ARROW) + badge('M20 21 V27 M17 24 H23')) },
  'drag-link': { hot: [1, 1], fallback: 'alias', svg: shadowed(shape(ARROW) + badge('M17.5 27 C17.5 23.5 19 22 22.5 22 M20.25 20 L22.75 22 L20.25 24.25')) },
};
for (let f = 0; f < FRAMES; f++) {
  const n = String(f).padStart(2, '0'), turn = f * (360 / FRAMES);
  files[`busy-${n}`] = { hot: [16, 16], fallback: 'wait', frame: f, svg: shadowed(ring(16, 16, 8.5, 4, turn)) };
  files[`working-${n}`] = { hot: [1, 1], fallback: 'progress', frame: f, svg: shadowed(shape(ARROW) + ring(22.5, 22, 4.25, 2.5, turn)) };
}

const out = {
  description: 'Mouse pointers of the Windows 11 shell POC: original artwork in the shape language of the Windows pointer scheme (white fill, 1 px black outline, pointer shadow). No Microsoft cursor file was copied, traced or converted.',
  author: 'Authored for this POC (PS5_Proton-design), 2026-10-15',
  license: 'Same terms as the rest of this prototype',
  source: 'make-cursors.mjs (vector source; rerun with node to regenerate every file here)',
  size: '32x32 CSS px, scaled by the browser for the display scale like Windows scales its cursors',
  animation: `busy-* and working-* are ${FRAMES} frames of one ring turning 30 degrees each; css/cursors.css swaps them with a stepped CSS animation (CSS cursors cannot animate themselves)`,
  files: {},
};
const VARS = { arrow: 'arrow', help: 'help', 'working-00': 'working', 'busy-00': 'busy', precision: 'precision', text: 'text',
  unavailable: 'unavailable', 'resize-ns': 'ns', 'resize-ew': 'ew', 'resize-nwse': 'nwse', 'resize-nesw': 'nesw', move: 'move',
  link: 'link', split: 'split', 'drag-move': 'drag-move', 'drag-copy': 'drag-copy', 'drag-link': 'drag-link' };
const value = (name) => {
  const c = files[name], data = c.svg.trim().replace(/"/g, "'").replace(/%/g, '%25').replace(/#/g, '%23').replace(/</g, '%3C').replace(/>/g, '%3E');
  return `url("data:image/svg+xml,${data}") ${c.hot.join(' ')}, ${c.fallback}`;
};
const keyframes = (anim, prop, prefix) => `@keyframes ${anim} {\n${Array.from({ length: FRAMES }, (_, f) =>
  `  ${+(f * 100 / FRAMES).toFixed(3)}% { ${prop}: ${value(`${prefix}-${String(f).padStart(2, '0')}`)}; }`).join('\n')}\n}`;
const block = `/* BEGIN generated by assets/cursors/make-cursors.mjs: do not edit by hand */\n:root {\n${Object.entries(VARS)
  .map(([file, v]) => `  --cursor-${v}: ${value(file)};`).join('\n')}\n}\n${keyframes('cursor-busy', '--cursor-busy', 'busy')}\n`
  + `${keyframes('cursor-working', '--cursor-arrow', 'working')}\n/* END generated */`;

for (const [name, c] of Object.entries(files)) {
  writeFileSync(new URL(`${name}.svg`, import.meta.url), c.svg);
  out.files[`${name}.svg`] = { hotspot: c.hot, fallback: c.fallback, ...(VARS[name] && { variable: `--cursor-${VARS[name]}` }), ...(c.frame !== undefined && { frame: c.frame }) };
}
writeFileSync(new URL('provenance.json', import.meta.url), `${JSON.stringify(out, null, 2)}\n`);
const cssUrl = new URL('../../css/cursors.css', import.meta.url), css = readFileSync(cssUrl, 'utf8');
const marked = /\/\* BEGIN generated[\s\S]*?\/\* END generated \*\//;
if (!marked.test(css)) throw new Error('css/cursors.css has no generated block markers');
writeFileSync(cssUrl, css.replace(marked, () => block));
console.log(`wrote ${Object.keys(files).length} cursors and the css/cursors.css block (${block.length} bytes)`);
