/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Sunguk Lee
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

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <psp2/rtc.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>
#include <stdlib.h>
#include "../src/gui/mdns_log.h"
#include "debug.h"
#include "config.h"

pthread_mutex_t print_mutex;

bool vita_debug_init() {
  if (pthread_mutex_init(&print_mutex, NULL) != 0) {
    return false;
  }
  return true;
}

void vita_debug_log(const char *s, ...) {
  // Custom build: always log. Upstream gates this behind
  // config.save_debug_log (default off), which leaves no way to diagnose
  // a pairing attempt that never completes.

  pthread_mutex_lock(&print_mutex);

  // rate-limit repetitive recovery messages BEFORE formatting or I/O.
  // All counters stay under the existing mutex; no new worker or allocation.
  static const char* const noisy[] = {
    "Unrecoverable frame", "Network dropped", "Waiting for IDR frame",
    "IDR frame request sent", "Reached consecutive drop limit", "remain frameskip"
  };
  static uint64_t last[6];
  static unsigned suppressed[6];
  unsigned omitted = 0;
  for (unsigned i = 0; i < 6; i++) {
    if (strncmp(s, noisy[i], strlen(noisy[i])) != 0) continue;
    uint64_t now = sceKernelGetSystemTimeWide();
    if (last[i] && now - last[i] < 1000000) {
      suppressed[i]++;
      pthread_mutex_unlock(&print_mutex);
      return;
    }
    last[i] = now;
    omitted = suppressed[i];
    suppressed[i] = 0;
    break;
  }

  // Static, not malloc: the call is already serialised by
  // print_mutex, and vita.c maps printf -> vita_debug_log, so this
  // ran an 8 KiB allocation on the video path.
  static char log_buffer[8192];
  char* buffer = log_buffer;

  SceDateTime time;
  sceRtcGetCurrentClock(&time, 0);

  snprintf(buffer, 26, "%04d%02d%02d %02d:%02d:%02d.%06d ",
           time.year, time.month, time.day,
           time.hour, time.minute, time.second,
           time.microsecond);

  va_list va;
  va_start(va, s);
  int len = vsnprintf(&buffer[25], 8000, s, va);
  va_end(va);
  // vsnprintf returns the required length, not the number actually written.
  if (len < 0) len = 0;
  if (len > 7999) len = 7999;
  buffer[25 + len] = '\0';
  if (omitted) {
    size_t end = strlen(buffer);
    if (end && buffer[end - 1] == '\n') buffer[--end] = '\0';
    snprintf(buffer + end, sizeof(log_buffer) - end,
             " [suppressed=%u]", omitted);
    len = (int)strlen(buffer) - 25;
  }

  if (config.log_file) {
    fprintf(config.log_file, "%s", buffer);
    if (buffer[len + 24] != '\n') {
        fprintf(config.log_file, "\n");
    }
    // Batched: fflush() is a synchronous storage write, and the
    // frame pacer logs a line per dropped frame. Flush every
    // 32 lines so a crash still leaves a usable tail.
    static unsigned flush_count = 0;
    if (++flush_count >= 32) {
      fflush(config.log_file);
      flush_count = 0;
    }
  } else {
    // Si no se pudo abrir el log, mostrar por pantalla
    printf("[Moonlight] No se pudo abrir el archivo de log. Mensaje: %s\n", &buffer[25]);
  }

#ifdef __vita__
  // También imprimir por mdns_log (sceClibPrintf)
  mdns_log("%s", &buffer[25]);
#endif

  pthread_mutex_unlock(&print_mutex);
}

