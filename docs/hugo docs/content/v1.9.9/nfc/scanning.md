---
title: "Scanning"
description: "Read GhostESP-compatible NFC tags and understand the on-screen feedback"
weight: 10
---

## Prerequisites

- GhostESP firmware compiled with NFC support and a connected PN532 module or chameleon ultra.
- (Optional) SD card if you plan to save tag dumps.

## Steps

1. **Open Scan.** Pick **Scan** in the NFC menu; the popup shows “Scanning NFC...” while the PN532 spins up.

2. **Bring in a tag.** Place it flat on the antenna. The UID, ATQA, and SAK appear immediately; Classic cards flip the title to “Bruteforcing keys… 0%”.

3. **Hold steady.** Keep the tag still while reads finish. If the tag slips away, the UI pauses and prompts you to return it before resuming.

4. **Check output.** Review the summary, toggle **More** for more info, and use **Save** to write the dump once “NFC Tag” is displayed.

## Auto-Pause Detection

- The scanner polls for the tag continuously. If it leaves the field mid-scan, the UI enters a paused state, disables brute-force attempts, and resumes automatically once the same UID is back in range.

## MIFARE Classic Flow

- **Dictionary attack order:** GhostESP tries user keys, then common keys, then the Flipper dictionary.

- **Caching behavior.** Once a sector unlocks, its blocks and both Key A/Key B values are cached. The title shifts to “Reading sectors...” during the copy. Successful keys are appended back to the user dictionary on the SD card.

- **Magic backdoor tags.** If the card supports the classic backdoor sequence, GhostESP logs the detection and can skip sector authentication, pulling data directly.

- **Skip option:** Tap **Skip** to bypass dictionaries when you only need public sectors.

- **After the scan.** The more summary lists recovered sectors and keys as well as any detected NDEF data.

## NTAG / Ultralight Flow

- **Immediate reads.** NTAG21x and Ultralight tags are readable without keys, so the title stays “Scanning NFC...” until the UID appears, then flips to “NFC Tag”.

- **Page sweep.** The reader streams all user pages, signature bytes, and counters if present. Progress is shown via the page counter in the popup body.

- **NDEF parsing.** Detected TLVs are decoded into text, URI, or custom payload summaries. Tap **More** to see the raw TLV breakdown.

- **Caching and saves.** All pages remain in RAM for the current session; saving writes the entire image to `/mnt/ghostesp/nfc/<Model>_<UID>.nfc` for later writes.

- **Verification.** Re-scan immediately after to confirm the data matches or to check the signature for authenticity.

## Chameleon Ultra Scanning

- **Connect first.** Complete the [Chameleon Ultra setup]({{< relref "chameleon-ultra.md" >}}) so GhostESP is paired over BLE.
- **Switch to reader.** Run `chameleon reader` in the CLI; the terminal confirms the device is ready to scan.
- **Start HF scans.** Use `chameleon scanhf` while holding the tag near the Chameleon Ultra antenna. The CLI mirrors the familiar popup summaries, including brute-force percentages for MIFARE Classic cards.
- **Start LF scans.** Use `chameleon scanlf` (or `scanlfall` to sweep profiles) for low-frequency tags; results appear in the CLI and the on-device terminal view.
- **Reuse cached data.** Once a scan finishes, you can proceed directly to the save flow without rescanning on the PN532.

## Verify

- Confirm the tag type and UID shown on-screen match the physical tag you scanned.
- For Mifare Classic cards, check the listed sectors to see how many keys were recovered.
- If you saved the capture, verify a new `<Model>_<UID>.nfc` file was created under `/mnt/ghostesp/nfc/`.

## Troubleshooting

- **No change from “Scanning NFC...”.** Re-seat the tag and verify PN532 wiring; try another tag to rule out hardware issues.

- **Stuck on “Bruteforcing keys… 0%”.** GhostESP is testing dictionaries. Use the **Skip** button if you only need publicly readable blocks.

- **UID reads but data is empty.** The card may be write-protected or needs a key not present in your dictionaries. You can add it to your user dictionary in `mnt/ghostesp/nfc/mfc_user_keys.nfc` and then try rescanning.

## FAQ

- **Which keys does GhostESP try?** Your User Keys list runs first, followed by bundled common keys and then the Flipper Zero Mifare Classic dictionary.

- **What do the sector labels mean?** Mifare Classic memory is split into numbered sectors, each protected by Key A and Key B. A listed sector means at least one key unlocked it during the scan.

- **Can I scan other tag families?** NTAG21x and Ultralight tags read without needing a brute force; they show their NDEF TLVs immediately after the UID appears.
