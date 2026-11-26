#!/bin/zsh
# zsh is required: this is a macOS installer/build script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
# Builds the command-line installer: a folder with install.sh and uninstall.sh next to the engines, the two decoder
# bundles and VCNEncodeFix, and a .tar.gz of it. It is the same engine the app runs; nothing here needs Xcode.
# Output: installer/build/vcn-video-decoders-cli-<version>/ and .tar.gz (+ .sha256).
# Run vtav1/build.sh and vtvp9/build.sh first, and vcnencodefix/build.sh (or keep vcnencodefix/prebuilt/).
# VERSION=x.y.z overrides the version (default: the app's version, plus the short commit).
set -e
cd "${0:A:h}"
AV1_SRC=../vtav1/build/AV1VCNDecoder.bundle
VP9_SRC=../vtvp9/build/VP9VCNDecoder.bundle
[[ -d $AV1_SRC && -d $VP9_SRC ]] || { echo "build vtav1 and vtvp9 first"; exit 1; }
ENC_VERSION=$(<../vcnencodefix/VERSION)
ENC_DIST=../vcnencodefix/dist/VCNEncodeFix-$ENC_VERSION
ENC_ZIP=../vcnencodefix/prebuilt/VCNEncodeFix-$ENC_VERSION.zip
[[ -d $ENC_DIST || -f $ENC_ZIP ]] || { echo "no VCNEncodeFix $ENC_VERSION: run vcnencodefix/build.sh or restore $ENC_ZIP"; exit 1; }
VER=${VERSION:-$(/usr/bin/plutil -extract CFBundleShortVersionString raw app/Info.plist)}
if [[ -z ${VERSION:-} ]]; then
  commit=$(git -C .. rev-parse --short HEAD 2>/dev/null || true)
  [[ -z $commit ]] || VER=$VER+$commit
fi
NAME=vcn-video-decoders-cli-$VER
OUT=build/$NAME
rm -rf "$OUT" "build/$NAME.tar.gz" "build/$NAME.tar.gz.sha256"
mkdir -p "$OUT"

cp cli/install.sh cli/uninstall.sh engine.sh encode-engine.sh validated-builds.json "$OUT/"
chmod +x "$OUT/install.sh" "$OUT/uninstall.sh" "$OUT/engine.sh" "$OUT/encode-engine.sh"
ditto "$AV1_SRC" "$OUT/AV1VCNDecoder.bundle"
ditto "$VP9_SRC" "$OUT/VP9VCNDecoder.bundle"
rm -rf "$OUT"/*.bundle/Contents/MacOS/*.dSYM
if [[ -d $ENC_DIST ]]; then
  ditto --norsrc --noextattr "$ENC_DIST" "$OUT/VCNEncodeFix"
else
  tmp=$(mktemp -d)
  ditto -x -k "$ENC_ZIP" "$tmp"
  ditto --norsrc --noextattr "$tmp/VCNEncodeFix-$ENC_VERSION" "$OUT/VCNEncodeFix"
  rm -rf "$tmp"
fi
(cd "$OUT/VCNEncodeFix" && shasum -a 256 -c -s SHA256SUMS) \
  || { echo "VCNEncodeFix files do not match their SHA256SUMS"; exit 1; }
cat > "$OUT/README.txt" <<EOF
AMD VCN Video Decoders $VER - command-line installer

  ./install.sh --check        what this Mac allows (changes nothing)
  ./install.sh                install the AV1 and VP9 hardware decoders (asks for sudo), then reboot
  ./install.sh --encode-fix   also install VCNEncodeFix ($ENC_VERSION), the encoding service
  ./uninstall.sh              remove the decoders; add --encode-fix or --all for VCNEncodeFix too

--help on either script lists every option. The decoders go into the sealed /System volume:
authenticated-root must be disabled and AMFI open (see the checks); the app's README has the details.
EOF
echo "$VER" > "$OUT/VERSION"
# Not signed on purpose: the bundles keep the signatures they were built with (the engine compares their cdhash).
COPYFILE_DISABLE=1 tar -C build -czf "build/$NAME.tar.gz" "$NAME"
(cd build && shasum -a 256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
echo "built build/$NAME and build/$NAME.tar.gz"
