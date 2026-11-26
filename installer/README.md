# AMD VCN Video Decoders — installer

One-window macOS app that installs the hardware AV1 + VP9 VideoToolbox decoders for AMD Navi 21
GPUs (RX 6800 / 6800 XT / 6900 XT / 6950 XT) on OpenCore hackintoshes running macOS Sequoia 15
through Tahoe 26. After a reboot, Safari and Chrome decode YouTube AV1/VP9 (up to 4K60, 8K for AV1)
on the GPU.

![screenshot](docs/screenshot.jpg)

## For users

1. Open the DMG and drag the app to **Applications**.
2. Open it. The app is not notarized, so the first time macOS refuses: go to **System Settings →
   Privacy & Security → Open Anyway** (on Sequoia and later, right-click → Open no longer works).
3. If every check is green, click **Install**, type your password, click **Reboot now**.

The app refuses to install when something would not work or could be unsafe, and says why:

| Check | Why it matters |
|---|---|
| macOS version | only Sequoia 15 – Tahoe 26 are supported |
| GPU driver validated | the AMD driver of this exact macOS build must have been checked; an unchecked build can hang the GPU. Blocked by default, "Install anyway" asks for explicit confirmation |
| Authenticated-root disabled | needed to write the sealed system volume (`csrutil authenticated-root disable` from Recovery) |
| AMFI open | `amfi_get_out_of_my_way=0x1` or AMFIPass, else macOS rejects the decoders |
| Supported AMD GPU | warning only; Navi 21 is the known-good family |
| System video plug-ins intact | Apple's own plug-ins must be complete; if any is missing the app offers **Repair**, which copies only the missing ones back from Apple's sealed snapshot of the same macOS build (and installs the decoders in the same reboot) |
| Decoder bundles present | the app was unzipped intact |

It also detects whether the decoders are already installed and up to date (cdhash compare), and
offers **Update** or **Uninstall** accordingly. Undo everything: **Uninstall** + reboot, or
`sudo bless --mount / --last-sealed-snapshot` + reboot.

### Encoding fix (optional)

Below the decoder checks, a collapsed **Encoding fix (optional)** section installs VCNEncodeFix (see
`../vcnencodefix/README.md`). It has its own checks and buttons and does not depend on the decoders:

| Check | Why it matters |
|---|---|
| Encoding fix package present | the app was unzipped intact |
| macOS build validated for encoding | the fix patches exact Apple and AMD binaries; only builds with `"encode": true` |
| Encoder files match | the service found the exact code it patches (speed preset, >4096-wide fix) |
| Navi 21 GPU | VCNEncodeFix is only for Navi 21 |
| Lilu loaded | warning only; needed by the clock-floor kext, not by the service |

**Install to** picks what is installed: *Service only* (works at once, no reboot) or one of the Mac's EFI
partitions, which also adds `VCNEncodeFloor.kext` to that OpenCore EFI (the partition is mounted if needed and
unmounted again; it must hold `EFI/OC/OpenCore.efi`, `config.plist` and `Lilu.kext`; `config.plist` gets a dated
backup and is checked after the edit). **Remove** undoes the same scope.

## Command-line installer

`build-cli.sh` (after `vtav1/build.sh`, `vtvp9/build.sh` and `vcnencodefix/build.sh`, or with the checked-in
`vcnencodefix/prebuilt/`) makes `build/vcn-video-decoders-cli-<version>/` and a `.tar.gz` with its `.sha256`: the same
engine the app runs, the two decoder bundles and VCNEncodeFix, behind two scripts.

```bash
./install.sh --check               # the preflight; changes nothing, needs no root
./install.sh                       # asks, then installs the decoders (sudo); reboot afterwards
./install.sh --encode-fix          # ... and the VCNEncodeFix service (no reboot for the service)
./install.sh --encode-fix --efi disk6s1   # ... and its kext into that OpenCore EFI
./uninstall.sh                     # removes the decoders; --encode-fix, --encode-fix-only or --all for VCNEncodeFix
```

`--yes` skips the question, `--force` is the app's "Install anyway" (a driver build that is not validated can hang
the GPU). Both scripts refuse an incomplete folder and a bad `--efi` name before anything is changed.

## Why /System and not /Library

VideoToolbox scans `/System/Library/Video/Plug-Ins` without a code-signature check. `/Library/Video/Plug-Ins`
requires an Apple signature, and patching that check out of CoreMedia at runtime is not possible on
macOS 11 and later: Lilu's user-space patcher is switched off there.

## For developers

- `engine.sh` — all logic: `check` (JSON preflight, no root), `install [--force]`, `repair [--force]`,
  `uninstall` (root). Deletions are guarded: only the two decoder bundle paths can ever be removed.
  The app is a thin SwiftUI shell around it; the CLI works the same: `sudo ./engine.sh install`.
- `encode-engine.sh` — the encoding fix: `check` (JSON, no root), `install [--efi diskNsM]`,
  `uninstall [--efi diskNsM]` (root). It wraps VCNEncodeFix's own `install.sh` / `uninstall.sh`.
- `validated-builds.json` — macOS builds whose driver was validated. Add a build only after
  validating its AMDRadeonVADriver2 (AV1 + VP9 paths) in IDA. `encode: true` only after VCNEncodeFix
  supports that build (its tool checks the exact UUIDs and bytes it patches).
- `build-app.sh` — builds `build/AMD VCN Video Decoders.app` from `../vtav1/build` and
  `../vtvp9/build`. Embedded bundles keep their original cdhashes (only the outer app is re-signed).
  VCNEncodeFix comes from `../vcnencodefix/dist` or else `../vcnencodefix/prebuilt/*.zip`.
- `make-dmg.sh` — packages the app into `build/AMD-VCN-Video-Decoders-<version>.dmg`.
- `make-assets.sh` — renders the app icon and the DMG background from `assets/render.swift`.
- `VCNVT_FAKE_BUILD=<build>` makes the preflight pretend another macOS build (testing the blocked path).
