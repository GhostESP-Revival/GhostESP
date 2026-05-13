---
title: "Motion Detector"
description: "On-device motion detection using the onboard camera, with optional SD snapshots and Discord alerts."
weight: 10
toc: true
---

GhostESP can use an onboard camera as a lightweight motion detector. The current implementation is tuned for small ESP32 devices and runs entirely on-device using grayscale frame comparison.

## Supported Boards

- **Seeed Studio XIAO ESP32-S3 Sense** (`xiao_esp32s3_sense`) is the primary supported camera build at the moment.
- Other boards need an enabled `CONFIG_HAS_CAMERA` build plus compatible camera pin mapping.

## How It Works

- Camera input uses **QQVGA** resolution (`160x120`).
- Frames are captured in **grayscale**.
- GhostESP keeps the previous frame in memory and compares sampled pixels against the new frame.
- If the changed-pixel percentage crosses the configured trigger threshold for consecutive frames, a motion event is raised.
- A small warmup window and hysteresis reduce repeated triggers from one burst of motion.

This design keeps RAM use low while still being fast enough for continuous monitoring on the ESP32-S3.

## Quick Start

### Basic detection

```text
motion start
```

Wave a hand or move an object in front of the camera. You should see output like:

```text
[MOTION] #1 44.5% changed
```

Stop detection with:

```text
motion stop
```

### Test without SD

The detector does **not** require an SD card for motion detection itself. SD is only used when snapshot saving is enabled or when Discord image alerts are being auto-saved.

### Enable Discord alerts

```text
motion discord https://discord.com/api/webhooks/...
motion cooldown 5000
motion start
```

If image uploads are not needed, disable them to reduce bandwidth and processing time:

```text
motion image off
```

## CLI Commands

- `motion start` — Start motion detection.
- `motion stop` — Stop motion detection.
- `motion status` — Show current detector state.
- `motion threshold <1-255>` — Set per-pixel difference threshold.
- `motion interval <100-10000>` — Set frame interval in milliseconds.
- `motion percent <1-100>` — Set trigger percentage.
- `motion sample <1-32>` — Compare every Nth pixel.
- `motion snap <on|off>` — Save motion snapshots to SD card.
- `motion image <on|off>` — Attach images to Discord alerts.
- `motion discord <url|off>` — Configure or disable the Discord webhook.
- `motion webhook <url|off>` — Alias for `motion discord`.
- `motion cooldown <ms>` — Minimum time between webhook alerts.

## Recommended Settings

### Fast local testing

```text
motion threshold 30
motion percent 10
motion interval 500
motion sample 4
```

### More sensitive detection

```text
motion threshold 20
motion percent 5
motion sample 2
```

Lower values detect smaller changes, but also increase false positives from lighting flicker, auto exposure shifts, and sensor noise.

## SD and Snapshot Notes

- Motion detection itself works without SD.
- `motion snap on` writes JPEG snapshots to `/mnt/ghostesp/captures`.
- If **Discord webhook alerts** are enabled and `motion image on` is active, images sent to Discord are also saved automatically when SD storage is available.
- Discord alerts can still work without SD because images are uploaded directly from the frame buffer.

## Troubleshooting

- **`Camera init failed: ESP_ERR_NOT_FOUND`** — Camera ribbon cable or pin mapping issue.
- **Detector starts but does not trigger** — Lower `motion threshold` or `motion percent`.
- **Too many triggers** — Raise `motion percent`, raise `motion threshold`, or increase `motion cooldown` for webhooks.
- **Discord TLS/certificate errors** — Ensure the device has Wi-Fi access and a valid time sync before the webhook attempt.
- **Frame buffer overflow logs (`FB-OVF`)** — Usually indicates the camera or processing path is under pressure; reduce extra work such as image uploads while testing.
