#!/bin/zsh
# VCNEncodeFix installer.
#
#   sudo ./install.sh                         install or update the background service (user-space half)
#   sudo ./install.sh --efi /Volumes/EFI      also add VCNEncodeFloor.kext to that mounted OpenCore EFI
#                                             (kernel half; takes effect after a reboot)
# Options: --preset speed|balance|quality (default speed), --no-preset, --no-wide, --force (skip the GPU check).
# Remove everything with ./uninstall.sh. Nothing here touches the macOS system volume.
emulate -L zsh
setopt no_unset
here=${0:A:h}
tool=$here/bin/vcnencodefix
label=com.droga.vcnencodefix
bin_dst=/usr/local/libexec/vcnencodefix
plist_dst=/Library/LaunchDaemons/$label.plist
log=/var/log/vcnencodefix.log

say()  { print -r -- "$@"; }
die()  { print -r -- "error: $*" >&2; exit 1; }
step() { print -r -- ""; print -r -- "== $*"; }

efi="" force=0 args=()
while (( $# )); do
  case $1 in
    --efi) (( $# >= 2 )) || die "--efi needs the mount point of the EFI partition"; efi=${2%/}; shift 2 ;;
    --preset) (( $# >= 2 )) || die "--preset needs speed, balance or quality"
              [[ $2 == (speed|balance|quality) ]] || die "unknown preset $2"; args+=(--preset $2); shift 2 ;;
    --no-preset|--no-wide) args+=($1); shift ;;
    --force) force=1; shift ;;
    -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' $0; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
done
(( EUID == 0 )) || die "run it with sudo: sudo $0"

# Files unpacked from a downloaded zip carry the quarantine flag, which stops unsigned tools from running.
xattr -dr com.apple.quarantine $here 2>/dev/null
[[ -x $tool ]] || die "$tool is missing; unpack the whole folder"

step "Checking this Mac"
gpu=$(system_profiler SPDisplaysDataType 2>/dev/null | awk -F': ' '/Chipset Model/{m=$2} /Device ID/{print m" ("$2")"}')
say "GPU: ${gpu:-unknown}"
if ! print -r -- $gpu | grep -qiE '\(0x73(a[0-9a-f]|bf)\)'; then
  (( force )) || die "no Navi 21 GPU (RX 6800, 6800 XT, 6900 XT, 6950 XT) found; VCNEncodeFix was only tested on Navi 21. --force skips this check."
  say "warning: no Navi 21 GPU found; continuing because of --force"
fi
status_out=$($tool status ${args[@]})
st=$?
print -r -- $status_out | sed 's/^/  /'
(( st == 0 )) || die "neither patch applies to this macOS build, so the service would do nothing; not installing"
if [[ $status_out == *"access  REFUSED"* ]]; then
  say "warning: SIP Debugging Restrictions are on. The service installs, but macOS will not let it into the encoder"
  say "         until bit 0x4 is added to csr-active-config and the Mac is rebooted (README, Requirements)."
  say "         The kernel half (--efi) works without it."
fi

step "Installing the service ($label)"
if [[ -e /Library/LaunchDaemons/com.droga.vcnpreset.plist ]] || launchctl print system/com.droga.vcnpreset >/dev/null 2>&1; then
  say "removing the earlier test version (com.droga.vcnpreset)"
  launchctl bootout system/com.droga.vcnpreset 2>/dev/null
  rm -f /Library/LaunchDaemons/com.droga.vcnpreset.plist /usr/local/libexec/vcnpreset
fi
launchctl bootout system/$label 2>/dev/null
mkdir -p /usr/local/libexec || die "cannot create /usr/local/libexec"
install -m 755 -o root -g wheel $tool $bin_dst || die "cannot install $bin_dst"
argxml=""
for a in ${args[@]}; do argxml+="\t\t<string>$a</string>\n"; done
awk -v a="$argxml" '/__ARGS__/ { printf "%s", a; next } { print }' $here/support/$label.plist > $plist_dst.tmp || die "cannot write $plist_dst"
chown root:wheel $plist_dst.tmp && chmod 644 $plist_dst.tmp
plutil -lint -s $plist_dst.tmp || { rm -f $plist_dst.tmp; die "generated launchd plist is invalid"; }
mv -f $plist_dst.tmp $plist_dst || die "cannot install $plist_dst"
install -m 644 -o root -g wheel $here/support/vcnencodefix.conf /etc/newsyslog.d/vcnencodefix.conf
touch $log && chmod 644 $log
launchctl bootstrap system $plist_dst || die "launchctl bootstrap failed; see $log"
sleep 1
pid=$(pgrep -f "^$bin_dst watch" | head -1)
if [[ -n $pid ]]; then say "running (pid $pid)"; else say "warning: the service is not running; see $log"; fi
tail -4 $log | sed 's/^/  /'

step "Kernel half (VCNEncodeFloor.kext, VCN clock floor)"
loaded=$(kextstat -l -b com.droga.VCNEncodeFloor 2>/dev/null | awk '{print $7}' | tr -d '()')
if [[ -n $loaded ]]; then
  state=$(ioreg -rd1 -c IOResources -k VCNEncodeFloor | awk -F'"' '$2 == "VCNEncodeFloor" {print $4}')
  say "loaded now: VCNEncodeFloor $loaded, ${state:-state not published}"
fi
if [[ -n $efi ]]; then
  cfg=$efi/EFI/OC/config.plist
  [[ -f $cfg ]] || die "$cfg not found: is $efi the mounted EFI partition with OpenCore?"
  [[ -d $efi/EFI/OC/Kexts/Lilu.kext ]] || die "$efi/EFI/OC/Kexts/Lilu.kext not found; VCNEncodeFloor needs Lilu"
  dst=$efi/EFI/OC/Kexts/VCNEncodeFloor.kext
  rm -rf $dst.new
  ditto --norsrc --noextattr $here/Kexts/VCNEncodeFloor.kext $dst.new || die "cannot copy the kext to $efi"
  if [[ -d $dst ]]; then rm -rf $dst || die "cannot replace $dst"; fi
  mv $dst.new $dst || die "cannot move the kext into place"
  say "copied VCNEncodeFloor.kext $(defaults read $dst/Contents/Info CFBundleShortVersionString) to $dst"
  $tool efi-add $cfg || die "config.plist was not changed correctly; see the message above"
  say "Reboot to load it. If the Mac does not start, boot another EFI (for example a USB stick) and either add"
  say "-vcnflooroff to boot-args or copy a config.plist.backup-* file from EFI/OC over EFI/OC/config.plist."
elif [[ -z $loaded ]]; then
  say "not installed. It needs an entry in OpenCore's config.plist. Mount the EFI partition and run:"
  say "  diskutil list | grep EFI                 (find the partition OpenCore boots from)"
  say "  sudo diskutil mount <disk>s<n>"
  say "  sudo $0 --efi /Volumes/<EFI volume name>"
fi

step "Done"
say "Check it at any time with ./doctor.sh and measure it with ./test.sh. Log: $log"
