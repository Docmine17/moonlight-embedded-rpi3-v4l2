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

#define _GNU_SOURCE

#include "v4l2.h"
#include "v4l2_drm.h"
#include "v4l2_codec.h"
#include "../util.h"

#include <sps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sched.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif

static struct v4l2_ctx v4l2_instance;

static void configure_realtime_thread(pthread_t thread, int target_priority, const char *role) {
  int min_prio = sched_get_priority_min(SCHED_FIFO);
  int max_prio = sched_get_priority_max(SCHED_FIFO);
  if (min_prio < 0 || max_prio < 0) {
    min_prio = 1;
    max_prio = 99;
  }

  /* Clamp priority safely within system bounds */
  if (target_priority < min_prio) target_priority = min_prio;
  if (target_priority > max_prio) target_priority = max_prio;

  struct sched_param param;
  memset(&param, 0, sizeof(param));
  param.sched_priority = target_priority;

  int ret = pthread_setschedparam(thread, SCHED_FIFO, &param);
  if (ret == 0) {
    fprintf(stderr, "V4L2: Successfully enabled SCHED_FIFO real-time priority (%d) for %s thread\n", target_priority, role);
  } else if (ret == EPERM) {
    fprintf(stderr, "V4L2: Note: SCHED_FIFO for %s thread requires elevated permissions (CAP_SYS_NICE or rtprio limits in /etc/security/limits.conf). Continuing with default scheduler.\n", role);
  } else {
    fprintf(stderr, "V4L2: Note: pthread_setschedparam (%s) failed: %s (code %d). Continuing with default scheduler.\n", role, strerror(ret), ret);
  }
}

static void *v4l2_event_thread(void *arg) {
  struct v4l2_ctx *ctx = (struct v4l2_ctx *)arg;
  struct pollfd pfd[2];

  drmEventContext evctx;
  memset(&evctx, 0, sizeof(evctx));
  evctx.version = DRM_EVENT_CONTEXT_VERSION;
  evctx.page_flip_handler = v4l2_drm_page_flip_handler;

  while (atomic_load(&ctx->running)) {
    pfd[0].fd = ctx->v4l2_fd;
    pfd[0].events = POLLIN | POLLPRI;
    pfd[0].revents = 0;

    pfd[1].fd = ctx->drm_fd;
    pfd[1].events = POLLIN;
    pfd[1].revents = 0;

    int ret = poll(pfd, 2, 50);
    if (ret <= 0)
      continue;

    /* 1. Handle V4L2 Dynamic Resolution / Format Change */
    if (pfd[0].revents & POLLPRI) {
      struct v4l2_event ev;
      memset(&ev, 0, sizeof(ev));
      if (ioctl(ctx->v4l2_fd, VIDIOC_DQEVENT, &ev) == 0) {
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
          v4l2_codec_handle_drc(ctx);
          continue; /* Re-poll cleanly on the newly reallocated queue */
        }
      }
    }

    /* 2. Drain decoded capture frames before VBlank so freshest frame stages in time */
    if (pfd[0].revents & POLLIN) {
      int newest_idx = -1;
      struct v4l2_plane planes[1];
      struct v4l2_buffer buf;
      memset(&buf, 0, sizeof(buf));
      memset(planes, 0, sizeof(planes));
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.length = 1;
      buf.m.planes = planes;

      while (ioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buf) == 0) {
        if ((buf.flags & V4L2_BUF_FLAG_ERROR) || buf.m.planes[0].bytesused == 0) {
          if (!(buf.flags & V4L2_BUF_FLAG_LAST)) {
            v4l2_codec_requeue_capture(ctx, buf.index);
          }
        } else {
          if (newest_idx != -1) {
            v4l2_codec_requeue_capture(ctx, newest_idx);
          }
          newest_idx = (int)buf.index;
        }

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = 1;
        buf.m.planes = planes;
      }

      if (newest_idx != -1) {
        v4l2_drm_present(ctx, newest_idx);
      }
    }

    /* 3. Handle DRM VBlank page-flip events */
    if (pfd[1].revents & POLLIN) {
      drmHandleEvent(ctx->drm_fd, &evctx);
    }
  }

  return NULL;
}

