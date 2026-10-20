/* Shell core: the one shared runtime every surface module builds on.
   Owns: event bus, persisted settings (theme, accent, wallpaper), the wallpaper and its
   Mica/noise derivatives, the icon loader, the app registry, the virtual file system,
   global keyboard shortcuts, layer containers and light-dismiss bookkeeping.
   Contract: CONTRACT.md. Modules never reach into each other's DOM; they talk through Shell. */
(() => {
  'use strict';

  const Shell = (window.Shell = window.Shell || {});

  /* ---------- event bus ---------- */
  const handlers = new Map();
  Shell.on = (name, fn) => {
    if (!handlers.has(name)) handlers.set(name, new Set());
    handlers.get(name).add(fn);
    return () => handlers.get(name).delete(fn);
  };
  Shell.emit = (name, detail) => {
    (handlers.get(name) || []).forEach((fn) => {
      try { fn(detail); } catch (e) { console.error(`[Shell] handler for ${name} failed`, e); }
    });
  };

  /* ---------- persisted state ---------- */
  const STORE_KEY = 'w11poc:';
  const fresh = new URLSearchParams(location.search).has('fresh'); // captures: defaults, nothing saved
  Shell.store = {
    get(key, fallback) {
      if (fresh) return fallback;
      try {
        const v = localStorage.getItem(STORE_KEY + key);
        return v === null ? fallback : JSON.parse(v);
      } catch { return fallback; }
    },
    set(key, value) {
      if (fresh) return;
      try { localStorage.setItem(STORE_KEY + key, JSON.stringify(value)); } catch { /* private window */ }
    },
    reset() {
      try {
        Object.keys(localStorage).filter((k) => k.startsWith(STORE_KEY)).forEach((k) => localStorage.removeItem(k));
      } catch { /* ignore */ }
    },
  };

  /* ---------- reduced motion ---------- */
  const motionQuery = window.matchMedia('(prefers-reduced-motion: reduce)');
  Shell.reducedMotion = () => motionQuery.matches || Shell.settings.animations === false;

  /* Web Animations helper with Windows' curves. Resolves when done (immediately if reduced). */
  const EASE = { in: 'cubic-bezier(0, 0, 0, 1)', out: 'cubic-bezier(1, 0, 1, 1)', standard: 'cubic-bezier(0.8, 0, 0.2, 1)' };
  Shell.ease = EASE;
  Shell.animate = (el, keyframes, opts = {}) => {
    const o = { duration: 250, easing: EASE.in, fill: 'none', ...opts };
    if (Shell.reducedMotion()) o.duration = o.delay = o.endDelay = 0; // final state at once, no staggered pop-in
    const a = el.animate(keyframes, o);
    return a.finished.catch(() => {});
  };

  /* ---------- settings: theme, accent, wallpaper ---------- */
  const defaults = {
    theme: 'light',
    accent: '#0078d4',
    accentAuto: false,
    wallpaper: 'meadow-light',
    transparency: true,
    animations: true,
    showDesktopIcons: true,
    iconSize: 'medium',
    autoArrange: false,
    alignToGrid: true,
    taskbarAlignment: 'center',
    volume: 62,
    brightness: 80,
    wifi: true,
    bluetooth: true,
    airplane: false,
    nightLight: false,
    energySaver: false,
    doNotDisturb: false,
  };
  Shell.settings = { ...defaults, ...Shell.store.get('settings', {}) };
  Shell.set = (key, value) => {
    Shell.settings[key] = value;
    Shell.store.set('settings', Shell.settings);
    applySetting(key);
    Shell.emit('setting', { key, value });
  };

  /* Wallpapers: "meadow-light" is the project's own Aero Meadow, the picture every build stages as
     the desktop's proton.bmp, and "meadow-dark" the same meadow at night; serve.sh draws both into
     local/ with tools/make-mihawk-wallpaper.py, and each has a gradient until (or unless) it loads.
     The rest are authored here as gradients so the POC has something to switch to.
     Each entry: {id, name, css, image?, mica? (Mica source when there is no image), solid?}. */
  Shell.wallpapers = [
    { id: 'meadow-light', name: 'Aero Meadow', image: 'local/wallpaper.png', css: 'linear-gradient(180deg, #2178e0 0%, #6bbdfa 45%, #e0f7ff 76%, #8fd14f 80%, #339415 100%)' },
    { id: 'meadow-dark', name: 'Aero Meadow (night)', image: 'local/wallpaper-dark.png', css: 'linear-gradient(180deg, #050a21 0%, #0a1a45 45%, #21456b 76%, #1c4a2a 80%, #05230a 100%)' },
    { id: 'glow', name: 'Violet dusk gradient', css: 'radial-gradient(70% 60% at 30% 70%, #f6b48f 0%, transparent 70%), radial-gradient(80% 70% at 75% 30%, #9a8cf0 0%, transparent 70%), linear-gradient(160deg, #3b2b6e, #e7826d)' },
    { id: 'captured-motion', name: 'Teal and amber gradient', css: 'conic-gradient(from 210deg at 40% 60%, #0f4c5c, #2f8f83, #e3b04b, #c0503f, #0f4c5c)' },
    { id: 'sunrise', name: 'Sunrise gradient', css: 'linear-gradient(180deg, #ffd7a8 0%, #f6a37a 35%, #9c6fb6 70%, #3e4a8c 100%)' },
    { id: 'flow', name: 'Cyan and violet gradient', css: 'radial-gradient(90% 80% at 80% 80%, #49c6e5 0%, transparent 60%), radial-gradient(80% 70% at 20% 20%, #7b5cff 0%, transparent 60%), #1b1f4b' },
    { id: 'solid-blue', name: 'Solid colour', css: '#0063b1', solid: true },
    { id: 'solid-dark', name: 'Solid dark', css: '#1f1f1f', solid: true },
  ];

  const root = document.documentElement;

  function hexToHsl(hex) {
    const n = parseInt(hex.slice(1), 16);
    let r = ((n >> 16) & 255) / 255, g = ((n >> 8) & 255) / 255, b = (n & 255) / 255;
    const max = Math.max(r, g, b), min = Math.min(r, g, b);
    let h = 0, s = 0; const l = (max + min) / 2;
    if (max !== min) {
      const d = max - min;
      s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
      h = max === r ? (g - b) / d + (g < b ? 6 : 0) : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
      h *= 60;
    }
    return [h, s * 100, l * 100];
  }
  const hsl = (h, s, l) => `hsl(${h.toFixed(1)} ${Math.min(100, s).toFixed(1)}% ${Math.max(0, Math.min(100, l)).toFixed(1)}%)`;

  /* Windows derives six shades from one accent; this approximates its ramp in HSL so the
     default #0078D4 lands near the shipped values (Light2 #60CDFF, Dark1 #005FB8). */
  function applyAccent(hex) {
    if (hex.toLowerCase() === '#0078d4') {
      ['', 'Light1', 'Light2', 'Light3', 'Dark1', 'Dark2', 'Dark3'].forEach((k) => root.style.removeProperty(`--SystemAccentColor${k}`));
      return;
    }
    const [h, s, l] = hexToHsl(hex);
    root.style.setProperty('--SystemAccentColor', hex);
    root.style.setProperty('--SystemAccentColorLight1', hsl(h, s + 8, l + 12));
    root.style.setProperty('--SystemAccentColorLight2', hsl(h, s + 12, l + 27));
    root.style.setProperty('--SystemAccentColorLight3', hsl(h, s + 14, l + 38));
    root.style.setProperty('--SystemAccentColorDark1', hsl(h, s, l - 7));
    root.style.setProperty('--SystemAccentColorDark2', hsl(h, s, l - 17));
    root.style.setProperty('--SystemAccentColorDark3', hsl(h, s, l - 28));
  }

  /* Noise texture for Acrylic, generated once (no shipped raster). */
  function makeNoise() {
    const c = document.createElement('canvas');
    c.width = c.height = 128;
    const ctx = c.getContext('2d');
    const img = ctx.createImageData(128, 128);
    for (let i = 0; i < img.data.length; i += 4) {
      const v = Math.random() * 255;
      img.data[i] = img.data[i + 1] = img.data[i + 2] = v;
      img.data[i + 3] = 255;
    }
    ctx.putImageData(img, 0, 0);
    root.style.setProperty('--noise', `url(${c.toDataURL('image/png')})`);
  }

  /* Mica samples only the wallpaper: render a heavily blurred copy once per wallpaper and
     expose it as --mica-image for `background-attachment: fixed` on windows and the taskbar. */
  function micaOf(img) {
    const w = 240, h = Math.round((240 * (img.naturalHeight || img.height)) / (img.naturalWidth || img.width));
    const c = document.createElement('canvas');
    c.width = w; c.height = h;
    const ctx = c.getContext('2d');
    ctx.filter = 'blur(12px) saturate(140%)';
    ctx.drawImage(img, -24, -24, w + 48, h + 48);
    return `url(${c.toDataURL('image/jpeg', 0.85)})`;
  }
  let wallpaperToken = 0;
  function applyWallpaper(id) {
    const wp = Shell.wallpapers.find((w) => w.id === id) || Shell.wallpapers[0];
    const token = ++wallpaperToken;
    const setCss = (bg, mica) => {
      root.style.setProperty('--wallpaper', bg);
      root.style.setProperty('--mica-image', mica);
      Shell.emit('wallpaper', wp);
    };
    if (!wp.image) { setCss(wp.css, wp.mica || wp.css); return; }
    const img = new Image();
    img.onload = () => {
      if (token !== wallpaperToken) return;
      /* absolute: a relative url() inside a custom property resolves against the using stylesheet */
      setCss(`url("${new URL(wp.image, location.href).href}") center / cover no-repeat`, micaOf(img));
    };
    img.onerror = () => { if (token === wallpaperToken) setCss(wp.css, wp.mica || wp.css); };
    img.src = wp.image;
  }

  function applySetting(key) {
    const s = Shell.settings;
    if (key === 'theme' || key === '*') root.dataset.theme = s.theme;
    if (key === 'accent' || key === '*') applyAccent(s.accent);
    if (key === 'wallpaper' || key === '*') applyWallpaper(s.wallpaper);
    if (key === 'transparency' || key === '*') root.dataset.transparency = s.transparency ? 'on' : 'off';
    if (key === 'animations' || key === '*') root.dataset.animations = s.animations ? 'on' : 'off';
    if (key === 'taskbarAlignment' || key === '*') root.dataset.taskbarAlign = s.taskbarAlignment;
  }

  /* ---------- icons ---------- */
  /* Shell.icon(name, {size}) -> HTMLElement. Names: "fluent/<name>" (20 px line glyphs,
     currentColor) or "object/<name>" (colour object icons). See assets/icons/manifest.json. */
  const svgCache = new Map();
  const FLUENT_BASE = 'assets/icons/fluent/';
  const OBJECT_BASE = 'assets/icons/object/';
  async function loadSvg(url) {
    if (!svgCache.has(url)) {
      svgCache.set(url, fetch(url).then((r) => (r.ok ? r.text() : Promise.reject(new Error(`${url}: ${r.status}`)))));
    }
    return svgCache.get(url);
  }
  Shell.iconUrl = (name) => {
    const [kind, id] = name.includes('/') ? name.split('/') : ['fluent', name];
    return (kind === 'object' ? OBJECT_BASE : FLUENT_BASE) + id + '.svg';
  };
  Shell.icon = (name, { size = 16, className = '' } = {}) => {
    const span = document.createElement('span');
    span.className = `icon ${className}`.trim();
    span.style.width = span.style.height = `${size}px`;
    span.setAttribute('aria-hidden', 'true');
    const url = Shell.iconUrl(name);
    if (name.startsWith('object/')) {
      const img = document.createElement('img');
      img.src = url; img.alt = ''; img.draggable = false;
      img.width = img.height = size;
      span.appendChild(img);
    } else {
      loadSvg(url).then((text) => {
        span.innerHTML = text.replace(/fill="#(212121|000000|000)"/gi, 'fill="currentColor"');
        const svg = span.querySelector('svg');
        if (svg) {
          svg.setAttribute('width', size); svg.setAttribute('height', size);
          /* Segoe Fluent Icons at N px fill an N px em; the 20 px Fluent files keep their glyph in
             the central 16 px, so draw that live area 1:1 (same extent and 1 px strokes as Windows). */
          if (svg.getAttribute('viewBox') === '0 0 20 20') { svg.setAttribute('viewBox', '2 2 16 16'); svg.style.overflow = 'visible'; }
        }
      }).catch((e) => { console.warn('[Shell] missing icon', name, e.message); span.classList.add('icon-missing'); });
    }
    return span;
  };

  /* ---------- app registry ---------- */
  const apps = new Map();
  Shell.apps = {
    register(app) { apps.set(app.id, app); Shell.emit('apps', [...apps.values()]); },
    get: (id) => apps.get(id),
    list: () => [...apps.values()],
    launch(id, args) {
      const app = apps.get(id);
      if (!app) { console.warn('[Shell] unknown app', id); return null; }
      Shell.emit('app-launch', { id, args });
      return app.open ? app.open(args || {}) : null;
    },
  };

  /* ---------- virtual file system ---------- */
  /* Items: {id, name, kind: 'folder'|'file'|'shortcut'|'drive'|'system', parent, icon,
     ext?, size?, modified (ms), target? (app id or item id for shortcuts), args?}.
     Synthetic content: names and sizes are demonstration data, not the user's files. */
  const DAY = 86400000;
  const now = Date.UTC(2026, 9, 15, 14, 30);
  const seed = () => {
    const items = [];
    /* Stable demo dates: a name carrying a date ("Demo Game 2026-10-13 19-48.png") is dated by it;
       everything else gets a fixed day and time of day derived from its id, so lists never reshuffle. */
    const stamp = (o) => {
      const m = /(\d{4})-(\d{2})-(\d{2}) (\d{2})-(\d{2})/.exec(o.name || '');
      if (m) return new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5]).getTime();
      let h = 2166136261; for (const ch of o.id) h = Math.imul(h ^ ch.charCodeAt(0), 16777619) >>> 0; // FNV-1a: neighbours scatter
      const day = new Date(now - (2 + (h % 38)) * DAY); day.setHours(0, 0, 0, 0);
      return day.getTime() + (8 * 60 + ((h >>> 8) % (14 * 60))) * 60000; // between 8:00 and 22:00
    };
    const add = (o) => { items.push({ modified: stamp(o), ...o }); return o.id; };
    add({ id: 'root', name: 'This PC', kind: 'system', parent: null, icon: 'object/this-pc' });
    add({ id: 'recycle', name: 'Recycle Bin', kind: 'system', parent: null, icon: 'object/recycle-empty' });
    add({ id: 'home', name: 'Home', kind: 'system', parent: null, icon: 'fluent/home' });
    add({ id: 'c', name: 'Local Disk (C:)', kind: 'drive', parent: 'root', icon: 'object/drive', size: 236e9, free: 98.4e9 });
    add({ id: 'd', name: 'PS5 Data (D:)', kind: 'drive', parent: 'root', icon: 'object/drive', size: 825e9, free: 412e9 });
    const known = [
      ['desktop', 'Desktop', 'object/desktop'], ['documents', 'Documents', 'object/documents'],
      ['downloads', 'Downloads', 'object/downloads'], ['pictures', 'Pictures', 'object/pictures'],
      ['music', 'Music', 'object/music'], ['videos', 'Videos', 'object/videos'], ['games', 'Games', 'object/games'],
    ];
    known.forEach(([id, name, icon]) => add({ id, name, kind: 'folder', parent: 'root', icon, known: true }));
    add({ id: 'sc-game', name: 'Example Game', kind: 'shortcut', parent: 'desktop', icon: 'object/app-game', target: 'game', args: { title: 'Example Game' } });
    add({ id: 'sc-demo', name: 'Demo Game', kind: 'shortcut', parent: 'desktop', icon: 'object/app-game', target: 'game', args: { title: 'Demo Game' } });
    add({ id: 'sc-launcher', name: 'Game Launcher', kind: 'shortcut', parent: 'desktop', icon: 'object/app-store', target: 'game', args: { title: 'Game Launcher' } });
    add({ id: 'sc-notepad', name: 'Notepad', kind: 'shortcut', parent: 'desktop', icon: 'object/app-notepad', target: 'notepad' });
    add({ id: 'f-readme', name: 'Read me.txt', kind: 'file', ext: 'txt', parent: 'desktop', icon: 'object/document', size: 2140, content: 'Welcome to Mihawk OS.\n\nThis is a prototype of the Mihawk 11 desktop for the PS5 Wine runtime.' });
    add({ id: 'f-shots', name: 'Screenshots', kind: 'folder', parent: 'pictures', icon: 'object/folder' });
    add({ id: 'f-wall', name: 'Wallpapers', kind: 'folder', parent: 'pictures', icon: 'object/folder' });
    ['Example Game 2026-10-14 21-03.png', 'Demo Game 2026-10-13 19-48.png', 'Desktop 2026-10-12 07-07.png'].forEach((n, i) =>
      add({ id: `f-shot${i}`, name: n, kind: 'file', ext: 'png', parent: 'f-shots', icon: 'object/image', size: 1.4e6 + i * 3e5 }));
    [['Budget 2026.xlsx', 'xlsx', 48e3], ['Notes.txt', 'txt', 3.2e3], ['Controller mapping.pdf', 'pdf', 412e3], ['Save backups', null, 0]].forEach(([n, ext, size], i) =>
      add(ext ? { id: `f-doc${i}`, name: n, kind: 'file', ext, parent: 'documents', icon: 'object/document', size } : { id: `f-doc${i}`, name: n, kind: 'folder', parent: 'documents', icon: 'object/folder' }));
    [['proton-ge-custom.tar.gz', 'gz', 418e6], ['Selawik.zip', 'zip', 1.1e6], ['setup.exe', 'exe', 92e6]].forEach(([n, ext, size], i) =>
      add({ id: `f-dl${i}`, name: n, kind: 'file', ext, parent: 'downloads', icon: ext === 'exe' ? 'object/app-generic' : 'object/document', size }));
    [['Example Game', 'game'], ['Demo Game', 'game'], ['Sample Game', 'game']].forEach(([n], i) =>
      add({ id: `f-game${i}`, name: n, kind: 'folder', parent: 'games', icon: 'object/folder' }));
    ['Windows', 'Program Files', 'Program Files (x86)', 'Users'].forEach((n, i) => add({ id: `f-c${i}`, name: n, kind: 'folder', parent: 'c', icon: 'object/folder' }));
    ['homebrew', 'Wine prefixes', 'Games'].forEach((n, i) => add({ id: `f-d${i}`, name: n, kind: 'folder', parent: 'd', icon: 'object/folder' }));
    return items;
  };
  let fsItems = Shell.store.get('fs', null) || seed();
  const saveFs = () => Shell.store.set('fs', fsItems);
  let nextId = Date.now();
  Shell.fs = {
    get: (id) => fsItems.find((i) => i.id === id) || null,
    children: (parent) => fsItems.filter((i) => i.parent === parent && !i.deleted),
    path(id) { const out = []; let it = Shell.fs.get(id); while (it) { out.unshift(it); it = it.parent ? Shell.fs.get(it.parent) : null; } return out; },
    search: (q) => { const s = q.trim().toLowerCase(); return s ? fsItems.filter((i) => !i.deleted && i.name.toLowerCase().includes(s)) : []; },
    uniqueName(parent, base, ext) {
      const names = new Set(Shell.fs.children(parent).map((i) => i.name.toLowerCase()));
      const mk = (n) => (ext ? `${n}.${ext}` : n);
      if (!names.has(mk(base).toLowerCase())) return mk(base);
      for (let k = 2; ; k++) if (!names.has(mk(`${base} (${k})`).toLowerCase())) return mk(`${base} (${k})`);
    },
    create(parent, props) {
      const item = { id: `n${nextId++}`, parent, modified: Date.now(), size: 0, ...props };
      fsItems.push(item); saveFs(); Shell.emit('fs', { type: 'create', item }); return item;
    },
    rename(id, name) {
      const it = Shell.fs.get(id); if (!it || !name.trim()) return false;
      it.name = name.trim(); it.modified = Date.now(); saveFs(); Shell.emit('fs', { type: 'rename', item: it }); return true;
    },
    move(id, parent) {
      const it = Shell.fs.get(id); if (!it) return false;
      it.parent = parent; saveFs(); Shell.emit('fs', { type: 'move', item: it }); return true;
    },
    remove(id) { /* to the Recycle Bin, restorable */
      const it = Shell.fs.get(id); if (!it) return false;
      it.deletedFrom = it.parent; it.parent = 'recycle'; it.modified = Date.now(); saveFs();
      Shell.emit('fs', { type: 'remove', item: it }); return true;
    },
    restore(id) {
      const it = Shell.fs.get(id); if (!it || !it.deletedFrom) return false;
      it.parent = it.deletedFrom; delete it.deletedFrom; saveFs(); Shell.emit('fs', { type: 'restore', item: it }); return true;
    },
    /* Duplicates a file or folder (with its contents) into parent; a name already taken there
       gets Windows' " - Copy" suffix. Refuses system items and copies into the item itself. */
    copy(id, parent) {
      const src = Shell.fs.get(id);
      if (!src || src.kind === 'system' || src.kind === 'drive' || Shell.fs.path(parent).some((i) => i.id === id)) return null;
      const { id: _id, parent: _parent, deletedFrom: _from, known: _known, ...props } = src;
      const taken = Shell.fs.children(parent).some((i) => i.name.toLowerCase() === src.name.toLowerCase());
      const stem = src.ext && src.name.toLowerCase().endsWith(`.${src.ext}`.toLowerCase()) ? src.name.slice(0, -src.ext.length - 1) : src.name;
      const name = taken ? Shell.fs.uniqueName(parent, `${stem} - Copy`, stem === src.name ? null : src.ext) : src.name;
      const item = Shell.fs.create(parent, { ...props, name, modified: Date.now() });
      Shell.fs.children(id).forEach((child) => Shell.fs.copy(child.id, item.id));
      return item;
    },
    emptyRecycleBin() {
      fsItems = fsItems.filter((i) => i.parent !== 'recycle'); saveFs(); Shell.emit('fs', { type: 'empty' });
    },
    open(id) {
      const it = Shell.fs.get(id); if (!it) return;
      if (it.kind === 'shortcut') return Shell.apps.launch(it.target, { ...(it.args || {}), from: it.id });
      if (it.kind === 'folder' || it.kind === 'drive' || it.kind === 'system') return Shell.apps.launch('explorer', { folder: it.id });
      if (it.ext === 'txt') return Shell.apps.launch('notepad', { file: it.id });
      return Shell.apps.launch('explorer', { folder: it.parent, select: it.id });
    },
    formatSize(bytes) {
      if (bytes == null) return '';
      const u = ['bytes', 'KB', 'MB', 'GB', 'TB']; let i = 0, v = bytes;
      while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
      return i === 0 ? `${v} bytes` : `${v < 10 ? v.toFixed(1) : Math.round(v)} ${u[i]}`;
    },
    reset() { fsItems = seed(); saveFs(); Shell.emit('fs', { type: 'reset' }); },
  };

  /* ---------- file clipboard ---------- */
  /* One clipboard for every surface (desktop, Explorer): Cut or Copy there, Paste anywhere.
     Cut items are moved on paste and the clipboard empties, as Windows does; copied items are
     duplicated and stay on it. Emits `clipboard` {ids, cut}; surfaces dim items while cut. */
  let clip = { ids: [], cut: false };
  const pasteTarget = (parent) => ['folder', 'drive'].includes(Shell.fs.get(parent)?.kind) && parent !== 'recycle';
  Shell.clipboard = {
    set(ids, { cut = false } = {}) {
      clip = { ids: [...ids], cut: !!cut };
      Shell.emit('clipboard', { ...clip, ids: [...clip.ids] });
    },
    /* -> {ids, cut} with the items that still exist outside the Recycle Bin, or null when empty */
    get() {
      const ids = clip.ids.filter((id) => { const it = Shell.fs.get(id); return it && it.parent !== 'recycle'; });
      return ids.length ? { ids, cut: clip.cut } : null;
    },
    clear() { if (clip.ids.length) Shell.clipboard.set([]); },
    canPaste: (parent) => !!Shell.clipboard.get() && pasteTarget(parent),
    /* -> the pasted (moved or new) items */
    paste(parent) {
      const c = Shell.clipboard.get();
      if (!c || !pasteTarget(parent)) return [];
      if (!c.cut) return c.ids.map((id) => Shell.fs.copy(id, parent)).filter(Boolean);
      const moved = c.ids.filter((id) => Shell.fs.get(id).parent !== parent && !Shell.fs.path(parent).some((i) => i.id === id));
      moved.forEach((id) => {
        const it = Shell.fs.get(id);
        if (Shell.fs.children(parent).some((i) => i.name.toLowerCase() === it.name.toLowerCase())) {
          const stem = it.ext && it.name.toLowerCase().endsWith(`.${it.ext}`) ? it.name.slice(0, -it.ext.length - 1) : it.name;
          Shell.fs.rename(id, Shell.fs.uniqueName(parent, stem, stem === it.name ? null : it.ext));
        }
        Shell.fs.move(id, parent);
      });
      Shell.clipboard.set([]);
      return moved.map((id) => Shell.fs.get(id));
    },
  };

  /* ---------- keyboard ---------- */
  /* Shell.keys.on('Win+E', fn). Combos are written Ctrl+Alt+Shift+Win+Key with Key as
     KeyboardEvent.key (letters upper-case, 'Tab', 'Escape', 'ArrowLeft', ...). The browser
     often keeps Win and Alt+Tab for the OS; full screen with Keyboard Lock (Chromium) hands
     them to the page, and every Win shortcut also answers a fallback: Win becomes Ctrl+Alt
     (Win+E -> Ctrl+Alt+E), plus Shift when Ctrl or Alt is already held (Ctrl+Win+D ->
     Ctrl+Alt+Shift+D). An explicit registration always beats a derived fallback. */
  const keymap = new Map();
  const derived = new Set(); // fallback combos, which an explicit registration replaces
  const MODS = ['Ctrl', 'Alt', 'Shift', 'Win'];
  function fallbackOf(combo) {
    const mods = new Set();
    let key = combo;
    for (let m; (m = MODS.find((x) => key.startsWith(`${x}+`) && key.length > x.length + 1));) { mods.add(m); key = key.slice(m.length + 1); }
    if (!mods.delete('Win')) return null;
    if (mods.has('Ctrl') || mods.has('Alt')) { if (mods.has('Shift')) return null; mods.add('Shift'); }
    mods.add('Ctrl').add('Alt');
    return [...MODS.filter((m) => mods.has(m)), key].join('+');
  }
  const comboOf = (e) => {
    const parts = [];
    if (e.ctrlKey) parts.push('Ctrl');
    if (e.altKey) parts.push('Alt');
    if (e.shiftKey) parts.push('Shift');
    if (e.metaKey) parts.push('Win');
    let k = e.key.length === 1 ? e.key.toUpperCase() : e.key;
    if (k === 'Meta' || k === 'OS') k = 'Win';
    if (!['Control', 'Alt', 'Shift', 'Win'].includes(k)) parts.push(k);
    else if (k === 'Win' && parts[parts.length - 1] !== 'Win') parts.push('Win');
    return parts.join('+');
  };
  Shell.keys = {
    on(combo, fn) {
      keymap.set(combo, fn);
      derived.delete(combo);
      const fb = fallbackOf(combo);
      if (fb && (!keymap.has(fb) || derived.has(fb))) { keymap.set(fb, fn); derived.add(fb); }
    },
    comboOf,
    fallbackOf,
  };
  let winDownAlone = false;
  window.addEventListener('keydown', (e) => {
    const combo = comboOf(e);
    if (e.key === 'Meta' || e.key === 'OS') { winDownAlone = true; return; }
    winDownAlone = false;
    const fn = keymap.get(combo);
    if (fn && fn(e) !== false) { e.preventDefault(); e.stopPropagation(); }
  }, true);
  /* F11: page full screen with Keyboard Lock (Chromium), so the Win key and Alt+Tab reach the
     shell instead of the host desktop. Holding Esc leaves full screen. */
  window.addEventListener('keydown', (e) => {
    if (e.key !== 'F11' || e.ctrlKey || e.altKey || e.metaKey || e.shiftKey) return;
    e.preventDefault();
    if (document.fullscreenElement) { document.exitFullscreen?.(); return; }
    document.documentElement.requestFullscreen?.().then(() => navigator.keyboard?.lock?.()).catch(() => {});
  }, true);
  document.addEventListener('fullscreenchange', () => { if (!document.fullscreenElement) navigator.keyboard?.unlock?.(); });
  window.addEventListener('keyup', (e) => {
    if ((e.key === 'Meta' || e.key === 'OS') && winDownAlone) {
      winDownAlone = false;
      Shell.emit('win-key');
    }
  }, true);

  /* ---------- layers and light dismiss ---------- */
  /* Layers (bottom to top): #wallpaper, #desktop, #windows, #taskbar, #flyouts, #overlays,
     #menus, #tooltips. Shell.layer(name) returns the container. */
  Shell.layer = (name) => document.getElementById(name);

  /* One transient surface (Start, Search, Quick Settings, Notifications, a menu) is open at
     a time per group. Shell.transient.open(id, closeFn) closes the previous holder of the
     same group first; pointerdown outside every registered element light-dismisses it. */
  const transients = new Map(); // id -> {close, elements, group}
  Shell.transient = {
    open(id, { close, elements = [], group = 'flyout' }) {
      for (const [other, t] of transients) if (other !== id && t.group === group) Shell.transient.close(other);
      transients.set(id, { close, elements, group });
      Shell.emit('transient', { id, open: true });
    },
    close(id) {
      const t = transients.get(id); if (!t) return;
      transients.delete(id);
      t.close();
      Shell.emit('transient', { id, open: false });
    },
    closeAll(group) { for (const [id, t] of [...transients]) if (!group || t.group === group) Shell.transient.close(id); },
    isOpen: (id) => transients.has(id),
  };
  window.addEventListener('pointerdown', (e) => {
    for (const [id, t] of [...transients]) {
      const inside = t.elements.some((el) => el && (typeof el === 'function' ? el(e) : el.contains(e.target)));
      if (!inside) Shell.transient.close(id);
    }
  }, true);
  window.addEventListener('blur', () => Shell.transient.closeAll('menu'));

  /* ---------- drag and drop between surfaces ---------- */
  /* Pointer-driven drags (desktop icons, Explorer items, taskbar buttons) stay inside their
     own module; when an item is dropped outside its source, the source asks the registry.
     Shell.drag.registerTarget(el, {accepts(ids) -> bool, onEnter?(ids), onLeave?(), onDrop(ids, {x, y, copy})})
     returns an unregister function. Topmost element under the point wins. */
  const dropTargets = new Map(); // el -> handlers
  let hoverTarget = null;
  Shell.drag = {
    registerTarget(el, handlers) { dropTargets.set(el, handlers); return () => dropTargets.delete(el); },
    targetAt(x, y, ids, exclude) {
      const stack = document.elementsFromPoint(x, y);
      for (const node of stack) {
        if (exclude && exclude.contains(node)) continue;
        for (const [el, h] of dropTargets) {
          if (el.contains(node) && (!h.accepts || h.accepts(ids))) return { el, h };
        }
      }
      return null;
    },
    hover(x, y, ids, exclude) { /* call on every move while dragging; returns the target or null */
      const t = Shell.drag.targetAt(x, y, ids, exclude);
      if ((t && t.el) !== (hoverTarget && hoverTarget.el)) {
        if (hoverTarget && hoverTarget.h.onLeave) hoverTarget.h.onLeave();
        hoverTarget = t;
        if (t && t.h.onEnter) t.h.onEnter(ids);
      }
      return t;
    },
    drop(x, y, ids, opts = {}) { /* returns true when a target took the items */
      const t = Shell.drag.targetAt(x, y, ids, opts.exclude);
      if (hoverTarget && hoverTarget.h.onLeave) hoverTarget.h.onLeave();
      hoverTarget = null;
      if (!t) return false;
      t.h.onDrop(ids, { x, y, copy: !!opts.copy });
      return true;
    },
  };

  /* ---------- clock ---------- */
  Shell.now = () => new Date();
  setInterval(() => Shell.emit('tick', Shell.now()), 1000);

  /* ---------- boot ---------- */
  /* ?open=start,explorer,... asks modules to open a surface right after boot (screenshots
     and review captures). Each module checks Shell.debugOpen.includes('<its name>'). */
  const params = new URLSearchParams(location.search);
  Shell.debugOpen = (params.get('open') || '').split(',').filter(Boolean);
  Shell.boot = () => {
    if (params.has('dark')) Shell.settings.theme = 'dark';
    if (params.has('light')) Shell.settings.theme = 'light';
    makeNoise();
    applySetting('*');
    motionQuery.addEventListener('change', () => Shell.emit('setting', { key: 'animations' }));
    Shell.emit('boot');
    document.documentElement.classList.add('booted');
  };
  Shell.resetAll = () => { Shell.store.reset(); location.reload(); };
})();
