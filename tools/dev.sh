#!/usr/bin/env bash
# Build / flash an example using the local secrets file.
#
#   tools/dev.sh build                    # default app (00_glyph_grid_reveal)
#   tools/dev.sh build 06_qianwenai_push_to_talk
#   tools/dev.sh flash 06_qianwenai_push_to_talk   # build + flash to /dev/ttyACM0
#   tools/dev.sh monitor                  # follow the serial log
#
# Credentials come from ./secrets.local (git-ignored), never from the command
# line, so they stay out of shell history and out of the build log.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

action="${1:-build}"
app="${2:-00_glyph_grid_reveal}"
port="${PORT:-/dev/ttyACM0}"

# --- ESP-IDF environment -------------------------------------------------
if ! command -v idf.py >/dev/null 2>&1; then
    # Fall back to the EIM install layout (same values the activation script
    # exports). Sourcing activate_idf_v6.1.sh here is deliberately avoided: it
    # rewrites PATH/state and returns non-zero in non-interactive shells.
    export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$HOME/.espressif/tools}"
    export IDF_PATH="${IDF_PATH:-$HOME/.espressif/v6.1/esp-idf}"
    export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$IDF_TOOLS_PATH/python/v6.1/venv}"
    export ESP_IDF_VERSION="${ESP_IDF_VERSION:-6.1}"
    export PATH="$IDF_TOOLS_PATH/python/v6.1/venv/bin:$IDF_TOOLS_PATH/shims/v6.1:$PATH"
    # Toolchain + build tools live in per-version directories.
    for d in "$IDF_TOOLS_PATH"/xtensa-esp-elf/*/xtensa-esp-elf/bin \
             "$IDF_TOOLS_PATH"/esp-clang/*/esp-clang/bin \
             "$IDF_TOOLS_PATH"/cmake/*/bin \
             "$IDF_TOOLS_PATH"/ninja/*; do
        [ -d "$d" ] && export PATH="$d:$PATH"
    done
fi
command -v idf.py >/dev/null 2>&1 || { echo "idf.py not found (source ESP-IDF first)"; exit 1; }

# --- secrets -------------------------------------------------------------
# Secrets go through the ENVIRONMENT, not -D: CMake turns them into
# build/<app>/ring_secrets.h. Passing them as -D would leak them into
# CMakeCache.txt and build/log/.
if [ -f secrets.local ]; then
    set -a
    # shellcheck disable=SC1091
    . ./secrets.local
    set +a
    # Map the friendly names in secrets.local to what the build expects.
    export QIANWEN_API_KEY="${QIANWEN_API_KEY:-}"
    export RING_WIFI_DEFAULT_SSID="${RING_WIFI_SSID:-}"
    export RING_WIFI_DEFAULT_PASSWORD="${RING_WIFI_PASSWORD:-}"
else
    echo "note: no secrets.local — building without API key / Wi-Fi credentials"
fi


build_dir="build/${app}"
common=(-D "RING_APP=${app}" -B "${build_dir}")

case "${action}" in
build)
    idf.py "${common[@]}" build
    ;;
flash)
    idf.py "${common[@]}" -p "${port}" flash
    ;;
monitor)
    idf.py -B "${build_dir}" -p "${port}" monitor
    ;;
*)
    echo "usage: $0 {build|flash|monitor} [app]"; exit 2
    ;;
esac
