/**
 * GhostESP Response Parsers
 * Ported from Android GhostResponse.kt
 *
 * Pre-compiled regex patterns and robust parse functions for all
 * firmware output formats. Returns structured data or null.
 */

// Keep transport decoration out of every firmware-format parser.
function normalizeOutput(text) {
  return String(text || '').replace(/^(?:RX: ?|ESP Comm Response: ?)/gm, '');
}

function sourceOutput(text, target = 'local') {
  return String(text || '').split(/\r?\n/).flatMap(line => {
    const peer = /^(?:RX: ?|ESP Comm Response: ?)/.test(line);
    if (line.startsWith('> [peer] ')) return target === 'peer' ? ['> ' + line.slice(9)] : [];
    return peer === (target === 'peer') ? [normalizeOutput(line)] : [];
  }).join('\n');
}

function latestCommandOutput(text, pattern) {
  const lines = normalizeOutput(text).split(/\r?\n/);
  let start = 0;
  for (let i = 0; i < lines.length; i++) {
    const command = /^> (.+)$/.exec(lines[i]);
    if (command && pattern.test(command[1])) start = i + 1;
  }
  return lines.slice(start).join('\n');
}

function latestBlock(text, header, footer) {
  const lines = normalizeOutput(text).split(/\r?\n/);
  let start = -1, end = lines.length;
  for (let i = 0; i < lines.length; i++) if (header.test(lines[i])) start = i;
  if (start < 0) return normalizeOutput(text);
  if (footer) for (let i = start + 1; i < lines.length; i++) {
    if (footer.test(lines[i])) { end = i; break; }
  }
  return lines.slice(start, end).join('\n');
}

function uniqueRows(rows, key = 'index') {
  const latest = new Map();
  for (const row of rows) latest.set(row[key] ?? row.mac ?? row.index, row);
  return [...latest.values()];
}

