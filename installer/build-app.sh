#!/bin/zsh
# zsh is required: this is a macOS installer/build script. Google Shell
# Style permits a non-bash interpreter when the execution environment
# forces it (https://google.github.io/styleguide/shellguide.html).
# Build "AMD VCN Video Decoders.app": SwiftUI front-end + engine.sh + both decoder bundles, plus the optional
# encoding fix (VCNEncodeFix + encode-engine.sh).
# Output: installer/build/AMD VCN Video Decoders.app  (make-dmg.sh packages it for sharing).
# VCNEncodeFix comes from ../vcnencodefix/dist (a local ./vcnencodefix/build.sh, which needs Lilu and MacKernelSDK)
# or else from the checked-in ../vcnencodefix/prebuilt/VCNEncodeFix-<version>.zip (what the release CI uses).
set -e
cd "${0:A:h}"
APP="build/AMD VCN Video Decoders.app"
AV1_SRC=../vtav1/build/AV1VCNDecoder.bundle
VP9_SRC=../vtvp9/build/VP9VCNDecoder.bundle
[[ -d $AV1_SRC && -d $VP9_SRC ]] || { echo "build vtav1 and vtvp9 first"; exit 1; }
ENC_VERSION=$(<../vcnencodefix/VERSION)
ENC_DIST=../vcnencodefix/dist/VCNEncodeFix-$ENC_VERSION
ENC_ZIP=../vcnencodefix/prebuilt/VCNEncodeFix-$ENC_VERSION.zip
[[ -d $ENC_DIST || -f $ENC_ZIP ]] || { echo "no VCNEncodeFix $ENC_VERSION: run vcnencodefix/build.sh or restore $ENC_ZIP"; exit 1; }

rm -rf "$APP" && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

# x86_64 only: the decoders drive the AMD VCN, which only exists on Intel/AMD hackintoshes.
xcrun swiftc -parse-as-library -O \
  -target x86_64-apple-macos15.0 \
  app/main.swift -o "$APP/Contents/MacOS/VCNInstaller"

cp app/Info.plist "$APP/Contents/Info.plist"
cp engine.sh encode-engine.sh validated-builds.json "$APP/Contents/Resources/"
[[ -f assets/AppIcon.icns ]] || zsh make-assets.sh
cp assets/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
chmod +x "$APP/Contents/Resources/engine.sh" "$APP/Contents/Resources/encode-engine.sh"
ditto "$AV1_SRC" "$APP/Contents/Resources/AV1VCNDecoder.bundle"
ditto "$VP9_SRC" "$APP/Contents/Resources/VP9VCNDecoder.bundle"
rm -rf "$APP"/Contents/Resources/*.bundle/Contents/MacOS/*.dSYM
if [[ -d $ENC_DIST ]]; then
  ditto --norsrc --noextattr "$ENC_DIST" "$APP/Contents/Resources/VCNEncodeFix"
else
  ENC_TMP=$(mktemp -d)
  ditto -x -k "$ENC_ZIP" "$ENC_TMP"
  ditto --norsrc --noextattr "$ENC_TMP/VCNEncodeFix-$ENC_VERSION" "$APP/Contents/Resources/VCNEncodeFix"
  rm -rf "$ENC_TMP"
fi
(cd "$APP/Contents/Resources/VCNEncodeFix" && shasum -a 256 -c -s SHA256SUMS) \
  || { echo "VCNEncodeFix files do not match their SHA256SUMS"; exit 1; }

codesign -f -s - "$APP"
rm -f build/VCN-Video-Decoders.zip
echo "built $APP"
