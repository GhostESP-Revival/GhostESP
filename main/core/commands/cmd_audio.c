// cmd_audio.c
// Audio player, receiver, and microphone calibration commands.

#include "core/commands.h"
#include "core/glog.h"
#include "sdkconfig.h"
#ifdef CONFIG_HAS_AUDIO_PLAYER
#include "managers/audio_stream_manager.h"
#endif
#if defined(CONFIG_HAS_TLV320DAC_I2S) || defined(CONFIG_HAS_AW88298_SPEAKER)
#include "managers/audio_receiver_manager.h"
#endif
#ifdef CONFIG_HAS_MIC
#include "managers/microphone/mic_driver.h"
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void handle_audio_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: audio <start|stop|pause|resume|flush|state>\n");
        return;
    }

    const char *sub = argv[1];

#ifdef CONFIG_HAS_AUDIO_PLAYER
    if (strcmp(sub, "state") == 0) {
        if (argc >= 4) {
            uint32_t played_ms = (argc >= 5) ? (uint32_t)strtoul(argv[4], NULL, 10) : 0;
            audio_stream_manager_update_receiver_status((size_t)strtoul(argv[2], NULL, 10),
                                                        (size_t)strtoul(argv[3], NULL, 10),
                                                        played_ms);
        }
        return;
    }
#endif

#if defined(CONFIG_HAS_TLV320DAC_I2S) || defined(CONFIG_HAS_AW88298_SPEAKER)
    if (strcmp(sub, "start") == 0) {
        if (!audio_receiver_manager_is_initialized()) {
            esp_err_t ret = audio_receiver_manager_init();
            if (ret != ESP_OK) {
                glog("Audio receiver init failed: %s\n", esp_err_to_name(ret));
                return;
            }
        }
        audio_receiver_manager_start();
        glog("Audio receiver started\n");
    } else if (strcmp(sub, "stop") == 0) {
        audio_receiver_manager_stop();
        glog("Audio receiver stopped\n");
    } else if (strcmp(sub, "pause") == 0) {
        audio_receiver_manager_pause();
        glog("Audio receiver paused\n");
    } else if (strcmp(sub, "resume") == 0) {
        audio_receiver_manager_resume();
        glog("Audio receiver resumed\n");
    } else if (strcmp(sub, "flush") == 0) {
        audio_receiver_manager_flush();
        glog("Audio receiver flushed\n");
    } else {
        glog("Unknown audio command: %s\n", sub);
    }
#else
    (void)sub;
    glog("Audio not supported on this device\n");
#endif
}

#ifdef CONFIG_HAS_MIC
void handle_mic_cal_cmd(int argc, char **argv) {
    extern void goertzel_restart_cal(void);
    extern void mic_restart_noise_cal(void);
    goertzel_restart_cal();
    mic_restart_noise_cal();
    glog("MIC calibration restarted\n");
}
#endif
