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

#include "v4l2_drm.h"
#include "v4l2_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <libdrm/drm_fourcc.h>

#define V4L2_LATE_LATCH_LEAD_NS 2000000ULL /* Commit 2.0 ms before target VBlank */

static int get_property_info(int fd, uint32_t obj_id, uint32_t obj_type, const char *name,
                             uint32_t *prop_id, uint64_t *init_val) {
  drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
  if (!props)
    return -1;

  for (uint32_t i = 0; i < props->count_props; i++) {
    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop)
      continue;

    if (strcmp(prop->name, name) == 0) {
      if (prop_id)
        *prop_id = prop->prop_id;
      if (init_val)
        *init_val = props->prop_values[i];
      drmModeFreeProperty(prop);
      drmModeFreeObjectProperties(props);
      return 0;
    }
    drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);
  return -1;
}

static uint64_t get_plane_type(int fd, uint32_t plane_id) {
  uint64_t type = DRM_PLANE_TYPE_OVERLAY;
  get_property_info(fd, plane_id, DRM_MODE_OBJECT_PLANE, "type", NULL, &type);
  return type;
}

static void get_color_enum_property(int fd, uint32_t plane_id, const char *prop_name,
                                    uint32_t *prop_id,
                                    const char *enum_a, uint64_t *val_a,
                                    const char *enum_b, uint64_t *val_b) {
  drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (!props)
    return;

  for (uint32_t i = 0; i < props->count_props; i++) {
    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop)
      continue;

    if (strcmp(prop->name, prop_name) == 0) {
      *prop_id = prop->prop_id;
      for (int e = 0; e < prop->count_enums; e++) {
        if (strcmp(prop->enums[e].name, enum_a) == 0)
          *val_a = prop->enums[e].value;
        else if (strcmp(prop->enums[e].name, enum_b) == 0)
          *val_b = prop->enums[e].value;
      }
      drmModeFreeProperty(prop);
      drmModeFreeObjectProperties(props);
      return;
    }
    drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);
}

static int open_vc4_drm_device(void) {
  for (int i = 0; i < 4; i++) {
    char dev_path[32];
    snprintf(dev_path, sizeof(dev_path), "/dev/dri/card%d", i);

    int fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
      continue;

    drmVersionPtr ver = drmGetVersion(fd);
    if (ver) {
      bool is_vc4 = (strcmp(ver->name, "vc4") == 0);
      drmFreeVersion(ver);
      if (is_vc4)
        return fd;
    }
    close(fd);
  }

  /* Fallback to primary card if vc4-specific match was not found */
  return open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
}

void v4l2_drm_update_geometry(struct v4l2_ctx *ctx) {
  ctx->plane_needs_full_props = true;

  if (ctx->crtc_width <= 0 || ctx->crtc_height <= 0 ||
      ctx->visible_width <= 0 || ctx->visible_height <= 0) {
    ctx->dst_x = 0;
    ctx->dst_y = 0;
    ctx->dst_w = ctx->crtc_width;
    ctx->dst_h = ctx->crtc_height;
    return;
  }

  float crtc_ratio = (float)ctx->crtc_width / ctx->crtc_height;
  float frame_ratio = (float)ctx->visible_width / ctx->visible_height;

  if (crtc_ratio > frame_ratio) {
    ctx->dst_w = (int)(frame_ratio / crtc_ratio * ctx->crtc_width);
    ctx->dst_h = ctx->crtc_height;
    ctx->dst_x = (ctx->crtc_width - ctx->dst_w) / 2;
    ctx->dst_y = 0;
  } else {
    ctx->dst_w = ctx->crtc_width;
    ctx->dst_h = (int)(crtc_ratio / frame_ratio * ctx->crtc_height);
    ctx->dst_x = 0;
    ctx->dst_y = (ctx->crtc_height - ctx->dst_h) / 2;
  }
}

