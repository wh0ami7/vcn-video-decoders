#!/bin/zsh
# AMD VCN Video Decoders - command-line installer.
#
#   ./install.sh                      check this Mac, then install the AV1 and VP9 hardware decoders
#   ./install.sh --check              only the preflight (no root, nothing is changed)
#   ./install.sh --yes                do not ask before installing
#   ./install.sh --force              install although this macOS build's AMD driver is not validated
#                                     (GPU-hang risk; only for a build you have checked yourself)
#   ./install.sh --encode-fix         also install VCNEncodeFix (the encoding service; works at once)
#   ./install.sh --encode-fix --efi disk6s1
#                                     ... and add its clock-floor kext to that OpenCore EFI partition
#   ./install.sh --encode-fix-only    only VCNEncodeFix, not the decoders
# Undo with ./uninstall.sh. The decoders go into the sealed /System volume, so a reboot is needed; the
# script says so when it is done. It is the same engine the graphical installer runs.
#
# Exit status: 0 done, 1 refused or failed (the message says why), 2 this Mac cannot take the install.
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

check=0 yes=0 force=0 enc=0 encOnly=0 efi=""
while (( $# )); do
  case $1 in
    --check) check=1 ;;
    --yes|-y) yes=1 ;;
    --force) force=1 ;;
    --encode-fix) enc=1 ;;
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
[[ -f $here/engine.sh && -d $here/AV1VCNDecoder.bundle && -d $here/VP9VCNDecoder.bundle ]] \
  || die "this folder is incomplete: engine.sh and the two decoder bundles must sit next to install.sh"

if (( ! encOnly )); then
  step "Checking this Mac"
  j=$(/bin/zsh $here/engine.sh check --res $here) || die "the preflight did not run"
  show_checks $j
  say "decoders installed: $installed"
  if [[ $can_install != true ]]; then
    if (( force )) && [[ $(json_get $j overridable) == true ]]; then
      say "The AMD driver of this macOS build is not validated; --force goes on at your own risk."
    else
      say ""; say "This Mac cannot take the install (see the FAIL lines above)."
      exit 2
    fi
  fi
fi
if (( enc )); then
  step "Checking the encoding fix"
  ej=$(/bin/zsh $here/encode-engine.sh check --res $here) || die "the encoding fix preflight did not run"
  show_checks $ej
  [[ $can_install == true ]] || { say "The encoding fix cannot be installed here (see above)."; (( encOnly )) && exit 2; enc=0; }
fi
(( check )) && { say ""; say "Nothing was changed (--check)."; exit 0; }

what="the AV1 and VP9 decoders"
(( encOnly )) && what="VCNEncodeFix"
(( enc && ! encOnly )) && what="the AV1 and VP9 decoders and VCNEncodeFix"
confirm "Install $what now?" || { say "Cancelled; nothing was changed."; exit 1; }
need_root $0 "$@"

rebootNeeded=0
if (( ! encOnly )); then
  step "Installing the decoders"
  /bin/zsh $here/engine.sh install $( (( force )) && print -- --force ) --res $here || die "the decoder install failed (nothing more was done)"
  rebootNeeded=1
fi
if (( enc )); then
  step "Installing VCNEncodeFix"
  /bin/zsh $here/encode-engine.sh install ${efi:+--efi $efi} --res $here || die "the encoding fix install failed"
  [[ -z $efi ]] || rebootNeeded=1
fi
step "Done"
(( rebootNeeded )) && say "Reboot to load the hardware decoders$([[ -n $efi ]] && print -n ' and the clock-floor kext')." || say "The encoding service is running; no reboot is needed."
say "Undo: ./uninstall.sh"
