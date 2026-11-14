#!/bin/zsh
# zsh is required: this is a macOS installer/build script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
# Build VCNVTLoader.kext (a Lilu plugin). Toolchain flags follow the standard Lilu-plugin
# kernel build; this is validated on the first real compile (after the CoreMedia patch bytes
# are filled in kern_coremedia.cpp). Needs the Command Line Tools.
set -e
cd "${0:A:h}"
SDK=${KEXT_SDK:?"set KEXT_SDK to a folder holding Lilu.kext (DEBUG build, it ships the headers) and MacKernelSDK"}
LILU_HDR="$SDK/Lilu.kext/Contents/Resources"              # so <Headers/...> resolves
LILU_LIB="$SDK/Lilu.kext/Contents/Resources/Library"     # plugin_start.cpp lives here
MKSDK="$SDK/MacKernelSDK"
NAME=VCNVTLoader
OUT=build
rm -rf "$OUT" && mkdir -p "$OUT/$NAME.kext/Contents/MacOS"

./gen_cdhashes.sh

CXX=(xcrun clang++)
FLAGS=(
  -std=c++17 -target x86_64-apple-macos10.15
  -fapple-kext -mkernel -nostdlib -nostdinc++
  -DMODULE_VERSION=1.0.0 "-DPRODUCT_NAME=$NAME"
  -I. -I"$LILU_HDR" -I"$MKSDK/Headers"
  -Oz -flto=thin -fno-exceptions -fno-rtti
  -Wall -Wextra -Wno-unused-parameter
)
SRC=( kern_start.cpp kern_amfi.cpp kern_coremedia.cpp "$LILU_LIB/plugin_start.cpp" )

OBJ=()
for s in $SRC; do
  o="$OUT/$(basename ${s%.cpp}).o"
  "${CXX[@]}" $FLAGS -c "$s" -o "$o"
  OBJ+=("$o")
done

# kmod_info glue. Xcode normally generates this from the MODULE_START/MODULE_STOP
# build settings; a manual clang/ld build must emit it, or the kext has no kmod_info
# and the kernel cannot start it (it loads but never runs). This defines _kmod_info and
# wires libkmod's _start/_stop -> _realmain/_antimain -> our Lilu plugin entry points.
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

# Link as a kext: Lilu KPI + kernel KPIs come from MacKernelSDK's kmod library.
xcrun ld -kext -arch x86_64 \
  -platform_version macos 10.15 10.15 \
  -L"$MKSDK/Library/x86_64" -lkmod \
  "${OBJ[@]}" -o "$OUT/$NAME.kext/Contents/MacOS/$NAME"

cp Info.plist "$OUT/$NAME.kext/Contents/Info.plist"
codesign -f -s - "$OUT/$NAME.kext"
echo "built $OUT/$NAME.kext"
