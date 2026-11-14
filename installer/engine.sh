#!/bin/zsh
# zsh is required: this is a macOS installer/build script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
#
# engine.sh — install/uninstall the AMD VCN VideoToolbox plug-ins (AV1 + VP9) into the sealed
# /System/Library/Video/Plug-Ins, plus an idiot-proof preflight. One engine shared by the CLI
# and the graphical installer; the app calls `engine.sh check` to render its preflight and
# `engine.sh install` (as root) to do the work.
#
# Why /System and not /Library: VideoToolbox scans /System/Library/Video/Plug-Ins with no code-
# signature check (figRegistry opt 0); /Library requires an Apple signature our adhoc bundles
# cannot provide, and patching CoreMedia for /Library is not possible on Tahoe (Lilu's userspace
# patcher does not support the split dyld shared cache).
#
# Commands:
#   engine.sh check                 -> JSON preflight to stdout (no root needed)
#   engine.sh install  [--force]    -> install both bundles (root). --force bypasses the
#                                       "driver build not validated" block (GPU-hang risk).
#   engine.sh repair   [--force]    -> restore any missing Apple plug-in in the system Plug-Ins
#                                       folder from Apple's sealed snapshot, then install both
#                                       bundles (root; one reboot)
#   engine.sh uninstall             -> remove both bundles (root)
#
# Resources (bundles + validated-builds.json) are looked up in, in order:
#   $VCNVT_RES, --res <dir>, the script's own directory.
#
set -u
emulate -L zsh

SELF_DIR=${0:A:h}
RES=${VCNVT_RES:-$SELF_DIR}
CMD=${1:-}
shift 2>/dev/null || true
FORCE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --force) FORCE=1 ;;
    --res) shift; RES=$1 ;;
    *) ;;
  esac
  shift
done

AV1=AV1VCNDecoder.bundle
VP9=VP9VCNDecoder.bundle
BUILDS_JSON=$RES/validated-builds.json
PLUGDIR=System/Library/Video/Plug-Ins
MNT=/System/Volumes/Update/mnt1

