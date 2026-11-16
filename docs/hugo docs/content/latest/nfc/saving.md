---
title: "Saving Tags"
description: "Store scanned NFC tags as Flipper-compatible files"
weight: 20
---

Saved tags are Flipper-compatible files written to `/mnt/ghostesp/nfc/`. You need a PN532, ST25R3916, or Chameleon Ultra build and an SD card with free space.

## Steps

1. **Scan a tag.** Complete the scan so the popup shows results; **Save** is enabled once data is cached.

2. **Tap Save.** Select **Save** in the scan popup. The title changes to “Saving...” and the button disables while GhostESP writes the file.

3. **Wait for confirmation.** Leave the device alone until it reads "Saved!". A failed save changes the title (for example, “Save failed”).

4. **Repeat as needed.** Saved files remain accessible after you leave the scan popup. The filename follows `<Model>_<UID>.nfc` in `/mnt/ghostesp/nfc/`, with a few exceptions: DESFire saves as `Desfire_<UID>.nfc`, EMV as `EMV_<UID>.nfc`, and PicoPass/iCLASS as `picopass_<UID>.picopass`.

## Chameleon Ultra Saves

- **Use the CLI.** After finishing `chameleon scanhf`, stay in the terminal and run `chameleon savehf <name>`. Files land in `/mnt/ghostesp/nfc/`.
- **Name files clearly.** Pick short descriptive filenames without spaces, for example `office_door`.
- **Verify later.** Saved Chameleon dumps can be copied to a PC from your SD card exactly like PN532 captures.

## Expected result

- Re-open the scan popup and use **More** to ensure details still match the saved dump.
- Optional: load the `.nfc` file in a Flipper Zero to confirm compatibility.

## Troubleshooting

- **Save button disabled.** Re-scan the tag and wait for the title to show “NFC Tag”.
- **“No SD card” error.** Check card seating and filesystem. The path `/mnt/ghostesp/` must be writable.
- **File overwriting**: GhostESP auto-generates names and will overwrite if the same model/UID is scanned repeatedly.

## FAQ

- **Can I save after removing the tag?** Yes. Once the scan completes, the device keeps data in RAM, allowing offline saves.
- **What if the tag is MIFARE Classic?** GhostESP saves all recovered sectors and keys, so you can reopen the file later without re-running the dictionary attack.
