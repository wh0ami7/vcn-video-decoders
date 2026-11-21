#!/bin/zsh
# zsh is required: this is a macOS installer/build script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
#
# Builds the tests that need no GPU into tests/build/. They are built with the
# address and undefined-behaviour sanitizers, and a finding stops them. Nothing
# here opens the GPU or installs anything; tests/run.sh runs them.
set -e
cd "${0:A:h}"
B=build
mkdir -p "${B}"
TFLAGS=(-arch x86_64 -O1 -g -Wall -Wextra -Wno-unused-function
  -Wno-unused-parameter -fsanitize=address,undefined
  -fno-sanitize-recover=undefined)
xcrun clang "${TFLAGS[@]}" -I../vcnav1 -I../vcnvp9 parsedigest.c \
  ../vcnav1/av1parse.c ../vcnvp9/vp9parse.c -o "${B}/parsedigest"
xcrun clang "${TFLAGS[@]}" -I../vcnav1 av1state.c ../vcnav1/av1parse.c \
  -o "${B}/av1state"
xcrun clang "${TFLAGS[@]}" -I../vcnvp9 vp9state.c ../vcnvp9/vp9parse.c \
  -o "${B}/vp9state"
# The plug-ins' VideoToolbox side, with a stand-in for the decoder session.
for c in av1 vp9; do
  xcrun clang "${TFLAGS[@]}" -DTEST_${(U)c} plugintest.c \
    -framework CoreFoundation -framework CoreMedia -framework CoreVideo \
    -framework VideoToolbox -o "${B}/plugintest-${c}"
done
xcrun clang "${TFLAGS[@]}" -I../vcncommon acceltest.c ../vcncommon/vcndriver.c \
  -framework CoreFoundation -framework IOKit -o "${B}/acceltest"
xcrun clang "${TFLAGS[@]}" -I../vcncommon waittest.c ../vcncommon/vcnsession.c \
  -framework CoreFoundation -framework IOKit -o "${B}/waittest"
# The decoder sessions against a made-up driver: sessiontest.c stands in for
# vcncommon/vcndriver.c.
for c in av1 vp9; do
  xcrun clang "${TFLAGS[@]}" -DTEST_${(U)c} -I../vcncommon -I../vcn${c} \
    sessiontest.c ../vcn${c}/vcn${c}.c ../vcn${c}/${c}parse.c \
    ../vcncommon/vcnsession.c -framework CoreFoundation -framework CoreVideo \
    -framework IOKit -framework IOSurface -o "${B}/sessiontest-${c}"
done
# The loader kext's decisions (vcnvtkext/allow_logic.hpp), in a normal program.
xcrun clang++ -std=c++17 "${TFLAGS[@]}" -I../vcnvtkext loadertest.cpp \
  -o "${B}/loadertest"
echo "built the tests in tests/${B}"