void v4l2_drm_set_colorspace(struct v4l2_ctx *ctx, uint32_t v4l2_colorspace, uint32_t v4l2_quantization) {
  if (v4l2_colorspace == V4L2_COLORSPACE_SMPTE170M ||
      v4l2_colorspace == V4L2_COLORSPACE_470_SYSTEM_M ||
      v4l2_colorspace == V4L2_COLORSPACE_470_SYSTEM_BG) {
    ctx->color_encoding = ctx->val_color_encoding_bt601;
  } else {
    ctx->color_encoding = ctx->val_color_encoding_bt709;
  }

  if (v4l2_quantization == V4L2_QUANTIZATION_FULL_RANGE) {
    ctx->color_range = ctx->val_color_range_full;
  } else {
    ctx->color_range = ctx->val_color_range_limited;
  }

  ctx->plane_needs_full_props = true;
}

int v4l2_drm_calc_refresh_x100(const drmModeModeInfo *mode) {
  if (!mode)
    return 0;

  if (mode->clock > 0 && mode->htotal > 0 && mode->vtotal > 0) {
    uint64_t num = (uint64_t)mode->clock * 100000ULL;
    if (mode->flags & DRM_MODE_FLAG_INTERLACE)
      num *= 2ULL;

    uint64_t denom = (uint64_t)mode->htotal * (uint64_t)mode->vtotal;
    if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
      denom *= 2ULL;
    if (mode->vscan > 1)
      denom *= (uint64_t)mode->vscan;

    if (denom > 0) {
      int x100 = (int)((num + denom / 2ULL) / denom);
      if (x100 > 0)
        return x100;
    }
  }

  return (mode->vrefresh > 0) ? (int)(mode->vrefresh * 100U) : 0;
}

static int find_active_drm_mode(int drm_fd, bool probe_connectors,
                                drmModeResPtr *res_out, uint32_t *conn_id_out,
                                uint32_t *crtc_id_out, drmModeModeInfo *mode_out,
                                bool *mode_valid_out) {
  drmModeResPtr res = drmModeGetResources(drm_fd);
  if (!res)
    return -1;

  drmModeConnectorPtr conn = NULL;
  for (int i = 0; i < res->count_connectors; i++) {
    conn = probe_connectors ? drmModeGetConnector(drm_fd, res->connectors[i])
                            : drmModeGetConnectorCurrent(drm_fd, res->connectors[i]);
    if (!probe_connectors && conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes == 0) {
      drmModeFreeConnector(conn);
      conn = drmModeGetConnector(drm_fd, res->connectors[i]);
    }
    if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
      break;

    if (conn) {
      drmModeFreeConnector(conn);
      conn = NULL;
    }
  }

  if (!conn) {
    drmModeFreeResources(res);
    return -1;
  }

  drmModeEncoderPtr enc = NULL;
  for (int i = 0; i < res->count_encoders; i++) {
    enc = drmModeGetEncoder(drm_fd, res->encoders[i]);
    if (enc && enc->encoder_id == conn->encoder_id)
      break;

    if (enc) {
      drmModeFreeEncoder(enc);
      enc = NULL;
    }
  }

  uint32_t crtc_id = 0;
  if (enc) {
    crtc_id = enc->crtc_id;
    drmModeFreeEncoder(enc);
  }
  if (crtc_id == 0 && res->count_crtcs > 0)
    crtc_id = res->crtcs[0];

  bool mode_valid = false;
  drmModeModeInfo mode;
  memset(&mode, 0, sizeof(mode));

  drmModeCrtcPtr crtc = crtc_id ? drmModeGetCrtc(drm_fd, crtc_id) : NULL;
  if (crtc && crtc->mode_valid && crtc->width > 0 && crtc->height > 0) {
    mode = crtc->mode;
    mode_valid = true;
  } else if (conn->count_modes > 0) {
    mode = conn->modes[0];
  }
  if (crtc)
    drmModeFreeCrtc(crtc);

  if (conn_id_out)
    *conn_id_out = conn->connector_id;
  if (crtc_id_out)
    *crtc_id_out = crtc_id;
  if (mode_out)
    *mode_out = mode;
  if (mode_valid_out)
    *mode_valid_out = mode_valid;

  drmModeFreeConnector(conn);
  if (res_out)
    *res_out = res;
  else
    drmModeFreeResources(res);

  return 0;
}

