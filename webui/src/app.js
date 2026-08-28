/**
 * GhostESP V2 WebUI
 * Refactored to use commands.js and parsers.js for alignment
 * with the Android companion app logic.
 */

/* ======================== STATE ======================== */
const state = {
  settings: {},
  comm: { state: 'unknown', connected: false },
  logs: '',
  currentPath: '/mnt',
  pendingRiskCommand: null,
  lastCommand: '',
  actionRegistry: {},
  activeAction: null,
  activeItem: null,
  selectedIndices: new Set(),
  terminalTimer: null,
  commandHistory: [],
  historyIndex: -1,
  commHistory: [],
  commHistoryIndex: -1,
  isLoading: false,
  settingsOpen: new Set(),
  settingsFilter: '',
  reconnectActive: false,
  bleFilter: 'ALL',
  dashboardAutoPopulated: false,
  commLogOffset: 0,
  commPending: 0,
  commPendingTimer: null,
  parseCache: { logs: null, aps: null, stations: null, ble: null, flippers: null, airtags: null, gatt: null, ports: null },
};

const pages = [
  ['dashboard', 'Dashboard', '00'],
  ['wifi', 'WiFi', '01'],
  ['ble', 'BLE', '02'],
  ['badusb', 'BadUSB', '03'],
  ['files', 'Files', '04'],
  ['ghostlink', 'GhostLink', '05'],
  ['settings', 'Settings', '06'],
  ['terminal', 'Terminal', '07'],
];

/* ======================== UTILITIES ======================== */
function $(id) { return document.getElementById(id); }
function api(path, options) { return fetch(path, options); }

function esc(str) { return String(str || '').replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }
function escapeAttr(str) { return String(str).replace(/&/g, '&amp;').replace(/"/g, '&quot;'); }

function formatBytes(bytes) {
  if (!bytes) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  let value = bytes, idx = 0;
  while (value >= 1024 && idx < units.length - 1) { value /= 1024; idx++; }
  return value.toFixed(idx ? 1 : 0) + ' ' + units[idx];
}

function toast(message, type = '') {
  const node = document.createElement('div');
  node.className = 'toast ' + type;
  node.textContent = message;
  $('toasts').appendChild(node);
  setTimeout(() => {
    node.style.opacity = '0';
    node.style.transition = 'opacity .25s ease';
    setTimeout(() => node.remove(), 280);
  }, 3400);
}

function setLoading(el, loading) {
  if (!el) return;
  if (loading) {
    el.dataset.originalText = el.textContent;
    el.innerHTML = '<span class="spinner" style="display:inline-block;vertical-align:middle;margin-right:6px;width:14px;height:14px;border-width:2px;"></span>' + (el.dataset.originalText || '');
    el.disabled = true;
  } else {
    el.textContent = el.dataset.originalText || el.textContent;
    el.disabled = false;
  }
}

/* ======================== COMMAND WRAPPERS ======================== */
function isRisky(command) { return isRiskyCommand(command); }
function canUseGhostLink() { return !!state.comm.connected; }

async function runCommandSequence(commands) {
  for (const command of commands) {
    await new Promise(resolve => {
      runCommand(command, { skipRisk: true, onComplete: resolve });
    });
  }
}

/* ======================== SHELL BUILDERS ======================== */
function buildShell() {
  $('nav').innerHTML = pages.map(([id, label, code], idx) =>
    `<button class="${idx === 0 ? 'active' : ''}" data-page="${id}" aria-label="${label} page"><span>${label}</span><small>${code}</small></button>`
  ).join('');
  document.querySelectorAll('[data-page]').forEach(el =>
    el.addEventListener('click', () => showPage(el.dataset.page))
  );
  buildDashboard();
  buildWifi();
  buildBle();
  buildBadUsb();
  buildSettingsForm();
}

/* ======================== BADUSB ======================== */
const BADUSB_BUILTIN_LABEL = 'Ghost Art (Built-in)';
const HID_KEYCODES = {
  BACKSPACE: 0x2A, ENTER: 0x28, ESC: 0x29, TAB: 0x2B, SPACE: 0x2C,
  RIGHT: 0x4F, LEFT: 0x50, DOWN: 0x51, UP: 0x52,
  HOME: 0x4A, PAGE_UP: 0x4B, PAGE_DOWN: 0x4E, END: 0x4D, INSERT: 0x49, DELETE: 0x4C,
  F1: 0x3A, F2: 0x3B, F3: 0x3C, F4: 0x3D, F5: 0x3E, F6: 0x3F,
  F7: 0x40, F8: 0x41, F9: 0x42, F10: 0x43, F11: 0x44, F12: 0x45,
  A: 0x04, B: 0x05, C: 0x06, D: 0x07, E: 0x08, F: 0x09, G: 0x0A, H: 0x0B,
  I: 0x0C, J: 0x0D, K: 0x0E, L: 0x0F, M: 0x10, N: 0x11, O: 0x12, P: 0x13,
  Q: 0x14, R: 0x15, S: 0x16, T: 0x17, U: 0x18, V: 0x19, W: 0x1A, X: 0x1B,
  Y: 0x1C, Z: 0x1D,
  '1': 0x1E, '2': 0x1F, '3': 0x20, '4': 0x21, '5': 0x22, '6': 0x23, '7': 0x24, '8': 0x25, '9': 0x26, '0': 0x27,
};
const HID_MOD_LEFT_CTRL = 0x01;
const HID_MOD_LEFT_SHIFT = 0x02;

const badusbState = {
  trackpad: { active: false, lastX: 0, lastY: 0, holding: 0, holdTimer: null, moveTimer: null },
  jiggling: false,
  keyboard: false,
  scriptsCache: [],
};

function buildBadUsb() {
  const select = $('badusb-script-select');
  if (!select) return;

  document.querySelectorAll('[data-badusb-tab]').forEach(btn => {
    btn.addEventListener('click', () => {
      if (state.activeAction) closeAction();
      document.querySelectorAll('[data-badusb-tab]').forEach(x => x.classList.toggle('active', x === btn));
      document.querySelectorAll('[data-badusb-page]').forEach(x => x.classList.toggle('active', x.dataset.badusbPage === btn.dataset.badusbTab));
      if (btn.dataset.badusbTab === 'settings') badusbLoadSettings();
    });
  });

  $('badusb-refresh-btn')?.addEventListener('click', () => badusbRefreshScripts(true));
  $('badusb-stop-btn')?.addEventListener('click', () => runCommand(CMD.badusbStop().cmd));
  $('badusb-cancel-btn')?.addEventListener('click', () => {
    runCommandSequence([
      CMD.badusbTrackpadStop().cmd,
      CMD.badusbJiggleStop().cmd,
      CMD.badusbKeyboardStop().cmd,
      CMD.badusbStop().cmd,
    ]);
  });

  $('badusb-run-script-btn')?.addEventListener('click', () => {
    const sel = $('badusb-script-select');
    const value = sel && sel.value;
    if (!value) { toast('Pick a script first', 'warn'); return; }
    if (value === BADUSB_BUILTIN_LABEL) {
      runCommand(CMD.badusbRunBuiltin().cmd);
    } else {
      runCommand(CMD.badusbRun(value).cmd);
    }
  });

  $('badusb-jiggle-start-btn')?.addEventListener('click', () => {
    badusbState.jiggling = true;
    updateBadUsbStatus('Jiggling', 'warn');
    runCommand(CMD.badusbJiggleStart().cmd, { silent: true });
  });
  $('badusb-jiggle-stop-btn')?.addEventListener('click', () => {
    badusbState.jiggling = false;
    updateBadUsbStatus('Idle', '');
    runCommand(CMD.badusbJiggleStop().cmd, { silent: true });
  });

  // Trackpad controls
  $('badusb-trackpad-start-btn')?.addEventListener('click', () => {
    badusbState.trackpad.active = true;
    $('badusb-trackpad').classList.add('active');
    $('badusb-trackpad-status').textContent = 'Active. Drag on the pad, click the buttons, or scroll the wheel area.';
    runCommand(CMD.badusbTrackpadStart().cmd);
  });
  $('badusb-trackpad-stop-btn')?.addEventListener('click', () => {
    badusbStopTrackpad();
  });

  // Trackpad mouse buttons
  document.querySelectorAll('[data-badusb-mouse]').forEach(btn => {
    btn.addEventListener('click', () => {
      const mask = parseInt(btn.dataset.badusbMouse, 10) || 0;
      runCommand(CMD.badusbTrackpadButton(mask).cmd, { silent: true });
      $('badusb-trackpad-status').textContent = `Buttons mask=${mask}`;
    });
  });

  bindTrackpad($('badusb-trackpad'));

  // Keyboard
  $('badusb-kb-start-btn')?.addEventListener('click', () => {
    badusbState.keyboard = true;
    updateBadUsbStatus('Keyboard mode', 'good');
    runCommand(CMD.badusbKeyboardStart().cmd);
  });
  $('badusb-kb-stop-btn')?.addEventListener('click', () => {
    badusbState.keyboard = false;
    updateBadUsbStatus('Idle', '');
    runCommand(CMD.badusbKeyboardStop().cmd);
  });
  $('badusb-kb-send-btn')?.addEventListener('click', () => {
    const text = ($('badusb-kb-input').value || '').trim();
    if (!text) { toast('Type some text first', 'warn'); return; }
    runCommand(CMD.badusbType(text).cmd);
  });
  $('badusb-kb-clear-btn')?.addEventListener('click', () => { $('badusb-kb-input').value = ''; });
  $('badusb-kb-input')?.addEventListener('keydown', e => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      const text = (e.target.value || '');
      const lines = text.split('\n');
      const head = lines.slice(0, -1).join('\n');
      const last = lines[lines.length - 1];
      e.target.value = head;
      if (last) runCommand(CMD.badusbType(last).cmd, { silent: true });
      runCommand(CMD.badusbKeysend(0, HID_KEYCODES.ENTER).cmd, { silent: true });
    }
  });
  document.querySelectorAll('[data-badusb-key]').forEach(btn => {
    btn.addEventListener('click', () => {
      const [modifier, keycode] = (btn.dataset.badusbKey || '').split(' ').map(s => s.trim());
      runCommand(CMD.badusbKeysend(modifier || 0, keycode || 0x00).cmd, { silent: true });
    });
  });

  // Settings
  $('badusb-settings-apply-btn')?.addEventListener('click', badusbApplySettings);
  $('badusb-settings-reload-btn')?.addEventListener('click', badusbLoadSettings);
  $('badusb-settings-reset-btn')?.addEventListener('click', () => {
    if (!confirm('Reset BadUSB settings to firmware defaults?')) return;
    runCommand(CMD.settingsReset('badusb').cmd, { onComplete: () => setTimeout(badusbLoadSettings, 700) });
  });

  // First paint
  badusbRefreshScripts(false);
}

function updateBadUsbStatus(text, dotClass) {
  const label = $('badusb-status-label');
  const dot = $('badusb-status-dot');
  const chip = $('badusb-status-chip');
  if (label) label.textContent = text;
  if (dot) dot.className = 'dot ' + (dotClass || '');
  if (chip) chip.style.borderColor = dotClass === 'good' ? 'rgba(52,211,153,.35)'
    : dotClass === 'bad' ? 'rgba(248,113,113,.35)'
    : dotClass === 'warn' ? 'rgba(245,158,11,.35)' : '';
}