# ---- small helpers -------------------------------------------------------------------------
jesc() { print -rn -- "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }

CHECKS=()
add_check() { # id label status detail   (status: ok|warn|fail)
  CHECKS+=("{\"id\":\"$(jesc "$1")\",\"label\":\"$(jesc "$2")\",\"status\":\"$(jesc "$3")\",\"detail\":\"$(jesc "$4")\"}")
}

plx() { /usr/bin/plutil -extract "$1" raw -o - "$BUILDS_JSON" 2>/dev/null; }

macos_product() { /usr/bin/sw_vers -productVersion 2>/dev/null; }
macos_build()   { print -r -- "${VCNVT_FAKE_BUILD:-$(/usr/bin/sw_vers -buildVersion 2>/dev/null)}"; }  # FAKE_BUILD: testing only
macos_major()   { macos_product | cut -d. -f1; }

amfi_open() {
  /usr/sbin/nvram boot-args 2>/dev/null | grep -q 'amfi_get_out_of_my_way' && return 0
  /usr/sbin/kextstat 2>/dev/null | grep -qi 'AMFIPass' && return 0
  return 1
}

authroot_disabled() { csrutil authenticated-root status 2>/dev/null | grep -qi disabled; }

gpu_model() { /usr/sbin/system_profiler SPDisplaysDataType 2>/dev/null | awk -F': ' '/Chipset Model/{print $2; exit}'; }

# ---- preflight -----------------------------------------------------------------------------
# Globals set by preflight(): CAN_INSTALL (0/1), BLOCK_OVERRIDABLE (0/1 = only blocker is an
# overridable one, e.g. unvalidated build).
preflight() {
  CAN_INSTALL=1; HARD_FAIL=0; SOFT_FAIL=0; SYS_DAMAGED=0
  CHECKS=()
  # HARD_OTHER = hard failures other than a damaged Plug-Ins folder (which `repair` can fix)
  HARD_OTHER=0

  # 1. macOS in supported range (Sequoia 15 .. Tahoe 26)
  local prod=$(macos_product) major=$(macos_major)
  if [[ -n "$major" && "$major" -ge 15 && "$major" -le 26 ]]; then
    add_check macos "macOS version" ok "$prod (supported: Sequoia 15 – Tahoe 26)"
  else
    add_check macos "macOS version" fail "${prod:-unknown} is outside the supported range (Sequoia 15 – Tahoe 26)"
    HARD_FAIL=1; HARD_OTHER=1
  fi

  # 2. driver build validated (hang risk if not)
  local build=$(macos_build) n hit=0 name=""
  n=$(plx validated 2>/dev/null)
  if [[ -n "$n" ]]; then
    local i=0
    while [[ $i -lt ${n:-0} ]]; do
      if [[ "$(plx validated.$i.build)" == "$build" ]]; then hit=1; name=$(plx validated.$i.name); break; fi
      ((i++))
    done
  fi
  if [[ $hit -eq 1 ]]; then
    add_check driver "GPU driver validated" ok "$name ($build): AMD VCN driver validated"
  else
    add_check driver "GPU driver validated" fail "build $build is not in the validated list — installing may HANG the GPU. Validate it first, or override at your own risk."
    SOFT_FAIL=1
  fi

  # 3. authenticated-root disabled (required to write the sealed system volume)
  if authroot_disabled; then
    add_check authroot "Authenticated-root disabled" ok "sealed /System can be modified"
  else
    add_check authroot "Authenticated-root disabled" fail "run 'csrutil authenticated-root disable' from Recovery first (needs OpenCore low security)"
    HARD_FAIL=1; HARD_OTHER=1
  fi

  # 4. AMFI open (adhoc plug-in cannot load into the media process otherwise)
  if amfi_open; then
    add_check amfi "AMFI open" ok "amfi_get_out_of_my_way / AMFIPass present — adhoc plug-in can load"
  else
    add_check amfi "AMFI open" fail "add amfi_get_out_of_my_way=0x1 to boot-args or load AMFIPass, else the plug-in is rejected"
    HARD_FAIL=1; HARD_OTHER=1
  fi

  # 5. GPU recognized (soft)
  local gpu=$(gpu_model)
  if [[ -n "$gpu" ]] && print -rn -- "$gpu" | grep -qiE 'RX 68| RX 69'; then
    add_check gpu "Supported AMD GPU" ok "$gpu"
  elif [[ -n "$gpu" ]]; then
    add_check gpu "Supported AMD GPU" warn "$gpu — only Navi 21 (RX 6800/6800 XT/6900 XT/6950 XT) is known-good for VCN AV1/VP9"
  else
    add_check gpu "Supported AMD GPU" warn "could not read the GPU model"
  fi

  # 6. system Plug-Ins folder intact (Apple's own decoders present, nothing loose in it).
  #    Not counted in HARD_OTHER: the app can fix this one itself (`repair`).
  local live=/$PLUGDIR
  local -a livebundles=( "$live"/*.bundle(N) )
  if [[ -e "$live/Contents" || ! -d "$live/AppleGVAHEVCDecoder.bundle" || ${#livebundles} -lt 5 ]]; then
    add_check sysplugins "System video plug-ins intact" fail "Apple's own video plug-ins are damaged or missing. Click Repair to restore them from Apple's original system snapshot."
    SYS_DAMAGED=1; HARD_FAIL=1
  else
    add_check sysplugins "System video plug-ins intact" ok "Apple's HEVC/AV1/VP9 plug-ins present"
  fi

  # 7. bundles present
  if [[ -d "$RES/$AV1" && -d "$RES/$VP9" ]]; then
    add_check bundles "Decoder bundles present" ok "AV1 + VP9 bundles found"
  else
    add_check bundles "Decoder bundles present" fail "missing $AV1 or $VP9 next to the installer"
    HARD_FAIL=1; HARD_OTHER=1
  fi

  BLOCK_OVERRIDABLE=0
  if [[ $HARD_FAIL -eq 1 ]]; then CAN_INSTALL=0
  elif [[ $SOFT_FAIL -eq 1 ]]; then CAN_INSTALL=0; BLOCK_OVERRIDABLE=1
  fi
  # repairable: the damaged Plug-Ins folder is the only hard blocker
  REPAIRABLE=0
  [[ $SYS_DAMAGED -eq 1 && $HARD_OTHER -eq 0 ]] && REPAIRABLE=1
  return 0
}

cdhash_of() { codesign -dvvv "$1" 2>&1 | awk -F= '/^CDHash=/{print $2}'; }

# none | current | outdated  — what is in /System/Library/Video/Plug-Ins right now
installed_state() {
  local live=/$PLUGDIR any=0 same=1 b
  for b in $AV1 $VP9; do
    if [[ -d "$live/$b" ]]; then
      any=1
      [[ "$(cdhash_of "$live/$b")" == "$(cdhash_of "$RES/$b")" ]] || same=0
    else
      same=0
    fi
  done
  if [[ $any -eq 0 ]]; then echo none
  elif [[ $same -eq 1 ]]; then echo current
  else echo outdated
  fi
}

emit_check_json() {
  local can=$1 ovr=$2; shift 2
  print -rn -- "{\"canInstall\":$([[ $can -eq 1 ]] && echo true || echo false),"
  print -rn -- "\"installed\":\"$(installed_state)\","
  print -rn -- "\"overridable\":$([[ $ovr -eq 1 ]] && echo true || echo false),"
  print -rn -- "\"repairable\":$([[ ${REPAIRABLE:-0} -eq 1 ]] && echo true || echo false),"
  print -rn -- "\"validated\":$([[ ${SOFT_FAIL:-1} -eq 0 ]] && echo true || echo false),"
  print -rn -- "\"macos\":\"$(jesc "$(macos_product) ($(macos_build))")\","
  print -rn -- "\"checks\":["
  local first=1 c
  for c in "${CHECKS[@]}"; do
    [[ $first -eq 1 ]] || print -rn -- ","
    print -rn -- "$c"; first=0
  done
  print -r -- "]}"
}

# ---- sealed-volume install ----------------------------------------------------------------
# The system volume (/dev/diskNsM) from the booted snapshot (/dev/diskNsMsK). Strict: anything
# else (e.g. a whole container /dev/diskN) is refused.
system_volume() {
  local node
  node=$(diskutil info / | awk -F': *' '/Device Node/{print $2; exit}')
  if [[ "$node" =~ '^(/dev/disk[0-9]+s[0-9]+)s[0-9]+$' ]]; then print -r -- "$match[1]"
  elif [[ "$node" =~ '^/dev/disk[0-9]+s[0-9]+$' ]]; then print -r -- "$node"
  else echo "unexpected boot device '$node'" >&2; return 1
  fi
}

mount_system() {
  local vol
  vol=$(system_volume) || return 1
  echo "system volume $vol -> $MNT"
  mkdir -p "$MNT"
  mount | grep -q " on $MNT " || mount -o nobrowse -t apfs "$vol" "$MNT" || return 1
  [[ -d "$MNT/System/Library/CoreServices" ]] \
    || { echo "$MNT does not look like a system volume" >&2; return 1; }
}

# Put back every Apple plug-in missing from DST, copied from the pristine SRC (Apple's sealed
# snapshot of this same macOS build). Never overwrites or deletes an existing bundle; the only
# thing it removes is a loose "Contents" folder (what the 2026-11-06 bug left behind).
restore_apple_plugins() { # src-dir dst-dir
  local src=$1
  local dst=$2
  case "$dst" in
    */System/Library/Video/Plug-Ins) ;;
    *) echo "REFUSING to repair unexpected path: '$dst'" >&2; return 1 ;;
  esac
  local -a apple=( "$src"/*.bundle(N) )
  [[ -d "$src" && ! -e "$src/Contents" && -d "$src/AppleGVAHEVCDecoder.bundle" && ${#apple} -ge 5 ]] \
    || { echo "Apple's original snapshot does not look right (${#apple} bundles in $src); not touching anything" >&2; return 1; }
  mkdir -p "$dst"
  if [[ -d "$dst/Contents" ]]; then rm -rf -- "$dst/Contents"; echo "removed loose $dst/Contents"; fi
  local a n=0
  for a in "${apple[@]}"; do
    [[ -e "$dst/${a:t}" ]] && continue
    ditto "$a" "$dst/${a:t}" || return 1
    echo "restored ${a:t}"
    n=$((n + 1))
  done
  for a in "${apple[@]}"; do
    [[ -f "$dst/${a:t}/Contents/Info.plist" ]] || { echo "still missing ${a:t}" >&2; return 1; }
  done
  echo "Apple plug-ins OK: ${#apple} present, $n restored"
}

SEALMNT=/private/var/tmp/vcn-sealed-snapshot

do_repair() {
  [[ $EUID -eq 0 ]] || { echo "repair must run as root" >&2; exit 1; }
  preflight
  [[ $HARD_OTHER -eq 0 ]] || { echo "preflight has blocking failures besides the plug-ins folder; aborting" >&2; exit 2; }
  local vol seal
  vol=$(system_volume) || exit 1
  seal=$(diskutil apfs listSnapshots "$vol" 2>/dev/null | awk -F': *' '/Name:/{print $2}' | grep -m1 '^com\.apple\.os\.update-')
  [[ -n "$seal" ]] || { echo "Apple's original system snapshot was not found on $vol" >&2; exit 5; }
  mount_system || exit 1
  mkdir -p "$SEALMNT"
  mount | grep -q " on $SEALMNT " || mount_apfs -s "$seal" -o rdonly,nobrowse "$vol" "$SEALMNT" \
    || { echo "could not mount $seal" >&2; exit 5; }
  echo "pristine source: $seal"
  restore_apple_plugins "$SEALMNT/$PLUGDIR" "$MNT/$PLUGDIR"
  local rc=$?
  umount "$SEALMNT" 2>/dev/null
  [[ $rc -eq 0 ]] || exit 6
  # install our decoders in the same snapshot (one reboot), unless the build is unvalidated
  if [[ $SOFT_FAIL -eq 0 || $FORCE -eq 1 ]]; then
    install_one "$RES/$AV1" "$AV1" || exit 1
    install_one "$RES/$VP9" "$VP9" || exit 1
  else
    echo "driver build not validated: decoders NOT installed (Apple plug-ins repaired only)"
  fi
  echo "blessing new snapshot..."
  bless --folder "$MNT/System/Library/CoreServices" --bootefi --create-snapshot || exit 1
  echo "OK: reboot to apply"
}

# Refuse to delete anything that is not exactly one of our two bundle paths. This is the guard
# that would have caught the 2026-11-06 bug (an empty name turned `rm -rf $dest` into
# `rm -rf .../Video/Plug-Ins`, wiping Apple's plug-ins).
safe_rm_bundle() { # path
  local p=$1
  case "$p" in
    */Video/Plug-Ins/AV1VCNDecoder.bundle|*/Video/Plug-Ins/VP9VCNDecoder.bundle) ;;
    *) echo "REFUSING to delete unexpected path: '$p'" >&2; return 1 ;;
  esac
  rm -rf -- "$p"
}

