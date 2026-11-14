#!/bin/zsh
# zsh is required: this is a macOS installer/build script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
#
# Build AV1VCNDecoder.bundle (VideoToolbox decoder plugin for av01 on AMD VCN).
set -e
cd "${0:A:h}"
B=build/AV1VCNDecoder.bundle
rm -rf "${B}"
mkdir -p "${B}/Contents/MacOS"
cp Info.plist "${B}/Contents/Info.plist"
clang -arch x86_64 -O2 -g -bundle -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security \
  -fno-strict-overflow \
  -Wno-four-char-constants -Wno-deprecated-declarations -mmacosx-version-min=13.0 \
  AV1VCNDecoder.c ../vcnav1/vcnav1.c ../vcnav1/av1parse.c \
  -framework CoreFoundation -framework CoreMedia -framework CoreVideo \
  -framework VideoToolbox -framework IOKit -framework IOSurface \
  -o "${B}/Contents/MacOS/AV1VCNDecoder"
codesign -f -s - "${B}"
nm -gU "${B}/Contents/MacOS/AV1VCNDecoder" | grep -i createinstance
echo "built ${B}"
