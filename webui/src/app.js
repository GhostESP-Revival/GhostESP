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
  isLoading: false,
  settingsOpen: new Set(),
  settingsFilter: '',
};

const pages = [
  ['dashboard', 'Dashboard', '00'],
  ['wifi', 'WiFi', '01'],
  ['ble', 'BLE', '02'],
  ['files', 'Files', '03'],
  ['ghostlink', 'GhostLink', '04'],
  ['settings', 'Settings', '05'],
  ['terminal', 'Terminal', '06'],
  ['help', 'Help', '07']
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
  buildHelp();
  buildSettingsForm();
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
  $('ble-actions').innerHTML = BLE_ACTIONS.map(action => actionRow(action, 'ble')).join('');
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
  const aps = parseAllAccessPoints(state.logs);
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
      <h3>${esc(label)} ${risky ? '<span class="badge">May disconnect</span>' : ''}</h3>
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
  if (action.section === 'wifi') {
    $('wifi-subpages').hidden = true;
    document.querySelector('[data-tabs="wifi"]').hidden = true;
    $('wifi-detail').hidden = false;
  } else if (action.section === 'ble') {
    $('ble-actions').hidden = true;
    $('ble-detail').hidden = false;
  }
  renderActionDetail();
}

function closeAction() {
  const section = state.activeAction ? state.activeAction.section : null;
  state.activeAction = null;
  state.activeItem = null;
  state.selectedIndices.clear();
  if (section === 'wifi') {
    document.querySelector('[data-tabs="wifi"]').hidden = false;
    $('wifi-subpages').hidden = false;
    $('wifi-detail').hidden = true;
    $('wifi-detail').innerHTML = '';
  } else if (section === 'ble') {
    $('ble-actions').hidden = false;
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
  const categoryBadge = `<span class="badge">${esc(commandCategory(action.command))}</span>`;
  const safetyBadge = action.risky ? '<span class="badge">May disconnect</span>' : '<span class="badge">Local-safe</span>';
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
  if (/^(scanap|list -a|scanall)\b/.test(cmd)) return renderApTable(parseAllAccessPoints(state.logs));
  if (/^(scansta|list -s)\b/.test(cmd)) return renderStationTable(parseAllStations(state.logs));
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
    const ble = parseAllBleDevices(state.logs);
    const flippers = parseAllFlippers(state.logs);
    const airtags = parseAllAirTags(state.logs);
    const gatt = parseAllGattDevices(state.logs);
    const all = [...ble, ...flippers, ...airtags, ...gatt];
    return renderBleTable(all);
  }
  if (/^scanports\b/.test(cmd)) return renderPortTable(parsePortScan(state.logs));
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
      <button class="btn danger" data-item-action="deauth">Deauth</button>
      <button class="btn" data-item-action="track-ap">Track AP</button>
      <button class="btn" data-item-action="connect-ap" data-ssid="${escapeAttr(data.ssid)}">Connect</button>
    </div>
    ${!isOpen ? `<div style="margin-top:10px"><input id="ap-connect-pass" placeholder="Password (optional)" style="max-width:260px"></div>` : ''}
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
      <button class="btn danger" data-item-action="deauth">Deauth</button>
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
  return `<div class="batch-toolbar"><span>${count} ${label}${count > 1 ? 's' : ''} selected</span><div class="actions">
    <button class="btn danger" data-batch-action="deauth">Deauth Selected</button>
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
  return `<div class="table-wrap"><table><thead><tr><th>#</th><th>MAC</th><th>Name</th><th>Type</th><th>RSSI</th></tr></thead><tbody>
    ${rows.map((r, i) => `<tr class="clickable-row" data-select-item="ble" data-index="${escapeAttr(r.index || String(i))}">
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

function buildHelp() {
  const groups = [
    ['WiFi', 'scanap, scansta, list -a, select -a, attack, beaconspam, capture'],
    ['BLE', 'blescan, blespam, blewardriving, listairtags'],
    ['GhostLink', 'commstatus, commdiscovery, commconnect, commsend, commdisconnect'],
    ['Files', 'Use Files page for SD card browse/upload/download/delete'],
    ['Safety', 'Risky local radio commands may disconnect WebUI. GhostLink is preferred.'],
    ['Recovery', 'After local disruption, V2 polls APIs and reloads logs when reachable.'],
  ];
  $('help-grid').innerHTML = groups.map(([title, body]) =>
    `<div class="card" style="cursor:default;min-height:auto"><h3>${esc(title)}</h3><p>${esc(body)}</p></div>`
  ).join('');
}

/* ======================== SETTINGS (Accordion) ======================== */
function buildSettingsForm() {
  const container = $('settings-container');
  if (!container) return;
  container.innerHTML = SETTINGS_SCHEMA.map((group, gi) => {
    const isOpen = state.settingsOpen.has(gi);
    const fieldsHtml = group.fields.map(field => renderField(field)).join('');
    return `<div class="settings-group ${isOpen ? 'open' : ''}" data-settings-group="${gi}">
      <div class="settings-group-header" role="button" tabindex="0" aria-expanded="${isOpen}" aria-controls="sg-body-${gi}">
        <div class="sg-icon">${esc(group.icon)}</div>
        <div style="min-width:0">
          <div class="sg-title">${esc(group.category)}</div>
          <div class="sg-desc">${esc(group.description)}</div>
        </div>
        <div class="sg-chevron" aria-hidden="true">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"></polyline></svg>
        </div>
      </div>
      <div class="settings-group-body" id="sg-body-${gi}">
        <div class="form-grid">${fieldsHtml}</div>
      </div>
    </div>`;
  }).join('');
  bindSettingsAccordion();
}

function renderField(field) {
  let input = '';
  if (field.type === 'bool') {
    input = `<input id="${field.id}" type="checkbox">`;
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
  return `<div class="field" data-field-label="${esc(field.label.toLowerCase())}" data-field-id="${esc(field.id.toLowerCase())}">
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
    $('api-chip').style.borderColor = 'rgba(52,211,153,.25)';
    renderDashboard();
  } catch (e) {
    $('api-dot').className = 'dot bad';
    $('api-status').textContent = 'Disconnected';
    $('api-chip').style.borderColor = 'rgba(248,113,113,.25)';
  } finally {
    setLoading(refreshBtn, false);
  }
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
  setVal('station_ip', data.station_ip || 'Not connected');
}

function setVal(id, value) { const el = $(id); if (el) el.value = value; }
function getVal(id) { const el = $(id); return el ? el.value : ''; }
function getBool(id) { const el = $(id); return el ? !!el.checked : false; }

async function loadCommStatus() {
  const res = await api('/api/esp_comm/status');
  if (!res.ok) throw new Error('comm failed');
  const data = await res.json();
  state.comm = data;
  const label = data.state ? data.state.charAt(0).toUpperCase() + data.state.slice(1) : 'Unknown';
  $('comm-status').textContent = 'GhostLink ' + label;
  $('comm-state').textContent = label;
  $('comm-remote').textContent = data.is_remote_command ? 'Yes' : 'No';
  $('comm-dot').className = 'dot ' + (data.connected ? 'good' : data.state === 'error' ? 'bad' : data.state === 'scanning' || data.state === 'handshake' ? 'warn' : '');
  const chip = $('comm-chip');
  if (chip) chip.style.borderColor = data.connected ? 'rgba(52,211,153,.25)' : data.state === 'error' ? 'rgba(248,113,113,.25)' : '';
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
  state.commandHistory.push(command);
  state.lastCommand = command;
  state.historyIndex = state.commandHistory.length;
  localStorage.setItem('ghost_v2_pending_command', JSON.stringify({ command, at: Date.now() }));
  appendTerminal('> ' + command + '\n');
  try {
    const res = await api('/api/command', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ command }) });
    if (!res.ok) throw new Error('command failed');
    toast('Command sent', 'good');
    setTimeout(() => refreshLogs(true), 900);
    if (isRisky(command)) startReconnectWatch();
  } catch (err) {
    if (isRisky(command)) startReconnectWatch();
    else toast('Command failed: ' + err.message, 'bad');
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
  appendComm('> ' + command);
  try {
    const res = await api('/api/esp_comm/send', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ command }) });
    const data = await res.json();
    appendComm(data.message || (data.success ? 'Command sent' : 'Failed'));
    toast(data.success ? 'GhostLink command sent' : 'GhostLink command failed', data.success ? 'good' : 'bad');
    if (data.success) setTimeout(() => refreshLogs(true), 1200);
  } catch (err) {
    appendComm('Error: ' + err.message);
    toast('GhostLink failed', 'bad');
  }
}

