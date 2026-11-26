# Changelog

## 1.5.1 (not released)

- *cc* also patches `AirPlayXPCHelper`, the AirPlay sender's video encoder host. For an extended display or
  mirroring it asks `VCPCompressionSessionCreate` for usage 42, and on a MacPro7,1 / iMacPro1,1 board-id the check
  that site E jumps over sent that to the T2 (`No bridgeOS device available`, error -18, black screen on the
  receiver). The service now patches it the way it patches `ContinuityCaptureAgent` and `avconferenced`; no new
  site, the same VideoProcessing image. Not run against an AirPlay receiver.
- *hevc10* now knows the `AMDRadeonVADriver2` of macOS 15.8.2 (24H232, UUID
  `5D90FED3-BCBF-3475-9F4B-8FC00026D3AB`, the same binary as 15.8.3). Its two places are the same code as in
  26.7.3 (`Vcn3Encoder::setInputAndOutputFormatParams` +0x2a and `AVDVcn3HevcEncDataProcessor::setupDeblocking`
  +0x83: same instructions, same struct offsets, only three rip-relative constant addresses differ). Read in
  Hopper; `tests/hevc10_test.c` finds each place exactly once in this Mac's running driver. Main10 itself was not
  run on a GPU here: VideoToolbox offers it only with the kext's `-vcnhevc10` gate, which this Mac does not have,
  so the 10-bit picture is unmeasured on 15.8.2. The service still patches only 10-bit input.
- New `tools/vtdiag` (in the repository's `tools/`): checks the hardware decoders, hardware H.264 and HEVC encodes
  up to 7680x4320 and, with `-psnr10`, a Main10 round trip with PSNR. AV1 and VP9 are reported as having no
  hardware encoder (VCN 2/3 cannot encode them). 8192x4320 HEVC is past the video engine's limit and is not
  attempted.

## 1.5.0 (not released)

- *cc* also fixes FaceTime's video encoder on a Mac Pro (MacPro7,1) or iMac Pro (iMacPro1,1) SMBIOS. On those
  board-ids VideoProcessing sends FaceTime's encoder (`VCPCompressionSessionCreate`, usage 42) to the T2's remote
  encoder, which a Mac without a T2 does not have: `No bridgeOS device available`, error -18, no outgoing video.
  The same check makes `VCPCodecCopyProperties` report H.264 as unsupported for usage 42, so AVConference asks for
  HEVC, which the local path refuses (`No HEVC video encoder support for FaceTime on Mac yet`). A fourth cc site
  (E) jumps over the check's two board-id compares to its own release of the board-id string and its
  `BridgeEncode` preference check, so it answers no unless that preference is set and a bridge device exists, as
  on any other Mac. Then H.264 is offered and the session is a local VideoToolbox one (`h264.gva` when hardware is
  required). Same builds as before: VideoProcessing of 15.8.2 and 26.7.2/26.7.3 (the check is the same code in
  both, only its call and string offsets differ).
- A process that an older version patched (sites K, A and B only) is now completed with the missing site instead
  of being left alone as "unexpected bytes"; `status` shows such a process as `partly patched`.
- `tests/cc_test.c` covers site E: lookup and state like the other sites, the original and the patched check run on
  the CPU for both board-ids, another one and none, with and without `BridgeEncode` and a bridge device, and on this
  Mac the two board-id strings, `BridgeEncode` and every call of the check. New `tests/ftenc_test.c`, built by
  `build.sh` and run by hand: on a Mac Pro or iMac Pro board-id it asks VideoProcessing for FaceTime's encoder
  before and after writing site E in its own process.
- Run on an RX 6800 XT, macOS 26.7.3, MacPro7,1 SMBIOS, with `ftenc_test`: before, `VCPCodec_AVC1` 0 and the
  session -18 (the log shows `No bridgeOS device available`); after, `VCPCodec_AVC1` 1 and `Created video encoder
  with usage 42 on macOS`, 30 H.264 frames with hardware required. Not run in a FaceTime call or on Sequoia.
- Only the service changed. The kext's code is the same as 1.4.0 (it says 1.5.0 because `build.sh` stamps the
  version), so the EFI does not need updating and no reboot is needed for this fix.

