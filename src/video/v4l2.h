/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2026 Moonlight Embedded Contributors
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef VIDEO_V4L2_H
#define VIDEO_V4L2_H

#include "video.h"
#include <Limelight.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#ifdef V4L2_BENCHMARK
#include <time.h>
#endif
#include <linux/videodev2.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define V4L2_MAX_OUTPUT_BUFFERS 4
#define V4L2_MAX_CAPTURE_BUFFERS 20
#define V4L2_OUTPUT_BUFFER_SIZE (1024 * 1024) /* 1 MB buffer for high-bitrate I-frames */

struct v4l2_buf {
  void *start;
  size_t length;
  int export_fd;
  uint32_t gem_handle;
  uint32_t fb_id;
  bool queued;
#ifdef V4L2_BENCHMARK
  uint64_t t_submit_ns;
  uint64_t t_dqbuf_ns;
#endif
};

#ifdef V4L2_BENCHMARK
struct v4l2_bench_stats {
  uint64_t window_start_ns;
  uint64_t session_start_ns;
  /* Per-1s window counters */
  _Atomic uint32_t rx_frames;
  uint32_t dec_frames;
  uint32_t disp_frames;
  uint32_t staged_drops;
  uint32_t burst_drops;
  _Atomic uint64_t submit_total_ns;
  _Atomic uint64_t submit_wait_ns;
  uint64_t hw_dec_total_ns;
  uint64_t vblank_wait_total_ns;
  uint64_t total_pi_ns;
  uint64_t total_pi_min_ns;
  uint64_t total_pi_max_ns;
  /* Session cumulative counters */
  _Atomic uint64_t sess_rx_frames;
  uint64_t sess_dec_frames;
  uint64_t sess_disp_frames;
  uint64_t sess_staged_drops;
  uint64_t sess_burst_drops;
  _Atomic uint64_t sess_submit_total_ns;
  _Atomic uint64_t sess_submit_wait_ns;
  uint64_t sess_hw_dec_total_ns;
  uint64_t sess_vblank_wait_total_ns;
  uint64_t sess_total_pi_ns;
  uint64_t sess_total_pi_min_ns;
  uint64_t sess_total_pi_max_ns;
};

static inline uint64_t v4l2_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

struct v4l2_ctx {
  int v4l2_fd;
  int drm_fd;

  /* Target stream dimensions */
  int stream_width;
  int stream_height;

  /* Decoder negotiated geometry */
  uint32_t coded_width;
  uint32_t coded_height;
  uint32_t visible_width;
  uint32_t visible_height;
  uint32_t stride;

  /* Display destination geometry */
  int crtc_width;
  int crtc_height;
  int dst_x;
  int dst_y;
  int dst_w;
  int dst_h;

  /* V4L2 queues */
  bool output_streaming;
  bool capture_streaming;
  int num_output_bufs;
  int num_capture_bufs;
  struct v4l2_buf output_bufs[V4L2_MAX_OUTPUT_BUFFERS];
  struct v4l2_buf capture_bufs[V4L2_MAX_CAPTURE_BUFFERS];

  /* DRM / KMS atomic resources */
  uint32_t crtc_id;
  uint32_t plane_id;
  uint32_t conn_id;
  drmModeAtomicReqPtr atomic_req;
  bool plane_needs_full_props;

  /* DRM plane atomic property IDs */
  uint32_t prop_fb_id;
  uint32_t prop_crtc_id;
  uint32_t prop_crtc_x;
  uint32_t prop_crtc_y;
  uint32_t prop_crtc_w;
  uint32_t prop_crtc_h;
  uint32_t prop_src_x;
  uint32_t prop_src_y;
  uint32_t prop_src_w;
  uint32_t prop_src_h;

  /* Optional DRM plane color space properties */
  uint32_t prop_color_encoding;
  uint32_t prop_color_range;
  uint64_t val_color_encoding_bt601;
  uint64_t val_color_encoding_bt709;
  uint64_t val_color_range_limited;
  uint64_t val_color_range_full;
  uint64_t color_encoding;
  uint64_t color_range;

  /* Saved initial plane state for console restoration */
  bool orig_plane_restored;
  uint64_t orig_fb_id;
  uint64_t orig_crtc_id;
  uint64_t orig_crtc_x;
  uint64_t orig_crtc_y;
  uint64_t orig_crtc_w;
  uint64_t orig_crtc_h;
  uint64_t orig_src_x;
  uint64_t orig_src_y;
  uint64_t orig_src_w;
  uint64_t orig_src_h;

  /* Presentation and page-flip synchronization */
  pthread_mutex_t drm_lock;
  bool flip_pending;
  int current_displayed_idx;
  int pending_flip_idx;
  int staged_frame_idx;

  /* Threading & lifecycle */
  pthread_mutex_t state_mutex;
  atomic_bool running;
  atomic_bool streaming;
  bool submit_rt_configured;
  pthread_t event_thread;
#ifdef V4L2_BENCHMARK
  struct v4l2_bench_stats stats;
#endif
};

#endif /* VIDEO_V4L2_H */
