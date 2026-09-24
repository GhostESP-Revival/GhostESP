// cmd_mesh.c
// Unified mesh layer: one place to see and switch between the Meshtastic
// (`lora`) and MeshCore (`meshcore`) backends, plus protocol-agnostic verbs
// (send / dm / peers / channels / app).
//
// Only one backend can own the SX1262 at a time. Switching stops the other
// one and remembers the choice in NVS so `mesh on` resumes it. Protocol-specific
// settings (regions, presets, PKI, adverts, identity) stay under `lora` and
// `meshcore`; `mesh` just makes the common path obvious.

#include "core/commands.h"
#include "core/glog.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#ifdef CONFIG_HAS_LORA
#include "managers/lora_manager.h"
#endif
#ifdef CONFIG_HAS_MESHCORE
#include "managers/meshcore_manager.h"
#endif

typedef enum {
    MESH_BACKEND_NONE = -1,
    MESH_BACKEND_MESHTASTIC = 0,
    MESH_BACKEND_MESHCORE = 1,
} mesh_backend_t;

static mesh_backend_t active_backend(void) {
#ifdef CONFIG_HAS_MESHCORE
    if (mc_manager_is_running()) return MESH_BACKEND_MESHCORE;
#endif
#ifdef CONFIG_HAS_LORA
    if (lora_manager_is_running()) return MESH_BACKEND_MESHTASTIC;
#endif
    return MESH_BACKEND_NONE;
}

static const char *backend_name(mesh_backend_t b) {
    switch (b) {
        case MESH_BACKEND_MESHCORE: return "MeshCore";
        case MESH_BACKEND_MESHTASTIC: return "Meshtastic";
        default: return "none";
    }
}

static mesh_backend_t parse_backend(const char *s) {
    if (!s) return MESH_BACKEND_NONE;
    if (!strcmp(s, "meshtastic") || !strcmp(s, "mt") || !strcmp(s, "lora"))
        return MESH_BACKEND_MESHTASTIC;
    if (!strcmp(s, "meshcore") || !strcmp(s, "mc"))
        return MESH_BACKEND_MESHCORE;
    return MESH_BACKEND_NONE;
}

static mesh_backend_t stored_backend(void) {
#ifdef CONFIG_HAS_MESHCORE
    return mc_manager_default_backend_meshcore() ? MESH_BACKEND_MESHCORE : MESH_BACKEND_MESHTASTIC;
#else
    return MESH_BACKEND_MESHTASTIC;
#endif
}

static void store_backend(mesh_backend_t b) {
#ifdef CONFIG_HAS_MESHCORE
    mc_manager_set_default_backend_meshcore(b == MESH_BACKEND_MESHCORE);
#else
    (void)b;
#endif
}

// The line users need to see: how to get to the other backend.
static void print_switch_hint(void) {
    mesh_backend_t cur = active_backend();
#ifdef CONFIG_HAS_MESHCORE
    const char *other = (cur == MESH_BACKEND_MESHCORE) ? "meshtastic"
                     : (cur == MESH_BACKEND_MESHTASTIC) ? "meshcore" : NULL;
    if (other) glog("Switch with: mesh switch %s   |   mesh off\n", other);
    else glog("Switch with: mesh switch meshtastic   |   mesh switch meshcore\n");
#else
    (void)cur;
    glog("Switch with: mesh switch meshtastic\n");
#endif
}

static void cmd_status(void) {
    mesh_backend_t b = active_backend();
    if (b == MESH_BACKEND_NONE) {
        glog("Mesh: idle - no protocol is using the radio (boot selection: %s)\n",
             backend_name(stored_backend()));
    } else {
        glog("Mesh: %s is active\n", backend_name(b));
#ifdef CONFIG_HAS_MESHCORE
        if (b == MESH_BACKEND_MESHCORE) {
            char *a[] = {(char *)"meshcore", (char *)"status"};
            handle_meshcore_cmd(2, a);
        } else
#endif
#ifdef CONFIG_HAS_LORA
        {
            char *a[] = {(char *)"lora", (char *)"status"};
            handle_lora_cmd(2, a);
        }
#endif
    }
    print_switch_hint();
}