static int v4l2_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  if (videoFormat != VIDEO_FORMAT_H264) {
    fprintf(stderr, "V4L2: Only H.264 video format is supported on Raspberry Pi 3B\n");
    return -1;
  }

  memset(&v4l2_instance, 0, sizeof(v4l2_instance));
  v4l2_instance.v4l2_fd = -1;
  v4l2_instance.drm_fd = -1;
  for (int i = 0; i < V4L2_MAX_OUTPUT_BUFFERS; i++)
    v4l2_instance.output_bufs[i].export_fd = -1;
  for (int i = 0; i < V4L2_MAX_CAPTURE_BUFFERS; i++)
    v4l2_instance.capture_bufs[i].export_fd = -1;

  v4l2_instance.stream_width = width;
  v4l2_instance.stream_height = height;

  if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) == 0) {
    fprintf(stderr, "V4L2: Locked process memory pages (MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)\n");
  } else if (mlockall(MCL_CURRENT) == 0) {
    fprintf(stderr, "V4L2: Locked current process memory pages (MCL_CURRENT)\n");
  } else {
    fprintf(stderr, "V4L2: Note: mlockall failed: %s (CAP_IPC_LOCK or memlock limit required)\n", strerror(errno));
  }

  pthread_mutex_init(&v4l2_instance.state_mutex, NULL);

  gs_sps_init(width, height);

  if (v4l2_drm_init(&v4l2_instance) != 0) {
    fprintf(stderr, "V4L2: Failed to initialize DRM display engine\n");
    pthread_mutex_destroy(&v4l2_instance.state_mutex);
    return -1;
  }

  if (v4l2_codec_init(&v4l2_instance) != 0) {
    fprintf(stderr, "V4L2: Failed to initialize V4L2 M2M decoder\n");
    v4l2_drm_destroy(&v4l2_instance);
    pthread_mutex_destroy(&v4l2_instance.state_mutex);
    return -1;
  }

  atomic_store(&v4l2_instance.running, true);
  atomic_store(&v4l2_instance.streaming, true);

  if (pthread_create(&v4l2_instance.event_thread, NULL, v4l2_event_thread, &v4l2_instance) != 0) {
    perror("V4L2: Failed to create event loop thread");
    atomic_store(&v4l2_instance.running, false);
    atomic_store(&v4l2_instance.streaming, false);
    v4l2_codec_destroy(&v4l2_instance);
    v4l2_drm_destroy(&v4l2_instance);
    pthread_mutex_destroy(&v4l2_instance.state_mutex);
    return -1;
  }

  /* Elevate display presentation thread to real-time priority (45: optimal for video display) */
  configure_realtime_thread(v4l2_instance.event_thread, 45, "display");

  return DR_OK;
}

static void v4l2_start(void) {
  atomic_store(&v4l2_instance.streaming, true);
}

static void v4l2_stop(void) {
  atomic_store(&v4l2_instance.streaming, false);
}

static void v4l2_cleanup(void) {
  atomic_store(&v4l2_instance.running, false);
  atomic_store(&v4l2_instance.streaming, false);

  if (v4l2_instance.event_thread) {
    pthread_join(v4l2_instance.event_thread, NULL);
    v4l2_instance.event_thread = 0;
  }

  /* 1. Drain pending page flips before releasing buffers */
  v4l2_drm_drain_pending_flip(&v4l2_instance);

  /* 2. Synchronously restore console plane scanout (or disable if none was saved) */
  v4l2_drm_restore_plane_sync(&v4l2_instance);

  /* 3. Destroy DRM framebuffers, GEM handles, and DMABUF export FDs */
  v4l2_drm_release_capture_buffers(&v4l2_instance);

  /* 4. Destroy V4L2 decoder queues and release kernel buffers under state_mutex
   *    so any unblocked submit thread waking from poll() sees v4l2_fd < 0 cleanly. */
  pthread_mutex_lock(&v4l2_instance.state_mutex);
  v4l2_codec_destroy(&v4l2_instance);
  pthread_mutex_unlock(&v4l2_instance.state_mutex);

  /* 5. Destroy DRM resources and device */
  v4l2_drm_destroy(&v4l2_instance);

  pthread_mutex_destroy(&v4l2_instance.state_mutex);
}

