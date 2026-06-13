import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const port = Number(process.env.PORT || 8080);

const state = {
  settings: {
    broadcast_speed: 100,
    gps_rx_pin: 16,
    station_ip: '192.168.1.87',
    display_timeout: 10000,
    hex_accent_color: '#ffffff',
    timezone_str: 'UTC0',
    portal_url: 'default',
    portal_ssid: 'GhostPortal',
    portal_password: '',
    portal_ap_ssid: 'FreeWiFi',
    portal_domain: '',
    portal_offline: false,
    printer_ip: '192.168.1.120',
    printer_text: 'GhostESP test print',
    printer_font_size: 24,
    printer_alignment: 0,
    rgb_speed: 25,
    rgb_mode: 2,
    rgb_data_pin: 8,
    rgb_red_pin: 4,
    rgb_green_pin: 5,
    rgb_blue_pin: 6,
    neopixel_bright: 80,
    ap_ssid: 'GhostESP',
    ap_password: 'ghostesp123',
    ap_enabled: true,
    sta_ssid: 'LabNet',
    sta_password: 'labnetpass',
    max_bright: 100,
    invert_colors: false,
    terminal_color: '#00ff00',
    menu_theme: 0,
    channel_delay: 0.25,
    power_save: false,
    zebra_menus: true,
    nav_buttons: true,
    menu_layout: 0,
    infrared_easy: false,
    web_auth: false,
    rts_enabled: false,
    third_ctrl: false,
    auto_save_scans: true,
    flappy_name: 'Ghost',
    timezone: 'UTC0',
    accent_color: '#ffffff',
    esp_comm_tx_pin: 6,
    esp_comm_rx_pin: 7,
    io_btn_p10_cmd: 'scanap',
    io_btn_p11_cmd: 'blescan -ds',
    io_btn_p12_cmd: 'wifistatus',
  },
  comm: {
    state: 'connected',
    connected: true,
    is_remote_command: false,
  },
  logs: 'GhostESP mock host ready.\nType "help" for commands.\n',
  flakyUntil: 0,
  files: new Map(),
  scanResults: {
    aps: [],
    stations: [],
    ble: [],
    flippers: [],
    airtags: [],
  },
};

seedFiles();
seedScanResults();

function seedFiles() {
  state.files.set('/mnt', [
    dir('ghostesp', '/mnt/ghostesp'),
    dir('captures', '/mnt/captures'),
    file('readme.txt', '/mnt/readme.txt', 128, 'Mock GhostESP SD card.\n'),
  ]);
  state.files.set('/mnt/ghostesp', [
    dir('evil_portal', '/mnt/ghostesp/evil_portal'),
    dir('nfc', '/mnt/ghostesp/nfc'),
    dir('portals', '/mnt/ghostesp/portals'),
    file('settings.json', '/mnt/ghostesp/settings.json', 512, '{"mock":true}\n'),
  ]);
  state.files.set('/mnt/ghostesp/evil_portal', [
    dir('portals', '/mnt/ghostesp/evil_portal/portals'),
    file('credentials.txt', '/mnt/ghostesp/evil_portal/credentials.txt', 92, 'user@example.com:hunter2\n'),
  ]);
  state.files.set('/mnt/ghostesp/portals', [
    file('google.html', '/mnt/ghostesp/portals/google.html', 2048, '<html>mock portal</html>\n'),
    file('default.html', '/mnt/ghostesp/portals/default.html', 1024, '<html>default portal</html>\n'),
  ]);
  state.files.set('/mnt/ghostesp/nfc', [
    file('sample.nfc', '/mnt/ghostesp/nfc/sample.nfc', 768, 'Filetype: Flipper NFC device\n'),
  ]);
  state.files.set('/mnt/captures', [
    file('scan_001.pcap', '/mnt/captures/scan_001.pcap', 262144, 'pcap mock\n'),
    file('wardrive.csv', '/mnt/captures/wardrive.csv', 4096, 'ssid,bssid,rssi\n'),
  ]);
}

