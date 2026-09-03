# GhostESP App

This is a standalone GhostESP native SD app created by `gbt`. Update the author, description, permissions, and hardware requirements in `manifest.json`, then add an open-source `LICENSE` file before publishing it.

## Build

```powershell
gbt setup --target esp32s3
gbt dist . --target esp32s3 --gapp
```

If a Windows build reports that `ESP_ROM_ELF_DIR` is undefined, run `. "$HOME\.ghostbt\esp-idf\export.ps1"` in that PowerShell session and build again.

Copy the generated `.gapp` from `dist/` to `ghostesp/apps/` on the SD card and reboot the device.

Keep the bundled files under `sdk/` in source control so catalog CI can build the same SDK version.
