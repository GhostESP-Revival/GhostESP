// cmd_aerial.c
// Aerial and Flock drone/surveillance detection commands.

#include "core/commands.h"
#include "core/glog.h"
#include "esp_timer.h"
#include "managers/aerial_detector_manager.h"
#include "managers/flock_detector_manager.h"
#include "managers/views/terminal_screen.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void handle_aerial_scan_cmd(int argc, char **argv) {
    uint32_t duration = 30000;  // default 30 seconds

    if (argc > 1) {
        duration = atoi(argv[1]) * 1000;
        if (duration < 1000) duration = 1000;
        if (duration > 300000) duration = 300000;
    }

    aerial_detector_init();
    esp_err_t ret = aerial_detector_start_scan(duration);

    if (ret == ESP_OK) {
        glog("Scan Started (%lu sec)\n", duration / 1000);
        glog("Phase 1: WiFi | Phase 2: BLE\n");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        glog("Scan already running\n");
    } else {
        glog("Failed to start scan\n");
    }
}

void handle_aerial_list_cmd(int argc, char **argv) {
    aerial_detector_compact_known_devices();
    
    int total = aerial_detector_get_device_count();
    int shown = 0;
    
    for (int i = 0; i < total; i++) {
        AerialDevice *dev = aerial_detector_get_device(i);
        if (!dev || dev->type == AERIAL_TYPE_UNKNOWN) continue;
        
        if (shown == 0) {
            glog("Detected aerial device(s):\n\n");
        }
        shown++;
        
        glog("[%d] %s\n", i, dev->device_id);
        glog("    MAC: %s\n", dev->mac);
        glog("    Type: %s\n", aerial_detector_get_type_string(dev->type));
        glog("    RSSI: %d dBm\n", dev->rssi);
        
        if (dev->vendor[0] != '\0') {
            glog("    Vendor: %s\n", dev->vendor);
        }
        
        if (dev->has_location) {
            glog("    Location: %.6f, %.6f\n", dev->latitude, dev->longitude);
            if (dev->altitude > -1000.0f) {
                glog("    Altitude: %.1f m\n", dev->altitude);
            }
            if (dev->speed_horizontal < 255.0f) {
                glog("    Speed: %.1f m/s @ %.0f°\n", dev->speed_horizontal, dev->direction);
            }
            glog("    Status: %s\n", aerial_detector_get_status_string(dev->status));
        }
        
        if (dev->has_operator_location) {
            glog("    Operator: %.6f, %.6f", dev->operator_latitude, dev->operator_longitude);
            if (dev->operator_altitude > -1000.0f) {
                glog(" @ %.1f m", dev->operator_altitude);
            }
            glog("\n");
        }
        
        if (strcmp(dev->operator_id, "N/A") != 0) {
            glog("    Operator ID: %s\n", dev->operator_id);
        }
        
        if (strcmp(dev->description, "N/A") != 0 && dev->description[0] != '\0') {
            glog("    Description: %s\n", dev->description);
        }
        
        uint32_t age_sec = (esp_timer_get_time() / 1000 - dev->last_seen_ms) / 1000;
        glog("    Last seen: %lu sec ago\n", age_sec);
        glog("\n");
    }

    if (shown == 0) {
        glog("No aerial devices detected\n");
    }
}

void handle_aerial_track_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: aerialtrack <device_index|mac_address>\n");
        glog("Use 'aeriallist' to see available devices\n");
        return;
    }
    
    AerialDevice *dev = NULL;
    
    // check if argument is a number (device index)
    if (argv[1][0] >= '0' && argv[1][0] <= '9') {
        int index = atoi(argv[1]);
        dev = aerial_detector_get_device(index);
        if (!dev) {
            glog("Invalid device index. Use 'aeriallist' to see available devices\n");
            return;
        }
    } else {
        // assume mac address
        dev = aerial_detector_find_device_by_mac(argv[1]);
        if (!dev) {
            glog("Device not found: %s\n", argv[1]);
            return;
        }
    }
    
    // ensure scanning is running to keep updates flowing
    if (!aerial_detector_is_scanning()) {
        aerial_detector_start_scan(30000); // default 30s; tracking will refresh each phase
        glog("Started aerial scan for tracking\n");
    }
    
    esp_err_t ret = aerial_detector_track_device(dev->mac);
    if (ret == ESP_OK) {
        glog("Now tracking: %s (%s)\n", dev->device_id, dev->mac);
        glog("RSSI: %d dBm\n", dev->rssi);
        
        if (dev->has_location) {
            glog("Location: %.6f, %.6f @ %.1f m\n", 
                 dev->latitude, dev->longitude, dev->altitude);
        }
    } else {
        glog("Failed to track device\n");
    }
}

