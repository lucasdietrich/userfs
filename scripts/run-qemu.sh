#!/bin/bash
set -euo pipefail

# check whether the Yocto SDK is set up (SDKTARGETSYSROOT)
if [ -z "${SDKTARGETSYSROOT:-}" ]; then
  echo "Error: SDKTARGETSYSROOT is not set. Please source the Yocto SDK environment setup script first."
  exit 1
fi

# Check for --debug flag and remove it from arguments
if [[ "${1:-}" == "--debug" ]]; then
  QEMU_ARG_GDB="-g 1234"
  shift
fi

qemu-aarch64 \
-L "$SDKTARGETSYSROOT" \
  ${QEMU_ARG_GDB:-} \
  $@