static void cmd_off(void) {
    mesh_backend_t b = active_backend();
    if (b == MESH_BACKEND_NONE) {
        glog("Mesh: nothing is running\n");
    } else {
        if (b == MESH_BACKEND_MESHCORE) {
            char *a[] = {(char *)"meshcore", (char *)"stop"};
            handle_meshcore_cmd(2, a);
        } else {
            char *a[] = {(char *)"lora", (char *)"stop"};
            handle_lora_cmd(2, a);
        }
        glog("Mesh: %s stopped (radio released)\n", backend_name(b));
    }
    print_switch_hint();
}

static void cmd_switch(mesh_backend_t want) {
    if (want == MESH_BACKEND_NONE) {
        glog("Usage: mesh switch <meshtastic|meshcore>\n");
        return;
    }
#ifndef CONFIG_HAS_MESHCORE
    if (want == MESH_BACKEND_MESHCORE) {
        glog("MeshCore not enabled on this board (enable HAS_MESHCORE in Kconfig)\n");
        return;
    }
#endif
#ifndef CONFIG_HAS_LORA
    if (want == MESH_BACKEND_MESHTASTIC) {
        glog("Meshtastic not enabled on this board (enable HAS_LORA in Kconfig)\n");
        return;
    }
#endif
#ifdef CONFIG_HAS_LORA
    if (want == MESH_BACKEND_MESHTASTIC && lora_manager_needs_setup()) {
        char q[640];
        if (lora_manager_setup_text(q, sizeof(q))) glog("%s", q);
        return;
    }
#endif

    mesh_backend_t cur = active_backend();
    if (cur == want) {
        glog("Mesh: already on %s\n", backend_name(want));
        print_switch_hint();
        return;
    }

    glog("Starting %s...\n", backend_name(want));
    if (want == MESH_BACKEND_MESHCORE) {
        char *a[] = {(char *)"meshcore", (char *)"start"};
        handle_meshcore_cmd(2, a);
    } else {
        char *a[] = {(char *)"lora", (char *)"start"};
        handle_lora_cmd(2, a);
    }

    if (active_backend() == want) {
        /* The backend command persists its selection only after a successful
         * start. Keep this fallback for implementations that do not. */
        store_backend(want);
        glog("Active mesh: %s\n", backend_name(want));
    } else {
        glog("Active mesh: none (start failed - see message above)\n");
    }
    print_switch_hint();
}

static void cmd_on(int argc, char **argv) {
    if (argc >= 3) {
        mesh_backend_t b = parse_backend(argv[2]);
        if (b == MESH_BACKEND_NONE) {
            glog("Unknown protocol '%s'. Use: mesh on meshtastic | mesh on meshcore\n", argv[2]);
            return;
        }
        cmd_switch(b);
        return;
    }
    mesh_backend_t cur = active_backend();
    if (cur != MESH_BACKEND_NONE) {
        glog("Mesh: %s is already running\n", backend_name(cur));
        print_switch_hint();
        return;
    }
    cmd_switch(stored_backend());
}

// Run a protocol-agnostic verb against whichever backend is active by
// rewriting argv in place and delegating to the existing handler.
static void passthrough(int argc, char **argv, mesh_backend_t b) {
    const char *verb = argv[1];
    if (b == MESH_BACKEND_MESHCORE) {
        argv[0] = (char *)"meshcore";
        if (!strcmp(verb, "messages")) argv[1] = (char *)"messages";
        else if (!strcmp(verb, "peers")) argv[1] = (char *)"contacts";
        else if (!strcmp(verb, "app")) argv[1] = (char *)"ble";
        handle_meshcore_cmd(argc, argv);
    } else {
        argv[0] = (char *)"lora";
        if (!strcmp(verb, "send")) argv[1] = (char *)"chat";
        else if (!strcmp(verb, "messages")) argv[1] = (char *)"chat";
        else if (!strcmp(verb, "peers")) argv[1] = (char *)"nodes";
        else if (!strcmp(verb, "app")) argv[1] = (char *)"ble";
        handle_lora_cmd(argc, argv);
    }
}

