import os
import pathlib
import subprocess
import sys

from .config import load_manifest
from .esp_idf import get_idf_env, _find_idf_python, get_idf_path


def _find_esptool() -> list[str] | None:
    idf_path = get_idf_path()
    if idf_path:
        idf_python, _ = _find_idf_python(idf_path)
        if idf_python:
            return [idf_python, "-m", "esptool"]
    from shutil import which
    if which("esptool"):
        return ["esptool"]
    if which("esptool.py"):
        return ["esptool.py"]
    return None


def _find_idf_monitor() -> list[str] | None:
    idf_path = get_idf_path()
    if idf_path:
        idf_python, _ = _find_idf_python(idf_path)
        if idf_python:
            idf_monitor = str(idf_path / "tools" / "idf_monitor.py")
            if pathlib.Path(idf_monitor).exists():
                return [idf_python, idf_monitor]
    return None


def _detect_baud_from_flash_args(build_dir: pathlib.Path) -> int:
    flash_args = build_dir / "flash_args"
    if flash_args.exists():
        text = flash_args.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if "--baud" in line:
                parts = line.split()
                for i, p in enumerate(parts):
                    if p == "--baud" and i + 1 < len(parts):
                        try:
                            return int(parts[i + 1])
                        except ValueError:
                            pass
    return 460800


def list_serial_ports() -> list[dict]:
    try:
        import serial.tools.list_ports
        ports = serial.tools.list_ports.comports()
        return [
            {"device": p.device, "description": p.description, "hwid": p.hwid}
            for p in sorted(ports, key=lambda p: p.device)
        ]
    except ImportError:
        pass
    idf_path = get_idf_path()
    if idf_path:
        idf_python, _ = _find_idf_python(idf_path)
        if idf_python:
            try:
                result = subprocess.run(
                    [idf_python, "-m", "serial.tools.list_ports"],
                    capture_output=True, text=True, timeout=10,
                )
                if result.returncode == 0 and result.stdout.strip():
                    return [
                        {"device": line.strip(), "description": "", "hwid": ""}
                        for line in result.stdout.strip().splitlines()
                        if line.strip() and "ports found" not in line.lower()
                    ]
            except (subprocess.TimeoutExpired, FileNotFoundError):
                pass
    return []


def flash_firmware(
    port: str = None,
    baud: int = None,
    board: str = None,
    repo_root: str = None,
    build_dir: str = None,
    verify: bool = False,
    erase_flash: bool = False,
) -> str:
    """Flash firmware. Returns the serial port used."""
    if build_dir:
        bdir = pathlib.Path(build_dir).resolve()
    else:
        if repo_root:
            root = pathlib.Path(repo_root).resolve()
        else:
            from .config import find_repo_root
            root = find_repo_root()
        bdir = root / "build"

    if not bdir.is_dir():
        print(f"build directory not found: {bdir}", file=sys.stderr)
        print("Run 'gbt firmware <board>' first.", file=sys.stderr)
        raise SystemExit(2)

    flash_args_file = bdir / "flash_args"
    bootloader = bdir / "bootloader" / "bootloader.bin"
    partition_table = bdir / "partition_table" / "partition-table.bin"

    if board:
        from .firmware import build_firmware
        build_firmware(board, repo_root=str(bdir.parent) if bdir.name == "build" else None)

    bin_files = list(bdir.glob("*.bin"))
    app_bin = None
    for f in sorted(bin_files):
        if f.name not in ("bootloader.bin", "partition-table.bin"):
            app_bin = f
            break

    if not app_bin or not bootloader.exists() or not partition_table.exists():
        print("firmware binaries not found in build dir.", file=sys.stderr)
        print("Run 'gbt firmware <board>' first.", file=sys.stderr)
        raise SystemExit(2)

    target_chip = "esp32s3"
    proj_desc = bdir / "project_description.json"
    if proj_desc.exists():
        try:
            import json
            desc = json.loads(proj_desc.read_text(encoding="utf-8"))
            target_chip = desc.get("target", target_chip)
        except (json.JSONDecodeError, OSError):
            pass

    esptool_cmd = _find_esptool()
    if not esptool_cmd:
        print("esptool not found. Run 'gbt setup' to install ESP-IDF.", file=sys.stderr)
        raise SystemExit(2)

    env = get_idf_env() or os.environ.copy()

    if not port:
        ports = list_serial_ports()
        if len(ports) == 0:
            print("No serial ports found. Connect your device and try again.", file=sys.stderr)
            raise SystemExit(2)
        if len(ports) == 1:
            port = ports[0]["device"]
            print(f"Auto-detected port: {port}")
        else:
            print("Multiple serial ports found:")
            for i, p in enumerate(ports):
                desc = f" - {p['description']}" if p["description"] else ""
                print(f"  [{i + 1}] {p['device']}{desc}")
            try:
                choice = input("Select port (number or path): ").strip()
                if choice.isdigit() and 1 <= int(choice) <= len(ports):
                    port = ports[int(choice) - 1]["device"]
                else:
                    port = choice
            except (EOFError, KeyboardInterrupt):
                print("\nAborted.")
                raise SystemExit(1)

    effective_baud = baud or _detect_baud_from_flash_args(bdir)

    if erase_flash:
        print(f"\nErasing flash on {port}...")
        erase_cmd = esptool_cmd + [
            "--chip", target_chip,
            "--port", port,
            "--baud", str(effective_baud),
            "erase-flash",
        ]
        subprocess.check_call(erase_cmd, env=env)

    cmd = esptool_cmd + [
        "--chip", target_chip,
        "--port", port,
        "--baud", str(effective_baud),
        "--before", "default-reset",
        "--after", "hard-reset",
        "write-flash",
    ]
    if verify:
        cmd.append("--verify")

    if flash_args_file.exists():
        cmd.extend(["@", str(flash_args_file)])
    else:
        cmd.extend([
            "0x0", str(bootloader),
            "0x8000", str(partition_table),
            "0x10000", str(app_bin),
        ])

    print(f"\nFlashing {app_bin.name} to {port} at {effective_baud} baud...")
    subprocess.check_call(cmd, env=env)
    print(f"\nFlash complete. Device should reboot.")
    return port


