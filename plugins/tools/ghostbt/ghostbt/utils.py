import pathlib
import shutil
import struct
import zlib


def checksum_file(path: pathlib.Path) -> str:
    h = 0xCBF29CE484222325
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            for b in chunk:
                h ^= b
                h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def checksum_bytes(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def deflate_raw(data: bytes) -> bytes:
    comp = zlib.compressobj(level=9, wbits=-15)
    return comp.compress(data) + comp.flush()


def write_gapp(package_dir: pathlib.Path, out_path: pathlib.Path, store_prefixes: tuple = ()) -> None:
    """Pack package_dir into a .gapp. Files whose archive-relative path starts
    with one of store_prefixes are always written uncompressed (STORE), even
    if DEFLATE would shrink them — this lets the firmware serve them directly
    out of the .gapp at read time instead of extracting a copy to the SD
    cache (see plugin_installer.c's ".direct_index"), which only works for
    byte-identical (uncompressed) archive entries. Opt-in per app via the
    manifest's "direct_read_assets" flag; default packaging is unaffected."""
    files = sorted(path for path in package_dir.rglob("*") if path.is_file())
    with out_path.open("wb") as out:
        out.write(struct.pack("<4sHHI", b"GAPP", 1, 0, len(files)))
        for path in files:
            rel_str = path.relative_to(package_dir).as_posix()
            rel = rel_str.encode("utf-8")
            data = path.read_bytes()
            if store_prefixes and rel_str.startswith(store_prefixes):
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


def copy_if_exists(src: pathlib.Path, dst: pathlib.Path, checksums: dict, rel: str) -> None:
    if not src.exists():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    checksums[rel.replace("\\", "/")] = checksum_file(dst)
