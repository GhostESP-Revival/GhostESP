#!/usr/bin/env python3
import argparse
import pathlib
import shutil
import subprocess
import sys


def idf_command(*args: str) -> list[str]:
    idf = shutil.which("idf.py")
    if idf and pathlib.Path(idf).suffix.lower() == ".py":
        return [sys.executable, idf, *args]
    return [idf or "idf.py", *args]


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a GhostESP native SD app")
    parser.add_argument("app_dir", help="App project directory")
    parser.add_argument("--target", default="esp32s3", help="ESP-IDF target")
    parser.add_argument("--skip-set-target", action="store_true")
    args = parser.parse_args()

    app_dir = pathlib.Path(args.app_dir).resolve()
    if not (app_dir / "CMakeLists.txt").exists():
        print(f"not an app project: {app_dir}", file=sys.stderr)
        return 2

    if not args.skip_set_target:
        subprocess.check_call(idf_command("set-target", args.target), cwd=app_dir)
    subprocess.check_call(idf_command("build"), cwd=app_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
