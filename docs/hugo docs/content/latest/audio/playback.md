---
title: "MP3 Playback"
description: "Play MP3 files over GhostLink using the Audio app"
weight: 5
toc: true
---

The Audio app plays MP3 files stored on the SD card and streams them over GhostLink between the two chips in **The Banshee**.

## Requirements

- **The Banshee** hardware. Its two-chip audio setup uses the `somethingsomething` and `somethingsomething2` builds.
- A GhostLink connection between the Banshee control/display chip and audio receiver chip.
- MP3 files on the SD card in `/audio`.
- MP3 bitrate under `200kbps`. Higher bitrate files are rejected with an on-device toast.
- For headphone output, the TLV320DAC3100 route is detected once before playback starts.

## File Setup

Create this folder on the SD card:

```text
/audio
```

Copy `.mp3` files into that folder. The Audio app scans the folder when it opens and lists the tracks alphabetically.

Recommended encoding:

- MP3, constant bitrate when possible.
- `128kbps` or `160kbps` for best reliability.
- Under `200kbps`; `200kbps` and higher is not accepted.
- Standard sample rates such as `44.1kHz`.

## Opening the App

On the device UI, open:

```text
Apps -> Audio
```

If no MP3 files are found, the app shows a toast asking for files in `/audio`.

## Controls

- Tap a track row to start playback.
- Use previous/next to switch tracks.
- Use play/pause to pause or resume playback.
- Encoder rotation adjusts volume on supported hardware.
- Touch-drag inside the track list scrolls the list.

The currently selected row is marked `SEL`. The active track is marked `PLAY`; paused playback is marked `PAUSE`.

## Playback Behavior

Audio data is sent over GhostLink as a paced stream. The receiver reports its audio buffer fill level and playback time back to the sender so the stream can adapt to real receiver conditions.

The app displays:

- Current track name.
- Elapsed and estimated total time.
- Playback state.
- Detected bitrate.
- Receiver buffer percentage when feedback is available.
- Volume percentage.

Pause is immediate: the receiver discards buffered audio and resets output so it does not continue playing through the queued buffer.

## GhostLink Commands

These commands are used internally by the Audio app and can be useful for debugging:

```text
audio start
audio stop
audio pause
audio flush
audio state <fill_bytes> <capacity_bytes> <played_ms>
```

`audio state` is sent by the receiver to report buffer fill and playback clock. The sender uses it for progress display and adaptive pacing.

## Troubleshooting

- **Audio app is missing:** Audio playback is currently Banshee-only. Make sure both Banshee chips are flashed with the correct paired builds.
- **No files shown:** Check that the SD card contains `.mp3` files in `/audio`.
- **Bitrate toast appears:** Re-encode the file below `200kbps`; `128kbps` or `160kbps` is recommended.
- **Playback underruns or stutters:** Try a lower bitrate file and confirm GhostLink is connected with no other streaming app active.
- **No headphone output:** Reconnect headphones before starting playback so one-shot route detection can select the right output path.
- **Progress looks wrong:** Some VBR files do not expose accurate duration data. CBR files produce more stable estimates.
