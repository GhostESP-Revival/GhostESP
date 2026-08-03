---
title: "Waterfall Spectrum Analyzer"
description: "Use the SubGHz waterfall to visualize CC1101 RSSI activity across common RF bands."
keywords: ["SubGHz", "waterfall", "spectrum analyzer", "CC1101", "RSSI", "RF"]
weight: 28
---

The SubGHz waterfall spectrum analyzer visualizes RF activity from a CC1101 radio using real RSSI measurements. It is designed to show where signal energy is present across common SubGHz bands without using fake FFT-derived output.

## What It Shows

The waterfall scans and composites five common bands:

- `315 MHz`
- `390 MHz`
- `433.92 MHz`
- `868.35 MHz`
- `915 MHz`

Each band contributes 64 RSSI bins. A full waterfall row is built from all five bands, producing 320 real RF measurement bins per sweep.

The display is laid out left to right as:

```text
315 | 390 | 433 | 868 | 915
```

Hard separators are shown between bands because the frequency gaps between those bands are not continuously scanned. This avoids implying that unmeasured spectrum between bands has been sampled.

## How It Works

For each band, GhostESP tunes the CC1101 to the band frequency and scans 64 channel offsets using the CC1101 channel register. Each bin is converted from the CC1101 RSSI register into a 0-100 display level.

Once all five bands have been received, the display builds one stable composite row and scrolls it into the waterfall. This keeps every visible row using the same frequency layout.

## Remote Mode

When using a separate GhostLink peer with the CC1101 attached, the peer performs the RF scan and streams the result to the display device.

Each streamed waterfall packet includes:

- Protocol version
- Packet type
- Total bin count
- Band index
- Sequence number
- Chunk offset and length
- RSSI bin data

The display device reassembles the streamed chunks, stores the latest line for each band, and renders a new composite row only after all five bands are available.

## Peak Readout

The waterfall includes a compact peak readout:

```text
Peak: 433.92 MHz ch 18 Lv 72
```

This shows the strongest bin in the latest full composite:

- Band frequency
- Channel/bin index inside that band
- RSSI display level from 0-100

## Palette And Smoothing

The palette is tuned for RF visualization:

```text
black -> deep blue -> purple -> magenta -> orange -> white
```

Low-level noise is kept dark, while stronger signals move into brighter colors. The waterfall uses percentile-based noise-floor and ceiling tracking so a single spike does not cause the whole display to flicker or re-scale aggressively.

## Notes And Limits

- The CC1101 is not an SDR. It does not provide raw IQ samples.
- The waterfall uses RSSI measurements, not FFT output.
- Band separators indicate real gaps in coverage between scanned bands.
- Signals outside the configured scan bands will not appear.
- Results depend on antenna quality, board layout, local RF noise, and CC1101 configuration.

## Responsible Use

The waterfall is a passive RF visualization tool. Use it for education, debugging, authorized testing, and understanding local RF environments. It is not a jamming or interference feature.
