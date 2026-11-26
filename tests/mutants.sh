#!/bin/zsh
# zsh is required: this is a macOS build/test script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
#
# Does sessiontest-vp9 notice when the VP9 session's new rules are broken? Builds it from three damaged copies of
# vcnvp9.c and requires each to fail:
#   no-guard     the 2x-smaller-reference check removed        (the 4x frame would be decoded)
#   no-drop      the emptying of the slots of a failed frame removed  (later frames would decode on stale pictures)
#   too-eager    the check refuses any upscaled reference      (2x, which the engine does bit-exactly, would be refused)
# Nothing opens the GPU. Needs tests/media (tests/fetch_media.sh). Exit 0 = every damaged build was caught.
set -u
cd "${0:A:h}"
B=build/mutants
rm -rf "$B" && mkdir -p "$B"
SRC=../vcnvp9/vcnvp9.c
TFLAGS=(-arch x86_64 -O1 -g -Wno-unused-function -Wno-unused-parameter -fsanitize=address,undefined
  -fno-sanitize-recover=undefined)
CLIPS=(media/vp9/vp90-2-12-droppable_1.ivf
  media/vp9-invalid/invalid-vp90-2-09-aq2.webm.ivf.s3984_r01-05_b6-.ivf
  media/vp9/vp90-2-22-svc_1280x720_3.ivf media/vp9/vp90-2-13-mv-with-scaling.ivf)
# old text -> new text, exactly as in vcnvp9.c (perl quotes them, so no regex characters to escape)
typeset -A OLD=(
  no-guard   'if (2 * r->width < h->width || 2 * r->height < h->height) {'
  no-drop    'if (p->fh.refresh_frame_flags & (1u << i)) p->ref[i].valid = false;'
  too-eager  'if (2 * r->width < h->width || 2 * r->height < h->height) {'
)
typeset -A NEW=(
  no-guard   'if (0) {'
  no-drop    ';'
  too-eager  'if (r->width < h->width || r->height < h->height) {'
)
bad=0
for m in no-guard no-drop too-eager; do
  OLDTXT=${OLD[$m]} NEWTXT=${NEW[$m]} perl -0pe 's/\Q$ENV{OLDTXT}\E/$ENV{NEWTXT}/' $SRC > "$B/vcnvp9-$m.c"
  if cmp -s $SRC "$B/vcnvp9-$m.c"; then echo "FAIL $m: the edit did not change vcnvp9.c (the source moved?)"; bad=1; continue; fi
  cp ../vcnvp9/vp9parse.h ../vcnvp9/vp9msg.h ../vcnvp9/vcnvp9.h ../vcnvp9/ac_vcn_vp9_default.h "$B/" 2>/dev/null
  xcrun clang "${TFLAGS[@]}" -DTEST_VP9 -I../vcncommon -I../vcnvp9 sessiontest.c "$B/vcnvp9-$m.c" \
    ../vcnvp9/vp9parse.c ../vcncommon/vcnsession.c -framework CoreFoundation -framework CoreVideo \
    -framework IOKit -framework IOSurface -o "$B/sessiontest-$m" 2> "$B/$m.log" \
    || { echo "FAIL $m: the damaged copy does not build"; tail -3 "$B/$m.log"; bad=1; continue; }
  if "$B/sessiontest-$m" "${CLIPS[@]}" > "$B/$m.txt" 2>&1; then
    echo "FAIL $m: sessiontest still passes with this damage"; bad=1
  else
    echo "ok   $m: caught ($(grep -c '^FAIL' "$B/$m.txt") failed checks)"
  fi
done
exit $bad
