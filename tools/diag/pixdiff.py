#!/usr/bin/env python3
"""Where do the GPU's pixels differ from a CPU decode? (needs PyAV and numpy)

  pixdiff.py stream.ivf gpu.yuv [--frames N]

gpu.yuv is the planar I420 that `VCNVP9_DUMP_YUV=gpu.yuv VCNVP9_SESSION=vp9 VCNVP9_ALLOW_GPU=yes-i-know-it-can-hang
vcnvp9/decode_vp9 stream.ivf` appends for every shown frame, in output order (a VP9 superframe of a spatial-SVC
stream shows one frame per layer). The CPU frames come from PyAV in the same order. For each frame that differs the
script prints the size, how many luma samples differ, the largest difference and the 64x64 blocks that differ.
"""
import argparse
import sys

import av
import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stream")
    ap.add_argument("yuv")
    ap.add_argument("--frames", type=int, default=0)
    a = ap.parse_args()
    data = np.fromfile(a.yuv, dtype=np.uint8)
    pos = 0
    n = 0
    bad = 0
    c = av.open(a.stream)
    for pkt in c.demux(c.streams.video[0]):
        for fr in pkt.decode():
            w, h = fr.width, fr.height
            cw, ch = (w + 1) // 2, (h + 1) // 2
            need = w * h + 2 * cw * ch
            if pos + need > len(data):
                print(f"frame {n}: the GPU file ends")
                return 2
            g = fr.reformat(format="yuv420p")
            ref = [np.frombuffer(bytes(pl), np.uint8).reshape(pl.height, pl.line_size)[:, :pl.width] for pl in g.planes]
            gpu = [data[pos:pos + w * h].reshape(h, w),
                   data[pos + w * h:pos + w * h + cw * ch].reshape(ch, cw),
                   data[pos + w * h + cw * ch:pos + need].reshape(ch, cw)]
            pos += need
            n += 1
            diffs = [np.abs(r.astype(int) - q.astype(int)) for r, q in zip(ref, gpu)]
            if any(d.any() for d in diffs):
                bad += 1
                y = diffs[0]
                blocks = sorted({(int(r) // 64, int(cc) // 64) for r, cc in zip(*np.nonzero(y))})
                print(f"frame {n:3} {w}x{h}: Y {int((y > 0).sum())} samples differ (max {int(y.max())}), "
                      f"U {int((diffs[1] > 0).sum())}, V {int((diffs[2] > 0).sum())}; "
                      f"{len(blocks)} of {((h + 63) // 64) * ((w + 63) // 64)} 64x64 blocks: "
                      + " ".join(f"({r},{cc})" for r, cc in blocks[:12]) + (" ..." if len(blocks) > 12 else ""))
            else:
                print(f"frame {n:3} {w}x{h}: identical")
            if a.frames and n >= a.frames:
                break
        if a.frames and n >= a.frames:
            break
    return 2 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