async function badusbRefreshScripts(showToastOnError) {
  const sel = $('badusb-script-select');
  if (!sel) return;
  try {
    const res = await api('/api/sdcard?path=' + encodeURIComponent('/mnt/ghostesp/badusb'));
    if (!res.ok) throw new Error('SD unavailable');
    const data = await res.json();
    const files = (data.files || []).filter(f => f.type !== 'folder' && f.name.toLowerCase().endsWith('.txt'));
    sel.innerHTML = '';
    const opt0 = document.createElement('option');
    opt0.value = BADUSB_BUILTIN_LABEL;
    opt0.textContent = BADUSB_BUILTIN_LABEL;
    sel.appendChild(opt0);
    files.forEach(f => {
      const opt = document.createElement('option');
      opt.value = f.name;
      opt.textContent = `${f.name}${f.size != null ? ' (' + formatBytes(f.size) + ')' : ''}`;
      sel.appendChild(opt);
    });
    badusbState.scriptsCache = files.map(f => f.name);
    if (!files.length) {
      const opt = document.createElement('option');
      opt.value = '';
      opt.textContent = '(no scripts on SD)';
      opt.disabled = true;
      sel.appendChild(opt);
    }
  } catch (e) {
    sel.innerHTML = '<option disabled>SD card unavailable</option>';
    if (showToastOnError) toast('Could not list scripts: ' + e.message, 'bad');
  }
}

function badusbStopTrackpad() {
  badusbState.trackpad.active = false;
  $('badusb-trackpad').classList.remove('active');
  $('badusb-trackpad-status').textContent = 'Inactive. Tap Start to enable relative mouse reporting.';
  if (badusbState.trackpad.holdTimer) { clearTimeout(badusbState.trackpad.holdTimer); badusbState.trackpad.holdTimer = null; }
  if (badusbState.trackpad.moveTimer) { clearTimeout(badusbState.trackpad.moveTimer); badusbState.trackpad.moveTimer = null; }
  badusbState.trackpad.holding = 0;
  runCommand(CMD.badusbTrackpadStop().cmd);
}

function bindTrackpad(el) {
  if (!el) return;
  let pointerId = null;
  let startX = 0, startY = 0, startTime = 0;
  let dragMoved = false;
  let scrollBucket = 0;
  let activePointerStillDown = false;

  const dxThreshold = 8;
  const msHoldThreshold = 300;
  const moveThrottleMs = 12;
  const lastMove = { x: 0, y: 0, t: 0, dx: 0, dy: 0 };
  let moveInFlight = false;

  function flushMove() {
    if (lastMove.dx === 0 && lastMove.dy === 0 && scrollBucket === 0) return;
    if (lastMove.dx !== 0 || lastMove.dy !== 0) {
      if (moveInFlight) return;
      const dx = lastMove.dx, dy = lastMove.dy;
      lastMove.dx = 0;
      lastMove.dy = 0;
      moveInFlight = true;
      runCommand(CMD.badusbTrackpadMove(dx, dy).cmd, { silent: true, onComplete: () => { moveInFlight = false; } });
    }
    if (scrollBucket !== 0) {
      // Wheel via dy overflow — most firmware trackpads do not have a separate wheel command, so encode wheel as dy in panel.
      // To keep changes minimal we skip firmware-side wheel here; UI just animates a counter.
      scrollBucket = 0;
    }
  }

  el.addEventListener('pointerdown', e => {
    if (!badusbState.trackpad.active) {
      toast('Tap Start first', 'warn');
      return;
    }
    if (e.button !== 0) return; // LMB only initiates tap/hold/drag
    pointerId = e.pointerId;
    el.setPointerCapture(pointerId);
    startX = e.clientX; startY = e.clientY;
    startTime = performance.now();
    dragMoved = false;
    activePointerStillDown = true;

    if (badusbState.trackpad.holdTimer) clearTimeout(badusbState.trackpad.holdTimer);
    badusbState.trackpad.holdTimer = setTimeout(() => {
      if (!activePointerStillDown || dragMoved) return;
      // Right-click on long-press
      badusbState.trackpad.holding = 2;
      runCommand(CMD.badusbTrackpadButton(2).cmd, { silent: true });
      $('badusb-trackpad-status').textContent = 'Right-click held';
    }, msHoldThreshold);
  });

  el.addEventListener('pointermove', e => {
    if (!activePointerStillDown || pointerId !== e.pointerId) return;
    const dx = e.clientX - lastMove.x;
    const dy = e.clientY - lastMove.y;
    const totalDx = e.clientX - startX;
    const totalDy = e.clientY - startY;
    lastMove.x = e.clientX;
    lastMove.y = e.clientY;

    // Detect drag intent
    if (!dragMoved && (Math.abs(totalDx) > dxThreshold || Math.abs(totalDy) > dxThreshold)) {
      dragMoved = true;
      if (badusbState.trackpad.holdTimer) { clearTimeout(badusbState.trackpad.holdTimer); badusbState.trackpad.holdTimer = null; }
    }

    // Wheel area
    if (e.target && e.target.id === 'badusb-trackpad-hit') {
      scrollBucket += dy;
      const now = performance.now();
      if (now - lastMove.t > moveThrottleMs) {
        lastMove.t = now;
        // 12px of drag = one wheel notch
        const steps = Math.max(-3, Math.min(3, Math.round(scrollBucket / 12)));
        if (steps) {
          runCommand(CMD.badusbTrackpadWheel(steps * -1).cmd, { silent: true });
          scrollBucket = 0;
        }
      }
      return;
    }

    if (!dragMoved) return;

    // Trackpad acceleration mirrors the on-device curve (4 -> 32 step) — but PC trackpads are usually unscaled; use 1:1 with cap
    lastMove.dx = Math.max(-127, Math.min(127, lastMove.dx + Math.round(dx)));
    lastMove.dy = Math.max(-127, Math.min(127, lastMove.dy + Math.round(dy)));

    const now = performance.now();
    if (now - lastMove.t >= moveThrottleMs) {
      lastMove.t = now;
      flushMove();
    }
  });

  const endHandler = e => {
    if (pointerId !== e.pointerId) return;
    activePointerStillDown = false;
    if (badusbState.trackpad.holdTimer) { clearTimeout(badusbState.trackpad.holdTimer); badusbState.trackpad.holdTimer = null; }

    if (dragMoved) {
      flushMove();
    } else if (performance.now() - startTime < msHoldThreshold) {
      // Tap → L-click
      runCommand(CMD.badusbTrackpadButton(1).cmd, { silent: true });
      setTimeout(() => runCommand(CMD.badusbTrackpadButton(0).cmd, { silent: true }), 30);
      $('badusb-trackpad-status').textContent = 'Left-click';
    }
    if (badusbState.trackpad.holding) {
      badusbState.trackpad.holding = 0;
      runCommand(CMD.badusbTrackpadButton(0).cmd, { silent: true });
    }

    try { el.releasePointerCapture(pointerId); } catch (_) { /* ignore */ }
    pointerId = null;
  };
  el.addEventListener('pointerup', endHandler);
  el.addEventListener('pointercancel', endHandler);
  el.addEventListener('pointerleave', endHandler);
}

async function badusbLoadSettings() {
  try {
    const res = await api('/api/settings');
    if (!res.ok) return;
    const data = await res.json();
    const get = key => data && data[key] != null ? data[key] : null;
    $('badusb-vid').value = get('badusb_vid') != null ? '0x' + Number(get('badusb_vid')).toString(16).toUpperCase().padStart(4, '0') : '';
    $('badusb-pid').value = get('badusb_pid') != null ? '0x' + Number(get('badusb_pid')).toString(16).toUpperCase().padStart(4, '0') : '';
    $('badusb-mfr').value = get('badusb_manufacturer') || '';
    $('badusb-prod').value = get('badusb_product') || '';
    $('badusb-layout').value = String(get('badusb_kb_layout') != null ? get('badusb_kb_layout') : 0);
    $('badusb-rand').checked = !!get('badusb_randomize');
  } catch (e) {
    toast('Could not load settings: ' + e.message, 'bad');
  }
}

async function badusbApplySettings() {
  const vid = parseInt(($('badusb-vid').value || '').toLowerCase().replace(/^0x/, ''), 16);
  const pid = parseInt(($('badusb-pid').value || '').toLowerCase().replace(/^0x/, ''), 16);
  const mfr = $('badusb-mfr').value.trim();
  const prod = $('badusb-prod').value.trim();
  const layout = parseInt($('badusb-layout').value, 10);
  const rand = $('badusb-rand').checked ? 1 : 0;

  if (!Number.isFinite(vid) || vid < 0 || vid > 0xFFFF) { toast('VID must be 0x0000-0xFFFF', 'bad'); return; }
  if (!Number.isFinite(pid) || pid < 0 || pid > 0xFFFF) { toast('PID must be 0x0000-0xFFFF', 'bad'); return; }
  if (!Number.isFinite(layout) || layout < 0 || layout > 4) { toast('Layout must be 0-4', 'bad'); return; }

  const seq = [];
  seq.push(`badusb set_vid 0x${vid.toString(16)}`);
  seq.push(`badusb set_pid 0x${pid.toString(16)}`);
  seq.push(`badusb set_mfr ${JSON.stringify(mfr)}`);
  seq.push(`badusb set_prod ${JSON.stringify(prod)}`);
  seq.push(`badusb set_layout ${layout}`);
  seq.push(`badusb set_rand ${rand}`);
  seq.push('settings save');
  await runCommandSequence(seq);
  toast('BadUSB settings applied', 'good');
}

function showPage(page) {
  const current = document.querySelector('.page.active');
  const isSame = current && current.id === 'page-' + page;
  document.querySelectorAll('.page').forEach(el => el.classList.remove('active'));
  $('page-' + page).classList.add('active');
  document.querySelectorAll('.nav button').forEach(el =>
    el.classList.toggle('active', el.dataset.page === page)
  );
  if (isSame && state.activeAction) closeAction();
  if (page === 'files') loadFiles();
  if (page === 'dashboard') renderDashboard();
  if (page === 'badusb') {
    badusbRefreshScripts(false);
    badusbState.trackpad.active = false;
    if ($('badusb-trackpad')) $('badusb-trackpad').classList.remove('active');
  }
}

/* ======================== WiFi PAGE ======================== */
function buildWifi() {
  state.actionRegistry = {};
  const groups = Object.keys(WIFI_GROUPS);
  document.querySelector('[data-tabs="wifi"]').innerHTML = groups.map((name, i) =>
    `<button class="tab-btn ${i === 0 ? 'active' : ''}" data-wifi-tab="${esc(name)}">${esc(name)}</button>`
  ).join('');
  $('wifi-subpages').innerHTML = groups.map((name, i) =>
    `<div class="subpage ${i === 0 ? 'active' : ''}" data-wifi-page="${esc(name)}">
      <div class="menu-list">${WIFI_GROUPS[name].map(action => actionRow(action, 'wifi')).join('')}</div>
    </div>`
  ).join('');
  document.querySelectorAll('[data-wifi-tab]').forEach(btn =>
    btn.addEventListener('click', () => {
      if (state.activeAction) closeAction();
      document.querySelectorAll('[data-wifi-tab]').forEach(x => x.classList.toggle('active', x === btn));
      document.querySelectorAll('[data-wifi-page]').forEach(x => x.classList.toggle('active', x.dataset.wifiPage === btn.dataset.wifiTab));
    })
  );
}

