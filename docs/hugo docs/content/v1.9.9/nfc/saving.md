---
title: "Saving Tags"
description: "Store scanned NFC tags as Flipper-compatible files"
weight: 20
---

## Prerequisites

- A PN532 or Chameleon Ultra enabled build.
- SD card mounted with free space.

## Steps

1. **Scan a tag.** Complete the scan process so the popup shows results.
   You should see the **Save** button enabled once data is cached.

2. **Tap Save.** Select **Save** in the scan popup.
   You should see the title change to “Saving...” and the button disable while GhostESP writes the file.

3. **Wait for confirmation.** Leave the device alone until it reads "Saved!".
   You should see the title change if the save fails (for example, “Save Failed!”).
   
4. **Repeat as needed.** Saved files remain accessible even after you leave the scan popup.
   You should see the filename follow the pattern `<Model>_<UID>.nfc` in `/mnt/ghostesp/nfc/`.

## Chameleon Ultra Saves

- **Use the CLI.** After finishing `chameleon scanhf`, stay in the terminal and run `chameleon savehf <name>`. Files land in `/mnt/ghostesp/chameleon/`.
- **Name files clearly.** Pick short descriptive filenames without spaces, for example `office_door`.
- **Verify later.** Saved Chameleon dumps can be copied to a PC from your SD Card exactly like PN532 captures.

## Verify

- Browse the SD card and confirm the new file exists with the expected timestamp.
- Re-open the scan popup and use **More** to ensure details still match the saved dump.
- Optional: load the `.nfc` file in a Flipper Zero to confirm compatibility.

## Troubleshooting

- **Save button disabled.** Re-scan the tag and wait for the title to show “NFC Tag”.
- **“No SD card” error.** Check card seating and filesystem. The path `/mnt/ghostesp/` must be writeable.
- **File overwriting**: GhostESP auto-generates names and will overwrite if the same model/UID is scanned repeatedly.

## FAQ

- **Can I save after removing the tag?** Yes. Once the scan completes, the device keeps data in RAM, allowing offline saves.
- **What if the tag is MIFARE Classic?** GhostESP saves all recovered sectors and keys, so you can reopen the file later without re-running the dictionary attack.
- **Where are the files stored?** Under `/mnt/ghostesp/nfc/`. 