function seedScanResults() {
  state.scanResults.aps = [
    { idx: 0, ssid: 'LabNet', bssid: 'A4:CF:12:34:56:78', rssi: -43, ch: 6, sec: 'WPA2', vendor: 'Espressif Inc.', band: '2.4GHz', pmf: 'Required' },
    { idx: 1, ssid: 'CoffeeShop', bssid: 'B8:27:EB:11:22:33', rssi: -67, ch: 11, sec: 'Open', vendor: 'Raspberry Pi Foundation', band: '2.4GHz', pmf: 'Disabled' },
    { idx: 2, ssid: '(Hidden)', bssid: '90:9A:4A:99:88:77', rssi: -72, ch: 1, sec: 'WPA2/WPA3', vendor: 'TP-Link Technologies', band: '2.4GHz', pmf: 'Required' },
    { idx: 3, ssid: 'GhostNet', bssid: '24:6F:28:AA:BB:CC', rssi: -35, ch: 6, sec: 'WPA2', vendor: 'Espressif Inc.', band: '2.4GHz', pmf: 'Optional' },
    { idx: 4, ssid: 'CorpWiFi-5G', bssid: 'AC:86:74:DD:EE:FF', rssi: -51, ch: 36, sec: 'WPA3-Enterprise', vendor: 'Cisco Systems', band: '5GHz', pmf: 'Required' },
  ];
  state.scanResults.stations = [
    { idx: 0, mac: '60:F8:1D:01:02:03', vendor: 'Apple, Inc.', apSsid: 'LabNet', apBssid: 'A4:CF:12:34:56:78', apVendor: 'Espressif Inc.', rssi: -55 },
    { idx: 1, mac: '3C:22:FB:04:05:06', vendor: 'Google, Inc.', apSsid: 'CoffeeShop', apBssid: 'B8:27:EB:11:22:33', apVendor: 'Raspberry Pi Foundation', rssi: -68 },
    { idx: 2, mac: 'F0:18:98:07:08:09', vendor: 'Samsung Electronics', apSsid: 'LabNet', apBssid: 'A4:CF:12:34:56:78', apVendor: 'Espressif Inc.', rssi: -49 },
  ];
  state.scanResults.ble = [
    { idx: 0, mac: '7C:2F:80:11:22:33', rssi: -62, name: 'AirTag', type: 'AIR_TAG' },
    { idx: 1, mac: 'C8:89:F3:AA:BB:CC', rssi: -71, name: 'Flipper Zero', type: 'FLIPPER_ZERO' },
    { idx: 2, mac: 'AC:23:3F:DD:EE:FF', rssi: -55, name: 'Unknown', type: 'GENERIC' },
  ];
  state.scanResults.flippers = [
    { idx: 0, name: 'LabFlipper', mac: 'C8:89:F3:AA:BB:CC', rssi: -71, flipperType: 'White' },
  ];
  state.scanResults.airtags = [
    { idx: 0, mac: '7C:2F:80:11:22:33', rssi: -62, total: 1, payload: '01 02 03 04 05 06 07 08' },
  ];
}

function dir(name, fullPath) {
  return { name, path: fullPath, type: 'folder' };
}

function file(name, fullPath, size, content = '') {
  return { name, path: fullPath, type: 'file', size, content };
}

function addLog(text) {
  state.logs += text.endsWith('\n') ? text : `${text}\n`;
  if (state.logs.length > 16384) state.logs = state.logs.slice(-16384);
}

function send(res, status, body, type = 'text/plain', extraHeaders = {}) {
  const data = Buffer.isBuffer(body) ? body : Buffer.from(String(body));
  res.writeHead(status, {
    'Content-Type': type,
    'Content-Length': data.length,
    'Cache-Control': 'no-store',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Access-Control-Allow-Methods': 'GET,POST,DELETE,OPTIONS',
    ...extraHeaders,
  });
  res.end(data);
}

function json(res, value, status = 200) {
  send(res, status, JSON.stringify(value), 'application/json');
}

async function readBody(req) {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  return Buffer.concat(chunks);
}

async function handle(req, res) {
  const url = new URL(req.url, `http://${req.headers.host}`);

  if (req.method === 'OPTIONS') return send(res, 204, '');

  if (Date.now() < state.flakyUntil && url.pathname.startsWith('/api/')) {
    req.socket.destroy();
    return;
  }

  try {
    if (url.pathname === '/' || url.pathname === '/ghost_site.html') {
      const htmlPath = path.join(__dirname, 'dist', 'ghost_site.html');
      if (!fs.existsSync(htmlPath)) {
        return send(res, 500, 'Run node build.mjs first.');
      }
      return send(res, 200, fs.readFileSync(htmlPath), 'text/html; charset=utf-8');
    }

    if (url.pathname.startsWith('/src/')) {
      return serveStatic(req, res, url.pathname.slice(5));
    }

    if (url.pathname === '/api/settings') return handleSettings(req, res, url);
    if (url.pathname === '/api/logs') return send(res, 200, state.logs);
    if (url.pathname === '/api/clear_logs') {
      state.logs = '';
      return json(res, { status: 'success', message: 'logs_cleared' });
    }
    if (url.pathname === '/api/command') return handleCommand(req, res);
    if (url.pathname === '/api/esp_comm/status') return json(res, state.comm);
    if (url.pathname === '/api/esp_comm/control') return handleCommControl(req, res);
    if (url.pathname === '/api/esp_comm/send') return handleCommSend(req, res);
    if (url.pathname === '/api/sdcard') return handleSdCard(req, res, url);
    if (url.pathname === '/api/sdcard/download') return handleDownload(req, res);
    if (url.pathname === '/api/sdcard/upload') return handleUpload(req, res, url);

    return send(res, 404, 'Not found');
  } catch (error) {
    console.error(error);
    return json(res, { error: error.message }, 500);
  }
}

function serveStatic(req, res, relativePath) {
  const safePath = path.normalize(relativePath).replace(/^\.\.(?:[\\/]|$)/, '');
  const sourceRoot = path.join(__dirname, 'src');
  const fullPath = path.join(sourceRoot, safePath || 'index.html');
  if (!fullPath.startsWith(sourceRoot) || !fs.existsSync(fullPath)) {
    return send(res, 404, 'Not found');
  }
  const ext = path.extname(fullPath);
  const type = ext === '.html' ? 'text/html; charset=utf-8' : ext === '.css' ? 'text/css; charset=utf-8' : ext === '.js' ? 'text/javascript; charset=utf-8' : 'application/octet-stream';
  return send(res, 200, fs.readFileSync(fullPath), type);
}