## 1.4.0 (not released)

- New service fix, *hevc10*: 10-bit HEVC (Main10) hardware encoding on the VCN 3 of RX 6000 cards. Two bugs in
  `AMDRadeonVADriver2` broke it, and the service changes both places in every `VTEncoderXPCService`, together or
  not at all:
  - `Vcn3Encoder::setInputAndOutputFormatParams` told the video engine the input was 8-bit NV12 even in a 10-bit
    session (a green, striped picture, about 7 dB PSNR): the encoder context gets NV12 and its `x420` branch
    leaves the packing format at 0. It now sets 10-bit and P010 from the session's 10-bit SPS flag; 8-bit HEVC,
    H.264 and RGB take the same path as before.
  - `AVDVcn3HevcEncDataProcessor::setupDeblocking` copied only the picture's Cb/Cr QP offsets (0, 0) into the
    deblocking packet and dropped the slice's Cr offset of -3 that `AppleGVAHEVCEncoder` sets for 10-bit and the
    driver writes into the stream (Cr scaled by about 0.73, V PSNR 20.4 dB). It now adds the slice's offsets to
    the picture's, as Mesa does (0 + 0 for 8-bit).
- Known for the `AMDRadeonVADriver2` of macOS 26.7.3 (25G342, UUID `1181E2CA-31B3-3DF0-90E2-3AE70552D753`) only.
  On any other build *hevc10* stays off and `status` and the log say why; the other fixes are not affected.
- If writing the second place fails, the first one is put back, so a process is never left half patched (this
  covers every patch made of several sites).
- `--no-hevc10` (service and `install.sh`) turns it off. `status` now shows `off` for a fix turned off with any
  `--no-*` option, and exits 3 when every fix is off or unavailable, so the installer's check no longer accepts an
  install that would do nothing.
- Kext: new opt-in boot argument `-vcnhevc10` (off by default). With it, the kext prepares `IOGVAHEVCEncode` =
  `"2"` and profile 2 (HEVC Main10) in `IOGVAHEVCEncodeCapabilities` on supported AMD accelerators, but publishes
  them only while the service says the driver fix is active, through the root-only sysctl `hw.vcnhevc10`:
  `vcnencodefix watch` writes 1 when *hevc10* is active and SIP allows access, and 0 when it stops, so
  VideoToolbox never sends 10-bit HEVC to an unpatched driver. Its state is in `VCNHEVC10` on `IOResources`
  (`waiting for vcnencodefix service` until then, `off: service says the driver patch is not active` after).
  `status` shows the gate. Without `-vcnhevc10` the kext behaves as before; with an older kext the service logs
  once that `hw.vcnhevc10` is not there and goes on. The kext needs the EFI update and a reboot.
