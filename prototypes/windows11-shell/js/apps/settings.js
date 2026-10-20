/* Settings (Win+I): the Windows 11 23H2 Settings app. Owns its window content: the title bar
   row (back, hamburger, icon, title), the navigation pane (account header, "Find a setting"
   with suggestions, NavigationView with the sliding selection pill, overlay pane below
   1008 px), the breadcrumb page title, settings cards and expanders, every page, the custom
   colour dialog and the Rename dialog. Also Shell.settingsIndex (for Search) and automatic
   accent colour from the wallpaper (setting accentAuto).
   API: app 'settings' (pinned, startPinned), open({page}); page ids are paths such as
   'personalization/colors'; a bare last segment ('taskbar', 'display') also resolves.
   Settings keys owned: accentOnStart, accentOnTitleBars, nightLightStrength. Bound here, owned
   elsewhere: taskbarAutoHide, taskbarBadges, taskbarFlashing, taskbarPeek (taskbar; fallbacks
   false/true/true/false), clipboardHistory (popups; fallback true).
   Store keys owned: settings-local (toggles no other surface reads), settings-recent-accents.
   Search entries whose page is 'app:<id>[/<page>]' launch that app instead (Task Manager).
   Captures: ?open=settings opens Personalization > Colors; &settingsPage=<id> picks another;
   ?open=settings-taskbar (Taskbar behaviors open) and ?open=settings-clipboard.
   Device, network and app names are synthetic demonstration data. */
