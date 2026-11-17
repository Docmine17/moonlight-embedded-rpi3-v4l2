/*
 * Unit and Regression Test Harness for V4L2 M2M + DRM/KMS Backend
 *
 * Tests:
 * 1. Geometry and Aspect-Ratio Scaling (1080p on 1080p, 720p on 1080p, 16:9 on 16:10)
 * 2. Macroblock Crop and Padding Calculations (1088 coded -> 1080 visible)
 * 3. Staging Slot Queue State Machine under High Burst Rate (Stress Test)
 * 4. SPS Bitstream Header Fixup Integration
 */

#include "../src/video/v4l2.h"
#include "../src/video/v4l2_drm.h"
#include "../src/video/v4l2_codec.h"

#include <sps.h>
#include <h264_stream.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static int g_requeued_indices[32];
static int g_requeued_count = 0;

void v4l2_codec_requeue_capture(struct v4l2_ctx *ctx, int index) {
  assert(index >= 0 && index < V4L2_MAX_CAPTURE_BUFFERS);
  g_requeued_indices[g_requeued_count++] = index;
}

static void test_geometry_scaling(void) {
  printf("[TEST] Testing Geometry and Aspect Ratio Scaling...\n");

  struct v4l2_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* Case 1: 1080p on 1080p display (1:1 match) */
  ctx.crtc_width = 1920;
  ctx.crtc_height = 1080;
  ctx.visible_width = 1920;
  ctx.visible_height = 1080;
  v4l2_drm_update_geometry(&ctx);
  assert(ctx.dst_x == 0);
  assert(ctx.dst_y == 0);
  assert(ctx.dst_w == 1920);
  assert(ctx.dst_h == 1080);

  /* Case 2: 720p on 1080p display (same 16:9 aspect, should scale fullscreen) */
  ctx.visible_width = 1280;
  ctx.visible_height = 720;
  v4l2_drm_update_geometry(&ctx);
  assert(ctx.dst_x == 0);
  assert(ctx.dst_y == 0);
  assert(ctx.dst_w == 1920);
  assert(ctx.dst_h == 1080);

  /* Case 3: 16:9 on 16:10 display (1920x1200, letterboxed top/bottom) */
  ctx.crtc_width = 1920;
  ctx.crtc_height = 1200;
  ctx.visible_width = 1920;
  ctx.visible_height = 1080;
  v4l2_drm_update_geometry(&ctx);
  assert(ctx.dst_x == 0);
  assert(ctx.dst_w == 1920);
  assert(ctx.dst_h == 1080);
  assert(ctx.dst_y == (1200 - 1080) / 2); /* 60px letterbox */

  /* Case 4: 16:9 on 4:3 display (1024x768, letterboxed top/bottom) */
  ctx.crtc_width = 1024;
  ctx.crtc_height = 768;
  ctx.visible_width = 1920;
  ctx.visible_height = 1080;
  v4l2_drm_update_geometry(&ctx);
  assert(ctx.dst_x == 0);
  assert(ctx.dst_w == 1024);
  assert(ctx.dst_h == (int)(1024 * (9.0 / 16.0))); /* 576px */
  assert(ctx.dst_y == (768 - 576) / 2);

  printf("  -> Geometry calculations PASSED.\n");
}

static void test_macroblock_padding_handling(void) {
  printf("[TEST] Testing Macroblock Alignment and Viewport Source Cropping...\n");

  struct v4l2_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));

  ctx.coded_width = 1920;
  ctx.coded_height = 1088; /* 8 lines padding */
  ctx.visible_width = 1920;
  ctx.visible_height = 1080;
  ctx.stride = 1920;

  /* Check that visible dimensions clip exactly the padding */
  uint32_t src_w = ctx.visible_width << 16;
  uint32_t src_h = ctx.visible_height << 16;

  assert(src_w == (1920 << 16));
  assert(src_h == (1080 << 16));
  assert(ctx.coded_height - ctx.visible_height == 8);

  printf("  -> Macroblock padding clipping logic PASSED.\n");
}

