/* Session: the lock screen, sign-in, sleep, restart, shut down and sign out, and the Alt+F4
   "Shut Down Mihawk OS" dialog. Owns js/session.js and css/session.css (contract: CONTRACT.md).
   While a session screen is up, one full-screen root sits at the bottom of #overlays in the dark
   theme (Windows' logon UI is dark in either app mode) and the layers beneath it are inert.
   API: Shell.session.lock(), .sleep(), .restart(), .shutdown(), .signOut(). Win+L locks; the
   wm's `alt-f4-desktop` event opens the dialog. Captures: ?lock, and ?open=lock, signin,
   restarting (held on the "Restarting" screen) or shutdown-dialog. */
(() => {
  'use strict';

  const Shell = window.Shell;
  const EASE = Shell.ease;
  const INERT_LAYERS = ['desktop', 'windows', 'taskbar', 'flyouts'];
  const SIGNIN_IDLE_MS = 60000; // Windows drops back to the lock screen after a minute without input
  const RAISE_MS = 450; // the lock screen sliding up or down (shell motion; Microsoft publishes none)
  const RING = '<span class="session-ring" aria-hidden="true"><i></i><i></i><i></i><i></i><i></i></span>';
  const LOGO = '<img class="session-logo" src="../../themes/windows11/start.svg" alt="">'; // the project's Mihawk mark
  const BUSY_TEXT = { restart: 'Restarting', shutdown: 'Shutting down', signout: 'Signing out' };

  let root = null; // the full-screen session surface, while one is up
  let els = null;
  let screen = null; // 'lock' | 'signin' | 'welcome' | 'busy' | 'sleep' | 'off' | 'boot'
  let flow = 0; // bumped by every transition so an older timed sequence stops where it is
  let timers = [];
  let offs = [];
  let idleTimer = 0;
  let wakeAt = Infinity;
  let raising = false;
  let powerMenu = null;
  let menuClosedAt = 0;
  let savedFocus = null;
  let lockQueued = false; // Win+L during "Welcome": Windows never drops it, it locks once the desktop is up

  const wait = (ms) => new Promise((resolve) => timers.push(setTimeout(resolve, ms)));
  const unlocked = () => !screen || screen === 'lock' || screen === 'signin';

  /* As Windows 11 puts the dark Bloom on its lock screen, the light Aero Meadow desktop gets the
     night meadow here, where the white clock reads; any other wallpaper is used as is. */
  function lockImage() {
    const id = Shell.settings.wallpaper === 'meadow-light' ? 'meadow-dark' : Shell.settings.wallpaper;
    const wp = Shell.wallpapers.find((w) => w.id === id);
    if (!wp) return 'var(--wallpaper)';
    if (!wp.image) return wp.css;
    const tint = wp.dim ? `linear-gradient(rgba(0, 8, 24, ${wp.dim}), rgba(0, 8, 24, ${wp.dim})), ` : '';
    return `${tint}url("${new URL(wp.image, location.href).href}") center / cover no-repeat, ${wp.css}`;
  }

  /* ---------- mounting ---------- */

  function setScreen(name, visual = name) {
    screen = name;
    if (visual) root.dataset.screen = visual;
  }

  /* Returns true when it created the surface (the caller then fades it in over the desktop). */
  function mount() {
    if (root) return false;
    closeDialog(true);
    savedFocus = document.activeElement;
    Shell.transient?.closeAll?.();
    Shell.tooltip?.hide?.();
    Shell.taskView?.close?.();

    root = document.createElement('div');
    root.className = 'session';
    root.dataset.theme = 'dark';
    root.tabIndex = -1;
    root.setAttribute('role', 'dialog');
    root.setAttribute('aria-modal', 'true');
    root.setAttribute('aria-label', 'Lock screen');
    root.style.setProperty('--session-image', lockImage());
    root.innerHTML = `
      <div class="session-bg"></div>
      <div class="session-center">
        <div class="session-user">
          <span class="session-avatar"></span>
          <div class="session-name">Mihawk</div>
        </div>
        <button type="button" class="btn session-signin">Sign in</button>
        <div class="session-progress" role="status">${RING}<span class="session-progress-text"></span></div>
      </div>
      <div class="session-corner">
        <button type="button" class="session-corner-btn" data-icon="wifi" data-tooltip="Network" aria-label="Network"></button>
        <button type="button" class="session-corner-btn" data-icon="accessibility" data-tooltip="Accessibility" aria-label="Accessibility"></button>
        <button type="button" class="session-corner-btn" data-icon="power" data-tooltip="Power" aria-label="Power" aria-haspopup="menu"></button>
      </div>
      <div class="session-lock">
        <div class="session-clock"><div class="session-time"></div><div class="session-date"></div></div>
        <div class="session-status" aria-hidden="true"></div>
      </div>
      <div class="session-black">
        <div class="session-boot">${LOGO}${RING}</div>
        <div class="session-off"><span class="session-off-text">Click to start</span></div>
      </div>`;
    const $ = (sel) => root.querySelector(sel);
    els = {
      center: $('.session-center'), corner: $('.session-corner'), lock: $('.session-lock'),
      time: $('.session-time'), date: $('.session-date'), signin: $('.session-signin'),
      progressText: $('.session-progress-text'), black: $('.session-black'), boot: $('.session-boot'),
      off: $('.session-off'), power: $('[data-icon="power"]'),
    };
    $('.session-avatar').append(Shell.icon('object/user-avatar', { size: 192 }));
    root.querySelectorAll('.session-corner-btn').forEach((b) => b.append(Shell.icon(`fluent/${b.dataset.icon}`, { size: 20 })));
    els.off.prepend(Shell.icon('fluent/power', { size: 32 }));
    const s = Shell.settings;
    const network = s.airplane ? 'airplane' : s.wifi === false ? 'wifi-off' : 'wifi';
    $('.session-status').append(Shell.icon(`fluent/${network}`, { size: 20 }), Shell.icon('fluent/battery', { size: 20 }));
    wire();

    Shell.layer('overlays').prepend(root); // below any screen filter (brightness, night light)
    INERT_LAYERS.forEach((name) => Shell.layer(name)?.setAttribute('inert', ''));
    tickClock();
    offs.push(Shell.on('tick', tickClock));
    window.addEventListener('keydown', onKey, true);
    offs.push(() => window.removeEventListener('keydown', onKey, true));
    return true;
  }

  function unmount() {
    flow++;
    timers.forEach(clearTimeout);
    timers = [];
    clearTimeout(idleTimer);
    offs.forEach((off) => off());
    offs = [];
    powerMenu?.close?.();
    root.remove();
    root = els = screen = null;
    INERT_LAYERS.forEach((name) => Shell.layer(name)?.removeAttribute('inert'));
    if (savedFocus && savedFocus.isConnected) savedFocus.focus({ preventScroll: true });
    savedFocus = null;
  }

  const timeFormat = new Intl.DateTimeFormat('en-US', { hour: 'numeric', minute: '2-digit' });
  const dateFormat = new Intl.DateTimeFormat('en-US', { weekday: 'long', month: 'long', day: 'numeric' });
  function tickClock() {
    if (!els) return;
    const now = Shell.now();
    // the lock screen drops AM/PM in 12-hour locales, as Windows does
    els.time.textContent = timeFormat.formatToParts(now).filter((p) => p.type !== 'dayPeriod').map((p) => p.value).join('').trim();
    els.date.textContent = dateFormat.format(now);
  }

  /* ---------- input ---------- */

  function wire() {
    const { lock } = els;
    let drag = null;
    lock.addEventListener('pointerdown', (e) => {
      if (screen !== 'lock' || raising) return;
      drag = { y: e.clientY, lastY: e.clientY, lastT: e.timeStamp, v: 0, moved: false };
      lock.setPointerCapture(e.pointerId);
    });
    lock.addEventListener('pointermove', (e) => {
      if (!drag) return;
      drag.v = (e.clientY - drag.lastY) / Math.max(1, e.timeStamp - drag.lastT);
      drag.lastY = e.clientY;
      drag.lastT = e.timeStamp;
      if (Math.abs(e.clientY - drag.y) > 4) drag.moved = true;
      lock.style.transform = `translateY(${Math.min(0, e.clientY - drag.y)}px)`;
    });
    const release = (e, cancelled) => {
      if (!drag) return;
      const dy = Math.min(0, e.clientY - drag.y);
      const { moved, v } = drag;
      drag = null;
      // a click, a flick up or a drag past a fifth of the screen raises it; anything else falls back
      if (!cancelled && (!moved || v < -0.5 || -dy > root.clientHeight / 5)) { raise(dy); return; }
      lock.style.transform = '';
      Shell.animate(lock, [{ transform: `translateY(${dy}px)` }, { transform: 'translateY(0)' }], { duration: 250 });
    };
    lock.addEventListener('pointerup', (e) => release(e, false));
    lock.addEventListener('pointercancel', (e) => release(e, true));
    lock.addEventListener('wheel', () => raise(), { passive: true });

    root.addEventListener('pointerdown', () => {
      if (screen === 'sleep') wake();
      else if (screen === 'off') boot();
    });
    root.addEventListener('pointermove', () => { if (screen === 'signin') armIdle(); });
    els.signin.addEventListener('click', signIn);
    els.power.addEventListener('click', openPowerMenu);
  }

  /* Capture phase on window, so a shortcut the core consumes still reaches the session. */
  function onKey(e) {
    if (powerMenu || e.defaultPrevented) return;
    if (screen === 'signin') {
      armIdle();
      if (e.key === 'Escape') { e.preventDefault(); lower(); }
      else if (e.key === 'Tab') cycleFocus(e, [els.signin, ...els.corner.querySelectorAll('button')]);
      return;
    }
    e.preventDefault(); // nothing typed on these screens reaches the desktop beneath
    if (e.repeat) return;
    if (screen === 'lock') raise();
    else if (screen === 'sleep') wake();
    else if (screen === 'off') boot();
  }

  /* Tab wraps through `stops` (Sign in → Network → Accessibility → Power on the sign-in screen,
     the combo box and buttons in the Shut Down dialog), never leaving the modal surface. */
  function cycleFocus(e, stops) {
    const i = stops.indexOf(document.activeElement);
    const last = stops.length - 1;
    e.preventDefault();
    stops[e.shiftKey ? (i <= 0 ? last : i - 1) : (i === last ? 0 : i + 1)].focus({ preventScroll: true });
  }

  function armIdle() {
    clearTimeout(idleTimer);
    idleTimer = setTimeout(lower, SIGNIN_IDLE_MS);
  }

  function openPowerMenu() {
    if (powerMenu || performance.now() - menuClosedAt < 250 || !Shell.menu?.show) return;
    armIdle();
    const r = els.power.getBoundingClientRect();
    powerMenu = Shell.menu.show({
      x: r.left, y: r.top, anchorRect: r, placement: 'top',
      items: [
        { label: 'Sleep', icon: 'fluent/weather-moon', action: () => sleep() },
        { label: 'Shut down', icon: 'fluent/power', action: () => powerCycle('shutdown') },
        { label: 'Restart', icon: 'fluent/arrow-clockwise', action: () => powerCycle('restart') },
      ],
      onClose: () => { powerMenu = null; menuClosedAt = performance.now(); },
    });
  }

  /* ---------- lock and sign-in ---------- */

  function showLock() {
    setScreen('lock');
    els.lock.getAnimations().forEach((a) => a.cancel());
    els.lock.style.transform = '';
    els.center.style.opacity = els.corner.style.opacity = '0';
    tickClock();
    root.focus({ preventScroll: true });
  }

  function lock({ instant = false } = {}) {
    if (screen === 'welcome') lockQueued = true;
    if (screen) return;
    mount();
    flow++;
    showLock();
    if (!instant) Shell.animate(root, [{ opacity: 0 }, { opacity: 1 }], { duration: 250 });
  }

  /* The lock screen slides up off the top and the sign-in screen fades in beneath it. */
  async function raise(fromY = 0) {
    if (screen !== 'lock' || raising) return;
    raising = true;
    const gen = flow;
    const { lock, center, corner } = els;
    lock.style.transform = '';
    center.style.opacity = corner.style.opacity = '';
    const reveal = { duration: 250, delay: 150, fill: 'backwards' };
    Shell.animate(center, [{ opacity: 0, transform: 'translateY(24px)' }, { opacity: 1, transform: 'none' }], reveal);
    Shell.animate(corner, [{ opacity: 0 }, { opacity: 1 }], reveal);
    await Shell.animate(lock, [{ transform: `translateY(${fromY}px)` }, { transform: `translateY(${-root.clientHeight}px)` }],
      { duration: RAISE_MS, easing: EASE.in, fill: 'forwards' });
    raising = false;
    if (gen !== flow) return;
    setScreen('signin');
    lock.getAnimations().forEach((a) => a.cancel());
    armIdle();
    els.signin.focus({ preventScroll: true });
  }

  /* Escape or a minute idle on the sign-in screen brings the lock screen back down. */
  async function lower() {
    if (screen !== 'signin') return;
    clearTimeout(idleTimer);
    const gen = ++flow;
    setScreen('lock');
    root.focus({ preventScroll: true });
    const { lock, center, corner } = els;
    center.style.opacity = corner.style.opacity = '0';
    Shell.animate(center, [{ opacity: 1 }, { opacity: 0 }], { duration: 167, easing: EASE.out });
    Shell.animate(corner, [{ opacity: 1 }, { opacity: 0 }], { duration: 167, easing: EASE.out });
    await Shell.animate(lock, [{ transform: `translateY(${-root.clientHeight}px)` }, { transform: 'translateY(0)' }],
      { duration: RAISE_MS, easing: EASE.in });
    if (gen === flow) tickClock();
  }

  async function signIn() {
    if (screen !== 'signin') return;
    clearTimeout(idleTimer);
    const gen = ++flow;
    lockQueued = false;
    els.progressText.textContent = 'Welcome';
    setScreen('welcome');
    root.focus({ preventScroll: true });
    await wait(1600);
    if (gen !== flow) return;
    await Shell.animate(root, [{ opacity: 1 }, { opacity: 0 }], { duration: 400, easing: EASE.standard, fill: 'forwards' });
    if (gen !== flow) return;
    unmount();
    if (lockQueued) { lockQueued = false; lock(); }
  }

  /* ---------- sleep, restart, shut down, sign out ---------- */

  function fadeBlack(to, duration) {
    const from = getComputedStyle(els.black).opacity;
    els.black.style.opacity = String(to);
    return Shell.animate(els.black, [{ opacity: from }, { opacity: to }], { duration, easing: 'linear' });
  }

  async function sleep() {
    if (!unlocked()) return;
    const fresh = mount();
    const gen = ++flow;
    clearTimeout(idleTimer);
    wakeAt = Infinity;
    setScreen('sleep', fresh ? 'sleep' : null); // over the desktop only the black layer shows
    root.focus({ preventScroll: true });
    await fadeBlack(1, 600);
    if (gen !== flow) return;
    root.dataset.screen = 'sleep';
    wakeAt = performance.now() + 300;
  }

  /* Any key or click wakes the PC to the lock screen. */
  function wake() {
    if (screen !== 'sleep' || performance.now() < wakeAt) return;
    flow++;
    showLock();
    fadeBlack(0, 500);
  }

  async function powerCycle(kind) {
    if (!unlocked()) return;
    const fresh = mount();
    const gen = ++flow;
    clearTimeout(idleTimer);
    els.progressText.textContent = BUSY_TEXT[kind];
    els.center.style.opacity = '';
    setScreen('busy');
    root.setAttribute('aria-label', BUSY_TEXT[kind]);
    root.focus({ preventScroll: true });
    if (fresh) await Shell.animate(root, [{ opacity: 0 }, { opacity: 1 }], { duration: 400, easing: EASE.standard });
    if (gen !== flow) return;
    // Windows closes every app before it signs out, restarts or shuts down
    (Shell.wm?.all?.() || []).forEach((win) => {
      try { win.close(); } catch (err) { console.warn('[session] closing a window failed', err); }
    });
    await wait(kind === 'signout' ? 2000 : 3000);
    if (gen !== flow) return;
    if (kind === 'signout') {
      showLock();
      Shell.animate(els.lock, [{ opacity: 0 }, { opacity: 1 }], { duration: 400, easing: EASE.standard });
      return;
    }
    await fadeBlack(1, 500);
    if (gen !== flow) return;
    if (kind === 'restart') {
      await wait(1000);
      if (gen === flow) boot();
      return;
    }
    setScreen('off');
    Shell.animate(els.off, [{ opacity: 0 }, { opacity: 1 }], { duration: 400, delay: 800, fill: 'backwards' });
  }

  /* Power on: black, the logo with the dots ring below it, then the lock screen. */
  async function boot() {
    if (screen === 'boot') return;
    const gen = ++flow;
    els.black.style.opacity = '1';
    setScreen('boot');
    await Shell.animate(els.boot, [{ opacity: 0 }, { opacity: 1 }], { duration: 250 });
    await wait(3200);
    if (gen !== flow) return;
    await Shell.animate(els.boot, [{ opacity: 1 }, { opacity: 0 }], { duration: 250, easing: EASE.out, fill: 'forwards' });
    await wait(500);
    if (gen !== flow) return;
    showLock();
    els.boot.getAnimations().forEach((a) => a.cancel());
    fadeBlack(0, 600);
  }

  /* ---------- the Alt+F4 "Shut Down Windows" dialog ---------- */

  const CHOICES = [
    { value: 'switch', label: 'Switch user', text: 'Lets another person sign in while your apps and files stay open.', run: () => lock() },
    { value: 'signout', label: 'Sign out', text: 'Closes all apps and signs you out.', run: () => powerCycle('signout') },
    { value: 'sleep', label: 'Sleep', text: 'The PC stays on but uses low power. Apps stay open so when the PC wakes up, you’re instantly back to where you left off.', run: () => sleep() },
    { value: 'shutdown', label: 'Shut down', text: 'Closes all apps and turns off the PC.', run: () => powerCycle('shutdown') },
    { value: 'restart', label: 'Restart', text: 'Closes all apps, turns off the PC, and then turns it on again.', run: () => powerCycle('restart') },
  ];

  let dialog = null; // {el, offs, returnFocus}

  /* Centres the dialog when x is undefined. A drag may push it partly off screen as long as the
     title bar stays reachable; `contain` (a resize) pulls it back fully inside when it fits. */
  function placeDialog(el, x, y, contain = false) {
    const w = el.offsetWidth;
    const bottom = innerHeight - (Shell.layer('taskbar')?.offsetHeight || 48);
    if (x === undefined) { x = (innerWidth - w) / 2; y = (bottom - el.offsetHeight) / 2; }
    const [minX, maxX] = contain ? [0, Math.max(0, innerWidth - w)] : [60 - w, innerWidth - 60];
    const maxY = contain ? Math.max(0, bottom - el.offsetHeight) : bottom - 32;
    el.style.left = `${Math.round(Math.min(Math.max(x, minX), maxX))}px`;
    el.style.top = `${Math.round(Math.min(Math.max(y, 0), maxY))}px`;
  }

  function openDialog() {
    if (dialog || screen) return;
    const el = document.createElement('div');
    el.className = 'sd';
    el.dataset.theme = 'light'; // this Win32 dialog stays light in dark mode on Windows 11
    el.setAttribute('role', 'dialog');
    el.setAttribute('aria-labelledby', 'sd-title');
    el.innerHTML = `
      <div class="sd-titlebar"><span class="sd-title" id="sd-title">Shut Down Mihawk OS</span>
        <button type="button" class="sd-close" aria-label="Close"></button></div>
      <div class="sd-banner" aria-hidden="true">${LOGO}<span class="sd-wordmark">Mihawk 11</span></div>
      <div class="sd-body">
        <span class="sd-icon"></span>
        <div class="sd-main">
          <label class="sd-label" for="sd-choice">What do you want the computer to do?</label>
          <div class="sd-select-wrap"><select class="sd-select" id="sd-choice" aria-describedby="sd-desc">
            ${CHOICES.map((c) => `<option value="${c.value}">${c.label}</option>`).join('')}</select></div>
          <p class="sd-desc" id="sd-desc"></p>
        </div>
      </div>
      <div class="sd-buttons">
        <button type="button" class="btn accent" data-act="ok">OK</button>
        <button type="button" class="btn" data-act="cancel">Cancel</button>
        <button type="button" class="btn" data-act="help">Help</button>
      </div>`;
    const select = el.querySelector('.sd-select');
    const stops = [select, ...el.querySelectorAll('.sd-buttons .btn')]; // the caption Close is no tab stop, as in Win32
    el.querySelector('.sd-close').tabIndex = -1;
    const desc = el.querySelector('.sd-desc');
    el.querySelector('.sd-close').append(Shell.icon('fluent/dismiss', { size: 16 }));
    el.querySelector('.sd-icon').append(Shell.icon('object/this-pc', { size: 32 }));
    el.querySelector('.sd-select-wrap').append(Shell.icon('fluent/chevron-down', { size: 12 }));
    select.value = 'shutdown';
    const describe = () => { desc.textContent = CHOICES.find((c) => c.value === select.value).text; };
    describe();
    select.addEventListener('change', describe);

    const ok = () => {
      const choice = CHOICES.find((c) => c.value === select.value);
      closeDialog();
      choice.run();
    };
    el.querySelector('[data-act="ok"]').addEventListener('click', ok);
    el.querySelector('[data-act="cancel"]').addEventListener('click', () => closeDialog());
    el.querySelector('.sd-close').addEventListener('click', () => closeDialog());

    const bar = el.querySelector('.sd-titlebar');
    bar.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 || e.target.closest('.sd-close')) return;
      const dx = e.clientX - el.offsetLeft;
      const dy = e.clientY - el.offsetTop;
      bar.setPointerCapture(e.pointerId);
      const move = (ev) => placeDialog(el, ev.clientX - dx, ev.clientY - dy);
      const up = () => {
        bar.removeEventListener('pointermove', move);
        bar.removeEventListener('pointerup', up);
        bar.removeEventListener('pointercancel', up);
      };
      bar.addEventListener('pointermove', move);
      bar.addEventListener('pointerup', up);
      bar.addEventListener('pointercancel', up);
    });

    // window capture: a module that claims Escape or Enter through Shell.keys cannot starve the dialog;
    // focus lost to nothing (body) still belongs to this modal dialog
    const onKey = (e) => {
      if (!el.contains(e.target) && e.target !== document.body) return;
      if (e.key === 'Escape') { e.preventDefault(); closeDialog(); }
      else if (e.key === 'Tab' && !e.ctrlKey && !e.altKey && !e.metaKey) cycleFocus(e, stops);
      else if (e.key === 'Enter' && !e.target.closest('button')) { e.preventDefault(); ok(); }
    };
    const onResize = () => placeDialog(el, el.offsetLeft, el.offsetTop, true);
    window.addEventListener('keydown', onKey, true);
    window.addEventListener('resize', onResize);
    dialog = {
      el,
      returnFocus: document.activeElement,
      offs: [() => window.removeEventListener('keydown', onKey, true), () => window.removeEventListener('resize', onResize)],
    };

    Shell.layer('overlays').append(el);
    placeDialog(el);
    Shell.animate(el, [{ opacity: 0, transform: 'scale(0.9)' }, { opacity: 1, transform: 'none' }], { duration: 250, easing: EASE.in });
    select.focus({ preventScroll: true });
  }

  function closeDialog(instant = false) {
    if (!dialog) return;
    const { el, offs, returnFocus } = dialog;
    dialog = null;
    offs.forEach((off) => off());
    if (instant) { el.remove(); return; }
    if (el.contains(document.activeElement) && returnFocus?.isConnected) returnFocus.focus({ preventScroll: true });
    Shell.animate(el, [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'scale(0.95)' }],
      { duration: 167, easing: EASE.out, fill: 'forwards' }).then(() => el.remove());
  }

  /* ---------- public API, shortcuts, boot ---------- */

  Shell.session = {
    lock: () => lock(),
    sleep: () => { sleep(); },
    restart: () => { powerCycle('restart'); },
    shutdown: () => { powerCycle('shutdown'); },
    signOut: () => { powerCycle('signout'); },
  };

  Shell.keys.on('Win+L', () => { lock(); });
  Shell.on('alt-f4-desktop', () => (dialog ? closeDialog() : openDialog()));

  Shell.on('boot', () => {
    // the wm owns Alt+F4; without it the desktop still answers with the dialog
    if (!Shell.wm) Shell.keys.on('Alt+F4', () => (dialog ? closeDialog() : openDialog()));
    const open = Shell.debugOpen || [];
    if (new URLSearchParams(location.search).has('lock') || open.includes('lock')) lock({ instant: true });
    if (open.includes('signin')) {
      lock({ instant: true });
      setScreen('signin');
      els.center.style.opacity = els.corner.style.opacity = '';
    }
    if (open.includes('restarting') && unlocked() && !root) {
      mount();
      els.progressText.textContent = BUSY_TEXT.restart;
      setScreen('busy'); // held: no timers, so captures see this screen
    }
    if (open.includes('shutdown-dialog')) openDialog();
  });
})();
