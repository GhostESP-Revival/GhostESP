import json
import pathlib
import shutil
import struct
import sys

from .icon import png_to_indexed_4bpp, png_to_rgb565, png_to_rgb565a8
from .utils import checksum_bytes, checksum_file, deflate_raw


IMAGE_MAGIC = b"GIMG"
IMAGE_VERSION = 1

FORMAT_RGB565 = 1
FORMAT_RGB565A8 = 2
FORMAT_INDEXED_4BPP = 3
FORMAT_BY_NAME = {
    "rgb565": FORMAT_RGB565,
    "rgb565a8": FORMAT_RGB565A8,
    "indexed_4bpp": FORMAT_INDEXED_4BPP,
}

COMPRESSION_NONE = 0
COMPRESSION_DEFLATE_RAW = 1

DEFAULT_ICON_VARIANTS = (32, 64)
DEFAULT_BACKGROUND_VARIANTS = {
    "full": {"width": 240, "height": 320, "format": "rgb565", "output": "bg/bg_full.gimg"},
    "half": {"width": 120, "height": 160, "format": "indexed_4bpp", "output": "bg/bg_half.gimg"},
    "tiny": {"width": 80, "height": 107, "format": "indexed_4bpp", "output": "bg/bg_tiny.gimg"},
    "tile": {"width": 32, "height": 32, "format": "indexed_4bpp", "output": "bg/bg_tile.gimg"},
}
VARIANT_LABELS = {
    32: "s",
    64: "l",
    128: "xl",
}


def _variant_label(size: int) -> str:
    return VARIANT_LABELS.get(size, str(size))


def _load_manifest(pack_dir: pathlib.Path) -> dict:
    manifest_path = pack_dir / "manifest.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"manifest.json not found in {pack_dir}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if "id" not in manifest:
        raise ValueError("asset pack manifest missing required field: id")
    return manifest


def _image_payload(src: pathlib.Path, width: int, height: int, fmt: str) -> bytes:
    if src.suffix.lower() != ".png":
        raise ValueError(f"unsupported image source format: {src}")
    if fmt == "rgb565a8":
        return png_to_rgb565a8(src, width, height)
    if fmt == "rgb565":
        return png_to_rgb565(src, width, height)
    if fmt == "indexed_4bpp":
        return png_to_indexed_4bpp(src, width, height)
    raise ValueError(f"unsupported image format: {fmt}")


def write_asset_image(
    src: pathlib.Path,
    dst: pathlib.Path,
    width: int,
    height: int,
    fmt: str = "rgb565a8",
    compress: bool = True,
) -> pathlib.Path:
    if width <= 0 or height <= 0:
        raise ValueError("image width and height must be positive")
    if fmt not in FORMAT_BY_NAME:
        raise ValueError(f"unsupported image format: {fmt}")

    raw = _image_payload(src, width, height, fmt)
    payload = raw
    compression = COMPRESSION_NONE
    if compress and fmt != "indexed_4bpp":
        # Indexed 4bpp is always stored uncompressed — the firmware rejects
        # deflated indexed payloads, and 4-bit data doesn't deflate well.
        compressed = deflate_raw(raw)
        if len(compressed) < len(raw):
            payload = compressed
            compression = COMPRESSION_DEFLATE_RAW

    # Header layout, little endian:
    # magic(4), version(u16), header_size(u16), width(u16), height(u16),
    # format(u8), compression(u8), reserved(u16), raw_size(u32),
    # payload_size(u32), raw_fnv1a64(u64)
    header = struct.pack(
        "<4sHHHHBBHIIQ",
        IMAGE_MAGIC,
        IMAGE_VERSION,
        32,
        width,
        height,
        FORMAT_BY_NAME[fmt],
        compression,
        0,
        len(raw),
        len(payload),
        checksum_bytes(raw),
    )
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(header + payload)
    return dst


def _parse_variant_sizes(values) -> tuple[int, ...]:
    if values is None:
        return DEFAULT_ICON_VARIANTS
    if isinstance(values, str):
        values = [v.strip() for v in values.split(",") if v.strip()]
    sizes = tuple(int(v) for v in values)
    if not sizes:
        raise ValueError("at least one icon variant size is required")
    for size in sizes:
        if size <= 0:
            raise ValueError("icon variant sizes must be positive")
    return sizes


def _background_variant_specs(spec: dict) -> dict:
    variants = spec.get("variants", DEFAULT_BACKGROUND_VARIANTS)
    if variants is True:
        variants = DEFAULT_BACKGROUND_VARIANTS
    if not isinstance(variants, dict):
        raise ValueError("background variants must be an object or true")

    out = {}
    for name, defaults in DEFAULT_BACKGROUND_VARIANTS.items():
        value = variants.get(name)
        if value is False or value is None:
            continue
        if value is True:
            value = {}
        if not isinstance(value, dict):
            raise ValueError(f"background variant {name!r} must be an object, true, or false")
        merged = dict(defaults)
        merged.update(value)
        out[name] = merged
    return out


def _copy_runtime_manifest(manifest: dict) -> dict:
    runtime = dict(manifest)
    for key in (
        "icon_sources",
        "icon_variants",
        "background_sources",
        "source_notes",
    ):
        runtime.pop(key, None)
    runtime.setdefault("version", 1)
    runtime.setdefault("format", "ghostesp_asset_pack")
    return runtime


