import json
import os
import pathlib
import shutil


REPO_MARKERS = ["plugins", "main", "components"]
SDK_RELATIVE = pathlib.Path("plugins") / "sdk"
TEMPLATES_RELATIVE = pathlib.Path("plugins") / "templates"


def _data_root() -> pathlib.Path:
    here = pathlib.Path(__file__).resolve().parent
    return here / "data"


def find_repo_root(start: pathlib.Path = None) -> pathlib.Path:
    env = os.environ.get("GHOSTBT_ROOT")
    if env:
        p = pathlib.Path(env)
        if all((p / m).is_dir() for m in REPO_MARKERS):
            return p
    cur = (start or pathlib.Path.cwd()).resolve()
    for _ in range(20):
        if all((cur / m).is_dir() for m in REPO_MARKERS):
            return cur
        parent = cur.parent
        if parent == cur:
            break
        cur = parent
    raise FileNotFoundError(
        "Cannot find GhostESP repo root. Run from inside the repo or set GHOSTBT_ROOT."
    )


def resolve_sdk_path(start: pathlib.Path = None) -> pathlib.Path:
    env = os.environ.get("GHOSTBT_SDK")
    if env:
        p = pathlib.Path(env)
        if p.is_file():
            return p
    try:
        root = find_repo_root(start)
        sdk = root / SDK_RELATIVE / "ghostesp_plugin_api.h"
        if sdk.is_file():
            return sdk
    except FileNotFoundError:
        pass
    bundled = _data_root() / "sdk" / "ghostesp_plugin_api.h"
    if bundled.is_file():
        return bundled
    raise FileNotFoundError(
        "SDK header not found. Set GHOSTBT_SDK environment variable, run from a GhostESP repo, or reinstall ghostbt."
    )


def resolve_template(name: str, start: pathlib.Path = None) -> pathlib.Path:
    try:
        repo = find_repo_root(start)
        tpl = repo / TEMPLATES_RELATIVE / name
        if tpl.is_dir():
            return tpl
    except FileNotFoundError:
        pass
    bundled = _data_root() / "templates" / name
    if bundled.is_dir():
        return bundled
    raise FileNotFoundError(f"Template not found: {name}")


def copy_bundled_sdk(dst_dir: pathlib.Path) -> pathlib.Path:
    dst = dst_dir / "sdk" / "ghostesp_plugin_api.h"
    dst.parent.mkdir(parents=True, exist_ok=True)
    src = resolve_sdk_path()
    if not dst.exists() or src.stat().st_mtime > dst.stat().st_mtime:
        shutil.copy2(src, dst)
    return dst


def load_manifest(app_dir: pathlib.Path) -> dict:
    manifest_path = app_dir / "manifest.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"manifest.json not found in {app_dir}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for field in ("id", "entry"):
        if field not in manifest:
            raise ValueError(f"manifest.json missing required field: {field}")
    return manifest


def detect_target(manifest: dict) -> str:
    return manifest.get("target", "esp32s3")
