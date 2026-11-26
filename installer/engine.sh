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
#   engine.sh uninstall             -> remove both bundles (root); refuses a volume that is not the system the Mac boots
#   engine.sh uninstall --sealed    -> boot Apple's own sealed snapshot of this macOS again (root); the volume is not touched
#
# Resources (bundles + validated-builds.json) are looked up in, in order:
#   $VCNVT_RES, --res <dir>, the script's own directory.
#
set -u
emulate -L zsh
# sudo keeps the caller's PATH on macOS: only the system's own tools, whatever is in it
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
# Tests (tests/enginetest.sh) put made-up diskutil, mount and bless in front, and a folder for the volume. Only a
# process that is not root takes them: a root engine always runs the system's own tools.
VCNVT_TESTING=0
if [[ -n ${VCNVT_TEST_BIN:-} && $EUID -ne 0 ]]; then
  PATH=$VCNVT_TEST_BIN:$PATH
  VCNVT_TESTING=1
fi

SELF_DIR=${0:A:h}
RES=${VCNVT_RES:-$SELF_DIR}
CMD=${1:-}
shift 2>/dev/null || true
FORCE=0
SEALED=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --force) FORCE=1 ;;
    --sealed) SEALED=1 ;;
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
if [[ ${VCNVT_TESTING:-0} -eq 1 ]]; then MNT=${VCNVT_TEST_MNT:?}; fi
LOG=/var/log/vcn-installer.log
if [[ $VCNVT_TESTING -eq 1 ]]; then LOG=${VCNVT_TEST_MNT}.log; fi

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

  # 4b. Software Update staging a macOS update reverts the volume at boot and drops the new snapshot (soft)
  local sysvol
  sysvol=$(system_volume 2>/dev/null)
  if [[ -n "$sysvol" ]] && update_staged "$sysvol"; then
    add_check swupdate "No macOS update staged" warn "Software Update has staged an update: at the next boot it reverts the system volume to Apple's snapshot, which removes an install. Turn off AutomaticDownload and restart first (see the log of the install)."
  elif auto_download_on; then
    add_check swupdate "No macOS update staged" warn "Automatic download of macOS updates is on: it can stage an update that reverts the system volume at boot. Turn off AutomaticDownload in /Library/Preferences/com.apple.SoftwareUpdate."
  else
    add_check swupdate "No macOS update staged" ok "no update staged, automatic download is off"
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

# What the engine did, on the screen and in $LOG (root only), so a snapshot that does not boot can be traced.
note() { echo "$*"; print -r -- "$(date '+%F %T') $*" >> "$LOG" 2>/dev/null; return 0; }

MOUNTED_HERE=0
mount_system() {
  local vol
  vol=$(system_volume) || return 1
  note "system volume $vol -> $MNT"
  mkdir -p "$MNT"
  local mounted
  mounted=$(mount | awk -v m=" on $MNT " 'index($0, m) { print; exit }')
  if [[ -n "$mounted" ]]; then
    # A mount that is already there (left by an earlier run, or by someone) is used only when it is this very
    # volume and can be written; anything else would be edited and blessed as if it were the system.
    [[ "${mounted%% on *}" == "$vol" ]] \
      || { echo "$MNT is already mounted from ${mounted%% on *}, not from $vol; not touching it" >&2; return 1; }
    [[ "$mounted" != *read-only* ]] \
      || { echo "$MNT is already mounted read-only; unmount it first (umount $MNT)" >&2; return 1; }
    note "$MNT was already mounted from $vol; it is left mounted"
  else
    mount -o nobrowse -t apfs "$vol" "$MNT" || return 1
    MOUNTED_HERE=1
  fi
  [[ -d "$MNT/System/Library/CoreServices" ]] \
    || { echo "$MNT does not look like a system volume" >&2; return 1; }
}

# Unmount the writable system volume if this run mounted it. A mount that was already there
# belongs to someone else and stays.
release_system() {
  [[ $MOUNTED_HERE -eq 1 ]] || return 0
  MOUNTED_HERE=0
  umount "$MNT" 2>/dev/null || echo "note: $MNT is still mounted; it goes away at reboot" >&2
}