int v4l2_drm_get_refresh_rate_x100(void) {
  int fd = open_vc4_drm_device();
  if (fd < 0)
    return 0;

  drmModeModeInfo mode;
  memset(&mode, 0, sizeof(mode));
  int ret = find_active_drm_mode(fd, false, NULL, NULL, NULL, &mode, NULL);
  close(fd);

  return (ret == 0) ? v4l2_drm_calc_refresh_x100(&mode) : 0;
}

int v4l2_drm_init(struct v4l2_ctx *ctx) {
  ctx->drm_fd = open_vc4_drm_device();
  if (ctx->drm_fd < 0) {
    perror("Can't open DRM device");
    return -1;
  }

  if (drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
    perror("drmSetClientCap UNIVERSAL_PLANES failed");
  }

  if (drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
    fprintf(stderr, "Atomic KMS not supported on DRM card\n");
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
    return -1;
  }

  drmModeResPtr res = NULL;
  drmModeModeInfo active_mode;
  bool mode_valid = false;
  if (find_active_drm_mode(ctx->drm_fd, true, &res, &ctx->conn_id, &ctx->crtc_id,
                           &active_mode, &mode_valid) != 0) {
    fprintf(stderr, "No connected DRM connector found\n");
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
    return -1;
  }

  ctx->vblank_period_ns = 16666667ULL; /* Default 60 Hz period until calibrated */
  ctx->last_vblank_ns = 0;
  ctx->next_commit_ns = 0;

  if (active_mode.hdisplay > 0 && active_mode.vdisplay > 0) {
    ctx->crtc_width = active_mode.hdisplay;
    ctx->crtc_height = active_mode.vdisplay;
    if (mode_valid && active_mode.clock > 0 && active_mode.htotal > 0 && active_mode.vtotal > 0) {
      uint64_t period = ((uint64_t)active_mode.htotal * (uint64_t)active_mode.vtotal * 1000000ULL) /
                        (uint64_t)active_mode.clock;
      if (period >= 8000000ULL && period <= 25000000ULL)
        ctx->vblank_period_ns = period;
    }
  } else {
    ctx->crtc_width = 1920;
    ctx->crtc_height = 1080;
  }

  /* Find CRTC index */
  int crtc_index = -1;
  for (int i = 0; i < res->count_crtcs; i++) {
    if (res->crtcs[i] == ctx->crtc_id) {
      crtc_index = i;
      break;
    }
  }
  if (crtc_index < 0) {
    fprintf(stderr, "V4L2 DRM: Failed to find CRTC index for ID %u\n", ctx->crtc_id);
    drmModeFreeResources(res);
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
    return -1;
  }

  /* Prefer a primary plane connected to this CRTC supporting NV12; fallback to overlay */
  drmModePlaneResPtr plane_res = drmModeGetPlaneResources(ctx->drm_fd);
  if (!plane_res) {
    perror("drmModeGetPlaneResources failed");
    drmModeFreeResources(res);
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
    return -1;
  }

  uint32_t primary_plane = 0;
  uint32_t fallback_plane = 0;
  for (uint32_t i = 0; i < plane_res->count_planes; i++) {
    drmModePlanePtr plane = drmModeGetPlane(ctx->drm_fd, plane_res->planes[i]);
    if (!plane)
      continue;

    /* Verify plane can connect to our chosen CRTC */
    if (!(plane->possible_crtcs & (1 << crtc_index))) {
      drmModeFreePlane(plane);
      continue;
    }

    bool has_nv12 = false;
    for (uint32_t j = 0; j < plane->count_formats; j++) {
      if (plane->formats[j] == DRM_FORMAT_NV12) {
        has_nv12 = true;
        break;
      }
    }

    uint32_t pid = plane->plane_id;
    drmModeFreePlane(plane);

    if (has_nv12) {
      if (get_plane_type(ctx->drm_fd, pid) == DRM_PLANE_TYPE_PRIMARY) {
        primary_plane = pid;
        break;
      }
      if (fallback_plane == 0)
        fallback_plane = pid;
    }
  }
  uint32_t selected_plane = (primary_plane != 0) ? primary_plane : fallback_plane;

  drmModeFreePlaneResources(plane_res);
  drmModeFreeResources(res);

  if (selected_plane == 0) {
    fprintf(stderr, "No DRM plane supporting DRM_FORMAT_NV12 found for CRTC %u\n", ctx->crtc_id);
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
    return -1;
  }

  ctx->plane_id = selected_plane;

  /* Cache atomic plane properties and snapshot initial plane state for exit restoration */
  if (get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &ctx->prop_fb_id, &ctx->orig_fb_id) != 0 ||
      get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &ctx->prop_crtc_id, &ctx->orig_crtc_id) != 0 ||
      get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &ctx->prop_crtc_x, &ctx->orig_crtc_x) != 0 ||
      get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &ctx->prop_crtc_y, &ctx->orig_crtc_y) != 0 ||
      get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &ctx->prop_crtc_w, &ctx->orig_crtc_w) != 0 ||
      get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &ctx->prop_crtc_h, &ctx->orig_crtc_h) != 0 ||
      get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &ctx->prop_src_x, &ctx->orig_src_x) != 0 ||
      get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &ctx->prop_src_y, &ctx->orig_src_y) != 0 ||
      get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &ctx->prop_src_w, &ctx->orig_src_w) != 0 ||
      get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &ctx->prop_src_h, &ctx->orig_src_h) != 0) {
    fprintf(stderr, "V4L2 DRM: Required atomic properties missing on plane %u\n", ctx->plane_id);
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
    return -1;
  }

  ctx->prop_color_encoding = 0;
  ctx->prop_color_range = 0;
  ctx->val_color_encoding_bt601 = UINT64_MAX;
  ctx->val_color_encoding_bt709 = UINT64_MAX;
  ctx->val_color_range_limited = UINT64_MAX;
  ctx->val_color_range_full = UINT64_MAX;
  get_color_enum_property(ctx->drm_fd, ctx->plane_id, "COLOR_ENCODING", &ctx->prop_color_encoding,
                          "ITU-R BT.601 YCbCr", &ctx->val_color_encoding_bt601,
                          "ITU-R BT.709 YCbCr", &ctx->val_color_encoding_bt709);
  get_color_enum_property(ctx->drm_fd, ctx->plane_id, "COLOR_RANGE", &ctx->prop_color_range,
                          "YCbCr limited range", &ctx->val_color_range_limited,
                          "YCbCr full range", &ctx->val_color_range_full);
  ctx->color_encoding = ctx->val_color_encoding_bt709;
  ctx->color_range = ctx->val_color_range_limited;

  ctx->atomic_req = drmModeAtomicAlloc();
  ctx->plane_needs_full_props = true;
  ctx->orig_plane_restored = false;

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
  pthread_mutex_init(&ctx->drm_lock, &attr);
  pthread_mutexattr_destroy(&attr);

  ctx->flip_pending = false;
  ctx->current_displayed_idx = -1;
  ctx->pending_flip_idx = -1;
  ctx->staged_frame_idx = -1;

  return 0;
}

