#!/usr/bin/env bash
#
# Copies the freshly built core and the frontend loader to the SD card.
# The GB300 firmware loads cores from two places:
#   /cores/eka2l1/core_87000000        (FrogUI core folder)
#   /system/Deimos/cores/eka2l1.sf2k   (Deimos menu core folder)
#
# Required:
#   MULTICORE - patched sf2000_multicore checkout
#   SD        - SD card mount point (default: /media/$USER/GB300)
#
set -euo pipefail

MULTICORE="${MULTICORE:-$HOME/gb300/sf2000_multicore}"
SD="${SD:-/media/$USER/GB300}"

if [[ ! -d "$SD" ]]; then
    echo "SD card not mounted at $SD" >&2
    exit 1
fi

echo "==> Copying core to $SD"
install -m 755 "$MULTICORE/build/core_87000000" "$SD/cores/eka2l1/core_87000000"
install -m 755 "$MULTICORE/build/core_87000000" "$SD/system/Deimos/cores/eka2l1.sf2k"

if [[ -f "$MULTICORE/build/bisrv.asd" ]]; then
    echo "==> Updating frontend loader /bios/bisrv.asd"
    cp -f "$SD/bios/bisrv.asd" "$SD/bios/bisrv.asd.bak" 2>/dev/null || true
    install -m 755 "$MULTICORE/build/bisrv.asd" "$SD/bios/bisrv.asd"
fi

sync
echo "==> Log is at $SD/system/logs/Multicore.log"
