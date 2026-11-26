#!/bin/zsh
# AMD VCN Video Decoders - command-line uninstaller.
#
#   ./uninstall.sh                    remove the AV1 and VP9 decoders from the system volume
#   ./uninstall.sh --yes              do not ask first
#   ./uninstall.sh --encode-fix       also remove VCNEncodeFix (the service)
#   ./uninstall.sh --encode-fix --efi disk6s1
#                                     ... and its clock-floor kext from that OpenCore EFI partition
#   ./uninstall.sh --encode-fix-only  only VCNEncodeFix, leave the decoders
#   ./uninstall.sh --sealed           put the Mac back on Apple's own sealed snapshot of this macOS instead of editing the
#                                     system volume: use it when the plain uninstall refuses because the volume holds
#                                     another macOS than the one that runs (an update was applied to it)
#   ./uninstall.sh --all              the decoders and VCNEncodeFix (the service)
# Only the two decoder bundles are ever deleted from /System. A reboot is needed afterwards; the script says so.
#
# Exit status: 0 done, 1 refused or failed.
emulate -L zsh
setopt no_unset
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
here=${0:A:h}

say()  { print -r -- "$@"; }
die()  { print -r -- "error: $*" >&2; exit 1; }
step() { print -r -- ""; print -r -- "== $*"; }

# json_get <json> <plutil key path>: one value out of the engine's JSON, empty when absent
json_get() { local v; v=$(print -r -- "$1" | /usr/bin/plutil -extract "$2" raw -o - - 2>&1) || return 1; print -r -- "$v"; }

# Prints the engine's preflight as a table; sets can_install and installed.
show_checks() {  # engine-json
  local j=$1 n=0 st id lbl det mark
  can_install=$(json_get "$j" canInstall); installed=$(json_get "$j" installed)
  local os=$(json_get "$j" macos)
  [[ -z $os ]] || say "macOS $os"
  while id=$(json_get "$j" checks.$n.id) && [[ -n $id ]]; do
    st=$(json_get "$j" checks.$n.status); lbl=$(json_get "$j" checks.$n.label); det=$(json_get "$j" checks.$n.detail)
    case $st in ok) mark="ok  " ;; warn) mark="WARN" ;; *) mark="FAIL" ;; esac
    say "  [$mark] $lbl: $det"
    n=$((n + 1))
  done
}

# Runs this script again as root when it is not.
need_root() {  # script args...
  (( EUID == 0 )) && return 0
  say "This needs administrator rights; running again with sudo."
  exec /usr/bin/sudo /bin/zsh "$@"
}

confirm() {  # question
  (( yes )) && return 0
  [[ -t 0 ]] || die "not a terminal: pass --yes to go on without asking"
  local a; read -r "a?$1 [y/N] "; [[ $a == [yY]* ]]
}

yes=0 enc=0 encOnly=0 efi="" engArgs=()
while (( $# )); do
  case $1 in
    --yes|-y) yes=1 ;;
    --sealed) engArgs+=(--sealed) ;;
    --all|--encode-fix) enc=1 ;;
    --encode-fix-only) enc=1 encOnly=1 ;;
    --efi) (( $# >= 2 )) || die "--efi needs a partition such as disk6s1"
           [[ $2 == disk<->s<-> ]] || die "bad EFI partition name $2 (expected diskNsM)"
           efi=$2; shift ;;
    -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' $0; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
  shift
done
[[ -z $efi ]] || (( enc )) || die "--efi only goes with --encode-fix"
[[ -f $here/engine.sh ]] || die "this folder is incomplete: engine.sh must sit next to uninstall.sh"
(( ! enc )) || [[ -f $here/encode-engine.sh ]] || die "encode-engine.sh is missing from this folder"

what="the AV1 and VP9 decoders"
(( encOnly )) && what="VCNEncodeFix"
(( enc && ! encOnly )) && what="the AV1 and VP9 decoders and VCNEncodeFix"
confirm "Remove $what now?" || { say "Cancelled; nothing was changed."; exit 1; }
need_root $0 "$@"

rebootNeeded=0
if (( ! encOnly )); then
  step "Removing the decoders"
  /bin/zsh $here/engine.sh uninstall $engArgs --res $here || die "removing the decoders failed (the encoding fix was not touched)"
  rebootNeeded=1
fi
if (( enc )); then
  step "Removing VCNEncodeFix"
  /bin/zsh $here/encode-engine.sh uninstall ${efi:+--efi $efi} --res $here || die "removing the encoding fix failed"
  [[ -z $efi ]] || rebootNeeded=1
fi
step "Done"
(( rebootNeeded )) && say "Reboot to apply." || say "The encoding service is removed; no reboot is needed."
