#!/usr/bin/env python3
"""Tests of the diagnostics themselves, and the integration test of the decoders.

  selftest.py            unit tests, no GPU: the field check, the feature dump and the reference rules
  selftest.py --gpu      also decode every stream on the GPU with the freshly built plug-ins (vtav1/build,
                         vtvp9/build) and require, per stream, the result recorded in EXPECT below
Exit status 0 = all passed, 1 = a test failed, 3 = a GPU result could not be proven (no PyAV).

Run it with a python that has PyAV for the GPU part (`python3 -m venv v; v/bin/pip install av`).
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import common  # noqa: E402
import decode  # noqa: E402
import features  # noqa: E402
import fields  # noqa: E402

# What every test stream must give with the built plug-ins. BIT-EXACT = every frame equals a CPU decode.
# REFUSED = errors and no wrong picture: the video engine cannot do it bit-exactly (README, decoders 0.3.6).
EXPECT = {
    "vp90-2-22-svc_1280x720_3.ivf": "REFUSED",
}
DEFAULT_EXPECT = "BIT-EXACT"


class FieldsTest(unittest.TestCase):
    def run_fields(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), mock.patch.object(sys, "argv", ["fields.py"]):
            rc = fields.main()
        return rc, out.getvalue()

    def test_all_fields_are_mapped(self):
        rc, out = self.run_fields()
        self.assertEqual(rc, 0, out[-600:])
        self.assertIn("0 problem(s)", out)

    def test_a_member_missing_from_our_parser_is_found(self):
        orig = fields.struct_members_c

        def without(text, name):
            m = orig(text, name)
            return [x for x in m if x != "SuperresDenom"] if name == "AV1Frame" and m else m

        with mock.patch.object(fields, "struct_members_c", without):
            rc, out = self.run_fields()
        self.assertEqual(rc, 3)
        self.assertIn("MISSING", out)
        self.assertIn("SuperresDenom", out)

    def test_a_field_missing_from_the_table_is_found(self):
        saved = dict(fields.MAP["v4l2_av1_cdef"])
        try:
            del fields.MAP["v4l2_av1_cdef"]["bits"]
            rc, out = self.run_fields()
        finally:
            fields.MAP["v4l2_av1_cdef"] = saved
        self.assertEqual(rc, 3)
        self.assertIn("UNMAPPED  bits", out)

    def test_the_header_parser_reads_every_struct(self):
        text = (common.DEFAULT_LINUX / "include/uapi/linux/v4l2-controls.h").read_text(errors="replace")
        self.assertEqual(len(common.struct_members_c(text, "v4l2_av1_cdef")), 6)
        self.assertIn("quant", common.struct_members_c(text, "v4l2_ctrl_vp9_frame"))


class FeatureDumpTest(unittest.TestCase):
    def test_the_parsers_show_as_many_frames_as_a_cpu_decoder(self):
        # An independent check of the header parsers: the number of pictures a stream shows (show_frame or
        # show_existing_frame) must equal the number FFmpeg's libdav1d / libvpx output.
        try:
            import av
        except ImportError:
            self.skipTest("PyAV is not installed")
        for codec in ("av1", "vp9"):
            for p in common.streams(codec):
                fr, err = common.frames(codec, p)
                self.assertEqual(err, "", p.name)
                ours = sum(1 for f in fr if f["kind"] == "show_existing" or f.get("show_frame") == 1)
                with av.open(str(p)) as c:
                    cpu = sum(1 for pkt in c.demux(c.streams.video[0]) for _ in pkt.decode())
                self.assertEqual(ours, cpu, f"{p.name}: pictures shown")

    def test_a_damaged_stream_is_reported(self):
        rc, out, err = common.run([common.build_featdump(), "vp9", "/nonexistent.ivf"])
        self.assertEqual(rc, 2)
        self.assertEqual(out, "")
        self.assertIn("nonexistent", err)


class FeaturePredicateTest(unittest.TestCase):
    def test_predicates_do_not_trip_on_a_frame_without_the_key(self):
        # a predicate reads a key an `ok` kind of frame lacks (show_existing): features.py skips it
        for _, _, pred in features.AV1 + features.VP9:
            with contextlib.suppress(KeyError):
                pred({"kind": "show_existing", "profile": 0, "shown_key": 0})

    def test_the_svc_stream_uses_the_tools_we_refuse(self):
        p = common.MEDIA / "vp9/vp90-2-22-svc_1280x720_3.ivf"
        if not p.is_file():
            self.skipTest("tests/media is not fetched")
        fr, _ = common.frames("vp9", p)
        scaled = [f for f in fr if f.get("scaled_ref") == 1]
        self.assertTrue(scaled)
        small = [f for f in scaled if any(2 * f[f"ref{i}_w"] < f["width"] for i in range(3) if f[f"ref{i}_w"])]
        self.assertTrue(small, "no frame predicts from a reference more than 2x smaller")


class ReferenceRuleTest(unittest.TestCase):
    def test_vp9_reference_keeps_the_last_frame_of_a_packet(self):
        try:
            import av  # noqa: F401
        except ImportError:
            self.skipTest("PyAV is not installed")
        p = common.MEDIA / "vp9/vp90-2-22-svc_1280x720_3.ivf"
        if not p.is_file():
            self.skipTest("tests/media is not fetched")
        ref = decode.pyav_reference(p, "vp9")
        self.assertEqual(len(ref), 20)  # one frame per 3-layer superframe, as libvpx outputs
        # libvpx's own list for this stream (vp90-2-22-svc_1280x720_3.ivf.md5), first frame
        self.assertEqual(ref[0], "369f3d6ce1ba7ad7bd5716d0aef8daf4")


class GpuIntegrationTest(unittest.TestCase):
    """Every test stream on the GPU with the freshly built plug-ins."""

    def test_streams(self):
        try:
            import av  # noqa: F401
            have_ref = True
        except ImportError:
            have_ref = False
        args = SimpleNamespace(built=True, write_md5=False, timeout=180)
        problems = []
        with __import__("tempfile").TemporaryDirectory() as td:
            args.tmpdir = td
            for codec in ("av1", "vp9"):
                for p in common.streams(codec):
                    state, detail, _ = decode.one(codec, p, args)
                    want = EXPECT.get(p.name, DEFAULT_EXPECT)
                    print(f"  {state:10} {p.name} (want {want}) {detail.splitlines()[0]}")
                    if not have_ref and state == "decoded":
                        continue  # cannot be proven without a reference
                    if state != want:
                        problems.append(f"{p.name}: {state}, wanted {want}: {detail}")
        self.assertEqual(problems, [])
        if not have_ref:
            self.skipTest("PyAV is not installed: streams decoded but were not compared with a reference")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gpu", action="store_true")
    a = ap.parse_args()
    suite = unittest.TestSuite()
    load = unittest.defaultTestLoader.loadTestsFromTestCase
    for t in (FieldsTest, FeatureDumpTest, FeaturePredicateTest, ReferenceRuleTest):
        suite.addTests(load(t))
    if a.gpu:
        suite.addTests(load(GpuIntegrationTest))
    res = unittest.TextTestRunner(verbosity=2).run(suite)
    if not res.wasSuccessful():
        return 1
    return 3 if res.skipped and a.gpu and any("PyAV" in r for _, r in res.skipped) else 0


if __name__ == "__main__":
    sys.exit(main())
