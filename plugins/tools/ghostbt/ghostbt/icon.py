import pathlib
import struct
import zlib


PNG_SIG = b"\x89PNG\r\n\x1a\n"


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _read_png_rgba(path: pathlib.Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIG):
        raise ValueError(f"not a PNG: {path}")

    pos = len(PNG_SIG)
    width = height = bit_depth = color_type = interlace = None
    idat = bytearray()
    palette: list[tuple[int, int, int]] = []
    trns = b""

    while pos + 8 <= len(data):
        size = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + size]
        pos += 12 + size

        if kind == b"IHDR":
            width, height, bit_depth, color_type, _comp, _filter, interlace = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"PLTE":
            palette = [(chunk[i], chunk[i + 1], chunk[i + 2]) for i in range(0, len(chunk), 3)]
        elif kind == b"tRNS":
            trns = chunk
        elif kind == b"IDAT":
            idat.extend(chunk)
        elif kind == b"IEND":
            break

    if width is None or height is None or bit_depth is None or color_type is None:
        raise ValueError(f"missing PNG header: {path}")
    if bit_depth != 8:
        raise ValueError(f"only 8-bit PNG icons are supported: {path}")
    if interlace != 0:
        raise ValueError(f"interlaced PNG icons are not supported: {path}")

    channels_by_type = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    if color_type not in channels_by_type:
        raise ValueError(f"unsupported PNG color type {color_type}: {path}")
    channels = channels_by_type[color_type]
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    expected = (stride + 1) * height
    if len(raw) < expected:
        raise ValueError(f"truncated PNG image data: {path}")

    recon = bytearray(width * height * channels)
    src = 0
    dst = 0
    prev = bytearray(stride)
    for _y in range(height):
        filter_type = raw[src]
        src += 1
        row = bytearray(raw[src:src + stride])
        src += stride
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = prev[x]
            up_left = prev[x - channels] if x >= channels else 0
            if filter_type == 1:
                row[x] = (row[x] + left) & 0xFF
            elif filter_type == 2:
                row[x] = (row[x] + up) & 0xFF
            elif filter_type == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 0xFF
            elif filter_type == 4:
                row[x] = (row[x] + _paeth(left, up, up_left)) & 0xFF
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG filter {filter_type}: {path}")
        recon[dst:dst + stride] = row
        dst += stride
        prev = row

    rgba = bytearray(width * height * 4)
    for i in range(width * height):
        p = i * channels
        o = i * 4
        if color_type == 0:
            g = recon[p]
            a = 255
            if len(trns) >= 2 and g == trns[1]:
                a = 0
            rgba[o:o + 4] = bytes((g, g, g, a))
        elif color_type == 2:
            r, g, b = recon[p], recon[p + 1], recon[p + 2]
            a = 255
            if len(trns) >= 6 and (r, g, b) == (trns[1], trns[3], trns[5]):
                a = 0
            rgba[o:o + 4] = bytes((r, g, b, a))
        elif color_type == 3:
            idx = recon[p]
            r, g, b = palette[idx] if idx < len(palette) else (0, 0, 0)
            a = trns[idx] if idx < len(trns) else 255
            rgba[o:o + 4] = bytes((r, g, b, a))
        elif color_type == 4:
            g, a = recon[p], recon[p + 1]
            rgba[o:o + 4] = bytes((g, g, g, a))
        else:
            rgba[o:o + 4] = recon[p:p + 4]

    return width, height, bytes(rgba)


