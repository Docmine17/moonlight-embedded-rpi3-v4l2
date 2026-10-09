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

static struct v4l2_ctx v4l2_instance;

static void configure_realtime_thread(pthread_t thread, int target_priority) {
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
    fprintf(stderr, "V4L2: Successfully enabled SCHED_FIFO real-time priority (%d) for display thread\n", target_priority);
  } else if (ret == EPERM) {
    fprintf(stderr, "V4L2: Note: SCHED_FIFO requires elevated permissions (CAP_SYS_NICE or rtprio limits in /etc/security/limits.conf). Continuing with default scheduler.\n");
  } else {
    fprintf(stderr, "V4L2: Note: pthread_setschedparam failed: %s (code %d). Continuing with default scheduler.\n", strerror(ret), ret);
  }
}

static void *v4l2_event_thread(void *arg) {
  struct v4l2_ctx *ctx = (struct v4l2_ctx *)arg;
  struct pollfd pfd[2];

  drmEventContext evctx;
  memset(&evctx, 0, sizeof(evctx));
  evctx.version = DRM_EVENT_CONTEXT_VERSION;
  evctx.page_flip_handler = v4l2_drm_page_flip_handler;

  while (ctx->running) {
    pfd[0].fd = ctx->v4l2_fd;
    pfd[0].events = POLLIN | POLLPRI;
    pfd[0].revents = 0;

    pfd[1].fd = ctx->drm_fd;
    pfd[1].events = POLLIN;
    pfd[1].revents = 0;

    int ret = poll(pfd, 2, 50);
    if (ret <= 0)
      continue;

    /* 1. Handle DRM VBlank page-flip events */
    if (pfd[1].revents & POLLIN) {
      drmHandleEvent(ctx->drm_fd, &evctx);
    }

    /* 2. Handle V4L2 Dynamic Resolution / Format Change */
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

    /* 3. Handle newly decoded frames ready for display (drain all ready frames) */
    if (pfd[0].revents & POLLIN) {
      struct v4l2_plane planes[1];
      struct v4l2_buffer buf;
      memset(&buf, 0, sizeof(buf));
      memset(planes, 0, sizeof(planes));
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.length = 1;
      buf.m.planes = planes;

      while (ioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buf) == 0) {
        v4l2_drm_present(ctx, buf.index);

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = 1;
        buf.m.planes = planes;
      }
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
  v4l2_instance.stream_width = width;
  v4l2_instance.stream_height = height;

  pthread_mutex_init(&v4l2_instance.state_mutex, NULL);

  gs_sps_init(width, height);

  v4l2_instance.bitstream_buf_size = INITIAL_DECODER_BUFFER_SIZE;
  v4l2_instance.bitstream_buf = malloc(v4l2_instance.bitstream_buf_size);
  if (!v4l2_instance.bitstream_buf) {
    fprintf(stderr, "V4L2: Failed to allocate bitstream staging buffer\n");
    return -1;
  }

  if (v4l2_drm_init(&v4l2_instance) != 0) {
    fprintf(stderr, "V4L2: Failed to initialize DRM display engine\n");
    free(v4l2_instance.bitstream_buf);
    pthread_mutex_destroy(&v4l2_instance.state_mutex);
    return -1;
  }

  if (v4l2_codec_init(&v4l2_instance) != 0) {
    fprintf(stderr, "V4L2: Failed to initialize V4L2 M2M decoder\n");
    v4l2_drm_destroy(&v4l2_instance);
    free(v4l2_instance.bitstream_buf);
    pthread_mutex_destroy(&v4l2_instance.state_mutex);
    return -1;
  }

  v4l2_instance.running = true;
  v4l2_instance.streaming = true;

  if (pthread_create(&v4l2_instance.event_thread, NULL, v4l2_event_thread, &v4l2_instance) != 0) {
    perror("V4L2: Failed to create event loop thread");
    v4l2_instance.running = false;
    v4l2_codec_destroy(&v4l2_instance);
    v4l2_drm_destroy(&v4l2_instance);
    free(v4l2_instance.bitstream_buf);
    pthread_mutex_destroy(&v4l2_instance.state_mutex);
    return -1;
  }

  /* Elevate display presentation thread to real-time priority (45: optimal for video display) */
  configure_realtime_thread(v4l2_instance.event_thread, 45);

  return DR_OK;
}

static void v4l2_start(void) {
  pthread_mutex_lock(&v4l2_instance.state_mutex);
  v4l2_instance.streaming = true;
  pthread_mutex_unlock(&v4l2_instance.state_mutex);
}

static void v4l2_stop(void) {
  pthread_mutex_lock(&v4l2_instance.state_mutex);
  v4l2_instance.streaming = false;
  pthread_mutex_unlock(&v4l2_instance.state_mutex);
}

static void v4l2_cleanup(void) {
  pthread_mutex_lock(&v4l2_instance.state_mutex);
  v4l2_instance.running = false;
  v4l2_instance.streaming = false;
  pthread_mutex_unlock(&v4l2_instance.state_mutex);

  if (v4l2_instance.event_thread) {
    pthread_join(v4l2_instance.event_thread, NULL);
    v4l2_instance.event_thread = 0;
  }

  /* 1. Drain pending page flips before releasing buffers */
  v4l2_drm_drain_pending_flip(&v4l2_instance);

  /* 2. Synchronously disable plane scanout */
  v4l2_drm_disable_plane_sync(&v4l2_instance);

  /* 3. Destroy DRM framebuffers, GEM handles, and DMABUF export FDs */
  v4l2_drm_release_capture_buffers(&v4l2_instance);

  /* 4. Destroy V4L2 decoder queues and release kernel buffers */
  v4l2_codec_destroy(&v4l2_instance);

  /* 5. Destroy DRM resources and device */
  v4l2_drm_destroy(&v4l2_instance);

  pthread_mutex_lock(&v4l2_instance.state_mutex);
  if (v4l2_instance.bitstream_buf) {
    free(v4l2_instance.bitstream_buf);
    v4l2_instance.bitstream_buf = NULL;
  }
  pthread_mutex_unlock(&v4l2_instance.state_mutex);

  pthread_mutex_destroy(&v4l2_instance.state_mutex);
}

static int v4l2_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  pthread_mutex_lock(&v4l2_instance.state_mutex);
  if (!v4l2_instance.streaming || !v4l2_instance.running) {
    pthread_mutex_unlock(&v4l2_instance.state_mutex);
    return DR_OK;
  }

  /* Ensure bitstream buffer can hold the entire NAL unit chain */
  ensure_buf_size((void **)&v4l2_instance.bitstream_buf,
                  &v4l2_instance.bitstream_buf_size,
                  decodeUnit->fullLength + 256);

  uint32_t length = 0;
  PLENTRY entry = decodeUnit->bufferList;

  while (entry != NULL) {
    if (entry->bufferType == BUFFER_TYPE_SPS) {
      /* Apply SPS fixup to enforce low-delay DPB queueing */
      gs_sps_fix(entry, GS_SPS_BITSTREAM_FIXUP, v4l2_instance.bitstream_buf, &length);
    } else {
      memcpy(v4l2_instance.bitstream_buf + length, entry->data, entry->length);
      length += entry->length;
    }
    entry = entry->next;
  }

  int ret = v4l2_codec_feed_packet(&v4l2_instance, v4l2_instance.bitstream_buf, length);
  pthread_mutex_unlock(&v4l2_instance.state_mutex);

  if (ret != 0) {
    /* If an individual frame had to be dropped because the decoder was backlogged,
     * do NOT request an IDR keyframe! Requesting an IDR creates an infinite storm
     * of massive keyframes that permanently locks up the decoder pipeline. */
    return DR_OK;
  }

  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_v4l2 = {
  .setup = v4l2_setup,
  .start = v4l2_start,
  .stop = v4l2_stop,
  .cleanup = v4l2_cleanup,
  .submitDecodeUnit = v4l2_submit_decode_unit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SLICES_PER_FRAME(4),
};
