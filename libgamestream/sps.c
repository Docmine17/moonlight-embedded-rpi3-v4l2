/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
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

#include "sps.h"

#include "h264_stream.h"

static h264_stream_t* h264_stream;
static int initial_width, initial_height;

void gs_sps_init(int width, int height) {
  if (h264_stream) {
    h264_free(h264_stream);
    h264_stream = NULL;
  }
  h264_stream = h264_new();
  initial_width = width;
  initial_height = height;
}

void gs_sps_fix(PLENTRY sps, int flags, uint8_t* out_buf, uint32_t* out_offset) {
  if (!sps || !sps->data || sps->length <= 4) {
    if (sps && sps->data && sps->length > 0) {
      memcpy(out_buf + *out_offset, sps->data, sps->length);
      *out_offset += sps->length;
    }
    return;
  }

  int start_len = sps->data[2] == 0x01 ? 3 : 4;

  if (read_nal_unit(h264_stream, sps->data + start_len, sps->length - start_len) < 0) {
    memcpy(out_buf + *out_offset, sps->data, sps->length);
    *out_offset += sps->length;
    return;
  }

  // Some decoders rely on H264 level to decide how many buffers are needed.
  // Clamp level_idc based on macroblock count and never exceed Level 4.2 (42),
  // as VideoCore IV hardware rejects Level 5.0 streams.
  int total_mbs = ((initial_width + 15) / 16) * ((initial_height + 15) / 16);
  if (total_mbs > 0 && total_mbs <= 3600) {
    h264_stream->sps->level_idc = 32; // Max 5 buffered frames at 1280x720x60
  } else if (total_mbs > 3600 && total_mbs <= 8160) {
    h264_stream->sps->level_idc = 42; // Max 4 buffered frames at 1920x1080x60
  } else if (h264_stream->sps->level_idc > 42) {
    h264_stream->sps->level_idc = 42;
  }

  // Some decoders require a reference frame count of 1 to decode successfully.
  h264_stream->sps->num_ref_frames = 1;

  // GFE 2.5.11 changed the SPS to add additional extensions.
  // Some devices don't like these so we remove them here.
  if (flags & GS_SPS_REMOVE_VST_FIXUP)
    h264_stream->sps->vui.video_signal_type_present_flag = 0;
  if (flags & GS_SPS_REMOVE_CLI_FIXUP)
    h264_stream->sps->vui.chroma_loc_info_present_flag = 0;

  if ((flags & GS_SPS_BITSTREAM_FIXUP) == GS_SPS_BITSTREAM_FIXUP) {
    // Ensure VUI parameters are actually emitted into the SPS RBSP.
    h264_stream->sps->vui_parameters_present_flag = 1;

    if (!h264_stream->sps->vui.bitstream_restriction_flag) {
      h264_stream->sps->vui.bitstream_restriction_flag = 1;
      h264_stream->sps->vui.motion_vectors_over_pic_boundaries_flag = 1;
      h264_stream->sps->vui.log2_max_mv_length_horizontal = 16;
      h264_stream->sps->vui.log2_max_mv_length_vertical = 16;
    }

    // Unconditionally force zero reordering and minimal DPB buffering even when
    // the host encoder already populated bitstream_restriction_flag.
    h264_stream->sps->vui.num_reorder_frames = 0;
    h264_stream->sps->vui.max_dec_frame_buffering = 1;
    h264_stream->sps->vui.max_bytes_per_pic_denom = 2;
    h264_stream->sps->vui.max_bits_per_mb_denom = 1;

    if (h264_stream->sps->vui.nal_hrd_parameters_present_flag ||
        h264_stream->sps->vui.vcl_hrd_parameters_present_flag) {
      h264_stream->sps->vui.low_delay_hrd_flag = 1;
    }
  }

  memcpy(out_buf + *out_offset, sps->data, start_len);
  int nal_len = write_nal_unit(h264_stream, out_buf + *out_offset + start_len, 256);
  if (nal_len > 0) {
    *out_offset += start_len + (uint32_t)nal_len;
  } else {
    memcpy(out_buf + *out_offset, sps->data, sps->length);
    *out_offset += sps->length;
  }
}