def _resize_nearest(width: int, height: int, rgba: bytes, out_w: int, out_h: int) -> bytes:
    if width == out_w and height == out_h:
        return rgba
    out = bytearray(out_w * out_h * 4)
    for y in range(out_h):
        sy = min(height - 1, (y * height) // out_h)
        for x in range(out_w):
            sx = min(width - 1, (x * width) // out_w)
            out[(y * out_w + x) * 4:(y * out_w + x + 1) * 4] = rgba[(sy * width + sx) * 4:(sy * width + sx + 1) * 4]
    return bytes(out)


def png_to_rgb565a8(src: pathlib.Path, width: int, height: int) -> bytes:
    in_w, in_h, rgba = _read_png_rgba(src)
    rgba = _resize_nearest(in_w, in_h, rgba, width, height)
    pixel_count = width * height
    out = bytearray(pixel_count * 3)
    rgb_off = 0
    a_off = pixel_count * 2
    for i in range(pixel_count):
        r, g, b, a = rgba[i * 4:i * 4 + 4]
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[rgb_off] = rgb565 & 0xFF
        out[rgb_off + 1] = (rgb565 >> 8) & 0xFF
        out[a_off] = a
        rgb_off += 2
        a_off += 1
    return bytes(out)


def png_to_rgb565(src: pathlib.Path, width: int, height: int) -> bytes:
    in_w, in_h, rgba = _read_png_rgba(src)
    rgba = _resize_nearest(in_w, in_h, rgba, width, height)
    out = bytearray(width * height * 2)
    for i in range(width * height):
        r, g, b, a = rgba[i * 4:i * 4 + 4]
        r = (r * a) // 255
        g = (g * a) // 255
        b = (b * a) // 255
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[i * 2] = rgb565 & 0xFF
        out[i * 2 + 1] = (rgb565 >> 8) & 0xFF
    return bytes(out)


def png_to_indexed_4bpp(src: pathlib.Path, width: int, height: int) -> bytes:
    """Quantize an RGBA PNG to a 16-color palette and emit [palette][pixels].

    Output layout (matches the firmware's GIMG_FORMAT_INDEXED_4BPP):
      - 64 bytes: 16 x lv_color32_t palette in (B, G, R, A) memory order
      - ceil(W*H/2) bytes: packed 4-bit indices, low nibble = even pixel
    """
    in_w, in_h, rgba = _read_png_rgba(src)
    rgba = _resize_nearest(in_w, in_h, rgba, width, height)
    pixel_count = width * height

    # Coarse-quantize each pixel to a 4-4-4-1 key and tally occurrences.
    # 4-4-4 RGB gives 4096 buckets; we keep the 16 most populated.
    coarse_counts: dict[tuple[int, int, int, int], int] = {}
    pixel_keys: list[tuple[int, int, int, int]] = [None] * pixel_count
    for i in range(pixel_count):
        r, g, b, a = rgba[i * 4:i * 4 + 4]
        key = (r >> 4, g >> 4, b >> 4, 1 if a >= 128 else 0)
        pixel_keys[i] = key
        coarse_counts[key] = coarse_counts.get(key, 0) + 1

    sorted_keys = sorted(coarse_counts.items(), key=lambda kv: -kv[1])
    palette_keys = [k for k, _ in sorted_keys[:16]]
    while len(palette_keys) < 16:
        palette_keys.append((0, 0, 0, 1))

    # Map each pixel to a palette slot. In-gamut pixels hit the dict; the
    # remainder fall back to nearest-neighbour in 4-4-4-1 space.
    key_to_pal = {k: i for i, k in enumerate(palette_keys)}
    pixel_indices = bytearray(pixel_count)
    for i, key in enumerate(pixel_keys):
        idx = key_to_pal.get(key)
        if idx is not None:
            pixel_indices[i] = idx
            continue
        best = 0
        best_d = 0x7FFFFFFF
        for j, pk in enumerate(palette_keys):
            d = (
                (key[0] - pk[0]) ** 2
                + (key[1] - pk[1]) ** 2
                + (key[2] - pk[2]) ** 2
                + (key[3] - pk[3]) ** 2
            )
            if d < best_d:
                best_d = d
                best = j
        pixel_indices[i] = best

    palette_bytes = bytearray()
    for qr, qg, qb, qa in palette_keys:
        r = (qr << 4) | 0x0F
        g = (qg << 4) | 0x0F
        b = (qb << 4) | 0x0F
        a = 255 if qa else 0
        palette_bytes.extend(struct.pack("<BBBB", b, g, r, a))

    packed_count = (pixel_count + 1) // 2
    pixel_bytes = bytearray(packed_count)
    for i in range(pixel_count):
        if i & 1:
            pixel_bytes[i >> 1] |= pixel_indices[i] << 4
        else:
            pixel_bytes[i >> 1] |= pixel_indices[i]

    return bytes(palette_bytes) + bytes(pixel_bytes)
