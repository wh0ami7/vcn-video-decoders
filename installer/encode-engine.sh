#!/bin/zsh
# zsh is required: this is a macOS installer/build script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
#
# encode-engine.sh — the optional "encoding fix" component of the installer app: VCNEncodeFix, which fixes slow or
# stuttering hardware HEVC/H.264 encoding (OBS, VideoToolbox) on Navi 21. It is a separate product with its own
# scripts (VCNEncodeFix/install.sh, uninstall.sh); this file only adds the preflight the app shows and the EFI
# handling. It never touches the decoders or the system volume.
#
# VCNEncodeFix has two halves:
#   service  /usr/local/libexec/vcnencodefix as LaunchDaemon com.droga.vcnencodefix; works at once, no reboot
#   kext     VCNEncodeFloor.kext in the OpenCore EFI (+ one Kernel > Add entry in config.plist); after a reboot
#
# Commands:
#   encode-engine.sh check                         JSON preflight to stdout (no root needed)
#   encode-engine.sh install   [--efi diskNsM]     install/update the service; with --efi also the kext (root)
#   encode-engine.sh uninstall [--efi diskNsM]     remove the service; with --efi also the kext (root)
# With --efi the partition is mounted if needed (and unmounted again afterwards), checked to be an OpenCore EFI
# with Lilu, and handed to VCNEncodeFix's own scripts, which back up config.plist and verify the edit.
#
# Resources: $VCNVT_RES, --res <dir>, or the script's own directory; the package is <res>/VCNEncodeFix.
#
set -u
emulate -L zsh

SELF_DIR=${0:A:h}
RES=${VCNVT_RES:-$SELF_DIR}
CMD=${1:-}
shift 2>/dev/null || true
EFI_ID=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --res) shift; RES=$1 ;;
    --efi) shift; EFI_ID=${1:-} ;;
    *) ;;
  esac
  shift
done

PKG=$RES/VCNEncodeFix
TOOL=$PKG/bin/vcnencodefix
BUILDS_JSON=$RES/validated-builds.json
LABEL=com.droga.vcnencodefix
BIN_DST=/usr/local/libexec/vcnencodefix
PLIST_DST=/Library/LaunchDaemons/$LABEL.plist