function buildBle() {
  const groups = Object.keys(BLE_GROUPS);
  const allGroups = [...groups, ...Object.keys(BADBLE_GROUPS)];
  document.querySelector('[data-tabs="ble"]').innerHTML = allGroups.map((name, i) =>
    `<button class="tab-btn ${i === 0 ? 'active' : ''}" data-ble-tab="${esc(name)}">${esc(name)}</button>`
  ).join('');
  $('ble-subpages').innerHTML = allGroups.map((name, i) => {
    const list = BLE_GROUPS[name] || BADBLE_GROUPS[name];
    return `<div class="subpage ${i === 0 ? 'active' : ''}" data-ble-page="${esc(name)}">
      <div class="menu-list">${list.map(action => actionRow(action, 'ble')).join('')}</div>
    </div>`;
  }).join('');
  document.querySelectorAll('[data-ble-tab]').forEach(btn =>
    btn.addEventListener('click', () => {
      if (state.activeAction) closeAction();
      document.querySelectorAll('[data-ble-tab]').forEach(x => x.classList.toggle('active', x === btn));
      document.querySelectorAll('[data-ble-page]').forEach(x => x.classList.toggle('active', x.dataset.blePage === btn.dataset.bleTab));
    })
  );
}

function buildDashboard() {
  const quickActions = [
    { label: 'Scan WiFi', command: CMD.scanAp().cmd, icon: 'WF' },
    { label: 'Scan BLE', command: CMD.bleScan('spam').cmd, icon: 'BT' },
    { label: 'Scan Flippers', command: CMD.bleScan('flipper').cmd, icon: 'FL' },
    { label: 'GPS Info', command: CMD.gpsInfo().cmd, icon: 'GP' },
    { label: 'WiFi Status', command: CMD.wifiStatus().cmd, icon: 'WS' },
    { label: 'Stop All', command: CMD.stop().cmd, icon: 'ST' },
  ];
  $('dash-quick-actions').innerHTML = quickActions.map(a =>
    `<button class="btn dash-action-btn" data-command="${escapeAttr(a.command)}">
      <span class="dash-action-icon">${esc(a.icon)}</span>
      <span>${esc(a.label)}</span>
    </button>`
  ).join('');
  $('dash-refresh-btn').addEventListener('click', () => {
    refreshAll().catch(() => {});
  });
}

function renderDashboard() {
  renderDashboardDevice();
  renderDashboardWifi();
  renderDashboardGhostLink();
  renderDashboardSd();
  renderDashboardFeatures();
  renderDashboardRecent();
}

function renderDashboardDevice() {
  const info = Parsers.chipInfo(state.logs);
  if (info) {
    $('dash-conn-status').textContent = 'Connected';
    $('dash-conn-status').className = 'dash-card-sub good';
    $('dash-model').textContent = info.model || '-';
    $('dash-revision').textContent = info.revision || '-';
    $('dash-cores').textContent = String(info.cores);
    $('dash-idf').textContent = info.idfVersion || '-';
    $('dash-heap').textContent = formatBytes(info.freeHeap);
    $('dash-build').textContent = info.buildConfig || '-';
  } else {
    $('dash-conn-status').textContent = 'No chipinfo yet';
    $('dash-conn-status').className = 'dash-card-sub';
  }
}

function renderDashboardWifi() {
  const ws = Parsers.wifiStatus(state.logs);
  const conn = Parsers.wifiConnection(state.logs);
  if (ws && ws.connected) {
    $('dash-wifi-status').textContent = 'Connected';
    $('dash-wifi-status').className = 'dash-card-sub good';
    $('dash-wifi-ssid').textContent = ws.connectedSsid || '-';
    $('dash-wifi-ip').textContent = conn?.ip || '-';
    $('dash-wifi-rssi').textContent = ws.connectedRssi != null ? ws.connectedRssi + ' dBm' : '-';
    $('dash-wifi-channel').textContent = ws.connectedChannel != null ? String(ws.connectedChannel) : '-';
  } else if (ws) {
    $('dash-wifi-status').textContent = 'Disconnected';
    $('dash-wifi-status').className = 'dash-card-sub bad';
    $('dash-wifi-ssid').textContent = '-';
    $('dash-wifi-ip').textContent = '-';
    $('dash-wifi-rssi').textContent = '-';
    $('dash-wifi-channel').textContent = '-';
  } else {
    $('dash-wifi-status').textContent = 'Unknown';
    $('dash-wifi-status').className = 'dash-card-sub';
  }
}

function renderDashboardGhostLink() {
  const c = state.comm;
  const label = c.state ? c.state.charAt(0).toUpperCase() + c.state.slice(1) : 'Unknown';
  $('dash-ghostlink-state').textContent = label;
  $('dash-ghostlink-remote').textContent = c.is_remote_command ? 'Yes' : 'No';
  if (c.connected) {
    $('dash-ghostlink-status').textContent = 'Connected';
    $('dash-ghostlink-status').className = 'dash-card-sub good';
  } else if (c.state === 'error') {
    $('dash-ghostlink-status').textContent = 'Error';
    $('dash-ghostlink-status').className = 'dash-card-sub bad';
  } else if (c.state === 'scanning' || c.state === 'handshake') {
    $('dash-ghostlink-status').textContent = 'Searching...';
    $('dash-ghostlink-status').className = 'dash-card-sub warn';
  } else {
    $('dash-ghostlink-status').textContent = 'Disconnected';
    $('dash-ghostlink-status').className = 'dash-card-sub';
  }
}

function renderDashboardSd() {
  const sdRows = parseKeyValueBlock(state.logs, /SD\s*Status/i, null);
  if (sdRows.length) {
    const map = {};
    sdRows.forEach(r => map[r.key.toLowerCase()] = r.value);
    $('dash-sd-status').textContent = map['mounted'] === 'true' ? 'Mounted' : 'Not mounted';
    $('dash-sd-status').className = 'dash-card-sub ' + (map['mounted'] === 'true' ? 'good' : '');
    $('dash-sd-path').textContent = map['mount point'] || map['path'] || '/mnt';
    $('dash-sd-used').textContent = map['used'] || '-';
  } else {
    $('dash-sd-status').textContent = 'Unknown';
    $('dash-sd-status').className = 'dash-card-sub';
  }
}

function renderDashboardFeatures() {
  const info = Parsers.chipInfo(state.logs);
  const features = info ? info.enabledFeatures : [];
  const featureLabels = {
    'DISPLAY': 'Display', 'TOUCHSCREEN': 'Touchscreen', 'STATUS_DISPLAY': 'OLED',
    'NFC': 'NFC', 'BADUSB': 'BadUSB', 'INFRARED_TX': 'IR TX', 'INFRARED_RX': 'IR RX',
    'GPS': 'GPS', 'ETHERNET': 'Ethernet', 'BATTERY': 'Battery', 'BATTERY_ADC': 'Battery ADC',
    'FUEL_GAUGE': 'Fuel Gauge', 'RTC_CLOCK': 'RTC', 'COMPASS': 'Compass',
    'ACCELEROMETER': 'Accel', 'JOYSTICK': 'Joystick', 'CARDPUTER': 'Cardputer',
    'TDECK': 'T-Deck', 'ROTARY_ENCODER': 'Rotary', 'USB_KEYBOARD': 'USB KB',
    'GHOST_BOARD': 'Ghost Board', 'S3TWATCH': 'S3TWatch',
    'SD_CARD_SPI': 'SD SPI', 'SD_CARD_MMC': 'SD MMC',
  };
  if (features.length) {
    $('dash-features').innerHTML = '<div class="dash-section"><h3>Enabled Features</h3><div class="dash-feature-chips">' +
      features.map(f => `<span class="chip">${esc(featureLabels[f] || f)}</span>`).join('') + '</div></div>';
  } else {
    $('dash-features').innerHTML = '';
  }
}

function renderDashboardRecent() {
  const { aps } = cachedParse(state.logs);
  const section = $('dash-recent-section');
  const list = $('dash-recent-list');
  if (!aps.length) {
    section.hidden = true;
    return;
  }
  section.hidden = false;
  const recent = aps.slice(-5).reverse();
  list.innerHTML = recent.map(ap => {
    const rssiClass = ap.rssi > -50 ? 'good' : ap.rssi > -70 ? 'warn' : 'bad';
    return `<div class="dash-recent-item">
      <div class="dash-recent-info">
        <strong>${esc(ap.ssid || '(Hidden)')}</strong>
        <small>${esc(ap.bssid)} &middot; Ch ${esc(String(ap.channel))}</small>
      </div>
      <span class="chip ${rssiClass}">${esc(String(ap.rssi))} dBm</span>
    </div>`;
  }).join('');
}

function actionRow(action, section) {
  const meta = action.factory();
  const label = action.label;
  const command = meta.cmd;
  const desc = meta.desc;
  const risky = meta.risky;
  const item = registerAction({ section, label, command, desc, risky, refreshCmd: action.refresh?.(), refreshLabel: action.refreshLabel });
  return `<div class="menu-row">
    <div>
      <h3 class="title-row">${esc(label)}${risky ? '<span class="badge warn">May disconnect</span>' : ''}</h3>
      <p>${esc(desc)}</p>
      <p><code>${esc(command)}</code></p>
    </div>
    <div class="actions">
      <button class="btn primary" data-open-action="${item.id}">Open</button>
    </div>
  </div>`;
}

function registerAction(action) {
  const id = `${action.section}-${slugify(action.label)}-${slugify(action.command)}`;
  state.actionRegistry[id] = { ...action, id };
  return state.actionRegistry[id];
}

function slugify(value) {
  return String(value).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'action';
}

function openAction(actionId) {
  const action = state.actionRegistry[actionId];
  if (!action) return;
  state.activeAction = action;
  state.activeItem = null;
  state.selectedIndices.clear();
  state.bleFilter = 'ALL';
  if (action.section === 'wifi') {
    $('wifi-subpages').hidden = true;
    document.querySelector('[data-tabs="wifi"]').hidden = true;
    $('wifi-detail').hidden = false;
  } else if (action.section === 'ble') {
    $('ble-subpages').hidden = true;
    document.querySelector('[data-tabs="ble"]').hidden = true;
    $('ble-detail').hidden = false;
  }
  renderActionDetail();
}

function closeAction() {
  const section = state.activeAction ? state.activeAction.section : null;
  state.activeAction = null;
  state.activeItem = null;
  state.selectedIndices.clear();
  state.bleFilter = 'ALL';
  if (section === 'wifi') {
    document.querySelector('[data-tabs="wifi"]').hidden = false;
    $('wifi-subpages').hidden = false;
    $('wifi-detail').hidden = true;
    $('wifi-detail').innerHTML = '';
  } else if (section === 'ble') {
    document.querySelector('[data-tabs="ble"]').hidden = false;
    $('ble-subpages').hidden = false;
    $('ble-detail').hidden = true;
    $('ble-detail').innerHTML = '';
  }
}

