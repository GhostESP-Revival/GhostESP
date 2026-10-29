#!/usr/bin/env python3
"""
GhostESP headless screen capture.

The deck mirrors its framebuffer over the same UART the console uses (see
main/core/screen_mirror.c). This tool turns the mirror on, reassembles the
partial flush areas into full frames in the host's memory, and writes one PNG
per settled screen -- so screenshots for docs / the firmware store can be
produced from the CLI without a phone photo of the panel.

Protocol (little-endian, from include/core/screen_mirror.h):

    struct { uint32 marker; uint8 cmd; uint16 x1, y1, x2, y2; uint32 data_len; }

    marker   0x47455350 "GESP"
    cmd      0x01 INFO     x1=width y1=height x2=colour depth, no pixel data
             0x02 FRAME            raw RGB565, data_len = px*2
             0x03 FRAME_RLE        (count u8, rgb565 u16) runs
             0x04 FRAME_8BIT       raw RGB332, data_len = px
             0x05 FRAME_8BIT_RLE   (count u8, rgb332 u8) runs
             0x06 FRAME_12BIT      packed RGB444, 2 px -> 3 B
    after the pixel data: 16-bit checksum for cmd 0x02/0x03 (none for the others)
    then 0x444E4547 "GEND"

Usage:

    python capture_screens.py --selftest
    python capture_screens.py --port COM8 --count 8 --out screenshots
    python capture_screens.py --port COM8 --out screenshots \\
        --steps "down;select;down select"

  No --steps  : saves a PNG every time the screen settles after a change.
  With --steps : sends the keys of each step, then saves the settled screen,
                 so the output is the exact screen list you asked for.
                Each step is one screen: keys separated by spaces, steps by ';'.
                Keys: up down left right select back home.
"""

import argparse
import os
import struct
import sys
import time
import zlib

MARKER = 0x47455350          # "GESP"
END_MARKER = 0x444E4547      # "GEND"
HEADER_SIZE = 17

CMD_INFO = 0x01
CMD_FRAME = 0x02
CMD_FRAME_RLE = 0x03
CMD_FRAME_8BIT = 0x04
CMD_FRAME_8BIT_RLE = 0x05
CMD_FRAME_12BIT = 0x06

CHECKSUMMED = (CMD_FRAME, CMD_FRAME_RLE)
MARKER_BYTES = struct.pack("<I", MARKER)
END_MARKER_BYTES = struct.pack("<I", END_MARKER)

DEFAULT_W, DEFAULT_H = 320, 240


# ---------------------------------------------------------------- colour tables

def _rgb332_table():
    """RGB332 (3-3-2) -> RGB888, expanding by bit replication so 0xFF means 255."""
    table = []
    for v in range(256):
        r3 = (v >> 5) & 0x07
        g3 = (v >> 2) & 0x07
        b2 = v & 0x03
        r = (r3 * 255) // 7
        g = (g3 * 255) // 7
        b = (b2 * 255) // 3
        table.append(bytes((r, g, b)))
    return table


def _rgb565_table():
    table = []
    for v in range(1 << 16):
        r = ((v >> 11) & 0x1F) << 3
        g = ((v >> 5) & 0x3F) << 2
        b = (v & 0x1F) << 3
        table.append(bytes((r | (r >> 5), g | (g >> 6), b | (b >> 5))))
    return table


def _rgb444_table():
    table = []
    for v in range(1 << 12):
        r4 = (v >> 8) & 0x0F
        g4 = (v >> 4) & 0x0F
        b4 = v & 0x0F
        r = r4 * 17
        g = g4 * 17
        b = b4 * 17
        table.append(bytes((r, g, b)))
    return table


P332 = _rgb332_table()
P565 = _rgb565_table()
P444 = _rgb444_table()


# -------------------------------------------------------------------- decoding

