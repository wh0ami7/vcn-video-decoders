#!/bin/zsh
# zsh is required: this is a macOS installer/build script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
# Builds VCNVideoProfile.kext, a Lilu plugin. Needs the Command Line Tools.
set -e
cd "${0:A:h}"
SDK=${KEXT_SDK:?"set KEXT_SDK to a folder holding Lilu.kext (DEBUG build, it ships the headers) and MacKernelSDK"}
LILU_HDR="$SDK/Lilu.kext/Contents/Resources"
LILU_LIB="$SDK/Lilu.kext/Contents/Resources/Library"
MKSDK="$SDK/MacKernelSDK"
NAME=VCNVideoProfile
OUT=build
rm -rf "$OUT" && mkdir -p "$OUT/$NAME.kext/Contents/MacOS"

CXX=(xcrun clang++)
FLAGS=(
  -std=c++17 -target x86_64-apple-macos10.15
  -fapple-kext -mkernel -nostdlib -nostdinc++
  -DMODULE_VERSION=1.0.0 "-DPRODUCT_NAME=$NAME"
  -I. -I"$LILU_HDR" -I"$MKSDK/Headers"
  -Oz -flto=thin -fno-exceptions -fno-rtti
  -Wall -Wextra -Wno-unused-parameter
)
SRC=( kern_start.cpp "$LILU_LIB/plugin_start.cpp" )

OBJ=()
for s in $SRC; do
  o="$OUT/$(basename ${s%.cpp}).o"
  "${CXX[@]}" $FLAGS -c "$s" -o "$o"
  OBJ+=("$o")
done

# A manual clang/ld build has to emit kmod_info itself, or the kext loads but never runs.
cat > "$OUT/kmod_info.c" <<EOF
#include <mach/mach_types.h>
#include <mach/kmod.h>
extern kern_return_t ${NAME}_kern_start(kmod_info_t *, void *);
extern kern_return_t ${NAME}_kern_stop(kmod_info_t *, void *);
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
KMOD_EXPLICIT_DECL(com.droga.${NAME}, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = ${NAME}_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = ${NAME}_kern_stop;
EOF
xcrun clang -target x86_64-apple-macos10.15 -fapple-kext -mkernel -nostdlib \
  -I"$MKSDK/Headers" -Oz -c "$OUT/kmod_info.c" -o "$OUT/kmod_info.o"
OBJ+=("$OUT/kmod_info.o")

xcrun ld -kext -arch x86_64 \
  -platform_version macos 10.15 10.15 \
  -L"$MKSDK/Library/x86_64" -lkmod \
  "${OBJ[@]}" -o "$OUT/$NAME.kext/Contents/MacOS/$NAME"

cp Info.plist "$OUT/$NAME.kext/Contents/Info.plist"
codesign -f -s - "$OUT/$NAME.kext"
echo "built $OUT/$NAME.kext"
