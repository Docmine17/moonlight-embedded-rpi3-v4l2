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

#include "v4l2_codec.h"
#include "v4l2_drm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

static int open_bcm2835_decoder_device(void) {
  for (int i = 10; i <= 24; i++) {
    char dev_path[32];
    snprintf(dev_path, sizeof(dev_path), "/dev/video%d", i);

    int fd = open(dev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
      continue;

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
      uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
      uint32_t req = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
      if ((caps & req) == req &&
          strcmp((char *)cap.card, "bcm2835-codec-decode") == 0) {
        return fd;
      }
    }
    close(fd);
  }

  /* Fallback to default decode node */
  return open("/dev/video10", O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

int v4l2_codec_init(struct v4l2_ctx *ctx) {
  for (int i = 0; i < V4L2_MAX_OUTPUT_BUFFERS; i++) {
    ctx->output_bufs[i].start = NULL;
    ctx->output_bufs[i].length = 0;
    ctx->output_bufs[i].export_fd = -1;
    ctx->output_bufs[i].queued = false;
  }
  for (int i = 0; i < V4L2_MAX_CAPTURE_BUFFERS; i++) {
    ctx->capture_bufs[i].export_fd = -1;
    ctx->capture_bufs[i].gem_handle = 0;
    ctx->capture_bufs[i].fb_id = 0;
    ctx->capture_bufs[i].queued = false;
  }

  ctx->v4l2_fd = open_bcm2835_decoder_device();
  if (ctx->v4l2_fd < 0) {
    perror("Can't open bcm2835 decoder device");
    return -1;
  }

  /* 1. Subscribe to Dynamic Resolution Change events */
  struct v4l2_event_subscription sub;
  memset(&sub, 0, sizeof(sub));
  sub.type = V4L2_EVENT_SOURCE_CHANGE;
  if (ioctl(ctx->v4l2_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) != 0) {
    perror("VIDIOC_SUBSCRIBE_EVENT failed");
    goto err_close;
  }

  /* 2. Configure OUTPUT_MPLANE with placeholder geometry to ensure SOURCE_CHANGE fires */
  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  fmt.fmt.pix_mp.width = 32;
  fmt.fmt.pix_mp.height = 32;
  fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
  fmt.fmt.pix_mp.num_planes = 1;
  fmt.fmt.pix_mp.plane_fmt[0].sizeimage = V4L2_OUTPUT_BUFFER_SIZE;

  if (ioctl(ctx->v4l2_fd, VIDIOC_S_FMT, &fmt) != 0) {
    perror("VIDIOC_S_FMT on OUTPUT_MPLANE failed");
    goto err_close;
  }

  /* 3. Allocate short OUTPUT queue (4 buffers) to eliminate queue bufferbloat */
  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = V4L2_MAX_OUTPUT_BUFFERS;
  req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
    perror("VIDIOC_REQBUFS on OUTPUT_MPLANE failed");
    goto err_close;
  }
  ctx->num_output_bufs = (req.count > V4L2_MAX_OUTPUT_BUFFERS)
                           ? V4L2_MAX_OUTPUT_BUFFERS
                           : (int)req.count;

  for (int i = 0; i < ctx->num_output_bufs; i++) {
    struct v4l2_plane planes[1];
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    buf.length = 1;
    buf.m.planes = planes;

    if (ioctl(ctx->v4l2_fd, VIDIOC_QUERYBUF, &buf) != 0) {
      perror("VIDIOC_QUERYBUF on OUTPUT_MPLANE failed");
      goto err_unmap;
    }

    ctx->output_bufs[i].length = buf.m.planes[0].length;
    ctx->output_bufs[i].start = mmap(NULL, buf.m.planes[0].length,
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     ctx->v4l2_fd, buf.m.planes[0].m.mem_offset);
    if (ctx->output_bufs[i].start == MAP_FAILED) {
      ctx->output_bufs[i].start = NULL;
      perror("mmap on OUTPUT_MPLANE failed");
      goto err_unmap;
    }
    ctx->output_bufs[i].queued = false;
  }

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  if (ioctl(ctx->v4l2_fd, VIDIOC_STREAMON, &type) != 0) {
    perror("VIDIOC_STREAMON on OUTPUT_MPLANE failed");
    goto err_unmap;
  }
  ctx->output_streaming = true;

  return 0;

err_unmap:
  for (int i = 0; i < ctx->num_output_bufs; i++) {
    if (ctx->output_bufs[i].start && ctx->output_bufs[i].start != MAP_FAILED) {
      munmap(ctx->output_bufs[i].start, ctx->output_bufs[i].length);
      ctx->output_bufs[i].start = NULL;
    }
  }
  ctx->num_output_bufs = 0;
  struct v4l2_requestbuffers req0;
  memset(&req0, 0, sizeof(req0));
  req0.count = 0;
  req0.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  req0.memory = V4L2_MEMORY_MMAP;
  ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req0);

err_close:
  close(ctx->v4l2_fd);
  ctx->v4l2_fd = -1;
  return -1;
}

