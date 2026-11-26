// featdump: prints one JSON object per coded frame with the header features the AV1 and VP9
// parsers read, so tools/diag/features.py can say which tools of each codec the test streams
// exercise. It uses the same parser sources as the decoders (no driver, no GPU).
//
//   featdump av1|vp9 file.(ivf|webm)
//
// Build (tools/diag/run_all.py does it): clang -O1 -I vcnav1 -I vcnvp9 tools/diag/featdump.c
//   vcnav1/av1parse.c vcnvp9/vp9parse.c -o tools/diag/featdump
// Exit status: 0 = the whole file was read, 2 = parse error or a damaged file, 1 = usage.
#include <stdio.h>
#include <string.h>

#include "av1parse.h"
#include "container.h"
#include "vp9parse.h"

static const char* gFile;
static int gFirst;

static void begin(const char* codec, const char* kind) {
  printf("{\"codec\":\"%s\",\"kind\":\"%s\"", codec, kind);
  gFirst = 0;
}
static void num(const char* k, long v) { printf(",\"%s\":%ld", k, v); }
static void end(void) { printf("}\n"); }

static int on_av1(void* ctx, AV1Parser* p, AV1ParseResult kind) {
  (void)ctx;
  const AV1Seq* s = &p->seq;
  const AV1Frame* h = &p->fh;
  begin("av1", kind == AV1_PARSE_SHOW_EXISTING ? "show_existing" : "frame");
  num("profile", s->seq_profile);
  num("bit_depth", s->BitDepth);
  num("mono", s->mono_chrome);
  num("subx", s->subsampling_x);
  num("suby", s->subsampling_y);
  num("still", s->still_picture);
  num("reduced_still", s->reduced_still_picture_header);
  num("sb128", s->use_128x128_superblock);
  num("op_points", s->operating_points_cnt_minus_1 + 1);
  num("frame_ids", s->frame_id_numbers_present_flag);
  num("order_hint_bits", s->OrderHintBits);
  num("seq_superres", s->enable_superres);
  num("seq_cdef", s->enable_cdef);
  num("seq_restoration", s->enable_restoration);
  num("seq_filter_intra", s->enable_filter_intra);
  num("seq_intra_edge", s->enable_intra_edge_filter);
  num("seq_interintra", s->enable_interintra_compound);
  num("seq_masked", s->enable_masked_compound);
  num("seq_warped", s->enable_warped_motion);
  num("seq_dual_filter", s->enable_dual_filter);
  num("seq_jnt_comp", s->enable_jnt_comp);
  num("seq_ref_mvs", s->enable_ref_frame_mvs);
  num("seq_film_grain", s->film_grain_params_present);
  num("timing_info", s->timing_info_present_flag);
  num("decoder_model", s->decoder_model_info_present_flag);
  num("color_desc", s->color_description_present_flag);
  num("color_range", s->color_range);
  num("separate_uv_delta_q", s->separate_uv_delta_q);
  if (kind == AV1_PARSE_SHOW_EXISTING) {
    num("frame_type", h->frame_type);
    num("shown_key", h->frame_type == AV1_KEY_FRAME);
    end();
    return 0;
  }
  num("frame_type", h->frame_type);
  num("show_frame", h->show_frame);
  num("showable", h->showable_frame);
  num("error_resilient", h->error_resilient_mode);
  num("cdf_update_off", h->disable_cdf_update);
  num("end_update_cdf_off", h->disable_frame_end_update_cdf);
  num("screen_content", h->allow_screen_content_tools);
  num("intrabc", h->allow_intrabc);
  num("integer_mv", h->force_integer_mv);
  num("frame_size_override", h->frame_size_override_flag);
  num("short_signaling", h->frame_refs_short_signaling);
  num("primary_none", h->primary_ref_frame == AV1_PRIMARY_REF_NONE);
  num("superres", h->use_superres);
  num("superres_denom", h->SuperresDenom);
  num("width", h->UpscaledWidth);
  num("height", h->FrameHeight);
  num("render_differs", h->RenderWidth != h->UpscaledWidth || h->RenderHeight != h->FrameHeight);
  num("tile_cols", h->TileCols);
  num("tile_rows", h->TileRows);
  num("tile_uniform", h->uniform_tile_spacing_flag);
  num("qmatrix", h->using_qmatrix);
  num("diff_uv_delta", h->diff_uv_delta);
  num("delta_q", h->delta_q_present);
  num("delta_lf", h->delta_lf_present);
  num("delta_lf_multi", h->delta_lf_multi);
  num("seg", h->segmentation_enabled);
  num("seg_update_map", h->segmentation_update_map);
  num("seg_temporal", h->segmentation_temporal_update);
  num("seg_update_data", h->segmentation_update_data);
  num("coded_lossless", h->CodedLossless);
  num("all_lossless", h->AllLossless);
  num("lf_on", h->loop_filter_level[0] || h->loop_filter_level[1]);
  num("lf_delta", h->loop_filter_delta_enabled);
  num("lf_sharp", h->loop_filter_sharpness);
  num("cdef_bits", h->cdef_bits);
  num("lr_y", h->FrameRestorationType[0]);
  num("lr_u", h->FrameRestorationType[1]);
  num("lr_v", h->FrameRestorationType[2]);
  num("tx_mode", h->TxMode);
  num("ref_select", h->reference_select);
  num("skip_mode", h->skip_mode_present);
  num("warped", h->allow_warped_motion);
  num("reduced_tx", h->reduced_tx_set);
  num("hp_mv", h->allow_high_precision_mv);
  num("interp", h->interpolation_filter);
  num("motion_switchable", h->is_motion_mode_switchable);
  num("ref_mvs", h->use_ref_frame_mvs);
  int gm = 0;
  for (int r = 1; r <= 7; r++) gm |= 1 << h->gm_type[r];
  num("gm_types", gm);  // bit t set: some reference uses global motion type t
  num("grain", h->film_grain.apply_grain);
  num("grain_update", h->film_grain.update_grain);
  num("grain_chroma_from_luma", h->film_grain.chroma_scaling_from_luma);
  num("grain_overlap", h->film_grain.overlap_flag);
  num("grain_ar_lag", h->film_grain.ar_coeff_lag);
  num("tile_data_bytes", (long)p->tile_data_size);
  end();
  av1_reference_update(p, 0);
  return 0;
}

