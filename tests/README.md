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
| `plugintest-av1`, `plugintest-vp9` | Each plug-in's VideoToolbox side, with the decoder session replaced by a stand-in: factory and session start, codec check, shown and dropped frames, calls made from inside the output callback, invalidate |