async function handleSettings(req, res, url) {
  if (req.method === 'GET') {
    if (url.searchParams.get('list') === 'all') {
      const lines = Object.entries(state.settings).map(([k, v]) => `${k}=${v}`);
      return send(res, 200, lines.join('\n'));
    }
    return json(res, state.settings);
  }
  if (req.method === 'POST') {
    const body = await readBody(req);
    Object.assign(state.settings, JSON.parse(body.toString() || '{}'));
    addLog('Settings updated from WebUI.');
    return json(res, { status: 'settings_updated' });
  }
  return send(res, 405, 'Method not allowed');
}

async function handleCommand(req, res) {
  if (req.method !== 'POST') return send(res, 405, 'Method not allowed');
  const body = JSON.parse((await readBody(req)).toString() || '{}');
  const command = String(body.command || '').trim();
  addLog(`> ${command}`);
  simulateCommand(command, false);
  return send(res, 200, 'Command executed');
}

async function handleCommControl(req, res) {
  const body = JSON.parse((await readBody(req)).toString() || '{}');
  if (body.action === 'disconnect') {
    state.comm.state = 'idle';
    state.comm.connected = false;
    addLog('GhostLink disconnected.');
    return json(res, { success: true, message: 'Disconnected' });
  }
  if (body.action === 'start_discovery') {
    state.comm.state = 'scanning';
    state.comm.connected = false;
    addLog('GhostLink discovery started.');
    setTimeout(() => {
      state.comm.state = 'connected';
      state.comm.connected = true;
      addLog('GhostLink peer connected: MOCK_PEER');
    }, 1500);
    return json(res, { success: true, message: 'Discovery started' });
  }
  return json(res, { success: false, message: `Unknown action: ${body.action}` }, 400);
}

async function handleCommSend(req, res) {
  const body = JSON.parse((await readBody(req)).toString() || '{}');
  const command = String(body.command || '').trim();
  if (!state.comm.connected) return json(res, { success: false, message: 'No GhostLink peer connected' });
  addLog(`TX: ${command}`);
  simulateCommand(command, true);
  return json(res, { success: true, message: 'Command sent successfully' });
}

function handleSdCard(req, res, url) {
  const requestedPath = url.searchParams.get('path') || '/mnt';
  if (req.method === 'GET') {
    const files = state.files.get(requestedPath);
    if (!files) return json(res, { error: 'Path not found.' }, 404);
    return json(res, {
      path: requestedPath,
      storage: { total: 32 * 1024 * 1024, used: 9 * 1024 * 1024, free: 23 * 1024 * 1024 },
      files: files.map(({ content, ...item }) => item),
    });
  }
  if (req.method === 'DELETE') {
    const parent = path.posix.dirname(requestedPath);
    const list = state.files.get(parent) || [];
    const idx = list.findIndex(item => item.path === requestedPath);
    if (idx === -1) return send(res, 404, 'File not found');
    list.splice(idx, 1);
    addLog(`Deleted file: ${requestedPath}`);
    return send(res, 200, 'File deleted successfully');
  }
  return send(res, 405, 'Method not allowed');
}

async function handleDownload(req, res) {
  const body = JSON.parse((await readBody(req)).toString() || '{}');
  const item = findFile(body.path);
  if (!item) return json(res, { error: 'File not found.' }, 404);
  return send(res, 200, item.content || `Mock content for ${item.name}\n`, 'application/octet-stream');
}

async function handleUpload(req, res, url) {
  const targetPath = url.searchParams.get('path') || '/mnt';
  const body = await readBody(req);
  const list = state.files.get(targetPath);
  if (!list) return json(res, { error: 'Upload path not found.' }, 404);
  const name = `uploaded_${Date.now()}.bin`;
  list.push(file(name, `${targetPath}/${name}`, body.length, body.toString('utf8')));
  addLog(`Uploaded file: ${targetPath}/${name}`);
  return send(res, 200, 'File uploaded successfully');
}

function findFile(filePath) {
  for (const list of state.files.values()) {
    const item = list.find(entry => entry.path === filePath && entry.type === 'file');
    if (item) return item;
  }
  return null;
}

/* ======================== REALISTIC FIRMWARE SIMULATION ======================== */

