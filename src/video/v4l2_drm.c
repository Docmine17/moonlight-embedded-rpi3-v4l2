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

static int get_property_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name, uint32_t *prop_id) {
  drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
  if (!props)
    return -1;

  for (uint32_t i = 0; i < props->count_props; i++) {
    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop)
      continue;

    if (strcmp(prop->name, name) == 0) {
      *prop_id = prop->prop_id;
      drmModeFreeProperty(prop);
      drmModeFreeObjectProperties(props);
      return 0;
    }
    drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);
  return -1;
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

  drmModeResPtr res = drmModeGetResources(ctx->drm_fd);
  if (!res) {
    perror("drmModeGetResources failed");
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
    return -1;
  }

  drmModeConnectorPtr conn = NULL;
  for (int i = 0; i < res->count_connectors; i++) {
    conn = drmModeGetConnector(ctx->drm_fd, res->connectors[i]);
    if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
      break;

    if (conn) {
      drmModeFreeConnector(conn);
      conn = NULL;
    }
  }

  if (!conn) {
    fprintf(stderr, "No connected DRM connector found\n");
    drmModeFreeResources(res);
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
    return -1;
  }

  ctx->conn_id = conn->connector_id;

  drmModeEncoderPtr enc = NULL;
  for (int i = 0; i < res->count_encoders; i++) {
    enc = drmModeGetEncoder(ctx->drm_fd, res->encoders[i]);
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

  if (crtc_id == 0 && res->count_crtcs > 0) {
    crtc_id = res->crtcs[0];
  }

  ctx->crtc_id = crtc_id;

  drmModeCrtcPtr crtc = drmModeGetCrtc(ctx->drm_fd, ctx->crtc_id);
  if (crtc) {
    ctx->crtc_width = crtc->width;
    ctx->crtc_height = crtc->height;
    drmModeFreeCrtc(crtc);
  } else if (conn->count_modes > 0) {
    ctx->crtc_width = conn->modes[0].hdisplay;
    ctx->crtc_height = conn->modes[0].vdisplay;
  } else {
    ctx->crtc_width = 1920;
    ctx->crtc_height = 1080;
  }

  drmModeFreeConnector(conn);

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

  /* Find an overlay or primary plane connected to this CRTC supporting NV12 */
  drmModePlaneResPtr plane_res = drmModeGetPlaneResources(ctx->drm_fd);
  if (!plane_res) {
    perror("drmModeGetPlaneResources failed");
    drmModeFreeResources(res);
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
    return -1;
  }

  uint32_t selected_plane = 0;
  for (uint32_t i = 0; i < plane_res->count_planes; i++) {
    drmModePlanePtr plane = drmModeGetPlane(ctx->drm_fd, plane_res->planes[i]);
    if (!plane)
      continue;

    /* Verify plane can connect to our chosen CRTC */
    if (!(plane->possible_crtcs & (1 << crtc_index))) {
      drmModeFreePlane(plane);
      continue;
    }

    for (uint32_t j = 0; j < plane->count_formats; j++) {
      if (plane->formats[j] == DRM_FORMAT_NV12) {
        selected_plane = plane->plane_id;
        break;
      }
    }
    drmModeFreePlane(plane);
    if (selected_plane != 0)
      break;
  }

  drmModeFreePlaneResources(plane_res);
  drmModeFreeResources(res);

  if (selected_plane == 0) {
    fprintf(stderr, "No DRM plane supporting DRM_FORMAT_NV12 found for CRTC %u\n", ctx->crtc_id);
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
    return -1;
  }

  ctx->plane_id = selected_plane;

  /* Cache atomic plane properties */
  if (get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &ctx->prop_fb_id) != 0 ||
      get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &ctx->prop_crtc_id) != 0 ||
      get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &ctx->prop_crtc_x) != 0 ||
      get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &ctx->prop_crtc_y) != 0 ||
      get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &ctx->prop_crtc_w) != 0 ||
      get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &ctx->prop_crtc_h) != 0 ||
      get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &ctx->prop_src_x) != 0 ||
      get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &ctx->prop_src_y) != 0 ||
      get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &ctx->prop_src_w) != 0 ||
      get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &ctx->prop_src_h) != 0) {
    fprintf(stderr, "V4L2 DRM: Required atomic properties missing on plane %u\n", ctx->plane_id);
    close(ctx->drm_fd);
    ctx->drm_fd = -1;
    return -1;
  }

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
    v4l2_drm_disable_plane_sync(ctx);
    v4l2_drm_release_capture_buffers(ctx);
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
  drmModeAtomicReqPtr req = drmModeAtomicAlloc();
  if (!req)
    return -1;

  uint32_t fb_id = ctx->capture_bufs[buf_idx].fb_id;

  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_fb_id, fb_id);
  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_id, ctx->crtc_id);
  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_x, ctx->dst_x);
  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_y, ctx->dst_y);
  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_w, ctx->dst_w);
  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_h, ctx->dst_h);
  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_x, 0);
  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_y, 0);
  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_w, ctx->visible_width << 16);
  drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_h, ctx->visible_height << 16);

  uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
  int ret = drmModeAtomicCommit(ctx->drm_fd, req, flags, ctx);
  if (ret != 0) {
    static int flip_err_count = 0;
    if (flip_err_count++ < 10) {
      fprintf(stderr, "V4L2 DRM: drmModeAtomicCommit failed: %d (%s)\n", ret, strerror(errno));
    }
  }
  drmModeAtomicFree(req);

  return ret;
}