install_one() { # src-bundle-dir  name
  # NOTE: one assignment per line. In zsh `local a=$1 b=$2 c=$b` expands the whole line before
  # assigning, so $b is still empty when c is built.
  local src=$1
  local name=$2
  [[ "$name" == "$AV1" || "$name" == "$VP9" ]] || { echo "bad bundle name '$name'" >&2; return 1; }
  local dest="$MNT/$PLUGDIR/$name"
  [[ -d "$src" ]] || { echo "missing $src" >&2; return 1; }
  safe_rm_bundle "$dest" || return 1
  ditto "$src" "$dest" || return 1
  for d in "$dest"/Contents/MacOS/*.dSYM(N); do rm -rf -- "$d"; done
  # a downloaded installer is quarantined and ditto would carry that onto the system copy
  xattr -dr com.apple.quarantine "$dest" 2>/dev/null
  chown -R root:wheel "$dest"
  chmod -R go-w "$dest"
  # verify it landed where VideoToolbox looks
  [[ -f "$dest/Contents/Info.plist" && "$(cdhash_of "$dest")" == "$(cdhash_of "$src")" ]] \
    || { echo "verification failed for $dest" >&2; return 1; }
  # the /Library copy is ignored by VideoToolbox (signature check); drop it to avoid confusion
  [[ -e "/Library/Video/Plug-Ins/$name" ]] && safe_rm_bundle "/Library/Video/Plug-Ins/$name"
  echo "installed $dest"
}

do_install() {
  [[ $EUID -eq 0 ]] || { echo "install must run as root" >&2; exit 1; }
  preflight
  if [[ $HARD_FAIL -eq 1 ]]; then
    echo "preflight has blocking failures; aborting" >&2
    emit_check_json 0 0 >&2
    exit 2
  fi
  if [[ $SOFT_FAIL -eq 1 && $FORCE -ne 1 ]]; then
    echo "driver build not validated; re-run with --force to override (GPU-hang risk)" >&2
    exit 3
  fi
  mount_system || exit 1
  # sanity: the target must be the real system Plug-Ins folder, with Apple's plug-ins in it
  [[ -d "$MNT/$PLUGDIR" && ! -e "$MNT/$PLUGDIR/Contents" ]] \
    || { echo "system Plug-Ins folder looks damaged; not touching it" >&2; exit 4; }
  install_one "$RES/$AV1" "$AV1" || exit 1
  install_one "$RES/$VP9" "$VP9" || exit 1
  echo "blessing new snapshot..."
  bless --folder "$MNT/System/Library/CoreServices" --bootefi --create-snapshot
  echo "OK: reboot to load the decoders"
}

do_uninstall() {
  [[ $EUID -eq 0 ]] || { echo "uninstall must run as root" >&2; exit 1; }
  authroot_disabled || { echo "authenticated-root must be disabled" >&2; exit 1; }
  mount_system || exit 1
  safe_rm_bundle "$MNT/$PLUGDIR/$AV1" || exit 1
  safe_rm_bundle "$MNT/$PLUGDIR/$VP9" || exit 1
  echo "removed both bundles"
  bless --folder "$MNT/System/Library/CoreServices" --bootefi --create-snapshot
  echo "OK: reboot to apply"
}

case "$CMD" in
  check)     preflight; emit_check_json $CAN_INSTALL $BLOCK_OVERRIDABLE ;;
  install)   do_install ;;
  repair)    do_repair ;;
  uninstall) do_uninstall ;;
  *) echo "usage: engine.sh {check|install [--force]|repair [--force]|uninstall} [--res DIR]" >&2; exit 64 ;;
esac
