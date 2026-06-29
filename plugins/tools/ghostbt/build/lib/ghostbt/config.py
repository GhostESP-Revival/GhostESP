import json
import pathlib


REPO_MARKERS = ["plugins", "main", "components"]
SDK_RELATIVE = pathlib.Path("plugins") / "sdk"
TEMPLATES_RELATIVE = pathlib.Path("plugins") / "templates"


def find_repo_root(start: pathlib.Path = None) -> pathlib.Path:
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
    import os
    env = os.environ.get("GHOSTBT_SDK")
    if env:
        p = pathlib.Path(env)
        if p.is_file():
            return p
    root = find_repo_root(start)
    sdk = root / SDK_RELATIVE / "ghostesp_plugin_api.h"
    if not sdk.is_file():
        raise FileNotFoundError(f"SDK header not found: {sdk}")
    return sdk


def resolve_template(name: str, start: pathlib.Path = None) -> pathlib.Path:
    root = find_repo_root(start)
    tpl = root / TEMPLATES_RELATIVE / name
    if not tpl.is_dir():
        raise FileNotFoundError(f"Template not found: {tpl}")
    return tpl


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