void v4l2_drm_present(struct v4l2_ctx *ctx, int new_buf_idx) {
  pthread_mutex_lock(&ctx->drm_lock);

  if (ctx->flip_pending) {
    if (ctx->staged_frame_idx != -1) {
      /* Drop older staged frame to maintain lowest latency */
      v4l2_codec_requeue_capture(ctx, ctx->staged_frame_idx);
    }
    ctx->staged_frame_idx = new_buf_idx;
    pthread_mutex_unlock(&ctx->drm_lock);
    return;
  }

  ctx->flip_pending = true;
  ctx->pending_flip_idx = new_buf_idx;

  int ret = commit_atomic_flip(ctx, new_buf_idx);
  if (ret != 0) {
    /* If commit failed, reset pending state and requeue */
    ctx->flip_pending = false;
    ctx->pending_flip_idx = -1;
    v4l2_codec_requeue_capture(ctx, new_buf_idx);
  }

  pthread_mutex_unlock(&ctx->drm_lock);
}

void v4l2_drm_drain_pending_flip(struct v4l2_ctx *ctx) {
  if (ctx->drm_fd < 0)
    return;

  drmEventContext evctx;
  memset(&evctx, 0, sizeof(evctx));
  evctx.version = DRM_EVENT_CONTEXT_VERSION;
  evctx.page_flip_handler = v4l2_drm_page_flip_handler;

  while (ctx->flip_pending) {
    struct pollfd pfd;
    pfd.fd = ctx->drm_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ret = poll(&pfd, 1, 100);
    if (ret > 0 && (pfd.revents & POLLIN)) {
      drmHandleEvent(ctx->drm_fd, &evctx);
    } else {
      ctx->flip_pending = false;
      break;
    }
  }
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
}

void v4l2_drm_page_flip_handler(int fd, unsigned int seq, unsigned int tv_sec,
                                unsigned int tv_usec, void *user_data) {
  struct v4l2_ctx *ctx = (struct v4l2_ctx *)user_data;
  if (!ctx)
    return;

  pthread_mutex_lock(&ctx->drm_lock);

  /* 1. Requeue previously displayed buffer to V4L2 decoder */
  if (ctx->current_displayed_idx != -1) {
    v4l2_codec_requeue_capture(ctx, ctx->current_displayed_idx);
  }

  ctx->current_displayed_idx = ctx->pending_flip_idx;
  ctx->flip_pending = false;

  /* 2. If a newer frame arrived during VBlank wait, commit it immediately */
  if (ctx->staged_frame_idx != -1) {
    int next_idx = ctx->staged_frame_idx;
    ctx->staged_frame_idx = -1;
    ctx->flip_pending = true;
    ctx->pending_flip_idx = next_idx;

    if (commit_atomic_flip(ctx, next_idx) != 0) {
      ctx->flip_pending = false;
      ctx->pending_flip_idx = -1;
      v4l2_codec_requeue_capture(ctx, next_idx);
    }
  }

  pthread_mutex_unlock(&ctx->drm_lock);
}
