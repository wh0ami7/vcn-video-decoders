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
check() {  # check <name> <command...>
  local name=$1
  shift
  if "$@"; then
    echo "PASS ${name}"
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

if (( ${#failed} )); then
  echo "FAILED: ${(j:, :)failed}"
  exit 1
fi
echo "all tests passed"