void v4l2_drm_destroy(struct v4l2_ctx *ctx) {
  if (ctx->drm_fd >= 0) {
    v4l2_drm_restore_plane_sync(ctx);
    v4l2_drm_release_capture_buffers(ctx);
    if (ctx->atomic_req) {
      drmModeAtomicFree(ctx->atomic_req);
      ctx->atomic_req = NULL;
    }
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
  }
  pthread_mutex_destroy(&ctx->drm_lock);
}

int v4l2_drm_import_capture_buffer(struct v4l2_ctx *ctx, int index) {
  if (ctx->capture_bufs[index].export_fd < 0)
    return -1;

  int ret = drmPrimeFDToHandle(ctx->drm_fd, ctx->capture_bufs[index].export_fd,
                               &ctx->capture_bufs[index].gem_handle);
  if (ret != 0) {
    perror("drmPrimeFDToHandle failed");
    return -1;
  }

  uint32_t handles[4] = {
    ctx->capture_bufs[index].gem_handle,
    ctx->capture_bufs[index].gem_handle,
    0, 0
  };
  uint32_t pitches[4] = { ctx->stride, ctx->stride, 0, 0 };
  uint32_t offsets[4] = { 0, ctx->stride * ctx->coded_height, 0, 0 };

  ret = drmModeAddFB2(ctx->drm_fd, ctx->coded_width, ctx->coded_height,
                      DRM_FORMAT_NV12, handles, pitches, offsets,
                      &ctx->capture_bufs[index].fb_id, 0);
  if (ret != 0) {
    perror("drmModeAddFB2 failed");
    return -1;
  }

  return 0;
}