const Patterns = {
  // AP scan (multiline)
  AP_INDEX:     /^\[(\d+)\]\s*SSID:/m,
  AP_SSID:      /SSID:[ \t]*(.*?)(?:,\s*BSSID:|,?[ \t]*$)/m,
  AP_BSSID:     /BSSID:\s*([0-9A-Fa-f:]{17})/,
  AP_RSSI:      /RSSI:\s*(-?\d+)/,
  AP_CHANNEL:   /Channel:\s*(\d+)/,
  AP_SECURITY:  /Security:[ \t]*([^\n,]+)/,
  AP_PMF:       /PMF:[ \t]*([^\n,]+)/,
  AP_VENDOR:    /Vendor:\s*(.+?)(?:\n|$)/,
  AP_BAND:      /Band:\s*(\S+)/,

  // Station
  STATION_INDEX: /^\[(\d+)\]\s*Station\s*MAC:/m,
  STATION_MAC:   /Station(?:\s*MAC)?:\s*([0-9A-Fa-f:]{17})/,
  STATION_VENDOR:/(?:Station|STA)\s*Vendor:\s*([^,\n]+)/,
  STATION_AP_SSID:/(?:Associated\s*AP|AP\s*SSID):[ \t]*([^\n]+)/,
  STATION_AP_BSSID:/AP\s*BSSID:\s*([0-9A-Fa-f:]{17})/,
  STATION_AP_VENDOR:/AP\s*Vendor:\s*([^,\n]+)/,
  STATION_RSSI:  /RSSI:\s*(-?\d+)/,

  // BLE
  BLE_NAME:      /BLE:\s*(.+?)\s*\|/,
  BLE_RSSI:      /RSSI:\s*(-?\d+)/,
  BLE_MAC:       /([0-9A-Fa-f:]{17})/,

  // Flipper
  FLIPPER_INDEX: /^\[(\d+)\]\s*(White|Black|Transparent)?\s*Flipper\s*Found/im,
  FLIPPER_MAC:   /MAC:\s*([0-9A-Fa-f:]{17})/,
  FLIPPER_NAME:  /Name:\s*([^,\n]+)/,
  FLIPPER_RSSI:  /RSSI:\s*(-?\d+)\s*dBm/,
  FLIPPER_TYPE:  /(White|Black|Transparent)\s*Flipper/i,

  // AirTag
  AIRTAG_INDEX:  /^\[(\d+)\]\s*AirTag\s*Found/m,
  AIRTAG_TOTAL:  /Total:\s*(\d+)/,
  AIRTAG_MAC:    /MAC:\s*([0-9A-Fa-f:]{17})/,
  AIRTAG_RSSI:   /RSSI:\s*(-?\d+)\s*dBm/,
  AIRTAG_PAYLOAD:/Payload:\s*([0-9A-Fa-f ]+)/,

  // GATT
  GATT_INDEX:    /^\[(\d+)\]\s*Name:/m,
  GATT_NAME:     /Name:\s*([^,\n]*)/,
  GATT_MAC:      /MAC:\s*([0-9A-Fa-f:]{17})/,
  GATT_RSSI:     /RSSI:\s*(-?\d+)/,
  GATT_TYPE:     /Type:\s*([^,\n]+)/,

  // SD
  SD_FILE:       /SD:FILE:\[(\d+)\]\s+(.+?)\s+(\d+)$/,
  SD_DIR:        /SD:DIR:\[(\d+)\]\s+(.+)$/,
  SD_OK:         /^SD:OK(:.*)?$/,
  SD_ERR:        /^SD:ERR:([^:]+)(?::(.*))?$/,
  SD_SIZE:       /SD:SIZE:(\d+)/,

  // GPS
  GPS_FIX:       /Fix:[ \t]*([^\n]+)/,
  GPS_SATS:      /Sats:\s*(\d+)(?:\/(\d+))?/,
  GPS_LAT:       /Lat:\s*(\d+)deg\s+([\d.]+)'([NS])/,
  GPS_LON:       /Long:\s*(\d+)deg\s+([\d.]+)'([EW])/,
  GPS_ALT:       /Alt:\s*(-?[\d.]+)m/,
  GPS_SPEED:     /Speed:\s*([\d.]+)\s*km\/h/,
  GPS_DIRECTION: /Direction:\s*(\d+)°\s*(\S+)/,
  GPS_HDOP:      /HDOP:\s*([\d.]+)/,

  // Tracking
  TRACK_RSSI:    /[#]+\s+(-?\d+)\s*dBm\s*\(min:(-?\d+)\s+max:(-?\d+)(?:\s+close:\d+%)?\)/,
  TRACK_RSSI_GATT:/\[[#]+\]\s*RSSI:\s*(-?\d+)\s*dBm,\s*Min:\s*(-?\d+),\s*Max:\s*(-?\d+)(?:,\s*Close:\s*\d+%)?(?:,\s*(CLOSER|FARTHER))?/,
  TRACK_HEADER:  /===\s*tracking\s+(ap|sta):\s*(.+)\s*===/i,
  TRACK_BSSID:   /bssid:\s*([0-9A-Fa-f:]{17})/i,
  TRACK_CHANNEL: /channel:\s*(\d+)/i,
  TRACK_FLIPPER: /Tracking Flipper (\d+):\s*RSSI\s*(-?\d+)\s*dBm(?:\s*\(([^)]+)\))?/i,

  // Handshake
  HANDSHAKE_AP:  /AP=([0-9A-Fa-f:]{17})/i,
  HANDSHAKE_PAIR:/Pair=(\S+)/i,

  // WiFi connection
  GOT_IP:        /Got IP:\s*(\d+\.\d+\.\d+\.\d+)/,
  WIFI_CONNECTED:/WiFi\s+Connected/i,
  WIFI_DISCONN:  /WiFi\s+[Dd]isconnected(?::\s*(.+))?/,

  // WiFi status block
  WIFI_STATUS_HDR: /===\s*WIFI\s*STATUS\s*===/,
  WIFI_STATUS_FTR: /===\s*END\s*STATUS\s*===/,
  WIFI_STATUS_KV:  /^(\w+)=(.*)$/,

  // Chipinfo
  CHIP_MODEL:      /Model:\s*([^,\s\n]+(?:\s+[^,\s\n]+)*?)(?=\s*(?:[,\n]|$))/,
  CHIP_REVISION:   /Revision:\s*v?(\d+(?:\.\d+)+)/,
  CHIP_CORES:      /CPU Cores:\s*(\d+)/,
  CHIP_FEATURES:   /(?<!Enabled )Features:[ \t]*([^\n]+)/,
  CHIP_FREE_HEAP:  /Free Heap:\s*(\d+)/,
  CHIP_MIN_HEAP:   /Min Free Heap:\s*(\d+)/,
  CHIP_IDF:        /IDF Version:\s*([^,\s\n]+)/,
  CHIP_BUILD_CFG:  /Build Config:\s*([^,\n]+)/,

  // IR
  IR_LEARNED:      /Captured:\s*(\S+)\s+A:0x([0-9A-Fa-f]+)\s+C:0x([0-9A-Fa-f]+)/,
  IR_LEARNED_RAW:  /Captured RAW signal\s*\((\d+)\s+samples\)/,
  IR_REMOTE:       /\[(\d+)\]\s*(.+?\.(?:ir|json))\s*$/,
  IR_BUTTON:       /\[(\d+)\]\s*(.+?)(?:\s*\(([^)]+)\)(?:\s+Addr:\s*0x[0-9a-f]+\s+Cmd:\s*0x[0-9a-f]+)?)?\s*$/i,

  // Portal
  PORTAL_CREDS:    /Captured credentials:\s*(.+)\s*\/\s*(.+)/,

  // Generic
  ERROR:           /ERROR:\s*(.+)/i,
  SUCCESS:         /^OK:\s*(.+)$/m,
  GHOSTESP_OK:     /GHOSTESP_OK/,
  SETTING_KV:      /([\w_]+)\s*=\s*(.+)/,

  // Wardrive
  WARDRIVE_HEART:  /Wardrive:\s*ap=(\d+)\s+logged=(\d+)\/(\d+)\s+gpsrej=(\d+).*?\s+ch=(\d+)\s+up=(\d+)m(\d+)s\s+gps=([^/]+)\/(\d+)(?:\s+sats=(\d+))?.*?\s+pending=(\d+)B/,

  // Port scan
  PORT_SCAN:       /Port\s+(\d+):\s*(\w+)/,
  ARP_ENTRY:       /(\d+\.\d+\.\d+\.\d+)\s+([0-9A-Fa-f:]{17})/,
};

const Parsers = {
  accessPoint(text) {
    if (!text.includes('[') || !text.includes('SSID:')) return null;
    const idx = Patterns.AP_INDEX.exec(text)?.[1];
    if (idx == null) return null;
    const ssid = (Patterns.AP_SSID.exec(text)?.[1] || '').trim();
    return {
      index: idx,
      ssid: ssid || '(Hidden)',
      bssid: Patterns.AP_BSSID.exec(text)?.[1] || '??:??:??:??:??:??',
      rssi: parseInt(Patterns.AP_RSSI.exec(text)?.[1] || '-100', 10),
      channel: parseInt(Patterns.AP_CHANNEL.exec(text)?.[1] || '-1', 10),
      security: (Patterns.AP_SECURITY.exec(text)?.[1] || 'Unknown').trim(),
      vendor: (Patterns.AP_VENDOR.exec(text)?.[1] || '').trim() || null,
      band: (Patterns.AP_BAND.exec(text)?.[1] || '').trim().replace(/,$/, '') || null,
      pmf: (Patterns.AP_PMF.exec(text)?.[1] || '').trim() || null,
      isHidden: !ssid || ssid === '(Hidden)',
    };
  },

  station(text) {
    if (!text.includes('Station MAC:') && !text.includes('Station:') && !text.includes('New Station:')) return null;
    const idx = Patterns.STATION_INDEX.exec(text)?.[1];
    const mac = Patterns.STATION_MAC.exec(text)?.[1];
    if (!mac) return null;
    return {
      index: idx,
      mac,
      vendor: (Patterns.STATION_VENDOR.exec(text)?.[1] || '').trim() || null,
      associatedApSsid: (Patterns.STATION_AP_SSID.exec(text)?.[1] || '').trim().replace(/,$/, '') || null,
      apBssid: Patterns.STATION_AP_BSSID.exec(text)?.[1] || null,
      apVendor: (Patterns.STATION_AP_VENDOR.exec(text)?.[1] || '').trim() || null,
      rssi: parseInt(Patterns.STATION_RSSI.exec(text)?.[1] || '-100', 10),
    };
  },
  _stationCounter: 0,
  resetStationCounter() { this._stationCounter = 0; },

  stationWithFallbackIndex(text) {
    const result = this.station(text);
    if (!result) return null;
    if (result.index == null) {
      result.index = String(this._stationCounter++);
    }
    return result;
  },

  bleDevice(line) {
    if (!line.startsWith('BLE:')) return null;
    const name = (Patterns.BLE_NAME.exec(line)?.[1] || '').trim() || null;
    const rssi = parseInt(Patterns.BLE_RSSI.exec(line)?.[1] || '-100', 10);
    const mac = Patterns.BLE_MAC.exec(line)?.[1] || null;
    const type = (() => {
      const n = name || '';
      if (/Flipper/i.test(n)) return 'FLIPPER_ZERO';
      if (/AirTag/i.test(n)) return 'AIR_TAG';
      if (/iPhone/i.test(n)) return 'IPHONE';
      if (/Samsung/i.test(n)) return 'SAMSUNG';
      if (/Google/i.test(n)) return 'GOOGLE';
      return 'GENERIC';
    })();
    return { name, mac, rssi, type };
  },

  flipper(text, listed = false) {
    const idx = Patterns.FLIPPER_INDEX.exec(text)?.[1] ?? (listed ? /^\[(\d+)\] MAC:/m.exec(text)?.[1] : null);
    if (idx == null) return null;
    return {
      index: idx,
      flipperType: (Patterns.FLIPPER_TYPE.exec(text)?.[1] || 'Unknown').trim(),
      mac: Patterns.FLIPPER_MAC.exec(text)?.[1] || null,
      name: (Patterns.FLIPPER_NAME.exec(text)?.[1] || '').trim() || null,
      rssi: parseInt(Patterns.FLIPPER_RSSI.exec(text)?.[1] || '-100', 10),
    };
  },

  airTag(text, listed = false) {
    const idx = Patterns.AIRTAG_INDEX.exec(text)?.[1] ?? (listed ? /^\[(\d+)\] MAC:/m.exec(text)?.[1] : null);
    if (idx == null) return null;
    return {
      index: idx,
      total: parseInt(Patterns.AIRTAG_TOTAL.exec(text)?.[1] || '1', 10),
      mac: Patterns.AIRTAG_MAC.exec(text)?.[1] || null,
      rssi: parseInt(Patterns.AIRTAG_RSSI.exec(text)?.[1] || '-100', 10),
      payload: (Patterns.AIRTAG_PAYLOAD.exec(text)?.[1] || '').trim() || null,
    };
  },

  gattDevice(text) {
    if (!text.includes('Name:') || !text.includes('MAC:') || text.includes('SSID:')) return null;
    const idx = Patterns.GATT_INDEX.exec(text)?.[1];
    if (idx == null) return null;
    return {
      index: idx,
      name: (Patterns.GATT_NAME.exec(text)?.[1] || '').trim() || null,
      mac: Patterns.GATT_MAC.exec(text)?.[1] || null,
      rssi: parseInt(Patterns.GATT_RSSI.exec(text)?.[1] || '-100', 10),
      type: (Patterns.GATT_TYPE.exec(text)?.[1] || '').trim() || null,
    };
  },

  sdEntry(line) {
    if (!line.startsWith('SD:')) return null;
    const file = Patterns.SD_FILE.exec(line);
    if (file) return { index: file[1], name: file[2].trim(), isDirectory: false, size: parseInt(file[3], 10) };
    const dir = Patterns.SD_DIR.exec(line);
    if (dir) return { index: dir[1], name: dir[2].trim(), isDirectory: true, size: null };
    return null;
  },

  wifiStatus(text) {
    text = latestBlock(text, Patterns.WIFI_STATUS_HDR, Patterns.WIFI_STATUS_FTR);
    if (!Patterns.WIFI_STATUS_HDR.test(text) && !text.includes('connected=')) return null;
    const values = {};
    for (const line of text.split(/\r?\n/)) {
      const m = Patterns.WIFI_STATUS_KV.exec(line.trim());
      if (m) values[m[1]] = m[2];
    }
    if (!values.connected) return null;
    return {
      connected: values.connected === 'true',
      hasSavedNetwork: values.has_saved_network === 'true',
      connectedSsid: values.connected_ssid || null,
      connectedRssi: values.connected_rssi != null ? parseInt(values.connected_rssi, 10) : null,
      connectedBssid: values.connected_bssid || null,
      connectedChannel: values.connected_channel != null ? parseInt(values.connected_channel, 10) : null,
      savedSsid: values.saved_ssid || null,
    };
  },

  wifiConnection(text) {
    for (const line of text.split(/\r?\n/).reverse()) {
      const ip = Patterns.GOT_IP.exec(line)?.[1];
      if (ip) return { isConnected: true, ip };
      const disconnected = Patterns.WIFI_DISCONN.exec(line);
      if (disconnected) return { isConnected: false, reason: disconnected[1] || null };
      if (Patterns.WIFI_CONNECTED.test(line)) return { isConnected: true };
    }
    return null;
  },

  chipInfo(text) {
    text = latestBlock(text, /\[CHIPINFO_START\]/, /\[CHIPINFO_END\]/);
    const hasInfo = text.includes('Chip Information') || (text.includes('Model:') && text.includes('IDF Version:') && text.includes('CPU Cores:'));
    if (!hasInfo) return null;
    const model = Patterns.CHIP_MODEL.exec(text)?.[1]?.trim();
    if (!model) return null;
    const features = [];
    const featureMap = {
      'Display': 'DISPLAY', 'Touchscreen': 'TOUCHSCREEN', 'Status Display (OLED)': 'STATUS_DISPLAY',
      'Status Display': 'STATUS_DISPLAY', 'NFC': 'NFC', 'BadUSB': 'BADUSB',
      'Infrared TX': 'INFRARED_TX', 'Infrared RX': 'INFRARED_RX', 'GPS': 'GPS',
      'Ethernet': 'ETHERNET', 'Battery (Power Save)': 'BATTERY', 'Battery ADC': 'BATTERY_ADC',
      'Fuel Gauge': 'FUEL_GAUGE', 'RTC Clock': 'RTC_CLOCK', 'Compass': 'COMPASS',
      'Accelerometer': 'ACCELEROMETER', 'Joystick': 'JOYSTICK', 'Cardputer': 'CARDPUTER',
      'T-Deck': 'TDECK', 'Rotary Encoder': 'ROTARY_ENCODER', 'USB Keyboard (Host)': 'USB_KEYBOARD',
      'Ghost Board': 'GHOST_BOARD', 'S3TWatch': 'S3TWATCH', 'SD Card (SPI)': 'SD_CARD_SPI',
      'SD Card (MMC)': 'SD_CARD_MMC',
      'NRF24': 'NRF24', 'SubGHz': 'SUBGHZ', 'AtomS3R': 'ATOMS3R', 'Core Dump': 'CORE_DUMP',
    };
    const enabledSection = text.indexOf('Enabled Features:');
    if (enabledSection !== -1) {
      const after = text.slice(enabledSection + 'Enabled Features:'.length);
      for (const seg of after.split(/,\s*|\n/)) {
        const t = seg.trim();
        if (featureMap[t]) features.push(featureMap[t]);
      }
    }
    return {
      model,
      revision: Patterns.CHIP_REVISION.exec(text)?.[1] || '0.0',
      cores: parseInt(Patterns.CHIP_CORES.exec(text)?.[1] || '1', 10),
      features: (Patterns.CHIP_FEATURES.exec(text)?.[1] || 'Unknown').trim(),
      freeHeap: parseInt(Patterns.CHIP_FREE_HEAP.exec(text)?.[1] || '0', 10),
      minFreeHeap: parseInt(Patterns.CHIP_MIN_HEAP.exec(text)?.[1] || '0', 10),
      idfVersion: (Patterns.CHIP_IDF.exec(text)?.[1] || 'Unknown').trim(),
      buildConfig: (Patterns.CHIP_BUILD_CFG.exec(text)?.[1] || '').trim() || null,
      enabledFeatures: features,
    };
  },

  gpsPosition(text) {
    text = latestBlock(text, /^GPS Info\s*$/);
    if (!text.includes('GPS Info') && !text.includes('Lat:') && !text.includes('Long:')) return null;
    const fixStr = Patterns.GPS_FIX.exec(text)?.[1]?.trim() || 'No Fix';
    const hasFix = /^(3D|2D|Fix)$/i.test(fixStr);
    const sats = Patterns.GPS_SATS.exec(text);
    const satsUsed = parseInt(sats?.[1] || '0', 10);
    const satsInView = parseInt(sats?.[2] || String(satsUsed), 10);
    const latM = Patterns.GPS_LAT.exec(text);
    const lonM = Patterns.GPS_LON.exec(text);
    let lat = 0, lon = 0;
    if (latM && lonM) {
      const d1 = parseFloat(latM[1]), m1 = parseFloat(latM[2]);
      lat = (d1 + m1 / 60) * (latM[3] === 'S' ? -1 : 1);
      const d2 = parseFloat(lonM[1]), m2 = parseFloat(lonM[2]);
      lon = (d2 + m2 / 60) * (lonM[3] === 'W' ? -1 : 1);
    }
    return {
      latitude: lat,
      longitude: lon,
      altitude: Patterns.GPS_ALT.exec(text)?.[1] ? parseFloat(Patterns.GPS_ALT.exec(text)[1]) : null,
      speed: Patterns.GPS_SPEED.exec(text)?.[1] ? parseFloat(Patterns.GPS_SPEED.exec(text)[1]) : null,
      satellites: satsUsed,
      satellitesInView: satsInView,
      fix: hasFix,
      fixType: fixStr,
      hdop: Patterns.GPS_HDOP.exec(text)?.[1] ? parseFloat(Patterns.GPS_HDOP.exec(text)[1]) : null,
      direction: Patterns.GPS_DIRECTION.exec(text)?.[1] ? parseInt(Patterns.GPS_DIRECTION.exec(text)[1], 10) : null,
      directionName: Patterns.GPS_DIRECTION.exec(text)?.[2] || null,
    };
  },

  trackData(line) {
    if (!line.includes('dBm')) return null;
    const w = Patterns.TRACK_RSSI.exec(line);
    if (w) {
      const rssi = parseInt(w[1], 10);
      return {
        rssi, minRssi: parseInt(w[2], 10), maxRssi: parseInt(w[3], 10),
        direction: line.includes('CLOSER') ? 'CLOSER' : line.includes('FARTHER') ? 'FARTHER' : 'STABLE',
      };
    }
    const g = Patterns.TRACK_RSSI_GATT.exec(line);
    if (g) {
      const rssi = parseInt(g[1], 10);
      return {
        rssi, minRssi: parseInt(g[2], 10), maxRssi: parseInt(g[3], 10),
        direction: g[4] || 'STABLE',
      };
    }
    return null;
  },

  trackHeader(text) {
    const h = Patterns.TRACK_HEADER.exec(text);
    if (/===\s*tracking sta\s*===/i.test(text)) {
      return { isAp: false, targetName: null, targetBssid: Patterns.TRACK_BSSID.exec(text)?.[1] || null, channel: null };
    }
    if (!h) {
      if (/Tracking\s+Device/i.test(text)) {
        const name = /Name:\s*([^,\n]+)/.exec(text)?.[1]?.trim();
        const mac = /MAC:\s*([0-9A-Fa-f:]{17})/.exec(text)?.[1];
        return { isAp: false, targetName: name || null, targetBssid: mac || null, channel: null };
      }
      return null;
    }
    return {
      isAp: h[1].toLowerCase() === 'ap',
      targetName: h[2].trim(),
      targetBssid: Patterns.TRACK_BSSID.exec(text)?.[1] || null,
      channel: Patterns.TRACK_CHANNEL.exec(text)?.[1] ? parseInt(Patterns.TRACK_CHANNEL.exec(text)[1], 10) : null,
    };
  },

  handshake(text) {
    if (!/Handshake found/i.test(text)) return null;
    return {
      apBssid: Patterns.HANDSHAKE_AP.exec(text)?.[1] || null,
      pairType: Patterns.HANDSHAKE_PAIR.exec(text)?.[1] || 'Unknown',
    };
  },

  flipperTrack(line) {
    const m = Patterns.TRACK_FLIPPER.exec(line);
    if (!m) return null;
    return { index: parseInt(m[1], 10), rssi: parseInt(m[2], 10), proximity: m[3]?.trim() || null };
  },

  irLearned(line) {
    const p = Patterns.IR_LEARNED.exec(line);
    if (p) return { protocol: p[1], address: p[2], command: p[3], rawSamples: null };
    const r = Patterns.IR_LEARNED_RAW.exec(line);
    if (r) return { protocol: 'RAW', address: null, command: null, rawSamples: parseInt(r[1], 10) };
    return null;
  },

  irRemote(line) {
    const m = Patterns.IR_REMOTE.exec(line.trim());
    if (!m) return null;
    return { index: parseInt(m[1], 10), filename: m[2] };
  },

  irButton(line) {
    const trimmed = line.trim();
    if (/^Signals in\s|^Unique buttons in\s|^IR:\s/.test(trimmed) || !trimmed.startsWith('[')) return null;
    const m = Patterns.IR_BUTTON.exec(trimmed);
    if (!m) return null;
    return { index: parseInt(m[1], 10), name: m[2], protocol: m[3] || null };
  },

  portalCreds(line) {
    if (!line.includes('Captured credentials:')) return null;
    const m = Patterns.PORTAL_CREDS.exec(line);
    if (!m) return null;
    return { username: m[1].trim(), password: m[2].trim() };
  },

  settingValue(line) {
    const m = Patterns.SETTING_KV.exec(line.trim());
    if (!m) return null;
    return { key: m[1], value: m[2].trim() };
  },

  error(line) {
    if (!/^ERROR:/i.test(line)) return null;
    return { message: Patterns.ERROR.exec(line)?.[1]?.trim() || 'Unknown error' };
  },

  success(line) {
    if (!line.startsWith('OK:')) return null;
    return { message: Patterns.SUCCESS.exec(line)?.[1]?.trim() || 'OK' };
  },

  wardriveStats(line) {
    const m = Patterns.WARDRIVE_HEART.exec(line);
    if (!m) return null;
    return {
      accessPoints: parseInt(m[1], 10),
      loggedOk: parseInt(m[2], 10),
      logAttempts: parseInt(m[3], 10),
      gpsRejected: parseInt(m[4], 10),
      channel: parseInt(m[5], 10),
      uptimeMinutes: parseInt(m[6], 10),
      uptimeSeconds: parseInt(m[7], 10),
      gpsFixStatus: m[8],
      gpsSatellites: parseInt(m[9], 10),
      pendingBytes: parseInt(m[11] || '0', 10),
    };
  },

  portScan(line) {
    const m = Patterns.PORT_SCAN.exec(line);
    if (!m) return null;
    return { port: parseInt(m[1], 10), state: m[2] };
  },

  arpEntry(line) {
    const m = Patterns.ARP_ENTRY.exec(line);
    if (!m) return null;
    return { ip: m[1], mac: m[2] };
  },
};

// Public parsers accept either raw local output or RX-decorated peer output.
for (const [name, parse] of Object.entries(Parsers)) {
  if (typeof parse !== 'function') continue;
  Parsers[name] = function (...args) {
    if (typeof args[0] === 'string') args[0] = normalizeOutput(args[0]);
    return parse.apply(this, args);
  };
}

function parseIndexedRows(text, header, parser, commands) {
  text = latestCommandOutput(text, commands);
  const rows = [];
  const blocks = text.split(new RegExp('\\n(?=' + header + ')', 'gi'));
  for (const block of blocks) {
    const row = parser(block);
    if (row) rows.push(row);
  }
  return uniqueRows(rows);
}

function parseAllAccessPoints(text) {
  return parseIndexedRows(text, '\\[\\d+\\]\\s+SSID:',
    block => Parsers.accessPoint(block), /^(?:scanap|scanall|list -a)\b/i);
}

function parseAllStations(text) {
  Parsers.resetStationCounter();
  // Unindexed discovery messages are useful in logs, but are not selectable IDs.
  return parseIndexedRows(text, '\\[\\d+\\]\\s+(?:Station MAC:|STA:)',
    block => { const row = Parsers.station(block); return row?.index != null ? row : null; },
    /^(?:scansta|scanall|list -s)\b/i);
}

function parseAllBleDevices(text) {
  text = latestCommandOutput(text, /^(?:blescan(?:\s+(?!-s\b).+)?|bledetect(?:\s+-l)?)\s*$/i);
  const rows = [];
  for (const line of text.split(/\r?\n/)) {
    const d = Parsers.bleDevice(line);
    if (d?.mac) rows.push({ ...d, index: String(rows.length), kind: 'advertisement' });
  }
  // Current -ds firmware emits indexed multi-line detector records.
  const blocks = text.split(/\n(?=\[\d+\]\s+[^\n]+ found)/gi);
  for (const block of blocks) {
    const head = /^\[(\d+)\] (.+) found\s*$/im.exec(block);
    if (!head) continue;
    const mac = /MAC:\s*([0-9A-Fa-f:]{17})/.exec(block)?.[1];
    if (!mac) continue;
    rows.push({ index: head[1], mac, name: /Name:[ \t]*([^\n]+)/.exec(block)?.[1]?.trim() || head[2],
      rssi: Number(/RSSI:\s*(-?\d+)/.exec(block)?.[1] || -100),
      type: /flipper/i.test(head[2]) ? 'FLIPPER_ZERO' : /airtag/i.test(head[2]) ? 'AIR_TAG' : 'GENERIC',
      kind: 'detector' });
  }
  for (const line of text.split(/\r?\n/)) {
    const m = /^\s*\[(\d+)\](?: \*)? (.+?) \| (.+?) \| (-?\d+) dBm\s*$/.exec(line);
    if (!m) continue;
    const isMac = /^[0-9A-Fa-f:]{17}$/.test(m[3]);
    rows.push({ index: m[1], mac: isMac ? m[3] : null, name: isMac ? m[2] : m[3],
      type: /flipper/i.test(m[2]) ? 'FLIPPER_ZERO' : /airtag/i.test(m[2]) ? 'AIR_TAG' : 'GENERIC',
      rssi: Number(m[4]), kind: 'detector' });
  }
  return uniqueRows(rows, 'mac');
}

function parseAllFlippers(text) {
  const marker = normalizeOutput(text).match(/^> (?:blescan -f|listflippers)\b.*$/gm)?.at(-1);
  const listed = marker?.startsWith('> listflippers') || (!marker && /Discovered Flippers/i.test(text));
  if (listed && marker) text = getOutputBlock(text, 'listflippers').join('\n');
  return parseIndexedRows(text, listed ? '\\[\\d+\\]\\s*MAC:' : '\\[\\d+\\]\\s*(?:White|Black|Transparent)?\\s*Flipper',
    block => { const row = Parsers.flipper(block, listed); return row && { ...row, type: 'FLIPPER_ZERO', kind: 'flipper' }; },
    /^(?:blescan -f|listflippers)\b/i);
}

function parseAllAirTags(text) {
  const marker = normalizeOutput(text).match(/^> (?:blescan -a|listairtags)\b.*$/gm)?.at(-1);
  const listed = marker?.startsWith('> listairtags') || (!marker && /Discovered AirTags/i.test(text));
  if (listed && marker) text = getOutputBlock(text, 'listairtags').join('\n');
  return parseIndexedRows(text, listed ? '\\[\\d+\\]\\s*MAC:' : '\\[\\d+\\]\\s*AirTag',
    block => { const row = Parsers.airTag(block, listed); return row && { ...row, type: 'AIR_TAG', kind: 'airtag' }; },
    /^(?:blescan -a|listairtags)\b/i);
}

function parseAllGattDevices(text) {
  return parseIndexedRows(text, '\\[\\d+\\]\\s*Name:',
    block => { const row = Parsers.gattDevice(block); return row && { ...row, kind: 'gatt' }; },
    /^(?:blescan -g|listgatt)\b/i);
}

function parseAllSdEntries(text) {
  const rows = latestCommandOutput(text, /^sd list\b/i).split(/\r?\n/).map(line => Parsers.sdEntry(line)).filter(Boolean);
  return uniqueRows(rows);
}

function getOutputBlock(logs, command) {
  const lines = normalizeOutput(logs).split(/\r?\n/).map(l => l.trimEnd());
  const start = lines.lastIndexOf(`> ${command}`);
  if (start < 0) return [];
  const out = [];
  for (let i = start + 1; i < lines.length; i++) {
    if (/^> /.test(lines[i])) break;
    out.push(lines[i]);
  }
  return out;
}

function parseKeyValueBlock(text, headerRe, footerRe) {
  text = headerRe ? latestBlock(text, headerRe, footerRe) : normalizeOutput(text);
  const out = [];
  let inBlock = !headerRe;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (headerRe && headerRe.test(line)) { inBlock = true; continue; }
    if (footerRe && footerRe.test(line)) break;
    if (!inBlock) continue;
    const kv = line.match(/^([^=:]+)\s*[=:]\s*(.*)$/);
    if (kv) out.push({ key: kv[1].trim(), value: kv[2].trim() });
  }
  return out;
}
