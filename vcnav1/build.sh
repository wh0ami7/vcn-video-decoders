#!/bin/zsh
# zsh is required: this is a macOS installer/build script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
# Builds the AV1 test tools: decode_ivf (VCN directly) and ../vtav1/vtav1test (through VideoToolbox).
# Installs nothing. decode_ivf refuses to touch the GPU unless VCNAV1_ALLOW_GPU is set.
set -e
cd "${0:A:h}"
CF=(-arch x86_64 -O1 -g -mmacosx-version-min=13.0 -Wall -Wextra -Wno-unused-parameter
  -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security
  -fno-strict-overflow -Wno-four-char-constants -Wno-deprecated-declarations
  -Wno-unused-function)
clang "${CF[@]}" av1parse.c vcnav1.c decode_ivf.c \
  -framework IOKit -framework IOSurface -framework CoreFoundation -framework CoreVideo -o decode_ivf
clang "${CF[@]}" ../vtav1/vtav1test.c av1parse.c \
  -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox -o ../vtav1/vtav1test
codesign -f -s - decode_ivf ../vtav1/vtav1test
echo "built decode_ivf and ../vtav1/vtav1test"