# ---- snapshots ----------------------------------------------------------------------------
# The snapshots of the system volume, one per line: uuid, name, xid, RootTo (1: the Mac boots this one next),
# Purgeable, RevertTo (1: the volume goes back to this one at its next mount), separated by tabs.
snapshot_list() { # vol
  local j n=0 u
  # diskutil lists the snapshots of a volume that is mounted: when it is not (before the install mounts it), ask
  # through /, which is mounted from one of its snapshots
  j=$(diskutil apfs listSnapshots -plist "$1" 2>/dev/null)
  [[ -n "$j" ]] || j=$(diskutil apfs listSnapshots -plist / 2>/dev/null)
  [[ -n "$j" ]] || return 1
  local -a f
  while u=$(print -r -- "$j" | /usr/bin/plutil -extract Snapshots.$n.SnapshotUUID raw -o - - 2>/dev/null) && [[ -n "$u" ]]; do
    f=()
    local k
    for k in SnapshotName SnapshotXID RootTo Purgeable RevertTo; do
      f+=( "$(print -r -- "$j" | /usr/bin/plutil -extract Snapshots.$n.$k raw -o - - 2>/dev/null)" )
    done
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$u" "${f[1]}" "${f[2]}" "${f[3]}" "${f[4]}" "${f[5]}"
    n=$((n + 1))
  done
}
# uuid of the snapshot the Mac boots next (RootTo), or empty
root_snapshot() { snapshot_list "$1" | awk -F'\t' '$4 == "1" || $4 == "true" { print $1; exit }'; }
# uuid of the snapshot the volume reverts to at its next mount (RevertTo), or empty
revert_snapshot() { snapshot_list "$1" | awk -F'\t' '$6 == "1" || $6 == "true" { print $1; exit }'; }
snapshot_name() { snapshot_list "$1" | awk -F'\t' -v u="$2" '$1 == u { print $2; exit }'; }
# uuid of the snapshot / is mounted from
booted_snapshot() { diskutil info / 2>/dev/null | awk -F': *' '/APFS Snapshot UUID/ { print $2; exit }'; }

# com.apple.MobileSoftwareUpdate (the part of Software Update that stages a macOS update, "MSUPrepareUpdate") unmounts
# the system volume a few seconds after boot and reverts it to a snapshot (kernel log, 2026-11-26 19:51:04: "unmounting
# volume, requested by com.apple.MobileSoftwareUpdate", "volume will revert to snapshot com.apple.os.update-...",
# "free'ing allocated extents for snap_xid 2618995 / 2643232 / 2670051": every snapshot newer than the target is
# deleted, the blessed root among them). The target is the snapshot the volume revert is set to (RevertTo). A snapshot
# blessed while the revert is set to another one is gone at the next mount, and the Mac boots Apple's own system:
# what was installed or removed has no effect, and the volume may be left in a state the root snapshot does not match.
update_staged() { snapshot_list "$1" | awk -F'\t' '$2 == "com.apple.os.update-MSUPrepareUpdate" { found = 1 } END { exit !found }'; }
auto_download_on() {
  local v
  v=$(defaults read /Library/Preferences/com.apple.SoftwareUpdate AutomaticDownload 2>/dev/null)
  [[ "$v" == 1 ]]
}
UPDATE_ADVICE='Turn off the automatic download of macOS updates, then remove what it staged:
  sudo defaults write /Library/Preferences/com.apple.SoftwareUpdate AutomaticDownload -bool false
  sudo defaults write /Library/Preferences/com.apple.SoftwareUpdate AutomaticCheckEnabled -bool false
  sudo softwareupdate --schedule off
and restart; Software Update drops its staged update when it is turned off. Then run this again.'

mount_snapshot() { # vol name  -> prints the read-only mount folder
  local d
  d=$(mktemp -d /private/var/tmp/vcn-snapshot.XXXXXX) || return 1
  mount_apfs -s "$2" -o rdonly,nobrowse "$1" "$d" >/dev/null 2>&1 || { rmdir "$d"; return 1; }
  print -r -- "$d"
}
unmount_snapshot() { umount "$1" 2>/dev/null; rmdir "$1" 2>/dev/null; return 0; }

