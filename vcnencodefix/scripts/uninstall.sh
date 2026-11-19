#!/bin/zsh
# VCNEncodeFix uninstaller.
#
#   sudo ./uninstall.sh                       stop and remove the background service
#   sudo ./uninstall.sh --efi /Volumes/EFI    also remove VCNEncodeFloor.kext from that mounted OpenCore EFI
# Encoder services that were already patched keep the patch until they exit (closing the app is enough); the kext
# stays loaded until the next reboot.
emulate -L zsh
setopt no_unset
here=${0:A:h}
tool=$here/bin/vcnencodefix
say() { print -r -- "$@"; }
die() { print -r -- "error: $*" >&2; exit 1; }

efi=""
while (( $# )); do
  case $1 in
    --efi) (( $# >= 2 )) || die "--efi needs the mount point of the EFI partition"; efi=${2%/}; shift 2 ;;
    -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' $0; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
done
(( EUID == 0 )) || die "run it with sudo: sudo $0"
xattr -dr com.apple.quarantine $here 2>/dev/null

for label in com.droga.vcnencodefix com.droga.vcnpreset; do
  if launchctl bootout system/$label 2>/dev/null; then say "stopped $label"; fi
done
for f in /Library/LaunchDaemons/com.droga.vcnencodefix.plist /usr/local/libexec/vcnencodefix \
         /etc/newsyslog.d/vcnencodefix.conf /Library/LaunchDaemons/com.droga.vcnpreset.plist /usr/local/libexec/vcnpreset; do
  if [[ -e $f ]]; then rm -f -- $f && say "removed $f"; fi
done
say "the log /var/log/vcnencodefix.log was kept"

if [[ -n $efi ]]; then
  cfg=$efi/EFI/OC/config.plist
  [[ -f $cfg ]] || die "$cfg not found: is $efi the mounted EFI partition with OpenCore?"
  [[ -x $tool ]] || die "$tool is missing; unpack the whole folder"
  $tool efi-remove $cfg || die "config.plist was not changed; see the message above"
  if [[ -d $efi/EFI/OC/Kexts/VCNEncodeFloor.kext ]]; then
    rm -rf -- $efi/EFI/OC/Kexts/VCNEncodeFloor.kext && say "removed $efi/EFI/OC/Kexts/VCNEncodeFloor.kext"
  fi
  say "The kext stays loaded until you reboot."
elif kextstat -l -b com.droga.VCNEncodeFloor 2>/dev/null | grep -q .; then
  say "VCNEncodeFloor.kext is still in your OpenCore EFI. To remove it: sudo $0 --efi /Volumes/<EFI volume name>"
fi