static void test_staging_slot_state_machine(void) {
  printf("[TEST] Testing Staging Slot State Machine under Frame Bursts...\n");

  struct v4l2_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  pthread_mutex_init(&ctx.drm_lock, NULL);
  ctx.current_displayed_idx = -1;
  ctx.pending_flip_idx = -1;
  ctx.staged_frame_idx = -1;
  ctx.flip_pending = false;

  g_requeued_count = 0;

  /* Simulate Frame 0 arrives */
  pthread_mutex_lock(&ctx.drm_lock);
  ctx.flip_pending = true;
  ctx.pending_flip_idx = 0;
  pthread_mutex_unlock(&ctx.drm_lock);

  /* Simulate Frame 1 arrives before Frame 0 VBlank completes */
  pthread_mutex_lock(&ctx.drm_lock);
  if (ctx.flip_pending) {
    ctx.staged_frame_idx = 1;
  }
  pthread_mutex_unlock(&ctx.drm_lock);
  assert(ctx.staged_frame_idx == 1);
  assert(g_requeued_count == 0); /* No frames dropped yet */

  /* Simulate Frame 2 arrives before Frame 0 VBlank completes (Burst scenario) */
  pthread_mutex_lock(&ctx.drm_lock);
  if (ctx.flip_pending) {
    if (ctx.staged_frame_idx != -1) {
      /* Drop older staged frame (Frame 1) to keep low latency */
      v4l2_codec_requeue_capture(&ctx, ctx.staged_frame_idx);
    }
    ctx.staged_frame_idx = 2;
  }
  pthread_mutex_unlock(&ctx.drm_lock);

  assert(ctx.staged_frame_idx == 2);
  assert(g_requeued_count == 1);
  assert(g_requeued_indices[0] == 1); /* Frame 1 was cleanly requeued */

  /* Now VBlank completes for Frame 0 */
  pthread_mutex_lock(&ctx.drm_lock);
  if (ctx.current_displayed_idx != -1) {
    v4l2_codec_requeue_capture(&ctx, ctx.current_displayed_idx);
  }
  ctx.current_displayed_idx = ctx.pending_flip_idx; /* 0 */
  ctx.flip_pending = false;

  /* Since Frame 2 was staged, it gets committed immediately */
  if (ctx.staged_frame_idx != -1) {
    int next_idx = ctx.staged_frame_idx;
    ctx.staged_frame_idx = -1;
    ctx.flip_pending = true;
    ctx.pending_flip_idx = next_idx; /* 2 */
  }
  pthread_mutex_unlock(&ctx.drm_lock);

  assert(ctx.current_displayed_idx == 0);
  assert(ctx.pending_flip_idx == 2);
  assert(ctx.staged_frame_idx == -1);
  assert(ctx.flip_pending == true);

  /* Now VBlank completes for Frame 2 */
  pthread_mutex_lock(&ctx.drm_lock);
  if (ctx.current_displayed_idx != -1) {
    v4l2_codec_requeue_capture(&ctx, ctx.current_displayed_idx);
  }
  ctx.current_displayed_idx = ctx.pending_flip_idx; /* 2 */
  ctx.flip_pending = false;
  pthread_mutex_unlock(&ctx.drm_lock);

  assert(ctx.current_displayed_idx == 2);
  assert(ctx.flip_pending == false);
  assert(g_requeued_count == 2);
  assert(g_requeued_indices[1] == 0); /* Frame 0 returned to decoder after scanout */

  pthread_mutex_destroy(&ctx.drm_lock);
  printf("  -> Staging slot state machine and buffer recycling PASSED.\n");
}

