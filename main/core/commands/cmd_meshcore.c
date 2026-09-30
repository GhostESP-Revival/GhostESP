// cmd_meshcore.c
// MeshCore command handler (mirrors cmd_lora.c structure).

#include "core/commands.h"
#include "core/glog.h"
#include "managers/meshcore_ble.h"
#include "managers/meshcore_config.h"
#include "managers/meshcore_crypto.h"
#include "managers/meshcore_identity.h"
#include "managers/meshcore_manager.h"
#include "managers/meshcore_mesh.h"
#include "managers/meshcore_store.h"
#include "sdkconfig.h"
#ifdef CONFIG_HAS_LORA
#include "managers/lora_manager.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_HAS_MESHCORE

static void mc_print_help(void) {
    glog("meshcore                 Show status\n"
         "meshcore on / off        Start / stop (stops Meshtastic first)\n"
         "meshcore autostart [on|off]  Boot into MeshCore (on) / disable\n"
         "meshcore send <text>     Send a group message on channel 0\n"
         "meshcore ch <0-39> <text> Send a group message on a channel\n"
         "meshcore dm <name|!hex> <text>  Direct message a contact\n"
         "meshcore advert [flood]  Send a self advert (zero-hop by default)\n"
         "meshcore contacts        List known contacts\n"
         "meshcore channels        List channel slots\n"
         "meshcore pubkey          Show our Ed25519 public key\n"
         "meshcore set name <n>    Set the advert name\n"
         "meshcore set freq <mhz> / bw <khz> / sf <5-12> / cr <5-8> / tx <dbm>\n"
         "meshcore set repeat <on|off>  Forward group/advert floods (default off)\n"
         "meshcore set latlon <lat> <lon> / loc <0|1>\n"
         "meshcore selftest        Run crypto known-answer tests\n"
         "meshcore regen           Regenerate the identity keypair\n"
         "meshcore ble [on|off|status]  BLE companion (MeshCore app link)\n"
         "meshcore messages        Drain received messages\n"
         "Tip: `mesh` shows the active mesh and switches Meshtastic <-> MeshCore\n");
}

static void mc_status(void) {
    mc_status_t st;
    mc_manager_get_status(&st);
    glog("MeshCore: %s radio:%s %.3fMHz BW%.1f SF%u CR4/%u TX%d nodes:%d rssi:%d snr:%.1f\n",
         st.running ? "ON" : "OFF", st.radio_present ? "ok" : "--",
         (double)st.freq_mhz, (double)st.bw_khz, (unsigned)st.sf, (unsigned)st.cr,
         st.tx_dbm, st.node_count, st.last_rssi, (double)st.last_snr);
    glog("  tx_ok:%u rx_ok:%u dup:%u bad:%u err:%s\n",
         (unsigned)st.tx_ok, (unsigned)st.rx_ok, (unsigned)st.rx_dup,
         (unsigned)st.rx_bad, mc_manager_last_error());
}

static void mc_restart_if_running(void) {
    if (mc_manager_is_running()) {
        mc_manager_stop();
        mc_manager_start();
    }
}

#endif // CONFIG_HAS_MESHCORE