function backToResults() {
  state.activeItem = null;
  renderActionDetail();
}

function renderActionDetail() {
  const action = state.activeAction;
  if (!action) return;
  const host = $(`${action.section}-detail`);
  if (!host) return;
  host.innerHTML = buildActionDetail(action);
}

function buildActionDetail(action) {
  const result = state.activeItem ? renderItemDetail(state.activeItem) : renderActionResult(action);
  const refreshCommand = action.refreshCmd ? action.refreshCmd.cmd : '';
  const backLabel = state.activeItem ? 'Back to Results' : (action.section === 'wifi' ? 'WiFi' : 'BLE');
  const categoryBadge = `<span class="badge mono">${esc(commandCategory(action.command))}</span>`;
  const safetyBadge = action.risky ? '<span class="badge warn">May disconnect</span>' : '<span class="badge good">Local-safe</span>';
  return `<div class="detail-toolbar">
    <div>
      <button class="btn ghost" data-${state.activeItem ? 'back-results' : 'close-action'}>Back to ${esc(backLabel)}</button>
    </div>
    <div class="actions">
      <button class="btn primary" data-command="${escapeAttr(action.command)}">Run</button>
      ${refreshCommand && !state.activeItem ? `<button class="btn" data-command="${escapeAttr(refreshCommand)}">${esc(action.refreshLabel || 'Refresh')}</button>` : ''}
      <button class="btn ghost" data-remote-command="${escapeAttr(action.command)}">Run via GhostLink</button>
    </div>
  </div>
  <div class="panel-title">
    <div>
      <h2>${esc(action.label)}</h2>
      <p>${esc(action.desc)}</p>
      <p><code>${esc(action.command)}</code></p>
    </div>
    <div style="display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end">
      ${categoryBadge} ${safetyBadge}
    </div>
  </div>
  ${result}`;
}

function renderActionResult(action) {
  const cmd = (action.command || '').trim().toLowerCase();
  const cache = cachedParse(state.logs);
  if (/^(scanap|list -a|scanall)\b/.test(cmd)) return renderApTable(cache.aps);
  if (/^(scansta|list -s)\b/.test(cmd)) return renderStationTable(cache.stations);
  if (/^wifistatus\b/.test(cmd)) {
    const status = Parsers.wifiStatus(state.logs);
    const rows = status ? objectToKeyValueRows(status) : parseKeyValueBlock(state.logs, /===\s*WIFI\s*STATUS\s*===/, /===\s*END\s*STATUS\s*===/);
    return renderKeyValueTable(rows, 'WiFi Status');
  }
  if (/^chipinfo\b/.test(cmd)) {
    const info = Parsers.chipInfo(state.logs);
    if (info) return renderDeviceInfo(info);
    return renderKeyValueTable(parseKeyValueBlock(state.logs, /\[CHIPINFO_START\]/, /\[CHIPINFO_END\]/), 'Chip Information');
  }
  if (/^gpsinfo\b/.test(cmd)) {
    const gps = Parsers.gpsPosition(state.logs);
    return gps ? renderGpsInfo(gps) : renderKeyValueTable(parseKeyValueBlock(state.logs, /GPS\s*Info/i, null), 'GPS Information');
  }
  if (/^blescan\b/.test(cmd)) {
    const { ble, flippers, airtags, gatt } = cachedParse(state.logs);
    const all = [...ble, ...flippers, ...airtags, ...gatt];
    return renderBleTable(all);
  }
  if (/^scanports\b/.test(cmd)) return renderPortTable(cachedParse(state.logs).ports);
  if (/^scanarp\b/.test(cmd)) return renderGenericRows(parseArpScan(state.logs), cmd);
  if (/^capture\b/.test(cmd)) return renderKeyValueTable(parseCaptureStatus(state.logs), 'Capture Status');
  if (/^congestion\b/.test(cmd)) return renderGenericRows(parseCongestion(state.logs), cmd);
  if (/^commstatus\b/.test(cmd)) return renderKeyValueTable(parseCommStatus(state.logs), 'GhostLink Status');
  if (/^(sweep|pineap|startportal|stopportal|listportals|dialconnect|wardrive|blewardriving)\b/.test(cmd)) return renderGenericRows(parseGenericRows(state.logs, action.command), cmd);
  return renderGenericRows(parseGenericRows(state.logs, action.command), cmd);
}

/* ======================== ITEM DETAIL ======================== */
function openItem(type, data) {
  state.activeItem = { type, data };
  renderActionDetail();
}

function renderItemDetail(item) {
  switch (item.type) {
    case 'ap': return renderApDetail(item.data);
    case 'station': return renderStationDetail(item.data);
    case 'ble': return renderBleDetail(item.data);
    case 'port': return renderPortDetail(item.data);
    case 'generic': return renderGenericDetail(item.data);
    default: return '<div class="empty">Unknown item type.</div>';
  }
}

function renderApDetail(data) {
  const isOpen = (data.security || '').toLowerCase().includes('open');
  const passId = `ap-connect-pass-${data.index}`;
  return `<div class="item-detail">
    <div class="status-grid" style="margin-bottom:10px">
      <div class="stat"><label>SSID</label><strong>${esc(data.ssid || '(Hidden)')}</strong></div>
      <div class="stat"><label>BSSID</label><strong><code>${esc(data.bssid)}</code></strong></div>
      <div class="stat"><label>RSSI</label><strong>${esc(data.rssi)} dBm</strong></div>
      <div class="stat"><label>Channel</label><strong>${esc(data.channel)}</strong></div>
      <div class="stat"><label>Security</label><strong>${esc(data.security)}</strong></div>
      <div class="stat"><label>Vendor</label><strong>${esc(data.vendor || '-')}</strong></div>
      ${data.band ? `<div class="stat"><label>Band</label><strong>${esc(data.band)}</strong></div>` : ''}
      ${data.pmf ? `<div class="stat"><label>PMF</label><strong>${esc(data.pmf)}</strong></div>` : ''}
    </div>
    <div class="detail-actions">
      <button class="btn primary" data-item-action="select-ap" data-index="${escapeAttr(data.index)}">Select AP</button>
      <button class="btn danger" data-item-action="deauth" data-index="${escapeAttr(data.index)}">Deauth</button>
      <button class="btn" data-item-action="track-ap">Track AP</button>
      <button class="btn" data-item-action="connect-ap" data-ssid="${escapeAttr(data.ssid)}">Connect</button>
    </div>
    ${!isOpen ? `<div style="margin-top:10px"><input id="${passId}" placeholder="Password (optional)" style="max-width:260px"></div>` : ''}
  </div>`;
}

function renderStationDetail(data) {
  return `<div class="item-detail">
    <div class="status-grid" style="margin-bottom:10px">
      <div class="stat"><label>Station MAC</label><strong><code>${esc(data.mac)}</code></strong></div>
      <div class="stat"><label>STA Vendor</label><strong>${esc(data.vendor || '-')}</strong></div>
      <div class="stat"><label>AP SSID</label><strong>${esc(data.associatedApSsid || '-')}</strong></div>
      <div class="stat"><label>AP BSSID</label><strong><code>${esc(data.apBssid || '-')}</code></strong></div>
      <div class="stat"><label>AP Vendor</label><strong>${esc(data.apVendor || '-')}</strong></div>
      <div class="stat"><label>RSSI</label><strong>${esc(data.rssi || '-')} dBm</strong></div>
    </div>
    <div class="detail-actions">
      <button class="btn primary" data-item-action="select-sta" data-index="${escapeAttr(data.index)}">Select Station</button>
      <button class="btn danger" data-item-action="deauth-sta" data-index="${escapeAttr(data.index)}">Deauth</button>
      <button class="btn" data-item-action="track-sta">Track Station</button>
    </div>
  </div>`;
}

function renderBleDetail(data) {
  const typeMap = { FLIPPER_ZERO: 'Flipper Zero', AIR_TAG: 'AirTag', IPHONE: 'iPhone', SAMSUNG: 'Samsung', GOOGLE: 'Google', GENERIC: 'Generic' };
  return `<div class="item-detail">
    <div class="status-grid" style="margin-bottom:10px">
      <div class="stat"><label>MAC</label><strong><code>${esc(data.mac || '-')}</code></strong></div>
      <div class="stat"><label>Name</label><strong>${esc(data.name || '-')}</strong></div>
      <div class="stat"><label>Type</label><strong>${esc(typeMap[data.type] || data.type || '-')}</strong></div>
      <div class="stat"><label>RSSI</label><strong>${esc(data.rssi || '-')} dBm</strong></div>
    </div>
    <div class="detail-actions">
      <button class="btn" data-item-action="track-ble">Track</button>
      <button class="btn" data-item-action="spoof-airtag">Spoof AirTag</button>
      <button class="btn" data-item-action="enum-gatt">Enumerate GATT</button>
    </div>
  </div>`;
}

function renderPortDetail(data) {
  return `<div class="item-detail">
    <div class="status-grid" style="margin-bottom:10px">
      <div class="stat"><label>Host</label><strong><code>${esc(data.host)}</code></strong></div>
      <div class="stat"><label>Port</label><strong>${esc(data.port)}</strong></div>
      <div class="stat"><label>Protocol</label><strong>${esc(data.proto)}</strong></div>
    </div>
    <div class="detail-actions">
      <button class="btn" data-item-action="copy" data-text="${escapeAttr(data.host + ':' + data.port)}">Copy Host:Port</button>
    </div>
  </div>`;
}

function renderGenericDetail(data) {
  return `<div class="item-detail">
    <div class="status-grid" style="margin-bottom:10px">
      <div class="stat"><label>Entry</label><strong>${esc(data.title)}</strong></div>
      <div class="stat"><label>Detail</label><strong>${esc(data.detail)}</strong></div>
    </div>
  </div>`;
}

function renderDeviceInfo(info) {
  const rows = [
    { key: 'Model', value: info.model },
    { key: 'Revision', value: info.revision },
    { key: 'CPU Cores', value: String(info.cores) },
    { key: 'Features', value: info.features },
    { key: 'Free Heap', value: formatBytes(info.freeHeap) },
    { key: 'Min Free Heap', value: formatBytes(info.minFreeHeap) },
    { key: 'IDF Version', value: info.idfVersion },
    { key: 'Build Config', value: info.buildConfig || '-' },
    { key: 'Enabled Features', value: info.enabledFeatures.join(', ') || '-' },
  ];
  return renderKeyValueTable(rows, 'Chip Information');
}

function renderGpsInfo(gps) {
  const rows = [
    { key: 'Fix', value: gps.fixType },
    { key: 'Satellites', value: `${gps.satellites} / ${gps.satellitesInView} in view` },
    { key: 'Latitude', value: gps.latitude.toFixed(6) },
    { key: 'Longitude', value: gps.longitude.toFixed(6) },
    { key: 'Altitude', value: gps.altitude != null ? gps.altitude + ' m' : '-' },
    { key: 'Speed', value: gps.speed != null ? gps.speed + ' km/h' : '-' },
    { key: 'HDOP', value: gps.hdop != null ? String(gps.hdop) : '-' },
    { key: 'Direction', value: gps.direction != null ? `${gps.direction}° ${gps.directionName || ''}` : '-' },
  ];
  return renderKeyValueTable(rows, 'GPS Position');
}

