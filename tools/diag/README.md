# AV1 and VP9 decode diagnostics (python3)

Run from anywhere; `tools/diag/run_all.py` runs all three.

| Script | What it proves | GPU |
|---|---|---|
| `fields.py` | every header field of the Linux V4L2 stateless AV1 and VP9 controls (`include/uapi/linux/v4l2-controls.h`) is kept by our parsers, derived while reading, read by the VCN firmware itself, or not a decode input. A field missing from the table fails the run | no |
| `features.py` | which coding tools the test streams use, from the decoders' own parsers (`featdump.c`). A tool no stream uses is `NOT EXERCISED`: nothing shows it works | no |
| `decode.py` | each stream decodes on the GPU with hardware required and every frame's MD5 equals a CPU decode (PyAV: libdav1d for AV1, libvpx for VP9) | yes |

```bash
python3 -m venv /tmp/diagvenv && /tmp/diagvenv/bin/pip install av      # the CPU reference decoder
tools/diag/run_all.py --python /tmp/diagvenv/bin/python
tools/diag/features.py -v --codec vp9                                    # which stream uses which tool
tools/diag/run_all.py --extra ~/more-vectors --python /tmp/diagvenv/bin/python
```

Without PyAV, `decode.py` still runs every stream but can only report `decoded`; that is not a correctness result.
Put `<stream>.md5` files next to a stream (FATE format for VP9, one MD5 per line for AV1) to use a reference
from another source. `--write-md5` keeps the PyAV reference next to the stream.

`decode.py` opens the GPU, one stream at a time, smallest first.

## Tests

`selftest.py` tests these scripts and, with `--gpu`, is the integration test of the decoders:

```bash
tools/diag/selftest.py                                        # no GPU: field check, feature dump, reference rules
/tmp/diagvenv/bin/python tools/diag/selftest.py --gpu         # also every stream on the GPU, freshly built plug-ins
```

It checks that the field check finds a member missing from our parser and a field missing from its table, that the
feature dump shows as many pictures as libdav1d / libvpx output for all 15 streams (an independent check of the
header parsers), that the VP9 reference keeps the last frame of a packet (libvpx's rule), and, with `--gpu`, that each
stream gives the result in `EXPECT`: `BIT-EXACT`, except the spatial-SVC stream, which must be `REFUSED`. Removing the
2x-smaller-reference check from `vcnvp9.c` turns that stream into a `FAIL`. The unit tests of the decoder rules are
`tests/sessiontest.c` (made-up driver) and `tests/mutants.sh` (does it notice damage).
