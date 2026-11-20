#!/bin/bash
# Builds the VP9 test tools. Nothing here is installed, and decode_vp9 refuses
# to touch the GPU on its own.
set -euo pipefail
cd "$(dirname "$0")"
CFLAGS=(
  -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
  -Wno-deprecated-declarations -std=c11 -D_DARWIN_C_SOURCE -arch x86_64
  -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security
  -fno-strict-overflow
)
clang "${CFLAGS[@]}" vp9parse.c vp9dump.c -o vp9dump
# -DVCN_DRYRUN_TOOLS adds the memory dump helper tools/dryrun_vp9.py calls; the
# plug-in is built without it.
clang "${CFLAGS[@]}" -DVCN_DRYRUN_TOOLS -I../vcncommon vp9parse.c vcnvp9.c \
  ../vcncommon/vcndriver.c decode_vp9.c -o decode_vp9 \
  -framework CoreFoundation -framework CoreGraphics -framework CoreVideo \
  -framework ImageIO -framework IOKit -framework IOSurface
echo built