/* ======================== PARSER FALLBACKS ======================== */
function parsePortScan(text) {
  const rows = [];
  let currentHost = '';
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.replace(/^RX:\s*/, '').trim();
    const host = line.match(/^Scanning\s+(.+)\s+tcp/i);
    if (host) currentHost = host[1];
    const port = line.match(/^\s{0,4}(?:Port\s+)?(\d+)(?:\s*\(?(tcp|udp)\)?)?/i);
    if (port) rows.push({ index: String(rows.length), host: currentHost, port: port[1], proto: (port[2] || 'tcp').toUpperCase() });
    const udp = line.match(/^\s{0,4}UDP\s+(\d+)/i);
    if (udp) rows.push({ index: String(rows.length), host: currentHost, port: udp[1], proto: 'UDP' });
  }
  return rows;
}

function parseArpScan(text) {
  const rows = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.replace(/^RX:\s*/, '').trim();
    const m = line.match(/^\[(\d+)\]\s+(?:IP:\s*)?([0-9.]+)\s*(?:MAC:\s*)?([0-9A-Fa-f:]{17})?/i);
    if (m) rows.push({ index: m[1], title: m[2], detail: m[3] || '' });
    else {
      const m2 = line.match(/^([0-9.]+)\s+([0-9A-Fa-f:]{17})/i);
      if (m2) rows.push({ index: String(rows.length), title: m2[1], detail: m2[2] });
    }
  }
  return rows;
}

function parseCaptureStatus(text) {
  const out = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.replace(/^RX:\s*/, '').trim();
    if (line.startsWith('>')) continue;
    const kv = line.match(/^([^=:]+)\s*[=:]\s*(.+)$/);
    if (kv) out.push({ key: kv[1].trim(), value: kv[2].trim() });
    else if (/(started|stopped|Saved)/i.test(line)) out.push({ key: 'Status', value: line });
  }
  return out;
}

function parseCongestion(text) {
  const rows = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.replace(/^RX:\s*/, '').trim();
    const m = line.match(/^\s*(?:Channel\s*)?(\d+)[\s:=-]+(\d+)/i);
    if (m) rows.push({ index: String(rows.length), title: 'Channel ' + m[1], detail: m[2] + ' networks' });
  }
  return rows;
}

function parseCommStatus(text) {
  const out = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.replace(/^RX:\s*/, '').trim();
    const kv = line.match(/^([^=:]+)\s*[=:]\s*(.+)$/);
    if (kv) out.push({ key: kv[1].trim(), value: kv[2].trim() });
  }
  return out;
}

function parseGenericRows(text, command) {
  const lines = text.split(/\r?\n/).map(l => l.trimEnd()).filter(Boolean);
  const commandIndex = command ? lines.map(l => l.replace(/^RX:\s*/, '')).lastIndexOf(`> ${command}`) : -1;
  const output = (commandIndex >= 0 ? lines.slice(commandIndex + 1) : lines.slice(-30))
    .filter(l => !l.replace(/^RX:\s*/, '').startsWith('>'))
    .slice(-24);
  return output.map((line, index) => {
    const stripped = line.replace(/^RX:\s*/, '').trim();
    const kv = stripped.match(/^([^=:\n]{2,40})\s*[=:]\s*(.+)$/);
    return kv
      ? { index: String(index), title: kv[1].trim(), detail: kv[2].trim() }
      : { index: String(index), title: stripped, detail: '' };
  });
}

/* ======================== RENDERERS ======================== */
function renderApTable(rows) {
  if (!rows.length) return '<div class="empty">No AP results yet. Press Scan APs, wait for reconnect if needed, then this page will populate from device logs.</div>';
  const batch = renderBatchToolbar('ap');
  const table = `<div class="table-wrap"><table><thead><tr>
    <th style="width:36px"><input type="checkbox" data-select-all="ap" aria-label="Select all APs"></th>
    <th>#</th><th>SSID</th><th>BSSID</th><th>RSSI</th><th>Ch</th><th>Security</th><th>Vendor</th>
  </tr></thead><tbody>${rows.map(row => {
    const selected = state.selectedIndices.has(row.index);
    return `<tr class="clickable-row" data-select-item="ap" data-index="${escapeAttr(row.index)}">
      <td><input type="checkbox" data-select-check="ap" data-index="${escapeAttr(row.index)}" ${selected ? 'checked' : ''} onclick="event.stopPropagation()" aria-label="Select AP ${esc(row.index)}"></td>
      <td>${esc(row.index)}</td>
      <td>${esc(row.ssid || '(Hidden)')}</td>
      <td><code>${esc(row.bssid)}</code></td>
      <td>${esc(row.rssi)}</td>
      <td>${esc(row.channel)}</td>
      <td>${esc(row.security)}</td>
      <td>${esc(row.vendor || '-')}</td>
    </tr>`;
  }).join('')}</tbody></table></div>`;
  return batch + table;
}

function renderStationTable(rows) {
  if (!rows.length) return '<div class="empty">No station results yet. Run the station scan, wait for reconnect if needed, then this page will populate from device logs.</div>';
  const batch = renderBatchToolbar('station');
  const table = `<div class="table-wrap"><table><thead><tr>
    <th style="width:36px"><input type="checkbox" data-select-all="station" aria-label="Select all stations"></th>
    <th>#</th><th>Station</th><th>STA Vendor</th><th>AP SSID</th><th>AP BSSID</th><th>AP Vendor</th>
  </tr></thead><tbody>${rows.map(row => {
    const selected = state.selectedIndices.has(row.index);
    return `<tr class="clickable-row" data-select-item="station" data-index="${escapeAttr(row.index)}">
      <td><input type="checkbox" data-select-check="station" data-index="${escapeAttr(row.index)}" ${selected ? 'checked' : ''} onclick="event.stopPropagation()" aria-label="Select station ${esc(row.index)}"></td>
      <td>${esc(row.index)}</td>
      <td><code>${esc(row.mac)}</code></td>
      <td>${esc(row.vendor || '-')}</td>
      <td>${esc(row.associatedApSsid || '-')}</td>
      <td><code>${esc(row.apBssid || '-')}</code></td>
      <td>${esc(row.apVendor || '-')}</td>
    </tr>`;
  }).join('')}</tbody></table></div>`;
  return batch + table;
}

function renderBatchToolbar(type) {
  const count = state.selectedIndices.size;
  if (!count) return '';
  const label = type === 'ap' ? 'AP' : 'Station';
  const action = type === 'ap' ? 'deauth' : null;
  return `<div class="batch-toolbar"><span>${count} ${label}${count > 1 ? 's' : ''} selected</span><div class="actions">
    ${action ? `<button class="btn danger" data-batch-action="${action}">Deauth Selected</button>` : '<span class="chip warn">Firmware only supports single-station deauth</span>'}
    <button class="btn ghost" data-batch-action="clear">Clear</button>
  </div></div>`;
}

function renderKeyValueTable(rows, caption) {
  if (!rows.length) return `<div class="empty">No ${esc(caption)} data yet. Run the command to populate.</div>`;
  return `<div class="table-wrap"><table><thead><tr><th>Key</th><th>Value</th></tr></thead><tbody>
    ${rows.map(r => `<tr><td>${esc(r.key)}</td><td><code>${esc(r.value)}</code></td></tr>`).join('')}
  </tbody></table></div>`;
}

function objectToKeyValueRows(value) {
  return Object.entries(value).map(([key, rowValue]) => ({
    key,
    value: rowValue == null ? '-' : String(rowValue),
  }));
}

function renderBleTable(rows) {
  if (!rows.length) return '<div class="empty">No BLE results yet. Run a BLE scan to populate.</div>';
  const filters = ['ALL', 'FLIPPER_ZERO', 'AIR_TAG', 'GATT', 'GENERIC'];
  const activeFilter = state.bleFilter || 'ALL';
  const filtered = activeFilter === 'ALL' ? rows : rows.filter(r => (r.type || 'GENERIC') === activeFilter);
  const chips = filters.map(f => {
    const count = f === 'ALL' ? rows.length : rows.filter(r => (r.type || 'GENERIC') === f).length;
    if (f !== 'ALL' && !count) return '';
    const active = f === activeFilter ? ' active' : '';
    return `<button class="tab-btn${active}" data-ble-filter="${f}">${esc(f.replace('_', ' '))} (${count})</button>`;
  }).filter(Boolean).join('');
  const filterBar = `<div class="tabs" style="margin-bottom:10px">${chips}</div>`;
  if (!filtered.length) return filterBar + '<div class="empty">No devices match the current filter.</div>';
  return filterBar + `<div class="table-wrap"><table><thead><tr><th>#</th><th>MAC</th><th>Name</th><th>Type</th><th>RSSI</th></tr></thead><tbody>
    ${filtered.map((r, i) => `<tr class="clickable-row" data-select-item="ble" data-index="${escapeAttr(r.index || String(i))}">
      <td>${esc(r.index || String(i))}</td>
      <td><code>${esc(r.mac || '-')}</code></td>
      <td>${esc(r.name || '-')}</td>
      <td>${esc(r.type || '-')}</td>
      <td>${esc(r.rssi != null ? r.rssi + ' dBm' : '-')}</td>
    </tr>`).join('')}
  </tbody></table></div>`;
}

function renderPortTable(rows) {
  if (!rows.length) return '<div class="empty">No port scan results yet. Run a port scan to populate.</div>';
  return `<div class="table-wrap"><table><thead><tr><th>#</th><th>Host</th><th>Port</th><th>Protocol</th></tr></thead><tbody>
    ${rows.map(r => `<tr class="clickable-row" data-select-item="port" data-index="${escapeAttr(r.index)}">
      <td>${esc(r.index)}</td><td><code>${esc(r.host)}</code></td><td>${esc(r.port)}</td><td>${esc(r.proto)}</td>
    </tr>`).join('')}
  </tbody></table></div>`;
}

function renderGenericRows(rows, command) {
  if (!rows.length) return `<div class="empty">No structured output yet for <code>${esc(command)}</code>. Run the action to populate this page.</div>`;
  return `<div class="result-list">${rows.map(row =>
    `<div class="result-row clickable-row" data-select-item="generic" data-index="${escapeAttr(row.index)}">
      <div class="result-index">${esc(row.index)}</div>
      <div class="result-main"><strong>${esc(row.title)}</strong><small>${esc(row.detail)}</small></div>
    </div>`
  ).join('')}</div>`;
}