def flash_app(
    app_dir: str,
    port: str = None,
    baud: int = None,
) -> str:
    """Print instructions for loading an app via SD card. Returns the serial port used."""

    app_path = pathlib.Path(app_dir).resolve()
    manifest = load_manifest(app_path)
    entry = manifest["entry"]

    so_path = app_path / "build" / entry
    if not so_path.exists():
        print(f"app binary not found: {so_path}", file=sys.stderr)
        print("Run 'gbt build <app_dir>' first.", file=sys.stderr)
        raise SystemExit(2)

    ports = list_serial_ports()
    if not port:
        if len(ports) == 0:
            print("No serial ports found. Connect your device and try again.", file=sys.stderr)
            raise SystemExit(2)
        if len(ports) == 1:
            port = ports[0]["device"]
        else:
            print("Multiple serial ports found:")
            for i, p in enumerate(ports):
                desc = f" - {p['description']}" if p["description"] else ""
                print(f"  [{i + 1}] {p['device']}{desc}")
            try:
                choice = input("Select port (number or path): ").strip()
                if choice.isdigit() and 1 <= int(choice) <= len(ports):
                    port = ports[int(choice) - 1]["device"]
                else:
                    port = choice
            except (EOFError, KeyboardInterrupt):
                print("\nAborted.")
                raise SystemExit(1)

    print(f"\nTo load app '{manifest['id']}' on your device:")
    print(f"  1. Copy manifest.json and {entry} to /ghostesp/apps/{manifest['id']}/ on the SD card")
    print(f"  2. Or copy the .gapp file to /ghostesp/apps/ or /ghostesp/packages/ on the SD")
    print(f"\n  App files:")
    print(f"    {app_path / 'manifest.json'}")
    print(f"    {so_path}")
    print(f"\n  Or package first:")
    print(f"    gbt package --gapp {app_path}")
    return port


def monitor(
    port: str = None,
    baud: int = 115200,
) -> None:
    idf_monitor_cmd = _find_idf_monitor()
    env = get_idf_env() or os.environ.copy()

    if not port:
        ports = list_serial_ports()
        if len(ports) == 0:
            print("No serial ports found. Connect your device.", file=sys.stderr)
            raise SystemExit(2)
        if len(ports) == 1:
            port = ports[0]["device"]
        else:
            print("Multiple serial ports found:")
            for i, p in enumerate(ports):
                desc = f" - {p['description']}" if p["description"] else ""
                print(f"  [{i + 1}] {p['device']}{desc}")
            try:
                choice = input("Select port (number or path): ").strip()
                if choice.isdigit() and 1 <= int(choice) <= len(ports):
                    port = ports[int(choice) - 1]["device"]
                else:
                    port = choice
            except (EOFError, KeyboardInterrupt):
                print("\nAborted.")
                raise SystemExit(1)

    elf_path = None
    from .config import find_repo_root
    try:
        root = find_repo_root()
        build_dir = root / "build"
        elf_files = list(build_dir.glob("*.elf"))
        if elf_files:
            elf_path = str(elf_files[0])
    except FileNotFoundError:
        pass

    if idf_monitor_cmd:
        cmd = idf_monitor_cmd + ["--port", port, "--baud", str(baud)]
        if elf_path:
            cmd.append(elf_path)
        print(f"Opening serial monitor on {port} at {baud} baud...")
        print("Press Ctrl+] to exit.\n")
        try:
            subprocess.check_call(cmd, env=env)
        except KeyboardInterrupt:
            pass
        return

    try:
        import serial
    except ImportError:
        print("No serial monitor available.", file=sys.stderr)
        print("Install pyserial: pip install pyserial", file=sys.stderr)
        raise SystemExit(2)

    print(f"Opening raw serial monitor on {port} at {baud} baud...")
    print("Press Ctrl+C to exit.\n")

    ser = serial.Serial(port, baudrate=baud, timeout=0.1)
    try:
        while True:
            data = ser.read(4096)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
            try:
                if os.name == "nt":
                    import msvcrt
                    if msvcrt.kbhit():
                        ch = msvcrt.getch()
                        ser.write(ch)
                else:
                    import select
                    if sys.stdin in select.select([sys.stdin], [], [], 0)[0]:
                        line = sys.stdin.readline()
                        if line:
                            ser.write(line.encode())
            except (EOFError, KeyboardInterrupt):
                break
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print("\nMonitor closed.")
