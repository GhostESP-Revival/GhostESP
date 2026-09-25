"""Rasterise a Google Material Symbols SVG glyph to a 50x50 launcher PNG.

No native SVG rasteriser is available in this environment (cairo is missing for
cairosvg, svglib is not installed, and headless screenshots need a visible
desktop), so this walks the glyph's single <path> itself:

  * parse the `d` attribute into subpaths, flattening Q/C/A into polylines
  * fill with an even-odd scanline rasteriser, so interior holes (the d-pad and
    button cutouts in some glyphs) come out right instead of being filled in
  * render at 4x and downsample with LANCZOS for clean edges at 50px

The result is white-on-transparent, matching the alpha-only Material icons the
firmware already generates for the P4 (see scripts/generate_p4_material_icons.py).
"""
from __future__ import annotations

import math
import pathlib
import re
import sys
from urllib.request import Request, urlopen

from PIL import Image

SIZE = 50
SS = 4                      # supersample factor
GLYPH = 0.86                # glyph occupies this fraction of the canvas
CANDIDATES = ("sports_esports", "videogame_asset", "extension_games")
SVG_URL = (
    "https://raw.githubusercontent.com/google/material-design-icons/master/"
    "symbols/web/{name}/materialsymbolsoutlined/{name}_24px.svg"
)

OUT = pathlib.Path(__file__).resolve().parent
DST = OUT / "gb.png"

NUM = re.compile(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")
CMD = re.compile(r"([MmLlHhVvCcSsQqTtAaZz])")


# ---------------------------------------------------------------- path parsing
def _nums(chunk: str) -> list[float]:
    return [float(m.group()) for m in NUM.finditer(chunk)]


def _quad(p0, p1, p2, n=16):
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        yield (u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
               u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1])


def _cubic(p0, p1, p2, p3, n=20):
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        yield (u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
               u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1])


def _arc(p0, rx, ry, rot, large, sweep, p1, n=24):
    """Endpoint -> centre parameterisation, then flatten."""
    if rx == 0 or ry == 0 or p0 == p1:
        yield p1
        return
    rx, ry = abs(rx), abs(ry)
    phi = math.radians(rot)
    cosp, sinp = math.cos(phi), math.sin(phi)
    dx2, dy2 = (p0[0] - p1[0]) / 2.0, (p0[1] - p1[1]) / 2.0
    x1 = cosp * dx2 + sinp * dy2
    y1 = -sinp * dx2 + cosp * dy2
    lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry)
    if lam > 1:
        s = math.sqrt(lam)
        rx, ry = rx * s, ry * s
    num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1
    den = rx * rx * y1 * y1 + ry * ry * x1 * x1
    co = math.sqrt(max(num / den, 0.0))
    if large == sweep:
        co = -co
    cx1, cy1 = co * rx * y1 / ry, -co * ry * x1 / rx
    cx = cosp * cx1 - sinp * cy1 + (p0[0] + p1[0]) / 2.0
    cy = sinp * cx1 + cosp * cy1 + (p0[1] + p1[1]) / 2.0

    def angle(ux, uy, vx, vy):
        dot = ux * vx + uy * vy
        n1 = math.hypot(ux, uy)
        n2 = math.hypot(vx, vy)
        a = math.acos(max(-1.0, min(1.0, dot / (n1 * n2))))
        return -a if ux * vy - uy * vx < 0 else a

    th1 = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry)
    dth = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry)
    if not sweep and dth > 0:
        dth -= 2 * math.pi
    elif sweep and dth < 0:
        dth += 2 * math.pi
    for i in range(1, n + 1):
        th = th1 + dth * i / n
        ex, ey = rx * math.cos(th), ry * math.sin(th)
        yield (cosp * ex - sinp * ey + cx, sinp * ex + cosp * ey + cy)


