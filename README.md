# AMD VCN Video Decoders

Hardware AV1 and VP9 decoding through VideoToolbox on AMD Navi 21 GPUs (RX 6800, 6800 XT, 6900 XT,
6950 XT) on OpenCore hackintoshes. Safari and Chrome then decode YouTube AV1 and VP9 on the GPU.

The installer also offers an optional encoding fix (VCNEncodeFix) for slow or stuttering hardware HEVC/H.264
recording, for example OBS at 4K60 or wider than 4096. See "Encoding fix (optional)" below.

Made by [DrogaBox](https://github.com/DrogaBox) & [Izzy](https://github.com/wh0ami7).

<img width="1900" height="859" alt="image" src="https://github.com/user-attachments/assets/a58ecd9d-c990-4707-aea9-8a3f5d1fe01e" />


# Requirements

AMD Navi 21 GPU: RX 6800, 6800 XT, 6900 XT or 6950 XT. Other cards are untested.

OpenCore hackintosh on one of the validated builds: Sequoia 15.8.2 (24H232) or 15.8.3 (24H241), Tahoe 26.7.1 (25G241), 26.7.2 (25G332) or 26.7.3 (25G342). Other builds are blocked by default, because an unchecked driver can hang the GPU.

On Sequoia the VP9 decoder has been run on one GPU so far (RX 6900 XT, 15.8.2). On 15.8.3, which ships the same driver binary, run the VP9 check once (see "Validating a new macOS build") and report the result.

Authenticated root disabled, to write the sealed system volume: csrutil authenticated-root disable from Recovery.

AMFI open: the amfi_get_out_of_my_way=0x1 boot-arg or AMFIPass. Otherwise macOS refuses to load the decoders.

To undo everything: Uninstall in the app and reboot, or sudo bless --mount / --last-sealed-snapshot and reboot.


## How it works

macOS ships AMD's VCN driver (`AMDRadeonVADriver2`), but only wires H.264 and HEVC to VideoToolbox.
The driver can still run AV1 jobs. These plug-ins parse the bitstream on the CPU, build the VCN
decode messages and submit them through the driver, then hand the frames back to VideoToolbox.

VP9 is not in Apple's driver at all. `vcnvp9` builds the VCN VP9 messages itself, following the
same layout as Mesa's `ac_vcn_dec.h`.

## Decoders 0.3.6

- A frame the video engine cannot decode bit-exactly is refused, and so is everything predicted from it. Found with
  `tools/diag/decode.py`, which compares every frame with a CPU decode: `vp90-2-22-svc_1280x720_3.ivf` (spatial
  SVC) decoded without an error but showed a wrong picture. Its 1280x720 layer predicts from a 320x180 reference,
  4x smaller; the engine predicts from a reference up to 2x smaller bit-exactly (the 320x180 to 640x360 layer
  was identical to libvpx), and from a smaller one it reads only the first rows of the reference and repeats
  them below, so more than half of the frame differs and the error carries into every frame predicted from it.
  VP9 now refuses a frame with a reference more than 2x smaller than the frame. The limit was found by comparing
  pictures, not read from the firmware; ratios between 2x and 4x were not tried and are refused too. Such a stream
  needs a software decoder (Safari and Chrome have one).
- A frame that fails for any reason now empties the reference slots it should have refreshed, in both plug-ins.
  Before, later frames kept predicting from the older pictures in those slots and showed wrong pictures without an
  error; now they are refused (`kVTVideoDecoderReferenceMissingErr`) until a frame refreshes the slots, so the
  client falls back to another decoder or waits for a key frame.
- `tools/diag/` (python3): V4L2 field coverage of the parsers, which coding tools the test streams use, and a
  bit-exact check of every stream against a CPU decode. 14 of the 15 streams match; the SVC one is refused.
- `VCNVP9_DUMP_YUV=<file>` makes `decode_vp9` append every frame as I420, and `tools/diag/pixdiff.py` shows where
  the pixels differ from a CPU decode.

## Decoders 0.3.5

- The `stats` log line now counts a frame as slow against the stream's own frame time. Before,
  it counted every frame whose queue wait plus decode passed a fixed 16.7 ms, so a 30 fps video
  showed 60 to 80 slow frames out of 120 while it played with time to spare, and the normal
  queue wait (up to four frames in flight) made 60 fps look worse than it was. Now a frame counts
  only when its decode alone takes longer than the frame time, taken from the sample's duration
  or, when the caller gives none, from the step between presentation times (steps over 100 ms
  are seeks and are skipped; 60 fps is assumed until a time is known). The line shows the
  average frame time it used: `decode over frame time (avg 33.4 ms) 0`. Samples that carry only
  a hidden frame do not change the frame time, and a new session learns it again.
- It is the stream's time, not the wall clock's: at 2x playback each frame really has half of
  it.
- Decoding itself is unchanged; both plug-ins move to 0.3.5.

Checked with `tests/run.sh`.

## Decoders 0.3.4

- The VP9 plug-in now declares a rating (`VTRating` 400 in its `CMMatchingInfo`, the value Apple
  gives its own hardware decoders). With an Intel iGPU of the Kaby Lake or Ice Lake families
  active next to the Radeon, macOS also offers Apple's Intel VP9 decoder (`AppleGVAVPXDecoder`,
  which needs the iGPU's `IOGVAXDecode` key). Both declared the same codec, hardware flag and
  version, and VideoToolbox breaks such a tie by an internal hash order that put Apple's first.
  On a UHD 630 + RX 6800 XT it decoded one frame and stalled, and our plug-in never got the
  video (issue #5).
- How VideoToolbox orders candidates, read with IDA in VideoToolbox of Tahoe 26.7.x and Sequoia
  15.8.2: GPU preference, the Apple-internal flag, then `VTRating` (missing counts as 0), then
  the hardware flag. Checked through the real selection code with two stand-in decoders: with
  the rating ours is tried first. If ours fails to start, VideoToolbox still falls back to
  Apple's. Macs without such an iGPU see no change.
- The AV1 plug-in only moves to 0.3.4 with it: macOS has no Apple AV1 hardware decoder.
- `vcnvtkext/released-builds.txt` now lists the two plug-ins of release 1.1.2, so a loader built
  from this tree still accepts them.

Not run on a Mac with an Intel iGPU.

## Decoders 0.3.3

The two plug-ins report version 0.3.3 (they were 0.1). What changed since 0.1, in short:

- Only AMD's accelerator is used. With an Intel iGPU active the decoders could take Intel's
  accelerator, which AMD's driver refuses, and nothing decoded.
- Each decode command has a time limit (1 s). The driver's own wait blocks in the kernel with no
  limit, so a command the GPU never finished held the decoding process until a reboot. A stuck
  session is closed on a background queue and later frames fail at once.
- Each session releases its accelerator (`AVD_DestroyAVDAccelerator`), as Apple's decoders do.
  Before, every session left three IOKit connections open until the process ended; Chrome keeps
  one process for all its videos.
- A stuck command or a failed driver call is reported as `kVTVideoDecoderMalfunctionErr`, not
  as bad data, so the host can fall back to another decoder.
- Plug-in threading: calls made from inside the output callback no longer trap or hang,
  invalidate and finalize wait for the calls on their way in, finalize may run from the output
  callback, and `canAcceptFormatDescription` no longer waits behind the GPU.
- More stream checks before anything reaches the GPU (AV1 sequence header, references, tile
  groups, VP9 bit depth and reference indices), and refused headers leave the parsers as they
  were.

Decoders 0.3.3 as a whole has not been run on a GPU yet. `tests/run.sh` runs the checks that need
no GPU (see `tests/README.md`).

## Encoding fix (optional)

macOS's hardware encoder leaves three things wrong on Navi 21: the video engine is never asked for clocks and
can stay at its lowest speed for a whole recording, no speed preset is ever selected, and every HEVC frame wider
than 4096 runs a small GPU job with more threads per group than the GPU allows, which stalls it.
**VCNEncodeFix 1.5.1** fixes all three with a background service (patches the encoder process in memory, no reboot)
and an optional Lilu kext in the OpenCore EFI (clock floor while encoding). It never touches the system volume.
From 1.3.0 the same service also lets Continuity Camera (an iPhone as the Mac's camera) and FaceTime calls decode
the incoming HEVC video on the GPU: it patches each `ContinuityCaptureAgent` and `avconferenced` in memory, in place
of a `DYLD_INSERT_LIBRARIES` library (details in `vcnencodefix/README.md`). That part is known for VideoProcessing of
15.8.2 and 26.7.2/26.7.3 only, and was not run with a camera or a call yet; `--no-cc` turns it off.

It is separate from the decoders: the installer shows it in its own collapsed section, with its own checks
(including an "Access to the encoder process" line) and Install/Remove buttons, and installs nothing unless asked.
It is enabled on Sequoia 15.8.2 (24H232) and 15.8.3 (24H241) and Tahoe 26.7.3 (25G342)
(`"encode": true` in `installer/validated-builds.json`).

Requirements and behaviour that matter in 1.2.x:

- The service needs SIP's **Debugging Restrictions** off (bit `0x4` of `csr-active-config`, for example
  `03080000` becomes `07080000`). With them on, macOS refuses root `task_for_pid` on the encoder process, so the
  service patches nothing; the installer shows a warning and the clock-floor kext still works.
- OpenCore kext entries use **MinKernel 23.0.0** (Sonoma through Tahoe). Older installs that wrote `25.0.0`
  (Tahoe only) skip the kext on Sequoia; run Install again on the EFI to rewrite that one line (with a backup).
- The service runs as launchd **Interactive** and re-checks spawning pids every 10 ms so a new encoder is patched
  before the first wide HEVC frame under CPU load.
- New in 1.2.3 and 1.2.4: the service and `vtenc` are built for macOS 14.0 (the 1.2.2 binaries were built for 26.0
  and would not start on Sequoia); the service log keeps growing after newsyslog rotates it; a service macOS refuses
  access to is logged once instead of four times a second; the package scripts check everything before changing
  anything; the kext sets its clock floor only on chips whose clock levels were read (Navi 21, and Navi 23, which
  was never run). New in 1.3.0: the Continuity Camera and FaceTime fix (cc); the kext is unchanged except its
  version string. Details in `vcnencodefix/CHANGELOG.md`.

Results so far: only Tahoe 26.7.3 has been measured on a GPU here (RX 6800 XT, OBS 33) — HEVC 4K60 from 55 %
repeated frames to 0.3 %, 3840x4320@30 from about 10 fps to real time, 5120x2160@30 from GPU stalls to real time.
On Sequoia the patched sites were checked in IDA (15.8.2 and 15.8.3 share the same encoder binaries) and the wide
patch CPU test covers all three plug-in builds. One user on 15.8.3 with an RX 6900 XT and NootRX reported, with the
standalone package: HEVC 60 fps exports at 4K from 72 to 99 fps, 5120x2880 from 40 fps with 5–10 s hangs to 58 fps
with none, and 7680x2160 from glitches that needed a reboot to 52 fps. The kext hooks either Navi 21 power library
(`AMDRadeonX6000HWLibs` or `AMDRadeonX6800HWLibs`), so NootRX setups are covered in code (load on that user's Mac
was not confirmed).

Details, limits and the command-line install are in `vcnencodefix/README.md`. Version history is in
`vcnencodefix/CHANGELOG.md`.

## Layout

| Folder | What it is |
|---|---|
| `vcnav1/` | AV1 core: bitstream parser, VCN submission. `decode_ivf` test tool |
| `vcncommon/` | What both cores share: the driver interface (`avd.h`), the accelerator lookup and the driver build check (`vcndriver.c`) |
| `vcnvp9/` | VP9 core: parser, VCN messages, IVF/WebM readers. `decode_vp9` test tool |
| `vtav1/` | `AV1VCNDecoder.bundle`, the VideoToolbox plug-in for `av01`. `vtav1test` drives it through the public VT API |
| `vtvp9/` | `VP9VCNDecoder.bundle`, the VideoToolbox plug-in for `vp09`. `vtvp9test` drives it through the public VT API |
| `installer/` | The graphical installer app and its shell engine. See `installer/README.md` |
| `vcnencodefix/` | The optional encoding fix (VCNEncodeFix): kext, service, scripts, tests. `prebuilt/` holds the built package the installer embeds |
| `vcnvtkext/` | VCNVTLoader: Lilu kext that lets Safari load the plug-ins when AMFIPass or the boot-arg don't. Optional, see its README |
| `vcnprofilekext/` | Lilu kext that switches the GPU to the VIDEO workload profile. Measurement tool, see Status |
| `tools/` | Reverse-engineering helpers (lldb dry runs, symbol and struct tools). They need Apple binaries that are not included |
| `webtest/` | Local test pages for MSE and mediaCapabilities. The test videos are not included |
| `userscript/` | Optional userscript that makes YouTube prefer AV1 |

## Building

Needs the Xcode Command Line Tools. Everything builds for x86_64.

```bash
./vtav1/build.sh
./vtvp9/build.sh
```

Each script writes the bundle to its own `build/` folder. The installer packages them:

```bash
./installer/build-app.sh
./installer/make-dmg.sh
```

Test tools: `vcnav1/build.sh` builds `decode_ivf`, `vtav1test`, `probe_ctx` and `tools/driveruuid`,
`vcnvp9/build.sh` builds `decode_vp9` and `vp9dump`, and `vtvp9/build.sh` also builds `vtvp9test`.
A missing or unreadable reference, a frame missing from the output, a frame past the end of the
reference and a damaged container are failures in `decode_ivf`, `decode_vp9`, `vtav1test` and
`vtvp9test`. `tests/README.md` lists which of them can hang the GPU.

The tests that need no GPU (parsers, plug-in glue and driver sessions against a made-up
driver, the validated-builds list) build and run with:

```bash
./tests/run.sh
```

The two kexts need Lilu's headers and MacKernelSDK:

```bash
KEXT_SDK=/path/to/sdk ./vcnvtkext/build.sh
```

`KEXT_SDK` must contain `Lilu.kext` (a build that ships `Contents/Resources/Headers` and `Library`)
and a `MacKernelSDK` checkout.

`vcnvtkext` allows the two plug-ins by cdhash. `vcnvtkext/build.sh` makes the list when it builds,
from `vtav1/build/` and `vtvp9/build/` (built first if missing), `vcnvtkext/released-builds.txt` and
any bundle named on its command line. Build the kext again whenever a plug-in is rebuilt; for a
release, list the DMG's bundles (see `vcnvtkext/README.md`).

VCNEncodeFix builds the same way (`KEXT_SDK=/path/to/sdk ./vcnencodefix/build.sh`, writes
`vcnencodefix/dist/`), and `build-app.sh` prefers that build. Without it, `build-app.sh` uses the checked-in
`vcnencodefix/prebuilt/VCNEncodeFix-<version>.zip` (after checking its SHA256SUMS), which is what the release CI
does. To refresh the prebuilt zip after a change: `./vcnencodefix/build.sh && ./vcnencodefix/export.sh .`
(from a VCNEncodeFix checkout of its own, `./export.sh <this repo>`).

A bundle you build yourself has a different cdhash from the one in the released DMG, even when the
machine code is identical: the symbol table's debug map records the build folder and the object
file timestamps. The installer compares cdhashes,
so with the released decoders installed it shows **Update** for your build. That's expected.

## Validating a new macOS build
Get the build's x86_64 dyld shared cache (from an install, or from the update's
`payloadv2/image_patches/cryptex-system-x86_64` through `tools/rawpatch`) and run:
```bash
python3 tools/dscdiff.py info <cache> AMDRadeonVADriver2
```
If the UUID is already in `kValidatedDrivers`, the driver is the same binary: add the build to
`installer/validated-builds.json` and you are done. If it is new, compare it against a validated one
with `tools/dscdiff.py diff <old cache> <new cache> AMDRadeonVADriver2` and review every changed
function on the decode path before adding the UUID.
`tools/mk4k.py` (AV1) and `tools/mkvp9.py` (VP9) make test clips of any size with per-frame
references for `vtav1test` and `vtvp9test`.

First VP9 run on a build that is listed as `"untested-on-gpu"`. The bundle is loaded
in the test process only, nothing is installed, and every frame is compared with a CPU decode:
```bash
python3 tools/mkvp9.py /tmp/v.ivf 640 360 30
./vtvp9/build.sh
./vtvp9/vtvp9test -load vtvp9/build/VP9VCNDecoder.bundle /tmp/v.ivf /tmp/v.ivf.md5 -id com.droga.videodecoder.vp9.vcn.vp09
```
`tools/mkvp9.py` needs PyAV and NumPy. Save your work first: a wrong message can hang the GPU.

- AV1 must only go to VCN instance 0. Instance 1 has no AV1 on Navi 21, and a job sent there hangs
  the GPU. The code pins instance 0. Don't change that.
- The plug-ins only submit on driver builds whose layout was checked in IDA: `kValidatedDrivers` in
  `vcnav1/vcnav1.c` and `installer/validated-builds.json`. A new macOS build needs that check first.
- The test tools refuse to touch the GPU unless you set `VCNAV1_ALLOW_GPU=yes-i-know-it-can-hang`
  (AV1) or `VCNVP9_ALLOW_GPU=yes-i-know-it-can-hang` with `VCNVP9_SESSION=vp9` (VP9).

## Status

- RX 6800 XT, Tahoe 26.7.1: AV1 and VP9 conformance vectors match bit for bit. YouTube 4K60 plays in
  Safari and Chrome (AV1: 6 dropped of 2509 frames, about 6.8 ms per frame).
- RX 6800 XT, Tahoe 26.7.3: 43 AV1 clips and 18 VP9 clips match bit for bit, portrait sizes included
  (up to 2160x3840).
- RX 6900 XT, Sequoia 15.8.2: all AV1 vectors match bit for bit and AV1 playback works, portrait
  Shorts included. VP9: the dry runs pass and the 640x360 check clip decodes on the GPU with 30
  frames, 0 mismatches. The larger VP9 clip set was not run there. VP9 was software up to 1.0.2,
  because the VP9 plug-in refused the Sequoia driver.
- Sequoia VP9, static check: against the Tahoe driver the AV1 processor and factory vtables have
  the same slot order, the objects have the same sizes and every offset the plug-in uses matches, and
  `AMDRadeonX6000HWLibs` carries a byte-identical VCN firmware (`aucVcn3v0_Firmware`). The Sequoia
  driver is now in `kValidatedDrivers` in `vcnvp9/vcnvp9.c`.
- Sequoia 15.8.3 and Tahoe 26.7.3 ship the same driver binaries as 15.8.2 and 26.7.2 (same UUID).
- Fixed: portrait AV1 video (YouTube Shorts) came out as a blank picture. The driver's AV1 path
  writes nothing when the output surface's padded height is larger than its pitch. The decoder now
  widens the rows of such surfaces. VP9 was not affected.
- 8K AV1 tops out at about 28 fps (about 35 ms per frame). 8K60 needs 16.7 ms.
- Pinning the GPU clocks with stable power mode gave 4-5% at 4K and 1.4% at 8K, for about 27 W more
  at idle. Not worth it.
- `vcnprofilekext`: the firmware accepted the VIDEO profile, but speed and power didn't change.
- Decoders 0.3.3: built and checked on the CPU only (`tests/run.sh`, against a made-up driver).
  Not run on a GPU yet.
- `vcnvtkext` (VCNVTLoader 1.1.0): booted on Tahoe 26.7.3 with AMFIPass off; Safari played AV1
  and VP9 4K in hardware through it. Not booted on Sequoia. The old CoreMedia patch for `/Library`
  is gone: it never ran, because Lilu's user-space patcher is off on macOS 11+, so the installer
  writes to `/System`.
- VCNEncodeFix 1.3.0: wide-patch CPU unit test passes for the Tahoe 26.7.3, Sequoia 15.8.2/15.8.3 and
  Sonoma 14.8.9 plug-in builds (1025 widths each, 0 failures), the cc test for the VideoProcessing of
  15.8.2 and 26.7.2/26.7.3 (every altered byte refused, original and patched code run on the CPU, every
  site found once in a running 26.7.3 and in copies of both builds), the floor rule test for 20 AMD
  device-ids, 23 EFI tests. The 1.2.4 kext and service were booted on an RX 6800 XT (Tahoe 26.7.3); the
  cc fix was not run with a camera or a FaceTime call; nothing ran on a Navi 23.
  Full kext package build still needs `KEXT_SDK` (Lilu + MacKernelSDK); without it the installer
  embeds `vcnencodefix/prebuilt/`. `prebuilt/VCNEncodeFix-1.3.0.zip` was built with
  `SOURCE_DATE_EPOCH=1795283122`; its kext has the same bytes as the 1.3.0 kext built from the upstream tree.

## Checking support

`VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9)` returns false until the app calls
`VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9)`. Safari, WebKit, Chrome, Firefox and FFmpeg do that.

This is Apple's own gate, read in VideoToolbox of macOS 15.8.2: that call sets a process-wide flag; when a decoder
session is made, VideoToolbox adds `AllowVP9Decode` to the decoder specification only if the flag is set, and for
`vp09` it refuses to list any decoder (Apple's or this one) without that key (-12906,
`kVTCouldNotFindVideoDecoderErr`). Only `VTDecoderXPCService`, the remote decode server, skips the check. AV1 has no
such gate: a client that asks for nothing, or for hardware, gets this decoder. `tools/vtdiag` shows both: AV1 on
hardware with no specification, VP9 refused before the call and on hardware after it.

## Not included

- Apple binaries extracted from macOS (drivers, kernel collections, frameworks) and IDA databases.
  The `tools/` scripts expect you to supply those from your own install.
- Test videos for `webtest/` and the conformance vectors. The AV1 vectors come from the AOM test
  suite, and the VP9 references from FFmpeg FATE (`tests/ref/fate/vp9-*`).
- Build output. Run the build scripts. The one exception is `vcnencodefix/prebuilt/`, because the release CI
  has no Lilu/MacKernelSDK to build the kext.

## Third-party code

`vcnvp9/ac_vcn_vp9_default.h` comes from AMD via Mesa and keeps its MIT license header.
