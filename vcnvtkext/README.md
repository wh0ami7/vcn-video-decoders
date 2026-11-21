# VCNVTLoader

A small kernel extension (Lilu plug-in) that lets the AV1 and VP9 decoder plug-ins load into
Safari's protected processes when AMFIPass or `amfi_get_out_of_my_way` don't let them in.

## Do you need it?

Safari decodes video in Apple programs (`com.apple.WebKit.GPU`, and `VTDecoderXPCService`
for AV1). They are platform binaries, and macOS only lets them map code signed by Apple. The
plug-ins built here are not, so Safari falls back to software decoding and the kernel log
shows:

```
Library Validation failed: Rejecting '.../VP9VCNDecoder.bundle/Contents/MacOS/VP9VCNDecoder'
(Team ID: none, platform: no) for process 'com.apple.WebKit...' ... reason: mapping process is
a platform binary, but mapped file is not
```

Chrome is not a platform binary, so it loads the plug-ins either way. If Safari decodes in
hardware on your Mac already, you don't need this kext.

To check: `/usr/bin/log show --last 10m --predicate 'eventMessage CONTAINS "VCNDecoder"' | grep -i "library validation"`
right after playing a VP9 or AV1 video in Safari.

The kext is open source and does one thing. It does not turn any protection off: it adds one
exception to one check, for the exact plug-in builds listed inside it. The exception is tied
to the build, not to Safari: any program with the same check may load those exact plug-ins.

## What it does

When macOS refuses to load a library into a protected program, the refusal goes through one
function of AppleMobileFileIntegrity (`_library_validation_failure`). The kext stands in front
of that function. If the library is one of the listed plug-in builds, it answers "allowed".
For anything else it calls Apple's function, so every other refusal stays as it was.

A build is identified by its code directory hash (what `codesign -dvvv` prints as `CDHash`).
The hash covers the plug-in's code, its `Info.plist` and its resources: change one byte of
the plug-in and it is refused again.

The kext only acts when the function it stands in front of starts the way it did in the macOS
builds that were read. On any other build it does nothing and says so.

The plug-ins stay where the installer puts them, in `/System/Library/Video/Plug-Ins`. The kext
does not make `/Library/Video/Plug-Ins` work: that folder has a second check in CoreMedia, in
user space, and Lilu's user-space patcher is off on macOS 11 and later.

## Build

```bash
./vtav1/build.sh && ./vtvp9/build.sh
KEXT_SDK=/path/to/sdk ./vcnvtkext/build.sh
```

`KEXT_SDK` is the same folder as for `vcnencodefix` (`Lilu.kext` DEBUG build + `MacKernelSDK`).
The result is `vcnvtkext/build/VCNVTLoader.kext`, and the script prints the builds it lists.

**The kext and the plug-ins go together.** A kext lists exact builds: if you rebuild the
plug-ins, rebuild the kext. For a release, list the bundles that ship in the DMG (the CI
build), not a local build, either by naming them:

```bash
KEXT_SDK=/path/to/sdk ./vcnvtkext/build.sh "/Volumes/<dmg>/.../AV1VCNDecoder.bundle" "/Volumes/<dmg>/.../VP9VCNDecoder.bundle"
```

or by adding their `CDHash` lines to `released-builds.txt`, which are always allowed.

The kext carries its build date (Lilu's version string), taken from `SOURCE_DATE_EPOCH`, which
defaults to the date of the last commit: the same commit and the same list give the same kext.

## Install

By hand, in the OpenCore EFI:

1. Copy `VCNVTLoader.kext` to `EFI/OC/Kexts`.
2. Add it to `config.plist` under Kernel > Add, **after Lilu**: `BundlePath`
   `VCNVTLoader.kext`, `ExecutablePath` `Contents/MacOS/VCNVTLoader`, `PlistPath`
   `Contents/Info.plist`, `Arch` `x86_64`, `Enabled` true, `MinKernel` `24.0.0`.
3. Keep a backup of `config.plist` and a way to boot without the kext (an OpenCore USB).
4. Reboot.

It can stay next to AMFIPass. If another kext changed the same AMFI function first, this one
steps aside and says so in its status.

## Check that it works

```bash
ioreg -r -c VCNVTLoader -l | grep -E '"(Status|AllowedBuilds|Overrides)"'
```

- `Status = "active"`: the exception is in place.
- `AllowedBuilds`: how many plug-in builds the kext lists.
- `Overrides`: how many times a listed plug-in was let through since boot. It goes up when
  Safari starts playing an AV1 or VP9 video. It stays at 0 when nothing refused the plug-ins
  in the first place (for example with AMFIPass doing it).

Any `Status` that starts with `off:` means the kext changed nothing, and why. `waiting for
AMFI` after the Mac has started means the same: the kext never got to act. `off: another kext
already handles this AMFI function` is what you see next to a kext that changed the same
function first.

## Switches

Boot arguments: `-vcnvtoff` turns the kext off, `-vcnvtdbg` logs more, `-vcnvtbeta` lets it
start on a kernel newer than the last one it was checked on.

## What has been checked

| What | Result |
|---|---|
| The two AMFI functions, in the kernel collections of 14.8.9 (23J631), 15.8.2 (24H232), 26.7.2 (25G332) and 26.7.3 (25G342) | Same names, same first instructions; accepted by the kext's entry check (`tests/loadertest` holds the decisions) |
| Version 1.1.0 (this code), RX 6800 XT, Tahoe 26.7.3, AMFIPass switched off, ad-hoc plug-ins installed (2026-11-18) | `Status = active`. Safari played YouTube in AV1 and VP9 at 3840x2160 in hardware, with the plug-ins mapped in `VTDecoderXPCService` and `com.apple.WebKit.GPU`; `Overrides` went up with each load; another ad-hoc library was still refused; no panic |
| 15.8.3 and 26.7.1 | Not checked: no kernel collection of those builds at hand |
| Sequoia 15 on a real machine | Not booted there |

## Changes from 1.0.0

- Only the AMFI part. The CoreMedia patch is gone: it never ran (Lilu's user-space patcher is
  off on macOS 11+), so `/Library` installs never worked through it.
- The list is made at build time from the plug-ins being built, plus `released-builds.txt`
  and any bundle named on the command line; the hash is the 20-byte `CDHash` AMFI compares.
  1.0.0 kept a checked-in list of older builds, which refuses every newer plug-in.
- Lilu's long jump for the hook (`routeMultipleLong`): with a short jump, Lilu panics when a
  later kext routes the same function.
- `active` only when the hook really landed (the function now starts with the jump); a
  status in the I/O Registry (`Status`, `AllowedBuilds`, `Overrides`).

## Limits

- Decoder plug-ins only. It does nothing for the encoding fix.
- macOS versions: Sequoia 15 and Tahoe 26.
- Each release of the plug-ins needs a kext that lists it.
- Built against Lilu 1.7.2; the test machine runs Lilu 1.7.3.