void v4l2_drm_release_capture_buffers(struct v4l2_ctx *ctx) {
  for (int i = 0; i < ctx->num_capture_bufs; i++) {
    if (ctx->capture_bufs[i].fb_id != 0) {
      drmModeRmFB(ctx->drm_fd, ctx->capture_bufs[i].fb_id);
      ctx->capture_bufs[i].fb_id = 0;
    }
    if (ctx->capture_bufs[i].gem_handle != 0) {
      struct drm_gem_close close_req = { .handle = ctx->capture_bufs[i].gem_handle };
      ioctl(ctx->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_req);
      ctx->capture_bufs[i].gem_handle = 0;
    }
    if (ctx->capture_bufs[i].export_fd >= 0) {
      close(ctx->capture_bufs[i].export_fd);
      ctx->capture_bufs[i].export_fd = -1;
    }
  }
}

static int commit_atomic_flip(struct v4l2_ctx *ctx, int buf_idx) {
  drmModeAtomicReqPtr req = ctx->atomic_req;
  bool temp_req = false;
  if (req) {
    drmModeAtomicSetCursor(req, 0);
  } else {
    req = drmModeAtomicAlloc();
    if (!req)
      return -1;
    temp_req = true;
  }

  uint32_t fb_id = ctx->capture_bufs[buf_idx].fb_id;
  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_fb_id, fb_id);

  if (ctx->plane_needs_full_props) {
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_id, ctx->crtc_id);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_x, ctx->dst_x);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_y, ctx->dst_y);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_w, ctx->dst_w);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_h, ctx->dst_h);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_x, 0);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_y, 0);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_w, (uint64_t)ctx->visible_width << 16);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_h, (uint64_t)ctx->visible_height << 16);
    if (ctx->prop_color_encoding != 0 && ctx->color_encoding != UINT64_MAX)
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_color_encoding, ctx->color_encoding);
    if (ctx->prop_color_range != 0 && ctx->color_range != UINT64_MAX)
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_color_range, ctx->color_range);
  }

  uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
  int ret = drmModeAtomicCommit(ctx->drm_fd, req, flags, ctx);
  if (ret == 0) {
    ctx->plane_needs_full_props = false;
  } else {
    static int flip_err_count = 0;
    if (flip_err_count++ < 10) {
      fprintf(stderr, "V4L2 DRM: drmModeAtomicCommit failed: %d (%s)\n", ret, strerror(errno));
    }
  }

  if (temp_req)
    drmModeAtomicFree(req);

  return ret;
}

static void commit_staged_locked(struct v4l2_ctx *ctx) {
  if (ctx->flip_pending || ctx->staged_frame_idx == -1)
    return;

  int idx = ctx->staged_frame_idx;
  ctx->staged_frame_idx = -1;
  ctx->flip_pending = true;
  ctx->pending_flip_idx = idx;

  if (commit_atomic_flip(ctx, idx) != 0) {
    /* If commit failed, reset pending state and requeue */
    ctx->flip_pending = false;
    ctx->pending_flip_idx = -1;
    v4l2_codec_requeue_capture(ctx, idx);
  }
}

