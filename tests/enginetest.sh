#!/bin/zsh
# zsh is required: this is a macOS build/test script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
#
# installer/engine.sh against a made-up world: a folder for the system volume, folders for its snapshots, and stand-ins
# for diskutil, mount, mount_apfs, umount, bless, csrutil and chown that read and write that world. Nothing touches
# the real volume, no root. It checks that an uninstall or install:
#   - leaves the Mac booting a new snapshot that holds what was asked, and leaves nothing mounted;
#   - refuses a volume that is not the system the Mac boots next (the way a bad snapshot is made);
#   - notices a bless that did not change the root snapshot, or made a system without boot.efi, and puts the root
#     snapshot back to the one it was;
#   - does not make a snapshot when there is nothing to remove;
#   - refuses a mount of another volume on its mount point.
# usage: enginetest.sh [folder holding AV1VCNDecoder.bundle and VP9VCNDecoder.bundle]
set -u
ROOT=${0:A:h:h}
cd "$ROOT/tests"
ENGINE=$ROOT/installer/engine.sh
# The bundles the engine installs: the ones given, else the plug-ins built in vtav1/build and vtvp9/build.
if [[ -n ${1:-} ]]; then
  RES=$1
else
  RES=$(mktemp -d /private/var/tmp/enginetest-res.XXXXXX)
  trap 'rm -rf "$RES"' EXIT
  if [[ -d $ROOT/vtav1/build/AV1VCNDecoder.bundle && -d $ROOT/vtvp9/build/VP9VCNDecoder.bundle ]]; then
    ditto $ROOT/vtav1/build/AV1VCNDecoder.bundle $RES/AV1VCNDecoder.bundle
    ditto $ROOT/vtvp9/build/VP9VCNDecoder.bundle $RES/VP9VCNDecoder.bundle
    cp $ROOT/installer/validated-builds.json $RES/
  fi
fi
[[ -d $RES/AV1VCNDecoder.bundle && -d $RES/VP9VCNDecoder.bundle ]] || { echo "skip: no plug-in bundles (build vtav1 and vtvp9 first)"; exit 0; }
fails=0
ok() { print -r -- "PASS $1"; }
no() {
  print -r -- "FAIL $1"; fails=$((fails + 1))
  [[ -f ${W:-/nonexistent}/out.txt ]] && sed 's/^/     | /' $W/out.txt | tail -${ENGINETEST_LINES:-12}
}
check() { if eval "$2"; then ok "$1"; else no "$1"; fi; }

U_ORIG=11111111-1111-1111-1111-111111111111
U_BOOT=22222222-2222-2222-2222-222222222222
ORIG_NAME=com.apple.os.update-$(printf 'A%.0s' {1..64})
BOOT_NAME=com.apple.bless.AAAAAAAA-0000-0000-0000-000000000000
PLUG=System/Library/Video/Plug-Ins

make_tree() { # dir with-ours(0|1)
  local t=$1 b
  mkdir -p $t/System/Library/CoreServices $t/System/Library/KernelCollections $t/System/Library/Extensions/a.kext $t/$PLUG
  cat > $t/System/Library/CoreServices/SystemVersion.plist <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>ProductBuildVersion</key><string>24TEST</string></dict></plist>
EOF
  : > $t/System/Library/CoreServices/boot.efi
  echo kc > $t/System/Library/KernelCollections/BootKernelExtensions.kc
  for b in AppleGVAHEVCDecoder AppleA AppleB AppleC AppleD; do
    mkdir -p $t/$PLUG/$b.bundle/Contents; echo x > $t/$PLUG/$b.bundle/Contents/Info.plist
  done
  if [[ $2 == 1 ]]; then
    for b in AV1VCNDecoder VP9VCNDecoder; do ditto "$RES/$b.bundle" $t/$PLUG/$b.bundle; done
  fi
}

