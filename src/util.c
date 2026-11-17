/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Iwan Timmer
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

#include "util.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

int write_bool(char *path, bool val) {
  int fd = open(path, O_RDWR);

  if(fd >= 0) {
    int ret = write(fd, val ? "1" : "0", 1);
    if (ret < 0)
      fprintf(stderr, "Failed to write %d to %s: %d\n", val ? 1 : 0, path, ret);

    close(fd);
    return 0;
  } else
    return -1;
}

int read_file(char *path, char* output, int output_len) {
  int fd = open(path, O_RDONLY);

  if(fd >= 0) {
    output_len = read(fd, output, output_len);
    close(fd);
    return output_len;
  } else
    return -1;
}

bool ensure_buf_size(void **buf, size_t *buf_size, size_t required_size) {
  if (*buf_size >= required_size)
    return false;

  *buf_size = required_size;
  *buf = realloc(*buf, *buf_size);
  if (!*buf) {
    fprintf(stderr, "Failed to allocate %zu bytes\n", *buf_size);
    abort();
  }

  return true;
}

bool configure_realtime_thread(pthread_t thread, int target_priority, uint32_t cpu_mask, const char *role) {
  int min_prio = sched_get_priority_min(SCHED_FIFO);
  int max_prio = sched_get_priority_max(SCHED_FIFO);
  if (min_prio < 0 || max_prio < 0) {
    min_prio = 1;
    max_prio = 99;
  }

  if (target_priority < min_prio)
    target_priority = min_prio;
  if (target_priority > max_prio)
    target_priority = max_prio;

  struct sched_param param;
  memset(&param, 0, sizeof(param));
  param.sched_priority = target_priority;

  int ret = pthread_setschedparam(thread, SCHED_FIFO, &param);
  if (ret != 0) {
    if (ret == EPERM) {
      fprintf(stderr, "RT: Note: SCHED_FIFO for %s thread requires elevated permissions (CAP_SYS_NICE or rtprio limits). Continuing with default scheduler.\n", role);
    } else {
      fprintf(stderr, "RT: Note: pthread_setschedparam (%s) failed: %s (code %d). Continuing with default scheduler.\n", role, strerror(ret), ret);
    }
    return false;
  }

  if (cpu_mask != 0) {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc > 1) {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      int count = 0;
      for (int cpu = 0; cpu < 32 && cpu < nproc; cpu++) {
        if (cpu_mask & (1u << cpu)) {
          CPU_SET(cpu, &cpuset);
          count++;
        }
      }
      if (count > 0 && pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset) != 0) {
        fprintf(stderr, "RT: Note: pthread_setaffinity_np (%s) failed, keeping default affinity\n", role);
      }
    }
  }

  fprintf(stderr, "RT: Enabled SCHED_FIFO (%d) for %s thread\n", target_priority, role);
  return true;
}