static void print_help(void) {
    glog("Switching (one radio, only one runs at a time):\n"
         "  mesh switch meshtastic   use Meshtastic  (alias: lora)\n"
         "  mesh switch meshcore     use MeshCore    (alias: mc)\n"
         "  mesh                     show what is active + how to switch\n"
         "  mesh on [proto]          start the remembered protocol\n"
         "  mesh off                 stop the active protocol\n"
         "  mesh autostart <meshtastic|meshcore|off>  what boots on power-up\n"
         "\n"
         "Active-mesh verbs:\n"
         "  mesh send <text>         send on channel 0\n"
         "  mesh messages            read received messages\n"
         "  mesh dm <peer> <text>    direct message\n"
         "  mesh peers               nodes (Meshtastic) / contacts (MeshCore)\n"
         "  mesh channels            list channels\n"
         "  mesh app [on|off]        BLE app link\n"
         "  mesh selftest            backend self test\n"
         "\n"
         "Protocol-specific commands stay under `lora` and `meshcore`.\n"
         "  mesh meshtastic <args>   same as `lora <args>`\n"
         "  mesh meshcore <args>     same as `meshcore <args>`\n");
}

void handle_mesh_cmd(int argc, char **argv) {
    const char *verb = (argc < 2) ? "status" : argv[1];

    if (!strcmp(verb, "help") || !strcmp(verb, "-h")) { print_help(); return; }
    if (!strcmp(verb, "status")) { cmd_status(); return; }
    if (!strcmp(verb, "on")) { cmd_on(argc, argv); return; }
    if (!strcmp(verb, "off") || !strcmp(verb, "stop")) { cmd_off(); return; }

    if (!strcmp(verb, "switch")) {
        if (argc < 3) { glog("Usage: mesh switch <meshtastic|meshcore>\n"); print_switch_hint(); return; }
        mesh_backend_t b = parse_backend(argv[2]);
        if (b == MESH_BACKEND_NONE) {
            glog("Unknown protocol '%s'. Use meshtastic or meshcore.\n", argv[2]);
            return;
        }
        cmd_switch(b);
        return;
    }

    // Boot auto-start selection: share the `lora autostart` implementation so
    // `mesh autostart meshcore` etc. behave identically.
    if (!strcmp(verb, "autostart")) {
#ifdef CONFIG_HAS_LORA
        if (argc >= 3) {
            char *a[] = {(char *)"lora", (char *)"autostart", argv[2]};
            handle_lora_cmd(3, a);
        } else {
            char *a[] = {(char *)"lora", (char *)"autostart"};
            handle_lora_cmd(2, a);
        }
#else
        glog("LoRa not enabled on this board\n");
#endif
        return;
    }

    // Protocol namespaces: `mesh meshtastic ...` / `mesh meshcore ...`
    mesh_backend_t ns = parse_backend(verb);
    if (ns == MESH_BACKEND_MESHTASTIC) {
#ifdef CONFIG_HAS_LORA
        argv[0] = (char *)"lora";
        for (int i = 1; i + 1 < argc; ++i) argv[i] = argv[i + 1];
        handle_lora_cmd(argc - 1, argv);
#else
        glog("Meshtastic not enabled on this board\n");
#endif
        return;
    }
    if (ns == MESH_BACKEND_MESHCORE) {
#ifdef CONFIG_HAS_MESHCORE
        argv[0] = (char *)"meshcore";
        for (int i = 1; i + 1 < argc; ++i) argv[i] = argv[i + 1];
        handle_meshcore_cmd(argc - 1, argv);
#else
        glog("MeshCore not enabled on this board\n");
#endif
        return;
    }

    // Verbs that need an active backend.
    if (!strcmp(verb, "send") || !strcmp(verb, "messages") || !strcmp(verb, "dm") ||
        !strcmp(verb, "peers") || !strcmp(verb, "channels") || !strcmp(verb, "app") ||
        !strcmp(verb, "selftest")) {
        mesh_backend_t b = active_backend();
        if (b == MESH_BACKEND_NONE) {
            glog("No mesh is running. Start one first:\n");
            print_switch_hint();
            return;
        }
        if (!strcmp(verb, "selftest")) {
#ifdef CONFIG_HAS_MESHCORE
            if (b == MESH_BACKEND_MESHCORE) {
                char *a[] = {(char *)"meshcore", (char *)"selftest"};
                handle_meshcore_cmd(2, a);
                return;
            }
#endif
            glog("No self test for Meshtastic; use `lora pkselftest`\n");
            return;
        }
        passthrough(argc, argv, b);
        return;
    }

    print_help();
}