function simulateCommand(command, remote) {
  const prefix = remote ? 'RX: ' : '';
  const c = command.trim();

  // Identify
  if (/^identify\b/i.test(c)) {
    addLog(`${prefix}GHOSTESP_OK`);
    return;
  }

  // Help
  if (/^help\b/i.test(c)) {
    addLog(`${prefix}GhostESP CLI Help`);
    addLog(`${prefix}WiFi: scanap, scansta, list -a, list -s, select -a, select -s, connect, disconnect, wifistatus`);
    addLog(`${prefix}Attack: attack -d, attack -e, beaconspam, karma, saeflood`);
    addLog(`${prefix}BLE: blescan, blespam, blewardriving, listflippers, listairtags`);
    addLog(`${prefix}System: chipinfo, gpsinfo, settings, reboot, stop`);
    addLog(`${prefix}Files: sd list, sd read, sd write, sd rm`);
    return;
  }

  // WiFi Scan
  if (/^scanap\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}Scan Started`);
    addLog(`${prefix}Scan Complete`);
    addApRows(prefix);
    return;
  }
  if (/^scansta\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}Station Scan Started`);
    addLog(`${prefix}Station Scan Complete`);
    addStationRows(prefix);
    return;
  }
  if (/^scanall\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}Combined Scan Started`);
    addLog(`${prefix}AP Scan Complete`);
    addApRows(prefix);
    addLog(`${prefix}Station Scan Complete`);
    addStationRows(prefix);
    return;
  }
  if (/^stopscan\b/i.test(c)) {
    addLog(`${prefix}Scan Stopped`);
    return;
  }
  if (/^list\s+-a\b/i.test(c)) {
    addLog(`${prefix}Access Points (${state.scanResults.aps.length}):`);
    addApRows(prefix);
    return;
  }
  if (/^list\s+-s\b/i.test(c)) {
    addLog(`${prefix}Stations (${state.scanResults.stations.length}):`);
    addStationRows(prefix);
    return;
  }

  // Select
  if (/^select\s+-a\b/i.test(c)) {
    const m = c.match(/select\s+-a\s+(.+)/i);
    addLog(`${prefix}Selected AP(s): ${m ? m[1] : ''}`);
    return;
  }
  if (/^select\s+-s\b/i.test(c)) {
    const m = c.match(/select\s+-s\s+(.+)/i);
    addLog(`${prefix}Selected station(s): ${m ? m[1] : ''}`);
    return;
  }
  if (/^selectgatt\b/i.test(c)) {
    const m = c.match(/selectgatt\s+(.+)/i);
    addLog(`${prefix}Selected GATT device: ${m ? m[1] : ''}`);
    return;
  }
  if (/^selectflipper\b/i.test(c)) {
    const m = c.match(/selectflipper\s+(\d+)/i);
    addLog(`${prefix}Tracking Flipper ${m ? m[1] : '0'}: RSSI -71 dBm (distant)`);
    return;
  }

  // WiFi Connection
  if (/^wifistatus\b/i.test(c)) {
    addLog(`${prefix}=== WIFI STATUS ===`);
    addLog(`${prefix}connected=true`);
    addLog(`${prefix}has_saved_network=true`);
    addLog(`${prefix}connected_ssid=LabNet`);
    addLog(`${prefix}connected_rssi=-51`);
    addLog(`${prefix}connected_bssid=A4:CF:12:34:56:78`);
    addLog(`${prefix}connected_channel=6`);
    addLog(`${prefix}saved_ssid=LabNet`);
    addLog(`${prefix}=== END STATUS ===`);
    return;
  }
  if (/^connect\b/i.test(c)) {
    addLog(`${prefix}Attempting connection to saved network: LabNet`);
    addLog(`${prefix}WiFi Connected`);
    addLog(`${prefix}Got IP: 192.168.1.100`);
    return;
  }
  if (/^disconnect\b/i.test(c)) {
    addLog(`${prefix}WiFi Disconnected: reason (0)`);
    return;
  }

  // Attacks
  if (/^attack\s+-d\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}Starting deauth attack on selected APs...`);
    addLog(`${prefix}Use stopdeauth to halt.`);
    return;
  }
  if (/^attack\s+-e\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}Starting EAPOL logoff attack...`);
    return;
  }
  if (/^stopdeauth\b/i.test(c)) {
    addLog(`${prefix}Deauth attack stopped.`);
    return;
  }
  if (/^beaconspam\b/i.test(c)) {
    simulateDisconnect(c);
    const mode = c.match(/-rr\b/) ? 'Rickroll' : c.match(/-l\b/) ? 'List' : c.match(/-r\b/) ? 'Random' : 'Custom';
    addLog(`${prefix}Starting ${mode} beacon spam...`);
    addLog(`${prefix}Use stopspam to halt.`);
    return;
  }
  if (/^stopspam\b/i.test(c)) {
    addLog(`${prefix}Beacon spam stopped.`);
    return;
  }
  if (/^karma\s+start\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}Karma attack started.`);
    addLog(`${prefix}Listening for probe requests...`);
    return;
  }
  if (/^karma\s+stop\b/i.test(c)) {
    addLog(`${prefix}Karma attack stopped.`);
    return;
  }
  if (/^saeflood\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}SAE flood started.`);
    return;
  }
  if (/^stopsaeflood\b/i.test(c)) {
    addLog(`${prefix}SAE flood stopped.`);
    return;
  }

  // Tracking
  if (/^trackap\b/i.test(c)) {
    addLog(`${prefix}=== tracking ap: LabNet ===`);
    addLog(`${prefix}bssid: A4:CF:12:34:56:78`);
    addLog(`${prefix}channel: 6`);
    addLog(`${prefix}##### -43 dBm (min:-45 max:-39)`);
    addLog(`${prefix}Use stop to halt tracking.`);
    return;
  }
  if (/^tracksta\b/i.test(c)) {
    addLog(`${prefix}=== tracking sta: 60:F8:1D:01:02:03 ===`);
    addLog(`${prefix}##### -55 dBm (min:-58 max:-52) ↑ CLOSER`);
    addLog(`${prefix}Use stop to halt tracking.`);
    return;
  }
  if (/^trackgatt\b/i.test(c)) {
    addLog(`${prefix}=== Tracking Device ===`);
    addLog(`${prefix}Name: Flipper Zero`);
    addLog(`${prefix}MAC: C8:89:F3:AA:BB:CC`);
    addLog(`${prefix}[##] RSSI: -71 dBm, Min: -75, Max: -68, CLOSER`);
    return;
  }

  // Capture
  if (/^capture\b/i.test(c)) {
    simulateDisconnect(c);
    const mode = c.match(/-deauth\b/) ? 'deauth' : c.match(/-beacon\b/) ? 'beacon' : c.match(/-raw\b/) ? 'raw' : c.match(/-wps\b/) ? 'wps' : c.match(/-eapol\b/) ? 'eapol' : 'probe';
    addLog(`${prefix}Starting ${mode} capture...`);
    addLog(`${prefix}Capture running. Use capture -stop to halt.`);
    return;
  }
  if (/^capture\s+-stop\b/i.test(c)) {
    addLog(`${prefix}Capture stopped.`);
    addLog(`${prefix}Saved capture to /mnt/captures/capture_${Date.now()}.pcap`);
    return;
  }

  // Environment
  if (/^sweep\b/i.test(c)) {
    if (c.match(/-s\b/)) {
      addLog(`${prefix}Sweep stopped.`);
      return;
    }
    simulateDisconnect(c);
    addLog(`${prefix}Sweep started.`);
    addLog(`${prefix}Capturing WiFi/BLE/GPS data...`);
    return;
  }
  if (/^pineap\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}PineAP detection started.`);
    addLog(`${prefix}Scanning for WiFi Pineapples...`);
    return;
  }
  if (/^congestion\b/i.test(c)) {
    addLog(`${prefix}Channel congestion:`);
    addLog(`${prefix}Channel 1: 4 networks`);
    addLog(`${prefix}Channel 6: 12 networks`);
    addLog(`${prefix}Channel 11: 7 networks`);
    return;
  }
  if (/^listenprobes\b/i.test(c)) {
    if (c.match(/-s\b/)) {
      addLog(`${prefix}Probe listener stopped.`);
      return;
    }
    simulateDisconnect(c);
    addLog(`${prefix}Listening for probe requests...`);
    return;
  }

  // Network
  if (/^scanports\b/i.test(c)) {
    addLog(`${prefix}Scanning common tcp ports on 192.168.1.1...`);
    addLog(`${prefix}Port 22: OPEN`);
    addLog(`${prefix}Port 80: OPEN`);
    addLog(`${prefix}Port 443: OPEN`);
    addLog(`${prefix}Scanning common udp ports on 192.168.1.1...`);
    addLog(`${prefix}UDP 53: OPEN`);
    return;
  }
  if (/^scanarp\b/i.test(c)) {
    addLog(`${prefix}Starting ARP scan on local network...`);
    addLog(`${prefix}[0] IP: 192.168.1.1, MAC: A4:CF:12:00:00:01`);
    addLog(`${prefix}[1] IP: 192.168.1.105, MAC: B8:27:EB:11:22:33`);
    addLog(`${prefix}[2] IP: 192.168.1.87, MAC: 60:F8:1D:AA:BB:CC`);
    return;
  }
  if (/^scanlocal\b/i.test(c)) {
    addLog(`${prefix}Local IP: 192.168.1.100`);
    addLog(`${prefix}Gateway: 192.168.1.1`);
    addLog(`${prefix}Netmask: 255.255.255.0`);
    return;
  }
  if (/^dhcpstarve\b/i.test(c)) {
    if (c.match(/-s\b/)) {
      addLog(`${prefix}DHCP starvation stopped.`);
      return;
    }
    simulateDisconnect(c);
    addLog(`${prefix}DHCP starvation started.`);
    return;
  }

  // Evil Portal
  if (/^startportal\b/i.test(c)) {
    simulateDisconnect(c);
    const m = c.match(/startportal\s+(\S+)\s+"([^"]+)"/);
    addLog(`${prefix}Starting Evil Portal with SSID ${m ? m[2] : 'FreeWiFi'}`);
    addLog(`${prefix}Portal running. Credentials will save to SD.`);
    return;
  }
  if (/^stopportal\b/i.test(c)) {
    addLog(`${prefix}Evil Portal stopped.`);
    return;
  }
  if (/^listportals\b/i.test(c)) {
    addLog(`${prefix}Available portals:`);
    addLog(`${prefix}  default.html`);
    addLog(`${prefix}  google.html`);
    return;
  }

  // BLE
  if (/^blescan\b/i.test(c)) {
    if (c.match(/-s\b/)) {
      addLog(`${prefix}BLE scan stopped.`);
      return;
    }
    simulateDisconnect(c);
    addLog(`${prefix}Starting BLE scan.`);
    if (c.match(/-f\b/)) {
      addFlipperRows(prefix);
    } else if (c.match(/-a\b/)) {
      addAirTagRows(prefix);
    } else if (c.match(/-g\b/)) {
      addGattRows(prefix);
    } else {
      addBleRows(prefix);
    }
    return;
  }
  if (/^blespam\b/i.test(c)) {
    if (c.match(/-s\b/)) {
      addLog(`${prefix}BLE spam stopped.`);
      return;
    }
    simulateDisconnect(c);
    const brand = c.match(/-apple\b/) ? 'Apple' : c.match(/-ms\b/) ? 'Microsoft' : c.match(/-samsung\b/) ? 'Samsung' : c.match(/-google\b/) ? 'Google' : 'Random';
    addLog(`${prefix}Starting BLE spam: ${brand}`);
    return;
  }
  if (/^listflippers\b/i.test(c)) {
    addLog(`${prefix}Flipper devices:`);
    addFlipperRows(prefix);
    return;
  }
  if (/^listairtags\b/i.test(c)) {
    addLog(`${prefix}AirTags:`);
    addAirTagRows(prefix);
    return;
  }
  if (/^listgatt\b/i.test(c)) {
    addLog(`${prefix}GATT devices:`);
    addGattRows(prefix);
    return;
  }
  if (/^enumgatt\b/i.test(c)) {
    addLog(`${prefix}Enumerating GATT services...`);
    addLog(`${prefix}Service: Generic Access (0x1800) handles 1-5`);
    addLog(`${prefix}Service: Device Information (0x180A) handles 6-12`);
    addLog(`${prefix}Service: Battery (0x180F) handles 13-16`);
    return;
  }
  if (/^spoofairtag\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}Started spoofing AirTag.`);
    return;
  }
  if (/^stopspoof\b/i.test(c)) {
    addLog(`${prefix}AirTag spoofing stopped.`);
    return;
  }
  if (/^blewardriving\b/i.test(c)) {
    if (c.match(/-s\b/)) {
      addLog(`${prefix}BLE wardriving stopped.`);
      return;
    }
    simulateDisconnect(c);
    addLog(`${prefix}BLE wardriving started.`);
    addLog(`${prefix}GPS: Locked`);
    addLog(`${prefix}BLE: 0`);
    addLog(`${prefix}Sats: 8/12`);
    return;
  }

  // NFC
  if (/^chameleon\s+scan\b/i.test(c)) {
    if (c.match(/scan\s+stop\b/i)) {
      addLog(`${prefix}NFC scan stopped.`);
      return;
    }
    simulateDisconnect(c);
    addLog(`${prefix}NFC Tag Found: NTAG213`);
    addLog(`${prefix}UID: 04A1B2C3D4E5F6`);
    return;
  }

  // IR
  if (/^ir\s+list\b/i.test(c)) {
    addLog(`${prefix}[0] Samsung.ir`);
    addLog(`${prefix}[1] LG.ir`);
    addLog(`${prefix}[2] Sony.json`);
    return;
  }
  if (/^ir\s+send\b/i.test(c)) {
    addLog(`${prefix}IR: send OK`);
    return;
  }
  if (/^ir\s+learn\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}IR learn task started`);
    addLog(`${prefix}Waiting for IR signal`);
    setTimeout(() => {
      addLog(`${prefix}Captured: NEC A:0x12345678 C:0x000000FF`);
      addLog(`${prefix}Saved to /mnt/ghostesp/ir/learned.ir`);
    }, 2000);
    return;
  }
  if (/^ir\s+dazzler\b/i.test(c)) {
    if (c.match(/stop\b/i)) {
      addLog(`${prefix}IR_DAZZLER:STOPPED`);
      return;
    }
    simulateDisconnect(c);
    addLog(`${prefix}IR_DAZZLER:STARTED`);
    return;
  }
  if (/^ir\s+show\b/i.test(c)) {
    addLog(`${prefix}Signals in Samsung.ir:`);
    addLog(`${prefix}[0] Power (NEC)`);
    addLog(`${prefix}[1] Volume_Up`);
    addLog(`${prefix}[2] Volume_Down (NEC)`);
    return;
  }

  // BadUSB
  if (/^badusb\s+list\b/i.test(c)) {
    addLog(`${prefix}[0] hello.txt`);
    addLog(`${prefix}[1] rickroll.txt`);
    return;
  }
  if (/^badusb\s+run\b/i.test(c)) {
    simulateDisconnect(c);
    const m = c.match(/badusb\s+run\s+(\S+)/);
    addLog(`${prefix}Running BadUSB script: ${m ? m[1] : ''}`);
    return;
  }
  if (/^badusb\s+stop\b/i.test(c)) {
    addLog(`${prefix}BadUSB stopped.`);
    return;
  }

  // GPS / Wardriving
  if (/^gpsinfo\b/i.test(c)) {
    if (c.match(/-s\b/)) {
      addLog(`${prefix}GPS tracking stopped.`);
      return;
    }
    addLog(`${prefix}GPS Info`);
    addLog(`${prefix}Fix: 3D`);
    addLog(`${prefix}Sats: 8/12 in view`);
    addLog(`${prefix}Lat: 31deg 54.7830'N`);
    addLog(`${prefix}Long: 115deg 51.6300'E`);
    addLog(`${prefix}Alt: 15.1m`);
    addLog(`${prefix}Speed: 0.0 km/h`);
    addLog(`${prefix}Direction: 276° WNW`);
    addLog(`${prefix}HDOP: 1.0`);
    return;
  }
  if (/^startwd\b/i.test(c)) {
    if (c.match(/-s\b/)) {
      addLog(`${prefix}Wardriving stopped.`);
      return;
    }
    simulateDisconnect(c);
    addLog(`${prefix}Wardrive started.`);
    addLog(`${prefix}Wardrive: ap=0 logged=0/0 gpsrej=0 ch=1 up=0m0s gps=No Fix/0 pending=0B`);
    return;
  }

  // Aerial
  if (/^aerialscan\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}[0] DJI-Mini-3`);
    addLog(`${prefix}    MAC: AC:86:74:11:22:33`);
    addLog(`${prefix}    Type: DRONE`);
    addLog(`${prefix}    RSSI: -62 dBm`);
    return;
  }
  if (/^aeriallist\b/i.test(c)) {
    addLog(`${prefix}[0] DJI-Mini-3, MAC: AC:86:74:11:22:33, RSSI: -62`);
    return;
  }
  if (/^aerialspoof\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}Aerial spoof started.`);
    return;
  }
  if (/^aerialspoofstop\b/i.test(c)) {
    addLog(`${prefix}Aerial spoof stopped.`);
    return;
  }

  // Ethernet
  if (/^ethinfo\b/i.test(c)) {
    addLog(`${prefix}Ethernet: UP`);
    addLog(`${prefix}IP: 192.168.1.50`);
    addLog(`${prefix}MAC: 00:11:22:33:44:55`);
    return;
  }
  if (/^etharp\b/i.test(c)) {
    addLog(`${prefix}[0] 192.168.1.1, MAC: A4:CF:12:00:00:01`);
    return;
  }

  // Settings
  if (/^settings\s+list\b/i.test(c)) {
    for (const [k, v] of Object.entries(state.settings)) {
      addLog(`${prefix}${k}=${v}`);
    }
    return;
  }
  if (/^settings\s+get\b/i.test(c)) {
    const m = c.match(/settings\s+get\s+(\S+)/);
    const key = m ? m[1] : '';
    addLog(`${prefix}${key}=${state.settings[key] ?? 'not_set'}`);
    return;
  }
  if (/^settings\s+set\b/i.test(c)) {
    const m = c.match(/settings\s+set\s+(\S+)\s+(.*)/);
    if (m) {
      state.settings[m[1]] = m[2];
      addLog(`${prefix}Set ${m[1]}=${m[2]}`);
    }
    return;
  }

  // SD Card
  if (/^sd\s+status\b/i.test(c)) {
    addLog(`${prefix}SD Card mounted (SPI). Total: 32 MB, Used: 9 MB, Free: 23 MB`);
    return;
  }
  if (/^sd\s+list\b/i.test(c)) {
    const m = c.match(/sd\s+list\s+(\S+)/);
    const p = m ? m[1] : '/mnt';
    const files = state.files.get(p);
    if (!files) {
      addLog(`${prefix}SD:ERR:path_not_found:${p}`);
      return;
    }
    files.forEach((f, i) => {
      if (f.type === 'folder') addLog(`${prefix}SD:DIR:[${i}] ${f.name}`);
      else addLog(`${prefix}SD:FILE:[${i}] ${f.name} ${f.size}`);
    });
    addLog(`${prefix}SD:OK:listed ${files.length} entries`);
    return;
  }
  if (/^sd\s+size\b/i.test(c)) {
    const m = c.match(/sd\s+size\s+(\S+)/);
    const item = findFile(m ? m[1] : '');
    addLog(`${prefix}SD:SIZE:${item ? item.size : 0}`);
    return;
  }
  if (/^sd\s+read\b/i.test(c)) {
    addLog(`${prefix}SD:READ:BEGIN:mock.bin`);
    addLog(`${prefix}SD:READ:SIZE:1024`);
    addLog(`${prefix}SD:READ:END:bytes=1024`);
    return;
  }
  if (/^sd\s+rm\b/i.test(c)) {
    addLog(`${prefix}SD:OK:removed:${c.match(/sd\s+rm\s+(\S+)/)?.[1] ?? ''}`);
    return;
  }
  if (/^sd\s+mkdir\b/i.test(c)) {
    addLog(`${prefix}SD:OK:created:${c.match(/sd\s+mkdir\s+(\S+)/)?.[1] ?? ''}`);
    return;
  }

  // GhostLink
  if (/^commstatus\b/i.test(c)) {
    addLog(`${prefix}Communication Status: connected`);
    addLog(`${prefix}Peer: MOCK_PEER`);
    addLog(`${prefix}RSSI: -42`);
    return;
  }
  if (/^commdiscovery\b/i.test(c)) {
    state.comm.state = 'scanning';
    state.comm.connected = false;
    addLog(`${prefix}GhostLink discovery started.`);
    return;
  }
  if (/^commconnect\b/i.test(c)) {
    state.comm.state = 'connected';
    state.comm.connected = true;
    addLog(`${prefix}GhostLink connected.`);
    return;
  }
  if (/^commdisconnect\b/i.test(c)) {
    state.comm.state = 'idle';
    state.comm.connected = false;
    addLog(`${prefix}GhostLink disconnected.`);
    return;
  }

  // Chip Info
  if (/^chipinfo\b/i.test(c)) {
    addLog(`${prefix}[CHIPINFO_START]`);
    addLog(`${prefix}Chip Information:`);
    addLog(`${prefix}  Firmware: GhostESP Revival 3.0.0`);
    addLog(`${prefix}  Git Commit: abc1234`);
    addLog(`${prefix}  Model: ESP32-S3`);
    addLog(`${prefix}  Revision: v1.2`);
    addLog(`${prefix}  CPU Cores: 2`);
    addLog(`${prefix}  Features: WiFi/BT/BLE/802.15.4/Embedded Flash/Embedded PSRAM`);
    addLog(`${prefix}  Free Heap: 156432 bytes`);
    addLog(`${prefix}  Min Free Heap: 89012 bytes`);
    addLog(`${prefix}  IDF Version: v5.2.1`);
    addLog(`${prefix}  Build Config: release`);
    addLog(`${prefix}  Enabled Features:, Display, Touchscreen, NFC, BadUSB, Infrared TX, Infrared RX, GPS, Ethernet, SD Card (SPI)`);
    addLog(`${prefix}[CHIPINFO_END]`);
    return;
  }

  // Stop / Reboot / Generic
  if (/^stop\b/i.test(c)) {
    addLog(`${prefix}All activities stopped.`);
    return;
  }
  if (/^reboot\b/i.test(c)) {
    simulateDisconnect(c);
    addLog(`${prefix}Rebooting...`);
    return;
  }

  addLog(`${prefix}Unknown command: ${command}`);
  addLog(`${prefix}Type "help" for available commands.`);
}

