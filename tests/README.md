# Tests that need no GPU

```bash
./tests/run.sh           # builds and runs all of them
./tests/run.sh --quick   # fewer damaged copies per stream
```

`tests/fetch_media.sh` (run by `run.sh`) downloads the public test streams into
`tests/media/` and checks each one against `tests/media-sha256.txt`: AV1 vectors
from the AOM test data, VP9 vectors from libvpx, and libvpx's damaged VP9
streams. Nothing here opens the GPU, loads a kext or installs anything. The
programs are built with the address and undefined-behaviour sanitizers.

| Program | What it checks |
|---|---|
| `parsedigest` | The AV1 and VP9 header parsers read every stream exactly as recorded in `expected-digests.txt`, survive libvpx's invalid streams, and survive thousands of copies damaged at random (`--mutate`) |
| `acceltest` | The rule that picks the accelerator handed to AMD's driver (only a codec whose name starts with "AMD"), then lists this machine's `IOAccelerator` services and says which one the decoders take. Read-only |
| `av1state` | A refused AV1 sample or frame header leaves the parser as it was: no frame from a half-written header, reference state restored, tile groups in order |
| `vp9state` | A refused VP9 frame header restores the parser state it changed (colour config, loop filter deltas, segmentation); a frame with no tile data is refused |
| `plugintest-av1`, `plugintest-vp9` | Each plug-in's VideoToolbox side, with the decoder session replaced by a stand-in: factory and session start, codec check, shown and dropped frames, a sample of only a hidden frame (its picture, no error), a missing reference (`kVTVideoDecoderReferenceMissingErr`), calls made from inside the output callback, invalidate, finalize from the output callback |
| `waittest` | The wait for one decode command (`vcn_wait_command`) against a made-up driver: done, firmware error, driver error, and a command that never finishes, which must time out |
| `sessiontest-av1`, `sessiontest-vp9` | Both decoder sessions against a made-up driver: everything is released once and in Apple's order (DeleteContexts, FreeBitstreamBufferPool, DeleteDriver, AVD_DestroyAVDAccelerator, the service), also after each failed create step, after a stuck command and on a driver without the destroy export. With a second clip: a hidden frame sent alone keeps its picture for the plug-in, and a frame whose reference was never decoded is a reference failure |
| `check_lists.sh` | `installer/validated-builds.json` names the same drivers as `kValidatedDrivers` in `vcnav1/vcnav1.c` and `vcnvp9/vcnvp9.c`: a build marked usable must be in the C lists, a driver in the C lists must be marked usable, and one driver may not be usable for one build and `false` for another |

## Programs that talk to the driver or the GPU

`tests/run.sh` never runs these. Run them by hand on a machine with the card,
one at a time, smallest clip first, after saving your work: a wrong message to
the video block can hang the GPU and the Mac then has to be reset.

| Program | Built by | Can hang the GPU | Armed by |
|---|---|---|---|
| `vcnav1/decode_ivf` | `vcnav1/build.sh` | yes | `VCNAV1_ALLOW_GPU=yes-i-know-it-can-hang` |
| `vcnvp9/decode_vp9` | `vcnvp9/build.sh` | yes | `VCNVP9_SESSION=vp9` and `VCNVP9_ALLOW_GPU=yes-i-know-it-can-hang` |
| `vtav1/vtav1test`, `vtvp9/vtvp9test` | `vcnav1/build.sh`, `vtvp9/build.sh` | yes | nothing: a decode run is armed |
| `tools/vtchurn` | `vcnav1/build.sh` | yes | nothing: every session decodes |
| `vcnav1/probe_ctx` | `vcnav1/build.sh` | low: it opens one AV1 context on the first VCN instance and decodes nothing | nothing |
| `tools/driveruuid` | `vcnav1/build.sh` | no: it only loads the driver's code to read its UUID | nothing |
| `tools/dryrun.py`, `tools/dryrun_vp9.py` | lldb scripts | the driver's own session setup goes out; our commands are stopped before the kernel | the scripts arm the program themselves |

`tools/vtchurn -load vtav1/build/AV1VCNDecoder.bundle av1 clip.ivf -n 50`
opens 50 sessions in one process and prints the process's Mach ports and IOKit
user clients before, during and after; a session that leaks shows as growth
per session after the first.
