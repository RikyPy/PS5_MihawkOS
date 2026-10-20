/* Task Manager, as Windows 11 (22H2/23H2) draws it, over the shell's windows and a simulated
   PS5 running Wine.
   Owns: the app `taskmgr` (Ctrl+Shift+Escape, single instance) and everything in its window: the
   title bar search ("Type a name, publisher, or PID to search"), the NavigationView (hamburger;
   Processes, Performance, App history, Startup apps, Users, Details, Services; Settings at the
   bottom; compact below 1008 px with an acrylic overlay pane), each page's command bar, the
   process tree table with Windows' heat shading (header totals, sort, column chooser, sizing by
   the header dividers, keyboard),
   the Performance graphs, an in-window ContentDialog, and the simulated machine: values step
   every second (Windows' Normal update speed) and stay plausible for 8 Zen 2 cores / 16 threads
   and 16 GB GDDR6 with 3.5 GB reserved. Running apps come from Shell.wm.all(); End task force-closes
   their windows (a hung one included); a hung program spins one logical processor. Store key
   `taskmgr` holds its own options (start page, update speed, columns, column widths, startup apps).
   Debug captures: ?open=taskmgr, taskmgr-perf, hang (Notepad, marked not responding). */
(() => {
  'use strict';

  const Shell = window.Shell;

  /* ---------- helpers ---------- */
  function h(tag, cls, props, kids) {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    for (const [k, v] of Object.entries(props || {})) {
      if (v == null || v === false) continue;
      if (k === 'text') el.textContent = v;
      else el.setAttribute(k, v === true ? '' : String(v));
    }
    if (kids) el.append(...kids.filter(Boolean));
    return el;
  }
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
  const rnd = (a, b) => a + Math.random() * (b - a);
  const last = (a) => a[a.length - 1] ?? 0;
  const val = (x) => String(typeof x === 'function' ? x() : x ?? '');
  const hash = (s) => { let x = 2166136261; for (const c of String(s)) x = Math.imul(x ^ c.charCodeAt(0), 16777619); return x >>> 0; };
  const thousands = (n) => String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  const F = {
    pct: (v) => (v < 0.05 ? '0%' : `${v.toFixed(1)}%`),
    pct0: (v) => `${Math.round(v)}%`,
    mb: (v) => { const [i, f] = v.toFixed(1).split('.'); return `${thousands(i)}.${f} MB`; },
    mbs: (v) => (v < 0.05 ? '0 MB/s' : `${v.toFixed(1)} MB/s`),
    mbps: (v) => (v < 0.05 ? '0 Mbps' : `${v.toFixed(1)} Mbps`),
    kbps: (v) => (v < 0.05 ? '0 Kbps' : v < 1000 ? `${v.toFixed(1)} Kbps` : `${(v / 1000).toFixed(1)} Mbps`),
    kbs: (v) => (v < 0.05 ? '0 KB/s' : v < 1024 ? `${v.toFixed(1)} KB/s` : `${(v / 1024).toFixed(1)} MB/s`),
    gb: (v) => `${v.toFixed(1)} GB`,
    k: (mb) => `${thousands(Math.round(mb * 1024))} K`,
    clock(s) { const p = (n) => String(Math.floor(n)).padStart(2, '0'); return `${Math.floor(s / 86400)}:${p((s % 86400) / 3600)}:${p((s % 3600) / 60)}:${p(s % 60)}`; },
  };
  /* Fluent glyphs come as SVG; this module's own glyphs (object/glyph-*) are masks painted in currentColor. */
  function ic(name, size = 16) {
    const el = Shell.icon(name, { size });
    if (name.startsWith('object/glyph-')) {
      el.classList.add('tm-glyph');
      el.style.webkitMaskImage = el.style.maskImage = `url("${Shell.iconUrl(name)}")`;
    }
    return el;
  }
  /* Heat shading: Windows tints a cell from pale yellow toward orange as its share of the resource grows. */
  const HEAT = {
    cpu: (v) => Math.sqrt(clamp(v / 100, 0, 1)),
    memory: (mb) => Math.sqrt(clamp(mb / 2048, 0, 1)),
    disk: (v) => Math.sqrt(clamp(v / 50, 0, 1)),
    network: (v) => Math.sqrt(clamp(v / 10, 0, 1)),
    gpu: (v) => Math.sqrt(clamp(v / 100, 0, 1)),
  };

  /* ---------- options ---------- */
  const DEFAULT_COLS = ['name', 'status', 'cpu', 'memory', 'disk', 'network'];
  const prefs = { start: 'processes', speed: 'normal', minimizeOnUse: false, askEfficiency: true, grouped: true, cols: DEFAULT_COLS, startup: {}, widths: {}, ...Shell.store.get('taskmgr', {}) };
  const save = () => Shell.store.set('taskmgr', prefs);
  const SPEED = { high: 500, normal: 1000, low: 4000, paused: 0 };

  /* ---------- the simulated machine ---------- */
  const LOGICAL = 16, HIST = 61;
  const MEM_TOTAL = 16, MEM_USABLE = 12.5; // GDDR6; the console keeps 3.5 GB for itself
  const CPU_NAME = 'AMD Custom Zen 2 8-Core Processor';
  const GPU_NAME = 'AMD Radeon Graphics (RADV)';
  const BOOTED = Date.now() - 8024e3; // up 0:02:13:44 when the page loaded
  const H = { cpu: [], mem: [], disk: [], read: [], write: [], send: [], recv: [], gpu: [], g3d: [], gcopy: [], genc: [], gdec: [], ded: [], sha: [], cores: Array.from({ length: LOGICAL }, () => []) };
  const M = { ticks: 0, cpu: 0, ghz: 3.2, mem: 3.4, cache: 2.1, disk: 0, resp: 0.3, read: 0, write: 0, send: 0, recv: 0, gpu: 0, ded: 0.2, sha: 0.6, threads: 0, handles: 0, procs: 0 };
  const push = (arr, v) => { arr.push(v); if (arr.length > HIST) arr.shift(); };
  const memPct = () => (M.mem / MEM_USABLE) * 100;
  const netPct = () => ((M.send + M.recv) / 1e6) * 100; // of the 1 Gbps link

  const usedPids = new Set();
  function newPid() { let p; do p = 4 * Math.floor(rnd(60, 3200)); while (usedPids.has(p)); usedPids.add(p); return p; }
  const proc = (o) => ({ cpu: 0, disk: 0, net: 0, gpu: 0, eff: false, priority: 'Normal', user: 'Mihawk', pub: 'Wine', arch: 'x64', icon: 'object/app-generic', thr: 8, cpuBase: 0, gpuBase: 0, mem: o.memBase, wins: [], ...o, pid: newPid() });
  /* The Wine prefix's own processes: what Task Manager lists under a PS5 Wine runtime. */
  const SYS = [
    { key: 'services', name: 'Services and Controller app', exe: 'services.exe', group: 'win', cpuBase: 0.05, memBase: 4.6, thr: 9, user: 'SYSTEM' },
    { key: 'explorer', name: 'Windows Explorer', exe: 'explorer.exe', group: 'win', cpuBase: 0.25, memBase: 41.8, thr: 26, icon: 'object/app-explorer', pub: 'Mihawk OS' },
    { key: 'plugplay', name: 'Plug and Play Service', exe: 'plugplay.exe', group: 'win', cpuBase: 0.04, memBase: 3.1, thr: 6, user: 'SYSTEM' },
    { key: 'rpcss', name: 'Remote Procedure Call', exe: 'rpcss.exe', group: 'win', memBase: 2.4, thr: 5, user: 'SYSTEM' },
    { key: 'winedevice1', name: 'Wine Device Manager', exe: 'winedevice.exe', group: 'win', cpuBase: 0.3, memBase: 9.6, thr: 12, user: 'SYSTEM' },
    { key: 'winedevice2', name: 'Wine Device Manager', exe: 'winedevice.exe', group: 'win', cpuBase: 0.15, memBase: 6.9, thr: 10, user: 'SYSTEM' },
    { key: 'eventlog', name: 'Service Host: Windows Event Log', exe: 'svchost.exe', group: 'win', memBase: 3.3, thr: 5, user: 'SYSTEM' },
    { key: 'wmi', name: 'Service Host: Windows Management Instrumentation', exe: 'svchost.exe', group: 'win', cpuBase: 0.03, memBase: 5.8, thr: 8, user: 'SYSTEM' },
    { key: 'launcher', name: 'Store Launcher Service (32 bit)', exe: 'launcherservice.exe', group: 'bg', cpuBase: 0.08, memBase: 6.8, thr: 14, user: 'SYSTEM', pub: 'Example Publisher', arch: 'x86', netBase: 0.03, diskOdds: 0.06 },
    { key: 'conhost', name: 'Console Window Host', exe: 'conhost.exe', group: 'bg', memBase: 1.9, thr: 3 },
    { key: 'menubuilder', name: 'Wine Menu Builder', exe: 'winemenubuilder.exe', group: 'bg', memBase: 2.2, thr: 2, eff: true },
  ].map(proc);
  const sysProc = (k) => SYS.find((p) => p.key === k);
  /* Programs with windows: name and image as Windows reports them. */
  const APP = {
    settings: { exe: 'SystemSettings.exe', cpuBase: 0.1, memBase: 74.2, thr: 32 },
    notepad: { exe: 'Notepad.exe', cpuBase: 0.05, memBase: 36.5, thr: 18 },
    calculator: { exe: 'CalculatorApp.exe', memBase: 21.7, thr: 14 },
    terminal: { exe: 'WindowsTerminal.exe', cpuBase: 0.2, memBase: 63.9, thr: 28 },
    taskmgr: { exe: 'Taskmgr.exe', cpuBase: 1.1, memBase: 46.3, thr: 22, gpuBase: 0.2 },
    game: { exe: 'game.exe', cpuBase: 3, memBase: 412, thr: 40, gpuBase: 14, pub: '' },
    'wm-hang': { exe: 'WerFault.exe', name: 'Windows Problem Reporting', memBase: 8.1, thr: 6 },
  };
  const appProcs = new Map(); // appId -> process while it has windows
  const ended = new Set();     // background and Windows processes ended this session
  const svcState = new Map();  // services without a process of their own: name -> pid or 0

  function running() {
    const wins = Shell.wm?.all?.() || [];
    const by = new Map();
    for (const w of wins) { if (!by.has(w.appId)) by.set(w.appId, []); by.get(w.appId).push(w); }
    for (const [id, p] of appProcs) if (!by.has(id)) { usedPids.delete(p.pid); appProcs.delete(id); }
    const explorer = sysProc('explorer');
    explorer.wins = [];
    explorer.hung = false;
    const apps = [...by].map(([id, ws]) => {
      let p = id === 'explorer' ? explorer : appProcs.get(id);
      if (!p) {
        const meta = APP[id] || {}, reg = Shell.apps?.get?.(id);
        p = proc({ key: `app:${id}`, app: id, group: 'app', name: meta.name || reg?.name || ws[0].title || id, exe: meta.exe || `${id}.exe`,
          icon: reg?.icon || ws[0].icon, cpuBase: meta.cpuBase ?? 0.05, memBase: meta.memBase ?? 24, thr: meta.thr ?? 12, gpuBase: meta.gpuBase ?? 0.05, pub: meta.pub ?? 'Mihawk OS' });
        appProcs.set(id, p);
      }
      p.wins = ws;
      p.hung = ws.some((w) => w.isNotResponding?.());
      return p;
    });
    const inApps = by.has('explorer');
    const bg = SYS.filter((p) => p.group === 'bg' && !ended.has(p.key));
    const sys = SYS.filter((p) => p.group === 'win' && !ended.has(p.key) && !(inApps && p.key === 'explorer'));
    return { apps, bg, sys, all: [...apps, ...bg, ...sys], windows: wins.length };
  }

  /* One sample: every process moves around its own base, the totals follow. */
  function step() {
    const { all, apps, windows } = running();
    M.ticks++;
    let cpu = 0, mem = 0, disk = 0, net = 0, gpu = 0, thr = 0;
    for (const p of all) {
      const want = (p.cpuBase * rnd(0.1, 1.9) + (Math.random() < 0.03 ? rnd(0.3, 2.5) : 0)) * (p.eff ? 0.4 : 1) + (p.hung ? 100 / LOGICAL : 0);
      p.cpu = p.cpu * 0.45 + want * 0.55;
      p.mem = p.memBase * (1 + 0.015 * Math.sin((M.ticks + p.pid) / 23)) + p.wins.length * 2.4;
      p.disk = Math.random() < (p.diskOdds ?? 0.02) ? rnd(0.05, 0.6) : 0;
      p.net = p.netBase && Math.random() < 0.5 ? rnd(0, p.netBase) : 0;
      p.gpu = p.gpuBase * rnd(0.5, 1.5);
      cpu += p.cpu; mem += p.mem; disk += p.disk; net += p.net; gpu += p.gpu; thr += p.thr;
    }
    if (all.length && Math.random() < 0.06) { const p = all[Math.floor(Math.random() * all.length)], b = rnd(1, 12); p.disk += b; disk += b; }
    M.cpu = clamp(cpu + rnd(0.2, 0.9), 0, 100);
    M.ghz = 3.5 * clamp((M.cpu > 15 ? 0.97 : 0.9) + rnd(-0.03, 0.03), 0.8, 1);
    const hot = apps.some((p) => p.hung) ? 5 : -1; // the hung program's thread pins one logical processor
    const rest = hot < 0 ? M.cpu : Math.max(0, (M.cpu * LOGICAL - 100) / (LOGICAL - 1));
    H.cores.forEach((c, i) => push(c, i === hot ? rnd(97, 100) : clamp(rest * rnd(0.2, 1.8), 0, 100)));
    M.mem = 2.65 + mem / 1024;
    M.cache = 2.1 + Math.sin(M.ticks / 40) * 0.05;
    M.read = disk * 1024 * rnd(0.5, 0.9);
    M.write = Math.max(0, disk * 1024 - M.read) + rnd(0, 24);
    M.disk = clamp(((M.read + M.write) / 1024) * 3.2 + rnd(0, 0.5), 0, 100);
    M.resp = 0.2 + rnd(0, 0.3) + (disk > 1 ? rnd(0.3, 1.2) : 0);
    M.recv = net * 700 + rnd(0, 12) + (Math.random() < 0.04 ? rnd(80, 600) : 0);
    M.send = net * 300 + rnd(0, 6);
    const g3d = clamp(0.8 + windows * 0.35 + gpu + rnd(-0.4, 0.6), 0, 100), gcopy = rnd(0, 0.4);
    M.gpu = Math.max(g3d, gcopy);
    M.ded = 0.19 + windows * 0.012;
    M.sha = 0.54 + windows * 0.03;
    M.threads = thr + 310;
    M.handles = thr * 31 + 4200;
    M.procs = all.length;
    push(H.cpu, M.cpu); push(H.mem, memPct()); push(H.disk, M.disk); push(H.read, M.read); push(H.write, M.write);
    push(H.recv, M.recv); push(H.send, M.send); push(H.gpu, M.gpu); push(H.g3d, g3d); push(H.gcopy, gcopy);
    push(H.genc, 0); push(H.gdec, 0); push(H.ded, M.ded); push(H.sha, M.sha);
  }

  /* ---------- graphs: 60 seconds of samples, newest at the right, grid scrolling with them ---------- */
  function graph({ grid = true } = {}) {
    const el = h('div', 'tm-graph');
    el.innerHTML = '<svg viewBox="0 0 60 100" preserveAspectRatio="none" aria-hidden="true"><path class="tm-g-grid"/><path class="tm-g-fill"/><path class="tm-g-line"/><path class="tm-g-line2"/></svg>';
    const [gp, fp, lp, l2] = el.querySelectorAll('path');
    const pts = (vals, max) => vals.map((v, i) => `${60 - (vals.length - 1 - i)} ${(100 - clamp(v / max, 0, 1) * 100).toFixed(1)}`);
    return {
      el,
      draw(a, max = 100, b = null) {
        if (grid) {
          let d = '';
          for (let y = 10; y < 100; y += 10) d += `M0 ${y}H60`;
          for (let x = 60 - (M.ticks % 3); x > 0; x -= 3) d += `M${x} 0V100`;
          gp.setAttribute('d', d);
        }
        const p = pts(a, max);
        lp.setAttribute('d', p.length > 1 ? `M${p.join('L')}` : '');
        fp.setAttribute('d', p.length > 1 ? `M${p[0].split(' ')[0]} 100L${p.join('L')}L60 100Z` : '');
        l2.setAttribute('d', b && b.length > 1 ? `M${pts(b, max).join('L')}` : '');
      },
    };
  }
  const KBS = [[100, '100 KB/s'], [500, '500 KB/s'], [1024, '1 MB/s'], [5120, '5 MB/s'], [10240, '10 MB/s'], [51200, '50 MB/s'], [102400, '100 MB/s'], [512000, '500 MB/s']];
  const KBPS = [[10, '10 Kbps'], [50, '50 Kbps'], [100, '100 Kbps'], [500, '500 Kbps'], [1000, '1 Mbps'], [5000, '5 Mbps'], [10000, '10 Mbps'], [100000, '100 Mbps'], [1000000, '1 Gbps']];
  /* Throughput graphs pick the smallest of Windows' round scales that holds the last minute. */
  function scaleOf(steps, ...arrs) {
    const m = Math.max(0, ...arrs.flat());
    const s = steps.find(([v]) => v >= m) || steps[steps.length - 1];
    return { max: s[0], label: s[1] };
  }
  const netScale = () => scaleOf(KBPS, H.recv, H.send);
  const diskScale = () => scaleOf(KBS, H.read, H.write);

  /* ---------- window state (single instance) ---------- */
  let tm = null;
  const refresh = () => { if (tm?.page) tm.page.update?.(); };

  /* ---------- small UI pieces ---------- */
  function cmd(icon, label, onClick, { toggle = false, menu = false } = {}) {
    const b = h('button', 'btn subtle tm-cmd', { type: 'button', 'aria-label': label, 'data-tooltip': label });
    if (toggle) b.setAttribute('aria-pressed', 'false');
    b.append(ic(icon, 16), h('span', 'tm-cmd-label', { text: label }));
    if (menu) b.append(ic('fluent/chevron-down', 12));
    b.addEventListener('click', () => onClick(b));
    return b;
  }
  const runCmd = () => {
    const b = cmd('fluent/window-new', 'Run new task', () => Shell.dialogs?.run?.());
    b.disabled = !Shell.dialogs?.run;
    return b;
  };
  function dropMenu(btn, items) {
    const r = btn.getBoundingClientRect();
    Shell.menu?.show?.({ x: r.left, y: r.bottom, anchorRect: r, placement: 'bottom-start', items, onClose: () => btn.classList.remove('open') });
    if (Shell.menu?.isOpen?.()) btn.classList.add('open');
  }
  /* WinUI ContentDialog inside the window: smoke over the content, the rest inert until answered. */
  function contentDialog({ title, text, primary, close = 'Cancel' }) {
    const t = tm;
    if (!t) return Promise.resolve(false);
    return new Promise((resolve) => {
      const back = document.activeElement;
      const ok = h('button', 'btn accent', { type: 'button', text: primary });
      const no = h('button', 'btn', { type: 'button', text: close });
      const dlg = h('div', 'tm-dialog', { role: 'dialog', 'aria-modal': 'true', 'aria-label': title }, [
        h('div', 'tm-dialog-body', null, [h('h2', 'tm-dialog-title', { text: title }), h('p', 'tm-dialog-text', { text })]),
        h('div', 'tm-dialog-foot', null, [ok, no]),
      ]);
      const smoke = h('div', 'tm-smoke', null, [dlg]);
      let settled = false;
      const done = (answer) => {
        if (settled) return;
        settled = true;
        t.shell.inert = t.tb.inert = false;
        smoke.style.pointerEvents = 'none';
        Shell.animate(dlg, [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'scale(1.05)' }], { duration: 167, easing: Shell.ease.out, fill: 'forwards' });
        Shell.animate(smoke, [{ opacity: 1 }, { opacity: 0 }], { duration: 167, easing: 'linear', fill: 'forwards' }).then(() => smoke.remove());
        if (back?.isConnected) back.focus({ preventScroll: true });
        resolve(answer);
      };
      ok.addEventListener('click', () => done(true));
      no.addEventListener('click', () => done(false));
      dlg.addEventListener('keydown', (e) => { if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); done(false); } });
      t.shell.inert = t.tb.inert = true;
      t.root.append(smoke);
      Shell.animate(smoke, [{ opacity: 0 }, { opacity: 1 }], { duration: 83, easing: 'linear' });
      Shell.animate(dlg, [{ opacity: 0, transform: 'scale(1.05)' }, { opacity: 1, transform: 'none' }], { duration: 250 });
      ok.focus();
    });
  }

  /* ---------- the tree table (Processes, Users, Details, Services, Startup apps, App history) ---------- */
  /* Nodes: {key, kind: 'group'|'row', name, icon?, children?, fixed? (keeps its order), cells: {col: text},
     load?: {col: 0..1} (heat), v?: {col: number} (sort), status?, hay (search text), proc?, win?}. */
  function treeTable({ cols, label, tree = true, onSelect, onActivate, onDelete, onMenu, onHeadMenu }) {
    const scroll = h('div', 'tm-scroll');
    const table = h('div', 'tm-table', { role: 'treegrid', tabindex: '0', 'aria-label': label });
    const head = h('div', 'tm-thead', { role: 'row' });
    const body = h('div', 'tm-tbody', { role: 'rowgroup' });
    table.append(head, body);
    scroll.append(table);
    const S = { cols: [], sort: { col: 'name', dir: 1, shown: false }, expanded: new Set(), sel: null, els: new Map(), vis: [], nodes: [], q: '' };
    let heads = {};
    const W = (prefs.widths[label] ||= {}); // column id -> px, once the user has sized this table's columns
    const tracks = () => S.cols.map((c) => (W[c.id] ? `${W[c.id]}px` : c.w)).join(' ');

    const value = (c, n) => (c.id === 'name' ? n.name : c.id === 'status' ? n.status || '' : n.v?.[c.id] ?? n.cells?.[c.id] ?? '');
    function cmp(a, b) {
      const c = S.cols.find((x) => x.id === S.sort.col) || S.cols[0];
      const va = value(c, a), vb = value(c, b);
      const r = typeof va === 'number' && typeof vb === 'number' ? va - vb : String(va).localeCompare(String(vb), undefined, { numeric: true, sensitivity: 'base' });
      return r * S.sort.dir || a.name.localeCompare(b.name);
    }
    function setCols(list) {
      S.cols = list;
      table.style.setProperty('--cols', tracks());
      table.classList.toggle('totals', list.some((c) => c.total));
      heads = {};
      head.replaceChildren(...list.map((c) => {
        const th = h('div', `tm-th${c.right ? ' right' : ''}`, { role: 'columnheader', 'data-col': c.id });
        const sortEl = h('span', 'tm-sort');
        const total = c.total ? h('span', 'tm-th-total') : null;
        const grip = h('div', 'tm-th-grip', { 'aria-hidden': 'true' });
        th.append(...[sortEl, total, h('span', 'tm-th-label', { text: c.label }), grip].filter(Boolean));
        th.addEventListener('click', () => sortBy(c));
        grip.addEventListener('pointerdown', (e) => sizeCol(e, c));
        grip.addEventListener('click', (e) => e.stopPropagation()); // the drag's click does not sort
        heads[c.id] = { th, sortEl, total };
        return th;
      }));
      S.els.forEach((r) => r.el.remove());
      S.els.clear();
      paintSort();
    }
    /* The header divider sizes the column on its left. The first drag fixes every column at the width
       it shows (Name stops filling the window), as the list view does; widths persist per table. */
    function sizeCol(e, c) {
      if (e.button !== 0) return;
      e.preventDefault();
      const grip = e.currentTarget, sx = e.clientX;
      for (const x of S.cols) W[x.id] ||= Math.round(heads[x.id].th.getBoundingClientRect().width);
      const start = W[c.id];
      try { grip.setPointerCapture(e.pointerId); } catch { /* pointer already gone */ }
      const move = (ev) => { W[c.id] = Math.max(40, Math.round(start + ev.clientX - sx)); table.style.setProperty('--cols', tracks()); };
      const up = () => {
        grip.removeEventListener('pointermove', move);
        grip.removeEventListener('pointerup', up);
        grip.removeEventListener('pointercancel', up);
        save();
      };
      grip.addEventListener('pointermove', move);
      grip.addEventListener('pointerup', up);
      grip.addEventListener('pointercancel', up);
    }
    function sortBy(c) {
      const again = S.sort.col === c.id && S.sort.shown;
      S.sort = { col: c.id, dir: again ? -S.sort.dir : c.right ? -1 : 1, shown: true };
      paintSort();
      render();
    }
    function paintSort() {
      for (const [id, { th, sortEl }] of Object.entries(heads)) {
        const on = S.sort.shown && id === S.sort.col;
        sortEl.replaceChildren(...(on ? [Shell.icon(S.sort.dir > 0 ? 'fluent/chevron-up' : 'fluent/chevron-down', { size: 12 })] : []));
        th.setAttribute('aria-sort', on ? (S.sort.dir > 0 ? 'ascending' : 'descending') : 'none');
      }
    }
    const isOpen = (n) => S.expanded.has(n.key) || (!!S.q && !!n.children?.length && !n.hay?.includes(S.q));
    function render(nodes = S.nodes) {
      S.nodes = nodes;
      for (const c of S.cols) {
        const hc = heads[c.id];
        if (!hc?.total) continue;
        hc.total.textContent = c.total();
        const load = clamp(c.headLoad?.() ?? 0, 0, 1);
        hc.th.classList.toggle('hot', load >= 0.5);
        hc.th.style.setProperty('--load', load.toFixed(3));
      }
      const q = S.q;
      const match = (n) => !q || (n.kind === 'group' ? (n.children || []).some(match) : n.hay?.includes(q) || (n.children || []).some(match));
      const vis = [];
      const walk = (list, depth, parent) => {
        const sorted = list[0]?.kind === 'group' || list[0]?.fixed ? list : [...list].sort(cmp);
        for (const n of sorted) {
          if (!match(n)) continue;
          n.parent = parent;
          const open = n.kind !== 'group' && isOpen(n);
          vis.push({ n, depth, open });
          if (n.children?.length && (n.kind === 'group' || open)) walk(n.children, n.kind === 'group' ? depth : depth + 1, n.kind === 'group' ? null : n);
        }
      };
      walk(nodes, 0, null);
      S.vis = vis;
      const keep = new Set();
      let prev = null;
      for (const v of vis) {
        const r = row(v);
        keep.add(v.n.key);
        const want = prev ? prev.nextSibling : body.firstChild;
        if (r.el !== want) body.insertBefore(r.el, want);
        prev = r.el;
      }
      for (const [k, r] of S.els) if (!keep.has(k)) { r.el.remove(); S.els.delete(k); }
      if (S.sel && !keep.has(S.sel)) { S.sel = null; onSelect?.(null); }
    }
    function row({ n, depth, open }) {
      const kids = n.kind !== 'group' && !!n.children?.length;
      let r = S.els.get(n.key);
      if (r && r.kids !== kids) { r.el.remove(); S.els.delete(n.key); r = null; }
      if (!r) {
        const el = h('div', `tm-tr ${n.kind}`, { role: 'row', 'data-key': n.key });
        const cells = S.cols.map((c, i) => h('div', `tm-td tm-col-${c.id}${c.right ? ' right' : ''}`, { role: i ? 'gridcell' : 'rowheader' }));
        el.append(...cells);
        r = { el, cells, kids, label: h('span', 'tm-name') };
        if (n.kind !== 'group') {
          if (kids) {
            r.chev = h('button', 'tm-chev', { type: 'button', tabindex: '-1', 'aria-label': 'Expand' }, [Shell.icon('fluent/chevron-right', { size: 12 })]);
            r.chev.addEventListener('click', (e) => { e.stopPropagation(); toggle(r.n.key); });
            cells[0].append(r.chev);
          } else if (tree) cells[0].append(h('span', 'tm-chev-space'));
          if (n.icon) cells[0].append(ic(n.icon, 16));
        }
        cells[0].append(r.label);
        S.els.set(n.key, r);
      }
      r.n = n;
      r.el.style.setProperty('--depth', depth);
      if (r.label.textContent !== n.name) r.label.textContent = n.name;
      if (r.chev) {
        r.chev.classList.toggle('open', open);
        r.chev.setAttribute('aria-label', open ? 'Collapse' : 'Expand');
        r.el.setAttribute('aria-expanded', String(open));
      }
      r.el.classList.toggle('selected', S.sel === n.key);
      r.el.setAttribute('aria-selected', String(S.sel === n.key));
      S.cols.forEach((c, i) => {
        if (!i) return;
        const td = r.cells[i];
        if (c.id === 'status' && n.status === 'eff') {
          if (!r.leaf) { r.leaf = ic('object/glyph-leaf', 16); r.leaf.dataset.tooltip = 'Efficiency mode'; }
          if (r.leaf.parentNode !== td) td.replaceChildren(r.leaf);
        } else {
          const t = String((c.id === 'status' ? n.status : n.cells?.[c.id]) ?? '');
          if (td.firstElementChild || td.textContent !== t) td.textContent = t;
        }
        const load = n.load?.[c.id];
        td.classList.toggle('heat', load != null);
        if (load != null) td.style.setProperty('--load', load.toFixed(3));
      });
      return r;
    }
    function select(key, { reveal = true } = {}) {
      S.sel = key;
      for (const [k, r] of S.els) {
        r.el.classList.toggle('selected', k === key);
        r.el.setAttribute('aria-selected', String(k === key));
      }
      const r = S.els.get(key);
      if (r && reveal) r.el.scrollIntoView({ block: 'nearest' });
      onSelect?.(r?.n || null);
    }
    function toggle(key, on = !S.expanded.has(key)) {
      if (on) S.expanded.add(key); else S.expanded.delete(key);
      render();
    }
    const rowOf = (e) => S.els.get(e.target.closest?.('.tm-tr')?.dataset.key);
    body.addEventListener('pointerdown', (e) => {
      const r = rowOf(e);
      if (!r || r.n.kind === 'group' || e.button > 2) return;
      select(r.n.key, { reveal: false });
      if (e.button === 0) table.focus({ preventScroll: true });
    });
    body.addEventListener('dblclick', (e) => {
      const r = rowOf(e);
      if (!r || r.n.kind === 'group' || e.target.closest('.tm-chev')) return;
      if (r.kids) toggle(r.n.key); else onActivate?.(r.n);
    });
    body.addEventListener('contextmenu', (e) => {
      const r = rowOf(e);
      e.preventDefault();
      if (!r || r.n.kind === 'group') return;
      select(r.n.key, { reveal: false });
      table.focus({ preventScroll: true });
      onMenu?.(r.n, e.clientX, e.clientY);
    });
    head.addEventListener('contextmenu', (e) => { e.preventDefault(); onHeadMenu?.(e.clientX, e.clientY); });
    table.addEventListener('keydown', (e) => {
      const rows = S.vis.filter((v) => v.n.kind !== 'group');
      let i = rows.findIndex((v) => v.n.key === S.sel);
      const cur = rows[i];
      const k = e.key, page = Math.max(1, Math.floor(scroll.clientHeight / 32) - 2);
      if (k === 'ArrowDown' || k === 'ArrowUp') i = clamp(i + (k === 'ArrowDown' ? 1 : -1), 0, rows.length - 1);
      else if (k === 'PageDown' || k === 'PageUp') i = clamp(i + (k === 'PageDown' ? page : -page), 0, rows.length - 1);
      else if (k === 'Home' || k === 'End') i = k === 'Home' ? 0 : rows.length - 1;
      else if (k === 'ArrowRight' && cur) {
        if (!S.els.get(cur.n.key)?.kids) return;
        if (!cur.open) { toggle(cur.n.key, true); i = -2; } else i += 1;
      } else if (k === 'ArrowLeft' && cur) {
        if (cur.open && S.expanded.has(cur.n.key)) { toggle(cur.n.key, false); i = -2; } else if (cur.n.parent) i = rows.findIndex((v) => v.n === cur.n.parent); else return;
      } else if (k === 'Enter' && cur) { onActivate?.(cur.n); i = -2; }
      else if (k === 'Delete' && cur) { onDelete?.(cur.n); i = -2; }
      else if ((k === 'ContextMenu' || (e.shiftKey && k === 'F10')) && cur) {
        const b = S.els.get(cur.n.key).el.getBoundingClientRect();
        onMenu?.(cur.n, b.left + 40, b.bottom);
        i = -2;
      } else return;
      e.preventDefault();
      e.stopPropagation();
      if (i >= 0 && rows[i] && rows[i].n.key !== S.sel) select(rows[i].n.key);
    });
    setCols(cols);
    const walkAll = (list, fn) => list.forEach((n) => { fn(n); if (n.children) walkAll(n.children, fn); });
    return {
      el: scroll, table, S, render, select, toggle, setCols, isOpen,
      selected: () => S.els.get(S.sel)?.n || null,
      setQuery(q) { S.q = q; },
      expandAll() { walkAll(S.nodes, (n) => { if (n.kind !== 'group' && n.children?.length) S.expanded.add(n.key); }); render(); },
      collapseAll() { S.expanded.clear(); render(); },
      selectWhere(fn) { const v = S.vis.find((x) => fn(x.n)); if (v) { select(v.n.key); table.focus({ preventScroll: true }); } },
    };
  }

  /* ---------- Processes ---------- */
  const TYPE = { app: 'App', bg: 'Background process', win: 'Windows process' };
  const COLS = {
    name: { id: 'name', label: 'Name', w: 'minmax(220px, 1fr)' },
    type: { id: 'type', label: 'Type', w: '150px' },
    status: { id: 'status', label: 'Status', w: '116px' },
    publisher: { id: 'publisher', label: 'Publisher', w: '170px' },
    pid: { id: 'pid', label: 'PID', w: '72px', right: true },
    process: { id: 'process', label: 'Process name', w: '170px' },
    cpu: { id: 'cpu', label: 'CPU', w: '76px', right: true, total: () => F.pct0(M.cpu), headLoad: () => M.cpu / 100 },
    memory: { id: 'memory', label: 'Memory', w: '100px', right: true, total: () => F.pct0(memPct()), headLoad: () => memPct() / 100 },
    disk: { id: 'disk', label: 'Disk', w: '84px', right: true, total: () => F.pct0(M.disk), headLoad: () => M.disk / 100 },
    network: { id: 'network', label: 'Network', w: '84px', right: true, total: () => F.pct0(netPct()), headLoad: () => netPct() / 100 },
    gpu: { id: 'gpu', label: 'GPU', w: '72px', right: true, total: () => F.pct0(M.gpu), headLoad: () => M.gpu / 100 },
  };
  const COL_ORDER = Object.keys(COLS);
  const procOf = (n) => n?.proc || n?.parent?.proc || null;
  function procNode(p, kids) {
    return {
      key: p.key, kind: 'row', proc: p, icon: p.icon, children: kids,
      name: kids?.length > 1 ? `${p.name} (${kids.length})` : p.name,
      status: p.hung ? 'Not responding' : p.eff ? 'eff' : '',
      cells: { type: TYPE[p.group === 'app' || p.wins.length ? 'app' : p.group], publisher: p.pub, pid: p.pid, process: p.exe,
        cpu: F.pct(p.cpu), memory: F.mb(p.mem), disk: F.mbs(p.disk), network: F.mbps(p.net), gpu: F.pct(p.gpu) },
      load: { cpu: HEAT.cpu(p.cpu), memory: HEAT.memory(p.mem), disk: HEAT.disk(p.disk), network: HEAT.network(p.net), gpu: HEAT.gpu(p.gpu) },
      v: { cpu: p.cpu, memory: p.mem, disk: p.disk, network: p.net, gpu: p.gpu, pid: p.pid },
      hay: `${p.name} ${p.pub} ${p.pid} ${p.exe}`.toLowerCase(),
    };
  }
  const winNode = (w) => ({ key: `w:${w.id}`, kind: 'row', fixed: true, win: w, name: w.title, icon: w.icon, hay: w.title.toLowerCase() });
  function procTree() {
    const { apps, bg, sys } = running();
    const appNodes = apps.map((p) => procNode(p, p.wins.map(winNode)));
    const bgNodes = bg.map((p) => procNode(p)), sysNodes = sys.map((p) => procNode(p));
    if (!prefs.grouped) return [...appNodes, ...bgNodes, ...sysNodes];
    return [['apps', 'Apps', appNodes], ['bg', 'Background processes', bgNodes], ['win', 'Windows processes', sysNodes]]
      .filter(([, , list]) => list.length)
      .map(([k, label, children]) => ({ key: `g:${k}`, kind: 'group', name: `${label} (${children.length})`, children }));
  }

  function switchTo(n) {
    const p = procOf(n);
    const w = n?.win || p?.wins?.[p.wins.length - 1];
    if (!w) return;
    w.focus();
    if (prefs.minimizeOnUse && tm && w !== tm.win) tm.win.minimize();
  }
  async function endTask(n) {
    const p = procOf(n);
    if (!p) return;
    if (p.key === 'explorer') { restartExplorer(); return; }
    if (p.app) {
      (Shell.wm?.all?.() || []).filter((w) => w.appId === p.app).forEach((w) => w.close({ force: true }));
      return;
    }
    if (p.group === 'win' && !(await contentDialog({
      title: `Do you want to end "${p.name}"?`,
      text: 'If an open program is associated with this process, it will close and you will lose any unsaved data. If you end a system process, it might result in system instability. Are you sure you want to continue?',
      primary: 'End process',
    }))) return;
    ended.add(p.key);
    refresh();
  }
  /* Windows offers Restart, not End task, for Windows Explorer: its windows close and it comes back. */
  function restartExplorer() {
    const p = sysProc('explorer');
    (Shell.wm?.all?.() || []).filter((w) => w.appId === 'explorer').forEach((w) => w.close({ force: true }));
    ended.add('explorer');
    refresh();
    setTimeout(() => { usedPids.delete(p.pid); p.pid = newPid(); ended.delete('explorer'); refresh(); }, 1500);
  }
  async function toggleEff(p) {
    if (!p || p.group === 'win') return;
    if (!p.eff && prefs.askEfficiency && !(await contentDialog({
      title: 'Turn on Efficiency mode?',
      text: 'Efficiency mode will lower process priority and improve power efficiency. It may cause instability for some processes.',
      primary: 'Turn on Efficiency mode',
    }))) return;
    p.eff = !p.eff;
    refresh();
  }
  const svcOf = (p) => SERVICES.find((s) => s.proc && s.proc === p?.key);

  function pageProcesses() {
    const t = treeTable({
      cols: prefs.cols.map((id) => COLS[id]).filter(Boolean), label: 'Processes',
      onSelect: () => paint(), onActivate: switchTo, onDelete: endTask, onHeadMenu: columnsMenu,
      onMenu(n, x, y) {
        const p = procOf(n);
        const items = n.win ? [
          { label: 'Switch to', action: () => switchTo(n) },
          { label: 'Bring to front', action: () => n.win.focus() },
          { label: 'Minimize', action: () => n.win.minimize() },
          { label: 'Maximize', action: () => n.win.maximize() },
          { separator: true },
          { label: 'End task', action: () => endTask(n) },
        ] : [
          ...(n.children?.length ? [{ label: t.isOpen(n) ? 'Collapse' : 'Expand', action: () => t.toggle(n.key, !t.isOpen(n)) }, { label: 'Switch to', action: () => switchTo(n) }] : []),
          { label: p.key === 'explorer' ? 'Restart' : 'End task', action: () => endTask(n) },
          { label: 'Efficiency mode', checked: p.eff, disabled: p.group === 'win', action: () => toggleEff(p) },
          { separator: true },
          { label: 'Go to details', action: () => go('details', { pid: p.pid }) },
          ...(svcOf(p) ? [{ label: 'Go to service(s)', action: () => go('services', { svc: svcOf(p).name }) }] : []),
        ];
        Shell.menu?.show?.({ x, y, items });
      },
    });
    const end = cmd('object/glyph-end-task', 'End task', () => { const n = t.selected(); if (n) endTask(n); });
    const eff = cmd('object/glyph-leaf', 'Efficiency mode', () => toggleEff(procOf(t.selected())), { toggle: true });
    const view = cmd('fluent/options', 'View', (b) => dropMenu(b, [
      { label: 'Group by type', checked: prefs.grouped, action: () => { prefs.grouped = !prefs.grouped; save(); refresh(); } },
      { separator: true },
      { label: 'Expand all', action: () => t.expandAll() },
      { label: 'Collapse all', action: () => t.collapseAll() },
    ]), { menu: true });
    function paint() {
      const n = t.selected(), p = procOf(n);
      end.disabled = !p;
      const label = p?.key === 'explorer' ? 'Restart' : 'End task';
      end.querySelector('.tm-cmd-label').textContent = label;
      end.setAttribute('aria-label', label);
      end.dataset.tooltip = label;
      eff.disabled = !p || !!n?.win || p.group === 'win';
      eff.setAttribute('aria-pressed', String(!eff.disabled && !!p.eff));
    }
    function columnsMenu(x, y) {
      Shell.menu?.show?.({
        x, y, items: COL_ORDER.filter((id) => id !== 'name').map((id) => ({
          label: COLS[id].label, checked: prefs.cols.includes(id),
          action: () => {
            const on = new Set(prefs.cols);
            if (on.has(id)) on.delete(id); else on.add(id);
            prefs.cols = COL_ORDER.filter((c) => on.has(c));
            save();
            t.setCols(prefs.cols.map((c) => COLS[c]));
            refresh();
          },
        })),
      });
    }
    return {
      title: 'Processes', search: true, cmds: [runCmd(), end, eff, view], el: t.el, focus: t.table,
      update() { t.setQuery(tm.query); t.render(procTree()); paint(); },
    };
  }

  /* ---------- Performance ---------- */
  const STATS_UP = () => F.clock(Math.floor((Date.now() - BOOTED) / 1000));
  /* One resource's view: title and model, graph groups ({grow, cols, top, cells: [{top, bottom, draw|bar}]}),
     the large figures (rows of [label, value]) and the key-value list at the right. */
  function perfDetail({ title, model, color, groups, big, kv, menu }) {
    const el = h('div', `tm-pd tm-c-${color}`);
    const live = [];
    const axis = (pair, cls = 'tm-pd-axis') => {
      const l = h('span'), r = h('span');
      live.push(() => { l.textContent = val(pair[0]); r.textContent = val(pair[1]); });
      return h('div', cls, null, [l, r]);
    };
    const area = h('div', 'tm-pd-graphs');
    for (const gr of groups) {
      const wrap = h('div', `tm-pd-group${gr.grow ? ' grow' : ''}`);
      wrap.style.setProperty('--cols', gr.cols || 1);
      for (const c of gr.cells) {
        const cell = h('div', 'tm-pd-cell');
        if (c.top) cell.append(axis(c.top));
        if (c.bar) {
          const segs = ['use', 'mod', 'stb'].map((k) => h('span', `tm-comp-${k}`));
          cell.append(h('div', 'tm-comp', { 'aria-hidden': 'true' }, [...segs, h('span', 'tm-comp-free')]));
          live.push(() => [M.mem, 0.12, M.cache].forEach((gb, i) => { segs[i].style.flexBasis = `${(gb / MEM_USABLE) * 100}%`; }));
        } else {
          const g = graph({ grid: c.grid !== false });
          cell.append(g.el);
          live.push(() => c.draw(g));
        }
        if (c.bottom) cell.append(axis(c.bottom));
        wrap.append(cell);
      }
      area.append(...(gr.top ? [axis(gr.top, 'tm-pd-axis tm-pd-axis-group')] : []), wrap);
    }
    area.addEventListener('contextmenu', (e) => { e.preventDefault(); menu?.(e.clientX, e.clientY); });
    const bigEl = h('div', 'tm-pd-big');
    for (const rowSpec of big) {
      bigEl.append(h('div', 'tm-st-row', null, rowSpec.map(([l, v]) => {
        const vEl = h('span', 'tm-st-v');
        live.push(() => { vEl.textContent = val(v); });
        return h('div', 'tm-st', null, [h('span', 'tm-st-l', { text: l }), vEl]);
      })));
    }
    const kvEl = h('dl', 'tm-pd-kv');
    for (const [l, v] of kv) {
      const dd = h('dd');
      live.push(() => { dd.textContent = val(v); });
      kvEl.append(h('dt', null, { text: l }), dd);
    }
    el.append(h('div', 'tm-pd-head', null, [h('h2', 'tm-pd-title', { text: title }), h('div', 'tm-pd-model', { text: model })]), area, h('div', 'tm-pd-stats', null, [bigEl, kvEl]));
    return {
      el,
      update: () => live.forEach((f) => f()),
      text: () => [title, model, '', ...big.flat().map(([l, v]) => `${l}\t${val(v)}`), ...kv.map(([l, v]) => `${l}\t${val(v)}`)].join('\n'),
    };
  }
  const over60 = (top, right, draw, extra = {}) => ({ top: [top, right], bottom: ['60 seconds', '0'], draw, ...extra });
  const RES = [
    {
      id: 'cpu', name: 'CPU', color: 'cpu', mini: (g) => g.draw(H.cpu), subs: () => [`${F.pct0(M.cpu)}  ${M.ghz.toFixed(2)} GHz`],
      detail: (menu) => perfDetail({
        title: 'CPU', model: CPU_NAME, color: 'cpu', menu,
        groups: [tm.cpuMode === 'logical'
          ? { grow: true, cols: 4, top: ['% Utilization over 60 seconds', '100%'], cells: H.cores.map((c) => ({ draw: (g) => g.draw(c) })) }
          : { grow: true, cells: [over60('% Utilization', '100%', (g) => g.draw(H.cpu))] }],
        big: [[['Utilization', () => F.pct0(M.cpu)], ['Speed', () => `${M.ghz.toFixed(2)} GHz`]],
          [['Processes', () => M.procs], ['Threads', () => thousands(M.threads)], ['Handles', () => thousands(M.handles)]],
          [['Up time', STATS_UP]]],
        kv: [['Base speed:', '3.50 GHz'], ['Sockets:', '1'], ['Cores:', '8'], ['Logical processors:', '16'], ['Virtualization:', 'Not capable'], ['L1 cache:', '512 KB'], ['L2 cache:', '4.0 MB'], ['L3 cache:', '8.0 MB']],
      }),
    },
    {
      id: 'memory', name: 'Memory', color: 'mem', mini: (g) => g.draw(H.mem), subs: () => [`${M.mem.toFixed(1)}/${MEM_USABLE.toFixed(1)} GB (${Math.round(memPct())}%)`],
      detail: (menu) => perfDetail({
        title: 'Memory', model: F.gb(MEM_TOTAL), color: 'mem', menu,
        groups: [{ grow: true, cells: [over60('Memory usage', F.gb(MEM_USABLE), (g) => g.draw(H.mem))] }, { cells: [{ top: ['Memory composition', ''], bar: true }] }],
        big: [[['In use (Compressed)', () => `${F.gb(M.mem)} (0 MB)`], ['Available', () => F.gb(MEM_USABLE - M.mem)]],
          [['Committed', () => `${(M.mem + 0.9).toFixed(1)}/${(MEM_USABLE + 2).toFixed(1)} GB`], ['Cached', () => F.gb(M.cache)]],
          [['Paged pool', '212 MB'], ['Non-paged pool', '96.4 MB']]],
        kv: [['Speed:', '14000 MT/s'], ['Slots used:', 'N/A'], ['Form factor:', 'Other'], ['Hardware reserved:', F.gb(MEM_TOTAL - MEM_USABLE)]],
      }),
    },
    {
      id: 'disk', name: 'Disk 0 (C:)', color: 'disk', mini: (g) => g.draw(H.disk), subs: () => ['SSD', F.pct0(M.disk)],
      detail: (menu) => {
        const cap = `${Math.round((Shell.fs?.get?.('c')?.size || 236e9) / 1e9)} GB`;
        return perfDetail({
          title: 'Disk 0 (C:)', model: 'PS5 internal SSD', color: 'disk', menu,
          groups: [{ grow: true, cells: [over60('Active time', '100%', (g) => g.draw(H.disk))] },
            { cells: [over60('Disk transfer rate', () => diskScale().label, (g) => g.draw(H.read, diskScale().max, H.write))] }],
          big: [[['Active time', () => F.pct0(M.disk)], ['Average response time', () => `${M.resp.toFixed(1)} ms`]],
            [['Read speed', () => F.kbs(M.read)], ['Write speed', () => F.kbs(M.write)]]],
          kv: [['Capacity:', cap], ['Formatted:', cap], ['System disk:', 'Yes'], ['Page file:', 'Yes'], ['Type:', 'SSD (NVMe)']],
        });
      },
    },
    {
      id: 'ethernet', name: 'Ethernet', color: 'net', mini: (g) => g.draw(H.recv, netScale().max, H.send), subs: () => ['Ethernet', `S: ${F.kbps(M.send)} R: ${F.kbps(M.recv)}`],
      detail: (menu) => perfDetail({
        title: 'Ethernet', model: 'Wine Ethernet Adapter', color: 'net', menu,
        groups: [{ grow: true, cells: [over60('Throughput', () => netScale().label, (g) => g.draw(H.recv, netScale().max, H.send))] }],
        big: [[['Send', () => F.kbps(M.send)], ['Receive', () => F.kbps(M.recv)]]],
        kv: [['Adapter name:', 'Ethernet'], ['Connection type:', 'Ethernet'], ['Link speed:', '1.0 Gbps']],
      }),
    },
    {
      id: 'gpu', name: 'GPU 0', color: 'gpu', mini: (g) => g.draw(H.gpu), subs: () => [GPU_NAME, F.pct0(M.gpu)],
      detail: (menu) => perfDetail({
        title: 'GPU', model: GPU_NAME, color: 'gpu', menu,
        groups: [
          { grow: true, cols: 2, cells: [['3D', H.g3d], ['Copy', H.gcopy], ['Video Encode', H.genc], ['Video Decode', H.gdec]].map(([l, arr]) => ({ top: [l, () => F.pct0(last(arr))], draw: (g) => g.draw(arr) })) },
          { cells: [{ top: ['Dedicated GPU memory usage', '0.5 GB'], draw: (g) => g.draw(H.ded, 0.5) }] },
          { cells: [{ top: ['Shared GPU memory usage', '6.2 GB'], draw: (g) => g.draw(H.sha, 6.2) }] },
        ],
        big: [[['Utilization', () => F.pct0(M.gpu)], ['Dedicated GPU memory', () => `${M.ded.toFixed(1)}/0.5 GB`], ['Shared GPU memory', () => `${M.sha.toFixed(1)}/6.2 GB`]],
          [['GPU Memory', () => `${(M.ded + M.sha).toFixed(1)}/6.7 GB`]]],
        kv: [['Driver version:', 'Mesa RADV'], ['DirectX version:', '12 (FL 12.1)'], ['Physical location:', 'PCI bus 0, device 0, function 0']],
      }),
    },
  ];

  function pagePerformance(arg) {
    const el = h('div', 'tm-perf');
    const list = h('div', 'tm-perf-list', { role: 'listbox', 'aria-label': 'Resources' });
    const main = h('div', 'tm-perf-main');
    let detail = null, cur = null;
    const items = RES.map((r) => {
      const g = graph({ grid: false });
      const sub1 = h('span', 'tm-pi-sub'), sub2 = h('span', 'tm-pi-sub');
      const b = h('button', `tm-pi tm-c-${r.color}`, { type: 'button', role: 'option', 'aria-label': r.name, 'data-tooltip': r.name, tabindex: '-1' }, [
        h('span', 'tm-pi-graph', null, [g.el]), h('span', 'tm-pi-text', null, [h('span', 'tm-pi-name', { text: r.name }), sub1, sub2])]);
      b.addEventListener('click', () => pick(r.id));
      list.append(b);
      return { r, b, g, sub1, sub2 };
    });
    list.addEventListener('keydown', (e) => {
      const i = items.findIndex((x) => x.r.id === cur);
      const n = { ArrowDown: i + 1, ArrowUp: i - 1, Home: 0, End: items.length - 1 }[e.key];
      if (n === undefined) return;
      e.preventDefault();
      const it = items[clamp(n, 0, items.length - 1)];
      pick(it.r.id);
      it.b.focus();
    });
    const menu = (x, y) => Shell.menu?.show?.({
      x, y, items: [
        ...(cur === 'cpu' ? [{ label: 'Change graph to', submenu: [
          { label: 'Overall utilization', radio: true, checked: tm.cpuMode !== 'logical', action: () => { tm.cpuMode = 'overall'; pick('cpu', true); } },
          { label: 'Logical processors', radio: true, checked: tm.cpuMode === 'logical', action: () => { tm.cpuMode = 'logical'; pick('cpu', true); } },
        ] }] : []),
        { label: 'View', submenu: RES.map((r) => ({ label: r.name, radio: true, checked: r.id === cur, action: () => pick(r.id) })) },
        { separator: true },
        { label: 'Copy', icon: 'fluent/copy', shortcut: 'Ctrl+C', action: copy },
      ],
    });
    function copy() { try { navigator.clipboard?.writeText(detail.text()).catch(() => {}); } catch { /* not allowed here */ } }
    function pick(id, force) {
      if (id === cur && !force) return;
      cur = id;
      tm.perfSel = id;
      items.forEach(({ r, b }) => { b.classList.toggle('selected', r.id === id); b.setAttribute('aria-selected', String(r.id === id)); b.tabIndex = r.id === id ? 0 : -1; });
      detail = RES.find((r) => r.id === id).detail(menu);
      main.replaceChildren(detail.el);
      detail.update();
    }
    el.append(list, main);
    pick(arg?.res || tm.perfSel || 'cpu');
    const more = cmd('fluent/more-horizontal', 'See more', (b) => {
      const r = b.getBoundingClientRect();
      menu(r.left, r.bottom);
    });
    more.querySelector('.tm-cmd-label').remove();
    return {
      title: 'Performance', search: false, cmds: [runCmd(), more], el, focus: list,
      update() {
        items.forEach(({ r, g, sub1, sub2 }) => {
          r.mini(g);
          const [a, b] = r.subs();
          sub1.textContent = a;
          sub2.textContent = b || '';
          sub2.hidden = !b;
        });
        detail?.update();
      },
      key(e) { if (Shell.keys.comboOf(e) === 'Ctrl+C' && !e.target.closest('input')) { e.preventDefault(); copy(); } },
    };
  }

  /* ---------- App history ---------- */
  const hms = (s) => `${Math.floor(s / 3600)}:${String(Math.floor((s % 3600) / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
  let historySince = Date.now() - 30 * 86400e3;
  function pageHistory() {
    const cols = [
      { id: 'name', label: 'Name', w: 'minmax(220px, 1fr)' },
      { id: 'cputime', label: 'CPU time', w: '110px', right: true },
      { id: 'network', label: 'Network', w: '110px', right: true },
      { id: 'metered', label: 'Metered network', w: '140px', right: true },
      { id: 'tiles', label: 'Tile updates', w: '110px', right: true },
    ];
    const t = treeTable({ cols, label: 'App history', tree: false, onActivate: (n) => Shell.apps.launch(n.app) });
    const note = h('p', 'tm-note');
    const del = cmd('fluent/delete', 'Delete usage history', () => { historySince = Date.now(); refresh(); });
    const nodes = () => {
      const days = (Date.now() - historySince) / 86400e3;
      return (Shell.apps?.list?.() || []).filter((a) => a.id !== 'game').map((a) => {
        const k = hash(a.id), secs = Math.round((k % 2400) * Math.min(1, days / 30)), mb = ((k >>> 8) % 900) / 10 * Math.min(1, days / 30);
        return {
          key: a.id, app: a.id, kind: 'row', name: a.name, icon: a.icon, hay: a.name.toLowerCase(),
          cells: { cputime: hms(secs), network: `${mb.toFixed(1)} MB`, metered: '0 MB', tiles: '0 MB' },
          load: { cputime: Math.sqrt(clamp(secs / 14400, 0, 1)), network: HEAT.memory(mb * 10), metered: 0, tiles: 0 },
          v: { cputime: secs, network: mb, metered: 0, tiles: 0 },
        };
      });
    };
    return {
      title: 'App history', search: true, cmds: [runCmd(), del], el: h('div', 'tm-col', null, [note, t.el]), focus: t.table,
      update() {
        note.textContent = `Resource usage since ${new Date(historySince).toLocaleDateString('en-US')} for current user account.`;
        t.setQuery(tm.query);
        t.render(nodes());
      },
    };
  }

  /* ---------- Startup apps ---------- */
  const STARTUP = [
    { name: 'Game Launcher', pub: 'Example Publisher', icon: 'object/app-game', on: false, impact: 'Not measured' },
    { name: 'Store Launcher', pub: 'Example Publisher', icon: 'object/app-store', on: false, impact: 'Not measured' },
    { name: 'Terminal', pub: 'Mihawk OS', icon: 'object/app-terminal', on: false, impact: 'Not measured' },
    { name: 'Wine Menu Builder', pub: 'Wine', icon: 'object/app-generic', on: true, impact: 'Low' },
  ];
  const startupOn = (s) => prefs.startup[s.name] ?? s.on;
  function pageStartup() {
    const cols = [
      { id: 'name', label: 'Name', w: 'minmax(220px, 1fr)' },
      { id: 'publisher', label: 'Publisher', w: '200px' },
      { id: 'status', label: 'Status', w: '110px' },
      { id: 'impact', label: 'Startup impact', w: '130px' },
    ];
    const set = (on) => { const s = t.selected()?.item; if (!s) return; prefs.startup = { ...prefs.startup, [s.name]: on }; save(); refresh(); };
    const t = treeTable({
      cols, label: 'Startup apps', tree: false, onSelect: () => paint(),
      onMenu: (n, x, y) => Shell.menu?.show?.({ x, y, items: [{ label: startupOn(n.item) ? 'Disable' : 'Enable', action: () => set(!startupOn(n.item)) }] }),
    });
    const enable = cmd('fluent/checkmark', 'Enable', () => set(true));
    const disable = cmd('object/glyph-end-task', 'Disable', () => set(false));
    function paint() {
      const s = t.selected()?.item;
      enable.disabled = !s || startupOn(s);
      disable.disabled = !s || !startupOn(s);
    }
    return {
      title: 'Startup apps', search: true, cmds: [runCmd(), enable, disable], el: t.el, focus: t.table,
      update() {
        t.setQuery(tm.query);
        t.render(STARTUP.map((s) => ({
          key: s.name, item: s, kind: 'row', name: s.name, icon: s.icon, hay: `${s.name} ${s.pub}`.toLowerCase(),
          status: startupOn(s) ? 'Enabled' : 'Disabled', cells: { publisher: s.pub, impact: startupOn(s) ? s.impact : 'None' },
        })));
        paint();
      },
    };
  }

  /* ---------- Users ---------- */
  function pageUsers() {
    const cols = [{ ...COLS.name, label: 'User' }, COLS.status, COLS.cpu, COLS.memory, COLS.disk, COLS.network];
    const t = treeTable({ cols, label: 'Users', onActivate: switchTo });
    const disconnect = cmd('fluent/sign-out', 'Disconnect', async () => {
      if (await contentDialog({ title: 'Do you want to disconnect Mihawk?', text: 'Any unsaved data on this session might be lost.', primary: 'Disconnect user' })) Shell.session?.lock?.();
    });
    const manage = cmd('fluent/person', 'Manage user accounts', () => Shell.apps.launch('settings'));
    return {
      title: 'Users', search: true, cmds: [runCmd(), disconnect, manage], el: t.el, focus: t.table,
      update() {
        const procs = running().all.filter((p) => p.user === 'Mihawk');
        const sum = (k) => procs.reduce((a, p) => a + p[k], 0);
        const [cpu, mem, disk, net] = ['cpu', 'mem', 'disk', 'net'].map(sum);
        t.setQuery(tm.query);
        t.render([{
          key: 'u:mihawk', kind: 'row', name: `Mihawk (${procs.length})`, icon: 'object/user-avatar', hay: 'mihawk', status: '',
          children: procs.map((p) => procNode(p)),
          cells: { cpu: F.pct(cpu), memory: F.mb(mem), disk: F.mbs(disk), network: F.mbps(net) },
          load: { cpu: HEAT.cpu(cpu), memory: HEAT.memory(mem), disk: HEAT.disk(disk), network: HEAT.network(net) },
        }]);
      },
    };
  }

  /* ---------- Details ---------- */
  const PRIORITY = ['Realtime', 'High', 'Above normal', 'Normal', 'Below normal', 'Low'];
  function pageDetails(arg) {
    const cols = [
      { id: 'name', label: 'Name', w: 'minmax(170px, 1fr)' },
      { id: 'pid', label: 'PID', w: '72px', right: true },
      { id: 'status', label: 'Status', w: '116px' },
      { id: 'user', label: 'User name', w: '110px' },
      { id: 'cpu', label: 'CPU', w: '56px', right: true },
      { id: 'memory', label: 'Memory (active private working set)', w: '230px', right: true },
      { id: 'arch', label: 'Architecture', w: '104px' },
      { id: 'desc', label: 'Description', w: 'minmax(220px, 1.4fr)' },
    ];
    const t = treeTable({
      cols, label: 'Details', tree: false, onSelect: () => paint(), onDelete: endTask,
      onMenu(n, x, y) {
        const p = n.proc;
        Shell.menu?.show?.({
          x, y, items: [
            { label: 'End task', action: () => endTask(n) },
            { label: 'End process tree', action: () => endTask(n) },
            { separator: true },
            { label: 'Set priority', submenu: PRIORITY.map((l) => ({ label: l, radio: true, checked: p.priority === l, action: () => { p.priority = l; } })) },
            { label: 'Efficiency mode', checked: p.eff, disabled: p.group === 'win', action: () => toggleEff(p) },
            ...(svcOf(p) ? [{ separator: true }, { label: 'Go to service(s)', action: () => go('services', { svc: svcOf(p).name }) }] : []),
          ],
        });
      },
    });
    const end = cmd('object/glyph-end-task', 'End task', () => { const n = t.selected(); if (n) endTask(n); });
    const eff = cmd('object/glyph-leaf', 'Efficiency mode', () => toggleEff(t.selected()?.proc), { toggle: true });
    function paint() {
      const p = t.selected()?.proc;
      end.disabled = !p;
      eff.disabled = !p || p.group === 'win';
      eff.setAttribute('aria-pressed', String(!eff.disabled && !!p.eff));
    }
    let first = true;
    return {
      title: 'Details', search: true, cmds: [runCmd(), end, eff], el: t.el, focus: t.table,
      update() {
        t.setQuery(tm.query);
        t.render(running().all.map((p) => ({
          key: p.key, kind: 'row', proc: p, name: p.exe, icon: p.icon, hay: `${p.exe} ${p.name} ${p.pid} ${p.pub}`.toLowerCase(),
          status: p.hung ? 'Not responding' : 'Running',
          cells: { pid: p.pid, user: p.user, cpu: String(Math.round(p.cpu)).padStart(2, '0'), memory: F.k(p.mem * 0.62), arch: p.arch, desc: p.name.replace(' (32 bit)', '') },
          v: { pid: p.pid, cpu: p.cpu, memory: p.mem },
        })));
        paint();
        if (first && arg?.pid) t.selectWhere((n) => n.proc?.pid === arg.pid);
        first = false;
      },
    };
  }

  /* ---------- Services ---------- */
  const SERVICES = [
    { name: 'BITS', desc: 'Background Intelligent Transfer Service', group: 'netsvcs' },
    { name: 'EventLog', desc: 'Windows Event Log', group: 'LocalServiceNetworkRestricted', proc: 'eventlog' },
    { name: 'MSIServer', desc: 'Windows Installer', group: '' },
    { name: 'PlugPlay', desc: 'Plug and Play', group: 'DcomLaunch', proc: 'plugplay' },
    { name: 'RpcSs', desc: 'Remote Procedure Call (RPC)', group: 'rpcss', proc: 'rpcss' },
    { name: 'Spooler', desc: 'Print Spooler', group: '' },
    { name: 'StoreLauncherService', desc: 'Store Launcher Service', group: '', proc: 'launcher' },
    { name: 'TermService', desc: 'Remote Desktop Services', group: 'NetworkService' },
    { name: 'Winmgmt', desc: 'Windows Management Instrumentation', group: 'netsvcs', proc: 'wmi' },
    { name: 'wuauserv', desc: 'Windows Update', group: 'netsvcs' },
  ];
  const svcPid = (s) => (s.proc ? (ended.has(s.proc) ? 0 : sysProc(s.proc).pid) : svcState.get(s.name) || 0);
  function setService(s, on) {
    if (s.proc) {
      const p = sysProc(s.proc);
      if (on && ended.has(s.proc)) { usedPids.delete(p.pid); p.pid = newPid(); ended.delete(s.proc); }
      if (!on) ended.add(s.proc);
    } else svcState.set(s.name, on ? newPid() : 0);
    refresh();
  }
  function pageServices(arg) {
    const cols = [
      { id: 'name', label: 'Name', w: '170px' },
      { id: 'pid', label: 'PID', w: '72px', right: true },
      { id: 'desc', label: 'Description', w: 'minmax(240px, 1fr)' },
      { id: 'status', label: 'Status', w: '100px' },
      { id: 'group', label: 'Group', w: '200px' },
    ];
    const restart = (s) => { setService(s, false); setTimeout(() => setService(s, true), 600); };
    const t = treeTable({
      cols, label: 'Services', tree: false, onSelect: () => paint(),
      onMenu(n, x, y) {
        const s = n.svc, on = !!svcPid(s);
        Shell.menu?.show?.({
          x, y, items: [
            { label: 'Start', disabled: on, action: () => setService(s, true) },
            { label: 'Stop', disabled: !on, action: () => setService(s, false) },
            { label: 'Restart', disabled: !on, action: () => restart(s) },
            ...(on && s.proc ? [{ separator: true }, { label: 'Go to details', action: () => go('details', { pid: svcPid(s) }) }] : []),
          ],
        });
      },
    });
    const start = cmd('fluent/play', 'Start', () => setService(t.selected().svc, true));
    const stop = cmd('object/glyph-stop', 'Stop', () => setService(t.selected().svc, false));
    const again = cmd('fluent/arrow-clockwise', 'Restart', () => restart(t.selected().svc));
    function paint() {
      const s = t.selected()?.svc, on = !!(s && svcPid(s));
      start.disabled = !s || on;
      stop.disabled = again.disabled = !on;
    }
    let first = true;
    return {
      title: 'Services', search: true, cmds: [runCmd(), start, stop, again], el: t.el, focus: t.table,
      update() {
        t.setQuery(tm.query);
        t.render(SERVICES.map((s) => {
          const pid = svcPid(s);
          return {
            key: s.name, svc: s, kind: 'row', name: s.name, icon: 'object/glyph-services', hay: `${s.name} ${s.desc} ${pid || ''}`.toLowerCase(),
            status: pid ? 'Running' : 'Stopped', cells: { pid: pid || '', desc: s.desc, group: s.group }, v: { pid },
          };
        }));
        paint();
        if (first && arg?.svc) t.selectWhere((n) => n.svc?.name === arg.svc);
        first = false;
      },
    };
  }

  /* ---------- Settings ---------- */
  function pageSettings() {
    const el = h('div', 'tm-settings');
    const section = (title, ...kids) => h('section', 'tm-set', null, [h('h3', 'tm-set-h', { text: title }), ...kids]);
    const combo = (label, options, value, onChange) => {
      const b = h('button', 'tm-set-combo', { type: 'button', 'aria-label': label });
      Shell.ui?.combo?.(b, { options, value, onChange });
      return b;
    };
    const check = (label, key) => {
      const input = h('input', 'checkbox', { type: 'checkbox' });
      input.checked = !!prefs[key];
      input.addEventListener('change', () => { prefs[key] = input.checked; save(); });
      return h('label', 'tm-set-check', null, [input, h('span', null, { text: label })]);
    };
    el.append(
      section('Default start page', combo('Default start page', PAGES.filter((p) => p.id !== 'settings').map((p) => ({ value: p.id, label: p.label })), prefs.start, (v) => { prefs.start = v; save(); })),
      section('Real time update speed', combo('Real time update speed', [['high', 'High'], ['normal', 'Normal'], ['low', 'Low'], ['paused', 'Paused']].map(([value, label]) => ({ value, label })), prefs.speed, (v) => { prefs.speed = v; save(); schedule(); })),
      section('Window management', check('Minimize on use', 'minimizeOnUse')),
      section('Other options', check('Ask me before applying Efficiency mode', 'askEfficiency')),
    );
    return { title: 'Settings', search: false, cmds: [], el };
  }

  /* ---------- shell: title bar, navigation, pages ---------- */
  const PAGES = [
    { id: 'processes', label: 'Processes', icon: 'fluent/grid', build: pageProcesses },
    { id: 'performance', label: 'Performance', icon: 'object/glyph-performance', build: pagePerformance },
    { id: 'history', label: 'App history', icon: 'fluent/history', build: pageHistory },
    { id: 'startup', label: 'Startup apps', icon: 'object/glyph-startup', build: pageStartup },
    { id: 'users', label: 'Users', icon: 'fluent/person', build: pageUsers },
    { id: 'details', label: 'Details', icon: 'fluent/text-bullet-list', build: pageDetails },
    { id: 'services', label: 'Services', icon: 'object/glyph-services', build: pageServices },
    { id: 'settings', label: 'Settings', icon: 'fluent/settings', build: pageSettings, bottom: true },
  ];
  const WIDE = 1008; // NavigationView's ExpandedModeThresholdWidth

  function go(id, arg) {
    if (!tm) return;
    const def = PAGES.find((p) => p.id === id) || PAGES[0];
    tm.page?.dispose?.();
    tm.pageId = def.id;
    const page = tm.page = def.build(arg);
    tm.titleEl.textContent = page.title;
    tm.cmds.replaceChildren(...page.cmds);
    tm.body.replaceChildren(page.el);
    tm.search.disabled = !page.search;
    tm.nav.select(tm.navItems[def.id], { silent: true });
    page.update?.();
    if (arg?.pid || arg?.svc) page.focus?.focus({ preventScroll: true });
  }
  function schedule() {
    if (!tm) return;
    clearInterval(tm.timer);
    const ms = SPEED[prefs.speed];
    if (ms) tm.timer = setInterval(() => { step(); refresh(); }, ms);
  }
  function setOverlay(on) {
    tm.root.classList.toggle('pane-open', on);
    if (on) tm.pane.classList.add('acrylic');
    else setTimeout(() => { if (tm && !tm.root.classList.contains('pane-open')) tm.pane.classList.remove('acrylic'); }, 250);
    paintBurger();
  }
  function togglePane() {
    if (tm.root.clientWidth >= WIDE) { tm.root.classList.toggle('pane-closed'); paintBurger(); }
    else setOverlay(!tm.root.classList.contains('pane-open'));
  }
  function paintBurger() {
    const wide = tm.root.clientWidth >= WIDE;
    const open = wide ? !tm.root.classList.contains('pane-closed') : tm.root.classList.contains('pane-open');
    const label = open ? 'Close Navigation' : 'Open Navigation';
    tm.burger.setAttribute('aria-label', label);
    tm.burger.dataset.tooltip = label;
    tm.burger.setAttribute('aria-expanded', String(open));
  }

  function build() {
    const t = tm = { query: '', perfSel: 'cpu', cpuMode: 'overall', offs: [] };
    /* title bar: icon, name, the search box centred on the window */
    t.search = h('input', 'textbox tm-search', { type: 'text', placeholder: 'Type a name, publisher, or PID to search', 'aria-label': 'Search', spellcheck: 'false', autocomplete: 'off' });
    t.tb = h('div', 'tm-tb', null, [
      h('span', 'tm-tb-app', null, [Shell.icon('object/app-taskmgr', { size: 16 }), h('span', 'tm-tb-title', { text: 'Task Manager' })]),
      h('div', 'tm-search-wrap', null, [t.search]),
    ]);
    Shell.ui?.textbox?.(t.search, { clear: true });
    t.search.addEventListener('input', () => { t.query = t.search.value.trim().toLowerCase(); refresh(); });
    t.search.addEventListener('keydown', (e) => {
      if (e.key === 'Escape' && t.search.value) { e.preventDefault(); e.stopPropagation(); t.search.value = ''; t.search.dispatchEvent(new Event('input')); }
      else if (e.key === 'ArrowDown' || e.key === 'Enter') { e.preventDefault(); t.page?.focus?.focus(); }
    });
    /* navigation pane */
    t.burger = h('button', 'tm-burger', { type: 'button' }, [Shell.icon('fluent/navigation', { size: 16 })]);
    t.burger.addEventListener('click', togglePane);
    const nav = h('div', 'tm-nav');
    t.navItems = {};
    for (const p of PAGES) {
      if (p.bottom) nav.append(h('div', 'tm-nav-spacer'));
      const b = h('button', 'nav-item', { type: 'button', 'data-tooltip': p.label, 'aria-label': p.label }, [ic(p.icon, 16), h('span', 'tm-nav-label', { text: p.label })]);
      b.dataset.page = p.id;
      nav.append(b);
      t.navItems[p.id] = b;
    }
    t.nav = Shell.ui.navList(nav, {
      onSelect(item) {
        if (t.root.classList.contains('pane-open')) setOverlay(false);
        if (item.dataset.page !== t.pageId) go(item.dataset.page);
      },
    });
    t.pane = h('nav', 'tm-pane', { 'aria-label': 'Task Manager' }, [t.burger, nav]);
    /* content: page header with command bar, page body */
    t.titleEl = h('h1', 'tm-title');
    t.cmds = h('div', 'tm-cmds', { role: 'toolbar', 'aria-label': 'Commands' });
    t.body = h('div', 'tm-body');
    t.shell = h('div', 'tm-shell', null, [
      h('div', 'tm-slot', null, [t.pane]),
      h('main', 'tm-content', null, [h('div', 'tm-head', null, [t.titleEl, t.cmds]), t.body]),
    ]);
    t.root = h('div', 'tm', null, [t.shell]);
    t.root.addEventListener('pointerdown', (e) => { if (t.root.classList.contains('pane-open') && !t.pane.contains(e.target)) setOverlay(false); }, true);
    const onKey = (e) => {
      const combo = Shell.keys.comboOf(e);
      if (combo === 'Ctrl+F' || combo === 'Ctrl+E') { if (!t.search.disabled) { e.preventDefault(); t.search.focus(); t.search.select(); } }
      else if (combo === 'F5') { e.preventDefault(); step(); refresh(); }
      else if (combo === 'Ctrl+Tab' || combo === 'Ctrl+Shift+Tab') {
        e.preventDefault();
        const i = PAGES.findIndex((p) => p.id === t.pageId);
        go(PAGES[(i + (e.shiftKey ? PAGES.length - 1 : 1)) % PAGES.length].id);
        t.navItems[t.pageId].focus();
      } else if (e.key === 'Escape' && t.root.classList.contains('pane-open')) { e.preventDefault(); setOverlay(false); t.burger.focus(); }
      else t.page?.key?.(e);
    };
    t.root.addEventListener('keydown', onKey);
    t.tb.addEventListener('keydown', onKey);
    /* NavigationView display mode follows the window's width */
    let wasWide = null;
    t.ro = new ResizeObserver(() => {
      const wide = t.root.clientWidth >= WIDE;
      if (wide === wasWide) return;
      wasWide = wide;
      if (wide && t.root.classList.contains('pane-open')) setOverlay(false);
      paintBurger();
    });
    t.ro.observe(t.root);
    let queued = false;
    t.offs.push(Shell.on('window', ({ type }) => {
      if (!['open', 'close', 'title', 'icon', 'hung', 'responding'].includes(type) || queued) return;
      queued = true;
      requestAnimationFrame(() => { queued = false; refresh(); });
    }));
    return t;
  }
  function teardown() {
    if (!tm) return;
    clearInterval(tm.timer);
    tm.ro.disconnect();
    tm.offs.forEach((off) => off());
    tm.page?.dispose?.();
    tm = null;
  }

  function open(args = {}) {
    if (tm) {
      if (tm.win.isMinimized()) tm.win.restore();
      tm.win.focus();
      if (args.page) go(args.page, args);
      return tm.win;
    }
    if (!Shell.wm?.open) { console.warn('[taskmgr] Shell.wm is missing'); return null; }
    const t = build();
    const wa = Shell.wm.workArea();
    t.win = Shell.wm.open({
      appId: 'taskmgr', title: 'Task Manager', icon: 'object/app-taskmgr', titlebar: 'tall', titlebarContent: t.tb, background: 'mica', content: t.root,
      width: Math.min(960, wa.width - 32), height: Math.min(700, wa.height - 32), minWidth: Math.min(480, wa.width), minHeight: Math.min(320, wa.height),
      onClose: teardown,
    });
    if (wa.width < 600) t.win.maximize();
    step();
    go(args.page || prefs.start, args);
    schedule();
    return t.win;
  }

  /* ---------- registration ---------- */
  Shell.apps.register({ id: 'taskmgr', name: 'Task Manager', icon: 'object/app-taskmgr', description: 'Processes, performance and startup apps', open });
  Shell.keys.on('Ctrl+Shift+Escape', () => { Shell.apps.launch('taskmgr'); });

  Shell.on('boot', () => setTimeout(() => {
    const want = (n) => Shell.debugOpen.includes(n);
    if (want('hang')) Shell.apps.launch('notepad', { file: 'f-readme' })?.setNotResponding?.(true, { after: 0 });
    if (want('taskmgr-perf')) open({ page: 'performance' });
    else if (want('taskmgr')) open({ page: 'processes' });
  }, 60));
})();