function simulateDisconnect(command) {
  const safe = /^(list\s+-a|list\s+-s|wifistatus|chipinfo|gpsinfo|commstatus|scanports|scanarp|congestion|settings\s|sd\s|help|identify|ir\s|badusb\s|eth)/i;
  if (safe.test(command)) return;
  state.flakyUntil = Date.now() + 3200;
}

function addApRows(prefix = '') {
  for (const ap of state.scanResults.aps) {
    addLog(`${prefix}[${ap.idx}] SSID: ${ap.ssid},`);
    addLog(`${prefix}     BSSID: ${ap.bssid},`);
    addLog(`${prefix}     RSSI: ${ap.rssi},`);
    addLog(`${prefix}     Channel: ${ap.ch},`);
    addLog(`${prefix}     Band: ${ap.band},`);
    addLog(`${prefix}     Security: ${ap.sec}`);
    addLog(`${prefix}     PMF: ${ap.pmf}`);
    addLog(`${prefix}     Vendor: ${ap.vendor}`);
  }
}

function addStationRows(prefix = '') {
  for (const s of state.scanResults.stations) {
    addLog(`${prefix}[${s.idx}] Station MAC: ${s.mac},`);
    addLog(`${prefix}     Station Vendor: ${s.vendor},`);
    addLog(`${prefix}     Associated AP: ${s.apSsid},`);
    addLog(`${prefix}     AP BSSID: ${s.apBssid},`);
    addLog(`${prefix}     AP Vendor: ${s.apVendor}`);
  }
}

