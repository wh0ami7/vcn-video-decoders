#!/bin/zsh
# zsh is required: this is a macOS installer/build script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
#
# repair-plugins.sh — one-off repair after the engine.sh bug of 2026-11-06 wiped
# /System/Library/Video/Plug-Ins (Apple's plug-ins) in the boot snapshot.
#
# What it does:
#   1. mounts the live system volume read-write at /System/Volumes/Update/mnt1
#   2. mounts the last GOOD snapshot (booted this afternoon) read-only
#   3. restores Apple's plug-ins from the good snapshot without deleting the directory
#   4. installs AV1VCNDecoder.bundle + VP9VCNDecoder.bundle into it and verifies cdhashes
#   5. blesses a new boot snapshot  -> reboot
#
# Nothing outside .../System/Library/Video/Plug-Ins on the system volume is touched.
# Run:  sudo zsh repair-plugins.sh
#
set -eu
setopt pipefail

[[ $EUID -eq 0 ]] || { echo "run with: sudo zsh $0"; exit 1; }
HERE=${0:A:h}

GOOD_SNAP=com.apple.bless.81846B18-5CF0-4A10-B806-1BCA9AF53056   # UUID 117F6397, booted 2026-11-06 afternoon
RES="$HERE/build/AMD VCN Video Decoders.app/Contents/Resources"
MNT=/System/Volumes/Update/mnt1
# New directory each run: a fixed path under /private/var/tmp can already be
# mounted by another user and would then serve attacker-controlled "Apple" bundles.
SNAPMNT=
REL=System/Library/Video/Plug-Ins

csrutil authenticated-root status | grep -qi disabled || { echo "authenticated-root must be disabled"; exit 1; }
for b in AV1VCNDecoder.bundle VP9VCNDecoder.bundle; do
  [[ -f "$RES/$b/Contents/Info.plist" ]] || { echo "missing $RES/$b"; exit 1; }
done

snap=$(diskutil info / | awk -F': *' '/Device Node/{print $2}')
vol=${snap%s[0-9]*}
[[ "$vol" == /dev/disk*s* ]] || { echo "could not determine system volume (got '$vol')"; exit 1; }
echo "system volume: $vol"

mkdir -p "$MNT"
mount | grep -q " on $MNT " || mount -o nobrowse -t apfs "$vol" "$MNT"
SNAPMNT=$(mktemp -d /private/var/tmp/vcn-goodsnap.XXXXXX) \
  || { echo "could not make a mount point for the good snapshot"; exit 1; }
mount_apfs -s "$GOOD_SNAP" -o rdonly,nobrowse "$vol" "$SNAPMNT" \
  || { rmdir "$SNAPMNT"; echo "could not mount good snapshot"; exit 1; }

SRC="$SNAPMNT/$REL"
DST="$MNT/$REL"

# --- sanity checks on the source (good snapshot) ---------------------------------------------
[[ -d "$SRC" ]] || { echo "good snapshot has no $REL"; umount "$SNAPMNT"; exit 1; }
[[ ! -e "$SRC/Contents" ]] || { echo "good snapshot's Plug-Ins looks damaged too"; umount "$SNAPMNT"; exit 1; }
n=$(ls -1d "$SRC"/*.bundle 2>/dev/null | wc -l | tr -d ' ')
[[ "$n" -ge 3 ]] || { echo "good snapshot has only $n bundles — refusing"; umount "$SNAPMNT"; exit 1; }
for must in AppleGVAHEVCDecoder.bundle AV1DecoderSW.bundle; do
  [[ -d "$SRC/$must" ]] || { echo "good snapshot lacks $must — refusing"; umount "$SNAPMNT"; exit 1; }
done
echo "Apple plug-ins in good snapshot ($n):"; ls -1 "$SRC" | sed 's/^/   /'

# --- restore the damaged folder without deleting it or unrelated files -----------------------
[[ "$DST" == "/System/Volumes/Update/mnt1/System/Library/Video/Plug-Ins" ]] || { echo "unexpected DST $DST"; exit 1; }
echo "damaged folder currently has:"; ls -1 "$DST" 2>/dev/null | sed 's/^/   /' || true
mkdir -p "$DST"
ditto "$SRC" "$DST"

# --- install our two decoders ----------------------------------------------------------------
cdh() { codesign -dvvv "$1" 2>&1 | awk -F= '/^CDHash=/{print $2}'; }
for b in AV1VCNDecoder.bundle VP9VCNDecoder.bundle; do
  ditto "$RES/$b" "$DST/$b"
  chown -R root:wheel "$DST/$b"
  chmod -R go-w "$DST/$b"
  [[ "$(cdh "$DST/$b")" == "$(cdh "$RES/$b")" ]] || { echo "cdhash mismatch for $b"; exit 1; }
  echo "installed $b"
done

# --- final check: every Apple bundle back + ours ----------------------------------------------
missing=0
for a in "$SRC"/*.bundle; do
  [[ -d "$DST/${a:t}" ]] || { echo "MISSING after copy: ${a:t}"; missing=1; }
done
[[ $missing -eq 0 ]] || { echo "copy incomplete; NOT blessing"; exit 1; }
echo "final Plug-Ins:"; ls -1 "$DST" | sed 's/^/   /'

umount "$SNAPMNT" || true
rmdir "$SNAPMNT" 2>/dev/null || true
bless --folder "$MNT/System/Library/CoreServices" --bootefi --create-snapshot
echo
echo "OK: Apple plug-ins restored + AV1/VP9 decoders installed. Reboot now."
