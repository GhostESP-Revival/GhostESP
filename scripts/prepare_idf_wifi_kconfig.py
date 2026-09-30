#!/usr/bin/env python3
"""Align the ESP-IDF Wi-Fi cache TX buffer Kconfig default with sdkconfig.

ESP-IDF 6.1 can discard an explicit integer value when it differs from the
Kconfig default, leaving CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM defined but
empty. Keep the two values aligned before Kconfig is evaluated. This is the
cross-platform equivalent of the workaround used by the GitHub build matrix.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

CONFIG_NAME = "CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM"
KCONFIG_SYMBOL = "ESP_WIFI_CACHE_TX_BUFFER_NUM"


def _config_value(defaults: Path) -> int | None:
    text = defaults.read_text(encoding="utf-8")
    match = re.search(rf"^{re.escape(CONFIG_NAME)}=(\d+)\s*$", text, re.MULTILINE)
    return int(match.group(1)) if match else None


def _kconfig_pattern() -> re.Pattern[str]:
    return re.compile(
        r"(config " + KCONFIG_SYMBOL + r"\n"
        r"(?:(?!\n\s*config ).)*?\n\s*default )(\d+)(\n)",
        re.DOTALL,
    )


def align_kconfig(idf_path: Path, defaults: Path) -> int:
    requested = _config_value(defaults)
    if requested is None:
        print(f"{CONFIG_NAME} is not set numerically; leaving ESP-IDF unchanged")
        return 0

    kconfig = idf_path / "components" / "esp_wifi" / "Kconfig"
    if not kconfig.is_file():
        raise FileNotFoundError(f"ESP-IDF Wi-Fi Kconfig not found: {kconfig}")

    with kconfig.open("r", encoding="utf-8", newline="") as f:
        source = f.read()
    match = _kconfig_pattern().search(source)
    if match is None:
        raise RuntimeError(
            f"could not find the {KCONFIG_SYMBOL} default in {kconfig}"
        )

    current = int(match.group(2))
    if current == requested:
        print(f"{CONFIG_NAME} already matches ESP-IDF default ({requested})")
        return 0

    updated = _kconfig_pattern().sub(
        lambda m: f"{m.group(1)}{requested}{m.group(3)}", source, count=1
    )
    with kconfig.open("w", encoding="utf-8", newline="") as f:
        f.write(updated)
    print(
        f"Aligned ESP-IDF {KCONFIG_SYMBOL} default {current} -> {requested} "
        f"from {defaults}"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--idf-path", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    args = parser.parse_args()
    return align_kconfig(args.idf_path, args.config)


if __name__ == "__main__":
    raise SystemExit(main())
