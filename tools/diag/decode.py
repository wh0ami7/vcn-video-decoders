#!/usr/bin/env python3
"""Decodes every AV1 / VP9 test stream on the GPU through VideoToolbox and checks it frame by frame.

For each stream (tests/media/av1, tests/media/vp9, plus --extra DIR):
  1. a reference: a FATE-style per-frame MD5 list `<stream>.md5` next to the stream if there is one, else a
     CPU decode with PyAV (`pip install av`) when it is importable, else none;
  2. vtav1test / vtvp9test (built by vcnav1/build.sh and vtvp9/build.sh) decode it with hardware required and
     compare every frame's MD5 (Y, U, V planes as I420; 10-bit as 16-bit little-endian) with the reference.

Per stream the result is one of
  BIT-EXACT   every frame matched the reference
  decoded     decoded with no error, but there was no reference: NOT a correctness result
  FAIL        a frame differed, was missing, or the decode failed (the tool's output is shown)
  REFUSED     the decoder returned errors and showed no wrong picture: a frame the video engine cannot do
              bit-exactly is refused (and every frame predicted from it), so the client can fall back. Counted
              as a pass; listed at the end because that stream needs a software decoder

  decode.py [--codec av1|vp9] [--extra DIR ...] [--built] [--write-md5] [--strict] [--timeout S]
  --built      test the plug-ins just built in vtav1/build and vtvp9/build (-load, in this process only) instead of
               the ones installed in /System
  --write-md5  keep the PyAV reference next to the stream as <stream>.md5 (git-ignored, *.md5)
  --strict     `decoded` (no reference) counts as a failure
  exit 0 all fine, 2 a stream failed, 3 --strict and some stream had no reference
This opens the GPU: one stream at a time, smallest first. A wrong message to the video engine can hang the GPU.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
import tempfile
from pathlib import Path

from common import ROOT, run, streams

TOOLS = {"av1": ROOT / "vtav1" / "vtav1test", "vp9": ROOT / "vtvp9" / "vtvp9test"}


def pyav_reference(path, codec="vp9"):
    """Per-frame MD5 hex digests from a CPU decode with PyAV, or None when PyAV is absent or cannot decode.

    libvpx hands back only the last shown frame of a packet (a VP9 superframe of a spatial-SVC stream holds one
    frame per layer, and it is the top one that is output); FFmpeg returns every shown frame. The hardware
    decoders follow libvpx, so for VP9 this keeps the last frame of each packet.
    """
    try:
        import av  # type: ignore
    except ImportError:
        return None

    def digest(fr):
        ten = "10" in fr.format.name
        f = fr.reformat(format="yuv420p10le" if ten else "yuv420p")
        h = hashlib.md5()
        for pl in f.planes:
            row = pl.width * (2 if ten else 1)
            buf = bytes(pl)
            for y in range(pl.height):
                h.update(buf[y * pl.line_size: y * pl.line_size + row])
        return h.hexdigest()

    out = []
    try:
        with av.open(str(path)) as c:
            st = c.streams.video[0]
            for pkt in c.demux(st):
                got = list(pkt.decode())
                if codec == "vp9" and got:
                    got = got[-1:]
                out += [digest(fr) for fr in got]
    except Exception as e:  # a stream PyAV cannot decode has no reference
        print(f"    (PyAV could not decode {path.name}: {e})")
        return None
    return out


def one(codec, path, args):
    tool = TOOLS[codec]
    md5 = path.with_suffix(path.suffix + ".md5")
    ref = None
    src = "none"
    if md5.is_file():
        ref, src = md5, "md5 file"
    else:
        lines = pyav_reference(path, codec)
        if lines:
            # vtvp9test reads FATE lines ("stream, pts, dts, duration, size, md5": the text after the last
            # comma); vtav1test reads the MD5 as the first word of a line.
            body = "\n".join((f"0, {i}, {i}, 1, 0, {h}" if codec == "vp9" else h) for i, h in enumerate(lines)) + "\n"
            tmp = Path(args.tmpdir) / (path.name + ".md5")
            tmp.write_text(body)
            ref, src = tmp, "PyAV CPU decode"
            if args.write_md5:
                md5.write_text(body)
    cmd = [tool]
    if args.built:
        bundle = ROOT / ("vtav1/build/AV1VCNDecoder.bundle" if codec == "av1" else "vtvp9/build/VP9VCNDecoder.bundle")
        cmd += ["-load", bundle]
    cmd += [path] + ([ref] if ref else [])
    if args.built:  # the loaded copy has its own id next to the installed one
        cmd += ["-id", "com.droga.videodecoder.%s.vcn.%s" % (codec, "av01" if codec == "av1" else "vp09")]
    rc, so, se = run(cmd, timeout=args.timeout)
    out = so + se
    m = re.search(r"(\d+) frames.*?(\d+) mismatches.*?(\d+) errors", out)
    frames, mism, errs = (int(m.group(i)) for i in (1, 2, 3)) if m else (0, 0, 0)
    if "UsingHardwareAcceleratedVideoDecoder = true" not in out and "hardware" not in out.lower() and rc == 0 and not m:
        return "FAIL", f"no result line; rc={rc}", src
    if rc == 124:
        return "FAIL", "timed out (a stuck GPU?)", src
    wrong = len(re.findall(r"MISMATCH expected", out))
    if errs and not wrong and frames == 0:
        return "REFUSED", f"{errs} samples refused, no wrong picture shown", src
    if rc != 0 or errs or mism:
        tail = "\n".join(out.strip().splitlines()[-6:])
        return "FAIL", f"rc={rc} frames={frames} wrong={wrong} errors={errs}\n{tail}", src
    if ref is None:
        return "decoded", f"{frames} frames, no reference", src
    return "BIT-EXACT", f"{frames} frames vs {src}", src


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--codec", choices=["av1", "vp9"])
    ap.add_argument("--extra", action="append", default=[], metavar="DIR")
    ap.add_argument("--built", action="store_true")
    ap.add_argument("--write-md5", action="store_true")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--timeout", type=int, default=180)
    args = ap.parse_args()
    for c, t in TOOLS.items():
        if (not args.codec or args.codec == c) and not t.exists():
            sys.exit(f"{t} is missing: run {t.parent.name}/build.sh (vtav1 and vtvp9 builds make it)")
    rc = 0
    unref = 0
    refused = []
    with tempfile.TemporaryDirectory() as td:
        args.tmpdir = td
        for codec in ("av1", "vp9"):
            if args.codec and args.codec != codec:
                continue
            files = list(streams(codec))
            for d in args.extra:
                files += sorted(p for p in Path(d).rglob("*") if p.suffix in (".ivf", ".webm") and codec in p.name.lower())
            files.sort(key=lambda p: p.stat().st_size)
            print(f"== {codec.upper()}: {len(files)} streams")
            for p in files:
                state, detail, src = one(codec, p, args)
                print(f"  {state:10} {p.name}  {detail.splitlines()[0]}")
                for extra in detail.splitlines()[1:]:
                    print(f"             {extra}")
                if state == "FAIL":
                    rc = 2
                if state == "decoded":
                    unref += 1
                if state == "REFUSED":
                    refused.append(p.name)
    if refused:
        print("\nrefused by the decoder, needs a software decoder: " + ", ".join(refused))
    if unref:
        print(f"\n{unref} stream(s) decoded without a reference: that shows they decode, not that they are bit-exact. "
              f"Install PyAV (python3 -m pip install av) or put <stream>.md5 files next to them.")
        if args.strict and rc == 0:
            rc = 3
    return rc


if __name__ == "__main__":
    sys.exit(main())