static uint64_t compute_next_deadline_locked(struct v4l2_ctx *ctx, uint64_t now) {
  if (ctx->last_vblank_ns == 0 || ctx->vblank_period_ns == 0 || now < ctx->last_vblank_ns)
    return 0;

  uint64_t elapsed = now - ctx->last_vblank_ns;
  if (elapsed > ctx->vblank_period_ns * 4ULL)
    return 0;

  uint64_t periods = (elapsed / ctx->vblank_period_ns) + 1ULL;
  uint64_t next_vblank = ctx->last_vblank_ns + periods * ctx->vblank_period_ns;

  /* If already within V4L2_LATE_LATCH_LEAD_NS of next_vblank, commit immediately */
  if (next_vblank <= now + V4L2_LATE_LATCH_LEAD_NS)
    return 0;

  return next_vblank - V4L2_LATE_LATCH_LEAD_NS;
}

void v4l2_drm_present(struct v4l2_ctx *ctx, int new_buf_idx) {
  pthread_mutex_lock(&ctx->drm_lock);

  if (ctx->staged_frame_idx != -1) {
    /* Drop older staged frame to maintain lowest latency */
#ifdef V4L2_BENCHMARK
    ctx->stats.staged_drops++;
    ctx->stats.sess_staged_drops++;
#endif
    v4l2_codec_requeue_capture(ctx, ctx->staged_frame_idx);
  }
  ctx->staged_frame_idx = new_buf_idx;

  if (!ctx->flip_pending) {
    uint64_t now = v4l2_now_ns();
    uint64_t deadline = compute_next_deadline_locked(ctx, now);
    if (deadline == 0 || now >= deadline) {
      ctx->next_commit_ns = 0;
      commit_staged_locked(ctx);
    } else {
      ctx->next_commit_ns = deadline;
    }
  }

  pthread_mutex_unlock(&ctx->drm_lock);
}

uint64_t v4l2_drm_check_late_latch(struct v4l2_ctx *ctx) {
  uint64_t wait_ns = 50000000ULL; /* Default 50 ms poll timeout */
  pthread_mutex_lock(&ctx->drm_lock);

  if (!ctx->flip_pending && ctx->staged_frame_idx != -1) {
    uint64_t now = v4l2_now_ns();
    if (ctx->next_commit_ns == 0)
      ctx->next_commit_ns = compute_next_deadline_locked(ctx, now);

    if (ctx->next_commit_ns == 0 || now >= ctx->next_commit_ns) {
      ctx->next_commit_ns = 0;
      commit_staged_locked(ctx);
    } else {
      wait_ns = ctx->next_commit_ns - now;
    }
  }

  pthread_mutex_unlock(&ctx->drm_lock);
  return wait_ns;
}

void v4l2_drm_drain_pending_flip(struct v4l2_ctx *ctx) {
  if (ctx->drm_fd < 0)
    return;

  drmEventContext evctx;
  memset(&evctx, 0, sizeof(evctx));
  evctx.version = DRM_EVENT_CONTEXT_VERSION;
  evctx.page_flip_handler = v4l2_drm_page_flip_handler;

  pthread_mutex_lock(&ctx->drm_lock);
  while (ctx->flip_pending) {
    pthread_mutex_unlock(&ctx->drm_lock);

    struct pollfd pfd;
    pfd.fd = ctx->drm_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ret = poll(&pfd, 1, 100);
    if (ret > 0 && (pfd.revents & POLLIN)) {
      drmHandleEvent(ctx->drm_fd, &evctx);
      pthread_mutex_lock(&ctx->drm_lock);
    } else {
      pthread_mutex_lock(&ctx->drm_lock);
      ctx->flip_pending = false;
      break;
    }
  }
  pthread_mutex_unlock(&ctx->drm_lock);
}