function addBleRows(prefix = '') {
  for (const d of state.scanResults.ble) {
    addLog(`${prefix}BLE: ${d.name} | RSSI: ${d.rssi}`);
    if (d.mac) addLog(`${prefix}MAC: ${d.mac}`);
  }
}

function addFlipperRows(prefix = '') {
  for (const f of state.scanResults.flippers) {
    addLog(`${prefix}[${f.idx}] ${f.flipperType} Flipper Found:`);
    addLog(`${prefix}     MAC: ${f.mac},`);
    addLog(`${prefix}     Name: ${f.name},`);
    addLog(`${prefix}     RSSI: ${f.rssi} dBm`);
  }
}

function addAirTagRows(prefix = '') {
  for (const a of state.scanResults.airtags) {
    addLog(`${prefix}[${a.idx}] AirTag Found (Total: ${a.total})`);
    addLog(`${prefix}     MAC: ${a.mac},`);
    addLog(`${prefix}     RSSI: ${a.rssi} dBm (near),`);
    addLog(`${prefix}     Payload: ${a.payload}`);
  }
}

function addGattRows(prefix = '') {
  addLog(`${prefix}[0] Name: Heart Rate Monitor,`);
  addLog(`${prefix}     MAC: AC:23:3F:DD:EE:FF,`);
  addLog(`${prefix}     RSSI: -55,`);
  addLog(`${prefix}     Type: HRM`);
}

const server = http.createServer((req, res) => {
  handle(req, res).catch(error => {
    console.error(error);
    json(res, { error: error.message }, 500);
  });
});

server.listen(port, () => {
  console.log(`GhostESP WebUI V2 mock host running at http://localhost:${port}`);
  console.log('Serving bundled UI at / and source UI at /src/index.html');
});
