/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
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

#include "audio.h"
#include "../util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus_multistream.h>
#include <pulse/simple.h>
#include <pulse/sample.h>
#include <pulse/error.h>

static OpusMSDecoder* decoder;
static pa_simple *dev = NULL;
static short* pcmBuffer;
static int samplesPerFrame;
static int channelCount;
static bool audio_rt_configured = false;

bool audio_pulse_init(char* audio_device) {
  pa_sample_spec spec = {
    .format = PA_SAMPLE_S16LE,
    .rate = 48000,
    .channels = 2
  };

  int error;
  dev = pa_simple_new(NULL, "Moonlight Embedded", PA_STREAM_PLAYBACK, audio_device, "Streaming", &spec, NULL, NULL, &error);

  if (dev)
    pa_simple_free(dev);

  return (bool) dev;
}

static int pulse_renderer_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  int rc, error;
  unsigned char alsaMapping[AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT];

  channelCount = opusConfig->channelCount;
  samplesPerFrame = opusConfig->samplesPerFrame;
  pcmBuffer = malloc(sizeof(short) * channelCount * samplesPerFrame);
  if (pcmBuffer == NULL)
    return -1;

  /* The supplied mapping array has order: FL-FR-C-LFE-RL-RR-SL-SR
   * ALSA expects the order: FL-FR-RL-RR-C-LFE-SL-SR
   * We need copy the mapping locally and swap the channels around.
   */
  memcpy(alsaMapping, opusConfig->mapping, sizeof(alsaMapping));
  if (opusConfig->channelCount >= 6) {
    alsaMapping[2] = opusConfig->mapping[4];
    alsaMapping[3] = opusConfig->mapping[5];
    alsaMapping[4] = opusConfig->mapping[2];
    alsaMapping[5] = opusConfig->mapping[3];
  }

  decoder = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount, opusConfig->streams, opusConfig->coupledStreams, alsaMapping, &rc);

  pa_sample_spec spec = {
    .format = PA_SAMPLE_S16LE,
    .rate = opusConfig->sampleRate,
    .channels = opusConfig->channelCount
  };

  pa_channel_map map;
  pa_channel_map_init_auto(&map, opusConfig->channelCount, PA_CHANNEL_MAP_ALSA);

  audio_rt_configured = false;

  pa_buffer_attr attr = {
    .maxlength = (uint32_t)pa_usec_to_bytes(80000, &spec),
    .tlength = (uint32_t)pa_usec_to_bytes(40000, &spec),
    .prebuf = (uint32_t)pa_usec_to_bytes(20000, &spec),
    .minreq = (uint32_t)pa_usec_to_bytes(10000, &spec),
    .fragsize = (uint32_t)-1,
  };

  char* audio_device = (char*) context;
  dev = pa_simple_new(NULL, "Moonlight Embedded", PA_STREAM_PLAYBACK, audio_device, "Streaming", &spec, &map, &attr, &error);

  if (!dev) {
    printf("Pulseaudio error: %s\n", pa_strerror(error));
    return -1;
  }

  return 0;
}

static void pulse_renderer_decode_and_play_sample(char* data, int length) {
  if (!audio_rt_configured) {
    audio_rt_configured = true;
    configure_realtime_thread(pthread_self(), 38, 0, "audio");
  }
  int decodeLen = opus_multistream_decode(decoder, data, length, pcmBuffer, samplesPerFrame, 0);
  if (decodeLen > 0) {
    int error;
    int rc = pa_simple_write(dev, pcmBuffer, decodeLen * sizeof(short) * channelCount, &error);

    if (rc<0)
      printf("Pulseaudio error: %s\n", pa_strerror(error));
  } else if (decodeLen < 0) {
    printf("Opus error from decode: %d\n", decodeLen);
  }
}

static void pulse_renderer_cleanup() {
  audio_rt_configured = false;
  if (decoder != NULL) {
    opus_multistream_decoder_destroy(decoder);
    decoder = NULL;
  }
  if (dev != NULL) {
    pa_simple_free(dev);
    dev = NULL;
  }
  if (pcmBuffer != NULL) {
    free(pcmBuffer);
    pcmBuffer = NULL;
  }
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_pulse = {
  .init = pulse_renderer_init,
  .cleanup = pulse_renderer_cleanup,
  .decodeAndPlaySample = pulse_renderer_decode_and_play_sample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