function startReconnectWatch() {
  const overlay = $('reconnect-overlay');
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
        overlay.classList.remove('show');
        localStorage.removeItem('ghost_v2_pending_command');
        renderLogs();
        renderActiveFeatureView();
        await Promise.allSettled([loadSettings(), loadCommStatus(), loadFiles(false)]);
        toast('Reconnected and recovered logs', 'good');
        return;
      }
    } catch (_) {}
    if (attempts < maxAttempts) setTimeout(tick, 2000);
    else toast('Reconnect timed out. Try refreshing after the AP returns.', 'warn');
  };
  setTimeout(tick, 1500);
}

async function refreshLogs(parse = false) {
  const res = await api('/api/logs', { cache: 'no-store' });
  if (!res.ok) throw new Error('logs failed');
  state.logs = await res.text();
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
function appendComm(text) {
  const t = $('comm-terminal');
  t.textContent += '\n' + text;
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
    list.innerHTML = '<div class="empty">Unable to load files.</div>';
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
      if (type === 'ap') data = parseAllAccessPoints(state.logs).find(r => r.index === index);
      else if (type === 'station') data = parseAllStations(state.logs).find(r => r.index === index);
      else if (type === 'ble') data = parseAllBleDevices(state.logs).concat(parseAllFlippers(state.logs), parseAllAirTags(state.logs), parseAllGattDevices(state.logs)).find(r => (r.index || '0') === index);
      else if (type === 'port') data = parsePortScan(state.logs).find(r => r.index === index);
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
      if (type === 'ap') rows = parseAllAccessPoints(state.logs);
      else if (type === 'station') rows = parseAllStations(state.logs);
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
        const indices = Array.from(state.selectedIndices).join(',');
        if (!indices) { toast('Nothing selected', 'warn'); return; }
        runCommand(CMD.deauth().cmd);
      }
      return;
    }

    const itemAction = event.target.closest('[data-item-action]');
    if (itemAction) {
      handleItemAction(itemAction.dataset.itemAction, itemAction.dataset);
      return;
    }
  });

  document.querySelectorAll('[data-refresh]').forEach(btn => btn.addEventListener('click', refreshAll));
  $('refresh-logs-btn').addEventListener('click', () => refreshLogs(true).catch(e => toast(e.message, 'bad')));
  $('clear-terminal-btn').addEventListener('click', () => { $('terminal').textContent = ''; });
  $('terminal-send-btn').addEventListener('click', sendTerminal);
  $('terminal-input').addEventListener('keydown', terminalKeydown);
  $('save-settings-btn').addEventListener('click', () => saveSettings().catch(e => toast(e.message, 'bad')));
  $('connect-wifi-btn').addEventListener('click', () => {
    const ssid = getVal('sta_ssid');
    const pass = getVal('sta_password');
    runCommand(ssid ? CMD.connect(ssid, pass).cmd : CMD.connect().cmd);
  });
  $('start-portal-btn').addEventListener('click', () => runCommand(CMD.startPortal(getVal('portal_url') || 'default', getVal('portal_ap_ssid') || 'FreeWiFi', getVal('portal_password')).cmd));
  $('send-printer-btn').addEventListener('click', () => runCommand(CMD.powerprinter(getVal('printer_ip'), getVal('printer_text'), getVal('printer_font_size'), 'CM').cmd));
  $('refresh-files-btn').addEventListener('click', () => loadFiles());
  $('up-folder-btn').addEventListener('click', () => {
    if (state.currentPath !== '/mnt') {
      state.currentPath = state.currentPath.split('/').slice(0, -1).join('/') || '/mnt';
      loadFiles();
    }
  });
  $('upload-btn').addEventListener('click', () => uploadFile().catch(e => toast(e.message, 'bad')));
  $('comm-send-btn').addEventListener('click', () => { const cmd = $('comm-input').value.trim(); if (cmd) { $('comm-input').value = ''; runRemote(cmd); } });
  $('comm-input').addEventListener('keydown', e => { if (e.key === 'Enter') $('comm-send-btn').click(); });
  $('comm-disconnect-btn').addEventListener('click', disconnectComm);
  $('risk-cancel-btn').addEventListener('click', closeRiskModal);
  $('risk-local-btn').addEventListener('click', () => { const cmd = state.pendingRiskCommand; closeRiskModal(); if (cmd) runCommand(cmd, { skipRisk: true }); });
  $('risk-ghostlink-btn').addEventListener('click', () => { const cmd = state.pendingRiskCommand; closeRiskModal(); if (cmd) runRemote(cmd); });
  $('dismiss-reconnect-btn').addEventListener('click', () => $('reconnect-overlay').classList.remove('show'));

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
    case 'deauth': runCommand(CMD.deauth().cmd); break;
    case 'track-ap': runCommand(CMD.trackAp().cmd); break;
    case 'track-sta': runCommand(CMD.trackSta().cmd); break;
    case 'connect-ap': {
      const pass = getVal('ap-connect-pass') || '';
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
    toast('Recovered pending command state; parsing logs', 'warn');
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
    refreshLogs(false).catch(() => {});
    if (document.querySelector('#page-dashboard.active')) renderDashboard();
  }, 2000);
  setInterval(() => loadCommStatus().catch(() => {}), 1500);
}

init();
