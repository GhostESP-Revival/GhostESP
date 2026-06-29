import json
import pathlib
import shutil
import sys

from .config import load_manifest
from .utils import checksum_file, copy_if_exists, write_gapp


def package_app(
    app_dir: str = ".",
    out: str = None,
    make_gapp: bool = False,
) -> pathlib.Path:
    app_path = pathlib.Path(app_dir).resolve()
    manifest = load_manifest(app_path)
    app_id = manifest["id"]
    version = manifest.get("version", "0.0.0")
    target = manifest.get("target", "unknown")
    entry = manifest["entry"]

    so_path = app_path / "build" / entry
    if not so_path.exists():
        print(f"entry binary not found: {so_path}", file=sys.stderr)
        raise SystemExit(2)

    dist_root = pathlib.Path(out).resolve() if out else app_path / "dist"
    package_dir = dist_root / app_id
    if package_dir.exists():
        shutil.rmtree(package_dir)
    package_dir.mkdir(parents=True, exist_ok=True)

    checksums: dict = {}
    copy_if_exists(app_path / "manifest.json", package_dir / "manifest.json", checksums, "manifest.json")
    copy_if_exists(so_path, package_dir / entry, checksums, entry)

    for key in ("icon",):
        rel = manifest.get(key)
        if rel:
            copy_if_exists(app_path / rel, package_dir / rel, checksums, rel)

    for rel in manifest.get("assets", []):
        src = app_path / rel
        if src.is_dir():
            for child in src.rglob("*"):
                if child.is_file():
                    out_rel = str(child.relative_to(app_path))
                    copy_if_exists(child, package_dir / out_rel, checksums, out_rel)
        else:
            copy_if_exists(src, package_dir / rel, checksums, rel)

    (package_dir / "checksums.json").write_text(
        json.dumps(checksums, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    if make_gapp:
        gapp_path = dist_root / f"{app_id}-{version}-{target}.gapp"
        if gapp_path.exists():
            gapp_path.unlink()
        write_gapp(package_dir, gapp_path)
        print(gapp_path)
        return gapp_path
    else:
        print(package_dir)
        return package_dir
