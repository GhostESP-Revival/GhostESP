---
title: "Writing NTAG"
description: "Program NTAG and Ultralight cards from saved dumps"
weight: 30
---

## Prerequisites

- An NTAG `.nfc` file on the SD card under `/mnt/ghostesp/nfc/`.
- NTAG21x or Ultralight tag compatible with the captured image size (NTAG213/215/216 page counts).
- A PN532 or Chameleon Ultra enabled build with a connected PN532 module or Chameleon Ultra.

## Steps

1. **Open NFC → Write.** From the NFC menu, choose **Write**.
   You should see a list of `.nfc` files detected on the SD card.

2. **Select an image.** Tap the file you want to program.
   You should see a popup summarizing the tag model, UID (if present), and total pages.

3. **Start the write.** Choose **Write** in the popup.
   You should see the status change to “Present tag to write...” while GhostESP waits for a blank card.

4. **Present the blank tag.** Hold the target NTAG on the antenna until the title updates.
   You should see “Writing… X%” as each page is written, finishing at “Write complete”.

## Chameleon Ultra Writing

- **No remote writes.** GhostESP cannot program tags through the Chameleon Ultra; use the PN532 workflow above.

## Verify

- Re-scan the card using the **Scan NFC Tags** guide to confirm the UID (if applicable) and data match the source image.
- Check the popup for success messages; any failed page write will trigger an error log with the page number.
- Optional: load the card in an external reader to confirm the NDEF payload or application data.

## Troubleshooting

- **File list empty.** Ensure `.nfc` files exist under `/mnt/ghostesp/nfc/` and the SD card is mounted.

- **“Present tag to write...” never changes.** The PN532 cannot see the card. Re-seat the antenna or try a different blank tag.

- **Write stops mid-way:** Remove the tag, power-cycle GhostESP, and retry. Failures often mean the card's capacity doesn't match the image.

## FAQ

- **Can I clone MIFARE Classic with this flow?** No, the Write flow is limited to NTAG/Ultralight dumps today.
- **Do I need to keep holding the card after 100%?** Hold it until “Write complete” appears; the device finalizes status after the last page.
- **Will the UID be overwritten?** NTAG UIDs are read-only. The saved UID is used for naming and verification, but the physical card keeps its factory UID.
