#!/usr/bin/env bash
# Local checks for the QL-700 / ESP32-S3 PlatformIO firmware.
set -euo pipefail
cd "$(dirname "$0")/.."

printf '\n== QL-700 protocol host tests ==\n'
./scripts/host-test.sh

printf '\n== PlatformIO firmware build ==\n'
command -v pio >/dev/null 2>&1 || {
    echo "PlatformIO CLI (pio) is not installed or not on PATH." >&2
    exit 127
}
pio run -e ql700
printf '\n== checks passed ==\n'
