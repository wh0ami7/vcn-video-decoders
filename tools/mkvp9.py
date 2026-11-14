# Make a synthetic VP9 IVF clip with libvpx and a FATE-style per-frame I420 MD5 reference decoded on the CPU.
#   .re/venv/bin/python tools/mkvp9.py <out.ivf> <w> <h> <frames>
# Writes <out.ivf> and <out.ivf.md5>. Use it with vtvp9test, e.g. for portrait sizes.
import hashlib
import sys

import av
import numpy as np

out, W, H, N = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])


def frame(i):
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    tex = np.random.default_rng(1234).integers(0, 255, (256, 256)).astype(np.float32)
    t = np.tile(np.roll(np.roll(tex, i * 7, 0), i * 13, 1), (H // 256 + 1, W // 256 + 1))[:H, :W]
    y = 0.35 * (xx / W * 255) + 0.35 * t + 0.3 * (128 + 127 * np.sin((xx + i * 24) / 90) * np.cos((yy - i * 11) / 70))
    u = 128 + 60 * np.sin((xx[::2, ::2] + i * 5) / 200)
    v = 128 + 60 * np.cos((yy[::2, ::2] - i * 4) / 150)
    return y, u, v


oc = av.open(out, "w", format="ivf")
st = oc.add_stream("libvpx-vp9", rate=30)
st.width, st.height, st.pix_fmt = W, H, "yuv420p"
st.options = {"crf": "32", "b": "0", "cpu-used": "6", "row-mt": "1"}
for i in range(N):
    y, u, v = frame(i)
    f = av.VideoFrame(W, H, "yuv420p")
    for plane, data in zip(f.planes, (y, u, v)):
        a = np.clip(data, 0, 255).astype(np.uint8)
        buf = np.frombuffer(plane, np.uint8).reshape(a.shape[0], plane.line_size)
        buf[:, : a.shape[1]] = a
    f.pts = i
    for pkt in st.encode(f):
        oc.mux(pkt)
for pkt in st.encode(None):
    oc.mux(pkt)
oc.close()

n = 0
with open(out + ".md5", "w") as ref:
    for fr in av.open(out).decode(video=0):
        m = hashlib.md5()
        for plane, (pw, ph) in zip(fr.planes, ((W, H), ((W + 1) // 2, (H + 1) // 2), ((W + 1) // 2, (H + 1) // 2))):
            a = np.frombuffer(plane, np.uint8).reshape(-1, plane.line_size)[:ph, :pw]
            m.update(a.tobytes())
        ref.write("0, %10d, %10d,        1, %8d, %s\n" % (n, n, W * H * 3 // 2, m.hexdigest()))
        n += 1
print(out, "frames md5:", n)