# A new world: live volume and the snapshot the Mac boots (U_BOOT, ROOT) hold the same system.
new_world() { # with-ours-in-live-and-snapshot
  W=$(mktemp -d /private/var/tmp/enginetest.XXXXXX)
  mkdir -p $W/bin $W/snaps $W/mnt1
  make_tree $W/live $1
  make_tree $W/snaps/$ORIG_NAME 0
  ditto $W/live $W/snaps/$BOOT_NAME
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' $U_ORIG $ORIG_NAME 100 0 0 0 $U_BOOT $BOOT_NAME 200 1 1 1 > $W/snaps.tsv
  echo $U_BOOT > $W/booted
  echo $BOOT_NAME > $W/booted_name
  : > $W/mounts; : > $W/calls
  stubs
}

stubs() {
  cat > $W/bin/diskutil <<'EOS'
#!/bin/zsh
W=${VCNVT_TEST_MNT:h}
if [[ $1 == info ]]; then
  echo "   Device Node:               /dev/disk9s1s1"
  echo "   APFS Snapshot UUID:        $(<$W/booted)"
elif [[ $1 == apfs && $2 == listSnapshots ]]; then
  echo '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>Snapshots</key><array>'
  while IFS=$'\t' read -r u n x r p v; do
    echo "<dict><key>Purgeable</key><integer>$p</integer><key>RevertTo</key><integer>$v</integer><key>RootTo</key><integer>$r</integer><key>SnapshotName</key><string>$n</string><key>SnapshotUUID</key><string>$u</string><key>SnapshotXID</key><integer>$x</integer></dict>"
  done < $W/snaps.tsv
  echo '</array></dict></plist>'
fi
EOS
  cat > $W/bin/mount <<'EOS'
#!/bin/zsh
W=${VCNVT_TEST_MNT:h}
if (( $# == 0 )); then cat $W/mounts; exit 0; fi
dir=${@[-1]}; vol=${@[-2]}
rmdir $dir 2>/dev/null; ln -s $W/live $dir
echo "$vol on $dir (apfs, local, journaled, nobrowse)" >> $W/mounts
EOS
  cat > $W/bin/mount_apfs <<'EOS'
#!/bin/zsh
W=${VCNVT_TEST_MNT:h}
name=$2; dir=${@[-1]}
[[ -d $W/snaps/$name ]] || exit 1
# the snapshot the Mac booted is mounted at / already: APFS refuses a second mount (mount_apfs: Resource busy, rc 75)
[[ $name == $(<$W/booted_name) ]] && exit 75
rmdir $dir; ln -s $W/snaps/$name $dir
EOS
  cat > $W/bin/umount <<'EOS'
#!/bin/zsh
W=${VCNVT_TEST_MNT:h}
dir=$1
rm -f $dir; mkdir -p $dir
grep -v " on $dir " $W/mounts > $W/mounts.new; mv $W/mounts.new $W/mounts
EOS
  cat > $W/bin/csrutil <<'EOS'
#!/bin/zsh
echo "Authenticated Root status: disabled"
EOS
  printf '#!/bin/zsh\nexit 0\n' > $W/bin/chown
  cat > $W/bin/bless <<'EOS'
#!/bin/zsh
W=${VCNVT_TEST_MNT:h}
echo "bless $*" >> $W/calls
mode=${STUB_BLESS:-ok}
if [[ $1 == --mount ]]; then   # --mount dir --snapshot uuid   |   --mount / --bootefi --last-sealed-snapshot
  u=${@[-1]}
  [[ $u == --last-sealed-snapshot ]] && u=$(awk -F'\t' '$2 ~ /^com.apple.os.update-/ { print $1; exit }' $W/snaps.tsv)
  awk -F'\t' -v OFS='\t' -v u=$u '{ $4 = ($1 == u) ? 1 : 0; print }' $W/snaps.tsv > $W/snaps.new && mv $W/snaps.new $W/snaps.tsv
  exit 0
fi
[[ $mode == fail ]] && exit 1
n=com.apple.bless.NEW$RANDOM; u=33333333-3333-3333-3333-$(printf '%012d' $RANDOM)
ditto $W/live $W/snaps/$n
[[ $mode == broken ]] && rm -f $W/snaps/$n/System/Library/CoreServices/boot.efi
if [[ $mode != noroot ]]; then
  # Apple's bless points RootTo and RevertTo at the new snapshot; "revertkeep" leaves the revert on the old one
  if [[ $mode == revertkeep ]]; then rv=1; else awk -F'\t' -v OFS='\t' '{ $6 = 0; print }' $W/snaps.tsv > $W/snaps.new && mv $W/snaps.new $W/snaps.tsv; rv=1; fi
  awk -F'\t' -v OFS='\t' '{ $4 = 0; print }' $W/snaps.tsv > $W/snaps.new && mv $W/snaps.new $W/snaps.tsv
  [[ $mode == revertkeep ]] && printf '%s\t%s\t%s\t%s\t%s\t%s\n' $u $n 300 1 1 0 >> $W/snaps.tsv || printf '%s\t%s\t%s\t%s\t%s\t%s\n' $u $n 300 1 1 1 >> $W/snaps.tsv
else
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' $u $n 300 0 1 0 >> $W/snaps.tsv
fi
exit 0
EOS
  chmod +x $W/bin/*
}

run_engine() { # command [env...]   (ENGINE_ARGS: more arguments for the engine)
  local cmd=$1; shift
  env "$@" VCNVT_TEST_BIN=$W/bin VCNVT_TEST_MNT=$W/mnt1 VCNVT_TEST_ROOT=$W/snaps/$BOOT_NAME VCNVT_FAKE_BUILD=24TEST \
    zsh $ENGINE $cmd ${ENGINE_ARGS:-} --res "$RES" > $W/out.txt 2>&1
  return $?
}
root_uuid() { awk -F'\t' '$4 == 1 { print $1 }' $W/snaps.tsv; }
root_name() { awk -F'\t' '$4 == 1 { print $2 }' $W/snaps.tsv; }
bless_calls() { grep -c '^bless' $W/calls; }
cleanup() { [[ -n ${ENGINETEST_KEEP:-} ]] && { echo "     (world kept: $W)"; return; }; rm -rf $W; }

# 1. uninstall: matching volume, decoders installed
new_world 1
run_engine uninstall; rc=$?
check "uninstall: succeeds" "[[ $rc -eq 0 ]]"
check "uninstall: the decoders are gone from the volume" "[[ ! -e $W/live/$PLUG/AV1VCNDecoder.bundle && ! -e $W/live/$PLUG/VP9VCNDecoder.bundle ]]"
check "uninstall: Apple's plug-ins are all still there" "[[ -d $W/live/$PLUG/AppleGVAHEVCDecoder.bundle && \$(ls -d $W/live/$PLUG/Apple*.bundle | wc -l) -eq 5 ]]"
check "uninstall: the Mac boots a new snapshot that has none of them" "[[ \$(root_uuid) != $U_BOOT && -n \$(root_uuid) && \$(bless_calls) -eq 1 && -d $W/snaps/\$(root_name) && ! -e $W/snaps/\$(root_name)/$PLUG/AV1VCNDecoder.bundle && ! -e $W/snaps/\$(root_name)/$PLUG/VP9VCNDecoder.bundle ]]"
check "uninstall: the old snapshot is kept" "[[ -d $W/snaps/$BOOT_NAME && -d $W/snaps/$ORIG_NAME ]]"
check "uninstall: nothing is left mounted" "[[ ! -s $W/mounts ]]"
check "uninstall: what it did is logged" "grep -q 'will boot' $W/mnt1.log"
cleanup

# 2. the volume is not what the Mac boots next: it lacks the decoders the snapshot has (what a revert leaves)
new_world 1
rm -rf $W/live/$PLUG/AV1VCNDecoder.bundle $W/live/$PLUG/VP9VCNDecoder.bundle; touch $W/live/$PLUG
run_engine uninstall; rc=$?
check "diverged volume: uninstall is refused (exit 7)" "[[ $rc -eq 7 ]]"
check "diverged volume: no snapshot was made and the root is unchanged" "[[ \$(bless_calls) -eq 0 && \$(root_uuid) == $U_BOOT ]]"
check "diverged volume: it says why" "grep -q 'not the system the Mac boots' $W/out.txt"
check "diverged volume: nothing is left mounted" "[[ ! -s $W/mounts ]]"
run_engine install; rc=$?
check "diverged volume: install is refused too, or fails an earlier check" "[[ $rc -ne 0 && \$(bless_calls) -eq 0 ]]"
cleanup

# 3. bless says yes but the root snapshot did not change
new_world 1
run_engine uninstall STUB_BLESS=noroot; rc=$?
check "bless that changes no root: uninstall fails" "[[ $rc -ne 0 ]]"
check "bless that changes no root: the root snapshot is set back to what it was" "grep -q -- \"--snapshot $U_BOOT\" $W/calls && [[ \$(root_uuid) == $U_BOOT ]]"
check "bless that changes no root: nothing is left mounted" "[[ ! -s $W/mounts ]]"
cleanup

# 4. bless makes a system without boot.efi
new_world 1
run_engine uninstall STUB_BLESS=broken; rc=$?
check "a snapshot without boot.efi: uninstall fails" "[[ $rc -ne 0 ]] && grep -q 'no boot.efi' $W/out.txt"
check "a snapshot without boot.efi: the root snapshot is set back" "[[ \$(root_uuid) == $U_BOOT ]]"
cleanup

# 5. bless fails
new_world 1
run_engine uninstall STUB_BLESS=fail; rc=$?
check "bless that fails: uninstall fails and says the Mac keeps booting the current snapshot" "[[ $rc -ne 0 ]] && grep -q 'keeps booting the current snapshot' $W/out.txt && [[ \$(root_uuid) == $U_BOOT && ! -s $W/mounts ]]"
cleanup

# 6. nothing to remove
new_world 0
run_engine uninstall; rc=$?
check "nothing to remove: ok, and no snapshot is made" "[[ $rc -eq 0 && \$(bless_calls) -eq 0 && \$(root_uuid) == $U_BOOT && ! -s $W/mounts ]]"
cleanup

# 7. another volume mounted on the mount point
new_world 1
echo "/dev/disk4s1 on $W/mnt1 (apfs, local)" > $W/mounts
run_engine uninstall; rc=$?
check "another volume on the mount point: refused, nothing changed" "[[ $rc -ne 0 && \$(bless_calls) -eq 0 && -d $W/live/$PLUG/AV1VCNDecoder.bundle ]] && grep -q 'not from' $W/out.txt"
cleanup

# 9. the volume holds another macOS than the one that runs (an update was applied to it): the case that made a
#    snapshot the Mac could not boot from
new_world 1
sed -i '' 's/24TEST/25TEST/' $W/live/System/Library/CoreServices/SystemVersion.plist
run_engine uninstall; rc=$?
check "volume of another macOS: uninstall is refused (exit 7) and names both versions" "[[ $rc -eq 7 ]] && grep -q 'holds macOS' $W/out.txt && grep -q '25TEST' $W/out.txt && grep -q '24TEST' $W/out.txt"
check "volume of another macOS: no snapshot was made, the root is unchanged, nothing is mounted" "[[ \$(bless_calls) -eq 0 && \$(root_uuid) == $U_BOOT && ! -s $W/mounts && -d $W/live/$PLUG/AV1VCNDecoder.bundle ]]"
ENGINE_ARGS=--force run_engine install; rc=$?
check "volume of another macOS: install is refused too" "[[ $rc -ne 0 && \$(bless_calls) -eq 0 ]]"
ENGINE_ARGS=--sealed run_engine uninstall; rc=$?
check "uninstall --sealed: the Mac is set to boot Apple's sealed snapshot, the volume is not touched" "[[ $rc -eq 0 && \$(root_uuid) == $U_ORIG && -d $W/live/$PLUG/AV1VCNDecoder.bundle ]] && grep -q -- '--last-sealed-snapshot' $W/calls && ! grep -q -- '--create-snapshot' $W/calls"
cleanup

# 10. the sealed snapshot is another macOS than the running one: not used
new_world 1
sed -i '' 's/24TEST/25TEST/' $W/snaps/$ORIG_NAME/System/Library/CoreServices/SystemVersion.plist
ENGINE_ARGS=--sealed run_engine uninstall; rc=$?
check "uninstall --sealed: a sealed snapshot of another macOS is refused" "[[ $rc -ne 0 && \$(root_uuid) == $U_BOOT && \$(bless_calls) -eq 0 ]]"
cleanup

# 11. the sealed snapshot holds our bundles: not a clean system, not used
new_world 1
ditto "$RES/AV1VCNDecoder.bundle" $W/snaps/$ORIG_NAME/$PLUG/AV1VCNDecoder.bundle
ENGINE_ARGS=--sealed run_engine uninstall; rc=$?
check "uninstall --sealed: a snapshot that holds the decoders is refused" "[[ $rc -ne 0 && \$(bless_calls) -eq 0 ]]"
cleanup

# 12. a macOS update is staged (Software Update reverts the volume at boot and drops the new snapshot)
new_world 0
printf '%s\t%s\t%s\t%s\t%s\t%s\n' 44444444-4444-4444-4444-444444444444 com.apple.os.update-MSUPrepareUpdate 400 0 0 0 >> $W/snaps.tsv
ENGINE_ARGS=--force run_engine install; rc=$?
check "update staged: install is refused (exit 8) and says how to turn the download off" "[[ $rc -eq 8 && \$(bless_calls) -eq 0 && ! -s $W/mounts ]] && grep -q 'AutomaticDownload' $W/out.txt"
cleanup
new_world 1
printf '%s\t%s\t%s\t%s\t%s\t%s\n' 44444444-4444-4444-4444-444444444444 com.apple.os.update-MSUPrepareUpdate 400 0 0 0 >> $W/snaps.tsv
run_engine uninstall; rc=$?
check "update staged: uninstall goes on, with a warning" "[[ $rc -eq 0 && \$(bless_calls) -eq 1 ]] && grep -q 'update is staged' $W/mnt1.log"
cleanup

# 13. MobileSoftwareUpdate has the volume set to revert to another snapshot than the one the Mac boots: a snapshot
#     blessed now is deleted by that revert at the next mount (kernel log 2026-11-26 19:51:04)
new_world 1
awk -F'\t' -v OFS='\t' -v o=$U_ORIG '{ $6 = ($1 == o) ? 1 : 0; print }' $W/snaps.tsv > $W/snaps.new && mv $W/snaps.new $W/snaps.tsv
run_engine uninstall; rc=$?
check "revert armed to another snapshot: uninstall is refused (exit 7) before any change" "[[ $rc -eq 7 && \$(bless_calls) -eq 0 && -d $W/live/$PLUG/AV1VCNDecoder.bundle ]] && grep -q 'revert to snapshot' $W/out.txt"
cleanup
# ... and after the bless: the revert is still set to the old snapshot
new_world 1
run_engine uninstall STUB_BLESS=revertkeep; rc=$?
check "revert left on the old snapshot after bless: uninstall fails and the root is set back" "[[ $rc -ne 0 && \$(root_uuid) == $U_BOOT ]] && grep -q 'revert' $W/out.txt"
cleanup

# 8. install (its preflight reads this Mac: GPU, AMFI, the system plug-ins; the made-up build needs --force)
new_world 0
ENGINE_ARGS=--force run_engine install; rc=$?
if [[ $rc -eq 2 ]]; then
  echo "skip install: this Mac's preflight does not pass"
else
  check "install: succeeds, the volume and the new snapshot hold both bundles" "[[ $rc -eq 0 && -f $W/live/$PLUG/AV1VCNDecoder.bundle/Contents/Info.plist && -f $W/snaps/\$(root_name)/$PLUG/VP9VCNDecoder.bundle/Contents/Info.plist && \$(root_uuid) != $U_BOOT ]]"
  check "install: nothing is left mounted" "[[ ! -s $W/mounts ]]"
fi
cleanup

print -r -- ""
(( fails == 0 )) && { echo "all engine tests passed"; exit 0; }
echo "$fails engine test(s) failed"; exit 1
