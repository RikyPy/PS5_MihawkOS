/* MihawkBrowser: the native chrome of src/pw_browser.c, drawn here for the owner's approval
   (docs/MIHAWK_BROWSER.md §4, H4). Pages are CEF's on the console; here a page is a stand-in.
   Owns: the app registration `browser` (pinned after Terminal by load order, Start-pinned) and
   everything inside its window: the 40 px command row (Back, Forward, Reload/Stop as 32 x 32
   subtle buttons at 4 px gaps, the 32 px address box, Downloads), the start view (four launcher
   buttons, 200 x 40 in three columns at 8 px gaps, 120 px down, the fourth alone on row 2), the error panel and the 360 px
   Downloads flyout under its button (Open / Show in folder / Cancel per row).
   Native geometry it mirrors: pw_browser.c TOOLBAR 40, BUTTON 32, GAP 4, PANEL 360.
   Debug captures: ?open=browser, browser-page, browser-loading, browser-error, browser-downloads. */
(() => {
  'use strict';

  const Shell = window.Shell;
  const NAME = 'MihawkBrowser';
  const SEARCH = 'https://html.duckduckgo.com/html/?q='; // PW_BROWSER_SEARCH
  /* pw_browser.c launchers[]; the file names are what each server suggests, sizes only drive the demo */
  const LAUNCHERS = [
    ['Epic Games', 'EpicGamesLauncherInstaller.msi', 2.0e8], ['GOG Galaxy', 'GOG_Galaxy_2.0.exe', 3.1e6],
    ['EA app', 'EAappInstaller.exe', 2.2e6], ['Ubisoft Connect', 'UbisoftConnectInstaller.exe', 1.5e8],
  ];

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
  function iconButton(icon, label) {
    const b = h('button', 'btn subtle icon-only', { type: 'button', 'aria-label': label, 'data-tooltip': label });
    b.append(Shell.icon(icon, { size: 16 }));
    return b;
  }
  const setIcon = (b, icon, label) => { b.replaceChildren(Shell.icon(icon, { size: 16 })); b.setAttribute('aria-label', label); b.dataset.tooltip = label; };

  /* pw_browser_address (src/pw_browser_policy.c), rule for rule: an https or http URL (http upgraded; another scheme
     with // refused), about:blank, a host the user means to visit given https://, a refused scheme, else a search. */
  const REFUSED = { refused: true };
  const REFUSED_SCHEMES = ['about', 'blob', 'chrome', 'data', 'file', 'ftp', 'javascript', 'mailto', 'view-source', 'vbscript'];
  const schemeLength = (t) => { const m = /^[A-Za-z][A-Za-z0-9+.-]*:/.exec(t); return m ? m[0].length - 1 : 0; };
  function looksLikeHost(t) {
    const head = t.slice(0, (t.search(/[/?#]/) + 1 || t.length + 1) - 1);
    if (head && head[0] === '[') return head.includes(']');
    const colon = head.includes(':') ? head.lastIndexOf(':') : head.length;
    if (colon < head.length && !/^\d{1,5}$/.test(head.slice(colon + 1))) return false;
    const host = head.slice(0, colon);
    if (!host || /^[.-]|[.-]$/.test(host) || /[^A-Za-z0-9.\-\u0080-\uffff]/.test(host)) return false;
    if (host.toLowerCase() === 'localhost') return true;
    if (/^[\d.]+$/.test(host)) return host.split('.').length === 4;
    const tld = host.slice(host.lastIndexOf('.') + 1);  // a top-level part of 2+ characters with a letter: "1.5" searches
    return host.includes('.') && tld.length >= 2 && /[A-Za-z\u0080-\uffff]/.test(tld);
  }
  function addressToUrl(text) {
    const t = text.trim();
    if (!t) return null;
    const spaces = /\s/.test(t), scheme = spaces ? 0 : schemeLength(t), name = t.slice(0, scheme).toLowerCase();
    if (scheme && t.length > scheme + 2 && t.startsWith('//', scheme + 1)) return name === 'https' ? t : name === 'http' ? `https${t.slice(4)}` : REFUSED;
    if (!spaces && t.toLowerCase() === 'about:blank') return 'about:blank';
    if (!spaces && looksLikeHost(t)) return `https://${t}`;
    if (scheme && REFUSED_SCHEMES.includes(name)) return REFUSED;
    return SEARCH + [...new TextEncoder().encode(t)].map((b) => (/[A-Za-z0-9._~-]/.test(String.fromCharCode(b)) ? String.fromCharCode(b)
      : b === 32 ? '+' : `%${b.toString(16).toUpperCase().padStart(2, '0')}`)).join('');
  }
  /* tests/test_pw_browser_policy.c's address cases, so the prototype and the native box agree */
  [['https://store.example.com/about/', 'https://store.example.com/about/'], ['  http://example.com/a?b=c  ', 'https://example.com/a?b=c'],
   ['HTTP://Example.com', 'https://Example.com'], ['example.com', 'https://example.com'], ['www.gog.com/galaxy', 'https://www.gog.com/galaxy'],
   ['localhost:8443/x', 'https://localhost:8443/x'], ['192.0.2.20:1749', 'https://192.0.2.20:1749'], ['[::1]:443/', 'https://[::1]:443/'],
   ['bücher.de', 'https://bücher.de'], ['about:blank', 'about:blank'], ['launcher download', `${SEARCH}launcher+download`], ['1.5', `${SEARCH}1.5`],
   ['v2.0', `${SEARCH}v2.0`], ['c++ & co?', `${SEARCH}c%2B%2B+%26+co%3F`], ['re:zero', `${SEARCH}re%3Azero`],
   ['file:///c:/windows/win.ini', REFUSED], ['chrome://gpu', REFUSED], ['javascript:alert(1)', REFUSED], ['data:text/html,x', REFUSED],
   ['ftp://example.com/', REFUSED], ['   ', null],
  ].forEach(([input, want]) => console.assert(addressToUrl(input) === want, `[browser] address "${input}" gave`, addressToUrl(input)));
  const hostOf = (url) => { try { return new URL(url).host; } catch { return url; } };

  /* ---------- downloads (shared by every window, as one Chromium profile) ---------- */
  const downloads = []; // newest first: {id, name, size, received, state: active|done|canceled, fsId}
  let dlSeq = 0;
  const listeners = new Set();
  const changed = () => listeners.forEach((fn) => fn());
  function startDownload(name, size, { received = 0, hold = false } = {}) {
    const d = { id: ++dlSeq, name, size, received, state: 'active', fsId: null };
    downloads.unshift(d);
    const step = () => {
      if (d.state !== 'active' || hold) return; // hold: a capture keeps its progress still
      d.received = Math.min(size, d.received + size / 16);
      if (d.received >= size) {
        d.state = 'done';
        const ext = name.split('.').pop().toLowerCase();
        d.fsId = Shell.fs.create('downloads', { name: Shell.fs.uniqueName('downloads', name.replace(/\.[^.]+$/, ''), ext), kind: 'file', ext, size, icon: 'object/app-generic' }).id;
      } else setTimeout(step, 250); // pw_browser.c's TIMER_TICK
      changed();
    };
    setTimeout(step, 250);
    changed();
    return d;
  }
  const isInstaller = (name) => /\.(exe|msi)$/i.test(name); // pw_browser_is_installer
  const active = () => downloads.filter((d) => d.state === 'active');

  /* ---------- window ---------- */
  function openBrowser({ url, delay, downloadsOpen } = {}) {
    const back = iconButton('fluent/arrow-left', 'Back');
    const forward = iconButton('fluent/arrow-right', 'Forward');
    const reload = iconButton('fluent/arrow-clockwise', 'Reload');
    const dlBtn = iconButton('fluent/arrow-download', 'Downloads');
    const dlBar = h('div', 'progress-bar br-dl-bar', { hidden: true });
    dlBtn.append(dlBar);
    const lock = h('span', 'br-lock', { hidden: true, 'data-tooltip': 'Connection is secure' }, [Shell.icon('fluent/lock-closed', { size: 16 })]);
    const address = h('input', 'textbox br-address', { type: 'text', spellcheck: 'false', autocomplete: 'off', 'aria-label': 'Address and search bar', placeholder: 'Search DuckDuckGo or type an address' });
    const commands = h('div', 'br-commands', { role: 'toolbar', 'aria-label': 'Navigation' },
      [back, forward, reload, h('div', 'br-address-wrap', null, [lock, address]), dlBtn]);

    const start = h('div', 'br-start', null, [
      h('p', 'br-start-text', { text: 'Search the web or type an address above.\nOr get a game launcher:' }),
      h('div', 'br-launchers', { role: 'group', 'aria-label': 'Game launchers' },
        LAUNCHERS.map(([label, file, size]) => {
          const b = h('button', 'btn br-launcher', { type: 'button', text: label });
          b.addEventListener('click', () => { startDownload(file, size); showFlyout(true); });
          return b;
        })),
    ]);
    const page = h('div', 'br-page', { hidden: true });
    const error = h('div', 'br-error', { hidden: true, role: 'alert' });
    const view = h('div', 'br-view', null, [start, page, error]);
    const flyout = h('section', 'br-downloads acrylic surface-flyout', { hidden: true, role: 'dialog', 'aria-label': 'Downloads' });
    const root = h('div', 'br', null, [commands, view, flyout]);

    const win = Shell.wm.open({ appId: 'browser', title: NAME, icon: 'object/app-browser', width: 1280, height: 800, minWidth: 420, minHeight: 300, background: 'solid', content: root,
      onClose: () => { listeners.delete(paintDownloads); Shell.transient.close(flyoutId); } });
    if (!win) return null;
    if (Shell.wm.workArea().width < 600) win.maximize();
    const flyoutId = `browser-downloads-${win.id}`;

    /* ---- navigation: a history list and a stand-in page; loading lasts 600 ms ---- */
    const history = [];
    let at = -1, loading = 0, current = null;
    function paintNav() {
      back.disabled = at <= 0;
      forward.disabled = at >= history.length - 1;
      if (loading) setIcon(reload, 'fluent/dismiss', 'Stop'); else setIcon(reload, 'fluent/arrow-clockwise', 'Reload');
      lock.hidden = page.hidden || !!loading;
      address.classList.toggle('br-has-lock', !lock.hidden);
    }
    function showError(title, detail) {
      error.replaceChildren(...[h('div', 'br-error-title', { text: title }), detail && h('div', 'br-error-detail', { text: detail })].filter(Boolean));
      error.hidden = false;
    }
    /* delay: 0 shows the page at once, Infinity keeps it loading (captures) */
    function load(u, { push = true, delay = 600 } = {}) {
      error.hidden = true;
      if (push) { history.splice(at + 1); history.push(u); at = history.length - 1; }
      address.value = u;
      clearTimeout(loading);
      loading = delay === Infinity ? -1 : setTimeout(() => { loading = 0; finish(u); }, delay);
      if (!delay) { clearTimeout(loading); loading = 0; finish(u); }
      paintNav();
    }
    function finish(u) {
      current = u;
      start.hidden = true;
      const host = hostOf(u);
      if (/(^|\.)invalid$/.test(host)) { page.hidden = true; showError("This page can't be reached.", 'ERR_NAME_NOT_RESOLVED (-105)'); win.setTitle(NAME); }
      else {
        /* the web page itself: drawn by Chromium on the console, so it is not themed */
        page.replaceChildren(h('div', 'br-page-body', null, [
          h('h1', null, { text: host }), h('p', null, { text: 'The page Chromium renders appears here.' }),
          h('div', 'br-page-lines', { 'aria-hidden': 'true' }, [1, 2, 3, 4].map(() => h('span'))),
        ]));
        page.hidden = false;
        win.setTitle(`${host} - ${NAME}`);
      }
      paintNav();
    }
    function stop() { clearTimeout(loading); loading = 0; address.value = current || ''; paintNav(); }
    back.addEventListener('click', () => { if (at > 0) load(history[--at], { push: false }); });
    forward.addEventListener('click', () => { if (at < history.length - 1) load(history[++at], { push: false }); });
    reload.addEventListener('click', () => { if (loading) stop(); else if (current) load(current, { push: false }); });
    address.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        const u = addressToUrl(address.value);
        if (u?.refused) { start.hidden = true; page.hidden = true; showError(`${NAME} opens https:// addresses only.`); }
        else if (u) load(u);
      } else if (e.key === 'Escape') address.value = current || '';
    });
    address.addEventListener('mouseup', () => { if (address.selectionStart === address.selectionEnd) address.select(); });

    /* ---- Downloads flyout ---- */
    function row(d) {
      const pct = d.size ? Math.round((d.received / d.size) * 100) : 0;
      const status = d.state === 'done' ? Shell.fs.formatSize(d.size) : d.state === 'canceled' ? 'Canceled'
        : `${Shell.fs.formatSize(d.received)} of ${Shell.fs.formatSize(d.size)}`;
      const btn = (label, cls, fn) => { const b = h('button', `btn${cls}`, { type: 'button', text: label }); b.addEventListener('click', fn); return b; };
      const actions = d.state === 'active'
        ? [btn('Cancel', '', () => { d.state = 'canceled'; changed(); })]
        : d.state === 'done'
          ? [btn('Open', isInstaller(d.name) ? ' accent' : '', () => Shell.notify?.({ app: NAME, icon: 'object/app-browser', title: `Starting ${d.name}`, body: 'The installer opens in a moment.' })),
            btn('Show in folder', '', () => Shell.apps.launch('explorer', { folder: 'downloads', select: d.fsId }))]
          : [];
      return h('div', `br-dl-row br-dl-${d.state}`, { role: 'listitem' }, [
        Shell.icon('object/app-generic', { size: 32, className: 'br-dl-icon' }),
        h('div', 'br-dl-main', null, [
          h('div', 'br-dl-name', { text: d.name, title: d.name }),
          d.state === 'active' && h('div', 'progress-bar br-dl-progress', { style: `--value:${pct}`, role: 'progressbar', 'aria-valuenow': pct, 'aria-label': d.name }),
          h('div', 'br-dl-status', { text: status }),
          actions.length && h('div', 'br-dl-actions', null, actions),
        ]),
      ]);
    }
    function paintDownloads() {
      const running = active();
      dlBar.hidden = !running.length;
      if (running.length) dlBar.style.setProperty('--value', Math.round(100 * running.reduce((s, d) => s + d.received, 0) / running.reduce((s, d) => s + d.size, 0)));
      if (flyout.hidden) return;
      flyout.replaceChildren(h('div', 'br-dl-head', { text: 'Downloads' }),
        downloads.length ? h('div', 'br-dl-list', { role: 'list' }, downloads.map(row)) : h('div', 'br-dl-empty', { text: 'No downloads yet' }));
    }
    function showFlyout(on) {
      if (on === !flyout.hidden) return;
      if (!on) { Shell.transient.close(flyoutId); return; }
      flyout.hidden = false;
      dlBtn.classList.add('open');
      paintDownloads();
      Shell.animate(flyout, [{ opacity: 0, transform: 'translateY(-12px)' }, { opacity: 1, transform: 'none' }], { duration: 250 });
      Shell.transient.open(flyoutId, { group: `browser-${win.id}`, elements: [flyout, dlBtn], close: () => { flyout.hidden = true; dlBtn.classList.remove('open'); } });
    }
    dlBtn.addEventListener('click', () => showFlyout(flyout.hidden));
    listeners.add(paintDownloads);

    paintNav();
    paintDownloads();
    if (url) load(url, { delay });
    else setTimeout(() => address.focus({ preventScroll: true }));
    if (downloadsOpen) showFlyout(true);
    return win;
  }

  Shell.apps.register({ id: 'browser', name: NAME, icon: 'object/app-browser', pinned: true, startPinned: true, description: 'Web browser', open: () => openBrowser() });

  Shell.on('boot', () => setTimeout(() => {
    const want = (n) => Shell.debugOpen.includes(n);
    if (want('browser')) openBrowser();
    if (want('browser-page')) openBrowser({ url: 'https://example.com/', delay: 0 });
    if (want('browser-loading')) openBrowser({ url: 'https://example.com/', delay: Infinity });
    if (want('browser-error')) openBrowser({ url: 'https://no-such-host.invalid/', delay: 0 });
    if (want('browser-downloads')) {
      startDownload('ExampleSetup.exe', 2.4e6, { received: 2.4e6 }); // finishes on the first tick
      startDownload('EpicGamesLauncherInstaller.msi', 2.0e8, { received: 0.84e8, hold: true });
      openBrowser({ downloadsOpen: true });
    }
  }));
})();
