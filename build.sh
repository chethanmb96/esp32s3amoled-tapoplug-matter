#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build.sh -- Sets ESP_MATTER_PATH to the managed component, sources ESP-IDF,
#             then delegates to idf.py.
#
# Usage:
#   ./build.sh menuconfig          -- configure (set Wi-Fi password, setup code)
#   ./build.sh build               -- compile
#   ./build.sh flash monitor       -- flash + open serial monitor
#   ./build.sh flash monitor -p /dev/ttyACM0
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The esp_matter_controller CMakeLists.txt checks ENV{ESP_MATTER_PATH} and
# expects to find connectedhomeip/connectedhomeip inside it.
# The managed component already ships this tree, so we point directly at it.
export ESP_MATTER_PATH="${SCRIPT_DIR}/managed_components/espressif__esp_matter"

echo "[build.sh] ESP_MATTER_PATH=${ESP_MATTER_PATH}"

if ! command -v idf.py &>/dev/null; then
    echo "[build.sh] Sourcing ESP-IDF..."
    # shellcheck disable=SC1090
    source ~/esp/esp-idf/export.sh
fi

cd "${SCRIPT_DIR}"
echo "[build.sh] Running: idf.py $*"
exec idf.py "$@"
