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

#ifndef VIDEO_V4L2_DRM_H
#define VIDEO_V4L2_DRM_H

#include "v4l2.h"

int v4l2_drm_init(struct v4l2_ctx *ctx);
void v4l2_drm_destroy(struct v4l2_ctx *ctx);

int v4l2_drm_import_capture_buffer(struct v4l2_ctx *ctx, int index);
void v4l2_drm_release_capture_buffers(struct v4l2_ctx *ctx);

void v4l2_drm_update_geometry(struct v4l2_ctx *ctx);
void v4l2_drm_present(struct v4l2_ctx *ctx, int new_buf_idx);
void v4l2_drm_disable_plane_sync(struct v4l2_ctx *ctx);
void v4l2_drm_drain_pending_flip(struct v4l2_ctx *ctx);

void v4l2_drm_page_flip_handler(int fd, unsigned int seq, unsigned int tv_sec,
                                unsigned int tv_usec, void *user_data);

#endif /* VIDEO_V4L2_DRM_H */
