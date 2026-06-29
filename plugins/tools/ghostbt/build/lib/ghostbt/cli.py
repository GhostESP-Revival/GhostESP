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

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