void v4l2_codec_destroy(struct v4l2_ctx *ctx) {
  if (ctx->v4l2_fd < 0)
    return;

  /* Send EOS */
  struct v4l2_decoder_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.cmd = V4L2_DEC_CMD_STOP;
  ioctl(ctx->v4l2_fd, VIDIOC_DECODER_CMD, &cmd);

  /* Stop queues in strict order: OUTPUT first, then CAPTURE */
  if (ctx->output_streaming) {
    enum v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ioctl(ctx->v4l2_fd, VIDIOC_STREAMOFF, &out_type);
    ctx->output_streaming = false;
  }

  if (ctx->capture_streaming) {
    enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(ctx->v4l2_fd, VIDIOC_STREAMOFF, &cap_type);
    ctx->capture_streaming = false;
  }

  /* 1. Unmap OUTPUT memory FIRST to satisfy vb2_core_reqbufs check */
  for (int i = 0; i < ctx->num_output_bufs; i++) {
    if (ctx->output_bufs[i].start && ctx->output_bufs[i].start != MAP_FAILED) {
      munmap(ctx->output_bufs[i].start, ctx->output_bufs[i].length);
      ctx->output_bufs[i].start = NULL;
    }
  }

  /* 2. Now request 0 buffers to release kernel memory without -EBUSY */
  struct v4l2_requestbuffers req0;
  memset(&req0, 0, sizeof(req0));
  req0.count = 0;
  req0.memory = V4L2_MEMORY_MMAP;

  req0.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req0);

  req0.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req0);

  close(ctx->v4l2_fd);
  ctx->v4l2_fd = -1;
}

/* Caller must hold ctx->state_mutex; lock is temporarily dropped while polling. */
int v4l2_codec_acquire_output_buffer(struct v4l2_ctx *ctx, int timeout_ms) {
  int waited_ms = 0;

  while (1) {
    if (!atomic_load(&ctx->running) || !atomic_load(&ctx->streaming) ||
        ctx->v4l2_fd < 0 || !ctx->output_streaming) {
      return -ECANCELED;
    }

    /* 1. Drain finished output buffers in a non-blocking loop */
    struct v4l2_plane planes[1];
    struct v4l2_buffer buf;
    while (1) {
      memset(&buf, 0, sizeof(buf));
      memset(planes, 0, sizeof(planes));
      buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.length = 1;
      buf.m.planes = planes;

      if (ioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buf) != 0)
        break;

      if ((int)buf.index < ctx->num_output_bufs) {
        ctx->output_bufs[buf.index].queued = false;
      }
    }

    for (int i = 0; i < ctx->num_output_bufs; i++) {
      if (!ctx->output_bufs[i].queued) {
        return i;
      }
    }

    if (waited_ms >= timeout_ms) {
      return -EBUSY;
    }

    /* Drop state_mutex around poll() so v4l2_cleanup() and other callers are never blocked */
    int fd = ctx->v4l2_fd;
    pthread_mutex_unlock(&ctx->state_mutex);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    poll(&pfd, 1, 3);
    waited_ms += 3;

    pthread_mutex_lock(&ctx->state_mutex);
  }
}

/* Caller must hold ctx->state_mutex. */
int v4l2_codec_queue_output_buffer(struct v4l2_ctx *ctx, int index, size_t bytesused) {
  if (index < 0 || index >= ctx->num_output_bufs ||
      ctx->v4l2_fd < 0 || !ctx->output_streaming) {
    return -EINVAL;
  }

  struct v4l2_plane planes[1];
  struct v4l2_buffer buf;
  memset(&buf, 0, sizeof(buf));
  memset(planes, 0, sizeof(planes));
  buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = index;
  buf.length = 1;
  buf.m.planes = planes;
  buf.m.planes[0].bytesused = bytesused;

  if (ioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf) != 0) {
    perror("VIDIOC_QBUF on OUTPUT_MPLANE failed");
    return -errno;
  }

  ctx->output_bufs[index].queued = true;
  return 0;
}