static int v4l2_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  if (!atomic_load(&v4l2_instance.streaming) || !atomic_load(&v4l2_instance.running)) {
    return DR_OK;
  }

  pthread_mutex_lock(&v4l2_instance.state_mutex);
  if (!atomic_load(&v4l2_instance.streaming) || !atomic_load(&v4l2_instance.running)) {
    pthread_mutex_unlock(&v4l2_instance.state_mutex);
    return DR_OK;
  }

  if (!v4l2_instance.submit_rt_configured) {
    v4l2_instance.submit_rt_configured = true;
    configure_realtime_thread(pthread_self(), 40, "submit");
  }

  /* Allow extra wait during initial DRC or IDR frames so the first keyframe isn't dropped */
  int timeout_ms = (decodeUnit->frameType == FRAME_TYPE_IDR || !v4l2_instance.capture_streaming) ? 51 : 12;
  int buf_idx = v4l2_codec_acquire_output_buffer(&v4l2_instance, timeout_ms);
  if (buf_idx < 0) {
    pthread_mutex_unlock(&v4l2_instance.state_mutex);
    /* Request IDR only when an IDR itself could not be queued; avoid IDR storms on transient P-frame drops */
    return (decodeUnit->frameType == FRAME_TYPE_IDR) ? DR_NEED_IDR : DR_OK;
  }

  uint8_t *dst = (uint8_t *)v4l2_instance.output_bufs[buf_idx].start;
  size_t cap = v4l2_instance.output_bufs[buf_idx].length;
  if (!dst) {
    pthread_mutex_unlock(&v4l2_instance.state_mutex);
    return DR_NEED_IDR;
  }

  uint32_t length = 0;
  PLENTRY entry = decodeUnit->bufferList;

  while (entry != NULL) {
    if (entry->bufferType == BUFFER_TYPE_SPS) {
      if ((size_t)length + (size_t)entry->length + 256 > cap) {
        fprintf(stderr, "V4L2: SPS unit exceeds output buffer capacity (%zu > %zu)\n",
                (size_t)length + (size_t)entry->length + 256, cap);
        pthread_mutex_unlock(&v4l2_instance.state_mutex);
        return DR_NEED_IDR;
      }
      /* Apply SPS fixup directly into V4L2 OUTPUT mmap buffer */
      gs_sps_fix(entry, GS_SPS_BITSTREAM_FIXUP, dst, &length);
    } else {
      if ((size_t)length + (size_t)entry->length > cap) {
        fprintf(stderr, "V4L2: Bitstream unit exceeds output buffer capacity (%zu > %zu)\n",
                (size_t)length + (size_t)entry->length, cap);
        pthread_mutex_unlock(&v4l2_instance.state_mutex);
        return DR_NEED_IDR;
      }
      memcpy(dst + length, entry->data, entry->length);
      length += entry->length;
    }
    entry = entry->next;
  }

  int ret = v4l2_codec_queue_output_buffer(&v4l2_instance, buf_idx, length);
  pthread_mutex_unlock(&v4l2_instance.state_mutex);

  if (ret != 0) {
    return (decodeUnit->frameType == FRAME_TYPE_IDR || ret == -EINVAL) ? DR_NEED_IDR : DR_OK;
  }

  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_v4l2 = {
  .setup = v4l2_setup,
  .start = v4l2_start,
  .stop = v4l2_stop,
  .cleanup = v4l2_cleanup,
  .submitDecodeUnit = v4l2_submit_decode_unit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SLICES_PER_FRAME(1),
};
