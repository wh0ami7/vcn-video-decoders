#!/bin/zsh
# zsh is required: this is a macOS installer/build script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
#
# Builds and runs every decoder test that needs no GPU.
#
#   tests/run.sh            all of it
#   tests/run.sh --quick    fewer damaged copies per stream (about ten times
#                           faster)
#
# Nothing here opens the GPU, loads a kext or changes the system. The programs
# that do talk to the GPU are listed in tests/README.md and are run by hand.
# Exit status 0 = every test passed.
set -u -o pipefail
cd "${0:A:h}"
rounds=300
[[ ${1:-} == --quick ]] && rounds=30
failed=()
passed=0
check() {  # check <name> <command...>
  local name=$1
  shift
  if "$@"; then
    echo "PASS ${name}"
    passed=$((passed + 1))
  else
    echo "FAIL ${name}"
    failed+=("${name}")
  fi
}

./build.sh > /dev/null || { echo "FAIL the tests do not build"; exit 1; }
./fetch_media.sh || { echo "FAIL no test media"; exit 1; }
B=build
PD=${B}/parsedigest

digests() {
  "${PD}" media/av1/*.ivf media/vp9/*.ivf > "${B}/digests.txt" || return 1
  diff expected-digests.txt "${B}/digests.txt"
}
check "valid streams are read as recorded" digests
invalid() { "${PD}" media/vp9-invalid/*.ivf > "${B}/invalid.txt"; }
check "libvpx's invalid streams are survived" invalid
damaged() {
  "${PD}" --mutate "${rounds}" media/av1/*.ivf media/vp9/*.ivf \
    > "${B}/damaged.txt"
}
check "${rounds} damaged copies of each stream are survived" damaged

av1state() {
  "${B}/av1state" media/av1/av1-1-b8-01-size-16x16.ivf > "${B}/av1state.txt" ||
    { cat "${B}/av1state.txt"; return 1; }
}
check "a refused AV1 sample leaves the parser consistent" av1state
vp9state() {
  "${B}/vp9state" media/vp9/vp90-2-05-resize.ivf > "${B}/vp9state.txt" ||
    { cat "${B}/vp9state.txt"; return 1; }
}
check "a refused VP9 header leaves the parser consistent" vp9state
plugins() {
  local c
  for c in av1 vp9; do
    "${B}/plugintest-${c}" > "${B}/plugintest-${c}.txt" ||
      { cat "${B}/plugintest-${c}.txt"; return 1; }
  done
}
check "the plug-ins' VideoToolbox side, with a stand-in decoder" plugins

lists() {
  ./check_lists.sh > "${B}/check_lists.txt" ||
    { cat "${B}/check_lists.txt"; return 1; }
}
check "installer/validated-builds.json agrees with the decoders' driver lists" \
  lists
check "only AMD's accelerator is taken" "${B}/acceltest"
check "a decode command that never finishes times out" "${B}/waittest"
check "the loader kext lets through only the listed plug-ins" "${B}/loadertest"
sessions() {
  "${B}/sessiontest-av1" media/av1/av1-1-b8-01-size-16x16.ivf \
    > "${B}/sessiontest-av1.txt" &&
    "${B}/sessiontest-vp9" media/vp9/vp90-2-12-droppable_1.ivf \
      > "${B}/sessiontest-vp9.txt" ||
    { cat "${B}/sessiontest-av1.txt" "${B}/sessiontest-vp9.txt" 2>/dev/null |
        grep -v ^PASS; return 1; }
}
check "decoder sessions release the driver once, also after failures and a \
stuck GPU (made-up driver)" sessions

if (( ${#failed} )); then
  echo "FAILED: ${(j:, :)failed}"
  exit 1
fi
echo "all ${passed} tests passed"
