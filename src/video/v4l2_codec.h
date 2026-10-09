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

#ifndef VIDEO_V4L2_CODEC_H
#define VIDEO_V4L2_CODEC_H

#include "v4l2.h"

int v4l2_codec_init(struct v4l2_ctx *ctx);
void v4l2_codec_destroy(struct v4l2_ctx *ctx);

int v4l2_codec_feed_packet(struct v4l2_ctx *ctx, const uint8_t *data, size_t length);
int v4l2_codec_handle_drc(struct v4l2_ctx *ctx);
void v4l2_codec_requeue_capture(struct v4l2_ctx *ctx, int index);

#endif /* VIDEO_V4L2_CODEC_H */
