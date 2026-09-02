"""
Enumerate exported (T) symbols in ESP32-C5 WiFi/PHY blobs and categorize them
to find internal APIs useful for novel attack primitives.
"""
import subprocess, re, collections, os

NM = r"I:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin\riscv32-esp-elf-nm.exe"
WIFI = r"I:\Espressif\frameworks\esp-idf-v6.0\components\esp_wifi\lib\esp32c5"
PHY  = r"I:\Espressif\frameworks\esp-idf-v6.0\components\esp_phy\lib\esp32c5"

BLOBS = {
    "net80211": os.path.join(WIFI, "libnet80211.a"),
    "pp":       os.path.join(WIFI, "libpp.a"),
    "core":     os.path.join(WIFI, "libcore.a"),
    "espnow":   os.path.join(WIFI, "libespnow.a"),
    "mesh":     os.path.join(WIFI, "libmesh.a"),
    "smartcfg": os.path.join(WIFI, "libsmartconfig.a"),
    "wapi":     os.path.join(WIFI, "libwapi.a"),
    "phy":      os.path.join(PHY,  "libphy.a"),
    "rfate":    os.path.join(PHY,  "librfate.a"),
    "rftest":   os.path.join(PHY,  "librftest.a"),
    "btbb":     os.path.join(PHY,  "libbtbb.a"),
    "bttest":   os.path.join(PHY,  "libbttestmode.a"),
}

def nm_exports(path):
    out = subprocess.run(
        [NM, "--print-armap", "--defined-only", path],
        capture_output=True, text=True, errors="ignore"
    ).stdout
    syms = []
    for line in out.splitlines():
        # 00001234 T name   OR   00001234 t name
        m = re.match(r"^[0-9a-fA-F]+\s+([Tt])\s+(\S+)", line.strip())
        if m:
            syms.append((m.group(1) == "T", m.group(2)))
    return syms

# Public esp_wifi.h API we already have (to filter out)
PUBLIC_API = set()
for hdr in [
    r"I:\Espressif\frameworks\esp-idf-v6.0\components\esp_wifi\include\esp_wifi.h",
    r"I:\Espressif\frameworks\esp-idf-v6.0\components\esp_wifi\include\esp_wifi_types.h",
    r"I:\Espressif\frameworks\esp-idf-v6.0\components\esp_wifi\include\esp_wifi_he.h",
    r"I:\Espressif\frameworks\esp-idf-v6.0\components\esp_private\wifi.h",
]:
    try:
        for line in open(hdr, encoding="utf-8", errors="ignore"):
            m = re.match(r"\s*(?:esp_err_t|void|int|bool|uint\w+_t)\s+(\w+)\s*\(", line)
            if m:
                PUBLIC_API.add(m.group(1))
    except FileNotFoundError:
        pass

print(f"=== Public esp_wifi API symbols known: {len(PUBLIC_API)} ===\n")

all_syms = {}
for name, path in BLOBS.items():
    if not os.path.exists(path):
        print(f"  MISSING: {path}")
        continue
    syms = nm_exports(path)
    all_syms[name] = syms
    print(f"  {name:10} {len(syms):>5} exported ({sum(1 for e,_ in syms if e)} global)")

# Save full dumps
with open(r"I:\GhostESP2\Ghost_ESP\wifi_symbols_dump.txt", "w", encoding="utf-8") as f:
    for name, syms in all_syms.items():
        f.write(f"########## {name} ##########\n")
        for exported, sym in sorted(syms, key=lambda x: x[1]):
            f.write(f"  {'T' if exported else 't'}  {sym}\n")
        f.write("\n")

# Categorize by attack-relevant patterns
CATEGORIES = {
    "RAW_TX_RX":        r"(?i)(raw_tx|80211_tx|tx_raw|inject|send_raw|send_80211|tx_frame|send_frame|send_mgmt|tx_mgmt|send_action|raw_recv|raw_rx|raw_data)",
    "MGT_FRAME":        r"(?i)(send_(deauth|disassoc|auth|assoc|beacon|probe|action)|ieee80211_send|ieee80211_mgmt|ieee80211_input|ieee80211_recv|ieee80211_encap|ieee80211_decap)",
    "CHANNEL_PHY":      r"(?i)(set_chan|set_channel|switch_chan|offchan|set_phy|set_bw|set_band|freq_to|chan_to_freq|set_rate)",
    "TX_POWER":         r"(?i)(tx_power|power_limit|set_power|power_ctrl|boost|rf_power)",
    "PROMISCUOUS":      r"(?i)(promisc|sniffer|monitor|set_filter|packet_filter|rx_filter)",
    "CSI":              r"(?i)(csi|channel_state|steering|beamform)",
    "AUTH_BYPASS":      r"(?i)(auth_bypass|assoc_bypass|skip_auth|fast_auth|owe|sae|h2e|ft_auth|ft_assoc)",
    "KEY_INSTALL":      r"(?i)(install_key|set_key|set_gtk|set_ptk|set_pmk|wpa_key|key_install|set_tmk|key_mtx|crypto_set)",
    "JAMMING_RF":       r"(?i)(jam|cw|continuous_wave|tone|carrier|tx_tone|rx_tone|preamble|jammer|raw_phy|phy_tx|phy_rx|continuous_tx|cont_tx|cont_rx|rx_cont)",
    "TEST_MODE":        r"(?i)(testmode|test_mode|rf_test|cmd_test|set_test|enter_test|exit_test|rfmode|rf_mode)",
    "RATE_CTRL":        r"(?i)(rate_ctrl|amrr|set_mcs|set_mod|set_gi|set_ltf|set_nss|set_stbc|set_ldpc)",
    "MFP_BYPASS":       r"(?i)(mfp|pmf|bip|sa_query|robust_mgmt|mfpc|mfpr|unprotect)",
    "AMSDU_AMPDU":      r"(?i)(amsdu|ampdu|addba|delba|blockack|ba_setup|ba_teardown)",
    "REG_DOMAIN":       r"(?i)(regdomain|country|regulatory|set_reg|ieee80211_regdomain)",
    "MAC_SPOOF":        r"(?i)(set_mac|spoof_mac|clone_mac|set_bssid|set_self_mac|addr_mask|rand_mac|set_addr)",
    "SCAN_INTERNAL":    r"(?i)(scan_start|scan_stop|scan_param|scan_add|scan_delete|scan_chan|scan_result|scan_flush|fast_scan|targeted_scan)",
    "TIMING":           r"(?i)(tsf|timing|timestamp|set_tsf|adj_tsf|sync_tsf|beacon_int|tbtt)",
    "VENDOR_IE":        r"(?i)(vendor_ie|add_ie|append_ie|ie_set|set_ie|user_ie|vnd_ie)",
}

# Tag symbols
tagged = collections.defaultdict(list)
for blob, syms in all_syms.items():
    for exported, sym in syms:
        if sym in PUBLIC_API:
            continue  # already public, skip
        for cat, pat in CATEGORIES.items():
            if re.search(pat, sym):
                tagged[cat].append((blob, exported, sym))

print("\n=== INTERNAL APIs BY CATEGORY (excluding public esp_wifi.h) ===")
for cat in CATEGORIES:
    items = sorted(set(tagged[cat]))
    if not items:
        continue
    print(f"\n--- {cat} ({len(items)}) ---")
    for blob, exp, sym in items[:40]:
        flag = "G" if exp else "l"  # Global vs local
        print(f"  [{blob:8} {flag}] {sym}")
    if len(items) > 40:
        print(f"  ... +{len(items)-40} more")