void v4l2_drm_disable_plane_sync(struct v4l2_ctx *ctx) {
  if (ctx->drm_fd < 0 || ctx->plane_id == 0)
    return;

  drmModeAtomicReqPtr req = drmModeAtomicAlloc();
  if (!req)
    return;

  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_fb_id, 0);
  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_id, 0);

  drmModeAtomicCommit(ctx->drm_fd, req, 0, NULL);
  drmModeAtomicFree(req);
  ctx->plane_needs_full_props = true;
}

void v4l2_drm_restore_plane_sync(struct v4l2_ctx *ctx) {
  if (ctx->drm_fd < 0 || ctx->plane_id == 0)
    return;

  if (ctx->orig_fb_id != 0 && ctx->orig_crtc_id != 0) {
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    if (req) {
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_fb_id, ctx->orig_fb_id);
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_id, ctx->orig_crtc_id);
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_x, ctx->orig_crtc_x);
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_y, ctx->orig_crtc_y);
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_w, ctx->orig_crtc_w);
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_h, ctx->orig_crtc_h);
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_x, ctx->orig_src_x);
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_y, ctx->orig_src_y);
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_w, ctx->orig_src_w);
      drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_h, ctx->orig_src_h);

      int ret = drmModeAtomicCommit(ctx->drm_fd, req, 0, NULL);
      drmModeAtomicFree(req);
      ctx->orig_fb_id = 0;
      ctx->plane_needs_full_props = true;
      if (ret == 0) {
        ctx->orig_plane_restored = true;
        return;
      }
    }
  }

  if (!ctx->orig_plane_restored) {
    v4l2_drm_disable_plane_sync(ctx);
  }
}