static int on_vp9(void* ctx, VP9Parser* p, VP9ParseResult kind, const uint8_t* d,
                  size_t size) {
  (void)d, (void)size;
  int superframe = ctx ? *(int*)ctx : 0;
  const VP9Frame* h = &p->fh;
  begin("vp9", kind == VP9_PARSE_SHOW_EXISTING ? "show_existing" : "frame");
  num("profile", h->profile);
  num("superframe", superframe);
  if (kind == VP9_PARSE_SHOW_EXISTING) {
    end();
    return 0;
  }
  num("bit_depth", h->bit_depth);
  num("subx", h->subsampling_x);
  num("suby", h->subsampling_y);
  num("frame_type", h->frame_type);
  num("show_frame", h->show_frame);
  num("intra_only", h->intra_only);
  num("error_res", h->error_res);
  num("reset_ctx", h->reset_frame_context);
  num("refresh_ctx", h->refresh_frame_context);
  num("frame_parallel", h->frame_parallel);
  num("ctx_idx", h->frame_context_idx);
  num("width", h->width);
  num("height", h->height);
  int scaled = 0;
  if (h->frame_type != 0 && !h->intra_only)
    for (int i = 0; i < 3; i++) {
      const VP9Ref* r = &p->ref[h->ref_frame_idx[i]];
      if (r->valid && (r->width != h->width || r->height != h->height)) scaled = 1;
    }
  num("scaled_ref", scaled);
  for (int i = 0; i < 3; i++) {  // size of LAST, GOLDEN, ALTREF (0 for an intra frame)
    char k[16];
    const VP9Ref* r = h->frame_type != 0 && !h->intra_only ? &p->ref[h->ref_frame_idx[i]] : NULL;
    snprintf(k, sizeof k, "ref%d_w", i);
    num(k, r && r->valid ? (long)r->width : 0);
    snprintf(k, sizeof k, "ref%d_h", i);
    num(k, r && r->valid ? (long)r->height : 0);
  }
  num("sign_bias", h->sign_bias[0] | h->sign_bias[1] | h->sign_bias[2]);
  num("hp_mv", h->allow_high_precision_mv);
  num("interp", h->interp_filter);
  num("lf_level", h->lf_level);
  num("lf_sharp", h->lf_sharpness);
  num("lf_delta", h->lf_delta_enabled);
  num("lf_delta_update", h->lf_delta_update);
  num("base_q", h->base_q_idx);
  num("lossless", h->base_q_idx == 0 && !h->y_dc_delta_q && !h->uv_dc_delta_q && !h->uv_ac_delta_q);
  num("delta_q", h->y_dc_delta_q || h->uv_dc_delta_q || h->uv_ac_delta_q);
  num("seg", h->seg_enabled);
  num("seg_update_map", h->seg_update_map);
  num("seg_temporal", h->seg_temporal_update);
  num("seg_update_data", h->seg_update_data);
  num("seg_abs", p->seg_abs_delta);
  num("tile_cols_log2", h->log2_tile_cols);
  num("tile_rows_log2", h->log2_tile_rows);
  num("prev_mvs", h->use_prev_frame_mvs);
  num("supported", vp9_frame_supported(p));
  end();
  vp9_frame_done(p, 0);
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 3 || (strcmp(argv[1], "av1") && strcmp(argv[1], "vp9"))) {
    fprintf(stderr, "usage: featdump av1|vp9 file.(ivf|webm)\n");
    return 1;
  }
  gFile = argv[2];
  Container c;
  if (!container_open(&c, gFile)) {
    fprintf(stderr, "%s: %s\n", gFile, c.problem);
    return 2;
  }
  int bad = 0;
  const uint8_t* d;
  size_t n;
  if (!strcmp(argv[1], "av1")) {
    AV1Parser* p = calloc(1, sizeof(*p));
    av1_parser_init(p);
    while (container_next(&c, &d, &n))
      if (av1_parse_temporal_unit(p, d, n, on_av1, NULL)) {
        fprintf(stderr, "%s: AV1 parse error\n", gFile);
        bad = 1;
        break;
      }
    av1_parser_free(p);
    free(p);
  } else {
    VP9Parser* p = calloc(1, sizeof(*p));
    vp9_parser_init(p);
    while (container_next(&c, &d, &n)) {
      int sf = n && (d[n - 1] & 0xe0) == 0xc0;
      if (vp9_parse_chunk(p, d, n, on_vp9, &sf)) {
        fprintf(stderr, "%s: VP9 parse error: %s\n", gFile, p->err);
        bad = 1;
        break;
      }
    }
    free(p);
  }
  if (c.problem) {
    fprintf(stderr, "%s: %s\n", gFile, c.problem);
    bad = 1;
  }
  container_close(&c);
  return bad ? 2 : 0;
}
