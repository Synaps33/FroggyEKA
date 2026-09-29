#!/usr/bin/env bash
#
# Builds the EKA2L1 SF2000 static core library (with the baremetal MIPS
# toolchain) and links the final GB300 V2 core image.
#
# Required:
#   EKA2L1_SRC   - path to the patched EKA2L1 checkout (default: ~/gb300/eka2l1)
#   MULTICORE    - path to the patched sf2000_multicore checkout
#   toolchain    - /opt/frog-toolchain/... (see the multicore Makefile)
#
set -euo pipefail

EKA2L1_SRC="${EKA2L1_SRC:-$HOME/gb300/eka2l1}"
MULTICORE="${MULTICORE:-$HOME/gb300/sf2000_multicore}"
FROGGY_TYPE="${FROGGY_TYPE:-GB300V2}"

echo "==> Building EKA2L1 core objects + merging archives"
make -C "$MULTICORE/cores/eka2l1" platform=sf2000

echo "==> Linking the device core ($FROGGY_TYPE)"
make -C "$MULTICORE" FROGGY_TYPE="$FROGGY_TYPE" CORE=cores/eka2l1

echo "==> Done: $MULTICORE/build/core_87000000"
md5sum "$MULTICORE/build/core_87000000"
