#!/usr/bin/env python3
"""Which AV1 and VP9 coding tools do the test streams exercise?

Runs tools/diag/featdump (the decoders' own header parsers) over every stream and counts, per coding tool,
the frames and streams that use it. A tool no stream uses is NOT EXERCISED: the parser may read it right, but
nothing here shows it. Add a stream that uses it (tests/media/<codec>/ or --extra DIR) and run again.

  features.py [--codec av1|vp9] [--extra DIR ...] [-v] [--require-all]
  exit 0; 3 with --require-all when some tool is not exercised; 2 when a stream does not parse
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from common import streams, frames

# (name, what it is, predicate over one frame dict). `ok` kind frames only unless the name says otherwise.
AV1 = [
    ("profile 0 (8/10-bit 4:2:0)", "main profile", lambda f: f["profile"] == 0),
    ("profile 1 (4:4:4)", "high profile", lambda f: f["profile"] == 1),
    ("profile 2 (4:2:2 / 12-bit)", "professional profile", lambda f: f["profile"] == 2),
    ("8-bit", "", lambda f: f["bit_depth"] == 8),
    ("10-bit", "", lambda f: f["bit_depth"] == 10),
    ("12-bit", "", lambda f: f["bit_depth"] == 12),
    ("monochrome", "", lambda f: f["mono"] == 1),
    ("still picture / reduced header", "", lambda f: f["still"] or f["reduced_still"]),
    ("128x128 superblocks", "", lambda f: f["sb128"] == 1),
    ("several operating points", "scalability", lambda f: f["op_points"] > 1),
    ("frame ids", "frame_id_numbers_present", lambda f: f["frame_ids"] == 1),
    ("timing / decoder model info", "", lambda f: f["timing_info"] or f["decoder_model"]),
    ("key frame", "", lambda f: f["kind"] == "frame" and f["frame_type"] == 0),
    ("inter frame", "", lambda f: f["kind"] == "frame" and f["frame_type"] == 1),
    ("intra-only frame", "", lambda f: f["kind"] == "frame" and f["frame_type"] == 2),
    ("switch frame", "", lambda f: f["kind"] == "frame" and f["frame_type"] == 3),
    ("hidden frame (show_frame 0)", "", lambda f: f["kind"] == "frame" and f["show_frame"] == 0),
    ("show_existing_frame", "", lambda f: f["kind"] == "show_existing"),
    ("show_existing of a key frame", "refresh of all slots", lambda f: f["kind"] == "show_existing" and f["shown_key"]),
    ("error resilient mode", "", lambda f: f.get("error_resilient") == 1),
    ("disable_cdf_update", "", lambda f: f.get("cdf_update_off") == 1),
    ("disable_frame_end_update_cdf", "", lambda f: f.get("end_update_cdf_off") == 1),
    ("primary_ref_frame none on an inter frame", "", lambda f: f.get("primary_none") == 1 and f.get("frame_type") == 1),
    ("screen content tools", "", lambda f: f.get("screen_content") == 1),
    ("intra block copy", "", lambda f: f.get("intrabc") == 1),
    ("force_integer_mv", "", lambda f: f.get("integer_mv") == 1),
    ("frame_size_override", "", lambda f: f.get("frame_size_override") == 1),
    ("frame_refs_short_signaling", "", lambda f: f.get("short_signaling") == 1),
    ("superres", "", lambda f: f.get("superres") == 1),
    ("render size differs from frame size", "", lambda f: f.get("render_differs") == 1),
    ("several tiles", "", lambda f: f.get("tile_cols", 1) * f.get("tile_rows", 1) > 1),
    ("non-uniform tile spacing", "", lambda f: f.get("tile_uniform") == 0 and f.get("tile_cols", 1) * f.get("tile_rows", 1) > 1),
    ("quantizer matrices", "", lambda f: f.get("qmatrix") == 1),
    ("separate U/V delta q", "", lambda f: f.get("diff_uv_delta") == 1),
    ("delta_q", "", lambda f: f.get("delta_q") == 1),
    ("delta_lf", "", lambda f: f.get("delta_lf") == 1),
    ("delta_lf_multi", "", lambda f: f.get("delta_lf_multi") == 1),
    ("segmentation", "", lambda f: f.get("seg") == 1),
    ("segmentation temporal update", "", lambda f: f.get("seg_temporal") == 1),
    ("lossless", "", lambda f: f.get("coded_lossless") == 1),
    ("loop filter with sharpness", "", lambda f: f.get("lf_on") == 1 and f.get("lf_sharp", 0) > 0),
    ("loop filter deltas", "", lambda f: f.get("lf_delta") == 1),
    ("CDEF", "", lambda f: f.get("cdef_bits", 0) > 0 or f.get("seq_cdef") == 1),
    ("loop restoration (any plane)", "", lambda f: any(f.get(k, 0) for k in ("lr_y", "lr_u", "lr_v"))),
    ("TX mode select", "", lambda f: f.get("tx_mode") == 2),
    ("reference_select (compound)", "", lambda f: f.get("ref_select") == 1),
    ("skip mode", "", lambda f: f.get("skip_mode") == 1),
    ("warped motion", "", lambda f: f.get("warped") == 1),
    ("reduced tx set", "", lambda f: f.get("reduced_tx") == 1),
    ("global motion: translation", "", lambda f: f.get("gm_types", 0) & 2),
    ("global motion: rotzoom", "", lambda f: f.get("gm_types", 0) & 4),
    ("global motion: affine", "", lambda f: f.get("gm_types", 0) & 8),
    ("high precision mv", "", lambda f: f.get("hp_mv") == 1),
    ("switchable interpolation filter", "", lambda f: f.get("interp") == 4),
    ("motion mode switchable", "", lambda f: f.get("motion_switchable") == 1),
    ("use_ref_frame_mvs", "", lambda f: f.get("ref_mvs") == 1),
    ("film grain applied", "", lambda f: f.get("grain") == 1),
    ("film grain update from a reference", "", lambda f: f.get("grain") == 1 and f.get("grain_update") == 0),
    ("film grain chroma from luma", "", lambda f: f.get("grain_chroma_from_luma") == 1),
    ("film grain overlap", "", lambda f: f.get("grain_overlap") == 1),
    ("film grain ar_coeff_lag 3", "", lambda f: f.get("grain_ar_lag") == 3),
]
VP9 = [
    ("profile 0 (8-bit 4:2:0)", "", lambda f: f["profile"] == 0),
    ("profile 1 (4:4:4 / 4:2:2 / 4:4:0)", "", lambda f: f["profile"] == 1),
    ("profile 2 (10/12-bit 4:2:0)", "", lambda f: f["profile"] == 2),
    ("profile 3 (10/12-bit 4:4:4)", "", lambda f: f["profile"] == 3),
    ("10-bit", "", lambda f: f.get("bit_depth") == 10),
    ("superframe", "", lambda f: f["superframe"] == 1),
    ("key frame", "", lambda f: f["kind"] == "frame" and f["frame_type"] == 0),
    ("inter frame", "", lambda f: f["kind"] == "frame" and f["frame_type"] == 1 and not f["intra_only"]),
    ("intra-only frame", "", lambda f: f.get("intra_only") == 1),
    ("hidden frame (show_frame 0)", "", lambda f: f["kind"] == "frame" and f["show_frame"] == 0),
    ("show_existing_frame", "", lambda f: f["kind"] == "show_existing"),
    ("error resilient", "", lambda f: f.get("error_res") == 1),
    ("reset_frame_context", "", lambda f: f.get("reset_ctx", 0) > 0),
    ("refresh_frame_context", "", lambda f: f.get("refresh_ctx") == 1),
    ("frame_parallel off", "", lambda f: f["kind"] == "frame" and f.get("frame_parallel") == 0),
    ("frame_context_idx > 0", "", lambda f: f.get("ctx_idx", 0) > 0),
    ("scaled reference frame", "", lambda f: f.get("scaled_ref") == 1),
    ("sign_bias (compound prediction)", "", lambda f: f.get("sign_bias") == 1),
    ("high precision mv", "", lambda f: f.get("hp_mv") == 1),
    ("switchable interpolation filter", "", lambda f: f.get("interp") == 4),
    ("loop filter sharpness", "", lambda f: f.get("lf_sharp", 0) > 0),
    ("loop filter deltas", "", lambda f: f.get("lf_delta") == 1),
    ("loop filter delta update", "", lambda f: f.get("lf_delta_update") == 1),
    ("lossless", "", lambda f: f.get("lossless") == 1),
    ("delta q", "", lambda f: f.get("delta_q") == 1),
    ("segmentation", "", lambda f: f.get("seg") == 1),
    ("segmentation update map", "", lambda f: f.get("seg_update_map") == 1),
    ("segmentation temporal update", "", lambda f: f.get("seg_temporal") == 1),
    ("segmentation update data", "", lambda f: f.get("seg_update_data") == 1),
    ("segmentation absolute values", "", lambda f: f.get("seg") == 1 and f.get("seg_abs") == 1),
    ("several tile columns", "", lambda f: f.get("tile_cols_log2", 0) > 0),
    ("several tile rows", "", lambda f: f.get("tile_rows_log2", 0) > 0),
    ("use_prev_frame_mvs", "", lambda f: f.get("prev_mvs") == 1),
    ("odd frame size", "", lambda f: f.get("width", 2) % 8 != 0 or f.get("height", 2) % 8 != 0),
    ("not supported by the VCN (refused)", "expected for profiles 1 and 3, 12-bit, non-4:2:0",
     lambda f: f["kind"] == "frame" and f.get("supported") == 0),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--codec", choices=["av1", "vp9"])
    ap.add_argument("--extra", action="append", default=[], metavar="DIR", help="more streams (any .ivf/.webm below DIR)")
    ap.add_argument("-v", action="store_true", help="name the streams that use each tool")
    ap.add_argument("--require-all", action="store_true")
    a = ap.parse_args()
    status = 0
    missing_total = 0
    for codec, table in (("av1", AV1), ("vp9", VP9)):
        if a.codec and a.codec != codec:
            continue
        files = list(streams(codec))
        for d in a.extra:
            files += sorted(p for p in Path(d).rglob("*") if p.suffix in (".ivf", ".webm") and codec in p.name.lower())
        if not files:
            print(f"{codec}: no streams (tests/fetch_media.sh downloads them)")
            continue
        per = {}
        total = 0
        for p in files:
            fr, err = frames(codec, p)
            if err:
                print(f"{codec}: {p.name}: {err}")
                status = 2
            per[p.name] = fr
            total += len(fr)
        print(f"\n== {codec.upper()}: {len(files)} streams, {total} frames")
        missing = []
        for name, note, pred in table:
            used = {}
            for sname, fr in per.items():
                n = 0
                for f in fr:
                    try:
                        if pred(f):
                            n += 1
                    except KeyError:
                        pass
                if n:
                    used[sname] = n
            if used:
                line = f"  exercised  {name:46} {sum(used.values()):5} frames in {len(used)} stream(s)"
                if a.v:
                    line += "  " + ", ".join(sorted(used))
                print(line)
            else:
                missing.append(name)
                print(f"  NOT EXERCISED  {name}" + (f"   ({note})" if note else ""))
        missing_total += len(missing)
        print(f"  {len(table) - len(missing)} of {len(table)} coding tools exercised")
    if a.require_all and missing_total and status == 0:
        status = 3
    return status


if __name__ == "__main__":
    sys.exit(main())