/* ======================== SETTINGS (Accordion) ======================== */
function buildSettingsForm() {
  const container = $('settings-container');
  const tabs = $('settings-tabs');
  if (!container || !tabs) return;
  const DEFAULT_OPEN = new Set([0, 1]);
  container.innerHTML = SETTINGS_SCHEMA.map((group, gi) => {
    const isOpen = state.settingsOpen.has(gi) || (state.settingsOpen.size === 0 && DEFAULT_OPEN.has(gi));
    const fieldsHtml = group.fields.map(field => renderField(field)).join('');
    return `<div class="settings-group ${isOpen ? 'open' : ''}" data-settings-group="${gi}" id="settings-group-${gi}">
      <div class="settings-group-header" role="button" tabindex="0" aria-expanded="${isOpen}" aria-controls="sg-body-${gi}">
        <div class="sg-icon">${esc(group.icon)}</div>
        <div class="sg-meta">
          <div class="sg-title">${esc(group.category)} <span class="sg-count">${group.fields.length}</span></div>
          <div class="sg-desc">${esc(group.description)}</div>
        </div>
        <div class="sg-summary" id="sg-summary-${gi}"></div>
        <div class="sg-chevron" aria-hidden="true">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"></polyline></svg>
        </div>
      </div>
      <div class="settings-group-body" id="sg-body-${gi}">
        <div class="form-grid form-grid--mixed">${fieldsHtml}</div>
      </div>
    </div>`;
  }).join('');
  tabs.innerHTML = SETTINGS_SCHEMA.map((group, gi) =>
    `<a class="settings-tab" href="#settings-group-${gi}" data-tab-group="${gi}"><span class="settings-tab-icon">${esc(group.icon)}</span><span>${esc(group.category)}</span></a>`
  ).join('');
  bindSettingsAccordion();
  bindSettingsTabs();
  updateSettingsSummaries();
}

function updateSettingsSummaries() {
  SETTINGS_SCHEMA.forEach((group, gi) => {
    const summary = $('sg-summary-' + gi);
    if (!summary) return;
    const bits = group.fields.slice(0, 2).map(f => {
      const v = readSettingDisplay(f);
      return v ? `${esc(f.label)}: <strong>${esc(v)}</strong>` : '';
    }).filter(Boolean);
    summary.innerHTML = bits.join(' &middot; ');
  });
}

function readSettingDisplay(field) {
  const el = $(field.id);
  if (!el) return '';
  if (field.type === 'bool') return el.checked ? 'On' : 'Off';
  if (field.type === 'color') return el.value || '';
  if (field.type === 'enum') {
    const opt = field.options.find(([v]) => v === el.value);
    return opt ? opt[1] : el.value;
  }
  const v = el.value;
  if (v == null || v === '') return '';
  return v.length > 24 ? v.slice(0, 24) + '…' : v;
}

function renderField(field) {
  let input = '';
  const wide = field.type === 'text' || field.type === 'textarea' || field.type === 'color';
  if (field.type === 'bool') {
    input = `<label class="switch" for="${field.id}"><input id="${field.id}" type="checkbox"><span class="switch-track" aria-hidden="true"><span class="switch-thumb"></span></span></label>`;
  } else if (field.type === 'textarea') {
    input = `<textarea id="${field.id}" rows="2" placeholder="${esc(field.hint || '')}"></textarea>`;
  } else if (field.type === 'enum') {
    input = `<select id="${field.id}">${field.options.map(([v, l]) => `<option value="${v}">${esc(l)}</option>`).join('')}</select>`;
  } else if (field.type === 'color') {
    input = `<input id="${field.id}" type="color" value="#ffffff">`;
  } else {
    const attrs = [`placeholder="${esc(field.hint || '')}"`];
    if (field.min !== undefined) attrs.push(`min="${field.min}"`);
    if (field.max !== undefined) attrs.push(`max="${field.max}"`);
    if (field.max) attrs.push(`maxlength="${field.max}"`);
    input = `<input id="${field.id}" type="${field.type === 'number' ? 'number' : 'text'}" ${attrs.join(' ')}>`;
  }
  return `<div class="field ${wide ? 'field--wide' : ''}" data-field-label="${esc(field.label.toLowerCase())}" data-field-id="${esc(field.id.toLowerCase())}">
    <label for="${field.id}">${esc(field.label)}</label>
    ${input}
  </div>`;
}

function bindSettingsAccordion() {
  document.querySelectorAll('.settings-group-header').forEach(header => {
    header.addEventListener('click', () => {
      const group = header.closest('.settings-group');
      const idx = parseInt(group.dataset.settingsGroup, 10);
      const isOpen = group.classList.toggle('open');
      header.setAttribute('aria-expanded', String(isOpen));
      if (isOpen) state.settingsOpen.add(idx); else state.settingsOpen.delete(idx);
    });
    header.addEventListener('keydown', e => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); header.click(); }
    });
  });
}

function filterSettings(query) {
  const q = query.trim().toLowerCase();
  document.querySelectorAll('.settings-group').forEach(group => {
    const fields = group.querySelectorAll('.field');
    let anyMatch = false;
    fields.forEach(field => {
      const label = field.dataset.fieldLabel;
      const id = field.dataset.fieldId;
      const match = !q || label.includes(q) || id.includes(q);
      field.style.display = match ? '' : 'none';
      if (match) anyMatch = true;
    });
    group.style.display = anyMatch ? '' : 'none';
    if (anyMatch && q) {
      group.classList.add('open', 'highlight');
      group.querySelector('.settings-group-header').setAttribute('aria-expanded', 'true');
    } else if (!q) {
      group.classList.remove('highlight');
    }
  });
}

function expandAllSettings(open) {
  document.querySelectorAll('.settings-group').forEach((group, idx) => {
    group.classList.toggle('open', open);
    group.querySelector('.settings-group-header').setAttribute('aria-expanded', String(open));
    if (open) state.settingsOpen.add(idx); else state.settingsOpen.delete(idx);
  });
}

function bindSettingsTabs() {
  document.querySelectorAll('.settings-tab').forEach(tab => {
    tab.addEventListener('click', e => {
      e.preventDefault();
      const idx = parseInt(tab.dataset.tabGroup, 10);
      const group = $('settings-group-' + idx);
      if (!group) return;
      document.querySelectorAll('.settings-tab').forEach(t => t.classList.toggle('active', t === tab));
      if (!group.classList.contains('open')) {
        group.classList.add('open');
        group.querySelector('.settings-group-header').setAttribute('aria-expanded', 'true');
        state.settingsOpen.add(idx);
      }
      const offset = group.getBoundingClientRect().top + window.scrollY - 80;
      window.scrollTo({ top: offset, behavior: 'smooth' });
    });
  });
}

/* ======================== API & DATA ======================== */
async function refreshAll() {
  const refreshBtn = document.querySelector('[data-refresh]');
  setLoading(refreshBtn, true);
  try {
    const [settingsResult, commResult, logsResult] = await Promise.allSettled([loadSettings(), loadCommStatus(), refreshLogs(false)]);
    loadFiles(false).catch(() => {});
    if ([settingsResult, commResult, logsResult].some(result => result.status === 'rejected')) {
      throw new Error('core API unavailable');
    }
    $('api-dot').className = 'dot good';
    $('api-status').textContent = 'Connected';
    renderDashboard();
  } catch (e) {
    $('api-dot').className = 'dot bad';
    $('api-status').textContent = 'Disconnected';
  } finally {
    setLoading(refreshBtn, false);
  }
}

const DASHBOARD_AUTO_QUERIES = [
  { cmd: 'chipinfo',   has: (logs) => Parsers.chipInfo(logs) != null },
  { cmd: 'wifistatus', has: (logs) => /===\s*WIFI\s*STATUS\s*===/.test(logs) },
];

function populateDashboardIfStale() {
  if (state.dashboardAutoPopulated) return;
  const missing = DASHBOARD_AUTO_QUERIES.filter(q => !q.has(state.logs || ''));
  if (!missing.length) {
    state.dashboardAutoPopulated = true;
    return;
  }
  state.dashboardAutoPopulated = true;
  missing.forEach(({ cmd }) => runCommand(cmd));
}

async function loadSettings() {
  const res = await api('/api/settings');
  if (!res.ok) throw new Error('settings failed');
  const data = await res.json();
  state.settings = data;
  for (const group of SETTINGS_SCHEMA) {
    for (const field of group.fields) {
      let value = data[field.id];
      if (value === undefined || value === null) {
        if (field.type === 'bool') value = false;
        else if (field.type === 'number') value = 0;
        else value = '';
      }
      const el = $(field.id);
      if (!el) continue;
      if (field.type === 'bool') el.checked = !!value;
      else if (field.type === 'color') {
        const hex = typeof value === 'string' ? value.replace(/^0x/i, '#') : (value ? '#' + value.toString(16).padStart(6, '0') : '#ffffff');
        el.value = hex;
      } else el.value = String(value);
    }
  }
  updateSettingsSummaries();
  document.querySelectorAll('#settings-container input, #settings-container select, #settings-container textarea').forEach(el => {
    el.addEventListener('input', updateSettingsSummaries);
    el.addEventListener('change', updateSettingsSummaries);
  });
}

function getVal(id) { const el = $(id); return el ? el.value : ''; }
function getBool(id) { const el = $(id); return el ? !!el.checked : false; }

function cachedParse(logs) {
  if (state.parseCache.logs === logs) {
    return {
      aps: state.parseCache.aps,
      stations: state.parseCache.stations,
      ble: state.parseCache.ble,
      flippers: state.parseCache.flippers,
      airtags: state.parseCache.airtags,
      gatt: state.parseCache.gatt,
      ports: state.parseCache.ports,
    };
  }
  const aps = parseAllAccessPoints(logs);
  const stations = parseAllStations(logs);
  const ble = parseAllBleDevices(logs);
  const flippers = parseAllFlippers(logs);
  const airtags = parseAllAirTags(logs);
  const gatt = parseAllGattDevices(logs);
  const ports = parsePortScan(logs);
  state.parseCache = { logs, aps, stations, ble, flippers, airtags, gatt, ports };
  return { aps, stations, ble, flippers, airtags, gatt, ports };
}

function invalidateParseCache() {
  state.parseCache = { logs: null, aps: null, stations: null, ble: null, flippers: null, airtags: null, gatt: null, ports: null };
}

async function loadCommStatus() {
  const res = await api('/api/esp_comm/status');
  if (!res.ok) throw new Error('comm failed');
  const data = await res.json();
  state.comm = data;
  const label = data.state ? data.state.charAt(0).toUpperCase() + data.state.slice(1) : 'Unknown';
  $('comm-status').textContent = 'GhostLink ' + label;
  const cs = $('comm-state');
  if (cs) cs.textContent = label;
  const cr = $('comm-remote-badge');
  if (cr) {
    cr.textContent = data.is_remote_command ? 'Remote: Yes' : 'Remote: No';
    cr.className = 'badge ' + (data.is_remote_command ? 'warn' : '');
  }
  $('comm-dot').className = 'dot ' + (data.connected ? 'good' : data.state === 'error' ? 'bad' : data.state === 'scanning' || data.state === 'handshake' ? 'warn' : '');
}

async function saveSettings() {
  const btn = $('save-settings-btn');
  setLoading(btn, true);
  try {
    const payload = {};
    for (const group of SETTINGS_SCHEMA) {
      for (const field of group.fields) {
        const el = $(field.id);
        if (!el) continue;
        if (field.type === 'bool') payload[field.id] = el.checked;
        else if (field.type === 'number') payload[field.id] = parseFloat(el.value) || 0;
        else payload[field.id] = el.value;
      }
    }
    const res = await api('/api/settings', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify(payload) });
    if (!res.ok) throw new Error('save failed');
    toast('Settings saved', 'good');
    await loadSettings();
  } catch (e) {
    toast(e.message, 'bad');
  } finally {
    setLoading(btn, false);
  }
}