jesc() { print -rn -- "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }
CHECKS=()
add_check() { # id label status detail   (status: ok|warn|fail)
  CHECKS+=("{\"id\":\"$(jesc "$1")\",\"label\":\"$(jesc "$2")\",\"status\":\"$(jesc "$3")\",\"detail\":\"$(jesc "$4")\"}")
}
plx() { /usr/bin/plutil -extract "$1" raw -o - "$BUILDS_JSON" 2>/dev/null; }
macos_build() { print -r -- "${VCNVT_FAKE_BUILD:-$(/usr/bin/sw_vers -buildVersion 2>/dev/null)}"; }  # FAKE_BUILD: testing only
pkg_version() { cat "$PKG/VERSION" 2>/dev/null || echo "?"; }

# ---- preflight -----------------------------------------------------------------------------
preflight() {
  CHECKS=()
  CAN_INSTALL=1

  if [[ -x $TOOL && -f $PKG/install.sh ]]; then
    add_check enc-bundle "Encoding fix package present" ok "VCNEncodeFix $(pkg_version)"
  else
    add_check enc-bundle "Encoding fix package present" fail "VCNEncodeFix is missing from the app; download it again"
    CAN_INSTALL=0
  fi

  # 1. this macOS build validated for encoding (the patches target exact Apple and AMD binaries)
  local build=$(macos_build) n i=0 hit="" name=""
  n=$(plx validated)
  while [[ $i -lt ${n:-0} ]]; do
    if [[ "$(plx validated.$i.build)" == "$build" ]]; then hit=$(plx validated.$i.encode); name=$(plx validated.$i.name); break; fi
    ((i++))
  done
  if [[ "$hit" == "true" ]]; then
    add_check enc-build "macOS build validated for encoding" ok "$name ($build)"
  else
    add_check enc-build "macOS build validated for encoding" fail "build $build is not validated for the encoding fix (only Sequoia 15.8.2 and 15.8.3, and Tahoe 26.7.3, so far). Both halves would stay off."
    CAN_INSTALL=0
  fi

  # 2. the exact Apple/AMD files the service patches are present (the tool checks UUIDs and bytes)
  if [[ -x $TOOL ]]; then
    local st
    st=$("$TOOL" status 2>/dev/null)
    local pre=0 wide=0
    print -r -- "$st" | grep -q 'preset  available' && pre=1
    print -r -- "$st" | grep -q 'wide    available' && wide=1
    if (( pre && wide )); then
      add_check enc-files "Encoder files match" ok "speed preset and >4096-wide fix both apply"
    elif (( pre || wide )); then
      add_check enc-files "Encoder files match" warn "only the $( (( pre )) && echo 'speed preset' || echo '>4096-wide fix') applies on this system"
    else
      add_check enc-files "Encoder files match" fail "neither service patch applies on this system"
      CAN_INSTALL=0
    fi
    # the service needs the encoder process's task port, which macOS only gives root with SIP's Debugging
    # Restrictions off (csr-active-config bit 0x4); without it the kext half still works
    if print -r -- "$st" | grep -q 'access  allowed'; then
      add_check enc-access "Access to the encoder process" ok "SIP Debugging Restrictions are off"
    elif print -r -- "$st" | grep -q 'access  REFUSED'; then
      add_check enc-access "Access to the encoder process" warn "SIP Debugging Restrictions are on, so the service cannot patch the encoder: add 0x4 to csr-active-config in OpenCore and reboot (see vcnencodefix/README.md). The clock-floor kext works without it."
    fi
  fi

  # 3. Navi 21 (the package's install.sh refuses anything else)
  local gpu=$(/usr/sbin/system_profiler SPDisplaysDataType 2>/dev/null | awk -F': ' '/Chipset Model/{m=$2} /Device ID/{print m" ("$2")"; exit}')
  if print -rn -- "$gpu" | grep -qiE '\(0x73(a[0-9a-f]|bf)\)'; then
    add_check enc-gpu "Navi 21 GPU" ok "$gpu"
  else
    add_check enc-gpu "Navi 21 GPU" fail "${gpu:-no AMD GPU found}: the encoding fix is only for Navi 21 (RX 6800 – 6950 XT)"
    CAN_INSTALL=0
  fi

  # 4. Lilu (only the kext needs it)
  local lilu=$(/usr/sbin/kextstat -l -b as.vit9696.Lilu 2>/dev/null | awk '{print $7}' | tr -d '()')
  if [[ -n $lilu ]]; then
    add_check enc-lilu "Lilu loaded" ok "Lilu $lilu (needed by the clock-floor kext)"
  else
    add_check enc-lilu "Lilu loaded" warn "Lilu is not loaded: install the service only, or add Lilu to OpenCore first"
  fi
}

# none | current | outdated
installed_state() {
  [[ -e $PLIST_DST ]] || { echo none; return; }
  cmp -s "$BIN_DST" "$TOOL" && echo current || echo outdated
}

kext_line() {
  local kv=$(/usr/sbin/kextstat -l -b com.droga.VCNEncodeFloor 2>/dev/null | awk '{print $7}' | tr -d '()')
  [[ -n $kv ]] || { echo "not loaded"; return; }
  local state=$(/usr/sbin/ioreg -rd1 -c IOResources -k VCNEncodeFloor | awk -F'"' '$2 == "VCNEncodeFloor" {print $4}')
  echo "loaded $kv, ${state:-no state published}"
}

# One JSON object per EFI partition. For mounted ones, whether it holds OpenCore and the kext entry.
efi_json() {
  local first=1 id
  print -rn -- "["
  for id in $(/usr/sbin/diskutil list 2>/dev/null | awk '$2 == "EFI" {print $NF}'); do
    [[ $id =~ '^disk[0-9]+s[0-9]+$' ]] || continue
    local info=$(/usr/sbin/diskutil info $id 2>/dev/null)
    local name=$(print -r -- "$info" | awk -F': *' '/Volume Name:/{print $2; exit}')
    local mounted=$(print -r -- "$info" | awk -F': *' '/Mounted:/{print $2; exit}')
    local mp=$(print -r -- "$info" | awk -F': *' '/Mount Point:/{print $2; exit}')
    local whole=$(print -r -- "$info" | awk -F': *' '/Part of Whole:/{print $2; exit}')
    local media=$(/usr/sbin/diskutil info $whole 2>/dev/null | awk -F': *' '/Device \/ Media Name:/{print $2; exit}')
    local oc=unknown entry=unknown kext=unknown
    if [[ $mounted == Yes && -n $mp ]]; then
      if [[ -f $mp/EFI/OC/config.plist ]]; then
        oc=yes
        if "$TOOL" efi-check "$mp/EFI/OC/config.plist" 2>/dev/null | grep -q 'VCNEncodeFloor.kext listed'; then entry=yes; else entry=no; fi
        local k=$mp/EFI/OC/Kexts/VCNEncodeFloor.kext/Contents/MacOS/VCNEncodeFloor
        if [[ ! -f $k ]]; then kext=missing
        elif cmp -s "$k" "$PKG/Kexts/VCNEncodeFloor.kext/Contents/MacOS/VCNEncodeFloor"; then kext=current
        else kext=outdated
        fi
      else
        oc=no
      fi
    fi
    [[ $first -eq 1 ]] || print -rn -- ","
    first=0
    print -rn -- "{\"id\":\"$id\",\"name\":\"$(jesc "${name:-EFI}")\",\"media\":\"$(jesc "${media:-?}")\",\"mounted\":$([[ $mounted == Yes ]] && echo true || echo false),"
    print -rn -- "\"mountPoint\":\"$(jesc "$mp")\",\"openCore\":\"$oc\",\"entry\":\"$entry\",\"kext\":\"$kext\"}"
  done
  print -rn -- "]"
}

emit_check_json() {
  local svc=none
  if [[ -e $PLIST_DST ]]; then
    pgrep -f "^$BIN_DST watch" >/dev/null && svc=running || svc=stopped
  fi
  print -rn -- "{\"canInstall\":$([[ $CAN_INSTALL -eq 1 ]] && echo true || echo false),"
  print -rn -- "\"version\":\"$(jesc "$(pkg_version)")\",\"installed\":\"$(installed_state)\",\"service\":\"$svc\","
  print -rn -- "\"kext\":\"$(jesc "$(kext_line)")\",\"efis\":$(efi_json),\"checks\":["
  local first=1 c
  for c in "${CHECKS[@]}"; do
    [[ $first -eq 1 ]] || print -rn -- ","
    print -rn -- "$c"; first=0
  done
  print -r -- "]}"
}

# ---- EFI handling (root) -------------------------------------------------------------------
MOUNTED_BY_US=0
EFI_MP=""
efi_open() { # id -> sets EFI_MP
  local id=$1
  [[ $id =~ '^disk[0-9]+s[0-9]+$' ]] || { echo "bad EFI partition '$id'" >&2; return 1; }
  local info=$(/usr/sbin/diskutil info $id 2>/dev/null)
  print -r -- "$info" | grep -qE 'Partition Type: +EFI$' || { echo "$id is not an EFI partition" >&2; return 1; }
  if ! print -r -- "$info" | grep -qE 'Mounted: +Yes'; then
    /usr/sbin/diskutil mount $id >/dev/null || { echo "could not mount $id" >&2; return 1; }
    MOUNTED_BY_US=1
  fi
  EFI_MP=$(/usr/sbin/diskutil info $id | awk -F': *' '/Mount Point:/{print $2; exit}')
  [[ -n $EFI_MP && -f $EFI_MP/EFI/OC/config.plist && -f $EFI_MP/EFI/OC/OpenCore.efi ]] \
    || { echo "$id has no OpenCore (EFI/OC/config.plist and OpenCore.efi) — pick the EFI your Mac boots from" >&2; efi_close $id; return 1; }
  [[ -d $EFI_MP/EFI/OC/Kexts/Lilu.kext ]] || { echo "$id has no Lilu.kext in EFI/OC/Kexts; the kext needs Lilu" >&2; efi_close $id; return 1; }
  echo "EFI $id at $EFI_MP"
}
efi_close() { # id
  if (( MOUNTED_BY_US )); then /usr/sbin/diskutil unmount $1 >/dev/null 2>&1 && echo "unmounted $1"; MOUNTED_BY_US=0; fi
}

do_install() {
  [[ $EUID -eq 0 ]] || { echo "install must run as root" >&2; exit 1; }
  preflight
  [[ $CAN_INSTALL -eq 1 ]] || { echo "the encoding fix cannot be installed on this Mac (see the checks)" >&2; exit 2; }
  local rc
  if [[ -n $EFI_ID ]]; then
    efi_open $EFI_ID || exit 1
    /bin/zsh "$PKG/install.sh" --efi "$EFI_MP" 2>&1
    rc=$?
    efi_close $EFI_ID
  else
    /bin/zsh "$PKG/install.sh" 2>&1
    rc=$?
  fi
  [[ $rc -eq 0 ]] || { echo "VCNEncodeFix install.sh failed ($rc); see the lines above" >&2; exit $rc; }
  echo "OK"
}

do_uninstall() {
  [[ $EUID -eq 0 ]] || { echo "uninstall must run as root" >&2; exit 1; }
  [[ -f $PKG/uninstall.sh ]] || { echo "VCNEncodeFix is missing from the app" >&2; exit 1; }
  local rc
  if [[ -n $EFI_ID ]]; then
    efi_open $EFI_ID || exit 1
    /bin/zsh "$PKG/uninstall.sh" --efi "$EFI_MP" 2>&1
    rc=$?
    efi_close $EFI_ID
  else
    /bin/zsh "$PKG/uninstall.sh" 2>&1
    rc=$?
  fi
  [[ $rc -eq 0 ]] || { echo "VCNEncodeFix uninstall.sh failed ($rc); see the lines above" >&2; exit $rc; }
  echo "OK"
}

case "$CMD" in
  check)     preflight; emit_check_json ;;
  install)   do_install ;;
  uninstall) do_uninstall ;;
  *) echo "usage: encode-engine.sh {check|install [--efi diskNsM]|uninstall [--efi diskNsM]} [--res DIR]" >&2; exit 64 ;;
esac
