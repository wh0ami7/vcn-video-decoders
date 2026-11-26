#!/usr/bin/env python3
"""Static check: every field a stateless hardware AV1 / VP9 decoder needs has a home in our parsers.

The reference is the Linux V4L2 stateless-decoder uAPI (include/uapi/linux/v4l2-controls.h), which lists, per
codec, each header value a hardware decoder is given. For every field this script says one of:

  parsed   the parser keeps it, in the named member (the member must exist in av1parse.h / vp9parse.h)
  derived  the parser computes it while reading, to decide what to read, and does not keep it
  fw       the VCN firmware reads or derives it from the bitstream itself (no host-side value needed)
  n/a      not a decode input (timestamps, padding, buffer handles)

A field missing from the table is UNMAPPED and fails the run, so a new V4L2 field cannot slip by unseen.

  fields.py [--linux DIR] [--list]        exit 0 all mapped, 3 something unmapped or a named member missing
"""
from __future__ import annotations

import argparse
import re
import sys

from common import DEFAULT_LINUX, ROOT, strip_c, struct_members_c

P, D, F, N = "parsed", "derived", "fw", "n/a"

# V4L2 struct -> {field: (class, detail)}; detail for `parsed` is "Struct.member" in our headers.
MAP = {
    "v4l2_ctrl_av1_sequence": {
        "flags": (P, "AV1Seq.still_picture,reduced_still_picture_header,use_128x128_superblock,enable_filter_intra,"
                   "enable_intra_edge_filter,enable_interintra_compound,enable_masked_compound,enable_warped_motion,"
                   "enable_dual_filter,enable_order_hint,enable_jnt_comp,enable_ref_frame_mvs,enable_superres,enable_cdef,"
                   "enable_restoration,mono_chrome,color_range,subsampling_x,subsampling_y,film_grain_params_present,"
                   "separate_uv_delta_q"),
        "seq_profile": (P, "AV1Seq.seq_profile"), "order_hint_bits": (P, "AV1Seq.OrderHintBits"),
        "bit_depth": (P, "AV1Seq.BitDepth"),
        "max_frame_width_minus_1": (P, "AV1Seq.max_frame_width_minus_1"),
        "max_frame_height_minus_1": (P, "AV1Seq.max_frame_height_minus_1"),
        "reserved": (N, "padding"),
    },
    "v4l2_ctrl_av1_frame": {
        "tile_info": (P, "AV1Frame.TileCols"), "quantization": (P, "AV1Frame.base_q_idx"),
        "superres_denom": (P, "AV1Frame.SuperresDenom"), "segmentation": (P, "AV1Frame.segmentation_enabled"),
        "loop_filter": (P, "AV1Frame.loop_filter_level"), "cdef": (P, "AV1Frame.cdef_bits"),
        "skip_mode_frame": (D, "SkipModeFrame[] is derived in skip_mode_params only to know if skip_mode_present is coded"),
        "primary_ref_frame": (P, "AV1Frame.primary_ref_frame"),
        "loop_restoration": (P, "AV1Frame.FrameRestorationType"), "global_motion": (P, "AV1Frame.gm_params"),
        "flags": (P, "AV1Frame.show_frame,showable_frame,error_resilient_mode,disable_cdf_update,allow_screen_content_tools,"
                  "force_integer_mv,allow_intrabc,use_superres,allow_high_precision_mv,is_motion_mode_switchable,"
                  "use_ref_frame_mvs,disable_frame_end_update_cdf,allow_warped_motion,reference_select,reduced_tx_set,"
                  "skip_mode_present,frame_size_override_flag,frame_refs_short_signaling"), "frame_type": (P, "AV1Frame.frame_type"),
        "order_hint": (P, "AV1Frame.order_hint"), "upscaled_width": (P, "AV1Frame.UpscaledWidth"),
        "interpolation_filter": (P, "AV1Frame.interpolation_filter"), "tx_mode": (P, "AV1Frame.TxMode"),
        "frame_width_minus_1": (P, "AV1Frame.frame_width_minus_1"),
        "frame_height_minus_1": (P, "AV1Frame.frame_height_minus_1"),
        "render_width_minus_1": (P, "AV1Frame.RenderWidth"), "render_height_minus_1": (P, "AV1Frame.RenderHeight"),
        "current_frame_id": (P, "AV1Frame.current_frame_id"),
        "buffer_removal_time": (D, "read and skipped: timing only, no decode effect"),
        "reserved": (N, "padding"), "order_hints": (P, "AV1Frame.OrderHints"),
        "reference_frame_ts": (N, "V4L2 buffer timestamps: our DPB slot is AV1RefSlot.dpb_slot"),
        "ref_frame_idx": (P, "AV1Frame.ref_frame_idx"), "refresh_frame_flags": (P, "AV1Frame.refresh_frame_flags"),
    },
    "v4l2_av1_tile_info": {
        "flags": (P, "AV1Frame.uniform_tile_spacing_flag"), "context_update_tile_id": (P, "AV1Frame.context_update_tile_id"),
        "tile_cols": (P, "AV1Frame.TileCols"), "tile_rows": (P, "AV1Frame.TileRows"),
        "mi_col_starts": (P, "AV1Frame.MiColStarts"), "mi_row_starts": (P, "AV1Frame.MiRowStarts"),
        "width_in_sbs_minus_1": (D, "from MiColStarts and the superblock size"),
        "height_in_sbs_minus_1": (D, "from MiRowStarts and the superblock size"),
        "tile_size_bytes": (P, "AV1Frame.TileSizeBytes"), "reserved": (N, "padding"),
    },
    "v4l2_av1_quantization": {
        "flags": (P, "AV1Frame.diff_uv_delta,using_qmatrix,delta_q_present"), "base_q_idx": (P, "AV1Frame.base_q_idx"),
        "delta_q_y_dc": (P, "AV1Frame.DeltaQYDc"), "delta_q_u_dc": (P, "AV1Frame.DeltaQUDc"),
        "delta_q_u_ac": (P, "AV1Frame.DeltaQUAc"), "delta_q_v_dc": (P, "AV1Frame.DeltaQVDc"),
        "delta_q_v_ac": (P, "AV1Frame.DeltaQVAc"), "qm_y": (P, "AV1Frame.qm_y"), "qm_u": (P, "AV1Frame.qm_u"),
        "qm_v": (P, "AV1Frame.qm_v"), "delta_q_res": (P, "AV1Frame.delta_q_res"),
    },
    "v4l2_av1_segmentation": {
        "flags": (P, "AV1Frame.segmentation_enabled,segmentation_update_map,segmentation_temporal_update,segmentation_update_data"), "last_active_seg_id": (P, "AV1Frame.LastActiveSegId"),
        "feature_enabled": (P, "AV1Frame.FeatureEnabled"), "feature_data": (P, "AV1Frame.FeatureData"),
    },
    "v4l2_av1_loop_filter": {
        "flags": (P, "AV1Frame.loop_filter_delta_enabled,loop_filter_delta_update,delta_lf_present,delta_lf_multi"), "level": (P, "AV1Frame.loop_filter_level"),
        "sharpness": (P, "AV1Frame.loop_filter_sharpness"), "ref_deltas": (P, "AV1Frame.loop_filter_ref_deltas"),
        "mode_deltas": (P, "AV1Frame.loop_filter_mode_deltas"), "delta_lf_res": (P, "AV1Frame.delta_lf_res"),
    },
    "v4l2_av1_cdef": {
        "damping_minus_3": (P, "AV1Frame.CdefDamping"), "bits": (P, "AV1Frame.cdef_bits"),
        "y_pri_strength": (P, "AV1Frame.cdef_y_pri"), "y_sec_strength": (P, "AV1Frame.cdef_y_sec"),
        "uv_pri_strength": (P, "AV1Frame.cdef_uv_pri"), "uv_sec_strength": (P, "AV1Frame.cdef_uv_sec"),
    },
    "v4l2_av1_loop_restoration": {
        "flags": (D, "uses_lr / uses_chroma_lr follow from FrameRestorationType"),
        "lr_unit_shift": (P, "AV1Frame.LoopRestorationSize"), "lr_uv_shift": (D, "from LoopRestorationSize[1] vs [0]"),
        "reserved": (N, "padding"), "frame_restoration_type": (P, "AV1Frame.FrameRestorationType"),
        "loop_restoration_size": (P, "AV1Frame.LoopRestorationSize"),
    },
    "v4l2_av1_global_motion": {
        "flags": (D, "invalid-shear mask: the warp validity check (7.11.3.6) is made by the firmware"),
        "type": (P, "AV1Frame.gm_type"), "params": (P, "AV1Frame.gm_params"),
        "invalid": (F, "shear validity is derived by the firmware from gm_params"),
        "reserved": (N, "padding"),
    },
    "v4l2_ctrl_av1_film_grain": {
        "flags": (P, "AV1FilmGrain.apply_grain,update_grain,chroma_scaling_from_luma,overlap_flag,clip_to_restricted_range"), "cr_mult": (P, "AV1FilmGrain.cr_mult"),
        "grain_seed": (P, "AV1FilmGrain.grain_seed"), "film_grain_params_ref_idx": (P, "AV1FilmGrain.film_grain_params_ref_idx"),
        "num_y_points": (P, "AV1FilmGrain.num_y_points"), "point_y_value": (P, "AV1FilmGrain.point_y_value"),
        "point_y_scaling": (P, "AV1FilmGrain.point_y_scaling"), "num_cb_points": (P, "AV1FilmGrain.num_cb_points"),
        "point_cb_value": (P, "AV1FilmGrain.point_cb_value"), "point_cb_scaling": (P, "AV1FilmGrain.point_cb_scaling"),
        "num_cr_points": (P, "AV1FilmGrain.num_cr_points"), "point_cr_value": (P, "AV1FilmGrain.point_cr_value"),
        "point_cr_scaling": (P, "AV1FilmGrain.point_cr_scaling"),
        "grain_scaling_minus_8": (P, "AV1FilmGrain.grain_scaling_minus_8"), "ar_coeff_lag": (P, "AV1FilmGrain.ar_coeff_lag"),
        "ar_coeffs_y_plus_128": (P, "AV1FilmGrain.ar_coeffs_y_plus_128"),
        "ar_coeffs_cb_plus_128": (P, "AV1FilmGrain.ar_coeffs_cb_plus_128"),
        "ar_coeffs_cr_plus_128": (P, "AV1FilmGrain.ar_coeffs_cr_plus_128"),
        "ar_coeff_shift_minus_6": (P, "AV1FilmGrain.ar_coeff_shift_minus_6"),
        "grain_scale_shift": (P, "AV1FilmGrain.grain_scale_shift"), "cb_mult": (P, "AV1FilmGrain.cb_mult"),
        "cb_luma_mult": (P, "AV1FilmGrain.cb_luma_mult"), "cr_luma_mult": (P, "AV1FilmGrain.cr_luma_mult"),
        "cb_offset": (P, "AV1FilmGrain.cb_offset"), "cr_offset": (P, "AV1FilmGrain.cr_offset"),
        "reserved": (N, "padding"),
    },
    "v4l2_ctrl_vp9_frame": {
        "lf": (P, "VP9Frame.lf_level"), "quant": (P, "VP9Frame.base_q_idx"), "seg": (P, "VP9Frame.seg_enabled"),
        "flags": (P, "VP9Frame.frame_type,show_frame,error_res,intra_only,allow_high_precision_mv,refresh_frame_context,"
                     "frame_parallel"),
        "compressed_header_size": (P, "VP9Frame.compressed_header_size"),
        "uncompressed_header_size": (P, "VP9Frame.uncompressed_header_size"),
        "frame_width_minus_1": (P, "VP9Frame.width"), "frame_height_minus_1": (P, "VP9Frame.height"),
        "render_width_minus_1": (D, "render_size is read and skipped: it does not change the decoded picture"),
        "render_height_minus_1": (D, "render_size is read and skipped: it does not change the decoded picture"),
        "last_frame_ts": (N, "V4L2 buffer timestamps: our DPB is VP9Ref.dpb_slot"),
        "golden_frame_ts": (N, "V4L2 buffer timestamps"), "alt_frame_ts": (N, "V4L2 buffer timestamps"),
        "ref_frame_sign_bias": (P, "VP9Frame.sign_bias"), "reset_frame_context": (P, "VP9Frame.reset_frame_context"),
        "frame_context_idx": (P, "VP9Frame.frame_context_idx"), "profile": (P, "VP9Frame.profile"),
        "bit_depth": (P, "VP9Frame.bit_depth"), "interpolation_filter": (P, "VP9Frame.interp_filter"),
        "tile_cols_log2": (P, "VP9Frame.log2_tile_cols"), "tile_rows_log2": (P, "VP9Frame.log2_tile_rows"),
        "reference_mode": (F, "coded in the compressed header, which the firmware reads"),
        "reserved": (N, "padding"),
    },
    "v4l2_vp9_loop_filter": {
        "ref_deltas": (P, "VP9Parser.lf_ref_deltas"), "mode_deltas": (P, "VP9Parser.lf_mode_deltas"),
        "level": (P, "VP9Frame.lf_level"), "sharpness": (P, "VP9Frame.lf_sharpness"),
        "flags": (P, "VP9Frame.lf_delta_enabled,lf_delta_update"), "reserved": (N, "padding"),
    },
    "v4l2_vp9_quantization": {
        "base_q_idx": (P, "VP9Frame.base_q_idx"), "delta_q_y_dc": (P, "VP9Frame.y_dc_delta_q"),
        "delta_q_uv_dc": (P, "VP9Frame.uv_dc_delta_q"), "delta_q_uv_ac": (P, "VP9Frame.uv_ac_delta_q"),
        "reserved": (N, "padding"),
    },
    "v4l2_vp9_segmentation": {
        "feature_data": (P, "VP9Parser.seg_feature_data"), "feature_enabled": (P, "VP9Parser.seg_feature_mask"),
        "tree_probs": (P, "VP9Parser.seg_tree_probs"), "pred_probs": (P, "VP9Parser.seg_pred_probs"),
        "flags": (P, "VP9Frame.seg_enabled,seg_update_map,seg_temporal_update,seg_update_data"), "reserved": (N, "padding"),
    },
    "v4l2_ctrl_vp9_compressed_hdr": {
        "tx_mode": (F, "compressed header: read by the firmware"), "tx8": (F, "compressed header"),
        "tx16": (F, "compressed header"), "tx32": (F, "compressed header"), "coef": (F, "compressed header"),
        "skip": (F, "compressed header"), "inter_mode": (F, "compressed header"),
        "interp_filter": (F, "compressed header"), "is_inter": (F, "compressed header"),
        "comp_mode": (F, "compressed header"), "single_ref": (F, "compressed header"),
        "comp_ref": (F, "compressed header"), "y_mode": (F, "compressed header"), "uv_mode": (F, "compressed header"),
        "partition": (F, "compressed header"), "mv": (F, "compressed header"),
    },
}
# which of our headers holds each C struct a `parsed` entry may name
OURS = {"AV1Seq": "vcnav1/av1parse.h", "AV1Frame": "vcnav1/av1parse.h", "AV1FilmGrain": "vcnav1/av1parse.h",
        "VP9Frame": "vcnvp9/vp9parse.h", "VP9Parser": "vcnvp9/vp9parse.h"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--linux", default=str(DEFAULT_LINUX), help="Linux source tree (default: $LINUX_SRC or the one in common.py)")
    ap.add_argument("--list", action="store_true", help="print every V4L2 field found and exit")
    a = ap.parse_args()
    hdr = (DEFAULT_LINUX.__class__(a.linux) / "include/uapi/linux/v4l2-controls.h")
    if not hdr.is_file():
        sys.exit(f"{hdr} not found (--linux DIR)")
    text = hdr.read_text(errors="replace")
    ours = {}
    for cname, path in OURS.items():
        ours[cname] = set(struct_members_c((ROOT / path).read_text(), cname) or [])
        if not ours[cname]:
            sys.exit(f"struct {cname} not found in {path}")
    bad = 0
    counts = {P: 0, D: 0, F: 0, N: 0}
    for sname, table in MAP.items():
        fields = struct_members_c(text, sname)
        if fields is None:
            print(f"?? {sname}: not in this Linux header (older uAPI?)")
            continue
        print(f"{sname}  ({len(fields)} fields)")
        for f in fields:
            if a.list:
                print(f"    {f}")
                continue
            ent = table.get(f)
            if not ent:
                print(f"  UNMAPPED  {f}")
                bad += 1
                continue
            cls, detail = ent
            counts[cls] += 1
            if cls == P:
                cs, _, members = detail.partition(".")
                gone = [m for m in members.split(",") if m not in ours.get(cs, ())]
                if gone:
                    print(f"  MISSING   {f}: {', '.join(gone)} not in {cs} ({OURS.get(cs)})")
                    bad += 1
                    continue
            print(f"  {cls:8}  {f}  ->  {detail}")
        for f in table:
            if f not in fields:
                print(f"  STALE     {f}: in the table but not in {sname} of this Linux tree")
    if a.list:
        return 0
    print(f"\n{counts[P]} parsed, {counts[D]} derived, {counts[F]} firmware, {counts[N]} not a decode input; "
          f"{bad} problem(s)")
    return 3 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