(() => {
  'use strict';

  const Shell = window.Shell;
  const S = () => Shell.settings;

  /* h('div', {class, text, onclick, ...attrs}, ...children) */
  function h(tag, props = {}, ...kids) {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(props)) {
      if (v == null || v === false) continue;
      if (k === 'class') n.className = v;
      else if (k === 'text') n.textContent = v;
      else if (k === 'style') n.style.cssText = v;
      else if (k.startsWith('on')) n.addEventListener(k.slice(2), v);
      else n.setAttribute(k, v === true ? '' : v);
    }
    n.append(...kids.flat(Infinity).filter((c) => c != null && c !== false));
    return n;
  }
  const icon = (name, size = 16) => Shell.icon(name.includes('/') ? name : `fluent/${name}`, { size });

  /* ---------- colours ---------- */
  /* Windows' 48 accent colours, in the order of the Colors page grid, with their tooltips. */
  const ACCENTS = [
    ['#ffb900', 'Yellow gold'], ['#ff8c00', 'Gold'], ['#f7630c', 'Orange bright'], ['#ca5010', 'Orange dark'],
    ['#da3b01', 'Rust'], ['#ef6950', 'Pale rust'], ['#d13438', 'Brick red'], ['#ff4343', 'Mod red'],
    ['#e74856', 'Pale red'], ['#e81123', 'Red'], ['#ea005e', 'Rose bright'], ['#c30052', 'Rose'],
    ['#e3008c', 'Plum light'], ['#bf0077', 'Plum'], ['#c239b3', 'Orchid light'], ['#9a0089', 'Orchid'],
    ['#0078d4', 'Default blue'], ['#0063b1', 'Navy blue'], ['#8e8cd8', 'Purple shadow'], ['#6b69d6', 'Purple shadow dark'],
    ['#8764b8', 'Iris pastel'], ['#744da9', 'Iris spring'], ['#b146c2', 'Violet red light'], ['#881798', 'Violet red'],
    ['#0099bc', 'Cool blue bright'], ['#2d7d9a', 'Cool blue'], ['#00b7c3', 'Seafoam'], ['#038387', 'Seafoam teal'],
    ['#00b294', 'Mint light'], ['#018574', 'Mint dark'], ['#00cc6a', 'Turf green'], ['#10893e', 'Sport green'],
    ['#7a7574', 'Gray'], ['#5d5a58', 'Gray brown'], ['#68768a', 'Steel blue'], ['#515c6b', 'Metal blue'],
    ['#567c73', 'Pale moss'], ['#486860', 'Moss'], ['#498205', 'Meadow green'], ['#107c10', 'Green'],
    ['#767676', 'Overcast'], ['#4c4a48', 'Storm'], ['#69797e', 'Blue gray'], ['#4a5459', 'Gray dark'],
    ['#647c64', 'Liddy green'], ['#525e54', 'Sage'], ['#847545', 'Camouflage desert'], ['#7e735f', 'Camouflage'],
  ];
  /* Background > Solid color: Windows' 24 background colours. */
  const SOLIDS = [
    '#ff8c00', '#e81123', '#d13438', '#c30052', '#bf0077', '#9a0089', '#881798', '#744da9',
    '#10893e', '#107c10', '#018574', '#2d7d9a', '#0063b1', '#6b69d6', '#8e8cd8', '#8764b8',
    '#038387', '#486860', '#525e54', '#7e735f', '#4c4a48', '#515c6b', '#4a5459', '#000000',
  ];

  const hexToRgb = (hex) => { const n = parseInt(hex.slice(1), 16); return [(n >> 16) & 255, (n >> 8) & 255, n & 255]; };
  const rgbToHex = (r, g, b) => `#${[r, g, b].map((v) => Math.round(v).toString(16).padStart(2, '0')).join('')}`;
  const hsvToHex = (hu, s, v) => {
    const f = (n) => { const k = (n + hu / 60) % 6; return (v - v * s * Math.max(0, Math.min(k, 4 - k, 1))) * 255; };
    return rgbToHex(f(5), f(3), f(1));
  };
  const hexToHsv = (hex) => {
    const [r, g, b] = hexToRgb(hex).map((x) => x / 255);
    const max = Math.max(r, g, b), d = max - Math.min(r, g, b);
    let hu = 0;
    if (d) hu = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
    return [(hu * 60 + 360) % 360, max ? d / max : 0, max];
  };
  const isLight = (hex) => { const [r, g, b] = hexToRgb(hex); return 0.299 * r + 0.587 * g + 0.114 * b > 150; };
  const nearestAccent = ([r, g, b]) => ACCENTS.reduce((best, [hex]) => {
    const [R, G, B] = hexToRgb(hex), d = (R - r) ** 2 + (G - g) ** 2 + (B - b) ** 2;
    return d < best.d ? { hex, d } : best;
  }, { hex: '#0078d4', d: Infinity }).hex;

  /* Solid colours are wallpapers too: register each so Shell.set('wallpaper', id) resolves,
     including a saved custom one (its id carries its hex). Runs before Shell.boot(). */
  function solidId(hex) {
    hex = hex.toLowerCase();
    const hit = Shell.wallpapers.find((w) => w.solid && w.css.toLowerCase() === hex);
    if (hit) return hit.id;
    const id = `solid-${hex.slice(1)}`;
    Shell.wallpapers.push({ id, name: 'Solid color', css: hex, solid: true });
    return id;
  }
  SOLIDS.forEach(solidId);
  if (/^solid-[0-9a-f]{6}$/.test(S().wallpaper || '')) solidId(`#${S().wallpaper.slice(6)}`);

  const wallpaper = (id) => Shell.wallpapers.find((w) => w.id === id) || Shell.wallpapers[0];
  /* A wallpaper as a CSS background; images keep their gradient underneath as the fallback. */
  function wpCss(wp) {
    if (!wp.image) return wp.css;
    const dim = wp.dim ? `linear-gradient(rgba(0, 8, 24, ${wp.dim}), rgba(0, 8, 24, ${wp.dim})), ` : '';
    return `${dim}url("${new URL(wp.image, location.href).href}") center / cover no-repeat, ${wp.css}`;
  }

  /* Accent: Automatic picks the wallpaper's most vivid colour and snaps it to Windows' grid. */
  function autoAccent() {
    const wp = wallpaper(S().wallpaper);
    const pick = (cols) => {
      let best = null, score = -1;
      for (const c of cols) {
        const [, s, v] = hexToHsv(rgbToHex(...c));
        const sc = s * Math.min(v, 0.85);
        if (sc > score) { score = sc; best = c; }
      }
      const hex = best && score > 0.12 ? nearestAccent(best) : '#0078d4';
      if (hex !== S().accent) Shell.set('accent', hex);
    };
    const fromCss = () => pick((wp.css.match(/#[0-9a-f]{6}/gi) || []).map(hexToRgb));
    if (!wp.image) { fromCss(); return; }
    const img = new Image();
    img.onload = () => {
      const c = document.createElement('canvas');
      c.width = c.height = 16;
      const ctx = c.getContext('2d');
      ctx.drawImage(img, 0, 0, 16, 16);
      const d = ctx.getImageData(0, 0, 16, 16).data, cols = [];
      for (let i = 0; i < d.length; i += 4) cols.push([d[i], d[i + 1], d[i + 2]]);
      pick(cols);
    };
    img.onerror = fromCss;
    img.src = wp.image;
  }
  Shell.on('wallpaper', () => { if (S().accentAuto) autoAccent(); });

  const RECENT_KEY = 'settings-recent-accents';
  let recentList = Shell.store.get(RECENT_KEY, ['#0078d4', '#e74856', '#038387', '#8764b8', '#ff8c00']);
  const recentAccents = () => recentList;
  function chooseAccent(hex) {
    hex = hex.toLowerCase();
    recentList = [hex, ...recentList.filter((c) => c !== hex)].slice(0, 5);
    Shell.store.set(RECENT_KEY, recentList);
    if (S().accentAuto) Shell.set('accentAuto', false);
    Shell.set('accent', hex);
  }

  /* ---------- state bindings ---------- */
  /* A binding is {get, set, key?}: `key` for Shell.settings (live-synced across surfaces),
     plain store entries for toggles no other surface reads. */
  let local = Shell.store.get('settings-local', {});
  const bind = (key, fallback) => ({ key, get: () => S()[key] ?? fallback, set: (v) => Shell.set(key, v) });
  const loc = (k, def) => ({ get: () => (k in local ? local[k] : def), set: (v) => { local = { ...local, [k]: v }; Shell.store.set('settings-local', local); } });

  /* Controls on the visible page re-read their value when its setting changes elsewhere. */
  let watchers = new Map();
  let pageCleanups = [];
  const watch = (key, fn) => { if (!watchers.has(key)) watchers.set(key, new Set()); watchers.get(key).add(fn); };
  const cleanupPage = () => { pageCleanups.forEach((fn) => fn()); pageCleanups = []; watchers = new Map(); };

  /* ---------- controls (ui module classes; behaviour wired here) ---------- */
  function toggleCtl(b, label, { disabled } = {}) {
    const input = h('input', { type: 'checkbox', role: 'switch', 'aria-label': label });
    const text = h('span', { class: 'toggle-text' });
    const sync = () => {
      input.checked = !!b.get();
      text.textContent = input.checked ? 'On' : 'Off';
      const off = typeof disabled === 'function' ? disabled() : !!disabled;
      input.disabled = off;
      el.classList.toggle('disabled', off);
    };
    const el = h('label', { class: 'toggle stg-toggle', onclick: (e) => e.stopPropagation() },
      text, input, h('span', { class: 'toggle-track' }, h('span', { class: 'toggle-thumb' })));
    input.addEventListener('change', () => { b.set(input.checked); sync(); });
    sync();
    if (b.key) watch(b.key, sync);
    if (typeof disabled === 'function') watch('theme', sync);
    return el;
  }

  function checkRow(b, label, { disabled } = {}) {
    const input = h('input', { type: 'checkbox', class: 'checkbox', disabled });
    input.checked = !!b.get();
    input.addEventListener('change', () => b.set(input.checked));
    if (b.key) watch(b.key, () => { input.checked = !!b.get(); });
    return h('label', { class: `stg-check${disabled ? ' disabled' : ''}` }, input, h('span', { text: label }));
  }

  /* options: [[value, label], ...]. Uses the ui module's ComboBox when it is there. */
  function comboCtl(b, options, label, { disabled } = {}) {
    const idx = () => Math.max(0, options.findIndex(([v]) => v === b.get()));
    if (Shell.ui?.combo) {
      const el = h('button', { class: 'stg-combo', type: 'button', 'aria-label': label, disabled, onclick: (e) => e.stopPropagation() });
      const api = Shell.ui.combo(el, {
        options: options.map(([value, l]) => ({ value, label: l })),
        value: options[idx()][0],
        onChange: (v) => b.set(v),
      });
      if (b.key) watch(b.key, () => { if (api.value !== options[idx()][0]) api.set(options[idx()][0]); });
      return el;
    }
    const sel = h('select', { class: 'stg-select', 'aria-label': label, disabled, onclick: (e) => e.stopPropagation() },
      options.map(([, l], i) => h('option', { value: i, text: l })));
    sel.value = idx();
    sel.addEventListener('change', () => b.set(options[sel.value][0]));
    if (b.key) watch(b.key, () => { sel.value = idx(); });
    return sel;
  }

  function sliderCtl(b, label, { min = 0, max = 100 } = {}) {
    const input = h('input', { type: 'range', class: 'slider', min, max, step: 1, 'aria-label': label });
    const fill = () => input.style.setProperty('--fill', `${((input.value - min) / (max - min)) * 100}%`);
    input.value = b.get();
    input.addEventListener('input', () => { fill(); b.set(Number(input.value)); });
    input.addEventListener('click', (e) => e.stopPropagation());
    if (Shell.ui?.slider) Shell.ui.slider(input);
    fill();
    if (b.key) watch(b.key, () => { if (Number(input.value) !== b.get()) { input.value = b.get(); fill(); } });
    return input;
  }

  const btn = (text, onclick, cls = '') => h('button', { class: `btn ${cls}`.trim(), type: 'button', text, onclick: (e) => { e.stopPropagation(); onclick?.(e); } });

  /* ---------- cards ---------- */
  /* spec: {icon, title, desc, to?, ctl?() -> element, items?: [item], open?, disabled?}
     item: {title, desc?, ctl?()} | {check: binding, title} | {node()} */
  const titles = {}; // page id -> title, learned from link cards for pages without a definition

  function cardHead(spec) {
    return [
      spec.icon ? h('span', { class: 'stg-card-icon' }, typeof spec.icon === 'string' ? icon(spec.icon, 20) : spec.icon) : null,
      h('div', { class: 'stg-card-text' },
        h('div', { class: 'stg-card-title', text: spec.title }),
        spec.desc ? h('div', { class: 'stg-card-desc', text: spec.desc }) : null),
      spec.ctl ? h('div', { class: 'stg-card-ctl' }, spec.ctl()) : null,
    ];
  }

  function clickable(el, fn) {
    el.setAttribute('role', 'button');
    el.tabIndex = 0;
    el.addEventListener('click', fn);
    el.addEventListener('keydown', (e) => {
      if ((e.key === 'Enter' || e.key === ' ') && e.target === el) { e.preventDefault(); fn(e); }
    });
    return el;
  }

  function card(spec) {
    if (spec.items) return expander(spec);
    const el = h('div', { class: `stg-card${spec.disabled ? ' disabled' : ''}` }, cardHead(spec));
    if (spec.to || spec.onClick) {
      if (spec.to) titles[spec.to] = titles[spec.to] || spec.title;
      el.classList.add('stg-card-click');
      el.append(h('span', { class: 'stg-card-chev' }, icon(spec.external ? 'open' : 'chevron-right', 16)));
      clickable(el, () => (spec.onClick ? spec.onClick() : go(spec.to, { anim: 'drill' })));
    }
    return el;
  }

  const expanded = new Map(); // `${page}|${title}` -> open, kept while the window lives
  function expander(spec) {
    const k = `${current}|${spec.title}`;
    let isOpen = expanded.has(k) ? expanded.get(k) : !!spec.open;
    const chev = h('span', { class: 'stg-exp-chev' }, icon('chevron-down', 16));
    const head = h('div', { class: 'stg-card stg-exp-head', 'aria-expanded': String(isOpen) }, cardHead(spec), chev);
    const body = h('div', { class: 'stg-exp-body' }, spec.items.map(expItem));
    const clip = h('div', { class: 'stg-exp-clip' }, body);
    const wrap = h('div', { class: `stg-exp${isOpen ? ' open' : ''}` }, head, clip);
    clip.hidden = !isOpen;
    clickable(head, () => {
      isOpen = !isOpen;
      expanded.set(k, isOpen);
      head.setAttribute('aria-expanded', String(isOpen));
      if (isOpen) {
        wrap.classList.add('open');
        clip.hidden = false;
        const hgt = body.offsetHeight;
        Shell.animate(clip, [{ height: '0px' }, { height: `${hgt}px` }], { duration: 250 });
        Shell.animate(body, [{ transform: `translateY(-${hgt}px)` }, { transform: 'none' }], { duration: 250 });
      } else {
        const hgt = body.offsetHeight;
        Shell.animate(body, [{ transform: 'none' }, { transform: `translateY(-${hgt}px)` }], { duration: 167, easing: Shell.ease.out });
        Shell.animate(clip, [{ height: `${hgt}px` }, { height: '0px' }], { duration: 167, easing: Shell.ease.out }).then(() => {
          if (!isOpen) { clip.hidden = true; wrap.classList.remove('open'); }
        });
      }
    });
    return wrap;
  }

  function expItem(it) {
    if (it.node) return h('div', { class: 'stg-exp-item stg-exp-node' }, it.node());
    if (it.check) return h('div', { class: 'stg-exp-item' }, checkRow(it.check, it.title, it));
    const row = h('div', { class: 'stg-exp-item' },
      h('div', { class: 'stg-card-text' },
        h('div', { class: 'stg-card-title', text: it.title }),
        it.desc ? h('div', { class: 'stg-card-desc', text: it.desc }) : null),
      it.ctl ? h('div', { class: 'stg-card-ctl' }, it.ctl()) : null);
    if (it.to) {
      row.classList.add('stg-exp-click');
      row.append(h('span', { class: 'stg-card-chev' }, icon('chevron-right', 16)));
      clickable(row, () => go(it.to, { anim: 'drill' }));
    }
    return row;
  }

  const section = (title, ...cards) => h('section', { class: 'stg-section' },
    title ? h('h3', { class: 'stg-section-title', text: title }) : null, cards.flat().map((c) => (c instanceof Node ? c : card(c))));
  const list = (...cards) => section(null, ...cards);

  /* Card shorthands: link, Shell setting toggle, local toggle. */
  const L = (to, ic, title, desc, more) => ({ to, icon: ic, title, desc, ...more });
  const T = (key, ic, title, desc, more = {}) => ({ icon: ic, title, desc, to: more.to, ctl: () => toggleCtl(bind(key, false), title, more) });
  const TL = (k, def, ic, title, desc, more = {}) => ({ icon: ic, title, desc, to: more.to, ctl: () => toggleCtl(loc(k, def), title, more) });
  const C = (k, def, title) => ({ check: loc(k, def), title });

  /* ---------- shared page parts ---------- */
  const deviceName = () => loc('deviceName', 'MIHAWK-PS5').get();
  const fmtTime = (d) => d.toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit' });

  function heroTile(ic, title, sub, to) {
    titles[to] = titles[to] || title;
    return clickable(h('div', { class: 'stg-hero-tile' },
      h('span', { class: 'stg-hero-tile-icon' }, icon(ic, 20)),
      h('div', {}, h('div', { class: 'stg-hero-tile-title', text: title }), h('div', { class: 'stg-hero-tile-sub', text: sub }))),
    () => go(to, { anim: 'drill' }));
  }

  const online = () => S().wifi && !S().airplane;
  function deviceHero() {
    const name = h('div', { class: 'stg-device-name', text: deviceName() });
    const tiles = h('div', { class: 'stg-hero-tiles' });
    const paint = () => tiles.replaceChildren(
      online() ? heroTile('wifi', 'Mihawk-5G', 'Connected, secured', 'network/wifi') : heroTile('wifi-off', 'Wi-Fi', 'Not connected', 'network/wifi'),
      heroTile('arrow-sync', 'Windows Update', `Last checked: ${updateState.label()}`, 'update'));
    paint();
    watch('wifi', paint);
    watch('airplane', paint);
    return h('div', { class: 'stg-hero' },
      h('div', { class: 'stg-device' },
        h('div', { class: 'stg-device-pic', 'aria-hidden': 'true' }, h('div', { class: 'stg-device-screen' })),
        h('div', { class: 'stg-device-text' }, name,
          h('div', { class: 'stg-device-sub', text: 'Proton runtime' }),
          h('button', { class: 'hyperlink stg-link', type: 'button', text: 'Rename', onclick: () => renameDialog(name) }))),
      tiles);
  }

  /* Desktop preview at the top of Personalization pages: the live wallpaper, a window in the
     current mode with the accent, and a taskbar strip. Restyles itself through the tokens. */
  function preview(cls = '') {
    return h('div', { class: `stg-preview ${cls}`.trim(), 'aria-hidden': 'true' },
      h('div', { class: 'stg-preview-win' },
        h('div', { class: 'stg-preview-bar' }),
        h('div', { class: 'stg-preview-text', text: 'Aa' }),
        h('div', { class: 'stg-preview-btn' })),
      h('div', { class: 'stg-preview-taskbar' },
        h('span', { class: 'stg-preview-start' }), h('span'), h('span'), h('span')));
  }

  /* ---------- themes ---------- */
  const THEMES = [
    { name: 'Windows (light)', wallpaper: 'meadow-light', theme: 'light', accent: '#0078d4' },
    { name: 'Windows (dark)', wallpaper: 'meadow-dark', theme: 'dark', accent: '#0078d4' },
    { name: 'Glow', wallpaper: 'glow', theme: 'dark', accent: '#8764b8' },
    { name: 'Captured Motion', wallpaper: 'captured-motion', theme: 'dark', accent: '#038387' },
    { name: 'Sunrise', wallpaper: 'sunrise', theme: 'light', accent: '#ca5010' },
    { name: 'Flow', wallpaper: 'flow', theme: 'light', accent: '#6b69d6' },
  ];
  const themeMatches = (t) => S().wallpaper === t.wallpaper && S().theme === t.theme && S().accent.toLowerCase() === t.accent;
  const currentTheme = () => THEMES.find(themeMatches)?.name || 'Custom';
  function applyTheme(t) {
    if (S().accentAuto) Shell.set('accentAuto', false);
    Shell.set('theme', t.theme);
    Shell.set('wallpaper', t.wallpaper);
    Shell.set('accent', t.accent);
  }
  function themeGrid() {
    const grid = h('div', { class: 'stg-themes', role: 'listbox', 'aria-label': 'Select a theme to apply' },
      THEMES.map((t) => {
        const b = h('button', { class: 'stg-theme', type: 'button', role: 'option', 'data-tooltip': t.name, 'aria-label': t.name,
          style: `--thumb-bg: ${wpCss(wallpaper(t.wallpaper))}; --thumb-accent: ${t.accent}`, onclick: () => applyTheme(t) },
        h('span', { class: 'stg-theme-win', 'data-theme': t.theme },
          h('span', { class: 'stg-theme-text', text: 'Aa' }), h('span', { class: 'stg-theme-accent' })));
        b._theme = t;
        return b;
      }));
    const sync = () => grid.querySelectorAll('.stg-theme').forEach((b) => b.setAttribute('aria-selected', String(themeMatches(b._theme))));
    sync();
    ['theme', 'wallpaper', 'accent'].forEach((k) => watch(k, sync));
    return grid;
  }

  /* ---------- swatches ---------- */
  function swatches(colors, { selected, onPick, label = (c) => c, cls = '' }) {
    const grid = h('div', { class: `stg-swatches ${cls}`.trim(), role: 'listbox' },
      colors.map((c) => {
        const b = h('button', { class: 'stg-swatch', type: 'button', role: 'option', 'data-tooltip': label(c), 'aria-label': label(c),
          style: `--swatch: ${c}`, onclick: () => onPick(c) }, h('span', { class: `stg-swatch-check${isLight(c) ? ' dark' : ''}` }, icon('checkmark', 16)));
        b._color = c;
        return b;
      }));
    const sync = () => grid.querySelectorAll('.stg-swatch').forEach((b) => b.setAttribute('aria-selected', String(selected() === b._color)));
    sync();
    grid._sync = sync;
    return grid;
  }

  /* ---------- dialogs (ContentDialog: smoke over the window, 8 px radius, two bands) ---------- */
  let openDialog = null;
  function dialog({ title, body, primary, secondary = 'Cancel', onPrimary, primaryDisabled }) {
    closeDialog(true);
    const ok = btn(primary, () => { if (onPrimary?.() !== false) closeDialog(); }, 'accent');
    ok.disabled = !!primaryDisabled;
    const box = h('div', { class: 'stg-dialog', role: 'dialog', 'aria-modal': 'true', 'aria-label': title },
      h('div', { class: 'stg-dialog-body' }, h('div', { class: 'stg-dialog-title', text: title }), body),
      h('div', { class: 'stg-dialog-foot' }, ok, btn(secondary, () => closeDialog())));
    const layer = h('div', { class: 'stg-dialog-layer' }, box);
    layer.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { e.stopPropagation(); closeDialog(); }
      if (e.key === 'Enter' && e.target.tagName === 'INPUT' && !ok.disabled) { e.preventDefault(); ok.click(); }
    });
    app.root.append(layer);
    openDialog = { layer, box, ok };
    Shell.animate(layer, [{ opacity: 0 }, { opacity: 1 }], { duration: 167, easing: 'linear' });
    Shell.animate(box, [{ transform: 'scale(1.05)' }, { transform: 'none' }], { duration: 250 });
    requestAnimationFrame(() => (box.querySelector('input') || ok).focus());
    return openDialog;
  }
  function closeDialog(now) {
    if (!openDialog) return;
    const { layer, box } = openDialog;
    openDialog = null;
    if (now) { layer.remove(); return; }
    layer.style.pointerEvents = 'none';
    Shell.animate(box, [{ transform: 'none' }, { transform: 'scale(1.05)' }], { duration: 167, easing: Shell.ease.out, fill: 'forwards' });
    Shell.animate(layer, [{ opacity: 1 }, { opacity: 0 }], { duration: 167, easing: Shell.ease.out, fill: 'forwards' }).then(() => layer.remove());
  }

  function renameDialog(nameEl) {
    const input = h('input', { class: 'textbox stg-dialog-input', type: 'text', maxlength: 15, 'aria-label': 'New PC name', spellcheck: 'false' });
    input.value = deviceName();
    const err = h('div', { class: 'stg-dialog-error' });
    const d = dialog({
      title: 'Rename your PC',
      body: [h('p', { class: 'stg-dialog-text', text: 'You can use a combination of letters, hyphens, and numbers.' }),
        input, err, h('p', { class: 'stg-dialog-text stg-dialog-muted', text: `Current PC name: ${deviceName()}` })],
      primary: 'Next',
      onPrimary: () => {
        const v = input.value.trim().toUpperCase();
        loc('deviceName').set(v);
        nameEl.textContent = v;
        Shell.notify?.({ app: 'Settings', title: 'Restart to finish renaming', body: `Your PC will be called ${v} after you restart.`, icon: 'object/app-settings' });
      },
    });
    const check = () => {
      const v = input.value.trim();
      const bad = !v ? '' : !/^[A-Za-z0-9-]+$/.test(v) ? 'The name can only contain letters, hyphens, and numbers.' : /^\d+$/.test(v) ? 'The name can\'t contain only numbers.' : '';
      err.textContent = bad;
      d.ok.disabled = !v || !!bad || v.toUpperCase() === deviceName();
    };
    input.addEventListener('input', check);
    check();
    input.select();
  }

  /* Custom colour: WinUI ColorPicker (hue x saturation spectrum, value slider, hex box). */
  function colorDialog({ title, initial, onDone }) {
    let [hu, sa, va] = hexToHsv(initial);
    const thumb = h('span', { class: 'stg-spectrum-thumb' });
    const spectrum = h('div', { class: 'stg-spectrum', role: 'slider', tabindex: 0, 'aria-label': 'Color spectrum' }, h('span', { class: 'stg-spectrum-shade' }), thumb);
    const value = h('input', { type: 'range', class: 'stg-value', min: 0, max: 100, 'aria-label': 'Value' });
    const swatch = h('div', { class: 'stg-color-preview' });
    const hex = h('input', { class: 'textbox stg-hex', type: 'text', maxlength: 7, 'aria-label': 'Hex', spellcheck: 'false' });
    const color = () => hsvToHex(hu, sa, va);
    const paint = (fromHex) => {
      const c = color();
      spectrum.style.setProperty('--v', va);
      thumb.style.left = `${(hu / 360) * 100}%`;
      thumb.style.top = `${(1 - sa) * 100}%`;
      thumb.style.background = c;
      value.value = Math.round(va * 100);
      value.style.setProperty('--hue', hsvToHex(hu, sa, 1));
      swatch.style.background = c;
      if (!fromHex) hex.value = c.toUpperCase();
    };
    const fromPoint = (e) => {
      const r = spectrum.getBoundingClientRect();
      hu = Math.min(359.9, Math.max(0, ((e.clientX - r.left) / r.width) * 360));
      sa = Math.min(1, Math.max(0, 1 - (e.clientY - r.top) / r.height));
      paint();
    };
    spectrum.addEventListener('pointerdown', (e) => {
      spectrum.setPointerCapture(e.pointerId);
      fromPoint(e);
      const move = (ev) => fromPoint(ev);
      const up = () => { spectrum.removeEventListener('pointermove', move); spectrum.removeEventListener('pointerup', up); };
      spectrum.addEventListener('pointermove', move);
      spectrum.addEventListener('pointerup', up);
    });
    spectrum.addEventListener('keydown', (e) => {
      const step = { ArrowLeft: [-4, 0], ArrowRight: [4, 0], ArrowUp: [0, 0.04], ArrowDown: [0, -0.04] }[e.key];
      if (!step) return;
      e.preventDefault();
      hu = (hu + step[0] + 360) % 360;
      sa = Math.min(1, Math.max(0, sa + step[1]));
      paint();
    });
    value.addEventListener('input', () => { va = value.value / 100; paint(); });
    hex.addEventListener('input', () => {
      const m = /^#?([0-9a-f]{6})$/i.exec(hex.value.trim());
      if (m) { [hu, sa, va] = hexToHsv(`#${m[1]}`); paint(true); }
    });
    paint();
    dialog({
      title,
      body: [h('div', { class: 'stg-picker' }, spectrum, h('div', { class: 'stg-picker-side' }, swatch)), value,
        h('label', { class: 'stg-hex-row' }, h('span', { text: 'Hex' }), hex)],
      primary: 'Done',
      onPrimary: () => onDone(color()),
    });
  }

  /* ---------- Windows Update state (lives as long as the page) ---------- */
  const updateState = {
    checked: new Date(Shell.now().getTime() - 2 * 3600e3),
    label() {
      const mins = Math.round((Shell.now() - this.checked) / 60000);
      return mins < 1 ? 'Just now' : mins < 60 ? `${mins} minutes ago` : `Today, ${fmtTime(this.checked)}`;
    },
  };

  /* ---------- pages ---------- */
  function homeCards() {
    const rec = h('div', { class: 'stg-home-card' },
      h('h3', { class: 'stg-home-title', text: 'Recommended settings' }),
      h('p', { class: 'stg-home-desc', text: 'Recent and commonly used settings' }),
      h('div', { class: 'stg-home-list' }, [['Display', 'system/display'], ['Sound', 'system/sound'], ['Taskbar', 'personalization/taskbar'], ['Colors', 'personalization/colors']]
        .map(([t, to]) => clickable(h('div', { class: 'stg-home-row' }, h('span', { text: t }), icon('chevron-right', 16)), () => go(to, { anim: 'drill' })))));
    const pers = h('div', { class: 'stg-home-card' },
      h('h3', { class: 'stg-home-title', text: 'Personalize your device' }),
      h('p', { class: 'stg-home-desc', text: 'Select a theme to apply' }),
      themeGrid(),
      h('div', { class: 'stg-home-list' },
        h('div', { class: 'stg-home-row static' }, h('span', { text: 'Color mode' }), comboCtl(bind('theme'), [['light', 'Light'], ['dark', 'Dark']], 'Color mode')),
        clickable(h('div', { class: 'stg-home-row' }, h('span', { text: 'Browse more backgrounds, colors, and themes' }), icon('chevron-right', 16)), () => go('personalization', { anim: 'drill' }))));
    const bt = h('div', { class: 'stg-home-card' },
      h('h3', { class: 'stg-home-title', text: 'Bluetooth devices' }),
      h('p', { class: 'stg-home-desc', text: 'Manage, add, and remove devices' }),
      h('div', { class: 'stg-home-list' },
        h('div', { class: 'stg-home-row static' }, h('span', { class: 'stg-home-dev' }, icon('games', 20),
          h('span', {}, h('span', { class: 'stg-home-dev-name', text: 'DualSense Wireless Controller' }), h('span', { class: 'stg-home-dev-sub', text: 'Connected' })))),
        h('div', { class: 'stg-home-row static' }, h('span', { text: 'Bluetooth' }), toggleCtl(bind('bluetooth'), 'Bluetooth'))),
      h('div', { class: 'stg-home-actions' }, btn('View all devices', () => go('bluetooth', { anim: 'drill' })), btn('Add device', () => go('bluetooth/devices', { anim: 'drill' }))));
    return h('div', { class: 'stg-home-grid' }, rec, pers, bt);
  }

  function personalizationTop() {
    return h('div', { class: 'stg-pers-top' }, preview('large'),
      h('div', { class: 'stg-pers-themes' }, h('div', { class: 'stg-pers-label', text: 'Select a theme to apply' }), themeGrid()));
  }

  function backgroundPage() {
    const isSolid = () => !!wallpaper(S().wallpaper).solid;
    const mode = { key: 'wallpaper', get: () => (isSolid() ? 'solid' : 'picture'), set: (v) => {
      if (v === 'solid' && !isSolid()) Shell.set('wallpaper', solidId('#0063b1'));
      if (v === 'picture' && isSolid()) Shell.set('wallpaper', 'meadow-light');
    } };
    const pictures = () => Shell.wallpapers.filter((w) => !w.solid);
    const recent = h('div', { class: 'stg-recent' });
    const paintRecent = () => recent.replaceChildren(...pictures().slice(0, 7).map((w) => h('button', {
      class: 'stg-recent-img', type: 'button', 'data-tooltip': w.name, 'aria-label': w.name, 'aria-selected': String(S().wallpaper === w.id),
      style: `--thumb-bg: ${wpCss(w)}`, onclick: () => Shell.set('wallpaper', w.id) })));
    const browse = () => {
      const f = h('input', { type: 'file', accept: 'image/*' });
      f.addEventListener('change', () => {
        const file = f.files?.[0];
        if (!file) return;
        // shortcut: a browsed photo lives for this page load only (blob URL), not across reloads.
        const id = `photo-${Date.now()}`;
        Shell.wallpapers.splice(2, 0, { id, name: file.name, image: URL.createObjectURL(file), css: 'var(--wallpaper-fallback)' });
        Shell.set('wallpaper', id);
      });
      f.click();
    };
    const solid = swatches(SOLIDS, {
      selected: () => wallpaper(S().wallpaper).css.toLowerCase(),
      onPick: (c) => Shell.set('wallpaper', solidId(c)),
      label: (c) => c.toUpperCase(),
    });
    const body = h('div', { class: 'stg-bg-body' });
    const customSolid = () => colorDialog({ title: 'Choose a custom background color', initial: wallpaper(S().wallpaper).css, onDone: (c) => Shell.set('wallpaper', solidId(c)) });
    const row = (title, ctl) => h('div', { class: 'stg-bg-row' }, h('span', { class: 'stg-card-title', text: title }), ctl);
    const paint = () => {
      paintRecent();
      solid._sync();
      body.replaceChildren(...(isSolid()
        ? [h('div', { class: 'stg-accent-label', text: 'Choose your background color' }), solid, row('Custom colors', btn('View colors', customSolid))]
        : [h('div', { class: 'stg-accent-label', text: 'Recent images' }), recent, row('Choose a photo', btn('Browse photos', browse))]));
    };
    paint();
    watch('wallpaper', paint);
    return [h('div', { class: 'stg-pers-top' }, preview('large')),
      list({ icon: 'image', title: 'Personalize your background', desc: 'A picture background applies to your current desktop. Solid color or slideshow backgrounds apply to all your desktops.',
        open: true, ctl: () => comboCtl(mode, [['picture', 'Picture'], ['solid', 'Solid color']], 'Personalize your background'), items: [{ node: () => body }] },
      { icon: 'maximize', title: 'Choose a fit for your desktop image',
        ctl: () => comboCtl(loc('fit', 'fill'), [['fill', 'Fill'], ['fit', 'Fit'], ['stretch', 'Stretch'], ['tile', 'Tile'], ['center', 'Center'], ['span', 'Span']], 'Choose a fit for your desktop image') }),
      section('Related settings', L('personalization/contrast', 'color', 'Contrast themes', 'Color themes for low vision, light sensitivity'))];
  }

  function colorsPage() {
    const grid = swatches(ACCENTS.map(([c]) => c), {
      selected: () => (S().accentAuto ? '' : S().accent.toLowerCase()),
      onPick: chooseAccent,
      label: (c) => ACCENTS.find(([x]) => x === c)[1],
    });
    const recent = h('div', { class: 'stg-recent-colors' });
    const paintRecent = () => recent.replaceChildren(swatches(recentAccents(), {
      selected: () => (S().accentAuto ? '' : S().accent.toLowerCase()),
      onPick: chooseAccent,
      label: (c) => ACCENTS.find(([x]) => x === c)?.[1] || c.toUpperCase(),
    }));
    const wrap = h('div', { class: 'stg-accent-items' },
      h('div', { class: 'stg-accent-label', text: 'Recent colors' }), recent,
      h('div', { class: 'stg-accent-label', text: 'Windows colors' }), grid);
    const disabled = () => wrap.classList.toggle('stg-auto', !!S().accentAuto);
    const accentMode = { key: 'accentAuto', get: () => (S().accentAuto ? 'auto' : 'manual'), set: (v) => {
      Shell.set('accentAuto', v === 'auto');
      if (v === 'auto') autoAccent();
    } };
    paintRecent();
    disabled();
    watch('accent', () => { grid._sync(); paintRecent(); });
    watch('accentAuto', () => { grid._sync(); paintRecent(); disabled(); });
    const modeOpts = [['light', 'Light'], ['dark', 'Dark']];
    return [h('div', { class: 'stg-pers-top' }, preview('large')),
      list(
        { icon: 'color', title: 'Choose your mode', desc: 'Change the colors that appear in Windows and your apps', ctl: () => comboCtl(bind('theme'), modeOpts, 'Choose your mode') },
        T('transparency', 'square-multiple', 'Transparency effects', 'Windows and surfaces appear translucent'),
        { icon: 'paint-brush', title: 'Accent color', open: true, ctl: () => comboCtl(accentMode, [['manual', 'Manual'], ['auto', 'Automatic']], 'Accent color'),
          items: [
            { node: () => wrap },
            { title: 'Custom colors', ctl: () => btn('View colors', () => colorDialog({ title: 'Pick a custom accent color', initial: S().accent, onDone: chooseAccent })) },
          ] },
        T('accentOnStart', 'grid', 'Show accent color on Start and taskbar', null, { disabled: () => S().theme === 'light' }),
        T('accentOnTitleBars', 'window', 'Show accent color on title bars and window borders')),
      section('Related settings', L('personalization/contrast', 'color', 'Contrast themes', 'Color themes for low vision, light sensitivity'))];
  }

  function nowLabel() { const d = Shell.now(); return { time: fmtTime(d), date: d.toLocaleDateString('en-US', { weekday: 'long', month: 'long', day: 'numeric', year: 'numeric' }) }; }

  function updatePage() {
    const title = h('div', { class: 'stg-upd-title', text: 'You\'re up to date' });
    const sub = h('div', { class: 'stg-upd-sub', text: `Last checked: ${updateState.label()}` });
    let timer = 0;
    const check = btn('Check for updates', () => {
      check.disabled = true;
      title.textContent = 'Checking for updates...';
      sub.replaceChildren(h('div', { class: 'progress-bar indeterminate stg-progress', role: 'progressbar', 'aria-label': 'Checking for updates' }));
      timer = setTimeout(() => {
        updateState.checked = Shell.now();
        title.textContent = 'You\'re up to date';
        sub.textContent = `Last checked: ${updateState.label()}`;
        check.disabled = false;
      }, 2600);
    }, 'accent');
    pageCleanups.push(() => clearTimeout(timer));
    return [h('div', { class: 'stg-upd' },
      h('span', { class: 'stg-upd-icon' }, icon('arrow-sync', 56), h('span', { class: 'stg-upd-badge' }, icon('checkmark', 12))),
      h('div', { class: 'stg-upd-text' }, title, sub), h('div', { class: 'stg-upd-action' }, check)),
    list(TL('updatesEarly', false, 'arrow-down', 'Get the latest updates as soon as they\'re available', 'Be among the first to get the latest non-security updates, fixes, and improvements as they roll out.')),
    section('More options',
      { icon: 'timer', title: 'Pause updates', ctl: () => comboCtl(loc('pause', 0), [[0, 'Pause for 1 week'], [2, 'Pause for 2 weeks'], [3, 'Pause for 3 weeks'], [4, 'Pause for 4 weeks'], [5, 'Pause for 5 weeks']], 'Pause updates') },
      L('update/history', 'history', 'Update history'),
      L('update/advanced', 'settings', 'Advanced options', 'Delivery optimization, optional updates, active hours, other update settings'),
      L('update/insider', 'person', 'Windows Insider Program', 'Get preview builds of Windows to share feedback on new features and updates'))];
  }

  function timeHero() {
    const t = h('div', { class: 'stg-time-big' }), d = h('div', { class: 'stg-hero-tile-sub' });
    const paint = () => { const n = nowLabel(); t.textContent = n.time; d.textContent = n.date; };
    paint();
    pageCleanups.push(Shell.on('tick', paint));
    return h('div', { class: 'stg-hero' }, h('div', { class: 'stg-device' }, h('div', {}, t, d)),
      h('div', { class: 'stg-hero-tiles' }, heroTile('globe', 'Language & region', 'English (United States)', 'time/language')));
  }

  function accountHero() {
    return h('div', { class: 'stg-hero' }, h('div', { class: 'stg-device' },
      Shell.icon('object/user-avatar', { size: 96, className: 'stg-avatar-big' }),
      h('div', {}, h('div', { class: 'stg-device-name', text: 'MIHAWK' }), h('div', { class: 'stg-device-sub', text: 'Local account' }), h('div', { class: 'stg-device-sub', text: 'Administrator' }))));
  }

  function appsInstalled() {
    const q = h('input', { class: 'textbox stg-apps-search', type: 'search', placeholder: 'Search apps', 'aria-label': 'Search apps' });
    const out = h('div', { class: 'stg-section' });
    const sizes = [24.6, 3.1, 1.4, 12.8, 642, 88.2, 5.5];
    const paint = () => {
      const s = q.value.trim().toLowerCase();
      const apps = Shell.apps.list().filter((a) => a.name.toLowerCase().includes(s)).sort((a, b) => a.name.localeCompare(b.name));
      out.replaceChildren(h('div', { class: 'stg-apps-count', text: `${apps.length} apps found` }),
        ...apps.map((a, i) => card({ icon: Shell.icon(a.icon || 'object/app-generic', { size: 24 }), title: a.name,
          desc: `Mihawk OS  |  10/${String(1 + (i % 14)).padStart(2, '0')}/2026`,
          ctl: () => h('span', { class: 'stg-apps-ctl' }, h('span', { class: 'stg-apps-size', text: `${sizes[i % sizes.length]} MB` }),
            h('button', { class: 'btn subtle icon-only stg-more', type: 'button', 'aria-label': 'More options', 'data-tooltip': 'More options',
              onclick: (e) => Shell.menu?.show?.({ x: e.clientX, y: e.clientY, anchorRect: e.currentTarget.getBoundingClientRect(), placement: 'bottom-start',
                items: [{ label: 'Advanced options', disabled: true }, { label: 'Move', disabled: true }, { label: 'Uninstall', disabled: true }] }) },
            icon('more-horizontal', 16))) })));
    };
    q.addEventListener('input', paint);
    paint();
    return [h('div', { class: 'stg-apps-bar' }, q), out];
  }

  function networkStatus() {
    const el = h('div', { class: 'stg-device' });
    const paint = () => el.replaceChildren(h('span', { class: 'stg-net-icon' }, icon(online() ? 'wifi' : 'wifi-off', 48)),
      h('div', {}, h('div', { class: 'stg-device-name', text: online() ? 'Wi-Fi' : 'Not connected' }),
        h('div', { class: 'stg-device-sub', text: online() ? 'Mihawk-5G  ·  Connected, secured' : 'You aren\'t connected to any networks' })));
    paint();
    watch('wifi', paint);
    watch('airplane', paint);
    return el;
  }

  const NETWORKS = [['Mihawk-5G', 'Connected, secured'], ['Mihawk-Guest', 'Secured'], ['Studio-5G', 'Secured'], ['Library', 'Open']];
  const COMBINE = [['always', 'Always'], ['full', 'When taskbar is full'], ['never', 'Never']];

  /* System > Clipboard. Clear empties the history (except pinned items) and the file clipboard,
     then shows Windows' checkmark beside the button. */
  function clipboardPage() {
    const done = h('span', { class: 'stg-cleared', role: 'status', 'aria-label': 'Cleared', hidden: true }, icon('checkmark', 16));
    const clear = () => {
      Shell.clipboardPanel?.clear?.();
      Shell.clipboard?.clear?.();
      done.hidden = false;
      Shell.animate(done, [{ opacity: 0 }, { opacity: 1 }], { duration: 167, easing: 'linear' });
    };
    return [list(
      { icon: 'history', title: 'Clipboard history', desc: 'Save multiple items to the clipboard to use later. Press the Windows logo key + V to view your clipboard history and paste from it.',
        ctl: () => toggleCtl(bind('clipboardHistory', true), 'Clipboard history') },
      { icon: 'arrow-sync', title: 'Sync across your devices', desc: 'Paste text on your other devices when you sign in with your account',
        ctl: () => btn('Get started', () => go('accounts', { anim: 'drill' })) },
      TL('suggestedActions', true, 'star', 'Suggested actions', 'Show suggested actions for copied text like dates and phone numbers'),
      { icon: 'delete', title: 'Clear clipboard data', desc: 'Clear everything (except pinned items) on this device',
        ctl: () => h('span', { class: 'stg-btn-pair' }, done, btn('Clear', clear)) })];
  }

  /* Settings search entries that open another app instead of a page: 'app:<id>[/<page>]'.
     Falls through to Settings when the app is not registered. */
  function launchExternal(page) {
    const m = /^app:([\w-]+)(?:\/(.+))?$/.exec(page || '');
    if (!m || !Shell.apps.get(m[1])) return false;
    Shell.apps.launch(m[1], m[2] ? { page: m[2] } : {});
    return true;
  }

  /* Each page: {title, icon, keywords, render() -> nodes}. Ids are paths; the nav selects the first
     segment. Pages without an entry render from the link card that leads to them. */
  const PAGES = {
    home: { title: 'Home', icon: 'home', render: () => [deviceHero(), homeCards()] },

    system: { title: 'System', icon: 'desktop', keywords: 'pc device', render: () => [deviceHero(), list(
      L('system/display', 'desktop', 'Display', 'Monitors, brightness, night light, display profile'),
      L('system/sound', 'speaker-2', 'Sound', 'Volume levels, output, input, sound devices'),
      L('system/notifications', 'alert', 'Notifications', 'Alerts from apps and system, do not disturb'),
      L('system/focus', 'timer', 'Focus', 'Reduce distractions'),
      L('system/power', 'power', 'Power & battery', 'Sleep, battery usage, energy saver'),
      L('system/storage', 'hard-drive', 'Storage', 'Storage space, drives, configuration rules'),
      L('system/nearby-sharing', 'share', 'Nearby sharing', 'Discoverability, received files location'),
      L('system/multitasking', 'square-multiple', 'Multitasking', 'Snap windows, desktops, task switching'),
      L('system/developers', 'wrench', 'For developers', 'These settings are intended for development use only'),
      L('system/activation', 'checkmark', 'Activation', 'Activation state, subscriptions, product key'),
      L('system/troubleshoot', 'wrench', 'Troubleshoot', 'Recommended troubleshooters, preferences, history'),
      L('system/recovery', 'history', 'Recovery', 'Reset, advanced startup, go back'),
      L('system/projecting', 'cast', 'Projecting to this PC', 'Permissions, pairing PIN, discoverability'),
      L('system/remote-desktop', 'tab-desktop', 'Remote Desktop', 'Remote Desktop users, connection permissions'),
      L('system/clipboard', 'clipboard-paste', 'Clipboard', 'Cut and copy history, sync, clear'),
      L('system/about', 'info', 'About', 'Device specifications, rename PC, Windows specifications'))] },

    'system/display': { title: 'Display', icon: 'desktop', keywords: 'monitor screen brightness resolution scale', render: () => [
      section('Brightness & color',
        { icon: 'brightness', title: 'Brightness', desc: 'Adjust the brightness of the built-in display', ctl: () => sliderCtl(bind('brightness', 80), 'Brightness'),
          items: [C('autoBrightness', false, 'Change brightness based on content'), C('batteryBrightness', true, 'Help improve battery by optimizing the content shown and brightness')] },
        T('nightLight', 'weather-moon', 'Night light', 'Use warmer colors to help block blue light', { to: 'system/display/night-light' }),
        L('system/display/hdr', 'brightness', 'HDR', 'More about HDR')),
      section('Scale & layout',
        { icon: 'zoom-in', title: 'Scale', desc: 'Change the size of text, apps, and other items', to: 'system/display/custom-scaling',
          ctl: () => comboCtl(loc('scale', 100), [[100, '100% (Recommended)'], [125, '125%'], [150, '150%'], [175, '175%']], 'Scale') },
        { icon: 'desktop', title: 'Display resolution', desc: 'Adjust the resolution to fit your connected display',
          ctl: () => { const r = `${Math.round(screen.width * devicePixelRatio)} × ${Math.round(screen.height * devicePixelRatio)}`;
            return comboCtl(loc('resolution', r), [[r, `${r} (Recommended)`], ['1600 × 900', '1600 × 900'], ['1280 × 720', '1280 × 720']], 'Display resolution'); } },
        { icon: 'arrow-sync', title: 'Display orientation', ctl: () => comboCtl(loc('orientation', 'landscape'), [['landscape', 'Landscape'], ['portrait', 'Portrait'], ['landscape-flipped', 'Landscape (flipped)'], ['portrait-flipped', 'Portrait (flipped)']], 'Display orientation') },
        { icon: 'square-multiple', title: 'Multiple displays', desc: 'Choose the presentation mode for your displays',
          items: [C('rememberWindows', true, 'Remember window locations based on monitor connection'), C('minimizeOnDisconnect', true, 'Minimize windows when a monitor is disconnected'),
            { title: 'Detect other display', ctl: () => btn('Detect') }] }),
      section('Related settings',
        L('system/display/advanced', 'desktop', 'Advanced display', 'Display information, refresh rate'),
        L('system/display/graphics', 'games', 'Graphics'))] },

    'system/display/night-light': { title: 'Night light', icon: 'weather-moon', keywords: 'blue light warm', render: () => {
      const b = h('button', { class: 'btn', type: 'button', onclick: () => Shell.set('nightLight', !S().nightLight) });
      const sync = () => { b.textContent = S().nightLight ? 'Turn off now' : 'Turn on now'; };
      sync();
      watch('nightLight', sync);
      return [list(
        { title: 'Night light', desc: 'Use warmer colors to help block blue light', ctl: () => b },
        { title: 'Strength', ctl: () => sliderCtl(bind('nightLightStrength', 48), 'Strength') },
        { title: 'Schedule night light', ctl: () => toggleCtl(loc('nightSchedule', false), 'Schedule night light') })];
    } },

    'system/sound': { title: 'Sound', icon: 'speaker-2', keywords: 'volume speakers audio mute', render: () => {
      const mute = h('button', { class: 'btn subtle icon-only stg-mute', type: 'button', onclick: (e) => { e.stopPropagation(); Shell.set('mute', !S().mute); } });
      const sync = () => {
        const v = S().mute ? 0 : S().volume;
        mute.replaceChildren(icon(S().mute ? 'speaker-mute' : v === 0 ? 'speaker-0' : v < 50 ? 'speaker-1' : 'speaker-2', 20));
        mute.setAttribute('aria-label', S().mute ? 'Unmute' : 'Mute');
        mute.dataset.tooltip = S().mute ? 'Unmute' : 'Mute';
      };
      sync();
      watch('mute', sync);
      watch('volume', sync);
      const outputs = [['Speakers', 'High Definition Audio Device'], ['Headphones', 'DualSense Wireless Controller'], ['PS5 Display', 'HDMI']];
      const out = loc('output', 0);
      const radios = () => h('div', { class: 'stg-radios', role: 'radiogroup' }, outputs.map(([n, d], i) => {
        const r = h('input', { type: 'radio', class: 'radio', name: 'stg-output', onchange: () => out.set(i) });
        r.checked = out.get() === i;
        return h('label', { class: 'stg-radio-row' }, r, h('span', { class: 'stg-card-text' }, h('span', { class: 'stg-card-title', text: n }), h('span', { class: 'stg-card-desc', text: d })));
      }));
      return [section('Output',
        { icon: 'speaker-2', title: 'Choose where to play sound', desc: 'Apps might have their own settings', open: true, items: [{ node: radios }] },
        { icon: mute, title: 'Volume', ctl: () => sliderCtl({ key: 'volume', get: () => S().volume, set: (v) => { if (S().mute) Shell.set('mute', false); Shell.set('volume', v); } }, 'Volume') },
        { icon: 'add', title: 'Pair a new output device', ctl: () => btn('Add device', () => go('bluetooth/devices', { anim: 'drill' })) }),
      section('Input',
        { icon: 'mic', title: 'Microphone', desc: 'Microphone Array', to: 'system/sound/microphone' },
        { icon: 'mic', title: 'Volume', ctl: () => sliderCtl(loc('micVolume', 74), 'Input volume') }),
      section('Advanced',
        { icon: 'wrench', title: 'Troubleshoot common sound problems', ctl: () => h('span', { class: 'stg-btn-pair' }, btn('Output devices'), btn('Input devices')) },
        L('system/sound/devices', 'speaker-2', 'All sound devices', 'Turn devices on and off, troubleshoot, other options'),
        L('system/sound/mixer', 'options', 'Volume mixer', 'App volume mix, app input & output devices'))];
    } },

    'system/notifications': { title: 'Notifications', icon: 'alert', keywords: 'do not disturb dnd focus banners', render: () => [
      list(
        { icon: 'alert', title: 'Notifications', desc: 'Get notifications from apps and other senders', ctl: () => toggleCtl(loc('notifications', true), 'Notifications'),
          items: [C('lockNotifications', true, 'Show notifications on the lock screen'), C('notificationSounds', true, 'Allow notifications to play sounds')] },
        T('doNotDisturb', 'alert-off', 'Do not disturb', 'Notifications will be sent directly to notification center'),
        { icon: 'clock', title: 'Turn on do not disturb automatically',
          items: [C('dndTimes', false, 'During these times'), C('dndDuplicate', true, 'When duplicating your display'), C('dndGame', true, 'When playing a game'), C('dndFullscreen', true, 'When using an app in full-screen mode')] },
        { icon: 'star', title: 'Set priority notifications', to: 'system/notifications/priority' },
        L('system/focus', 'timer', 'Focus', 'Session duration, hide badge icons on apps')),
      section('Notifications from apps and other senders', Shell.apps.list().map((a) => ({
        icon: Shell.icon(a.icon || 'object/app-generic', { size: 24 }), title: a.name, desc: 'Banners, Sounds',
        ctl: () => toggleCtl(loc(`notify-${a.id}`, true), a.name) })))] },

    'system/power': { title: 'Power & battery', icon: 'power', keywords: 'battery sleep energy saver', render: () => [
      h('div', { class: 'stg-hero' }, h('div', { class: 'stg-device' }, h('span', { class: 'stg-battery-icon' }, icon('battery', 48)),
        h('div', {}, h('div', { class: 'stg-time-big', text: '87%' }), h('div', { class: 'stg-hero-tile-sub', text: 'Estimated time remaining: 5 hours 12 minutes' })))),
      section('Power',
        { icon: 'clock', title: 'Screen and sleep', items: [
          { title: 'Turn my screen off after', ctl: () => comboCtl(loc('screenOff', 10), [[5, '5 minutes'], [10, '10 minutes'], [15, '15 minutes'], [30, '30 minutes'], [0, 'Never']], 'Turn my screen off after') },
          { title: 'Make my device sleep after', ctl: () => comboCtl(loc('sleep', 30), [[15, '15 minutes'], [30, '30 minutes'], [60, '1 hour'], [0, 'Never']], 'Make my device sleep after') }] },
        { icon: 'power', title: 'Power mode', desc: 'Optimize your device based on power use and performance',
          ctl: () => comboCtl(loc('powerMode', 'balanced'), [['efficiency', 'Best power efficiency'], ['balanced', 'Balanced'], ['performance', 'Best performance']], 'Power mode') }),
      section('Battery',
        { icon: 'battery-saver', title: 'Energy saver', desc: 'Extend battery life by limiting some notifications and background activity', ctl: () => toggleCtl(bind('energySaver'), 'Energy saver'),
          items: [{ title: 'Turn energy saver on automatically at', ctl: () => comboCtl(loc('saverAt', 20), [[10, '10%'], [20, '20%'], [30, '30%'], [50, '50%'], [100, 'Always']], 'Turn energy saver on automatically at') },
            C('saverDim', true, 'Lower screen brightness when using energy saver')] },
        L('system/power/usage', 'battery', 'Battery usage'))] },

    'system/multitasking': { title: 'Multitasking', icon: 'square-multiple', keywords: 'snap layouts alt tab desktops', render: () => [list(
      { icon: 'square-multiple', title: 'Snap windows', ctl: () => toggleCtl(loc('snap', true), 'Snap windows'), items: [
        C('snapSuggest', true, 'Show snap suggestions when I snap a window'),
        C('snapHover', true, 'Show my snapped windows when I hover over taskbar apps, in Task View, and when I press Alt+Tab'),
        C('snapEdge', true, 'When I drag a window, let me snap it without dragging all the way to the screen edge'),
        C('snapLayouts', true, 'Show snap layouts when I hover over a window\'s maximize button'),
        C('snapBar', true, 'Show snap layouts when I drag a window to the top of my screen')] },
      { icon: 'tab-desktop', title: 'Desktops', items: [
        { title: 'On the taskbar, show all the open windows', ctl: () => comboCtl(loc('deskTaskbar', 'current'), [['current', 'Only on the desktop I\'m using'], ['all', 'On all desktops']], 'On the taskbar, show all the open windows') },
        { title: 'Show all open windows when I press Alt+Tab', ctl: () => comboCtl(loc('deskAltTab', 'current'), [['current', 'Only on the desktop I\'m using'], ['all', 'On all desktops']], 'Show all open windows when I press Alt+Tab') }] },
      TL('shake', false, 'window', 'Title bar window shake', 'When I grab a window\'s title bar and shake it, minimize all other windows'))] },

    bluetooth: { title: 'Bluetooth & devices', icon: 'bluetooth', keywords: 'devices controller mouse keyboard printer', render: () => [
      h('div', { class: 'stg-dev-tiles' },
        h('div', { class: 'stg-dev-tile' }, h('span', { class: 'stg-dev-tile-icon' }, icon('games', 32)),
          h('div', {}, h('div', { class: 'stg-hero-tile-title', text: 'DualSense Wireless Controller' }), h('div', { class: 'stg-hero-tile-sub', text: 'Connected' }))),
        clickable(h('div', { class: 'stg-dev-tile stg-dev-add' }, h('span', { class: 'stg-dev-tile-icon' }, icon('add', 24)), h('div', { class: 'stg-hero-tile-title', text: 'Add device' })), () => go('bluetooth/devices', { anim: 'drill' })),
        clickable(h('div', { class: 'stg-dev-tile stg-dev-add' }, h('div', { class: 'stg-hero-tile-title', text: 'View more devices' }), icon('chevron-right', 16)), () => go('bluetooth/devices', { anim: 'drill' }))),
      list(
        T('bluetooth', 'bluetooth', 'Bluetooth', `Discoverable as "${deviceName()}"`),
        L('bluetooth/devices', 'keyboard', 'Devices', 'Mouse, keyboard, pen, audio, displays and docks, other devices'),
        L('bluetooth/printers', 'print', 'Printers & scanners', 'Preferences, troubleshoot'),
        L('bluetooth/phone', 'phone', 'Phone Link', 'Instantly access your mobile device\'s photos, texts, and more'),
        L('bluetooth/cameras', 'camera', 'Cameras', 'Connected cameras, default image settings'),
        L('bluetooth/mouse', 'select-all', 'Mouse', 'Buttons, mouse pointer speed, scrolling'),
        L('bluetooth/pen', 'edit', 'Pen & Windows Ink', 'Right-handed or left-handed, pen button shortcuts, handwriting'),
        L('bluetooth/autoplay', 'play', 'AutoPlay', 'Defaults for removable drives and memory cards'),
        L('bluetooth/usb', 'hard-drive', 'USB', 'Notifications, USB battery saver'))] },

    network: { title: 'Network & internet', icon: 'wifi', keywords: 'wifi ethernet internet vpn airplane hotspot', render: () => [
      h('div', { class: 'stg-hero' },
        networkStatus(),
        h('div', { class: 'stg-hero-tiles' },
          heroTile('info', 'Properties', 'Public network, 5 GHz', 'network/wifi/properties'),
          heroTile('arrow-sort', 'Data usage', '2.4 GB, last 30 days', 'network/data-usage'))),
      list(
        T('wifi', 'wifi', 'Wi-Fi', 'Connect, manage known networks, metered network', { to: 'network/wifi' }),
        L('network/ethernet', 'globe', 'Ethernet', 'Authentication, IP and DNS settings, metered network'),
        L('network/vpn', 'shield', 'VPN', 'Add, connect, manage'),
        T('mobileHotspot', 'phone', 'Mobile hotspot', 'Share your internet connection', { to: 'network/hotspot' }),
        T('airplane', 'airplane', 'Airplane mode', 'Stop all wireless communication', { to: 'network/airplane' }),
        L('network/proxy', 'globe', 'Proxy', 'Proxy server for Wi-Fi and ethernet connections'),
        L('network/dial-up', 'phone', 'Dial-up', 'Set up a dial-up internet connection'),
        L('network/advanced', 'options', 'Advanced network settings', 'View all network adapters, network reset'))] },

    'network/wifi': { title: 'Wi-Fi', icon: 'wifi', keywords: 'wireless network ssid', render: () => [list(
      T('wifi', 'wifi', 'Wi-Fi'),
      L('network/wifi/properties', 'info', 'Mihawk-5G properties', 'Connected, secured'),
      { icon: 'text-bullet-list', title: 'Show available networks', items: NETWORKS.map(([n, d]) => ({ title: n, desc: d })) },
      L('network/wifi/hardware', 'options', 'Hardware properties', 'View and manage Wi-Fi adapter properties'),
      L('network/wifi/known', 'text-bullet-list', 'Manage known networks', 'Add, remove, edit networks'),
      TL('randomMac', false, 'shield', 'Random hardware addresses', 'Help protect your privacy by making it harder for people to track your device location when you connect to different Wi-Fi networks'))] },

    personalization: { title: 'Personalization', icon: 'paint-brush', keywords: 'theme wallpaper background colors', render: () => [personalizationTop(), list(
      L('personalization/background', 'image', 'Background', 'Background image, color, slideshow'),
      L('personalization/colors', 'color', 'Colors', 'Accent color, transparency effects, color theme'),
      L('personalization/themes', 'paint-brush', 'Themes', 'Install, create, manage'),
      L('personalization/dynamic-lighting', 'brightness', 'Dynamic Lighting', 'Connected devices, effects, app settings'),
      L('personalization/lock-screen', 'lock-closed', 'Lock screen', 'Lock screen images, apps, animations'),
      L('personalization/text-input', 'keyboard', 'Text input', 'Touch keyboard, voice typing, emoji and more, input method editor'),
      L('personalization/start', 'apps', 'Start', 'Recent apps and items, folders'),
      L('personalization/taskbar', 'grid', 'Taskbar', 'Taskbar behaviors, system pins'),
      L('personalization/fonts', 'text-font', 'Fonts', 'Install, manage'),
      L('personalization/device-usage', 'desktop', 'Device usage', 'Select all the ways you plan to use your device to get personalized tips and recommendations within Mihawk OS'))] },

    'personalization/background': { title: 'Background', icon: 'image', keywords: 'wallpaper picture solid color desktop background', render: backgroundPage },
    'personalization/colors': { title: 'Colors', icon: 'color', keywords: 'accent color dark mode light mode transparency theme', render: colorsPage },

    'personalization/themes': { title: 'Themes', icon: 'paint-brush', keywords: 'theme', render: () => {
      const cur = h('div', { class: 'stg-card-desc', text: currentTheme() });
      watch('theme', () => { cur.textContent = currentTheme(); });
      watch('wallpaper', () => { cur.textContent = currentTheme(); });
      watch('accent', () => { cur.textContent = currentTheme(); });
      return [h('div', { class: 'stg-pers-top' }, preview('large'), h('div', { class: 'stg-theme-parts' },
        [['image', 'Background', 'personalization/background'], ['color', 'Color', 'personalization/colors'], ['speaker-2', 'Sounds', 'personalization/themes/sounds'], ['select-all', 'Mouse cursor', 'bluetooth/mouse']]
          .map(([ic, t, to]) => clickable(h('div', { class: 'stg-hero-tile' }, h('span', { class: 'stg-hero-tile-icon' }, icon(ic, 20)), h('div', {}, h('div', { class: 'stg-hero-tile-title', text: t }))), () => go(to, { anim: 'drill' }))))),
      list({ icon: 'paint-brush', title: 'Current theme', ctl: () => cur, open: true, items: [{ node: themeGrid }] }),
      section('Related settings', L('personalization/contrast', 'color', 'Contrast themes', 'Color themes for low vision, light sensitivity'))];
    } },

    'personalization/start': { title: 'Start', icon: 'apps', keywords: 'start menu pins recommended', render: () => [list(
      { icon: 'grid', title: 'Layout', ctl: () => comboCtl(loc('startLayout', 'default'), [['pins', 'More pins'], ['default', 'Default'], ['recs', 'More recommendations']], 'Layout') },
      TL('startRecentApps', true, 'add', 'Show recently added apps'),
      TL('startMostUsed', false, 'star', 'Show most used apps'),
      TL('startRecentFiles', true, 'history', 'Show recommended files in Start, recent files in File Explorer, and items in Jump Lists'),
      TL('startTips', true, 'info', 'Show recommendations for tips, shortcuts, new apps, and more'),
      TL('startAccount', true, 'person', 'Show account-related notifications'),
      L('personalization/start/folders', 'folder', 'Folders', 'These folders appear on Start next to the Power button'))] },

    'personalization/taskbar': { title: 'Taskbar', icon: 'grid', keywords: 'taskbar alignment search task view widgets', render: () => [list(
      { icon: 'grid', title: 'Taskbar items', desc: 'Show or hide buttons that appear on the taskbar', open: true, items: [
        { title: 'Search', ctl: () => comboCtl(bind('taskbarSearch', 'box'), [['hidden', 'Hide'], ['icon', 'Search icon only'], ['box', 'Search box']], 'Search') },
        { title: 'Task view', ctl: () => toggleCtl(loc('taskView', true), 'Task view') },
        { title: 'Widgets', ctl: () => toggleCtl(loc('widgets', false), 'Widgets') }] },
      { icon: 'apps', title: 'System tray icons', desc: 'Show or hide system tray icons', items: [
        { title: 'Pen menu', desc: 'Show pen menu icon when pen is in use', ctl: () => toggleCtl(loc('penMenu', false), 'Pen menu') },
        { title: 'Touch keyboard', desc: 'Show touch keyboard icon', ctl: () => comboCtl(loc('touchKb', 'auto'), [['never', 'Never'], ['always', 'Always'], ['auto', 'When no keyboard attached']], 'Touch keyboard') },
        { title: 'Virtual touchpad', desc: 'Always show virtual touchpad icon', ctl: () => toggleCtl(loc('vTouchpad', false), 'Virtual touchpad') }] },
      { icon: 'more-horizontal', title: 'Other system tray icons', desc: 'Show or hide additional system tray icons', items: [
        { title: 'Hidden icon menu', desc: 'Show hidden icon menu', ctl: () => toggleCtl(loc('hiddenIcons', true), 'Hidden icon menu') }] },
      { icon: 'options', title: 'Taskbar behaviors', desc: 'Taskbar alignment, badging, automatically hide, and multiple displays', items: [
        { title: 'Taskbar alignment', ctl: () => comboCtl(bind('taskbarAlignment', 'center'), [['left', 'Left'], ['center', 'Center']], 'Taskbar alignment') },
        { check: bind('taskbarAutoHide', false), title: 'Automatically hide the taskbar' },
        { check: bind('taskbarBadges', true), title: 'Show badges on taskbar apps' },
        { check: bind('taskbarFlashing', true), title: 'Show flashing on taskbar apps' },
        /* one display: Windows greys out the multiple-display options */
        { check: loc('allDisplays', false), title: 'Show my taskbar on all displays', disabled: true },
        { title: 'When using multiple displays, show my taskbar apps on', ctl: () => comboCtl(loc('multiTaskbar', 'all'),
          [['all', 'All taskbars'], ['main', 'Main taskbar and taskbar where window is open'], ['open', 'Taskbar where window is open']], 'When using multiple displays, show my taskbar apps on', { disabled: true }) },
        C('shareWindow', true, 'Share any window from my taskbar'),
        C('farCorner', true, 'Select the far corner of the taskbar to show the desktop'),
        /* Windows 11 moved Peek out of Settings (Show desktop button menu, Performance Options); this is Windows 10's wording */
        { check: bind('taskbarPeek', false), title: 'Use Peek to preview the desktop when you move your mouse to the Show desktop button at the end of the taskbar' },
        { title: 'Combine taskbar buttons and hide labels', ctl: () => comboCtl(loc('combine', 'always'), COMBINE, 'Combine taskbar buttons and hide labels') },
        { title: 'Combine taskbar buttons and hide labels on other taskbars', ctl: () => comboCtl(loc('combineOther', 'always'), COMBINE, 'Combine taskbar buttons and hide labels on other taskbars', { disabled: true }) },
        C('clockSeconds', false, 'Show seconds in system tray clock (uses more power)')] })] },

    'system/clipboard': { title: 'Clipboard', icon: 'clipboard-paste', keywords: 'clipboard history copy paste win+v sync clear', render: clipboardPage },

    apps: { title: 'Apps', icon: 'apps', keywords: 'installed uninstall default startup', render: () => [list(
      L('apps/installed', 'apps-list', 'Installed apps', 'Uninstall, reorder'),
      L('apps/advanced', 'options', 'Advanced app settings', 'Choose where to get apps, app execution aliases, archive apps'),
      L('apps/default', 'apps', 'Default apps', 'Defaults for file and link types, other defaults'),
      L('apps/offline-maps', 'location', 'Offline maps', 'Downloads, storage location, map updates'),
      L('apps/optional-features', 'add', 'Optional features', 'Extra functionality for your device'),
      L('apps/websites', 'globe', 'Apps for websites', 'Websites that can open in an app instead of a browser'),
      L('apps/video', 'video', 'Video playback', 'Video adjustments, HDR streaming, battery options'),
      L('apps/startup', 'play', 'Startup', 'Apps that start automatically when you sign in'))] },
    'apps/installed': { title: 'Installed apps', icon: 'apps-list', keywords: 'uninstall programs', render: appsInstalled },

    accounts: { title: 'Accounts', icon: 'person', keywords: 'user sign-in password', render: () => [accountHero(), section('Account settings',
      L('accounts/info', 'person', 'Your info', 'Profile photo'),
      L('accounts/sign-in', 'lock-closed', 'Sign-in options', 'Windows Hello, security key, password, dynamic lock'),
      L('accounts/email', 'mail', 'Email & accounts', 'Accounts used by email, calendar, and contacts'),
      L('accounts/family', 'person', 'Family', 'Manage your family group, edit account types and device permissions'),
      L('accounts/backup', 'cloud', 'Windows backup', 'Back up your files, apps, preferences to restore them across devices'),
      L('accounts/other-users', 'person', 'Other users', 'Device access, work or school users, kiosk assigned access'),
      L('accounts/work', 'mail', 'Access work or school', 'Organization resources like email, apps, and network'))] },

    time: { title: 'Time & language', icon: 'globe', keywords: 'clock date region language keyboard', render: () => [timeHero(), list(
      L('time/date', 'calendar', 'Date & time', 'Time zones, automatic clock settings, calendar display'),
      L('time/language', 'globe', 'Language & region', 'Windows and some apps format dates and time based on your region'),
      L('time/typing', 'keyboard', 'Typing', 'Touch keyboard, text suggestions, preferences'),
      L('time/speech', 'mic', 'Speech', 'Speech language, speech recognition microphone setup, voices'))] },

    gaming: { title: 'Gaming', icon: 'games', keywords: 'game bar captures game mode', render: () => [list(
      L('gaming/game-bar', 'games', 'Game Bar', 'Controller and keyboard shortcuts'),
      L('gaming/captures', 'camera', 'Captures', 'Save location, recording preferences'),
      L('gaming/game-mode', 'games', 'Game Mode', 'Optimize your PC for play'))] },
    'gaming/game-mode': { title: 'Game Mode', icon: 'games', keywords: 'performance', render: () => [list(
      TL('gameMode', true, 'games', 'Game Mode', 'Optimize your PC for play by turning things off in the background')),
    section('Related settings', L('system/display/graphics', 'desktop', 'Graphics'))] },

    accessibility: { title: 'Accessibility', icon: 'accessibility', keywords: 'vision hearing narrator magnifier', render: () => [
      section('Vision',
        L('accessibility/text-size', 'text-font', 'Text size', 'Text size throughout Windows and your apps'),
        L('accessibility/visual-effects', 'brightness', 'Visual effects', 'Scrollbars, transparency, animations, notification timeout'),
        L('accessibility/mouse-pointer', 'select-all', 'Mouse pointer and touch', 'Mouse pointer color, size'),
        L('accessibility/text-cursor', 'text-font', 'Text cursor', 'Text cursor appearance and thickness, touch indicator'),
        L('accessibility/magnifier', 'zoom-in', 'Magnifier', 'Magnifier zoom level and increment, reading'),
        L('accessibility/color-filters', 'color', 'Color filters', 'Colorblindness filters, grayscale, inverted'),
        L('personalization/contrast', 'color', 'Contrast themes', 'Color themes for low vision, light sensitivity'),
        L('accessibility/narrator', 'speaker-2', 'Narrator', 'Voice, verbosity, keyboard, braille')),
      section('Hearing',
        L('accessibility/audio', 'speaker-1', 'Audio', 'Mono audio, audio notifications'),
        L('accessibility/captions', 'text-bullet-list', 'Captions', 'Styles')),
      section('Interaction',
        L('accessibility/speech', 'mic', 'Speech', 'Voice access, voice typing, Windows Speech Recognition'),
        L('accessibility/keyboard', 'keyboard', 'Keyboard', 'Sticky, Filter, and Toggle keys, on-screen keyboard'),
        L('accessibility/mouse', 'select-all', 'Mouse', 'Mouse keys, speed, acceleration'),
        L('accessibility/eye-control', 'person', 'Eye control', 'Eye tracker, text-to-speech'))] },
    'accessibility/visual-effects': { title: 'Visual effects', icon: 'brightness', keywords: 'animation effects transparency scrollbars motion', render: () => [list(
      TL('alwaysScrollbars', false, 'arrow-sort', 'Always show scrollbars'),
      T('transparency', 'square-multiple', 'Transparency effects'),
      T('animations', 'play', 'Animation effects'),
      { icon: 'timer', title: 'Dismiss notifications after this amount of time',
        ctl: () => comboCtl(loc('dismissAfter', 5), [[5, '5 seconds'], [7, '7 seconds'], [15, '15 seconds'], [30, '30 seconds'], [60, '1 minute'], [300, '5 minutes']], 'Dismiss notifications after this amount of time') })] },

    privacy: { title: 'Privacy & security', icon: 'shield', keywords: 'permissions location camera microphone', render: () => [
      section('Security',
        L('privacy/windows-security', 'shield', 'Windows Security', 'Antivirus, browser, firewall, and network protection for your device'),
        L('privacy/find-my-device', 'location', 'Find my device', 'Track your device if you think you\'ve lost it'),
        L('privacy/encryption', 'lock-closed', 'Device encryption', 'Help protect your data'),
        L('system/developers', 'wrench', 'For developers', 'These settings are intended for development use only')),
      section('Windows permissions',
        L('privacy/general', 'lock-closed', 'General', 'Advertising ID, local content, app launches, settings suggestion, productivity tools'),
        L('privacy/speech', 'mic', 'Speech', 'Online speech recognition for dictation and other voice-based interactions'),
        L('privacy/inking', 'edit', 'Inking & typing personalization', 'Custom dictionary, words in your dictionary'),
        L('privacy/diagnostics', 'document', 'Diagnostics & feedback', 'Diagnostic data, inking and typing data, tailored experiences, feedback frequency'),
        L('privacy/activity', 'history', 'Activity history', 'Options for getting more from your activity history across devices and accounts'),
        L('privacy/search', 'search', 'Search permissions', 'SafeSearch, cloud content search, search history'),
        L('privacy/searching', 'search', 'Searching Windows', 'Search indexing for faster results, Find My Files, folders excluded from search')),
      section('App permissions',
        L('privacy/location', 'location', 'Location'),
        L('privacy/camera', 'camera', 'Camera'),
        L('privacy/microphone', 'mic', 'Microphone'),
        L('privacy/voice', 'mic', 'Voice activation'),
        L('privacy/notifications', 'alert', 'Notifications'))] },

    update: { title: 'Windows Update', icon: 'arrow-sync', keywords: 'updates check for updates', render: updatePage },
  };

  /* Top-level NavigationView items: [id, label, glyph, category tint]. */
  const NAV = [
    ['home', 'Home', 'home', 'blue'], ['system', 'System', 'desktop', 'blue'], ['bluetooth', 'Bluetooth & devices', 'bluetooth', 'blue'],
    ['network', 'Network & internet', 'wifi', 'teal'], ['personalization', 'Personalization', 'paint-brush', 'orange'],
    ['apps', 'Apps', 'apps', 'purple'], ['accounts', 'Accounts', 'person', 'green'], ['time', 'Time & language', 'globe', 'blue'],
    ['gaming', 'Gaming', 'games', 'grey'], ['accessibility', 'Accessibility', 'accessibility', 'blue'],
    ['privacy', 'Privacy & security', 'shield', 'grey'], ['update', 'Windows Update', 'arrow-sync', 'blue'],
  ];

  /* Search's settings source: every page plus the individual settings people look for. */
  Shell.settingsIndex = [
    ...Object.entries(PAGES).map(([page, p]) => ({ title: p.title, page, keywords: p.keywords || '', icon: `fluent/${p.icon}` })),
    ...[
      ['Dark mode', 'personalization/colors', 'theme light mode color mode', 'color'],
      ['Accent color', 'personalization/colors', 'accent colour highlight', 'paint-brush'],
      ['Transparency effects', 'personalization/colors', 'transparent acrylic mica translucent', 'square-multiple'],
      ['Change the desktop background', 'personalization/background', 'wallpaper picture solid color', 'image'],
      ['Change brightness', 'system/display', 'brightness screen dim', 'brightness'],
      ['Change the volume', 'system/sound', 'volume sound mute speakers', 'speaker-2'],
      ['Do not disturb', 'system/notifications', 'dnd quiet hours focus', 'alert-off'],
      ['Animation effects', 'accessibility/visual-effects', 'animations motion reduce', 'play'],
      ['Taskbar alignment', 'personalization/taskbar', 'center left taskbar start', 'grid'],
      ['Automatically hide the taskbar', 'personalization/taskbar', 'auto hide autohide taskbar', 'grid'],
      ['Show badges on taskbar apps', 'personalization/taskbar', 'badge count unread taskbar', 'grid'],
      ['Show flashing on taskbar apps', 'personalization/taskbar', 'flash attention taskbar', 'grid'],
      ['Select the far corner of the taskbar to show the desktop', 'personalization/taskbar', 'show desktop peek corner', 'desktop'],
      ['Clipboard history', 'system/clipboard', 'clipboard win+v paste history copy', 'history'],
      ['Clear clipboard data', 'system/clipboard', 'clipboard clear delete history', 'delete'],
      ['View running processes with Task Manager', 'app:taskmgr', 'task manager taskmgr processes performance cpu memory end task', 'apps-list'],
      ['See which processes start up automatically when you start Windows', 'app:taskmgr/startup', 'task manager taskmgr startup apps', 'play'],
      ['Bluetooth', 'bluetooth', 'bluetooth pair devices', 'bluetooth'],
      ['Airplane mode', 'network', 'flight mode wireless', 'airplane'],
      ['Check for updates', 'update', 'windows update', 'arrow-sync'],
      ['Rename your PC', 'system', 'pc name computer name', 'rename'],
    ].map(([title, page, keywords, ic]) => ({ title, page, keywords, icon: `fluent/${ic}` })),
  ];

  /* ---------- window ---------- */
  let win = null;
  let app = null;   // DOM refs while the window is open
  let current = null;
  let backStack = [];
  let unsubs = [];

  function resolve(id) {
    if (!id) return 'home';
    id = String(id).replace(/^\/+|\/+$/g, '').toLowerCase();
    if (PAGES[id] || titles[id] || NAV.some(([n]) => id.split('/')[0] === n && id.includes('/'))) return id;
    const hit = Object.keys(PAGES).find((k) => k.split('/').pop() === id);
    return hit || 'home';
  }

  function build() {
    const back = h('button', { class: 'stg-tb-btn', type: 'button', 'aria-label': 'Back', 'data-tooltip': 'Back', onclick: goBack }, icon('arrow-left', 16));
    const burger = h('button', { class: 'stg-tb-btn stg-burger', type: 'button', 'aria-label': 'Open navigation', 'data-tooltip': 'Open navigation', onclick: () => togglePane() }, icon('navigation', 16));
    const titlebar = h('div', { class: 'stg-titlebar' }, back, burger,
      h('span', { class: 'window-titlebar-icon stg-tb-icon' }, Shell.icon('object/app-settings', { size: 16 })),
      h('span', { class: 'window-title stg-tb-title', text: 'Settings' }));

    const search = h('input', { class: 'textbox searchbox stg-search-input', type: 'text', placeholder: 'Find a setting', 'aria-label': 'Find a setting',
      role: 'combobox', 'aria-autocomplete': 'list', 'aria-expanded': 'false', spellcheck: 'false' });
    Shell.ui?.textbox?.(search, { clear: true });
    const nav = h('div', { class: 'stg-nav', role: 'navigation', 'aria-label': 'Settings' },
      NAV.map(([id, label, glyph, tint]) => h('button', { class: 'stg-nav-item', type: 'button', 'data-page': id, 'data-tint': tint, onclick: () => go(id, { anim: 'entrance' }) },
        h('span', { class: 'stg-nav-icon' }, icon(glyph, 16)), h('span', { class: 'stg-nav-label', text: label }))),
      h('span', { class: 'stg-pill', 'aria-hidden': 'true' }));
    nav.addEventListener('keydown', (e) => {
      if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
      const items = [...nav.querySelectorAll('.stg-nav-item')];
      const i = items.indexOf(document.activeElement);
      if (i < 0) return;
      e.preventDefault();
      items[(i + (e.key === 'ArrowDown' ? 1 : -1) + items.length) % items.length].focus();
    });
    const account = h('button', { class: 'stg-account', type: 'button', onclick: () => go('accounts', { anim: 'entrance' }) },
      Shell.icon('object/user-avatar', { size: 64, className: 'stg-avatar' }),
      h('span', { class: 'stg-account-text' }, h('span', { class: 'stg-account-name', text: 'Mihawk' }), h('span', { class: 'stg-account-sub', text: 'Local account' })));
    const pane = h('div', { class: 'stg-pane' }, account, h('div', { class: 'stg-search' }, search), nav);
    const head = h('div', { class: 'stg-head' });
    const scroll = h('div', { class: 'stg-scroll' });
    const page = h('div', { class: 'stg-page' }, head, scroll);
    const main = h('div', { class: 'stg-main' }, page);
    const popup = h('div', { class: 'stg-suggest acrylic surface-flyout', role: 'listbox', hidden: true });
    const root = h('div', { class: 'stg-app' }, pane, main, popup);
    app = { root, titlebar, back, burger, pane, nav, pill: nav.querySelector('.stg-pill'), search, popup, head, scroll, page, main, narrow: false };

    wireSearch();
    /* The titlebar (hamburger, back) is mounted by wm outside root, so it needs the same keys. */
    const onKey = (e) => {
      if (e.altKey && e.key === 'ArrowLeft') { e.preventDefault(); goBack(); }
      if (e.key === 'Escape' && root.classList.contains('pane-open')) { e.stopPropagation(); togglePane(false); burger.focus(); }
    };
    root.addEventListener('keydown', onKey);
    titlebar.addEventListener('keydown', onKey);
    main.addEventListener('pointerdown', () => { if (root.classList.contains('pane-open')) togglePane(false); });

    /* NavigationView display modes: expanded at 1008 px and wider, minimal (hamburger) below. */
    const ro = new ResizeObserver(() => {
      const narrow = root.clientWidth < 1008;
      if (narrow === app.narrow) return;
      app.narrow = narrow;
      root.classList.toggle('narrow', narrow);
      burger.hidden = !narrow;
      if (!narrow) togglePane(false);
    });
    ro.observe(root);
    burger.hidden = true;
    unsubs.push(() => ro.disconnect());
    unsubs.push(Shell.on('setting', ({ key }) => (watchers.get(key) || []).forEach((fn) => fn())));
  }

  function togglePane(force) {
    const open = force ?? !app.root.classList.contains('pane-open');
    if (open === app.root.classList.contains('pane-open')) return;
    app.root.classList.toggle('pane-open', open);
    app.burger.setAttribute('aria-expanded', String(open));
  }

  function placePill(animate) {
    const item = app.nav.querySelector(`.stg-nav-item[data-page="${current.split('/')[0]}"]`);
    const pill = app.pill;
    if (!item) { pill.hidden = true; return; }
    const to = item.offsetTop + (item.offsetHeight - 16) / 2;
    const from = parseFloat(pill.style.top);
    pill.hidden = false;
    pill.style.top = `${to}px`;
    if (!animate || Number.isNaN(from) || from === to) return;
    /* WinUI stretches the indicator toward the new item, then settles it there. */
    const mid = Math.min(from, to) + Math.abs(to - from) / 2 - Math.min(Math.abs(to - from), 48) / 2;
    Shell.animate(pill, [
      { top: `${from}px`, height: '16px' },
      { top: `${mid}px`, height: `${16 + Math.min(Math.abs(to - from), 48)}px`, offset: 0.5 },
      { top: `${to}px`, height: '16px' },
    ], { duration: 333, easing: Shell.ease.standard });
  }

  function breadcrumb(id) {
    const parts = id.split('/');
    const crumbs = parts.map((_, i) => parts.slice(0, i + 1).join('/'));
    const title = (p) => PAGES[p]?.title || titles[p] || (p.split('/').pop().charAt(0).toUpperCase() + p.split('/').pop().slice(1).replace(/-/g, ' '));
    const nodes = [];
    crumbs.forEach((p, i) => {
      const last = i === crumbs.length - 1;
      if (i) nodes.push(h('span', { class: 'stg-crumb-sep', 'aria-hidden': 'true' }, icon('chevron-right', 16)));
      nodes.push(last ? h('span', { class: 'stg-crumb-current', text: title(p) })
        : h('button', { class: 'stg-crumb', type: 'button', text: title(p), onclick: () => go(p, { anim: 'back' }) }));
    });
    return h('h1', { class: 'stg-title' }, nodes);
  }

  function render(anim) {
    cleanupPage();
    const page = PAGES[current];
    const nodes = page ? page.render() : [h('p', { class: 'stg-empty', text: 'These settings aren\'t part of this preview.' })];
    app.head.replaceChildren(breadcrumb(current));
    app.scroll.replaceChildren(h('div', { class: 'stg-col' }, nodes));
    app.scroll.scrollTop = 0;
    const kf = {
      entrance: [{ opacity: 0, transform: 'translateY(40px)' }, { opacity: 1, transform: 'none' }],
      drill: [{ opacity: 0, transform: 'scale(0.96)' }, { opacity: 1, transform: 'none' }],
      back: [{ opacity: 0, transform: 'scale(1.04)' }, { opacity: 1, transform: 'none' }],
    }[anim];
    if (kf) Shell.animate(app.page, kf, { duration: anim === 'entrance' ? 333 : 250 });
  }

  function go(id, { anim = 'drill', push = true } = {}) {
    if (!app) return;
    id = resolve(id);
    if (id === current) { togglePane(false); return; }
    if (push && current) backStack.push(current);
    if (backStack.length > 50) backStack.shift();
    current = id;
    app.nav.querySelectorAll('.stg-nav-item').forEach((n) => {
      if (n.dataset.page === id.split('/')[0]) n.setAttribute('aria-current', 'page'); else n.removeAttribute('aria-current');
    });
    app.back.disabled = !backStack.length;
    togglePane(false);
    closeSuggest();
    render(anim);
    placePill(anim !== 'none');
  }

  function goBack() {
    if (!backStack.length) return;
    const prev = backStack.pop();
    current = null; // force render
    go(prev, { anim: 'back', push: false });
  }

  /* ---------- Find a setting ---------- */
  let suggestions = [], active = -1;
  const searchIndex = (q) => {
    const s = q.trim().toLowerCase();
    if (!s) return [];
    const seen = new Set();
    return Shell.settingsIndex
      .filter((e) => e.title.toLowerCase().includes(s) || e.keywords.toLowerCase().includes(s))
      .sort((a, b) => (b.title.toLowerCase().startsWith(s) - a.title.toLowerCase().startsWith(s)) || a.title.localeCompare(b.title))
      .filter((e) => !seen.has(e.title) && seen.add(e.title))
      .slice(0, 8);
  };
  function wireSearch() {
    const { search } = app;
    search.addEventListener('input', () => showSuggest(search.value));
    search.addEventListener('focus', () => { if (search.value) showSuggest(search.value); });
    search.addEventListener('blur', () => setTimeout(closeSuggest, 120));
    search.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
        if (!suggestions.length) return;
        active = (active + (e.key === 'ArrowDown' ? 1 : -1) + suggestions.length) % suggestions.length;
        paintActive();
      } else if (e.key === 'Enter') { e.preventDefault(); submitSearch(); }
      else if (e.key === 'Escape') { e.stopPropagation(); if (!app.popup.hidden) closeSuggest(); else search.value = ''; }
    });
  }
  function showSuggest(q) {
    const { popup, search, root } = app;
    suggestions = searchIndex(q);
    active = -1;
    if (!q.trim()) { closeSuggest(); return; }
    popup.replaceChildren(...(suggestions.length ? suggestions.map((e, i) => h('div', { class: 'stg-suggest-item', role: 'option', id: `stg-sg-${i}`,
      onpointerdown: (ev) => ev.preventDefault(), onclick: () => pickSuggestion(e) }, icon(e.icon, 16), h('span', { text: e.title })))
      : [h('div', { class: 'stg-suggest-empty', text: 'No results found' })]));
    const r = search.getBoundingClientRect(), rr = root.getBoundingClientRect();
    popup.style.left = `${r.left - rr.left}px`;
    popup.style.top = `${r.bottom - rr.top + 4}px`;
    popup.style.width = `${Math.max(r.width, 280)}px`;
    if (popup.hidden) {
      popup.hidden = false;
      Shell.animate(popup, [{ opacity: 0, transform: 'translateY(-8px)' }, { opacity: 1, transform: 'none' }], { duration: 167 });
    }
    search.setAttribute('aria-expanded', 'true');
  }
  function paintActive() {
    app.popup.querySelectorAll('.stg-suggest-item').forEach((n, i) => n.classList.toggle('active', i === active));
    app.search.setAttribute('aria-activedescendant', active >= 0 ? `stg-sg-${active}` : '');
  }
  function closeSuggest() {
    if (!app || app.popup.hidden) return;
    app.popup.hidden = true;
    app.search.setAttribute('aria-expanded', 'false');
  }
  function pickSuggestion(e) {
    app.search.value = '';
    closeSuggest();
    if (!launchExternal(e.page)) go(e.page, { anim: 'drill' });
  }
  function submitSearch() {
    const hit = suggestions[active] || suggestions[0];
    if (hit) pickSuggestion(hit);
  }

  /* Windows Settings is single-instance: launching again focuses it and opens the page. */
  function open(args = {}) {
    if (launchExternal(args.page)) return null;
    if (win) {
      if (win.isMinimized?.()) win.restore?.();
      win.focus?.();
      if (args.page) go(args.page, { anim: 'drill' });
      return win;
    }
    build();
    const wa = Shell.wm?.workArea?.() || { width: window.innerWidth, height: window.innerHeight - 48 };
    const opts = {
      appId: 'settings', title: 'Settings', icon: 'object/app-settings', titlebar: 'tall', titlebarContent: app.titlebar,
      width: Math.min(1180, wa.width - 48), height: Math.min(800, wa.height - 48),
      minWidth: Math.min(500, wa.width), minHeight: Math.min(420, wa.height), background: 'mica', content: app.root,
      onClose: teardown,
    };
    win = Shell.wm?.open ? Shell.wm.open(opts) : fallbackWindow(opts);
    current = null;
    backStack = [];
    go(args.page || 'home', { anim: 'none', push: false });
    if (wa.width < 720) win.maximize?.();
    requestAnimationFrame(() => placePill(false));
    return win;
  }

  function teardown() {
    cleanupPage();
    closeDialog(true);
    unsubs.forEach((fn) => fn());
    unsubs = [];
    win = null;
    app = null;
    current = null;
  }

  /* Without the window manager (module run alone) the app still mounts, plainly framed. */
  function fallbackWindow(o) {
    const el = h('section', { class: 'stg-fallback mica', style: `width:${o.width}px;height:${o.height}px` },
      h('div', { class: 'stg-fallback-bar' }, o.titlebarContent,
        h('button', { class: 'stg-tb-btn', type: 'button', 'aria-label': 'Close', onclick: () => handle.close() }, icon('dismiss', 16))),
      o.content);
    Shell.layer('windows').append(el);
    const handle = { el, body: el, setTitle() {}, focus() {}, close() { if (o.onClose() !== false) el.remove(); } };
    return handle;
  }

  /* ---------- registration ---------- */
  Shell.apps.register({ id: 'settings', name: 'Settings', icon: 'object/app-settings', pinned: true, startPinned: true, description: 'System', open });
  Shell.keys.on('Win+I', () => { Shell.apps.launch('settings'); });
  Shell.on('boot', () => {
    const d = Shell.debugOpen;
    if (d.includes('settings')) Shell.apps.launch('settings', { page: new URLSearchParams(location.search).get('settingsPage') || 'personalization/colors' });
    if (d.includes('settings-clipboard')) Shell.apps.launch('settings', { page: 'system/clipboard' });
    if (d.includes('settings-taskbar')) {
      expanded.set('personalization/taskbar|Taskbar behaviors', true);
      Shell.apps.launch('settings', { page: 'personalization/taskbar' });
      requestAnimationFrame(() => { // bring the behaviors card (the last expander) to the top
        const exp = app && [...app.scroll.querySelectorAll('.stg-exp')].pop();
        if (exp) app.scroll.scrollTop += exp.getBoundingClientRect().top - app.scroll.getBoundingClientRect().top;
      });
    }
  });
})();
