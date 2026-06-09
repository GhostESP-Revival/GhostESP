import pathlib
import subprocess
import sys

from .config import load_manifest, detect_target
from .esp_idf import idf_command


def build_app(
    app_dir: str = ".",
    target: str = None,
    skip_set_target: bool = False,
) -> pathlib.Path:
    app_path = pathlib.Path(app_dir).resolve()
    if not (app_path / "CMakeLists.txt").exists():
        print(f"not an app project: {app_path}", file=sys.stderr)
        raise SystemExit(2)

    if target is None:
        try:
            manifest = load_manifest(app_path)
            target = detect_target(manifest)
        except (FileNotFoundError, ValueError):
            target = "esp32s3"

    if not skip_set_target:
        cmd, env = idf_command("set-target", target)
        subprocess.check_call(cmd, cwd=app_path, env=env)
    cmd, env = idf_command("build")
    subprocess.check_call(cmd, cwd=app_path, env=env)

    try:
        manifest = load_manifest(app_path)
        so_path = app_path / "build" / manifest["entry"]
        if so_path.exists():
            print(so_path)
            return so_path
    except (FileNotFoundError, ValueError):
        pass

    return app_path / "build"