async function runCommand(command, options = {}) {
  if (!options.skipRisk && isRisky(command)) {
    showRiskModal(command);
    return;
  }
  if (!options.silent) {
    state.commandHistory.push(command);
    state.lastCommand = command;
    state.historyIndex = state.commandHistory.length;
    localStorage.setItem('ghost_v2_pending_command', JSON.stringify({ command, at: Date.now() }));
    appendTerminal('> ' + command + '\n');
  }
  try {
    const res = await api('/api/command', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ command }) });
    if (!res.ok) throw new Error('command failed');
    if (!options.silent) {
      toast('Command sent', 'good');
      setTimeout(() => refreshLogs(true), 900);
    }
    if (isRisky(command) && !options.silent) {
      startReconnectWatch();
    } else if (!options.silent) {
      localStorage.removeItem('ghost_v2_pending_command');
    }
    if (typeof options.onComplete === 'function') options.onComplete();
  } catch (err) {
    if (isRisky(command) && !options.silent) startReconnectWatch();
    else if (!options.silent) toast('Command failed: ' + err.message, 'bad');
    if (typeof options.onComplete === 'function') options.onComplete(err);
  }
}

function showRiskModal(command) {
  state.pendingRiskCommand = command;
  $('risk-text').textContent = `"${command}" may stop the AP or use WiFi/BLE promiscuous/radio mode. If the page disconnects, V2 will try to reconnect and recover logs.`;
  $('risk-ghostlink-btn').disabled = !canUseGhostLink();
  $('risk-modal').classList.add('show');
}

function closeRiskModal() {
  $('risk-modal').classList.remove('show');
  state.pendingRiskCommand = null;
}

async function runRemote(command) {
  if (isRisky(command)) {
    const ok = confirm(`"${command}" is a radio-disruptive or risky command. Send it to the GhostLink peer anyway?`);
    if (!ok) return;
  }
  appendComm('> ' + command);
  setCommPending(true);
  try {
    const res = await api('/api/esp_comm/send', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ command }) });
    const data = await res.json();
    if (!data.success) {
      appendComm('Error: ' + (data.message || 'send failed'), { kind: 'error' });
      toast('GhostLink command failed', 'bad');
      setCommPending(false);
    } else {
      toast('GhostLink command sent', 'good');
      state.commLogOffset = (state.logs || '').length;
      setTimeout(() => refreshLogs(true).then(pollPeerResponses).catch(() => setCommPending(false)), 800);
    }
  } catch (err) {
    appendComm('Error: ' + err.message, { kind: 'error' });
    toast('GhostLink failed', 'bad');
    setCommPending(false);
  }
}

function setCommPending(on) {
  const el = $('comm-pending');
  if (el) el.hidden = !on;
  if (state.commPendingTimer) { clearTimeout(state.commPendingTimer); state.commPendingTimer = null; }
  if (on) {
    state.commPending = (state.commPending || 0) + 1;
    state.commPendingTimer = setTimeout(() => {
      if (state.commPending > 0) {
        appendComm('(no peer response within 12s)', { kind: 'error' });
      }
      setCommPending(false);
    }, 12000);
  } else {
    state.commPending = 0;
  }
}

function pollPeerResponses() {
  if (!state.logs) return;
  if (state.commLogOffset === 0 || state.commLogOffset > state.logs.length) {
    state.commLogOffset = state.logs.length;
    return;
  }
  const slice = state.logs.slice(state.commLogOffset);
  if (!slice) return;
  const lines = slice.split('\n');
  let appended = false;
  for (const raw of lines) {
    let body = null;
    let kind = 'response';
    let m = raw.match(/^RX:\s?(.*)$/);
    if (m) {
      body = m[1];
    } else {
      m = raw.match(/^ESP Comm Response:\s?(.*)$/);
      if (m) body = m[1];
    }
    if (body != null) {
      appendComm(body || '(empty response)', { kind });
      appended = true;
      continue;
    }
    if (/^E:\s*ESP Comm Response/i.test(raw) || /Response failed/i.test(raw) || /E:\s*esp_comm/i.test(raw)) {
      appendComm(raw.replace(/^E:\s*/, ''), { kind: 'error' });
      appended = true;
    }
  }
  state.commLogOffset = state.logs.length;
  if (appended) setCommPending(false);
}

function startReconnectWatch() {
  const overlay = $('reconnect-overlay');
  if (state.reconnectActive) {
    overlay.classList.add('show');
    return;
  }
  state.reconnectActive = true;
  overlay.classList.add('show');
  let attempts = 0;
  const maxAttempts = 60;
  const tick = async () => {
    attempts++;
    $('reconnect-progress').style.width = Math.min(100, attempts / maxAttempts * 100) + '%';
    try {
      const res = await api('/api/logs', { cache: 'no-store' });
      if (res.ok) {
        state.logs = await res.text();
        invalidateParseCache();
        overlay.classList.remove('show');
        state.reconnectActive = false;
        localStorage.removeItem('ghost_v2_pending_command');
        renderLogs();
        renderActiveFeatureView();
        await Promise.allSettled([loadSettings(), loadCommStatus(), loadFiles(false)]);
        state.dashboardAutoPopulated = false;
        populateDashboardIfStale();
        toast('Reconnected and recovered logs', 'good');
        return;
      }
    } catch (_) {}
    if (attempts < maxAttempts) setTimeout(tick, 2000);
    else {
      state.reconnectActive = false;
      toast('Reconnect timed out. Try refreshing after the AP returns.', 'warn');
    }
  };
  setTimeout(tick, 1500);
}

async function refreshLogs(parse = false) {
  const res = await api('/api/logs', { cache: 'no-store' });
  if (!res.ok) throw new Error('logs failed');
  const newLogs = await res.text();
  if (newLogs !== state.logs) {
    state.logs = newLogs;
    invalidateParseCache();
  }
  renderLogs();
  if (parse) {
    renderActiveFeatureView();
    renderDashboard();
  }
}

function appendTerminal(text) {
  const t = $('terminal');
  t.textContent += text;
  t.scrollTop = t.scrollHeight;
}
function renderLogs() {
  const t = $('terminal');
  t.textContent = state.logs || 'No logs yet.';
  t.scrollTop = t.scrollHeight;
}
function appendComm(text, opts) {
  const t = $('comm-terminal');
  const line = document.createElement('div');
  if (opts && opts.kind === 'response') line.className = 'peer-response';
  else if (opts && opts.kind === 'error') line.className = 'peer-error';
  line.textContent = text;
  t.appendChild(line);
  while (t.childElementCount > 500) t.removeChild(t.firstChild);
  t.scrollTop = t.scrollHeight;
}

function renderActiveFeatureView() {
  if (state.activeAction) renderActionDetail();
}

/* ======================== FILES ======================== */
async function loadFiles(showErrors = true) {
  const list = $('file-list');
  list.innerHTML = '<div class="skeleton skeleton-row"></div><div class="skeleton skeleton-row"></div><div class="skeleton skeleton-row"></div>';
  try {
    const res = await api('/api/sdcard?path=' + encodeURIComponent(state.currentPath));
    if (!res.ok) throw new Error('SD card unavailable');
    const data = await res.json();
    state.currentPath = data.path || state.currentPath;
    $('file-path').textContent = state.currentPath;
    if (data.storage) {
      $('stat-sd-used').textContent = `${formatBytes(data.storage.used)} / ${formatBytes(data.storage.total)}`;
      $('sd-progress').style.width = data.storage.total ? ((data.storage.used / data.storage.total) * 100) + '%' : '0';
    }
    const files = data.files || [];
    list.innerHTML = files.length ? files.map(fileRow).join('') : '<div class="empty">This folder is empty.</div>';
  } catch (err) {
    list.innerHTML = '<div class="empty">SD card not accessible.<br><small>Check that an SD card is inserted and the configured pins match your board. Run <code>sd status</code> in the Terminal to verify.</small></div>';
    if (showErrors) toast(err.message, 'bad');
  }
}

function fileRow(item) {
  const folder = item.type === 'folder';
  return `<div class="file-row">
    <div>
      <strong>${folder ? '[DIR] ' : ''}${esc(item.name)}</strong>
      <small>${esc(item.path)} ${item.size !== undefined ? ' - ' + formatBytes(item.size) : ''}</small>
    </div>
    <div class="actions">
      ${folder ? `<button class="btn" data-open-path="${escapeAttr(item.path)}">Open</button>` : `<button class="btn" data-download-path="${escapeAttr(item.path)}">Download</button><button class="btn danger" data-delete-path="${escapeAttr(item.path)}">Delete</button>`}
    </div>
  </div>`;
}

async function downloadFile(path) {
  const res = await api('/api/sdcard/download', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ path }) });
  if (!res.ok) throw new Error('download failed');
  const blob = await res.blob();
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = path.split('/').pop() || 'download.bin';
  a.click();
  URL.revokeObjectURL(url);
}

async function deleteFile(path) {
  if (!confirm('Delete ' + path + '?')) return;
  const res = await api('/api/sdcard?path=' + encodeURIComponent(path), { method: 'DELETE' });
  if (!res.ok) throw new Error('delete failed');
  toast('Deleted file', 'good');
  loadFiles();
}

async function uploadFile() {
  const input = $('upload-input');
  if (!input.files || !input.files[0]) { toast('Choose a file first', 'warn'); return; }
  const btn = $('upload-btn');
  setLoading(btn, true);
  try {
    const body = new FormData();
    body.append('file', input.files[0]);
    const res = await api('/api/sdcard/upload?path=' + encodeURIComponent(state.currentPath), { method: 'POST', body });
    if (!res.ok) throw new Error(await res.text());
    input.value = '';
    toast('Uploaded file', 'good');
    loadFiles();
  } catch (e) {
    toast(e.message, 'bad');
  } finally {
    setLoading(btn, false);
  }
}