void handle_meshcore_cmd(int argc, char **argv) {
#ifndef CONFIG_HAS_MESHCORE
    (void)argc; (void)argv;
    glog("MeshCore not enabled on this board (enable HAS_MESHCORE in Kconfig)\n");
    return;
#else
    if (argc >= 2 && (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0)) {
        mc_print_help();
        return;
    }
    const char *sub = argc < 2 ? "status" : argv[1];
    if (strcmp(sub, "on") == 0) sub = "start";
    if (strcmp(sub, "off") == 0) sub = "stop";

    if (strcmp(sub, "status") == 0 || strcmp(sub, "radio") == 0) {
        mc_status();
        return;
    }
    if (strcmp(sub, "start") == 0) {
#ifdef CONFIG_HAS_LORA
        if (lora_manager_is_running()) {
            glog("Stopped Meshtastic (one radio: MeshCore now owns it)\n");
        }
#endif
        if (mc_manager_start()) {
            mc_manager_set_default_backend_meshcore(true);
            glog("MeshCore started\n");
        } else {
            glog("MeshCore start failed: %s\n", mc_manager_last_error());
        }
        return;
    }
    if (strcmp(sub, "stop") == 0) {
        mc_manager_stop();
        glog("MeshCore stopped\n");
        return;
    }
    if (strcmp(sub, "autostart") == 0) {
#ifdef CONFIG_HAS_LORA
        const char *arg = argc >= 3 ? argv[2] : NULL;
        if (arg && (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0 ||
                    strcmp(arg, "false") == 0)) {
            glog(lora_manager_set_auto_start(false) ? "Auto-start OFF\n"
                                                    : "auto-start save failed\n");
            return;
        }
        if (arg && (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0 ||
                    strcmp(arg, "true") == 0 || strcmp(arg, "meshcore") == 0 ||
                    strcmp(arg, "mc") == 0)) {
            /* Selecting MeshCore also makes it the remembered boot protocol. */
            mc_manager_set_default_backend_meshcore(true);
            glog(lora_manager_set_auto_start(true)
                     ? "Auto-start ON (MeshCore)\n"
                     : "auto-start save failed\n");
            return;
        }
        glog("meshcore autostart: %s (%s)\n",
             lora_manager_auto_start_enabled() ? "on" : "off",
             mc_manager_default_backend_meshcore() ? "MeshCore" : "Meshtastic");
        glog("Set: meshcore autostart on|off\n");
#else
        glog("LoRa not enabled on this board\n");
#endif
        return;
    }
    if (strcmp(sub, "selftest") == 0) {
        uint8_t fail = mc_manager_selftest();
        glog("MeshCore selftest: %s (mask=0x%02X) sha256=%s hmac=%s aes=%s chash=%s hashtag=%s\n",
             fail ? "FAIL" : "PASS", (unsigned)fail,
             (fail & 0x01) ? "FAIL" : "ok",
             (fail & 0x02) ? "FAIL" : "ok",
             (fail & 0x04) ? "FAIL" : "ok",
             (fail & 0x08) ? "FAIL" : "ok",
             (fail & 0x10) ? "FAIL" : "ok");
        return;
    }
    if (strcmp(sub, "regen") == 0) {
        glog(mc_identity_regen() ? "MeshCore identity regenerated\n" : "regen failed\n");
        return;
    }
    if (strcmp(sub, "pubkey") == 0) {
        const mc_identity_t *id = mc_identity_get();
        if (!id || !id->valid) { glog("MeshCore identity not ready\n"); return; }
        char hex[MC_PUB_KEY_SIZE * 2 + 1];
        mc_to_hex(hex, id->pub_key, MC_PUB_KEY_SIZE);
        glog("MeshCore pubkey: %s (node hash !%02X)\n", hex, (unsigned)id->pub_key[0]);
        return;
    }
    if (strcmp(sub, "advert") == 0) {
        bool flood = (argc >= 3 && strcmp(argv[2], "flood") == 0);
        glog(mc_manager_send_advert(flood) ? "MeshCore advert sent\n" : "MeshCore advert failed\n");
        return;
    }
    if (strcmp(sub, "ch") == 0) {
         if (argc < 4) { glog("Usage: meshcore ch <0-39> <text>\n"); return; }
        int idx = atoi(argv[2]);
        char tmp[192] = {0};
        for (int i = 3; i < argc; ++i) {
            if (i > 3) strncat(tmp, " ", sizeof(tmp) - strlen(tmp) - 1);
            strncat(tmp, argv[i], sizeof(tmp) - strlen(tmp) - 1);
        }
        glog(mc_manager_send_channel_text((uint8_t)idx, tmp) ? "MeshCore queued\n"
                                                             : "MeshCore send failed: %s\n",
             mc_manager_last_error());
        return;
    }
    if (strcmp(sub, "send") == 0 || strcmp(sub, "messages") == 0) {
        if (strcmp(sub, "messages") == 0 || argc < 3) {
            uint32_t seq = 0;
            mc_msg_t msgs[8];
            uint16_t n = mc_manager_msg_since(&seq, msgs, 8);
            if (n == 0) glog("(no messages — `meshcore send <text>` to send)\n");
            for (uint16_t i = 0; i < n; ++i) {
                glog("[%s%s] %s\n", msgs[i].direct ? "dm " : "", msgs[i].who, msgs[i].text);
            }
            return;
        }
        char tmp[192] = {0};
        for (int i = 2; i < argc; ++i) {
            if (i > 2) strncat(tmp, " ", sizeof(tmp) - strlen(tmp) - 1);
            strncat(tmp, argv[i], sizeof(tmp) - strlen(tmp) - 1);
        }
        glog(mc_manager_send_text(tmp) ? "MeshCore queued\n" : "MeshCore send failed: %s\n",
             mc_manager_last_error());
        return;
    }
    if (strcmp(sub, "dm") == 0) {
        if (argc < 4) { glog("Usage: meshcore dm <name|!hex> <text>\n"); return; }
        char tmp[192] = {0};
        for (int i = 3; i < argc; ++i) {
            if (i > 3) strncat(tmp, " ", sizeof(tmp) - strlen(tmp) - 1);
            strncat(tmp, argv[i], sizeof(tmp) - strlen(tmp) - 1);
        }
        glog(mc_manager_send_dm(argv[2], tmp) ? "MeshCore DM queued\n"
                                              : "MeshCore DM failed: %s\n",
             mc_manager_last_error());
        return;
    }
    if (strcmp(sub, "contacts") == 0 || strcmp(sub, "nodes") == 0) {
        int n = mc_mesh_contact_count();
        if (n == 0) { glog("MeshCore contacts: none yet (need an advert)\n"); return; }
        for (int i = 0; i < n; ++i) {
            const mc_contact_t *c = mc_mesh_contact_at(i);
            if (!c) break;
            glog("  !%02X %-20s type:%u hops:%u mod:%u\n", (unsigned)c->pub_key[0],
                 c->name, (unsigned)c->type, (unsigned)(c->out_path_len & 63),
                 (unsigned)c->lastmod);
        }
        return;
    }
    if (strcmp(sub, "channels") == 0) {
        for (int i = 0; i < MC_MAX_GROUP_CHANNELS; ++i) {
            const mc_channel_t *ch = mc_mesh_channel((uint8_t)i);
            bool used = ch && (ch->secret[0] || ch->secret[15]);
            glog("  ch%d: %s hash=%02x name='%s'\n", i, used ? "set" : "empty",
                 ch ? (unsigned)ch->hash : 0, ch ? ch->name : "");
        }
        return;
    }
    if (strcmp(sub, "set") == 0) {
        mc_prefs_t *p = mc_mesh_prefs();
        if (argc < 4) {
            glog("Usage: meshcore set <name|freq|bw|sf|cr|tx|latlon|loc> <value>\n");
            return;
        }
        if (strcmp(argv[2], "name") == 0) {
            glog(mc_mesh_set_node_name(argv[3]) ? "MeshCore name saved\n" : "save failed\n");
        } else if (strcmp(argv[2], "freq") == 0) {
            bool ok = mc_mesh_apply_radio((float)atof(argv[3]), p->bw_khz, p->sf, p->cr);
            glog(ok ? "MeshCore freq saved\n" : "bad frequency\n");
            mc_restart_if_running();
        } else if (strcmp(argv[2], "bw") == 0) {
            bool ok = mc_mesh_apply_radio(p->freq_mhz, (float)atof(argv[3]), p->sf, p->cr);
            glog(ok ? "MeshCore bandwidth saved\n" : "bad bandwidth (7..500 kHz)\n");
            mc_restart_if_running();
        } else if (strcmp(argv[2], "sf") == 0) {
            bool ok = mc_mesh_apply_radio(p->freq_mhz, p->bw_khz, (uint8_t)atoi(argv[3]), p->cr);
            glog(ok ? "MeshCore SF saved\n" : "bad SF (5..12)\n");
            mc_restart_if_running();
        } else if (strcmp(argv[2], "cr") == 0) {
            bool ok = mc_mesh_apply_radio(p->freq_mhz, p->bw_khz, p->sf, (uint8_t)atoi(argv[3]));
            glog(ok ? "MeshCore CR saved\n" : "bad CR (5..8)\n");
            mc_restart_if_running();
        } else if (strcmp(argv[2], "tx") == 0) {
            glog(mc_mesh_set_tx_power((int8_t)atoi(argv[3])) ? "MeshCore TX saved\n" : "bad TX (-9..22)\n");
        } else if (strcmp(argv[2], "repeat") == 0) {
            p->repeat = (strcmp(argv[3], "on") == 0 || strcmp(argv[3], "1") == 0) ? 1 : 0;
            mc_mesh_save_prefs();
            glog(p->repeat ? "MeshCore repeat enabled (forwarding group/advert floods)\n"
                           : "MeshCore repeat disabled\n");
        } else if (strcmp(argv[2], "latlon") == 0) {
            if (argc < 5) { glog("Usage: meshcore set latlon <lat> <lon>\n"); return; }
            p->node_lat = (int32_t)(atof(argv[3]) * 1e6);
            p->node_lon = (int32_t)(atof(argv[4]) * 1e6);
            mc_mesh_save_prefs();
            glog("MeshCore location saved\n");
        } else if (strcmp(argv[2], "loc") == 0) {
            p->advert_loc_policy = (uint8_t)atoi(argv[3]);
            mc_mesh_save_prefs();
            glog("MeshCore advert location policy saved\n");
        } else {
            glog("Unknown meshcore set key\n");
        }
        return;
    }
    if (strcmp(sub, "ble") == 0) {
        const char *a = (argc >= 3) ? argv[2] : "status";
        if (strcmp(a, "on") == 0) {
            glog(mc_ble_start() ? "MeshCore BLE advertising\n"
                                : "MeshCore BLE refused (stop BLE scans/advs first)\n");
        } else if (strcmp(a, "off") == 0) {
            mc_ble_stop();
            glog("MeshCore BLE stopped\n");
        } else {
            glog("MeshCore BLE adv:%s conn:%s linked:%s\n",
                 mc_ble_is_advertising() ? "yes" : "no",
                 mc_ble_is_connected() ? "yes" : "no",
                 mc_ble_is_linked() ? "yes" : "no");
        }
        return;
    }
    mc_print_help();
#endif
}
