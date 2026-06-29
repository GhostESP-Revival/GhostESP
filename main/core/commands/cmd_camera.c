// cmd_camera.c
// Camera motion detection and streaming commands.

#include "core/commands.h"
#include "core/glog.h"
#include "managers/camera_stream_manager.h"
#include "managers/motion_detector_manager.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_HAS_CAMERA
void handle_motion_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: motion <command> [args]\n");
        glog("Commands:\n");
        glog("  motion start           - Start motion detection\n");
        glog("  motion stop            - Stop motion detection\n");
        glog("  motion status          - Show current state\n");
        glog("  motion threshold <1-255>  - Set pixel diff threshold\n");
        glog("  motion interval <100-10000> - Set interval in ms\n");
        glog("  motion percent <1-100> - Set trigger percentage\n");
        glog("  motion sample <1-32>   - Compare every Nth pixel\n");
        glog("  motion snap <on|off>   - Enable/disable SD snapshots\n");
        glog("  motion image <on|off>  - Attach image to Discord alert\n");
        glog("  motion discord <url|off> - Send Discord embed alerts\n");
        glog("  motion webhook <url|off> - Alias for discord/off\n");
        glog("  motion cooldown <ms>    - Webhook alert cooldown\n");
        return;
    }

    if (strcmp(argv[1], "start") == 0) {
        camera_stream_stop();
        esp_err_t ret = motion_detector_start();
        if (ret == ESP_OK) {
            glog("[MOTION] Started\n");
        } else {
            glog("[MOTION] Failed to start\n");
        }
    } else if (strcmp(argv[1], "stop") == 0) {
        motion_detector_stop();
    } else if (strcmp(argv[1], "status") == 0) {
        MotionDetectorState state = motion_detector_get_state();
        glog("[MOTION] Status:\n");
        glog("  Running:    %s\n", state.is_running ? "YES" : "NO");
        glog("  Threshold:  %d\n", state.threshold);
        glog("  Interval:   %dms\n", state.interval_ms);
        glog("  Trigger:    %d%%\n", state.trigger_percent);
        glog("  Sample:     every %d pixel(s)\n", state.sample_step);
        glog("  PSRAM:      %s\n", state.using_psram ? "YES" : "NO");
        glog("  Snapshots:  %s\n", state.save_snapshots ? "ON" : "OFF");
        glog("  Image:      %s\n", state.send_discord_image ? "ON" : "OFF");
        glog("  Webhook:    %s\n", state.webhook_enabled ? "configured" : "OFF");
        glog("  Cooldown:   %dms\n", state.webhook_cooldown_ms);
        glog("  Events:     %d\n", state.motion_count);
    } else if (strcmp(argv[1], "threshold") == 0) {
        if (argc < 3) {
            glog("Usage: motion threshold <1-255>\n");
            return;
        }
        motion_detector_set_threshold(atoi(argv[2]));
    } else if (strcmp(argv[1], "interval") == 0) {
        if (argc < 3) {
            glog("Usage: motion interval <100-10000>\n");
            return;
        }
        motion_detector_set_interval(atoi(argv[2]));
    } else if (strcmp(argv[1], "percent") == 0) {
        if (argc < 3) {
            glog("Usage: motion percent <1-100>\n");
            return;
        }
        motion_detector_set_trigger_percent(atoi(argv[2]));
    } else if (strcmp(argv[1], "sample") == 0) {
        if (argc < 3) {
            glog("Usage: motion sample <1-32>\n");
            return;
        }
        motion_detector_set_sample_step(atoi(argv[2]));
    } else if (strcmp(argv[1], "snap") == 0) {
        if (argc < 3) {
            glog("Usage: motion snap <on|off>\n");
            return;
        }
        motion_detector_set_save_snapshots(strcmp(argv[2], "on") == 0);
    } else if (strcmp(argv[1], "image") == 0) {
        if (argc < 3) {
            glog("Usage: motion image <on|off>\n");
            return;
        }
        motion_detector_set_discord_image(strcmp(argv[2], "on") == 0);
    } else if (strcmp(argv[1], "discord") == 0) {
        if (argc < 3) {
            glog("Usage: motion discord <discord_webhook_url|off>\n");
            return;
        }
        if (strcmp(argv[2], "off") == 0) {
            motion_detector_clear_webhook();
        } else {
            motion_detector_set_webhook(argv[2]);
        }
    } else if (strcmp(argv[1], "webhook") == 0) {
        if (argc < 3) {
            glog("Usage: motion webhook <url|off>\n");
            return;
        }
        if (strcmp(argv[2], "off") == 0) {
            motion_detector_clear_webhook();
        } else {
            motion_detector_set_webhook(argv[2]);
        }
    } else if (strcmp(argv[1], "cooldown") == 0) {
        if (argc < 3) {
            glog("Usage: motion cooldown <ms>\n");
            return;
        }
        motion_detector_set_webhook_cooldown(atoi(argv[2]));
    } else {
        glog("Unknown motion command: %s\n", argv[1]);
    }
}

void handle_camerastream_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: camerastream <command> [args]\n");
        glog("Commands:\n");
        glog("  camerastream start              - Start camera stream\n");
        glog("  camerastream stop               - Stop camera stream\n");
        glog("  camerastream status             - Show current state\n");
        glog("  camerastream quality <1-100>    - Set JPEG quality\n");
        glog("  camerastream resolution <name>  - Set resolution\n");
        glog("  camerastream fps <1-30>         - Set target FPS\n");
        glog("  Resolutions: QQVGA QVGA VGA SVGA XGA SXGA UXGA\n");
        return;
    }

    if (strcmp(argv[1], "start") == 0) {
        esp_err_t ret = camera_stream_start();
        if (ret == ESP_OK) {
            glog("[CAM_STREAM] Started - visit http://ghostesp.local/camera\n");
        } else {
            glog("[CAM_STREAM] Failed to start\n");
        }
    } else if (strcmp(argv[1], "stop") == 0) {
        camera_stream_stop();
    } else if (strcmp(argv[1], "status") == 0) {
        CameraStreamState st = camera_stream_get_state();
        glog("[CAM_STREAM] Status:\n");
        glog("  Running:    %s\n", st.is_running ? "YES" : "NO");
        glog("  Quality:    %d\n", st.quality);
        glog("  Resolution: %d (config)\n", st.frame_size);
        glog("  Target FPS: %d\n", st.fps_target);
        glog("  PSRAM:      %s\n", st.using_psram ? "YES" : "NO");
        glog("  Client:     %s\n", st.client_count > 0 ? "connected" : "none");
        glog("  Frames:     %d\n", st.frames_sent);
    } else if (strcmp(argv[1], "quality") == 0) {
        if (argc < 3) {
            glog("Usage: camerastream quality <1-100>\n");
            return;
        }
        camera_stream_set_quality(atoi(argv[2]));
    } else if (strcmp(argv[1], "resolution") == 0) {
        if (argc < 3) {
            glog("Usage: camerastream resolution <QQVGA|QVGA|VGA|SVGA|XGA|SXGA|UXGA>\n");
            return;
        }
        camera_stream_set_framesize(argv[2]);
    } else if (strcmp(argv[1], "fps") == 0) {
        if (argc < 3) {
            glog("Usage: camerastream fps <1-30>\n");
            return;
        }
        camera_stream_set_fps(atoi(argv[2]));
    } else {
        glog("Unknown camerastream command: %s\n", argv[1]);
    }
}
#endif
