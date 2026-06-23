#!/usr/bin/env python3
import argparse
import json
import pathlib
import shutil
import struct
import sys
import zlib


def checksum_file(path: pathlib.Path) -> str:
    h = 0xCBF29CE484222325
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            for b in chunk:
                h ^= b
                h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def copy_if_exists(src: pathlib.Path, dst: pathlib.Path, checksums: dict[str, str], rel: str) -> None:
    if not src.exists():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    checksums[rel.replace("\\", "/")] = checksum_file(dst)


def checksum_bytes(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def deflate_raw(data: bytes) -> bytes:
    comp = zlib.compressobj(level=9, wbits=-15)
    return comp.compress(data) + comp.flush()


def write_gapp(package_dir: pathlib.Path, out_path: pathlib.Path, no_compress: bool = False) -> None:
    files = sorted(path for path in package_dir.rglob("*") if path.is_file())
    with out_path.open("wb") as out:
        out.write(struct.pack("<4sHHI", b"GAPP", 1, 0, len(files)))
        for path in files:
            rel = path.relative_to(package_dir).as_posix().encode("utf-8")
            data = path.read_bytes()
            if no_compress:
                method = 0
                payload = data
            else:
                compressed = deflate_raw(data)
                if len(compressed) < len(data):
                    method = 1
                    payload = compressed
                else:
                    method = 0
                    payload = data
            if len(rel) > 65535:
                raise ValueError(f"archive path too long: {path}")
            out.write(struct.pack("<4sHHIIQ", b"FILE", method, len(rel), len(data), len(payload), checksum_bytes(data)))
            out.write(rel)
            out.write(payload)


def main() -> int:
    parser = argparse.ArgumentParser(description="Package a GhostESP native SD app")
    parser.add_argument("app_dir", help="App project directory")
    parser.add_argument("--out", default=None, help="Output dist directory")
    parser.add_argument("--gapp", action="store_true", help="Also create a compressed native .gapp archive")
    parser.add_argument("--zip", action="store_true", help="Deprecated alias for --gapp")
    parser.add_argument("--no-compress", action="store_true", help="Disable compression (store all files raw)")
    args = parser.parse_args()

    app_dir = pathlib.Path(args.app_dir).resolve()
    manifest_path = app_dir / "manifest.json"
    if not manifest_path.exists():
        print(f"manifest not found: {manifest_path}", file=sys.stderr)
        return 2

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    app_id = manifest["id"]
    version = manifest.get("version", "0.0.0")
    target = manifest.get("target", "unknown")
    entry = manifest["entry"]
    so_path = app_dir / "build" / entry
    if not so_path.exists():
        print(f"entry binary not found: {so_path}", file=sys.stderr)
        return 2

    dist_root = pathlib.Path(args.out).resolve() if args.out else app_dir / "dist"
    package_dir = dist_root / app_id
    if package_dir.exists():
        shutil.rmtree(package_dir)
    package_dir.mkdir(parents=True, exist_ok=True)

    checksums: dict[str, str] = {}
    copy_if_exists(manifest_path, package_dir / "manifest.json", checksums, "manifest.json")
    copy_if_exists(so_path, package_dir / entry, checksums, entry)

    for key in ("icon",):
        rel = manifest.get(key)
        if rel:
            copy_if_exists(app_dir / rel, package_dir / rel, checksums, rel)

    for rel in manifest.get("assets", []):
        src = app_dir / rel
        if src.is_dir():
            for child in src.rglob("*"):
                if child.is_file():
                    out_rel = str(child.relative_to(app_dir))
                    copy_if_exists(child, package_dir / out_rel, checksums, out_rel)
        else:
            copy_if_exists(src, package_dir / rel, checksums, rel)

    (package_dir / "checksums.json").write_text(json.dumps(checksums, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if args.gapp or args.zip:
        gapp_path = dist_root / f"{app_id}-{version}-{target}.gapp"
        if gapp_path.exists():
            gapp_path.unlink()
        write_gapp(package_dir, gapp_path, no_compress=args.no_compress)
        print(gapp_path)
    else:
        print(package_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
