import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

from .esp_idf import idf_command


CONFIGS_DIR_NAME = "configs"
SKIPPED_CONFIGS = {"somethingsomething", "somethingsomething2"}


def _detect_target_from_config(config_text: str) -> str:
    for line in config_text.splitlines():
        if line.startswith("CONFIG_IDF_TARGET="):
            return line.split("=", 1)[1].strip('"').strip()
        if line.startswith('CONFIG_IDF_TARGET="'):
            m = re.match(r'CONFIG_IDF_TARGET="(\w+)"', line)
            if m:
                return m.group(1)
    return "esp32s3"


def list_boards(repo_root: pathlib.Path = None) -> list[dict]:
    if repo_root is None:
        from .config import find_repo_root
        try:
            repo_root = find_repo_root()
        except FileNotFoundError:
            return []
    configs_dir = repo_root / CONFIGS_DIR_NAME
    if not configs_dir.is_dir():
        return []
    boards = []
    for f in sorted(configs_dir.iterdir()):
        if not f.is_file():
            continue
        if not f.name.startswith("sdkconfig."):
            continue
        board_id = f.name[len("sdkconfig."):]
        if board_id.startswith("default."):
            continue
        if board_id in SKIPPED_CONFIGS:
            continue
        text = f.read_text(encoding="utf-8", errors="replace")
        target = _detect_target_from_config(text)
        boards.append({"id": board_id, "target": target, "config": str(f)})
    return boards


def build_firmware(
    board: str,
    repo_root: str = None,
    skip_set_target: bool = False,
) -> pathlib.Path:
    if repo_root:
        root = pathlib.Path(repo_root).resolve()
    else:
        from .config import find_repo_root
        root = find_repo_root()

    if not (root / "CMakeLists.txt").exists() or not (root / "main").is_dir():
        print(f"not a GhostESP firmware repo: {root}", file=sys.stderr)
        raise SystemExit(2)

    config_file = root / CONFIGS_DIR_NAME / f"sdkconfig.{board}"
    if not config_file.is_file():
        available = [b["id"] for b in list_boards(root)]
        print(f"board config not found: {board}", file=sys.stderr)
        print(f"available boards: {', '.join(available)}", file=sys.stderr)
        raise SystemExit(2)

    config_text = config_file.read_text(encoding="utf-8")
    target = _detect_target_from_config(config_text)

    defaults_dst = root / "sdkconfig.defaults"
    shutil.copy2(config_file, defaults_dst)

    sdkconfig_dst = root / "sdkconfig"
    if sdkconfig_dst.exists():
        sdkconfig_dst.unlink()

    print(f"Building firmware for board: {board} (target: {target})")

    if not skip_set_target:
        cmd, env = idf_command("set-target", target)
        subprocess.check_call(cmd, cwd=str(root), env=env)

    cmd, env = idf_command("build")
    subprocess.check_call(cmd, cwd=str(root), env=env)

    build_dir = root / "build"
    bin_files = list(build_dir.glob("*.bin"))
    if bin_files:
        for b in sorted(bin_files):
            print(f"  {b}")
    print(f"Firmware built in {build_dir}")
    return build_dir
