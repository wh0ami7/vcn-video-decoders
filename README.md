# AMD VCN Video Decoders

Hardware AV1 and VP9 decoding through VideoToolbox on AMD Navi 21 GPUs (RX 6800, 6800 XT, 6900 XT,
6950 XT) on OpenCore hackintoshes. Safari and Chrome then decode YouTube AV1 and VP9 on the GPU.

Made by [DrogaBox](https://github.com/DrogaBox) & [Izzy](https://github.com/wh0ami7).

<img width="1900" height="859" alt="image" src="https://github.com/user-attachments/assets/a58ecd9d-c990-4707-aea9-8a3f5d1fe01e" />


# Requirements

AMD Navi 21 GPU: RX 6800, 6800 XT, 6900 XT or 6950 XT. Other cards are untested.

OpenCore hackintosh on one of the validated builds: Sequoia 15.8.2 (24H232) or 15.8.3 (24H241), Tahoe 26.7.1 (25G241), 26.7.2 (25G332) or 26.7.3 (25G342). Other builds are blocked by default, because an unchecked driver can hang the GPU.

On Sequoia the VP9 decoder is enabled from static validation only: the driver layout and the VCN firmware match Tahoe, but nobody has run it on a Sequoia GPU yet. Run the VP9 check once (see "Validating a new macOS build") and report the result.

Authenticated root disabled, to write the sealed system volume: csrutil authenticated-root disable from Recovery.

AMFI open: the amfi_get_out_of_my_way=0x1 boot-arg or AMFIPass. Otherwise macOS refuses to load the decoders.

To undo everything: Uninstall in the app and reboot, or sudo bless --mount / --last-sealed-snapshot and reboot.


## How it works

macOS ships AMD's VCN driver (`AMDRadeonVADriver2`), but only wires H.264 and HEVC to VideoToolbox.
The driver can still run AV1 jobs. These plug-ins parse the bitstream on the CPU, build the VCN
decode messages and submit them through the driver, then hand the frames back to VideoToolbox.

VP9 is not in Apple's driver at all. `vcnvp9` builds the VCN VP9 messages itself, following the
same layout as Mesa's `ac_vcn_dec.h`.

## Layout

| Folder | What it is |
|---|---|
| `vcnav1/` | AV1 core: bitstream parser, driver interface (`avd.h`), VCN submission. `decode_ivf` test tool |
| `vcnvp9/` | VP9 core: parser, VCN messages, IVF/WebM readers. `decode_vp9` test tool |
| `vtav1/` | `AV1VCNDecoder.bundle`, the VideoToolbox plug-in for `av01`. `vtav1test` drives it through the public VT API |
| `vtvp9/` | `VP9VCNDecoder.bundle`, the VideoToolbox plug-in for `vp09`. `vtvp9test` drives it through the public VT API |
| `installer/` | The graphical installer app and its shell engine. See `installer/README.md` |
| `vcnvtkext/` | Lilu kext to load the plug-ins from `/Library` instead of `/System`. Experimental, see Status |
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

Test tools: `vcnav1/build.sh` builds `decode_ivf` and `vtav1test`, `vcnvp9/build.sh` builds
`decode_vp9`, and `vtvp9/build.sh` also builds `vtvp9test`.

The two kexts need Lilu's headers and MacKernelSDK:

```bash
KEXT_SDK=/path/to/sdk ./vcnvtkext/build.sh
```

`KEXT_SDK` must contain `Lilu.kext` (a build that ships `Contents/Resources/Headers` and `Library`)
and a `MacKernelSDK` checkout.

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

First VP9 run on a build that is listed as `"untested-on-gpu"` (Sequoia today). The bundle is loaded
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
- RX 6900 XT, Sequoia 15.8.2: all AV1 vectors match bit for bit and AV1 playback works. VP9 was
  software there up to 1.0.2, because the VP9 plug-in refused the Sequoia driver.
- Sequoia VP9, static check only: against the Tahoe driver the AV1 processor and factory vtables have
  the same slot order, the objects have the same sizes and every offset the plug-in uses matches, and
  `AMDRadeonX6000HWLibs` carries a byte-identical VCN firmware (`aucVcn3v0_Firmware`). The Sequoia
  driver is now in `kValidatedDrivers` in `vcnvp9/vcnvp9.c`. Not run on a Sequoia GPU yet.
- Sequoia 15.8.3 and Tahoe 26.7.3 ship the same driver binaries as 15.8.2 and 26.7.2 (same UUID).
- Fixed: portrait AV1 video (YouTube Shorts) came out as a blank picture. The driver's AV1 path
  writes nothing when the output surface's padded height is larger than its pitch. The decoder now
  widens the rows of such surfaces. VP9 was not affected.
- 8K AV1 tops out at about 28 fps (about 35 ms per frame). 8K60 needs 16.7 ms.
- Pinning the GPU clocks with stable power mode gave 4-5% at 4K and 1.4% at 8K, for about 27 W more
  at idle. Not worth it.
- `vcnprofilekext`: the firmware accepted the VIDEO profile, but speed and power didn't change.
- `vcnvtkext`: the AMFI part works. The CoreMedia patch doesn't land on Tahoe because Lilu can't
  patch the split dyld shared cache, so the installer writes to `/System` instead.

## Checking support

`VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9)` returns false until the app calls
`VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9)`. Safari and Chrome do that.

## Not included

- Apple binaries extracted from macOS (drivers, kernel collections, frameworks) and IDA databases.
  The `tools/` scripts expect you to supply those from your own install.
- Test videos for `webtest/` and the conformance vectors. The AV1 vectors come from the AOM test
  suite, and the VP9 references from FFmpeg FATE (`tests/ref/fate/vp9-*`).
- Build output. Run the build scripts.

## Third-party code

`vcnvp9/ac_vcn_vp9_default.h` comes from AMD via Mesa and keeps its MIT license header.