# What the system volume holds that matters for booting, as one hash: the version, the video plug-ins folder with
# the date of every entry, the kernel collections, and how many extensions there are.
volume_fingerprint() { # root of a mounted system volume
  (
    cd "$1" 2>/dev/null || exit 1
    shasum -a 256 System/Library/CoreServices/SystemVersion.plist 2>/dev/null
    stat -f '%N %z %m' System/Library/Video/Plug-Ins System/Library/Video/Plug-Ins/*(N) 2>/dev/null
    stat -f '%N %z' System/Library/KernelCollections/*(N) 2>/dev/null
    ls -A System/Library/Extensions 2>/dev/null | wc -l
  ) | shasum -a 256 | awk '{ print $1 }'
}

# The snapshot of the live volume that bless makes is the system the Mac boots next. It is only that system when
# the live volume still holds what the Mac boots now (or what it was blessed to boot next): when something put the
# volume back (a revert, a bless from Recovery, an update step), a snapshot made from it is an older system with
# newer boot files beside it, and that Mac does not boot. So the volume is compared with the root snapshot first.
require_volume_matches_root() { # vol
  local vol=$1 base d live snap
  base=$(root_snapshot "$vol")
  [[ -n "$base" ]] || base=$(booted_snapshot)
  [[ -n "$base" ]] || { echo "cannot tell which snapshot the Mac boots; refusing to change the volume" >&2; return 1; }
  local rv
  rv=$(revert_snapshot "$vol")
  if [[ -n "$rv" && "$rv" != "$base" ]]; then
    echo "The volume is set to revert to snapshot $(snapshot_name "$vol" "$rv") at its next mount (com.apple.MobileSoftwareUpdate does that), not to the snapshot the Mac boots ($(snapshot_name "$vol" "$base")): a snapshot blessed now is deleted by that revert. Nothing was changed." >&2
    print -r -- "$UPDATE_ADVICE" >&2
    return 1
  fi
  local lv
  lv=$(/usr/bin/plutil -extract ProductBuildVersion raw -o - "$MNT/System/Library/CoreServices/SystemVersion.plist" 2>/dev/null)
  if [[ -n "$lv" && "$lv" != "$(macos_build)" ]]; then
    cat >&2 <<EOF
The system volume holds macOS $(/usr/bin/plutil -extract ProductUserVisibleVersion raw -o - "$MNT/System/Library/CoreServices/SystemVersion.plist" 2>/dev/null) ($lv), but this Mac is running $(macos_product) ($(macos_build)).
A boot snapshot made from the volume would be that other macOS, set to boot with this macOS's boot files: it does not
boot, and Recovery is needed to bless a working snapshot again. Nothing was changed.
This happens when a macOS update was applied to the volume (Software Update stages one) and the Mac then went back
to the older snapshot. To remove the decoders without touching the volume: engine.sh uninstall --sealed (puts the
Mac back on Apple's own sealed snapshot of this macOS), or let the update finish by booting the new system.
EOF
    return 1
  fi
  local name
  name=$(snapshot_name "$vol" "$base")
  [[ -n "$name" ]] || { echo "snapshot $base is not on $vol; refusing to change the volume" >&2; return 1; }
  d=$(mount_snapshot "$vol" "$name") || { echo "could not mount snapshot $name to compare; refusing" >&2; return 1; }
  live=$(volume_fingerprint "$MNT")
  snap=$(volume_fingerprint "$d")
  unmount_snapshot "$d"
  if [[ -z "$live" || "$live" != "$snap" ]]; then
    cat >&2 <<EOF
The system volume is not the system the Mac boots from (root snapshot $name):
a new boot snapshot made from it would be an older or different system, and could fail to boot. Nothing was changed.
This happens after the volume was reverted, or a snapshot was blessed from Recovery. To start from a clean state:
restart, and if the Mac is running Apple's own system again, run this once more; to remove the decoders without
touching the volume run: engine.sh uninstall --sealed (or, in Recovery,
bless --mount /Volumes/<your system volume> --bootefi --last-sealed-snapshot)
EOF
    return 1
  fi
  note "the volume matches the root snapshot $name"
}

rollback_root() { # uuid of the snapshot to boot again
  [[ -n "$1" ]] || return 0
  echo "putting the root snapshot back to $1" >&2
  bless --mount "$MNT" --snapshot "$1" >&2 \
    || echo "COULD NOT put it back. In Recovery run: bless --mount /Volumes/<your system volume> --snapshot $1" >&2
}

# Is the snapshot a system that boots, and does it hold our bundles as intended?  want: present | absent | any
verify_snapshot() { # vol uuid want
  local vol=$1 uuid=$2 want=$3 name d rc=0 b
  name=$(snapshot_name "$vol" "$uuid")
  [[ -n "$name" ]] || { echo "the new snapshot $uuid is not on $vol" >&2; return 1; }
  d=$(mount_snapshot "$vol" "$name") || { echo "could not mount the new snapshot $name" >&2; return 1; }
  local sv="$d/System/Library/CoreServices/SystemVersion.plist"
  [[ -f "$sv" ]] || { echo "the new snapshot has no SystemVersion.plist" >&2; rc=1; }
  if [[ -f "$sv" && "$(/usr/bin/plutil -extract ProductBuildVersion raw -o - "$sv" 2>/dev/null)" != "$(macos_build)" ]]; then
    echo "the new snapshot is macOS $(/usr/bin/plutil -extract ProductBuildVersion raw -o - "$sv" 2>/dev/null), not $(macos_build)" >&2; rc=1
  fi
  [[ -f "$d/System/Library/CoreServices/boot.efi" ]] || { echo "the new snapshot has no boot.efi" >&2; rc=1; }
  local -a kcs=( "$d"/System/Library/KernelCollections/*.kc(N) )
  (( $#kcs > 0 )) || { echo "the new snapshot has no kernel collection" >&2; rc=1; }
  local -a apple=( "$d/$PLUGDIR"/Apple*.bundle(N) )
  [[ -d "$d/$PLUGDIR/AppleGVAHEVCDecoder.bundle" && $#apple -ge 5 ]] \
    || { echo "the new snapshot's video plug-ins folder is not Apple's (${#apple} Apple bundles)" >&2; rc=1; }
  for b in $AV1 $VP9; do
    [[ $want == any ]] && continue
    if [[ $want == present && ! -f "$d/$PLUGDIR/$b/Contents/Info.plist" ]]; then echo "the new snapshot lacks $b" >&2; rc=1; fi
    if [[ $want == absent && -e "$d/$PLUGDIR/$b" ]]; then echo "the new snapshot still holds $b" >&2; rc=1; fi
  done
  unmount_snapshot "$d"
  return $rc
}

# Bless a new snapshot of the edited volume, check that it is the one the Mac will boot and that it is a system
# that boots, and unmount the volume. When bless fails, or the result is not right, the Mac is set to boot the
# snapshot it boots now, and this fails, so the app does not report success.
bless_snapshot() { # what (for the message)  want: present | absent
  local what=$1 want=${2:-present} vol prev new
  vol=$(system_volume) || return 1
  prev=$(root_snapshot "$vol")
  [[ -n "$prev" ]] || prev=$(booted_snapshot)
  note "snapshots before: $(snapshot_list "$vol" | awk -F'\t' '{ printf "%s%s(xid %s%s) ", $2, ($4 == "1" ? " ROOT" : ""), $3, ($5 == "1" ? ", purgeable" : "") }')"
  note "blessing new snapshot..."
  if ! bless --folder "$MNT/System/Library/CoreServices" --bootefi --create-snapshot; then
    echo "bless failed: the Mac keeps booting the current snapshot, so $what does not take effect at reboot. The live system volume was already changed; run this again before rebooting." >&2
    release_system
    return 1
  fi
  new=$(root_snapshot "$vol")
  if [[ -z "$new" || "$new" == "$prev" ]]; then
    echo "bless reported success, but the Mac is not set to boot a new snapshot (root: ${new:-none}); $what does not take effect." >&2
    rollback_root "$prev"; release_system; return 1
  fi
  local rv
  rv=$(revert_snapshot "$vol")
  if [[ -n "$rv" && "$rv" != "$new" ]]; then
    echo "the volume is set to revert to $(snapshot_name "$vol" "$rv") at its next mount, which deletes the new snapshot; $what does not take effect." >&2
    print -r -- "$UPDATE_ADVICE" >&2
    rollback_root "$prev"; release_system; return 1
  fi
  note "snapshots after: $(snapshot_list "$vol" | awk -F'\t' '{ printf "%s%s(xid %s%s) ", $2, ($4 == "1" ? " ROOT" : ""), $3, ($5 == "1" ? ", purgeable" : "") }')"
  if ! verify_snapshot "$vol" "$new" "$want"; then
    echo "the new snapshot is not a system the Mac can boot; $what was not applied." >&2
    rollback_root "$prev"; release_system; return 1
  fi
  sync
  sleep 2
  if [[ "$(root_snapshot "$vol")" != "$new" ]]; then
    echo "the new snapshot disappeared right after it was made; $what was not applied." >&2
    rollback_root "$prev"; release_system; return 1
  fi
  release_system
  note "the Mac will boot $(snapshot_name "$vol" "$new") next"
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

do_repair() {
  [[ $EUID -eq 0 || $VCNVT_TESTING -eq 1 ]] || { echo "repair must run as root" >&2; exit 1; }
  preflight
  [[ $HARD_OTHER -eq 0 ]] || { echo "preflight has blocking failures besides the plug-ins folder; aborting" >&2; exit 2; }
  local vol seal
  vol=$(system_volume) || exit 1
  seal=$(diskutil apfs listSnapshots "$vol" 2>/dev/null | awk -F': *' '/Name:/{print $2}' | grep -m1 -E '^com\.apple\.os\.update-[0-9A-Fa-f]{64}$')
  [[ -n "$seal" ]] || { echo "Apple's original system snapshot was not found on $vol" >&2; exit 5; }
  mount_system || exit 1
  require_volume_matches_root "$vol" || exit 7
  if update_staged "$vol"; then
    echo "A macOS update is staged on the system volume: at the next boot Software Update reverts the volume to Apple's snapshot. Nothing was changed." >&2
    print -r -- "$UPDATE_ADVICE" >&2
    exit 8
  fi
  # A new root-owned folder for every repair. A fixed name in /private/var/tmp, which every
  # user can write, let another user mount a disk image of their own there first: the old
  # code then skipped the mount and copied that image's "Apple" plug-ins onto /System.
  local sealmnt
  sealmnt=$(mktemp -d /private/var/tmp/vcn-sealed-snapshot.XXXXXX) \
    || { echo "could not make a mount point for $seal" >&2; exit 5; }
  mount_apfs -s "$seal" -o rdonly,nobrowse "$vol" "$sealmnt" \
    || { rmdir "$sealmnt"; echo "could not mount $seal" >&2; exit 5; }
  echo "pristine source: $seal"
  restore_apple_plugins "$sealmnt/$PLUGDIR" "$MNT/$PLUGDIR"
  local rc=$?
  umount "$sealmnt" 2>/dev/null && rmdir "$sealmnt"
  [[ $rc -eq 0 ]] || exit 6
  # install our decoders in the same snapshot (one reboot), unless the build is unvalidated
  local want=any
  if [[ $SOFT_FAIL -eq 0 || $FORCE -eq 1 ]]; then
    install_one "$RES/$AV1" "$AV1" || exit 1
    install_one "$RES/$VP9" "$VP9" || exit 1
    want=present
  else
    echo "driver build not validated: decoders NOT installed (Apple plug-ins repaired only)"
    want=any  # an older install of ours may still be there
  fi
  bless_snapshot "the repair" $want || exit 1
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
  # a downloaded installer is quarantined and ditto would carry that onto the system copy.
  # -s: act on a link itself, never on what it points to (this runs as root).
  xattr -drs com.apple.quarantine "$dest" 2>/dev/null
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
  [[ $EUID -eq 0 || $VCNVT_TESTING -eq 1 ]] || { echo "install must run as root" >&2; exit 1; }
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
  require_volume_matches_root "$(system_volume)" || exit 7
  if update_staged "$(system_volume)"; then
    echo "A macOS update is staged on the system volume: at the next boot Software Update reverts the volume to Apple's snapshot and the install is gone (kernel: handle_revert_to_snapshot). Nothing was changed." >&2
    print -r -- "$UPDATE_ADVICE" >&2
    exit 8
  fi
  install_one "$RES/$AV1" "$AV1" || exit 1
  install_one "$RES/$VP9" "$VP9" || exit 1
  bless_snapshot "the install" present || exit 1
  echo "OK: reboot to load the decoders"
}

# Uninstall without touching the live volume: boot Apple's own sealed snapshot of this macOS again. That snapshot
# was made by the macOS installer, before the decoders, so it holds none of them. It is only used when it is the
# macOS that is running (same build) and holds none of our bundles.
do_uninstall_sealed() {
  local vol seal uuid d rc=0 prev
  vol=$(system_volume) || exit 1
  seal=$(snapshot_list "$vol" | awk -F'\t' '$2 ~ /^com\.apple\.os\.update-[0-9A-Fa-f]+$/ && length($2) == 84 { print $1 "\t" $2; exit }')
  [[ -n "$seal" ]] || { echo "Apple's original sealed snapshot was not found on $vol" >&2; exit 5; }
  uuid=${seal%%$'\t'*}
  d=$(mount_snapshot "$vol" "${seal#*$'\t'}") || { echo "could not mount ${seal#*$'\t'}" >&2; exit 5; }
  local sv="$d/System/Library/CoreServices/SystemVersion.plist"
  if [[ "$(/usr/bin/plutil -extract ProductBuildVersion raw -o - "$sv" 2>/dev/null)" != "$(macos_build)" ]]; then
    echo "Apple's sealed snapshot is macOS $(/usr/bin/plutil -extract ProductBuildVersion raw -o - "$sv" 2>/dev/null), not the $(macos_build) that is running; not using it" >&2; rc=1
  fi
  [[ ! -e "$d/$PLUGDIR/$AV1" && ! -e "$d/$PLUGDIR/$VP9" && -f "$d/System/Library/CoreServices/boot.efi" ]] \
    || { echo "Apple's sealed snapshot is not a clean system; not using it" >&2; rc=1; }
  unmount_snapshot "$d"
  [[ $rc -eq 0 ]] || exit 1
  prev=$(root_snapshot "$vol")
  note "going back to Apple's sealed snapshot ${seal#*$'\t'} (root was: ${prev:-none})"
  if ! bless --mount / --bootefi --last-sealed-snapshot; then
    echo "bless failed: the Mac keeps booting the current snapshot" >&2; exit 1
  fi
  [[ "$(root_snapshot "$vol")" == "$uuid" ]] || {
    echo "bless reported success, but the Mac is not set to boot Apple's sealed snapshot" >&2
    rollback_root "$prev"; exit 1; }
  note "the Mac will boot Apple's sealed snapshot next"
  echo "OK: reboot to apply"
}

do_uninstall() {
  [[ $EUID -eq 0 || $VCNVT_TESTING -eq 1 ]] || { echo "uninstall must run as root" >&2; exit 1; }
  if [[ $SEALED -eq 1 ]]; then do_uninstall_sealed; exit $?; fi
  authroot_disabled || { echo "authenticated-root must be disabled" >&2; exit 1; }
  mount_system || exit 1
  require_volume_matches_root "$(system_volume)" || exit 7
  if update_staged "$(system_volume)"; then
    note "warning: a macOS update is staged; Software Update may revert the volume at the next boot (the decoders are then gone too)"
  fi
  if [[ ! -e "$MNT/$PLUGDIR/$AV1" && ! -e "$MNT/$PLUGDIR/$VP9" ]]; then
    # Nothing to remove from a volume that matches what the Mac boots next: no new snapshot is made for that.
    note "the decoders are not on the system volume; nothing to remove"
    release_system
    echo "OK: nothing to do"
    exit 0
  fi
  safe_rm_bundle "$MNT/$PLUGDIR/$AV1" || exit 1
  safe_rm_bundle "$MNT/$PLUGDIR/$VP9" || exit 1
  note "removed both bundles"
  bless_snapshot "the uninstall" absent || exit 1
  echo "OK: reboot to apply"
}

# Every way out of the script gives the volume back if this run mounted it. At top level on purpose: in zsh an EXIT
# trap set inside a function runs when that function returns, which would unmount the volume under the install.
trap release_system EXIT

case "$CMD" in
  check)     preflight; emit_check_json $CAN_INSTALL $BLOCK_OVERRIDABLE ;;
  install)   do_install ;;
  repair)    do_repair ;;
  uninstall) do_uninstall ;;
  *) echo "usage: engine.sh {check|install [--force]|repair [--force]|uninstall [--sealed]} [--res DIR]" >&2; exit 64 ;;
esac
