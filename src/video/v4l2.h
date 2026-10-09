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
#include <pthread.h>
#include <linux/videodev2.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define V4L2_MAX_OUTPUT_BUFFERS 16
#define V4L2_MAX_CAPTURE_BUFFERS 20
#define V4L2_OUTPUT_BUFFER_SIZE (1024 * 1024) /* 1 MB buffer for high-bitrate I-frames */

struct v4l2_buf {
  void *start;
  size_t length;
  int export_fd;
  uint32_t gem_handle;
  uint32_t fb_id;
  bool queued;
};

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

  /* Presentation and page-flip synchronization */
  pthread_mutex_t drm_lock;
  bool flip_pending;
  int current_displayed_idx;
  int pending_flip_idx;
  int staged_frame_idx;

  /* Threading & lifecycle */
  pthread_mutex_t state_mutex;
  bool running;
  bool streaming;
  pthread_t event_thread;

  /* Bitstream staging buffer */
  uint8_t *bitstream_buf;
  size_t bitstream_buf_size;
};

#endif /* VIDEO_V4L2_H */