def parse_path(d: str) -> list[list[tuple[float, float]]]:
    subpaths: list[list[tuple[float, float]]] = []
    cur: list[tuple[float, float]] = []
    pos = (0.0, 0.0)
    start = (0.0, 0.0)
    tokens = [t for t in CMD.split(d) if t.strip()]
    i = 0
    while i < len(tokens):
        cmd = tokens[i]
        args = _nums(tokens[i + 1]) if i + 1 < len(tokens) else []
        i += 2
        rel = cmd.islower()
        c = cmd.upper()
        k = 0
        if c == "Z":
            if cur:
                cur.append(start)
                subpaths.append(cur)
                cur = []
            pos = start
            continue
        while k < len(args):
            if c == "M":
                x, y = args[k], args[k + 1]
                k += 2
                if rel:
                    x, y = pos[0] + x, pos[1] + y
                if cur:
                    subpaths.append(cur)
                cur = [(x, y)]
                pos = start = (x, y)
                c = "L"                      # subsequent pairs are implicit lineto
            elif c == "L":
                x, y = args[k], args[k + 1]
                k += 2
                if rel:
                    x, y = pos[0] + x, pos[1] + y
                cur.append((x, y))
                pos = (x, y)
            elif c == "H":
                x = args[k]
                k += 1
                x = pos[0] + x if rel else x
                cur.append((x, pos[1]))
                pos = (x, pos[1])
            elif c == "V":
                y = args[k]
                k += 1
                y = pos[1] + y if rel else y
                cur.append((pos[0], y))
                pos = (pos[0], y)
            elif c in ("Q", "T"):
                if c == "Q":
                    x1, y1, x, y = args[k:k + 4]
                    k += 4
                    if rel:
                        x1, y1, x, y = (pos[0] + x1, pos[1] + y1,
                                        pos[0] + x, pos[1] + y)
                else:
                    x, y = args[k], args[k + 1]
                    k += 2
                    if rel:
                        x, y = pos[0] + x, pos[1] + y
                    x1, y1 = pos
                cur.extend(_quad(pos, (x1, y1), (x, y)))
                pos = (x, y)
            elif c == "C":
                x1, y1, x2, y2, x, y = args[k:k + 6]
                k += 6
                if rel:
                    x1, y1 = pos[0] + x1, pos[1] + y1
                    x2, y2 = pos[0] + x2, pos[1] + y2
                    x, y = pos[0] + x, pos[1] + y
                cur.extend(_cubic(pos, (x1, y1), (x2, y2), (x, y)))
                pos = (x, y)
            elif c == "S":
                x2, y2, x, y = args[k:k + 4]
                k += 4
                if rel:
                    x2, y2, x, y = (pos[0] + x2, pos[1] + y2,
                                    pos[0] + x, pos[1] + y)
                x1, y1 = (2 * pos[0] - cur[-2][0], 2 * pos[1] - cur[-2][1]) if cur else pos
                cur.extend(_cubic(pos, (x1, y1), (x2, y2), (x, y)))
                pos = (x, y)
            elif c == "A":
                rx, ry, rot, laf, sf, x, y = args[k:k + 7]
                k += 7
                if rel:
                    x, y = pos[0] + x, pos[1] + y
                cur.extend(_arc(pos, rx, ry, rot, int(laf), int(sf), (x, y)))
                pos = (x, y)
            else:
                raise ValueError(f"unsupported path command {cmd!r}")
    if cur:
        subpaths.append(cur)
    return subpaths


# ------------------------------------------------------------- even-odd fill
def fill_even_odd(subpaths, w: int, h: int) -> Image.Image:
    """Scanline fill honouring holes. Returns an 'L' coverage image."""
    mask = Image.new("L", (w, h), 0)
    px = mask.load()

    edges: list[tuple[float, float, float, float]] = []
    for sp in subpaths:
        if len(sp) < 2:
            continue
        pts = sp + [sp[0]]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            if y0 != y1:
                edges.append((x0, y0, x1, y1))
    if not edges:
        return mask

    for row in range(h):
        yc = row + 0.5
        xs: list[float] = []
        for x0, y0, x1, y1 in edges:
            if (y0 <= yc < y1) or (y1 <= yc < y0):
                t = (yc - y0) / (y1 - y0)
                xs.append(x0 + t * (x1 - x0))
        if not xs:
            continue
        xs.sort()
        for a, b in zip(xs[0::2], xs[1::2]):
            left = max(0, int(math.ceil(a - 0.5)))
            right = min(w - 1, int(math.floor(b - 0.5)))
            for col in range(left, right + 1):
                px[col, row] = 255
    return mask


# --------------------------------------------------------------------- main
def fetch(name: str) -> str | None:
    try:
        with urlopen(Request(SVG_URL.format(name=name),
                             headers={"User-Agent": "Mozilla/5.0"}), timeout=30) as r:
            return r.read().decode("utf-8")
    except Exception as exc:                                   # noqa: BLE001
        print(f"  {name}: {exc}")
        return None


def main() -> int:
    svg = None
    chosen = None
    for name in CANDIDATES:
        print(f"trying material symbol: {name}")
        svg = fetch(name)
        if svg:
            chosen = name
            break
    if not svg:
        print("ERROR: no material symbol could be fetched", file=sys.stderr)
        return 1

    d = re.search(r'\sd="([^"]+)"', svg).group(1)
    subpaths = parse_path(d)
    cmds = sorted(set(re.findall(r"[A-Za-z]", d)))
    print(f"  {chosen}: {len(d)} B, commands={cmds}, subpaths={len(subpaths)}")

    big = SIZE * SS
    span = big * GLYPH
    off = (big - span) / 2.0

    # viewBox is "0 -960 960 960" for Material Symbols: fit it into `span`.
    xs = [p[0] for sp in subpaths for p in sp]
    ys = [p[1] for sp in subpaths for p in sp]
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    s = span / max(maxx - minx, maxy - miny)
    cx = (minx + maxx) / 2.0
    cy = (miny + maxy) / 2.0

    scaled = [
        [(off + (x - cx) * s + span / 2.0, off + (y - cy) * s + span / 2.0) for (x, y) in sp]
        for sp in subpaths
    ]
    cov = fill_even_odd(scaled, big, big)

    glyph = cov.resize((SIZE, SIZE), Image.LANCZOS)
    canvas = Image.merge("RGBA", (
        Image.new("L", (SIZE, SIZE), 255),
        Image.new("L", (SIZE, SIZE), 255),
        Image.new("L", (SIZE, SIZE), 255),
        glyph,
    ))
    canvas.save(DST)
    print(f"wrote {DST} {canvas.size} {canvas.mode}")
    print(f"  corner alpha={canvas.getpixel((0, 0))[3]} centre={canvas.getpixel((SIZE // 2, SIZE // 2))}")
    print(f"  opaque px={sum(1 for v in glyph.tobytes() if v > 128)}/{SIZE * SIZE}")
    print(f"MATERIAL_ICON={chosen}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
