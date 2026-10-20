/* Notifications: the Windows 11 23H2 notification centre (Win+N) stacked above the calendar
   flyout (month / year / decade views, focus session strip), toasts, and Shell.notify.
   API: Shell.notifications.open/close/toggle/isOpen, Shell.notify({app, title, body, icon?,
   actions?: [{label, action}], onClick?}) -> id. Emits `notifications` {count}.
   Store key owned: calendar-expanded. Seed notifications are synthetic demonstration data. */
(() => {
  'use strict';

  const Shell = window.Shell;
  const ID = 'notifications';
  const TOAST_MS = 5000;
  const MAX_TOASTS = 3;

  function h(tag, props = {}, ...kids) {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(props)) {
      if (v == null || v === false) continue;
      if (k === 'class') n.className = v;
      else if (k === 'text') n.textContent = v;
      else if (k.startsWith('on')) n.addEventListener(k.slice(2), v);
      else n.setAttribute(k, v === true ? '' : v);
    }
    n.append(...kids.flat().filter((c) => c != null && c !== false));
    return n;
  }
  const icon = (name, size = 16) => Shell.icon(name.includes('/') ? name : `fluent/${name}`, { size });
  const iconBtn = (name, label, onclick, cls = '') =>
    h('button', { class: `fly-icon-btn ${cls}`.trim(), type: 'button', 'aria-label': label, 'data-tooltip': label, onclick }, icon(name));

  /* Slide `el` out to the right, then close the gap it leaves (height and the flex gap). */
  async function slideOut(el, { delay = 0 } = {}) {
    await Shell.animate(el, [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'translateX(calc(100% + 12px))' }],
      { duration: 167, delay, easing: Shell.ease.out, fill: 'forwards' });
    const gap = parseFloat(getComputedStyle(el.parentElement).rowGap) || 0;
    el.style.overflow = 'hidden';
    await Shell.animate(el, [{ height: `${el.offsetHeight}px`, marginTop: '0px' }, { height: '0px', marginTop: `${-gap}px` }],
      { duration: 167, easing: Shell.ease.standard, fill: 'forwards' });
    el.remove();
  }

  /* ---------- the store of notifications (newest first) ---------- */
  let items = [];
  let nextId = 1;
  const emitCount = () => Shell.emit('notifications', { count: items.length });

  function remove(id) {
    const i = items.findIndex((n) => n.id === id);
    if (i < 0) return null;
    const [n] = items.splice(i, 1);
    emitCount();
    return n;
  }

  /* Activation (click on a card or toast body) opens the source and removes it, as Windows does. */
  function activate(n, action) {
    dismiss(n.id);
    hideToast(n.id);
    if (stack) close();
    try { (action || n.onClick)?.(); } catch (e) { console.error('[notifications] action failed', e); }
  }

  /* Shared card content: title, body, action buttons. */
  function content(n, cls) {
    return h('div', { class: cls },
      h('p', { class: 'nc-card-title', text: n.title }),
      n.body ? h('p', { class: 'nc-card-body', text: n.body }) : null,
      n.actions?.length ? h('div', { class: 'nc-actions' }, n.actions.map((a) =>
        h('button', { class: 'btn', type: 'button', text: a.label, onclick: (e) => { e.stopPropagation(); activate(n, a.action); } }))) : null);
  }

  Shell.notify = ({ app = 'Windows', title = '', body = '', icon: ic = 'object/app-generic', actions = [], onClick } = {}) => {
    const n = { id: `toast${nextId++}`, app, title, body, icon: ic, actions, onClick, time: Date.now() };
    items.unshift(n);
    emitCount();
    if (stack) renderList({ added: n.id });
    else if (!Shell.settings.doNotDisturb) showToast(n);
    return n.id;
  };

  /* ---------- toasts ---------- */
  let toastStack = null;
  const toasts = new Map(); // id -> {el, timer}

  function showToast(n) {
    if (!toastStack) {
      toastStack = h('div', { class: 'toast-stack', role: 'region', 'aria-label': 'New notifications', 'aria-live': 'polite' });
      Shell.layer('flyouts').append(toastStack);
    }
    const el = h('div', { class: 'toast acrylic surface-flyout', role: 'status', 'data-nid': n.id, onclick: () => activate(n) },
      h('div', { class: 'toast-head' },
        icon(n.icon), h('span', { class: 'toast-app', text: n.app }),
        iconBtn('dismiss', 'Dismiss', (e) => { e.stopPropagation(); hideToast(n.id); })),
      content(n, 'toast-content'));
    const t = { el, timer: 0 };
    const arm = () => { clearTimeout(t.timer); t.timer = setTimeout(() => hideToast(n.id), TOAST_MS); };
    el.addEventListener('pointerenter', () => clearTimeout(t.timer));
    el.addEventListener('pointerleave', arm);
    toasts.set(n.id, t);

    /* Older toasts move up to make room (FLIP); the new one slides in from the right. */
    const before = new Map([...toastStack.children].map((c) => [c, c.getBoundingClientRect().top]));
    toastStack.append(el);
    before.forEach((top, c) => {
      const dy = top - c.getBoundingClientRect().top;
      if (dy) Shell.animate(c, [{ transform: `translateY(${dy}px)` }, { transform: 'none' }], { duration: 250, easing: Shell.ease.standard });
    });
    Shell.animate(el, [{ transform: 'translateX(calc(100% + 12px))' }, { transform: 'none' }], { duration: 250, easing: Shell.ease.in });
    arm();
    const live = [...toasts.keys()];
    if (live.length > MAX_TOASTS) hideToast(live[0]);
  }

  /* The toast leaves the screen; the notification stays in the centre unless it was activated. */
  function hideToast(id) {
    const t = toasts.get(id);
    if (!t) return;
    toasts.delete(id);
    clearTimeout(t.timer);
    t.el.inert = true;
    slideOut(t.el);
  }

  /* ---------- notification centre ---------- */
  let stack = null, nc = null, list = null, cal = null;
  let returnFocus = null, offTick = null;

  function groups() {
    const map = new Map();
    for (const n of items) {
      if (!map.has(n.app)) map.set(n.app, []);
      map.get(n.app).push(n);
    }
    return [...map.entries()];
  }

  function card(n) {
    return h('div', { class: 'nc-card', 'data-nid': n.id, role: 'listitem', tabindex: '0', 'aria-label': `${n.app}, ${n.title}`,
      onclick: () => activate(n),
      onkeydown: (e) => {
        if (e.target !== e.currentTarget) return;
        if (e.key === 'Enter') activate(n);
        if (e.key === 'Delete') dismiss(n.id);
      },
    },
    content(n, 'nc-card-content'),
    iconBtn('dismiss', 'Dismiss', (e) => { e.stopPropagation(); dismiss(n.id); }, 'nc-dismiss'));
  }

  function renderHeader() {
    const header = nc.querySelector('.nc-header');
    const dnd = Shell.settings.doNotDisturb;
    header.replaceChildren(...[
      h('h2', { class: 'fly-title', text: 'Notifications' }),
      items.length ? h('button', { class: 'fly-icon-btn', type: 'button', text: 'Clear all', onclick: clearAll }) : null,
      iconBtn(dnd ? 'alert-off' : 'alert', 'Do not disturb', () => Shell.set('doNotDisturb', !Shell.settings.doNotDisturb)),
    ].filter(Boolean));
    header.lastChild.setAttribute('aria-pressed', String(dnd));
  }

  /* Rebuilds the list; the flyout's height follows with Windows' point-to-point curve. */
  function renderList({ added } = {}) {
    if (!stack) return;
    const from = nc.offsetHeight;
    renderHeader();
    list.replaceChildren(...(items.length
      ? groups().map(([app, ns]) => h('section', { class: 'nc-group', 'aria-label': app },
        h('div', { class: 'nc-group-head' },
          icon(ns[0].icon), h('span', { class: 'nc-app', text: app }),
          iconBtn('dismiss', `Clear all ${app} notifications`, () => dismissGroup(app))),
        h('div', { class: 'nc-cards', role: 'list' }, ns.map(card))))
      : [h('p', { class: 'fly-empty', text: 'No new notifications' })]));
    const to = nc.offsetHeight;
    if (from !== to) Shell.animate(nc, [{ height: `${from}px` }, { height: `${to}px` }], { duration: 250, easing: Shell.ease.standard });
    if (added) {
      const el = list.querySelector(`[data-nid="${added}"]`);
      if (el) Shell.animate(el, [{ opacity: 0, transform: 'translateY(-12px)' }, { opacity: 1, transform: 'none' }], { duration: 250, easing: Shell.ease.in });
    }
  }

  function removeCard(id) {
    const el = list?.querySelector(`[data-nid="${id}"]`);
    if (!el) return;
    const group = el.closest('section.nc-group');
    const target = group && group.querySelectorAll('.nc-card').length === 1 ? group : el;
    target.inert = true;
    slideOut(target).then(() => { if (!items.length) renderList(); });
  }

  function dismiss(id) {
    if (!remove(id)) return;
    removeCard(id);
    if (stack) renderHeader();
  }

  function dismissGroup(app) {
    const ids = items.filter((n) => n.app === app).map((n) => n.id);
    items = items.filter((n) => n.app !== app);
    emitCount();
    const group = [...list.querySelectorAll('section.nc-group')].find((g) => g.getAttribute('aria-label') === app);
    if (!group) return;
    group.inert = true;
    slideOut(group).then(() => { if (!items.length) renderList(); else renderHeader(); });
    ids.forEach(hideToast);
  }

  function clearAll() {
    const els = [...list.querySelectorAll('section.nc-group')];
    items = [];
    emitCount();
    renderHeader();
    els.forEach((g) => { g.inert = true; });
    Promise.all(els.map((g, i) =>
      Shell.animate(g, [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'translateX(calc(100% + 12px))' }],
        { duration: 167, delay: i * 50, easing: Shell.ease.out, fill: 'forwards' }))).then(() => renderList());
    nc.focus({ preventScroll: true });
  }

  /* ---------- calendar ---------- */
  const DOW = ['Su', 'Mo', 'Tu', 'We', 'Th', 'Fr', 'Sa'];
  const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
  const sameDay = (a, b) => a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate();
  const longDate = (d) => d.toLocaleDateString('en-US', { weekday: 'long', month: 'long', day: 'numeric' });

  let mode = 'month';                      // month | year | decade
  let view = { y: 0, m: 0 };
  let selected = null;
  let focusMins = 30;
  let focusSession = null;                 // {end, prevDnd}

  function calHeadingText() {
    if (mode === 'month') return new Date(view.y, view.m, 1).toLocaleDateString('en-US', { month: 'long', year: 'numeric' });
    if (mode === 'year') return String(view.y);
    const d0 = Math.floor(view.y / 10) * 10;
    return `${d0} - ${d0 + 9}`;
  }

  /* Month view: 6 weeks from the Sunday on or before the 1st; any index maps to a date. */
  const dayAt = (i) => new Date(view.y, view.m, 1 - new Date(view.y, view.m, 1).getDay() + i);
  const indexOfDay = (d) => Math.round((d - dayAt(0)) / 864e5);

  function buildGrid() {
    const today = Shell.now();
    if (mode === 'month') {
      const cells = DOW.map((d) => h('span', { class: 'cal-dow', 'aria-hidden': 'true', text: d }));
      for (let i = 0; i < 42; i++) {
        const d = dayAt(i);
        const cls = ['cal-day', d.getMonth() !== view.m && 'other', sameDay(d, today) && 'today', selected && sameDay(d, selected) && 'selected'].filter(Boolean).join(' ');
        cells.push(h('button', {
          class: cls, type: 'button', text: String(d.getDate()), 'data-day': i,
          'aria-label': d.toLocaleDateString('en-US', { weekday: 'long', month: 'long', day: 'numeric', year: 'numeric' }),
          'aria-current': sameDay(d, today) ? 'date' : null,
          onclick: () => {
            selected = d;
            if (d.getMonth() !== view.m) navigate(d < new Date(view.y, view.m, 1) ? -1 : 1, d);
            else renderCal(0, i);
          },
        }));
      }
      return h('div', { class: 'cal-grid', role: 'grid' }, cells);
    }
    if (mode === 'year') {
      return h('div', { class: 'cal-grid year' }, Array.from({ length: 16 }, (_, i) => {
        const y = view.y + Math.floor(i / 12), m = i % 12;
        const cls = ['cal-day', i >= 12 && 'other', y === today.getFullYear() && m === today.getMonth() && 'today'].filter(Boolean).join(' ');
        return h('button', { class: cls, type: 'button', text: MONTHS[m], 'data-day': i, onclick: () => { view = { y, m }; mode = 'month'; renderCal(1); } });
      }));
    }
    const d0 = Math.floor(view.y / 10) * 10;
    return h('div', { class: 'cal-grid year' }, Array.from({ length: 16 }, (_, i) => {
      const y = d0 + i;
      const cls = ['cal-day', i >= 10 && 'other', y === today.getFullYear() && 'today'].filter(Boolean).join(' ');
      return h('button', { class: cls, type: 'button', text: String(y), 'data-day': i, onclick: () => { view = { y, m: view.m }; mode = 'year'; renderCal(1); } });
    }));
  }

  /* dir: -1 = earlier (new grid comes from above), 1 = later, 0 = in place. */
  function renderCal(dir = 0, focusIndex = null) {
    if (!cal) return;
    cal.querySelector('.cal-heading').textContent = calHeadingText();
    const viewEl = cal.querySelector('.cal-view');
    const grid = buildGrid();
    viewEl.replaceChildren(grid);
    if (dir) Shell.animate(grid, [{ opacity: 0, transform: `translateY(${dir * 40}px)` }, { opacity: 1, transform: 'none' }], { duration: 250, easing: Shell.ease.in });
    if (focusIndex != null) grid.querySelector(`[data-day="${focusIndex}"]`)?.focus({ preventScroll: true });
  }

  /* Pages the view; `focusDay` (month view) is the date to focus afterwards. */
  function navigate(dir, focusDay = null) {
    if (mode === 'month') {
      const d = new Date(view.y, view.m + dir, 1);
      view = { y: d.getFullYear(), m: d.getMonth() };
    } else view = { y: view.y + dir * (mode === 'year' ? 1 : 10), m: view.m };
    renderCal(dir, focusDay ? indexOfDay(focusDay) : null);
  }

  /* Arrow keys move between days (or months / years); Page Up / Down change the page. */
  function calKey(e) {
    const cell = e.target.closest?.('.cal-day');
    if (!cell) return;
    const cols = mode === 'month' ? 7 : 4;
    const step = { ArrowLeft: -1, ArrowRight: 1, ArrowUp: -cols, ArrowDown: cols }[e.key];
    if (e.key === 'PageUp' || e.key === 'PageDown') { e.preventDefault(); navigate(e.key === 'PageUp' ? -1 : 1); return; }
    if (!step) return;
    e.preventDefault();
    const next = Number(cell.dataset.day) + step;
    if (mode === 'month') {
      const d = dayAt(next);
      if (d.getMonth() !== view.m) navigate(step > 0 ? 1 : -1, d);
      else cal.querySelector(`.cal-view [data-day="${next}"]`)?.focus();
      return;
    }
    if (next < 0 || next >= 16) { navigate(next < 0 ? -1 : 1); return; }
    cal.querySelector(`.cal-view [data-day="${next}"]`)?.focus();
  }

  const minsText = (m) => `${m} mins`;
  function renderFocus() {
    const strip = cal?.querySelector('.cal-focus');
    if (!strip) return;
    if (focusSession) {
      const left = Math.max(0, Math.round((focusSession.end - Date.now()) / 1000));
      strip.replaceChildren(
        icon('timer'),
        h('span', { class: 'cal-mins', text: `${Math.floor(left / 60)}:${String(left % 60).padStart(2, '0')}` }),
        h('span', { class: 'spacer' }),
        h('button', { class: 'btn', type: 'button', onclick: () => endFocus() }, icon('dismiss'), 'End session'));
      return;
    }
    /* 5-minute steps up to an hour, 15-minute steps beyond. */
    const step = (dir) => {
      const size = dir > 0 ? (focusMins >= 60 ? 15 : 5) : (focusMins > 60 ? 15 : 5);
      focusMins = Math.min(240, Math.max(5, focusMins + dir * size));
      renderFocus();
    };
    strip.replaceChildren(
      h('button', { class: 'btn square', type: 'button', 'aria-label': 'Decrease time', 'data-tooltip': 'Decrease time', disabled: focusMins <= 5, onclick: () => step(-1) }, icon('subtract')),
      h('span', { class: 'cal-mins', text: minsText(focusMins) }),
      h('button', { class: 'btn square', type: 'button', 'aria-label': 'Increase time', 'data-tooltip': 'Increase time', disabled: focusMins >= 240, onclick: () => step(1) }, icon('add')),
      h('span', { class: 'spacer' }),
      h('button', { class: 'btn', type: 'button', onclick: startFocus }, icon('play'), 'Focus'));
  }

  /* A focus session turns on do not disturb for its length, then gives it back. */
  function startFocus() {
    focusSession = { end: Date.now() + focusMins * 60000, prevDnd: !!Shell.settings.doNotDisturb };
    if (!Shell.settings.doNotDisturb) Shell.set('doNotDisturb', true);
    renderFocus();
  }
  function endFocus({ done = false } = {}) {
    if (!focusSession) return;
    const prev = focusSession.prevDnd;
    focusSession = null;
    if (Shell.settings.doNotDisturb !== prev) Shell.set('doNotDisturb', prev);
    renderFocus();
    if (done) Shell.notify({ app: 'Clock', icon: 'fluent/timer', title: 'Focus session complete', body: 'Time for a break.' });
  }

  function buildCalendar() {
    const expanded = Shell.store.get('calendar-expanded', true);
    const today = Shell.now();
    view = { y: today.getFullYear(), m: today.getMonth() };
    mode = 'month';
    const chevron = iconBtn(expanded ? 'chevron-down' : 'chevron-up', expanded ? 'Collapse calendar' : 'Expand calendar', () => setExpanded(!cal.classList.contains('expanded')), 'cal-toggle');
    chevron.setAttribute('aria-expanded', String(expanded));
    const body = h('div', { class: 'cal-body', hidden: !expanded },
      h('div', { class: 'cal-nav' },
        h('button', { class: 'fly-icon-btn cal-heading', type: 'button', onclick: () => { if (mode !== 'decade') { mode = mode === 'month' ? 'year' : 'decade'; renderCal(0); } } }),
        h('span', { class: 'spacer' }),
        iconBtn('chevron-up', 'Previous', () => navigate(-1)),
        iconBtn('chevron-down', 'Next', () => navigate(1))),
      h('div', { class: 'cal-view', onkeydown: calKey }),
      h('div', { class: 'cal-focus' }));
    cal = h('section', { class: `cal acrylic surface-flyout${expanded ? ' expanded' : ''}`, 'aria-label': 'Calendar', tabindex: '-1' },
      h('header', { class: 'cal-header' }, h('h2', { class: 'fly-title cal-date', text: longDate(today) }), chevron),
      body);
    return cal;
  }

  /* Collapse keeps the month visible while the card shrinks over it, then hides it. */
  function setExpanded(on) {
    Shell.store.set('calendar-expanded', on);
    const body = cal.querySelector('.cal-body');
    const btn = iconBtn(on ? 'chevron-down' : 'chevron-up', on ? 'Collapse calendar' : 'Expand calendar', () => setExpanded(!on), 'cal-toggle');
    btn.setAttribute('aria-expanded', String(on));
    cal.querySelector('.cal-toggle').replaceWith(btn);
    btn.focus({ preventScroll: true });
    const from = cal.offsetHeight;
    cal.classList.toggle('expanded', on);
    body.hidden = !on;
    const to = cal.offsetHeight;
    body.hidden = false;
    const c = cal;
    Shell.animate(c, [{ height: `${from}px` }, { height: `${to}px` }], { duration: 250, easing: Shell.ease.standard })
      .then(() => { body.hidden = !c.classList.contains('expanded'); });
  }

  /* Tab cycles through the notification centre and the calendar as one flyout (same helper as
     quicksettings.js): next or previous tabbable in document order, wrapping at either end. */
  function trapTab(e, root) {
    const list = [...root.querySelectorAll('button, input, select, textarea, a[href], [tabindex]')]
      .filter((el) => el.tabIndex >= 0 && !el.disabled && !el.closest('[inert]') && el.getClientRects().length);
    if (!list.length) return;
    e.preventDefault();
    const cur = document.activeElement;
    const F = Node.DOCUMENT_POSITION_FOLLOWING;
    const next = !root.contains(cur) ? null
      : e.shiftKey ? list.filter((el) => el.compareDocumentPosition(cur) & F).at(-1)
        : list.find((el) => cur.compareDocumentPosition(el) & F);
    (next || (e.shiftKey ? list.at(-1) : list[0])).focus();
  }

  /* ---------- open / close ---------- */
  function onKey(e) {
    if (!stack || e.defaultPrevented) return;
    if (e.key === 'Escape') { e.preventDefault(); close(); }
    else if (e.key === 'Tab' && !e.altKey && !e.ctrlKey && !e.metaKey) trapTab(e, stack);
  }

  function onTick() {
    if (!cal) return;
    const d = cal.querySelector('.cal-date');
    const text = longDate(Shell.now());
    if (d.textContent !== text) { d.textContent = text; renderCal(0); }
    if (focusSession) renderFocus();
  }

  function open() {
    if (stack) return;
    returnFocus = document.activeElement;
    [...toasts.keys()].forEach(hideToast); // banners fold into the centre
    list = h('div', { class: 'nc-list' });
    nc = h('section', { class: 'nc acrylic surface-flyout', role: 'dialog', 'aria-label': 'Notifications', tabindex: '-1' },
      h('header', { class: 'nc-header' }), list);
    stack = h('div', { class: 'nc-stack' }, nc, buildCalendar());
    Shell.layer('flyouts').append(h('div', { class: 'fly-host' }, stack));
    renderList();
    renderCal(0);
    renderFocus();
    Shell.transient.open(ID, { close: hide, elements: [stack], group: 'flyout' });
    window.addEventListener('keydown', onKey);
    offTick = Shell.on('tick', onTick);
    [nc, cal].forEach((el) => Shell.animate(el,
      [{ opacity: 0, transform: 'translateY(40px)' }, { opacity: 1, transform: 'none' }],
      { duration: 250, easing: Shell.ease.in }));
    nc.focus({ preventScroll: true });
  }

  function hide() {
    const s = stack;
    if (!s) return;
    const parts = [nc, cal];
    stack = nc = list = cal = null;
    window.removeEventListener('keydown', onKey);
    offTick?.(); offTick = null;
    s.inert = true;
    if (s.contains(document.activeElement) && returnFocus?.isConnected) returnFocus.focus({ preventScroll: true });
    Promise.all(parts.map((el) => Shell.animate(el,
      [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'translateY(40px)' }],
      { duration: 167, easing: Shell.ease.out, fill: 'forwards' }))).then(() => s.parentElement.remove());
  }

  function close() { if (Shell.transient.isOpen(ID)) Shell.transient.close(ID); else hide(); }

  Shell.notifications = {
    open,
    close,
    toggle() { if (stack) close(); else open(); },
    isOpen: () => !!stack,
  };

  Shell.keys.on('Win+N', () => Shell.notifications.toggle());

  Shell.on('setting', ({ key }) => {
    if (key === 'doNotDisturb' && stack) renderHeader();
  });

  /* Focus sessions run while the flyout is closed too. */
  setInterval(() => { if (focusSession && Date.now() >= focusSession.end) endFocus({ done: true }); }, 1000);

  Shell.on('boot', () => {
    const t = Date.now();
    items = [
      { id: 'seed-storage', app: 'Storage', icon: 'object/pictures', title: 'Screenshots saved to Pictures',
        body: 'Three screenshots are in Pictures > Screenshots.', actions: [], time: t - 3600e3,
        onClick: () => Shell.fs.open('f-shots') },
      { id: 'seed-os', app: 'Mihawk OS', icon: 'object/app-settings', title: 'Prototype ready',
        body: 'This is a local preview of the Mihawk 11 desktop for the PS5 Wine runtime.', actions: [], time: t - 7200e3 },
    ];
    emitCount();
    if (Shell.debugOpen.includes('notifications')) open();
    if (Shell.debugOpen.includes('toast')) {
      setTimeout(() => Shell.notify({
        app: 'Snipping Tool', icon: 'object/image', title: 'Screenshot copied to clipboard',
        body: 'Select here to mark up and share the image.', onClick: () => Shell.fs.open('f-shots'),
      }), 300);
    }
  });
})();