def decode_payload(cmd, payload, area_w, area_h):
    """Return RGB888 bytes (area_w * area_h * 3) for one packet's pixel data."""
    count = area_w * area_h

    if cmd == CMD_FRAME_8BIT:
        if len(payload) < count:
            return None
        out = bytearray(count * 3)
        for i in range(count):
            out[i * 3:i * 3 + 3] = P332[payload[i]]
        return bytes(out)

    if cmd == CMD_FRAME_8BIT_RLE:
        out = bytearray()
        i = 0
        while i + 1 < len(payload) and len(out) < count * 3:
            run = payload[i]
            colour = P332[payload[i + 1]]
            out.extend(colour * run)
            i += 2
        if len(out) < count * 3:
            return None
        return bytes(out[:count * 3])

    if cmd == CMD_FRAME:
        if len(payload) < count * 2:
            return None
        vals = struct.unpack_from("<%dH" % count, payload, 0)
        out = bytearray(count * 3)
        for i, v in enumerate(vals):
            out[i * 3:i * 3 + 3] = P565[v]
        return bytes(out)

    if cmd == CMD_FRAME_RLE:
        out = bytearray()
        i = 0
        while i + 2 < len(payload) and len(out) < count * 3:
            run = payload[i]
            # the firmware writes the RGB565 run value high byte first
            v = (payload[i + 1] << 8) | payload[i + 2]
            out.extend(P565[v] * run)
            i += 3
        if len(out) < count * 3:
            return None
        return bytes(out[:count * 3])

    if cmd == CMD_FRAME_12BIT:
        out = bytearray()
        i = 0
        while i + 1 < len(payload) and len(out) < count * 3:
            if i + 2 < len(payload) and (count * 3 - len(out)) >= 6:
                p0 = (payload[i] << 4) | (payload[i + 1] >> 4)
                p1 = ((payload[i + 1] & 0x0F) << 8) | payload[i + 2]
                out.extend(P444[p0])
                out.extend(P444[p1])
                i += 3
            else:
                p0 = (payload[i] << 4) | (payload[i + 1] >> 4)
                out.extend(P444[p0])
                i += 2
        if len(out) < count * 3:
            return None
        return bytes(out[:count * 3])

    return None


