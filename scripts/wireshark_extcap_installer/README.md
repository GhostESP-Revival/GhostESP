# GhostESP Wireshark Extcap Installer

Installer package for GhostESP Wireshark integration.

## For Users - Quick Install

### Option 1: Use Pre-built Installer (Easiest)
1. Download `GhostESP_Wireshark_Installer.exe` from the releases
2. Run it
3. Click "Install"
4. Restart Wireshark
5. Done!

### Option 2: Run Python Installer
```bash
python installer_gui.py
```

## For Developers - Building the Installer

### Requirements
- Python 3.7+
- PyInstaller (auto-installed by build script)

### Build Instructions

**Windows:**
```bash
python build_installer.py
```

**Output:**
- `dist/GhostESP_Wireshark_Installer.exe` - Standalone executable

### Build Options

**Clean build artifacts:**
```bash
python build_installer.py --clean
```

**Manual PyInstaller build:**
```bash
pyinstaller --onefile --windowed --name GhostESP_Wireshark_Installer ^
  --add-data "ghostesp_extcap.py;." ^
  --add-data "ghostesp_extcap.bat;." ^
  installer_gui.py
```

## What Gets Installed

The installer copies these files to Wireshark's extcap folder:
- `ghostesp_extcap.py` - Main extcap script
- `ghostesp_extcap.bat` - Windows batch wrapper

**Default Installation Path:**
- `%APPDATA%\Wireshark\extcap\` (User-level, no admin required)

**Alternative Path:**
- `C:\Program Files\Wireshark\extcap\` (System-level, requires admin)

## Features

### GUI Installer Features
- ✓ Automatic dependency checking (Python, PySerial, Wireshark)
- ✓ One-click installation
- ✓ Uninstall functionality
- ✓ Installation log with detailed status
- ✓ No admin rights required (user-level install)

### Extcap Features
- ✓ Native Wireshark interface integration
- ✓ Auto-detect COM ports
- ✓ Configurable baud rate
- ✓ WiFi and BLE capture support
- ✓ Real-time PCAP streaming

## Troubleshooting

**Installer won't run:**
- Ensure Python 3.7+ is installed
- Run from command prompt to see errors

**GhostESP not showing in Wireshark:**
- Restart Wireshark completely
- Check Wireshark → Help → About → Folders → Extcap path
- Verify files exist in extcap folder

**No COM ports in dropdown:**
- Connect your GhostESP device
- Refresh Wireshark interfaces (F5)
- Check Device Manager for COM port

## File Structure

```
wireshark_extcap_installer/
├── installer_gui.py          # GUI installer (run this)
├── build_installer.py         # Build script for exe
├── ghostesp_extcap.py        # Extcap script
├── ghostesp_extcap.bat       # Windows wrapper
├── README.md                  # This file
└── dist/                      # Built executables (after build)
    └── GhostESP_Wireshark_Installer.exe
```

## Distribution

To distribute the installer:
1. Build the executable: `python build_installer.py`
2. Upload `dist/GhostESP_Wireshark_Installer.exe` to releases
3. Users download and run the exe - no Python required!

## Support

For issues or questions:
- GitHub: https://github.com/jaylikesbunda/Ghost_ESP
- Documentation: See parent docs/ folder