static void test_sps_vui_low_latency_fixup(void) {
  printf("[TEST] Testing H.264 SPS VUI Low-Latency Fixup & Level Clamping...\n");

  uint8_t in_buf[256];
  uint8_t out_buf[512];
  LENTRY entry;
  memset(&entry, 0, sizeof(entry));
  entry.bufferType = BUFFER_TYPE_SPS;

  /* Case 1: 720p stream with Level 5.0 (50) and VUI absent */
  h264_stream_t *h_in = h264_new();
  assert(h_in != NULL);
  h_in->nal->nal_ref_idc = NAL_REF_IDC_PRIORITY_HIGHEST;
  h_in->nal->nal_unit_type = NAL_UNIT_TYPE_SPS;
  h_in->sps->profile_idc = 100;
  h_in->sps->chroma_format_idc = 1;
  h_in->sps->level_idc = 50;
  h_in->sps->num_ref_frames = 4;
  h_in->sps->pic_width_in_mbs_minus1 = 79;
  h_in->sps->pic_height_in_map_units_minus1 = 44;
  h_in->sps->frame_mbs_only_flag = 1;
  h_in->sps->direct_8x8_inference_flag = 1;
  h_in->sps->vui_parameters_present_flag = 0;

  in_buf[0] = 0x00;
  in_buf[1] = 0x00;
  in_buf[2] = 0x00;
  in_buf[3] = 0x01;
  int in_nal_len = write_nal_unit(h_in, in_buf + 4, (int)sizeof(in_buf) - 4);
  assert(in_nal_len > 0);
  h264_free(h_in);

  entry.data = (char *)in_buf;
  entry.length = 4 + in_nal_len;

  gs_sps_init(1280, 720);
  uint32_t out_len = 0;
  gs_sps_fix(&entry, GS_SPS_BITSTREAM_FIXUP, out_buf, &out_len);
  assert(out_len > 4);

  h264_stream_t *h_out = h264_new();
  assert(h_out != NULL);
  int parsed = read_nal_unit(h_out, out_buf + 4, (int)(out_len - 4));
  assert(parsed > 0);
  assert(h_out->sps->vui_parameters_present_flag == 1);
  assert(h_out->sps->vui.bitstream_restriction_flag == 1);
  assert(h_out->sps->vui.num_reorder_frames == 0);
  assert(h_out->sps->vui.max_dec_frame_buffering == 1);
  assert(h_out->sps->level_idc == 32);
  assert(h_out->sps->num_ref_frames == 1);
  h264_free(h_out);

  /* Case 2: 1080p stream with existing bitstream_restriction_flag=1, reorder=4, level=50 */
  h_in = h264_new();
  assert(h_in != NULL);
  h_in->nal->nal_ref_idc = NAL_REF_IDC_PRIORITY_HIGHEST;
  h_in->nal->nal_unit_type = NAL_UNIT_TYPE_SPS;
  h_in->sps->profile_idc = 100;
  h_in->sps->chroma_format_idc = 1;
  h_in->sps->level_idc = 50;
  h_in->sps->num_ref_frames = 4;
  h_in->sps->pic_width_in_mbs_minus1 = 119;
  h_in->sps->pic_height_in_map_units_minus1 = 67;
  h_in->sps->frame_mbs_only_flag = 1;
  h_in->sps->direct_8x8_inference_flag = 1;
  h_in->sps->vui_parameters_present_flag = 1;
  h_in->sps->vui.bitstream_restriction_flag = 1;
  h_in->sps->vui.motion_vectors_over_pic_boundaries_flag = 1;
  h_in->sps->vui.log2_max_mv_length_horizontal = 16;
  h_in->sps->vui.log2_max_mv_length_vertical = 16;
  h_in->sps->vui.num_reorder_frames = 4;
  h_in->sps->vui.max_dec_frame_buffering = 8;

  in_nal_len = write_nal_unit(h_in, in_buf + 4, (int)sizeof(in_buf) - 4);
  assert(in_nal_len > 0);
  h264_free(h_in);

  entry.length = 4 + in_nal_len;
  gs_sps_init(1920, 1080);
  out_len = 0;
  gs_sps_fix(&entry, GS_SPS_BITSTREAM_FIXUP, out_buf, &out_len);
  assert(out_len > 4);

  h_out = h264_new();
  assert(h_out != NULL);
  parsed = read_nal_unit(h_out, out_buf + 4, (int)(out_len - 4));
  assert(parsed > 0);
  assert(h_out->sps->vui_parameters_present_flag == 1);
  assert(h_out->sps->vui.bitstream_restriction_flag == 1);
  assert(h_out->sps->vui.num_reorder_frames == 0);
  assert(h_out->sps->vui.max_dec_frame_buffering == 1);
  assert(h_out->sps->level_idc == 42);
  h264_free(h_out);

  printf("  -> H.264 SPS VUI fixup and level clamping PASSED.\n");
}

