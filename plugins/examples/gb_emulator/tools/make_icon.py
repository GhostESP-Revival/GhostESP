#!/usr/bin/env python3
"""Generate the GB Emulator app icon as a tint-ready silhouette.

The firmware's app gallery recolors plugin icons to the app's accent colour
(app_gallery_screen.c: app_item_icon_should_recolor), so the icon must be a
single-colour silhouette: WHITE shape, alpha defines the outline, transparent
elsewhere. The launcher tints it with the manifest accent.

Outputs, next to the manifest:
  icon.rgb565  RGB565 plane (all white) followed by an A8 plane, 50x50 -
               LV_IMG_CF_RGB565A8 layout for plugin_icon_load_rgb565a8().
  gb.png       preview: the silhouette tinted with the accent on black, as the
               gallery draws it.

Usage:
  python tools/make_icon.py
"""

import pathlib
import zlib

W = H = 50
ACCENT = (139, 172, 15)          # DMG light green preview tint
SHAPE = (255, 255, 255)          # pure white: recolor multiplies to the accent


def inside_rounded(x: int, y: int, w: int, h: int, r: int) -> bool:
    if x < 0 or y < 0 or x >= w or y >= h:
        return False
    nx = min(x, w - 1 - x)
    ny = min(y, h - 1 - y)
    if nx >= r or ny >= r:
        return True
    dx = r - nx
    dy = r - ny
    return dx * dx + dy * dy <= r * r


def build_rgba() -> bytearray:
    """White gamepad silhouette: rounded tile with the pad/features as holes."""
    px = bytearray(W * H * 4)
    for y in range(H):
        for x in range(W):
            a = 0
            if inside_rounded(x, y, W, H, 10):
                a = 255
                # gamepad body: wide rounded shape
                if 6 <= x <= 44 and 14 <= y <= 38:
                    a = 255
                # screen cut-out
                if 12 <= x <= 38 and 18 <= y <= 30:
                    a = 0
                # dpad hole
                if 14 <= x <= 22 and 22 <= y <= 26:
                    a = 0
                if 16 <= x <= 20 and 20 <= y <= 28:
                    a = 0
                # A/B holes
                if (x - 34) ** 2 + (y - 22) ** 2 <= 9:
                    a = 0
                if (x - 40) ** 2 + (y - 30) ** 2 <= 6:
                    a = 0
                # start/select holes
                if 24 <= x <= 28 and y in (34, 35):
                    a = 0
                if 30 <= x <= 34 and y in (34, 35):
                    a = 0
            i = (y * W + x) * 4
            px[i] = SHAPE[0]
            px[i + 1] = SHAPE[1]
            px[i + 2] = SHAPE[2]
            px[i + 3] = a
    return px


def write_icon(px: bytearray, path: pathlib.Path) -> None:
    out = bytearray(W * H * 2)
    for i in range(W * H):
        r, g, b = px[i * 4], px[i * 4 + 1], px[i * 4 + 2]
        rgb = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out[i * 2] = rgb & 0xFF
        out[i * 2 + 1] = (rgb >> 8) & 0xFF
    alpha = bytes(px[i * 4 + 3] for i in range(W * H))
    path.write_bytes(bytes(out) + alpha)


def write_png(px: bytearray, path: pathlib.Path, tint: tuple) -> None:
    import struct
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            i = (y * W + x) * 4
            a = px[i + 3]
            raw += bytes((tint[0], tint[1], tint[2], a))
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + _chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
        + _chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + _chunk(b"IEND", b"")
    )


def _chunk(tag: bytes, data: bytes) -> bytes:
    import struct
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def main() -> None:
    root = pathlib.Path(__file__).resolve().parent.parent
    px = build_rgba()
    write_icon(px, root / "icon.rgb565")
    write_png(px, root / "gb.png", ACCENT)
    print(f"wrote {root / 'icon.rgb565'} ({W * H * 3} bytes) and {root / 'gb.png'}")


if __name__ == "__main__":
    main()