/* ======================== EVENTS ======================== */
function bindEvents() {
  document.body.addEventListener('click', event => {
    const cmd = event.target.closest('[data-command]');
    const remote = event.target.closest('[data-remote-command]');
    const openActionBtn = event.target.closest('[data-open-action]');
    const closeActionBtn = event.target.closest('[data-close-action]');
    const backResultsBtn = event.target.closest('[data-back-results]');
    const openPath = event.target.closest('[data-open-path]');
    const downloadPath = event.target.closest('[data-download-path]');
    const deletePath = event.target.closest('[data-delete-path]');

    if (cmd) runCommand(cmd.dataset.command);
    if (remote) runRemote(remote.dataset.remoteCommand);
    if (openActionBtn) openAction(openActionBtn.dataset.openAction);
    if (closeActionBtn) closeAction();
    if (backResultsBtn) backToResults();
    if (openPath) { state.currentPath = openPath.dataset.openPath; loadFiles(); }
    if (downloadPath) downloadFile(downloadPath.dataset.downloadPath).catch(e => toast(e.message, 'bad'));
    if (deletePath) deleteFile(deletePath.dataset.deletePath).catch(e => toast(e.message, 'bad'));

    const itemRow = event.target.closest('[data-select-item]');
    if (itemRow && !event.target.closest('input[type="checkbox"]')) {
      const type = itemRow.dataset.selectItem;
      const index = itemRow.dataset.index;
      let data = null;
      const { aps, stations, ble, flippers, airtags, gatt, ports } = cachedParse(state.logs);
      if (type === 'ap') data = aps.find(r => r.index === index);
      else if (type === 'station') data = stations.find(r => r.index === index);
      else if (type === 'ble') data = ble.concat(flippers, airtags, gatt).find(r => (r.index || '0') === index);
      else if (type === 'port') data = ports.find(r => r.index === index);
      else if (type === 'generic') data = parseGenericRows(state.logs, state.activeAction ? state.activeAction.command : '').find(r => r.index === index);
      if (data) openItem(type, data);
      return;
    }

    const check = event.target.closest('[data-select-check]');
    if (check) {
      const idx = check.dataset.index;
      if (check.checked) state.selectedIndices.add(idx);
      else state.selectedIndices.delete(idx);
      renderActionDetail();
      return;
    }

    const selectAll = event.target.closest('[data-select-all]');
    if (selectAll) {
      const type = selectAll.dataset.selectAll;
      let rows = [];
      const cache = cachedParse(state.logs);
      if (type === 'ap') rows = cache.aps;
      else if (type === 'station') rows = cache.stations;
      if (selectAll.checked) rows.forEach(r => state.selectedIndices.add(r.index));
      else rows.forEach(r => state.selectedIndices.delete(r.index));
      renderActionDetail();
      return;
    }

    const batch = event.target.closest('[data-batch-action]');
    if (batch) {
      const action = batch.dataset.batchAction;
      if (action === 'clear') { state.selectedIndices.clear(); renderActionDetail(); }
      else if (action === 'deauth') {
        const indices = Array.from(state.selectedIndices);
        if (!indices.length) { toast('Nothing selected', 'warn'); return; }
        const selectCmd = CMD.selectAp(indices.join(',')).cmd;
        runCommandSequence([selectCmd, CMD.stopDeauth().cmd, 'stop', CMD.deauth().cmd]);
      }
      return;
    }

    const itemAction = event.target.closest('[data-item-action]');
    if (itemAction) {
      handleItemAction(itemAction.dataset.itemAction, itemAction.dataset);
      return;
    }

    const bleFilter = event.target.closest('[data-ble-filter]');
    if (bleFilter) {
      state.bleFilter = bleFilter.dataset.bleFilter;
      renderActionDetail();
      return;
    }
  });

  document.querySelectorAll('[data-refresh]').forEach(btn => btn.addEventListener('click', refreshAll));
  $('refresh-logs-btn').addEventListener('click', () => refreshLogs(true).catch(e => toast(e.message, 'bad')));
  $('clear-terminal-btn').addEventListener('click', () => { $('terminal').textContent = ''; });
  $('terminal-send-btn').addEventListener('click', sendTerminal);
  $('terminal-input').addEventListener('keydown', terminalKeydown);
  $('save-settings-btn').addEventListener('click', () => saveSettings().catch(e => toast(e.message, 'bad')));
  $('settings-reset-all-btn')?.addEventListener('click', () => {
    if (confirm('Reset ALL settings to firmware defaults? This will restart the AP if AP creds are reset.')) {
      runCommand(CMD.settingsReset().cmd);
    }
  });
  $('settings-backup-export-btn')?.addEventListener('click', () => runCommand(CMD.settingsBackupExport().cmd));
  $('settings-backup-import-btn')?.addEventListener('click', () => {
    if (confirm('Import settings from SD card backup? This will overwrite current settings.')) {
      runCommand(CMD.settingsBackupImport().cmd);
    }
  });
  $('reboot-btn')?.addEventListener('click', () => {
    if (confirm('Reboot the device now? The WebUI will be unavailable until the device restarts.')) {
      runCommand(CMD.reboot().cmd);
    }
  });
  $('set-rgb-pins-btn')?.addEventListener('click', () => {
    const data = getVal('rgb_data_pin');
    const r = getVal('rgb_red_pin');
    const g = getVal('rgb_green_pin');
    const b = getVal('rgb_blue_pin');
    runCommand(`setrgbpins ${data} ${r} ${g} ${b}`);
  });
  $('connect-wifi-btn').addEventListener('click', () => {
    const ssid = getVal('sta_ssid');
    const pass = getVal('sta_password');
    const cmd = ssid ? CMD.connect(ssid, pass).cmd : CMD.connect().cmd;
    if (ssid) runCommand(cmd);
    else runCommand(cmd);
  });
  $('start-portal-btn').addEventListener('click', () => {
    const cmd = CMD.startPortal(getVal('portal_url') || 'default', getVal('portal_ap_ssid') || 'FreeWiFi', getVal('portal_password')).cmd;
    runCommand(cmd);
  });
  $('send-printer-btn').addEventListener('click', () => {
    const cmd = CMD.powerprinter(getVal('printer_ip'), getVal('printer_text'), getVal('printer_font_size'), 'CM').cmd;
    runCommand(cmd);
  });
  $('refresh-files-btn').addEventListener('click', () => loadFiles());
  $('up-folder-btn').addEventListener('click', () => {
    if (state.currentPath !== '/mnt') {
      state.currentPath = state.currentPath.split('/').slice(0, -1).join('/') || '/mnt';
      loadFiles();
    }
  });
  $('upload-btn').addEventListener('click', () => uploadFile().catch(e => toast(e.message, 'bad')));
  $('comm-send-btn').addEventListener('click', () => {
    const cmd = $('comm-input').value.trim();
    if (cmd) {
      $('comm-input').value = '';
      state.commHistory.push(cmd);
      state.commHistoryIndex = state.commHistory.length;
      runRemote(cmd);
    }
  });
  $('comm-connect-btn')?.addEventListener('click', () => {
    const peer = prompt('Peer name (from commdiscovery list):', '');
    if (peer && peer.trim()) runCommand(CMD.commConnect(peer.trim()).cmd);
  });
  $('comm-input').addEventListener('keydown', e => {
    if (e.key === 'Enter') { $('comm-send-btn').click(); return; }
    if (e.key === 'ArrowUp') {
      e.preventDefault();
      if (state.commHistoryIndex > 0) state.commHistoryIndex--;
      $('comm-input').value = state.commHistory[state.commHistoryIndex] || '';
    }
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      if (state.commHistoryIndex < state.commHistory.length) state.commHistoryIndex++;
      $('comm-input').value = state.commHistory[state.commHistoryIndex] || '';
    }
  });
  $('comm-disconnect-btn').addEventListener('click', disconnectComm);
  $('risk-cancel-btn').addEventListener('click', closeRiskModal);
  $('risk-local-btn').addEventListener('click', () => { const cmd = state.pendingRiskCommand; closeRiskModal(); if (cmd) runCommand(cmd, { skipRisk: true }); });
  $('risk-ghostlink-btn').addEventListener('click', () => { const cmd = state.pendingRiskCommand; closeRiskModal(); if (cmd) runRemote(cmd); });
  $('dismiss-reconnect-btn').addEventListener('click', () => {
    $('reconnect-overlay').classList.remove('show');
    state.reconnectActive = false;
  });

  // Settings accordion controls
  const searchEl = $('settings-search');
  if (searchEl) {
    searchEl.addEventListener('input', e => filterSettings(e.target.value));
  }
  $('settings-expand-all')?.addEventListener('click', () => expandAllSettings(true));
  $('settings-collapse-all')?.addEventListener('click', () => expandAllSettings(false));
}

function handleItemAction(action, dataset) {
  switch (action) {
    case 'select-ap': runCommand(CMD.selectAp(dataset.index).cmd); break;
    case 'select-sta': runCommand(CMD.selectSta(dataset.index).cmd); break;
    case 'deauth': runCommandSequence([CMD.selectAp(dataset.index).cmd, CMD.deauth().cmd]); break;
    case 'deauth-sta': runCommandSequence([CMD.selectSta(dataset.index).cmd, CMD.deauth().cmd]); break;
    case 'track-ap': runCommand(CMD.trackAp().cmd); break;
    case 'track-sta': runCommand(CMD.trackSta().cmd); break;
    case 'connect-ap': {
      const pass = getVal(`ap-connect-pass-${dataset.index}`) || '';
      const ssid = dataset.ssid;
      if (ssid) runCommand(CMD.connect(ssid, pass).cmd);
      else toast('No SSID available', 'warn');
      break;
    }
    case 'track-ble': runCommand(CMD.bleScan('spam').cmd); break;
    case 'spoof-airtag': runCommand(CMD.spoofAirTag(true).cmd); break;
    case 'enum-gatt': runCommand(CMD.enumGatt().cmd); break;
    case 'copy': navigator.clipboard.writeText(dataset.text || '').then(() => toast('Copied', 'good')).catch(() => toast('Copy failed', 'bad')); break;
    default: toast('Action not implemented', 'warn');
  }
}

function sendTerminal() {
  const input = $('terminal-input');
  const command = input.value.trim();
  if (!command) return;
  input.value = '';
  runCommand(command);
}

function terminalKeydown(event) {
  if (event.key === 'Enter') { sendTerminal(); return; }
  if (event.key === 'ArrowUp') {
    event.preventDefault();
    if (state.historyIndex > 0) state.historyIndex--;
    $('terminal-input').value = state.commandHistory[state.historyIndex] || '';
  }
  if (event.key === 'ArrowDown') {
    event.preventDefault();
    if (state.historyIndex < state.commandHistory.length) state.historyIndex++;
    $('terminal-input').value = state.commandHistory[state.historyIndex] || '';
  }
}

async function disconnectComm() {
  const res = await api('/api/esp_comm/control', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ action: 'disconnect' }) });
  const data = await res.json();
  appendComm(data.message || 'Disconnected');
  await loadCommStatus();
}

function recoverPendingCommand() {
  const raw = localStorage.getItem('ghost_v2_pending_command');
  if (!raw) return;
  try {
    const pending = JSON.parse(raw);
    state.lastCommand = pending.command || '';
    if (pending.at && Date.now() - pending.at < 5 * 60 * 1000) {
      toast('Recovered pending command state; parsing logs', 'warn');
    } else {
      localStorage.removeItem('ghost_v2_pending_command');
    }
    refreshLogs(true).catch(() => {});
  } catch (_) {}
}

/* ======================== INIT ======================== */
function init() {
  buildShell();
  bindEvents();
  recoverPendingCommand();
  refreshAll().catch(() => {
    $('api-dot').className = 'dot bad';
    $('api-status').textContent = 'Disconnected';
    startReconnectWatch();
  });
  state.terminalTimer = setInterval(() => {
    refreshLogs(false).then(pollPeerResponses).catch(() => {});
    if (document.querySelector('#page-dashboard.active')) renderDashboard();
    populateDashboardIfStale();
  }, 2000);
  setInterval(() => loadCommStatus().catch(() => {}), 1500);
}

init();
