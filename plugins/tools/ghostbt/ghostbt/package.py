import json
import pathlib
import shutil
import sys

from .config import load_manifest
from .icon import png_to_rgb565, png_to_rgb565a8
from .utils import checksum_bytes, checksum_file, copy_if_exists, write_gapp


def _write_icon_from_source(app_path: pathlib.Path, package_dir: pathlib.Path, manifest: dict, checksums: dict) -> bool:
    icon_source = manifest.get("icon_source")
    icon = manifest.get("icon")
    if not icon_source:
        return False
    if not icon:
        raise ValueError("manifest icon_source requires icon output path")
    width = int(manifest.get("icon_width", 0))
    height = int(manifest.get("icon_height", 0))
    if width <= 0 or height <= 0:
        raise ValueError("manifest icon_source requires icon_width and icon_height")
    fmt = manifest.get("icon_format", "rgb565a8")
    src = app_path / icon_source
    if not src.exists():
        print(f"warning: icon_source not found, icon will not be generated: {src}", file=sys.stderr)
        return False
    if src.suffix.lower() != ".png":
        raise ValueError(f"unsupported icon_source format: {src}")

    if fmt == "rgb565a8":
        data = png_to_rgb565a8(src, width, height)
    elif fmt == "rgb565":
        data = png_to_rgb565(src, width, height)
    else:
        raise ValueError(f"unsupported icon_format for packing: {fmt}")

    dst = package_dir / icon
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(data)
    checksums[icon.replace("\\", "/")] = f"{checksum_bytes(data):016x}"
    return True


def package_app(
    app_dir: str = ".",
    out: str = None,
    make_gapp: bool = False,
    target: str = None,
) -> pathlib.Path:
    app_path = pathlib.Path(app_dir).resolve()
    manifest = load_manifest(app_path)
    app_id = manifest["id"]
    version = manifest.get("version", "0.0.0")
    target = target or manifest.get("target", "unknown")
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
    packaged_manifest = dict(manifest)
    if target:
        packaged_manifest["target"] = target
    manifest_data = json.dumps(packaged_manifest, indent=2) + "\n"
    (package_dir / "manifest.json").write_text(manifest_data, encoding="utf-8")
    checksums["manifest.json"] = f"{checksum_bytes(manifest_data.encode('utf-8')):016x}"
    copy_if_exists(so_path, package_dir / entry, checksums, entry)

    packed_icon = _write_icon_from_source(app_path, package_dir, manifest, checksums)
    if not packed_icon:
        for key in ("icon",):
            rel = manifest.get(key)
            if rel:
                src = app_path / rel
                if not src.exists():
                    print(f"warning: icon file not found, package will use firmware fallback icon: {src}", file=sys.stderr)
                    continue
                copy_if_exists(src, package_dir / rel, checksums, rel)

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