int v4l2_codec_handle_drc(struct v4l2_ctx *ctx) {
  /* 1. Drain pending page flips and detach DRM plane synchronously */
  v4l2_drm_drain_pending_flip(ctx);

  pthread_mutex_lock(&ctx->drm_lock);
  v4l2_drm_disable_plane_sync(ctx);
  ctx->flip_pending = false;
  ctx->current_displayed_idx = -1;
  ctx->pending_flip_idx = -1;
  ctx->staged_frame_idx = -1;
  pthread_mutex_unlock(&ctx->drm_lock);

  /* 2. If capture queue was active, drain it and stop */
  if (ctx->capture_streaming) {
    struct v4l2_plane planes[1];
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    memset(planes, 0, sizeof(planes));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.length = 1;
    b.m.planes = planes;

    while (ioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &b) == 0) {
      if (b.flags & V4L2_BUF_FLAG_LAST)
        break;
    }

    enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(ctx->v4l2_fd, VIDIOC_STREAMOFF, &cap_type);

    v4l2_drm_release_capture_buffers(ctx);

    struct v4l2_requestbuffers req0;
    memset(&req0, 0, sizeof(req0));
    req0.count = 0;
    req0.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req0.memory = V4L2_MEMORY_MMAP;
    ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req0);

    ctx->capture_streaming = false;
  }

  /* 3. Query format from decoder */
  struct v4l2_format gfmt;
  memset(&gfmt, 0, sizeof(gfmt));
  gfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

  if (ioctl(ctx->v4l2_fd, VIDIOC_G_FMT, &gfmt) != 0) {
    perror("VIDIOC_G_FMT on CAPTURE_MPLANE failed");
    return -1;
  }

  /* Explicitly enforce NV12 pixel format */
  gfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
  if (ioctl(ctx->v4l2_fd, VIDIOC_S_FMT, &gfmt) != 0) {
    perror("VIDIOC_S_FMT NV12 on CAPTURE_MPLANE failed");
    return -1;
  }

  ctx->coded_width = gfmt.fmt.pix_mp.width;
  ctx->coded_height = gfmt.fmt.pix_mp.height;
  ctx->stride = gfmt.fmt.pix_mp.plane_fmt[0].bytesperline;

  /* 4. Query visible crop selection */
  struct v4l2_selection sel;
  memset(&sel, 0, sizeof(sel));
  sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  sel.target = V4L2_SEL_TGT_COMPOSE;

  if (ioctl(ctx->v4l2_fd, VIDIOC_G_SELECTION, &sel) == 0 &&
      sel.r.width > 0 && sel.r.height > 0) {
    ctx->visible_width = sel.r.width;
    ctx->visible_height = sel.r.height;
  } else {
    ctx->visible_width = ctx->coded_width;
    ctx->visible_height = (ctx->coded_height == 1088) ? 1080 : ctx->coded_height;
  }

  v4l2_drm_update_geometry(ctx);

  /* 5. Query min buffers for capture and allocate tight DPB + DRM staging cushion */
  struct v4l2_control ctrl;
  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
  int min_bufs = 2;
  if (ioctl(ctx->v4l2_fd, VIDIOC_G_CTRL, &ctrl) == 0) {
    min_bufs = ctrl.value;
  }

  int req_count = min_bufs + 3; /* DPB refs + scanout + staged + next decode slot */
  if (req_count < 5)
    req_count = 5;
  if (req_count > V4L2_MAX_CAPTURE_BUFFERS)
    req_count = V4L2_MAX_CAPTURE_BUFFERS;

  for (int i = 0; i < V4L2_MAX_CAPTURE_BUFFERS; i++) {
    ctx->capture_bufs[i].export_fd = -1;
    ctx->capture_bufs[i].gem_handle = 0;
    ctx->capture_bufs[i].fb_id = 0;
    ctx->capture_bufs[i].queued = false;
  }

  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = req_count;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
    perror("VIDIOC_REQBUFS on CAPTURE_MPLANE failed");
    return -1;
  }
  ctx->num_capture_bufs = (req.count > V4L2_MAX_CAPTURE_BUFFERS)
                            ? V4L2_MAX_CAPTURE_BUFFERS
                            : (int)req.count;

  /* 6. Export DMABUF, import to DRM, and queue each capture buffer */
  for (int i = 0; i < ctx->num_capture_bufs; i++) {
    struct v4l2_exportbuffer exp;
    memset(&exp, 0, sizeof(exp));
    exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    exp.index = i;
    exp.flags = O_CLOEXEC | O_RDWR;

    if (ioctl(ctx->v4l2_fd, VIDIOC_EXPBUF, &exp) != 0) {
      perror("VIDIOC_EXPBUF failed");
      return -1;
    }
    ctx->capture_bufs[i].export_fd = exp.fd;

    if (v4l2_drm_import_capture_buffer(ctx, i) != 0) {
      fprintf(stderr, "Failed to import capture buffer %d to DRM\n", i);
      return -1;
    }

    struct v4l2_plane p[1];
    struct v4l2_buffer qb;
    memset(&qb, 0, sizeof(qb));
    memset(p, 0, sizeof(p));
    qb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    qb.memory = V4L2_MEMORY_MMAP;
    qb.index = i;
    qb.length = 1;
    qb.m.planes = p;

    if (ioctl(ctx->v4l2_fd, VIDIOC_QBUF, &qb) != 0) {
      perror("VIDIOC_QBUF on CAPTURE_MPLANE failed");
      return -1;
    }
    ctx->capture_bufs[i].queued = true;
  }

  enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (ioctl(ctx->v4l2_fd, VIDIOC_STREAMON, &cap_type) != 0) {
    perror("VIDIOC_STREAMON on CAPTURE_MPLANE failed");
    return -1;
  }
  ctx->capture_streaming = true;

  return 0;
}

void v4l2_codec_requeue_capture(struct v4l2_ctx *ctx, int index) {
  if (index < 0 || index >= ctx->num_capture_bufs || !ctx->capture_streaming)
    return;

  struct v4l2_plane planes[1];
  struct v4l2_buffer buf;
  memset(&buf, 0, sizeof(buf));
  memset(planes, 0, sizeof(planes));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = index;
  buf.length = 1;
  buf.m.planes = planes;

  ioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf);
}
