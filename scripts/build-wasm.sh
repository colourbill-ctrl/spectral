#!/usr/bin/env bash
# Build the spectral WASM module and copy the artifacts into public/wasm/.
#
# Prerequisites:
#   - Emscripten SDK activated (source emsdk_env.sh)
#   - iccDEV spectral worktree at /home/colour/code/iccdev-spectral
#     (override with ICCDEV_ROOT)
#
# Usage:
#   scripts/build-wasm.sh           # build + copy + refresh checksums
#   scripts/build-wasm.sh --verify  # rebuild and diff against committed checksums

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ICCDEV_ROOT="${ICCDEV_ROOT:-/home/colour/code/iccdev-spectral}"
SRC_DIR="$REPO_ROOT/spectral-wasm"
BUILD_DIR="$SRC_DIR/build"
OUT_DIR="$REPO_ROOT/public/wasm"
CHECKSUM_FILE="$OUT_DIR/SHA256SUMS"

if ! command -v emcmake >/dev/null; then
  echo "error: emcmake not on PATH — source your emsdk_env.sh first" >&2
  exit 1
fi

if [ ! -f "$ICCDEV_ROOT/IccProfLib/IccColorimetry.h" ]; then
  echo "error: iccDEV spectral source not found at $ICCDEV_ROOT" >&2
  echo "       (expected IccProfLib/IccColorimetry.h — set ICCDEV_ROOT)" >&2
  exit 1
fi

if [ ! -d "$BUILD_DIR" ]; then
  emcmake cmake -S "$SRC_DIR" -B "$BUILD_DIR" -DICCDEV_ROOT="$ICCDEV_ROOT"
fi
cmake --build "$BUILD_DIR" -j"$(nproc)"

mkdir -p "$OUT_DIR"
ARTIFACTS=(spectral.mjs spectral.wasm)

if [ "${1:-}" = "--verify" ]; then
  if [ ! -f "$CHECKSUM_FILE" ]; then
    echo "error: no committed checksums at $CHECKSUM_FILE" >&2
    exit 2
  fi
  cd "$BUILD_DIR"
  expected=$(sort "$CHECKSUM_FILE")
  actual=$(sha256sum "${ARTIFACTS[@]}" | sort)
  if [ "$expected" != "$actual" ]; then
    echo "FAIL: rebuilt artifacts do not match committed checksums" >&2
    diff <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") >&2 || true
    exit 3
  fi
  echo "OK: rebuilt artifacts match $CHECKSUM_FILE"
  exit 0
fi

for f in "${ARTIFACTS[@]}"; do
  cp "$BUILD_DIR/$f" "$OUT_DIR/"
done

cd "$OUT_DIR"
sha256sum "${ARTIFACTS[@]}" > SHA256SUMS
echo
echo "=== artifact checksums (public/wasm/SHA256SUMS) ==="
cat SHA256SUMS
