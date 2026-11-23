"""Pack the embedded MIFARE Classic text dictionary as ordered six-byte keys."""

import argparse
from pathlib import Path


def parse_key_line(line: bytes) -> bytes | None:
    # Match the firmware parser: ignore separators, reject comments, and use
    # the first six complete hex bytes on each line.
    key = bytearray()
    high = None
    for char in line:
        if char == ord("#"):
            return None
        if 48 <= char <= 57:
            nibble = char - 48
        elif 65 <= char <= 70:
            nibble = char - 65 + 10
        elif 97 <= char <= 102:
            nibble = char - 97 + 10
        else:
            continue
        if high is None:
            high = nibble
        else:
            key.append((high << 4) | nibble)
            high = None
            if len(key) == 6:
                return bytes(key)
    return None


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    packed = b"".join(
        key for line in args.source.read_bytes().split(b"\n")
        if (key := parse_key_line(line)) is not None
    )
    args.output.write_bytes(packed)


if __name__ == "__main__":
    main()
