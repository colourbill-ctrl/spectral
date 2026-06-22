#!/usr/bin/env bash
# Deploy spectral to chardata.colourbill.com/spectral/.
#
# The app is plain static files (public/), served by nginx via a
# `location /spectral/` alias block — there is no frontend build step, so a
# deploy is just an rsync. The WASM module is prebuilt into public/wasm/ and
# only needs rebuilding when wrapper.cpp / the iccDEV source changes.
#
# Usage:
#   scripts/deploy.sh            # rsync public/ as-is (default)
#   WITH_WASM=1 scripts/deploy.sh   # rebuild the WASM module first, then rsync
#
# Prerequisites:
#   - ~/.ssh/config host alias "chardata"
#   - nginx on the box serving /var/www/spectral/ at /spectral/
#   - (WITH_WASM=1 only) Emscripten SDK + iccDEV source — see scripts/build-wasm.sh
#     (builds against /home/colour/code/iccdev-spectral; override with ICCDEV_ROOT)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REMOTE="${DEPLOY_REMOTE:-chardata:/var/www/spectral/}"

cd "$REPO_ROOT"

if [ -n "${WITH_WASM:-}" ]; then
  # shellcheck disable=SC1091
  source "$HOME/emsdk-install/emsdk/emsdk_env.sh" 2>/dev/null || {
    echo "error: couldn't source emsdk env — unset WITH_WASM to skip, or install emsdk" >&2
    exit 1
  }
  scripts/build-wasm.sh
fi

# Guard against rsync --delete wiping the live site if the build is incomplete.
for f in index.html app.js spectral.js wasm/spectral.mjs wasm/spectral.wasm; do
  if [ ! -f "public/$f" ]; then
    echo "error: public/$f missing — refusing to rsync --delete" >&2
    exit 1
  fi
done

rsync -avz --delete public/ "$REMOTE"

echo
echo "deployed → https://chardata.colourbill.com/spectral/"
echo "if the browser shows a stale build: hard-reload (Ctrl+Shift+R / Cmd+Shift+R)"