void handle_aerial_stop_cmd(int argc, char **argv) {
    if (aerial_detector_is_scanning()) {
        aerial_detector_stop_scan();
        glog("Scan Stopped\n");
    } else {
        glog("No scan running\n");
    }

    aerial_detector_untrack_device();
}

void handle_aerial_spoof_cmd(int argc, char **argv) {
    const char *device_id;
    double lat;
    double lon;
    float alt;
    
    if (argc < 2) {
        // default test mode - no args needed
        device_id = "GHOST-TEST";
        lat = 37.7749;   // san francisco
        lon = -122.4194;
        alt = 100.0f;
        glog("Using default test drone:\n");
        glog("Device ID: %s\n", device_id);
        glog("Location: %.6f, %.6f @ %.1fm\n\n", lat, lon, alt);
    } else if (argc < 5) {
        glog("Usage: aerialspoof [device_id latitude longitude altitude]\n");
        glog("Examples:\n");
        glog("  aerialspoof                              # Use defaults\n");
        glog("  aerialspoof DRONE-1234 40.7128 -74.0060 100\n");
        glog("\nBroadcasts fake drone RemoteID for testing purposes.\n");
        glog("Complies with ASTM F3411 OpenDroneID standard.\n");
        return;
    } else {
        device_id = argv[1];
        lat = atof(argv[2]);
        lon = atof(argv[3]);
        alt = atof(argv[4]);
    }
    
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        glog("Invalid coordinates. Lat: -90 to 90, Lon: -180 to 180\n");
        return;
    }
    
    // stop existing spoof if running
    if (aerial_detector_is_emulating()) {
        aerial_detector_stop_emulation();
    }
    
    aerial_detector_init();
    esp_err_t ret = aerial_detector_start_emulation(device_id, lat, lon, alt);
    
    if (ret == ESP_OK) {
        glog("Spoofing Started\n");
        glog("ID: %s | Pos: %.6f, %.6f @ %.1fm\n", device_id, lat, lon, alt);
    } else {
        glog("Failed to start spoofing\n");
    }
}

void handle_aerial_spoof_stop_cmd(int argc, char **argv) {
    if (aerial_detector_is_emulating()) {
        aerial_detector_stop_emulation();
        glog("Spoofing Stopped\n");
    } else {
        glog("No spoofing active\n");
    }
}

void handle_flock_scan_cmd(int argc, char **argv) {
    if (flock_detector_is_running()) {
        glog("Flock detection is already running. Use 'flockstop' to stop first.\n");
        return;
    }
    flock_detector_init();
    esp_err_t ret = flock_detector_start();
    if (ret == ESP_OK) {
        TERMINAL_VIEW_ADD_TEXT("Flock detection started\n");
    } else {
        glog("Failed to start flock detection — out of memory\n");
    }
}

void handle_flock_list_cmd(int argc, char **argv) {
    int count = flock_detector_get_count();
    if (count == 0) {
        glog("No surveillance devices detected yet.\n");
        if (!flock_detector_is_running()) {
            glog("Use 'flockscan' to start scanning.\n");
        }
        return;
    }
    glog("Surveillance Devices Detected (%d):\n", count);
    glog("%-3s %-18s %-17s %-4s %-8s %3s %5s %s\n", "#", "MAC", "Method", "Conf", "Signal", "Ch", "Hits", "SSID");
    glog("--- ------------------ ----------------- ---- -------- --- ----- ------\n");
    for (int i = 0; i < count; i++) {
        const FlockDetection *d = flock_detector_get_detection(i);
        if (!d) continue;
        const char *sig = (d->rssi > -50) ? "Strong" : (d->rssi > -70) ? "Medium" : "Weak";
        char sigbuf[16];
        snprintf(sigbuf, sizeof(sigbuf), "%s (%dd)", sig, d->rssi);
        glog("%-3d %-18s %-17s %-4s %-8s %3d %5d %s\n",
             i, d->mac, d->method,
             d->confidence == FLOCK_CONF_HIGH ? "HIGH" : "LOW",
             sigbuf, d->channel, d->count,
             d->ssid[0] ? d->ssid : "-");
    }
}

void handle_flock_stop_cmd(int argc, char **argv) {
    if (!flock_detector_is_running()) {
        glog("Flock detection is not currently running.\n");
        return;
    }
    flock_detector_stop();
    TERMINAL_VIEW_ADD_TEXT("Flock detection stopped\n");
}
