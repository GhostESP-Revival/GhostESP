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


def write_gapp(package_dir: pathlib.Path, out_path: pathlib.Path) -> None:
    files = sorted(path for path in package_dir.rglob("*") if path.is_file())
    with out_path.open("wb") as out:
        out.write(struct.pack("<4sHHI", b"GAPP", 1, 0, len(files)))
        for path in files:
            rel = path.relative_to(package_dir).as_posix().encode("utf-8")
            data = path.read_bytes()
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