- `tests/hevc10_test.c` (the table, and every site found once in this Mac's driver when it is the 26.7.3 build)
  and `tests/hevc10_gate_test.cpp` (the kext's gate states) run in `build.sh` next to the cc test.
- Run on an RX 6800 XT, macOS 26.7.3: 1080p Main10 encode with `hevc_videotoolbox -profile:v main10 -allow_sw 0`,
  PSNR Y 57.4, U 54.9, V 57.3 dB; 8-bit encodes unchanged. Not run on Sequoia or on an RX 6900 XT.

## 1.3.0 (not released)

- New service fix, *cc*: the incoming HEVC video of Continuity Camera and FaceTime calls decoded by the GPU
  instead of the CPU, in every `ContinuityCaptureAgent` and `avconferenced`. Both decode through AVConference
  (`VideoDecoder_NewFormat_InitDecoder`), which puts `DecoderUsage = 1` in the decoder specification itself before
  VideoProcessing's VCPDec adds it again; with it AppleGVA takes its FaceTime path and the AMD driver refuses
  ("[HEVC] GVA ERROR: Couldn't set the stream resolution"). Patched: VCPDec's own removal of that key before the
  hardware decoder (restored for the software retry) runs for every codec, not only for other codecs on some
  board-ids, and `VCPDecompressionSessionSetProperty` drops the same key as a property with Apple's own check for
  it. Builds: VideoProcessing of 15.8.2 (24H232) and 26.7.2/26.7.3 (25G332/25G342, the same file). The site
  bytes and the jumps are checked per build; any other build stays off. Read in IDA on both builds (AVConference
  too); the call targets confirmed on 26.7.3 in a live process. Not run with a camera or a call: an interpose
  library that drops the same key gave a hardware decoder at 60 fps on an RX 6900 XT (15.8.2, Continuity Camera).
- `--no-cc` (service and `install.sh`) turns it off; `patch` / `unpatch <pid>` take those processes too;
  `status` shows the cc line and each of them running.
- `tests/cc_test.c`: for each build, the lookup refuses every single-byte change, a second copy, a B whose key
  load is not K's and an A that removes another slot; the state reader leaves altered bytes alone; the original
  and patched code run on the CPU with the calls recorded. With this Mac's own build (and any `CC_IMAGES` copies)
  it also finds every site once in the real file and checks the key and the call targets.
- The shared-cache lookup used by *preset* is shared with *cc*; *preset* is unchanged.
- The kext's code is unchanged; only its version string says 1.3.0. A 1.2.4 kext on the EFI can stay (no reboot
  needed), although `install/encoder.sh check` reports the new version as an update.

## 1.2.4 (not released)

- The kext sets the clock floor only on chips whose video clock levels were read, so the
  floor stays inside them. The same driver family runs chips whose top levels are below
  the floor (an RX 5600 XT table: VCLK 1267, DCLK 1086 MHz, against 1322 and 1225), and
  1.2.3 sent it to every chip. Now:
  - Navi 21 (device-id 0x73A0 to 0x73AF, 0x73BF): 132297 as before. This Mac's own
    table (`PP_PhmSoftPowerPlayTable`) has VCLK 363/1400 and DCLK 317/1225 MHz.
  - Navi 23, the RX 6600 family (0x73E0 to 0x73EF, 0x73FF): the same value, which lands
    inside its levels: two published RX 6600 XT tables have VCLK 690/1429 and DCLK
    572/1250 MHz. Not run on a card; the state says so.
  - Any other AMD GPU (RX 5000, Navi 22): off, unless `vcnfloor=N` is on the boot
    arguments, which is taken as the user's own value. When Lilu cannot list the GPUs, it
    works as before.
- The chip rule is in `kext/floor_logic.hpp`; `tests/floor_test.cpp` checks it against
  every AMD 0x73xx device-id in pci.ids (0x73F0 is Navi 33, not Navi 23) and the floor
  against each table's levels.
- The installers still stop on a card that is not a Navi 21; their message now says what
  the kext does on Navi 23.
- Booted on an RX 6800 XT (26.7.3): state "active: encode", `test.sh` 102 fps (preset) and
  p50 21.3 ms (floor), the same as 1.2.3. Not run on a Navi 23.

## 1.2.3 (not released)

- Scripts: `install.sh`, `uninstall.sh`, `doctor.sh` and `test.sh` run the system's own tools only (`PATH`
  set), whatever PATH sudo kept, and clear the quarantine flag with `xattr -s` so a link in the package does
  not make root clear it on the link's target.
- Kext: the log of the first 32 floored requests now takes its slot atomically, so two encode sessions at once
  cannot go past the cap.
- Tool: `patch <pid>` and `--interval` refuse anything that is not a whole number (atoi read `abc` as 0). The
  pid list survives a failed allocation, the log reopen keeps standard output when `open()` returns 1 or 2, and
  the free space checked for the wide patch must also be inside the file.
- config.plist editing (`efi`): every allocation is checked, and a path too long for the temporary or backup
  name stops with an error instead of using a cut name.