void v4l2_drm_page_flip_handler(int fd, unsigned int seq, unsigned int tv_sec,
                                unsigned int tv_usec, void *user_data) {
  struct v4l2_ctx *ctx = (struct v4l2_ctx *)user_data;
  if (!ctx)
    return;

  pthread_mutex_lock(&ctx->drm_lock);

  uint64_t now_vblank = v4l2_now_ns();
  uint64_t hw_vblank = (uint64_t)tv_sec * 1000000000ULL + (uint64_t)tv_usec * 1000ULL;
  uint64_t vblank_ts = (hw_vblank > 0 && now_vblank >= hw_vblank && (now_vblank - hw_vblank) < 10000000ULL)
                         ? hw_vblank
                         : now_vblank;
  if (ctx->last_vblank_ns > 0 && vblank_ts > ctx->last_vblank_ns) {
    uint64_t dt = vblank_ts - ctx->last_vblank_ns;
    if (dt >= 8000000ULL && dt <= 20000000ULL) {
      ctx->vblank_period_ns = (ctx->vblank_period_ns * 7ULL + dt) / 8ULL;
    }
  }
  ctx->last_vblank_ns = vblank_ts;

#ifdef V4L2_BENCHMARK
  {
    uint64_t t_vblank = now_vblank;
    int idx = ctx->pending_flip_idx;
    if (idx >= 0 && idx < V4L2_MAX_CAPTURE_BUFFERS) {
      ctx->stats.disp_frames++;
      ctx->stats.sess_disp_frames++;

      uint64_t t_dq = ctx->capture_bufs[idx].t_dqbuf_ns;
      uint64_t t_sub = ctx->capture_bufs[idx].t_submit_ns;
      if (t_dq > 0 && t_vblank >= t_dq) {
        uint64_t vb_ns = t_vblank - t_dq;
        ctx->stats.vblank_wait_total_ns += vb_ns;
        ctx->stats.sess_vblank_wait_total_ns += vb_ns;
      }
      if (t_sub > 0 && t_vblank >= t_sub) {
        uint64_t pi_ns = t_vblank - t_sub;
        ctx->stats.total_pi_ns += pi_ns;
        ctx->stats.sess_total_pi_ns += pi_ns;
        if (pi_ns < ctx->stats.total_pi_min_ns)
          ctx->stats.total_pi_min_ns = pi_ns;
        if (pi_ns > ctx->stats.total_pi_max_ns)
          ctx->stats.total_pi_max_ns = pi_ns;
        if (pi_ns < ctx->stats.sess_total_pi_min_ns)
          ctx->stats.sess_total_pi_min_ns = pi_ns;
        if (pi_ns > ctx->stats.sess_total_pi_max_ns)
          ctx->stats.sess_total_pi_max_ns = pi_ns;
      }
    }

    uint64_t elapsed_ns = t_vblank - ctx->stats.window_start_ns;
    if (elapsed_ns >= 1000000000ULL) {
      double win_s = (double)elapsed_ns / 1e9;
      uint32_t rx = atomic_exchange_explicit(&ctx->stats.rx_frames, 0, memory_order_relaxed);
      uint64_t sub_ns = atomic_exchange_explicit(&ctx->stats.submit_total_ns, 0, memory_order_relaxed);
      uint64_t wait_ns = atomic_exchange_explicit(&ctx->stats.submit_wait_ns, 0, memory_order_relaxed);
      uint32_t dec = ctx->stats.dec_frames;
      uint32_t disp = ctx->stats.disp_frames;

      double sub_ms = rx ? ((double)sub_ns / rx) / 1e6 : 0.0;
      double wait_ms = rx ? ((double)wait_ns / rx) / 1e6 : 0.0;
      double hw_ms = dec ? ((double)ctx->stats.hw_dec_total_ns / dec) / 1e6 : 0.0;
      double vb_ms = disp ? ((double)ctx->stats.vblank_wait_total_ns / disp) / 1e6 : 0.0;
      double pi_ms = disp ? ((double)ctx->stats.total_pi_ns / disp) / 1e6 : 0.0;
      double min_ms = (ctx->stats.total_pi_min_ns != UINT64_MAX)
                        ? (double)ctx->stats.total_pi_min_ns / 1e6
                        : 0.0;
      double max_ms = (double)ctx->stats.total_pi_max_ns / 1e6;

      fprintf(stderr,
              "[V4L2 BENCH %.1fs] rx: %.1f fps | dec: %.1f fps | disp: %.1f fps (drop staged:%u burst:%u) | submit: %.2fms (wait:%.2fms) | hw_dec: %.2fms | vblank_wait: %.2fms | total_pi: %.2fms (min:%.2f max:%.2f)\n",
              win_s,
              (double)rx / win_s,
              (double)dec / win_s,
              (double)disp / win_s,
              ctx->stats.staged_drops,
              ctx->stats.burst_drops,
              sub_ms,
              wait_ms,
              hw_ms,
              vb_ms,
              pi_ms,
              min_ms,
              max_ms);

      ctx->stats.dec_frames = 0;
      ctx->stats.disp_frames = 0;
      ctx->stats.staged_drops = 0;
      ctx->stats.burst_drops = 0;
      ctx->stats.hw_dec_total_ns = 0;
      ctx->stats.vblank_wait_total_ns = 0;
      ctx->stats.total_pi_ns = 0;
      ctx->stats.total_pi_min_ns = UINT64_MAX;
      ctx->stats.total_pi_max_ns = 0;
      ctx->stats.window_start_ns = t_vblank;
    }
  }
#endif

  /* 1. Requeue previously displayed buffer to V4L2 decoder */
  if (ctx->current_displayed_idx != -1) {
    v4l2_codec_requeue_capture(ctx, ctx->current_displayed_idx);
  }

  ctx->current_displayed_idx = ctx->pending_flip_idx;
  ctx->pending_flip_idx = -1;
  ctx->flip_pending = false;

  /* 2. Schedule Late-Latch deadline for staged frame instead of committing immediately */
  if (ctx->staged_frame_idx != -1) {
    uint64_t deadline = compute_next_deadline_locked(ctx, now_vblank);
    if (deadline == 0 || now_vblank >= deadline) {
      ctx->next_commit_ns = 0;
      commit_staged_locked(ctx);
    } else {
      ctx->next_commit_ns = deadline;
    }
  }

  pthread_mutex_unlock(&ctx->drm_lock);
}
