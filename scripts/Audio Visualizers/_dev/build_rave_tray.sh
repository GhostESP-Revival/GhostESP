#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TRAY_DIR="$ROOT_DIR/rave_tray"
OUT_BIN="$ROOT_DIR/rave_tray"
OUT_WORKER="$ROOT_DIR/rave_worker"
WORKER_SCRIPT="$ROOT_DIR/_internal/Display_Visualizer.py"

if ! command -v cargo >/dev/null 2>&1; then
  echo "cargo not found on PATH. Install Rust and try again."
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 not found on PATH. Install Python 3 and try again."
  exit 1
fi

if [ ! -f "$TRAY_DIR/Cargo.toml" ]; then
  echo "Missing $TRAY_DIR/Cargo.toml"
  exit 1
fi

if [ ! -f "$WORKER_SCRIPT" ]; then
  echo "Missing $WORKER_SCRIPT"
  exit 1
fi

echo "Building bundled worker (rave_worker) ..."
python3 -m pip install --upgrade pyinstaller >/dev/null
python3 -m PyInstaller --noconfirm --clean --onefile --name rave_worker --distpath "$SCRIPT_DIR" --workpath "$TRAY_DIR/build_py" --specpath "$TRAY_DIR/build_py" "$WORKER_SCRIPT"

echo "Building rave_tray (release) ..."
cargo build --release --manifest-path "$TRAY_DIR/Cargo.toml"

cp -f "$TRAY_DIR/target/release/rave_tray" "$OUT_BIN"
rm -f "$OUT_WORKER"

echo
echo "Built: $OUT_BIN"
echo "Worker is embedded in this binary."
echo "You only need to distribute rave_tray."