class Canvas:
    """RGB888 framebuffer the mirror's partial areas are composited into."""

    def __init__(self, width=DEFAULT_W, height=DEFAULT_H):
        self.width = width
        self.height = height
        self.pixels = bytearray(width * height * 3)
        self.dirty = False
        self.areas = 0

    def reset(self):
        self.pixels = bytearray(self.width * self.height * 3)
        self.dirty = False
        self.areas = 0

    def resize(self, width, height):
        self.width = width
        self.height = height
        self.reset()

    def blit(self, x1, y1, x2, y2, rgb):
        w = x2 - x1 + 1
        h = y2 - y1 + 1
        if w <= 0 or h <= 0:
            return
        row_bytes = w * 3
        for row in range(h):
            y = y1 + row
            if y < 0 or y >= self.height:
                continue
            src = row * row_bytes
            dst = (y * self.width + x1) * 3
            self.pixels[dst:dst + row_bytes] = rgb[src:src + row_bytes]
        self.dirty = True
        self.areas += 1

    def to_png(self, path):
        raw = bytearray()
        stride = self.width * 3
        for y in range(self.height):
            raw.append(0)  # PNG filter type 0
            raw += self.pixels[y * stride:(y + 1) * stride]

        def chunk(tag, data):
            return (struct.pack(">I", len(data)) + tag + data +
                    struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", struct.pack(">IIBBBBB", self.width, self.height, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        png += chunk(b"IEND", b"")
        with open(path, "wb") as fh:
            fh.write(png)
        return path


class MirrorStream:
    """Incremental parser: feed bytes in, get (cmd, x1, y1, x2, y2, payload)."""

    def __init__(self, canvas):
        self.buffer = bytearray()
        self.canvas = canvas
        self.bad_packets = 0
        self.packets = 0
        self.display = None  # (w, h, depth) from the INFO packet

    def feed(self, data):
        self.buffer.extend(data)
        out = []
        while True:
            idx = self.buffer.find(MARKER_BYTES)
            if idx < 0:
                # keep the tail: a marker may straddle two reads
                if len(self.buffer) > 3:
                    del self.buffer[:-3]
                break
            if idx:
                del self.buffer[:idx]
            if len(self.buffer) < HEADER_SIZE:
                break

            marker, cmd, x1, y1, x2, y2, dlen = struct.unpack_from("<IBHHHHI", self.buffer, 0)
            extra = 2 if cmd in CHECKSUMMED else 0
            total = HEADER_SIZE + dlen + extra + 4
            if len(self.buffer) < total:
                break

            payload = bytes(self.buffer[HEADER_SIZE:HEADER_SIZE + dlen])
            end = struct.unpack_from("<I", self.buffer, HEADER_SIZE + dlen + extra)[0]
            if end != END_MARKER:
                # not a real packet; drop one byte and resync
                self.bad_packets += 1
                del self.buffer[0]
                continue

            del self.buffer[:total]
            self.packets += 1
            out.append((cmd, x1, y1, x2, y2, payload))
        return out

    def apply(self, packet):
        cmd, x1, y1, x2, y2, payload = packet
        if cmd == CMD_INFO:
            self.display = (x1, y1, x2)
            return "info"
        rgb = decode_payload(cmd, payload, x2 - x1 + 1, y2 - y1 + 1)
        if rgb is None:
            return "short"
        self.canvas.blit(x1, y1, x2, y2, rgb)
        return "frame"


# ---------------------------------------------------------------- selftest

def selftest():
    """Exercise the parser + every decoder without any hardware."""
    failures = []

    def packet(cmd, x1, y1, x2, y2, payload, checksum=False):
        hdr = struct.pack("<IBHHHHI", MARKER, cmd, x1, y1, x2, y2, len(payload))
        body = hdr + payload
        if checksum:
            cs = 0
            for b in payload:
                cs = (cs + b) & 0xFFFF
            body += struct.pack("<H", cs)
        return body + END_MARKER_BYTES

    canvas = Canvas(16, 8)
    stream = MirrorStream(canvas)

    red = bytes((255, 0, 0))
    green = bytes((0, 255, 0))
    blue = bytes((0, 0, 255))
    # RGB332 codes for the three primaries, per the 3-3-2 bit layout.
    red332 = (7 << 5)
    green332 = (7 << 2)
    blue332 = 3

    cases = []

    # 0x04 raw RGB332, full width row
    cases.append(("8-bit raw", packet(CMD_FRAME_8BIT, 0, 0, 15, 0, bytes([red332]) * 16), red, 0, 0))
    # 0x05 RGB332 RLE, runs crossing the row boundary
    cases.append(("8-bit RLE", packet(CMD_FRAME_8BIT_RLE, 0, 1, 15, 1,
                                      bytes([8, green332, 8, blue332])), green, 0, 1))
    # 0x02 raw RGB565
    cases.append(("16-bit raw", packet(CMD_FRAME, 0, 2, 15, 2, struct.pack("<H", 0xF800) * 16, True),
                  red, 0, 2))
    # 0x03 RGB565 RLE
    cases.append(("16-bit RLE", packet(CMD_FRAME_RLE, 0, 3, 15, 3,
                                       bytes([16]) + struct.pack(">H", 0x07E0), True),
                  green, 0, 3))
    # 0x06 packed RGB444, two pixels -> three bytes
    p0 = 0xF00
    p1 = 0x0F0
    packed = bytes(((p0 >> 4) & 0xFF, ((p0 & 0x0F) << 4) | ((p1 >> 8) & 0x0F), p1 & 0xFF))
    cases.append(("12-bit packed", packet(CMD_FRAME_12BIT, 0, 4, 1, 4, packed), red, 0, 4))

    # INFO
    for pkt in stream.feed(packet(CMD_INFO, 16, 8, 16, 0, b"")):
        stream.apply(pkt)
    if stream.display != (16, 8, 16):
        failures.append("INFO packet not decoded: %r" % (stream.display,))

    for name, data, expect, px, py in cases:
        for pkt in stream.feed(data):
            stream.apply(pkt)
        got = canvas.pixels[(py * canvas.width + px) * 3:(py * canvas.width + px) * 3 + 3]
        if bytes(got) != expect:
            failures.append("%s: got %s expected %s" % (name, bytes(got), expect))

    # Garbage before a packet must be skipped, and a torn packet must resync.
    stream.feed(b"log line noise\n")
    for pkt in stream.feed(packet(CMD_FRAME_8BIT, 0, 5, 15, 5, bytes([blue332]) * 16)):
        stream.apply(pkt)
    got = canvas.pixels[(5 * canvas.width) * 3:(5 * canvas.width) * 3 + 3]
    if bytes(got) != blue:
        failures.append("resync after log noise: got %s expected %s" % (bytes(got), blue))

    torn = packet(CMD_FRAME_8BIT, 0, 6, 15, 6, bytes([red332]) * 16)
    for pkt in stream.feed(torn[:10]) + stream.feed(torn[10:]):
        stream.apply(pkt)
    got = canvas.pixels[(6 * canvas.width) * 3:(6 * canvas.width) * 3 + 3]
    if bytes(got) != red:
        failures.append("split packet: got %s expected %s" % (bytes(got), red))

    if canvas.areas < len(cases):
        failures.append("expected at least %d blits, saw %d" % (len(cases), canvas.areas))

    if failures:
        print("SELFTEST FAIL")
        for f in failures:
            print("  -", f)
        return 1
    print("SELFTEST PASS (%d packets, %d decoders, resync + split-packet cases)"
          % (stream.packets, len(cases)))
    return 0


# ---------------------------------------------------------------- live capture

KEY_CODES = {"up", "down", "left", "right", "select", "back", "home"}


def open_port(port, baud):
    import serial  # imported lazily so --selftest needs no pyserial

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    # Keep DTR/RTS deasserted: on a CH340X they are wired to EN/BOOT, and
    # opening the port with them asserted resets the deck.
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def send(ser, line):
    ser.write((line + "\n").encode())
    ser.flush()


def drain(ser, stream, canvas, settle, timeout):
    """Read until no packet has arrived for `settle` seconds. Returns True if any."""
    last_packet = time.time()
    saw = False
    deadline = last_packet + timeout
    while True:
        chunk = ser.read(4096)
        if chunk:
            for pkt in stream.feed(chunk):
                if stream.apply(pkt) == "frame":
                    saw = True
                    last_packet = time.time()
        else:
            if time.time() - last_packet >= settle:
                break
        if time.time() > deadline:
            break
    return saw


def capture(port, baud, out_dir, count, steps, settle, timeout, preview, pre=None, refresh=True):
    os.makedirs(out_dir, exist_ok=True)
    canvas = Canvas(DEFAULT_W, DEFAULT_H)
    stream = MirrorStream(canvas)
    ser = open_port(port, baud)
    saved = []

    try:
        time.sleep(0.4)
        ser.reset_input_buffer()
        send(ser, "identify")
        time.sleep(0.4)
        banner = ser.read(4096)
        text = banner.decode("ascii", errors="replace")
        print("identify -> %s" % (text.strip().splitlines()[-1] if text.strip() else "(no reply)"))

        for line in pre or []:
            print("pre: %s" % line)
            send(ser, line)
            time.sleep(0.6)

        send(ser, "mirror on")
        drain(ser, stream, canvas, settle, timeout)
        print("mirror on: %d packets, %d areas, display %r"
              % (stream.packets, canvas.areas, stream.display))

        index = 0

        def save_frame(label):
            nonlocal index
            index += 1
            path = os.path.join(out_dir, "%02d.png" % index)
            canvas.to_png(path)
            saved.append(path)
            print("[%02d] %-28s -> %s (areas %d)" % (index, label, path, canvas.areas))
            return path

        if steps:
            # Force a full repaint of whatever screen we are on, then save it.
            if refresh:
                send(ser, "mirror refresh")
            drain(ser, stream, canvas, settle, timeout)
            save_frame("step 1 (current screen)")
            for step_index, step in enumerate(steps, start=2):
                keys = [k for k in step.replace(",", " ").split() if k]
                for key in keys:
                    if key not in KEY_CODES:
                        print("unknown key %r in step %d" % (key, step_index))
                        continue
                    send(ser, "input %s" % key)
                    time.sleep(0.12)
                time.sleep(0.35)
                if refresh:
                    send(ser, "mirror refresh")
                drain(ser, stream, canvas, settle, timeout)
                save_frame("step %d (%s)" % (step_index, " ".join(keys)))
                if count and index >= count:
                    break
        else:
            # Capture-on-change: save every screen the deck settles on.
            last_state = None
            deadline = time.time() + timeout * 60
            while time.time() < deadline and (not count or index < count):
                send(ser, "mirror refresh")
                if drain(ser, stream, canvas, settle, timeout):
                    signature = bytes(canvas.pixels[::997])
                    if signature != last_state:
                        last_state = signature
                        save_frame("auto %d" % (index + 1))
                else:
                    time.sleep(0.5)
    finally:
        try:
            send(ser, "mirror off")
        except Exception:
            pass
        ser.close()

    print("saved %d screenshot(s) in %s" % (len(saved), os.path.abspath(out_dir)))
    if preview:
        for path in saved:
            print(path)
    return 0


def main():
    ap = argparse.ArgumentParser(description="Capture GhostESP screens over the serial mirror")
    ap.add_argument("--port", default=None, help="serial port (e.g. COM8); auto-detected if omitted")
    ap.add_argument("--baud", type=int, default=115200, help="console baud (default 115200)")
    ap.add_argument("--out", default="screenshots", help="output directory (default ./screenshots)")
    ap.add_argument("--count", type=int, default=0, help="stop after N screenshots (0 = no limit)")
    ap.add_argument("--steps", default=None,
                    help="navigation: 'down;select;down select' -- one step per screen")
    ap.add_argument("--settle", type=float, default=1.5,
                    help="seconds of quiet that counts as a settled screen (default 1.5)")
    ap.add_argument("--timeout", type=float, default=25.0,
                    help="seconds to wait for a screen before giving up (default 25)")
    ap.add_argument("--preview", action="store_true", help="print each saved path")
    ap.add_argument("--pre", default=None,
                    help="CLI commands to run before capturing, separated by ';' (e.g. 'apps')")
    ap.add_argument("--fast", action="store_true",
                    help="rely on LVGL's own partial updates instead of forcing a full repaint per step")
    ap.add_argument("--selftest", action="store_true", help="decode synthetic packets, no hardware")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    port = args.port
    if not port:
        try:
            import serial.tools.list_ports as lp
            ports = list(lp.comports())
            if not ports:
                print("no serial ports found; pass --port")
                return 2
            port = ports[0].device
            print("using port %s" % port)
        except ImportError:
            print("pyserial is required (pip install pyserial)")
            return 2

    pre = [c.strip() for c in (args.pre or "").split(";") if c.strip()]
    steps = [s.strip() for s in (args.steps or "").split(";") if s.strip()] or None
    return capture(port, args.baud, args.out, args.count, steps,
                   args.settle, args.timeout, args.preview, pre, not args.fast)


if __name__ == "__main__":
    sys.exit(main())
