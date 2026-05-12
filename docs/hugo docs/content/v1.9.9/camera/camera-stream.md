---
title: "Camera Stream"
description: "Live MJPEG camera stream viewable in a browser via the GhostESP web interface."
weight: 5
toc: true
---

GhostESP can serve a live camera stream over HTTP, viewable directly in a web browser at `http://ghostesp.local/camera`. The stream uses MJPEG multipart encoding for low-latency live video.

## Supported Boards

- **Seeed Studio XIAO ESP32-S3 Sense** (`xiao_esp32s3_sense`) — the only camera-enabled build currently available.
- Requires `CONFIG_HAS_CAMERA` and 8 MB PSRAM for best results.

## Quality

Unlike the motion detector (which uses QQVGA grayscale at 160x120), the camera stream uses:

- **JPEG** pixel format (hardware-encoded by the OV2640 sensor)
- **SVGA (800x600)** default resolution
- **Quality 80** JPEG compression
- **2 frame buffers in PSRAM** for smooth double-buffered capture
- Sensor tuned with slight contrast and sharpness boost

Resolution and quality can be changed at runtime without restarting the stream.

## Quick Start

### From the web interface

Open a browser connected to the GhostESP access point and navigate to:

```
http://ghostesp.local/camera
```

The page will **auto-start** the stream when loaded. Use the controls at the bottom to adjust quality, resolution, FPS, and stop/start the stream.

### From the CLI

```text
camerastream start
```

Then open `http://ghostesp.local/camera` or connect directly to the raw MJPEG stream at `http://ghostesp.local/camera/stream`.

Stop the stream with:

```text
camerastream stop
```

## Web Controls

The camera page at `/camera` includes inline controls:

- **Quality slider** (10–100) — adjusts JPEG compression quality in real time
- **Resolution dropdown** — switch between QQVGA, QVGA, VGA, SVGA, XGA, SXGA, and UXGA
- **FPS selector** — set target framerate (5–30 fps)
- **Stop / Start buttons** — toggle the stream on and off

Controls send commands through the `/api/command` endpoint, so they work the same as typing CLI commands.

## CLI Commands

- `camerastream start` — Initialize camera and enable streaming.
- `camerastream stop` — Stop streaming and release the camera.
- `camerastream status` — Show current state (running, quality, resolution, FPS, PSRAM, client count, frames served).
- `camerastream quality <1-100>` — Set JPEG quality.
- `camerastream resolution <name>` — Set resolution (see below).
- `camerastream fps <1-30>` — Set target framerate.

### Available Resolutions

| Name    | Resolution   |
|---------|-------------|
| QQVGA   | 160x120     |
| QVGA    | 320x240     |
| VGA     | 640x480     |
| SVGA    | 800x600     |
| XGA     | 1024x768    |
| SXGA    | 1280x1024   |
| UXGA    | 1600x1200   |

Higher resolutions produce larger frames and lower framerates. SVGA is a good default; UXGA works on the XIAO Sense with its 8 MB PSRAM but will be slower.

## Interaction with Motion Detector

The camera stream and motion detector **share the same camera hardware** and cannot run simultaneously:

- Starting the camera stream will automatically stop the motion detector.
- Starting the motion detector will automatically stop the camera stream.
- Whichever feature was started last takes ownership of the camera.

## Troubleshooting

- **"Stream Unavailable" in browser** — The stream hasn't started yet. The page auto-starts it, but if it fails, try `camerastream start` from the CLI.
- **"Stream busy — another client is connected"** — Only one client can view the stream at a time. Close the other tab or browser.
- **Camera init failed** — Check the camera ribbon cable connection on the XIAO Sense.
- **Low framerate at high resolutions** — Drop to VGA or SVGA for smoother streaming. UXGA is bandwidth-heavy.
- **Stream disconnects after a while** — The HTTP server has send timeouts; the page will auto-reconnect after 3 seconds.