- watch: an encoder service macOS refuses access to (SIP Debugging Restrictions on) was asked again on every
  full scan, four times a second, with one log line each time, for as long as it ran. It is now logged once and
  remembered like a patched service. A failed allocation of the pid lists no longer leaves a NULL list in use,
  and `--interval` takes 20 to 60000 ms (a negative value used to become a huge one).
- vcnencodefix and vtenc are built for macOS 14.0 again. 1.2.2 built them for 26.0, so on Sonoma and Sequoia, which
  the patch tables cover, they would not start at all. Built clean for 14.0 with no availability warning; not run on
  14 or 15 here.

- The service reopens its log after newsyslog rotated it. launchd opens the log once and newsyslog renames it at
  1 MB, so the service went on writing into the renamed file and /var/log/vcnencodefix.log stayed empty until the
  next restart (test.sh --wide then skipped its encode). Tried: the line after a rename goes to a new file.
- install.sh checks everything that can refuse before the first change: OpenCore on the EFI, Lilu and its version
  (1.7.0 or newer, from the kext's own requirement), Lilu enabled, a config entry it can write (a disabled entry,
  one before Lilu or with other settings is refused instead of failing after the service and the kext were
  already replaced), and free space. The service files are staged first and the old service is only stopped once
  that worked; the script waits for launchd to let go of it before starting the new one. The kext is swapped
  through a .old copy with Ctrl-C held back. Exit status 2 when the service does not stay running.
- uninstall.sh checks --efi before removing anything, removes the launchd plists before the binary (a plist left
  alone would start a missing binary every 30 s), fails when the kext cannot be deleted, and removes leftover
  .new/.old copies. Empty, relative and "/" --efi paths are handled in both.
- doctor.sh shows the service's options and whether the installed binary is the package's, reads the boot
  arguments the kernel started with, and always ends with 0.
- test.sh --wide only counts log lines written during the test (an old line about a reused pid could let a
  5120-wide encode through unpatched), stops its encode if the test is interrupted, refuses unknown options and
  returns failures.
- The config editor: efi-check no longer says "add updates it" for an old entry before Lilu, which add refuses; an
  entry removed from a file with CRLF line ends keeps the rest of the file as it was.
- efi_test.sh resolves its arguments before changing folder and refuses a missing tool or sample, which had let two
  refusal tests pass.

## 1.2.2

- The service needs SIP's Debugging Restrictions off (bit 0x4 of csr-active-config). With them on, macOS refuses
  the service access to Apple's encoder process even as root, so nothing was patched and the log only said
  "task_for_pid failed ((os/kern) failure)". This was never documented: the test machine had SIP fully relaxed. In
  15.8.3's AMFI (checked in IDA), root only gets the task port of such a process when csr_check(CSR_ALLOW_TASK_FOR_PID)
  allows it.
- `vcnencodefix status` (and so doctor.sh and install.sh) shows an "access" line: allowed, or REFUSED with what to
  change. install.sh warns but still installs. The service logs a warning at start and names SIP in each refused
  access.
- README: the requirement, how to set it in OpenCore, and a troubleshooting entry.
- The service ran as a launchd Background job, i.e. at priority 4. When the CPU was busy it could notice a new encoder
  service late: once 1.6 s after launch while OBS started a 5120x2160 recording, and up to 561 ms with every core
  loaded (normally 25-100 ms). OBS sends its first frame about 260 ms after the launch, and an unpatched encode wider
  than 4096 stalls the GPU. The job is now ProcessType Interactive. Its CPU use stays about 0.5% of one core.
- The 10 ms check looked at each new pid once. launchd starts the service in a fork of itself (about 1 ms), then
  xpcproxy (about 11 ms with every core loaded), then the service, so a pid caught in one of the first two stages
  waited for the next full scan (up to 250 ms; measured up to 235 ms with every core loaded). Such a pid is now
  checked again on every tick until it becomes the service.
- The kext is unchanged from 1.2.1 except for the version number.

## 1.2.1