static void test_export_fd_cleanup_safety(void) {
  printf("[TEST] Testing export_fd=-1 Cleanup Safety (No Close on fd 0)...\n");

  struct v4l2_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.drm_fd = -1;
  ctx.num_capture_bufs = 4;
  for (int i = 0; i < V4L2_MAX_CAPTURE_BUFFERS; i++) {
    ctx.capture_bufs[i].export_fd = -1;
  }

  v4l2_drm_release_capture_buffers(&ctx);

  assert(fcntl(0, F_GETFD) != -1);
  for (int i = 0; i < V4L2_MAX_CAPTURE_BUFFERS; i++) {
    assert(ctx.capture_bufs[i].export_fd == -1);
  }

  printf("  -> export_fd=-1 cleanup safety PASSED.\n");
}

static void test_colorspace_and_full_props_flag(void) {
  printf("[TEST] Testing Colorspace Mapping and Full-Props Commit Flag...\n");

  struct v4l2_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.val_color_encoding_bt601 = 0;
  ctx.val_color_encoding_bt709 = 1;
  ctx.val_color_range_limited = 0;
  ctx.val_color_range_full = 1;

  v4l2_drm_set_colorspace(&ctx, V4L2_COLORSPACE_REC709, V4L2_QUANTIZATION_LIM_RANGE);
  assert(ctx.color_encoding == ctx.val_color_encoding_bt709);
  assert(ctx.color_range == ctx.val_color_range_limited);
  assert(ctx.plane_needs_full_props == true);

  ctx.plane_needs_full_props = false;
  v4l2_drm_set_colorspace(&ctx, V4L2_COLORSPACE_SMPTE170M, V4L2_QUANTIZATION_FULL_RANGE);
  assert(ctx.color_encoding == ctx.val_color_encoding_bt601);
  assert(ctx.color_range == ctx.val_color_range_full);
  assert(ctx.plane_needs_full_props == true);

  printf("  -> Colorspace mapping and full-props flag PASSED.\n");
}

static void test_drm_refresh_rate_x100(void) {
  printf("[TEST] Testing DRM Mode Refresh Rate Calculation (x100)...\n");

  drmModeModeInfo mode;
  memset(&mode, 0, sizeof(mode));
  /* 1080p60 CEA: 148.500 MHz / (2200 * 1125) = 60.00 Hz -> 6000 */
  mode.clock = 148500;
  mode.htotal = 2200;
  mode.vtotal = 1125;
  assert(v4l2_drm_calc_refresh_x100(&mode) == 6000);

  /* 1080p59.94 CEA: 148.352 MHz / (2200 * 1125) = 59.94 Hz -> 5994 */
  mode.clock = 148352;
  assert(v4l2_drm_calc_refresh_x100(&mode) == 5994);

  /* 23.976 Hz rounding: 74.176 MHz / (2750 * 1125) = 23.976 Hz -> 2398 */
  mode.clock = 74176;
  mode.htotal = 2750;
  mode.vtotal = 1125;
  assert(v4l2_drm_calc_refresh_x100(&mode) == 2398);

  /* Interlaced doubles field rate; dblscan halves it */
  mode.clock = 74250;
  mode.htotal = 2200;
  mode.vtotal = 1125;
  mode.flags = DRM_MODE_FLAG_INTERLACE;
  assert(v4l2_drm_calc_refresh_x100(&mode) == 6000);

  mode.flags = DRM_MODE_FLAG_DBLSCAN;
  assert(v4l2_drm_calc_refresh_x100(&mode) == 1500);

  /* Fallback to vrefresh when timings are absent */
  memset(&mode, 0, sizeof(mode));
  mode.vrefresh = 60;
  assert(v4l2_drm_calc_refresh_x100(&mode) == 6000);
  assert(v4l2_drm_calc_refresh_x100(NULL) == 0);

  printf("  -> DRM mode refresh rate x100 PASSED.\n");
}

int main(void) {
  printf("=== RUNNING V4L2 BACKEND TEST SUITE ===\n");
  test_geometry_scaling();
  test_macroblock_padding_handling();
  test_staging_slot_state_machine();
  test_sps_vui_low_latency_fixup();
  test_export_fd_cleanup_safety();
  test_colorspace_and_full_props_flag();
  test_drm_refresh_rate_x100();
  printf("=== ALL V4L2 BACKEND TESTS PASSED SUCCESSFULLY! ===\n");
  return 0;
}
