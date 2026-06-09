import argparse
import sys

from . import __version__


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="gbt",
        description="Ghost Build Tool - build and package GhostESP native SD apps",
    )
    parser.add_argument("--version", action="version", version=f"gbt {__version__}")
    parser.add_argument("-v", "--verbose", action="store_true")

    sub = parser.add_subparsers(dest="command")

    p_create = sub.add_parser("create", help="Scaffold a new app from template")
    p_create.add_argument("app_id", help="Safe app id, e.g. my_tool")
    p_create.add_argument("--name", help="Display name (default: title-cased app_id)")
    p_create.add_argument("--template", default="basic_app", help="Template name under plugins/templates/")
    p_create.add_argument("--out", default=".", help="Output parent directory (default: current dir)")

    p_build = sub.add_parser("build", help="Build an app project")
    p_build.add_argument("app_dir", nargs="?", default=".", help="App project directory (default: .)")
    p_build.add_argument("--target", default=None, help="ESP-IDF target (default: from manifest or esp32s3)")
    p_build.add_argument("--skip-set-target", action="store_true", help="Skip idf.py set-target step")

    p_package = sub.add_parser("package", help="Package a built app")
    p_package.add_argument("app_dir", nargs="?", default=".", help="App project directory (default: .)")
    p_package.add_argument("--out", default=None, help="Output dist directory")
    p_package.add_argument("--gapp", action="store_true", help="Also create a .gapp archive")

    p_dist = sub.add_parser("dist", help="Build and package in one step")
    p_dist.add_argument("app_dir", nargs="?", default=".", help="App project directory (default: .)")
    p_dist.add_argument("--target", default=None, help="ESP-IDF target (default: from manifest or esp32s3)")
    p_dist.add_argument("--out", default=None, help="Output dist directory")
    p_dist.add_argument("--gapp", action="store_true", help="Also create a .gapp archive")

    p_setup = sub.add_parser("setup", help="Install or configure ESP-IDF toolchain")
    p_setup.add_argument("--target", nargs="+", default=["esp32s3"], help="ESP-IDF targets to install (default: esp32s3)")
    p_setup.add_argument("--idf-version", default=None, help="ESP-IDF version (default: v6.0.1)")
    p_setup.add_argument("--install-dir", default=None, help="Custom install directory (default: ~/.ghostbt/esp-idf)")

    p_boards = sub.add_parser("boards", help="List available firmware board configs")

    p_fw = sub.add_parser("firmware", help="Build GhostESP firmware for a board")
    p_fw.add_argument("board", help="Board config name (e.g. cardputer, ghostboard). Use 'gbt boards' to list.")
    p_fw.add_argument("--repo", default=None, help="Path to GhostESP repo (default: auto-detect)")
    p_fw.add_argument("--skip-set-target", action="store_true", help="Skip idf.py set-target step")

    p_flash = sub.add_parser("flash", help="Flash firmware or app to device")
    p_flash.add_argument("target", nargs="?", default="firmware", choices=["firmware", "app"],
                         help="What to flash: firmware (default) or app")
    p_flash.add_argument("--port", "-p", default=None, help="Serial port (auto-detected if omitted)")
    p_flash.add_argument("--baud", "-b", type=int, default=None, help="Baud rate (default: 460800)")
    p_flash.add_argument("--board", default=None, help="Board config (builds firmware if needed)")
    p_flash.add_argument("--app-dir", default=None, help="App directory (for 'app' target)")
    p_flash.add_argument("--build-dir", default=None, help="Custom build directory")
    p_flash.add_argument("--verify", action="store_true", help="Verify after writing")
    p_flash.add_argument("--erase", action="store_true", help="Erase entire flash before writing")
    p_flash.add_argument("--monitor", "-m", action="store_true", help="Open serial monitor after flashing")

    p_mon = sub.add_parser("monitor", help="Open serial monitor")
    p_mon.add_argument("--port", "-p", default=None, help="Serial port (auto-detected if omitted)")
    p_mon.add_argument("--baud", "-b", type=int, default=115200, help="Baud rate (default: 115200)")

    p_ports = sub.add_parser("ports", help="List available serial ports")

    p_asset = sub.add_parser("asset", help="Create GhostESP asset pack images and bundles")
    asset_sub = p_asset.add_subparsers(dest="asset_command")

    p_asset_image = asset_sub.add_parser("image", help="Convert a PNG into a GhostESP .gimg image")
    p_asset_image.add_argument("png", help="Source PNG")
    p_asset_image.add_argument("--out", required=True, help="Output .gimg path")
    p_asset_image.add_argument("--width", type=int, required=True, help="Output width in pixels")
    p_asset_image.add_argument("--height", type=int, required=True, help="Output height in pixels")
    p_asset_image.add_argument("--format", choices=["rgb565", "rgb565a8", "indexed_4bpp"], default="rgb565a8",
                               help="Pixel format (default: rgb565a8)")
    p_asset_image.add_argument("--no-compress", action="store_true", help="Store raw payload without deflate compression")

    p_asset_pack = asset_sub.add_parser("pack", help="Build an SD-ready asset pack from a source manifest")
    p_asset_pack.add_argument("pack_dir", nargs="?", default=".", help="Asset pack source directory (default: .)")
    p_asset_pack.add_argument("--out", default=None, help="Output dist directory")
    p_asset_pack.add_argument("--archive", action="store_true", help="Also create a .gtheme zip archive")
    p_asset_pack.add_argument("--compress", action="store_true", help="Deflate .gimg payloads (not recommended for firmware runtime themes)")

    args = parser.parse_args(argv)

    if not args.command:
        parser.print_help()
        return 0

    if args.command == "create":
        from .create import create_app
        create_app(
            app_id=args.app_id,
            name=args.name,
            template=args.template,
            out_dir=args.out,
        )
    elif args.command == "build":
        from .build import build_app
        build_app(
            app_dir=args.app_dir,
            target=args.target,
            skip_set_target=args.skip_set_target,
        )
    elif args.command == "package":
        from .package import package_app
        package_app(
            app_dir=args.app_dir,
            out=args.out,
            make_gapp=args.gapp,
        )
    elif args.command == "dist":
        from .dist import dist_app
        dist_app(
            app_dir=args.app_dir,
            target=args.target,
            out=args.out,
            make_gapp=args.gapp,
        )
    elif args.command == "setup":
        from .esp_idf import setup_idf
        setup_idf(
            targets=args.target,
            idf_version=args.idf_version,
            install_dir=args.install_dir,
        )
    elif args.command == "boards":
        from .firmware import list_boards
        boards = list_boards()
        if not boards:
            print("No board configs found. Run from inside the GhostESP repo.")
            return 1
        print(f"{'Board':<30} {'Target':<12}")
        print("-" * 42)
        for b in boards:
            print(f"{b['id']:<30} {b['target']:<12}")
        print(f"\n{len(boards)} boards available")
    elif args.command == "firmware":
        from .firmware import build_firmware
        build_firmware(
            board=args.board,
            repo_root=args.repo,
            skip_set_target=args.skip_set_target,
        )
    elif args.command == "flash":
        from .flash import flash_firmware, flash_app, monitor
        if args.target == "app":
            if not args.app_dir:
                print("--app-dir is required for 'app' target", file=sys.stderr)
                return 2
            used_port = flash_app(
                app_dir=args.app_dir,
                port=args.port,
                baud=args.baud,
            )
        else:
            used_port = flash_firmware(
                port=args.port,
                baud=args.baud,
                board=args.board,
                build_dir=args.build_dir,
                verify=args.verify,
                erase_flash=args.erase,
            )
        if args.monitor and used_port:
            monitor(port=used_port)
    elif args.command == "monitor":
        from .flash import monitor
        monitor(port=args.port, baud=args.baud)
    elif args.command == "ports":
        from .flash import list_serial_ports
        ports = list_serial_ports()
        if not ports:
            print("No serial ports found.")
            return 1
        print(f"{'Port':<20} {'Description'}")
        print("-" * 60)
        for p in ports:
            print(f"{p['device']:<20} {p['description']}")
        print(f"\n{len(ports)} port(s) available")
    elif args.command == "asset":
        if not args.asset_command:
            p_asset.print_help()
            return 0
        from .asset import make_asset_image, make_asset_pack
        if args.asset_command == "image":
            make_asset_image(
                src=args.png,
                out=args.out,
                width=args.width,
                height=args.height,
                fmt=args.format,
                compress=not args.no_compress,
            )
        elif args.asset_command == "pack":
            make_asset_pack(
                pack_dir=args.pack_dir,
                out=args.out,
                archive=args.archive,
                compress=args.compress,
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