- install.sh --efi wrote the kext's OpenCore entry with MinKernel 25.0.0 (Tahoe), so on macOS 14 and 15 OpenCore
  skipped VCNEncodeFloor.kext and doctor.sh said "not loaded". This came from 1.0.0, when only Tahoe was supported,
  and was missed when 1.1.0 added Sonoma and Sequoia. New entries now use MinKernel 23.0.0, the same range as the
  kext's Lilu configuration.
- efi-add updates an entry written by 1.0.0 to 1.2.0 in place: it changes only that MinKernel line, verifies the
  result and keeps a backup. Running `sudo ./install.sh --efi /Volumes/EFI` again is enough. efi-check reports such
  an entry as "old entry".
- doctor.sh: on macOS 14/15, "not loaded" now points to that fix.
- The kext and the service are unchanged from 1.2.0 except for the version number.

## 1.2.0

- VCNEncodeFloor.kext also hooks AMDRadeonX6800HWLibs, the second Navi 21 power library in macOS. macOS loads
  AMDRadeonX6000HWLibs for the RX 6800 / 6800 XT / 6900 XT, but setups with NootRX load AMDRadeonX6800HWLibs instead,
  and there the clock floor stayed at "waiting for AMDRadeonX6000HWLibs". PEM_CWDDEPM_PMRequest is the same code in
  both libraries (Hex-Rays: only the source line numbers of its log calls differ), in 14.8.9, 15.8.2, 15.8.3 and
  26.7.3; 14.8.9 ships X6800HWLibs stripped too, so there it is routed at its offset after a 32-byte compare.
- Each library keeps its own trampoline, so a Mac with two Navi 21 cards that load both is handled.
- Not run on a GPU with X6800HWLibs: the floor value was measured on an RX 6800 XT, and other cards' clock levels come
  from their own VBIOS.

## 1.1.0

- Supports macOS 14.8.9 (23J631), 15.8.2 (24H232), 15.8.3 (24H241) and 26.7.3 (25G342). Each patch keeps a table of
  validated builds (LC_UUID) with the exact bytes it expects; any other build is still left alone.
  - Apple's HEVC encoder plug-in is the same file in 15.8.2 and 15.8.3; the call site moved (0x13780 on 15.8.x,
    0x142CA on 14.8.9) and on 14.8.9 two kept register moves differ. The Metal kernel uses only
    thread_position_in_grid in every build.
  - AMDRadeonVADriver2 is the same in 15.8.2 and 15.8.3; on 14.8.9 only the jump distance after the preset load
    differs. VAUveEncoder::EncodePicture reads the preset the same way in all of them.
  - AMDRadeonX6000HWLibs: 15.8.x is identical to 26.7.3 in PEM_CWDDEPM_PMRequest, PP_Cwdde and PEM_ClockChange;
    14.8.9 ships the kext without symbols, so the kext routes the function at its offset after comparing its first
    32 bytes. The payload layout (client type +16, minMclk +36, VclkFloor +60) is the same in every build.
- VCNEncodeFloor.kext loads on Sonoma to Tahoe (it was Tahoe only).
- tests: the wide-patch CPU test runs once per validated build of the plug-in.
- Only 26.7.3 has been run on a GPU. The other builds were checked in IDA and with the CPU test only.

## 1.0.1

- vcnencodefix: new encoder services are found within about 10 ms of launch instead of up to 250 ms. In OBS at
  5120x2160 the first frame arrived before the service was patched and stalled the GPU once at the start.
- vcnencodefix: log lines have milliseconds; the wide patch is applied before the preset patch.
- doctor.sh: fixed the kext state and pid display. test.sh: fixed a message error in the wide test.
## 1.0.0

First release, for macOS 26.7.3 (25G342) on Navi 21.

- VCNEncodeFloor.kext: VCN clock floor for hardware encode sessions (Lilu plugin). Checks the AMDRadeonX6000HWLibs
  build before hooking it and publishes its state on IOResources.
- vcnencodefix service: VCN speed preset for every encode; HEVC widths above 4096 no longer stall the GPU.
- install.sh / uninstall.sh, including the OpenCore config.plist entry (backup and verification).
- doctor.sh and test.sh.