def _write_checksums(package_dir: pathlib.Path) -> dict:
    checksums = {}
    for path in sorted(package_dir.rglob("*")):
        if not path.is_file() or path.name == "checksums.json":
            continue
        rel = path.relative_to(package_dir).as_posix()
        checksums[rel] = checksum_file(path)
    (package_dir / "checksums.json").write_text(
        json.dumps(checksums, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return checksums


def _write_archive(package_dir: pathlib.Path, out_path: pathlib.Path) -> pathlib.Path:
    if out_path.exists():
        out_path.unlink()
    # .gtheme uses the same archive header as .gapp, but stores each file
    # uncompressed. The files inside (notably .gimg) are already compressed,
    # and stored entries let firmware extract with a tiny streaming buffer.
    files = sorted(path for path in package_dir.rglob("*") if path.is_file())
    with out_path.open("wb") as out:
        out.write(struct.pack("<4sHHI", b"GAPP", 1, 0, len(files)))
        for path in files:
            rel = path.relative_to(package_dir).as_posix().encode("utf-8")
            data = path.read_bytes()
            if len(rel) > 65535:
                raise ValueError(f"archive path too long: {path}")
            out.write(struct.pack("<4sHHIIQ", b"FILE", 0, len(rel), len(data), len(data), checksum_bytes(data)))
            out.write(rel)
            out.write(data)
    return out_path


def make_asset_pack(
    pack_dir: str = ".",
    out: str = None,
    archive: bool = False,
    compress: bool = False,
) -> pathlib.Path:
    src_root = pathlib.Path(pack_dir).resolve()
    manifest = _load_manifest(src_root)
    pack_id = manifest["id"]

    out_root = pathlib.Path(out).resolve() if out else src_root / "dist"
    package_dir = out_root / pack_id
    if package_dir.exists():
        shutil.rmtree(package_dir)
    package_dir.mkdir(parents=True, exist_ok=True)

    runtime_manifest = _copy_runtime_manifest(manifest)

    icon_sources = manifest.get("icon_sources", {})
    icon_variants = _parse_variant_sizes(manifest.get("icon_variants"))
    icon_format = manifest.get("icon_format", "rgb565a8")
    icons = dict(runtime_manifest.get("icons", {}))

    for icon_name, rel_src in sorted(icon_sources.items()):
        src = src_root / rel_src
        if not src.is_file():
            print(f"warning: icon source not found: {src}", file=sys.stderr)
            continue
        variant_map = {}
        for size in icon_variants:
            label = _variant_label(size)
            rel_out = f"icons/{icon_name}_{label}.gimg"
            write_asset_image(src, package_dir / rel_out, size, size, icon_format, compress=compress)
            variant_map[label] = rel_out
        icons[icon_name] = variant_map

    if icons:
        runtime_manifest["icons"] = icons

    background_sources = manifest.get("background_sources", {})
    for key, spec in sorted(background_sources.items()):
        if isinstance(spec, str):
            spec = {"source": spec}
        src = src_root / spec["source"]
        if not src.is_file():
            print(f"warning: background source not found: {src}", file=sys.stderr)
            continue

        if spec.get("variants") is not None or key == "background":
            backgrounds = dict(runtime_manifest.get("backgrounds", {}))
            for variant_name, variant in _background_variant_specs(spec).items():
                rel_out = variant.get("output", f"bg/bg_{variant_name}.gimg")
                write_asset_image(
                    src,
                    package_dir / rel_out,
                    int(variant["width"]),
                    int(variant["height"]),
                    variant.get("format", "indexed_4bpp"),
                    compress=compress,
                )
                backgrounds[variant_name] = rel_out
            if backgrounds:
                runtime_manifest["backgrounds"] = backgrounds
            continue

        width = int(spec.get("width", 64 if key == "bg_tile" else 240))
        height = int(spec.get("height", 64 if key == "bg_tile" else 320))
        fmt = spec.get("format", "rgb565")
        rel_out = spec.get("output", f"{key}.gimg")
        write_asset_image(src, package_dir / rel_out, width, height, fmt, compress=compress)
        runtime_manifest[key] = rel_out

    assets = manifest.get("assets", [])
    for rel in assets:
        src = src_root / rel
        dst = package_dir / rel
        if src.is_dir():
            shutil.copytree(src, dst, dirs_exist_ok=True)
        elif src.is_file():
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        else:
            print(f"warning: asset not found: {src}", file=sys.stderr)

    manifest_data = json.dumps(runtime_manifest, indent=2, sort_keys=True) + "\n"
    (package_dir / "manifest.json").write_text(manifest_data, encoding="utf-8")
    _write_checksums(package_dir)

    if archive:
        archive_path = out_root / f"{pack_id}.gtheme"
        _write_archive(package_dir, archive_path)
        print(archive_path)
        return archive_path

    print(package_dir)
    return package_dir


def make_asset_image(
    src: str,
    out: str,
    width: int,
    height: int,
    fmt: str = "rgb565a8",
    compress: bool = True,
) -> pathlib.Path:
    dst = write_asset_image(
        pathlib.Path(src).resolve(),
        pathlib.Path(out).resolve(),
        width,
        height,
        fmt,
        compress=compress,
    )
    print(dst)
    return dst
