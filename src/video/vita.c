/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2016 Ilya Zhuravlev
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

#include "../video.h"
#include "../config.h"
#include "../debug.h"
#include "../gui/guilib.h"
#include "../util.h"
#include "../input/vita.h"
#include "vita.h"
#include "sps.h"

#include <Limelight.h>

#include <stdbool.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/display.h>
#include <psp2/videodec.h>
#include <psp2/power.h>
#include <vita2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define printf vita_debug_log

extern void gs_sps_stop();

extern double_click_tracker dc_tracker;

void draw_streaming(vita2d_texture *frame_texture);
void draw_fps();
void draw_indicators();

enum {
  VITA_VIDEO_INIT_OK                    = 0,
  VITA_VIDEO_ERROR_NO_MEM               = 0x80010001,
  VITA_VIDEO_ERROR_INIT_LIB             = 0x80010002,
  VITA_VIDEO_ERROR_QUERY_DEC_MEMSIZE    = 0x80010003,
  VITA_VIDEO_ERROR_ALLOC_MEM            = 0x80010004,
  VITA_VIDEO_ERROR_GET_MEMBASE          = 0x80010005,
  VITA_VIDEO_ERROR_CREATE_DEC           = 0x80010006,
  VITA_VIDEO_ERROR_CREATE_PACER_THREAD  = 0x80010007,
};

#define DECODER_BUFFER_SIZE (128 * 1024)

#define AV_INPUT_BUFFER_PADDING_SIZE 64

static char* decoder_buffer = NULL;

static size_t decoder_buffer_size = 0;

enum {
  SCREEN_WIDTH = 960,
  SCREEN_HEIGHT = 544,
  LINE_SIZE = 960,
  FRAMEBUFFER_SIZE = 2 * 1024 * 1024,
  FRAMEBUFFER_ALIGNMENT = 256 * 1024
};

enum VideoStatus {
  NOT_INIT,
  INIT_GS,
  INIT_FRAMEBUFFER,
  INIT_AVC_LIB,
  INIT_DECODER_MEMBLOCK,
  INIT_AVC_DEC,
  INIT_FRAME_PACER_THREAD,
};

// sceAvcdecDecode() writes straight into the texture that the GPU
// sampled one frame ago, so a single target forces the CPU to block
// on sceGxmFinish() before it may reuse it. Alternating between two
// targets removes that dependency: the decoder always writes the
// texture the GPU is NOT currently reading.
vita2d_texture *frame_textures[2] = {NULL, NULL};
int frame_tex_idx = 0;
vita2d_texture *frame_texture = NULL;
enum VideoStatus video_status = NOT_INIT;

SceAvcdecCtrl *decoder = NULL;
SceUID displayblock = -1;
SceUID decoderblock = -1;
SceUID pacer_thread = -1;
SceVideodecQueryInitInfoHwAvcdec *init = NULL;
SceAvcdecQueryDecoderInfo *decoder_info = NULL;

typedef struct {
  bool activated;
  uint8_t alpha;
  bool plus;
} indicator_status;

//static unsigned numframes;
static bool active_video_thread = true;
static bool active_pacer_thread = false;
static indicator_status poor_net_indicator = {0};

uint32_t frame_count = 0;
uint32_t need_drop = 0;
uint32_t curr_fps[2] = {0, 0};
float carry = 0;

typedef struct {
  unsigned int texture_width;
  unsigned int texture_height;
  float origin_x;
  float origin_y;
  float region_x1;
  float region_y1;
  float region_x2;
  float region_y2;
} image_scaling_settings;

static image_scaling_settings image_scaling = {0};

void update_scaling_settings(int width, int height) {
  image_scaling.texture_width = SCREEN_WIDTH;
  image_scaling.texture_height = SCREEN_HEIGHT;
  image_scaling.origin_x = 0;
  image_scaling.origin_y = 0;
  image_scaling.region_x1 = 0;
  image_scaling.region_y1 = 0;
  image_scaling.region_x2 = image_scaling.texture_width;
  image_scaling.region_y2 = image_scaling.texture_height;

  double scaled_width = (double) SCREEN_HEIGHT * width / height;
  double scaled_height = (double) SCREEN_WIDTH * height / width;

  if (SCREEN_WIDTH * height == SCREEN_HEIGHT * width) {
    // streaming resolution ratio matches Vita's screen ratio
    // use default setting
  } else if (SCREEN_WIDTH * height > SCREEN_HEIGHT * width) {
    // host ratio example: 4:3, 16:10
    // Vita ratio range: 2:16 (64 x 544) - native (960 x 544)
    if (config.center_region_only) {
      image_scaling.texture_height = VITA_DECODER_RESOLUTION(scaled_height);
      image_scaling.region_y1 = VITA_DECODER_RESOLUTION((scaled_height - SCREEN_HEIGHT) / 2);
      image_scaling.region_y2 = VITA_DECODER_RESOLUTION((scaled_height + SCREEN_HEIGHT) / 2);
    } else {
      image_scaling.texture_width = VITA_DECODER_RESOLUTION(scaled_width);
      image_scaling.region_x2 = VITA_DECODER_RESOLUTION(scaled_width);
      image_scaling.origin_x = round((double) (SCREEN_WIDTH - image_scaling.texture_width) / 2);
    }
  } else {
    // host ratio example: 16:9, 21:9, 32:9
    // Vita ratio range: native (960 x 544) - 15:1 (960 x 64)
    if (config.center_region_only) {
      image_scaling.texture_width = VITA_DECODER_RESOLUTION(scaled_width);
      image_scaling.region_x1 = VITA_DECODER_RESOLUTION((scaled_width - SCREEN_WIDTH) / 2);
      image_scaling.region_x2 = VITA_DECODER_RESOLUTION((scaled_width + SCREEN_WIDTH) / 2);
    } else {
      image_scaling.texture_height = VITA_DECODER_RESOLUTION(scaled_height);
      image_scaling.region_y2 = VITA_DECODER_RESOLUTION(scaled_height);
      image_scaling.origin_y = round((double) (SCREEN_HEIGHT - image_scaling.texture_height) / 2);
    }
  }

  printf("update_scaling_settings: width = %u\n", width);
  printf("update_scaling_settings: height = %u\n", height);
  printf("update_scaling_settings: scaled_width = %f\n", scaled_width);
  printf("update_scaling_settings: scaled_height = %f\n", scaled_height);
  printf("update_scaling_settings: image_scaling.texture_width = %u\n", image_scaling.texture_width);
  printf("update_scaling_settings: image_scaling.texture_height = %u\n", image_scaling.texture_height);
  printf("update_scaling_settings: image_scaling.origin_x = %f\n", image_scaling.origin_x);
  printf("update_scaling_settings: image_scaling.origin_y = %f\n", image_scaling.origin_y);
  printf("update_scaling_settings: image_scaling.region_x1 = %f\n", image_scaling.region_x1);
  printf("update_scaling_settings: image_scaling.region_y1 = %f\n", image_scaling.region_y1);
  printf("update_scaling_settings: image_scaling.region_x2 = %f\n", image_scaling.region_x2);
  printf("update_scaling_settings: image_scaling.region_y2 = %f\n", image_scaling.region_y2);
}

static int vita_pacer_thread_main(SceSize args, void *argp) {
  // 250 ms tick. A 1 s window lets overshoot pile up into a
  // 20-frame backlog that is paid back as a visible freeze;
  // a short window keeps the backlog to one or two frames.
  const int wait = 250000;
  const uint32_t drop_cap = 2;
  uint32_t second_frames = 0;
  uint32_t second_ticks = 0;
  //float max_fps = 0;
  //sceDisplayGetRefreshRate(&max_fps);
  //if (config.stream.fps == 30) {
  //  max_fps /= 2;
  //}
  int max_fps = config.stream.fps;
  //uint64_t last_vblank_count = sceDisplayGetVcount();
  uint64_t last_check_time = sceKernelGetSystemTimeWide();
  //float carry = 0;
  need_drop = 0;
  frame_count = 0;
  while (active_pacer_thread) {
    //uint64_t curr_vblank_count = sceDisplayGetVcount();
    //uint32_t vblank_fps = curr_vblank_count - last_vblank_count;
    uint32_t curr_frame_count = frame_count;
    frame_count = 0;

    if (!active_video_thread) {
    //  carry = 0;
    } else {
      // expected frames in this window, not a whole second's worth
      uint32_t expected = (uint32_t)((uint64_t)max_fps * wait / 1000000);
      if (expected == 0) {
        expected = 1;
      }
      if (config.enable_frame_pacer && curr_frame_count > expected) {
        need_drop += curr_frame_count - expected;
        // Never bank more than a couple of frames: dropping a
        // burst is worse than briefly running over budget.
        if (need_drop > drop_cap) {
          need_drop = drop_cap;
        }
      }
      //vita_debug_log("fps0/fps1/carry/need_drop: %u/%u/%f/%u\n",
      //               curr_frame_count, vblank_fps, carry, need_drop);
    }

    // the on-screen counter still has to read frames per second
    second_frames += curr_frame_count;
    if (++second_ticks >= (1000000 / wait)) {
      curr_fps[0] = second_frames;
      second_frames = 0;
      second_ticks = 0;
      if (config.log_gpu_clock) {
        vita_debug_log("clk gpu=%d xbar=%d bus=%d arm=%d | fps=%u/%u | drop=%u\n",
                       scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency(),
                       scePowerGetBusClockFrequency(), scePowerGetArmClockFrequency(),
                       curr_fps[0], curr_fps[1], need_drop);
      }
    }
    curr_fps[1] = max_fps;

    //last_vblank_count = curr_vblank_count;
    uint64_t curr_check_time = sceKernelGetSystemTimeWide();
    uint32_t lapse = curr_check_time - last_check_time;
    last_check_time = curr_check_time;
    if (lapse > wait && (lapse - wait) < wait) {
      //vita_debug_log("sleep: %d", wait * 2 - lapse);
      sceKernelDelayThread(wait * 2 - lapse);
    } else {
      sceKernelDelayThread(wait);
    }
  }
  return 0;
}

static void vita_cleanup() {
  if (video_status == INIT_FRAME_PACER_THREAD) {
    active_pacer_thread = false;
    // wait 10sec
    SceUInt timeout = 10000000;
    int ret;
    sceKernelWaitThreadEnd(pacer_thread, &ret, &timeout);
    sceKernelDeleteThread(pacer_thread);
    video_status--;
  }

  if (video_status == INIT_AVC_DEC) {
    sceAvcdecDeleteDecoder(decoder);
    video_status--;
  }

  if (video_status == INIT_DECODER_MEMBLOCK) {
    if (decoderblock >= 0) {
      sceKernelFreeMemBlock(decoderblock);
      decoderblock = -1;
    }
    if (decoder != NULL) {
      free(decoder);
      decoder = NULL;
    }
    if (decoder_info != NULL) {
      free(decoder_info);
      decoder_info = NULL;
    }
    video_status--;
  }

  if (video_status == INIT_AVC_LIB) {
    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);

    if (init != NULL) {
      free(init);
      init = NULL;
    }
    video_status--;
  }

  if (video_status == INIT_FRAMEBUFFER) {
    for (int ft = 0; ft < 2; ft++) {
      if (frame_textures[ft] != NULL) {
        vita2d_free_texture(frame_textures[ft]);
        frame_textures[ft] = NULL;
      }
    }
    frame_texture = NULL;

    if (decoder_buffer != NULL) {
      free(decoder_buffer);
      decoder_buffer = NULL;
    }
    video_status--;
  }

  if (video_status == INIT_GS) {
    gs_sps_stop();
    video_status--;
  }
}

// single decoder thread owns these aggregates; no per-packet tracing.
static struct {
  uint64_t since;
  uint32_t frames, decode_max_us, present_max_us, au_max, errors;
} perf_stats;

static void record_perf(uint64_t decode_us, uint64_t present_us,
                       unsigned au_bytes, bool failed) {
  uint64_t now = sceKernelGetSystemTimeWide();
  if (!perf_stats.since) perf_stats.since = now;
  perf_stats.frames++;
  if (decode_us > perf_stats.decode_max_us) perf_stats.decode_max_us = (uint32_t)decode_us;
  if (present_us > perf_stats.present_max_us) perf_stats.present_max_us = (uint32_t)present_us;
  if (au_bytes > perf_stats.au_max) perf_stats.au_max = au_bytes;
  if (failed) perf_stats.errors++;
  if (now - perf_stats.since >= 1000000) {
    vita_debug_log("perf: frames=%u decode_max_us=%u present_max_us=%u au_max=%u errors=%u\n",
                   perf_stats.frames, perf_stats.decode_max_us, perf_stats.present_max_us,
                   perf_stats.au_max, perf_stats.errors);
    memset(&perf_stats, 0, sizeof(perf_stats));
    perf_stats.since = now;
  }
}

static int vita_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  int ret;
  memset(&perf_stats, 0, sizeof(perf_stats));
  printf("vita video setup\n");

  if (video_status == NOT_INIT) {
    // INIT_GS
    gs_sps_init(width, height);
    video_status++;
  }

  if (video_status == INIT_GS) {
    // INIT_FRAMEBUFFER
    update_scaling_settings(width, height);

    decoder_buffer_size = DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE;
    decoder_buffer = malloc(decoder_buffer_size);
    if (decoder_buffer == NULL) {
      printf("decoder_buffer: not enough memory\n");
      ret = VITA_VIDEO_ERROR_NO_MEM;
      goto cleanup;
    } 

    for (int ft = 0; ft < 2; ft++) {
      frame_textures[ft] = vita2d_create_empty_texture_format(image_scaling.texture_width, image_scaling.texture_height, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
      if (frame_textures[ft] == NULL) {
        printf("frame_texture: not enough memory\n");
        ret = VITA_VIDEO_ERROR_NO_MEM;
        goto cleanup;
      }
    }
    frame_tex_idx = 0;
    frame_texture = frame_textures[0];

    printf("Initalized Framebuffer");
    if (config.gpu_clock_mhz > 0) {
      int got = scePowerSetGpuClockFrequency(config.gpu_clock_mhz);
      vita_debug_log("GPU clock lock requested %d MHz -> 0x%x (now %d MHz)\n",
                     config.gpu_clock_mhz, got, scePowerGetGpuClockFrequency());
    }

    video_status++;
  }

  if (video_status == INIT_FRAMEBUFFER) {
    // INIT_AVC_LIB
    if (init == NULL) {
      init = calloc(1, sizeof(SceVideodecQueryInitInfoHwAvcdec));
      if (init == NULL) {
        printf("init_framebuffer: not enough memory\n");
        ret = VITA_VIDEO_ERROR_NO_MEM;
        goto cleanup;
      }
    }
    init->size = sizeof(SceVideodecQueryInitInfoHwAvcdec);
    init->horizontal = VITA_DECODER_RESOLUTION(width);
    init->vertical = VITA_DECODER_RESOLUTION(height);
    init->numOfRefFrames = 4;
    init->numOfStreams = 1;

    ret = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, init);
    if (ret < 0) {
      printf("sceVideodecInitLibrary 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_INIT_LIB;
      goto cleanup;
    }
    printf("Initalized AVC library");
    video_status++;
  }

  if (video_status == INIT_AVC_LIB) {
    // INIT_DECODER_MEMBLOCK
    if (decoder_info == NULL) {
      decoder_info = calloc(1, sizeof(SceAvcdecQueryDecoderInfo));
      if (decoder_info == NULL) {
        printf("decoder_info: not enough memory\n");
        ret = VITA_VIDEO_ERROR_NO_MEM;
        goto cleanup;
      }
    }
    decoder_info->horizontal = init->horizontal;
    decoder_info->vertical = init->vertical;
    decoder_info->numOfRefFrames = init->numOfRefFrames;

    SceAvcdecDecoderInfo decoder_info_out = {0};

    ret = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, decoder_info, &decoder_info_out);
    if (ret < 0) {
      printf("sceAvcdecQueryDecoderMemSize 0x%x size 0x%x\n", ret, decoder_info_out.frameMemSize);
      ret = VITA_VIDEO_ERROR_QUERY_DEC_MEMSIZE;
      goto cleanup;
    }

    decoder = calloc(1, sizeof(SceAvcdecCtrl));
    if (decoder == NULL) {
      printf("not enough memory\n");
      ret = VITA_VIDEO_ERROR_ALLOC_MEM;
      goto cleanup;
    }

    size_t sz = (decoder_info_out.frameMemSize + 0xFFFFF) & ~0xFFFFF;
    decoder->frameBuf.size = sz;
    printf("allocating size 0x%x\n", sz);

    decoderblock = sceKernelAllocMemBlock("decoder", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
    if (decoderblock < 0) {
      printf("decoderblock: 0x%08x\n", decoderblock);
      ret = VITA_VIDEO_ERROR_ALLOC_MEM;
      goto cleanup;
    }

    ret = sceKernelGetMemBlockBase(decoderblock, &decoder->frameBuf.pBuf);
    if (ret < 0) {
      printf("sceKernelGetMemBlockBase: 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_GET_MEMBASE;
      goto cleanup;
    }
    video_status++;
  }

  if (video_status == INIT_DECODER_MEMBLOCK) {
    // INIT_AVC_DEC
    printf("base: 0x%08x\n", (unsigned int)decoder->frameBuf.pBuf);

    ret = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, decoder, decoder_info);
    if (ret < 0) {
      printf("sceAvcdecCreateDecoder 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_CREATE_DEC;
      goto cleanup;
    }
    video_status++;
  }

  if (video_status == INIT_AVC_DEC) {
    // INIT_FRAME_PACER_THREAD
    ret = sceKernelCreateThread("frame_pacer", vita_pacer_thread_main, 0, 0x10000, 0, 0, NULL);
    if (ret < 0) {
      printf("sceKernelCreateThread 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_CREATE_PACER_THREAD;
      goto cleanup;
    }
    pacer_thread = ret;
    active_pacer_thread = true;
    sceKernelStartThread(pacer_thread, 0, NULL);
    video_status++;
  }

  return VITA_VIDEO_INIT_OK;

cleanup:
  vita_cleanup();
  return ret;
}

static int vita_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  // One-shot: report what the OS actually gave this thread.
  // The video threads are created by PltCreateThread()'s pthread branch
  // (Platform.c), which passes no priority at all, while frame_pacer and
  // the input threads are created with an explicit initPriority of 0.
  // Without the real numbers there is no safe value to change it to.
  static bool thread_reported = false;
  if (!thread_reported) {
    thread_reported = true;
    SceUID self = sceKernelGetThreadId();
    SceKernelThreadInfo info = {0};
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(self, &info) == 0) {
      vita_debug_log("video thread: name %s initPriority 0x%x currentPriority 0x%x affinity 0x%x\n",
                     info.name, (unsigned)info.initPriority,
                     (unsigned)info.currentPriority, (unsigned)info.initCpuAffinityMask);
      if (config.video_thread_priority >= 0 &&
          sceKernelChangeThreadPriority(self, config.video_thread_priority) == 0) {
        vita_debug_log("video thread priority 0x%x -> %d\n",
                         (unsigned)info.currentPriority, config.video_thread_priority);
      }
    } else {
      vita_debug_log("sceKernelGetThreadInfo failed on the video thread\n");
    }
  }
  // verbose: per-frame timing trace. Only emits when enabled, and
  // only on the direct-submit path (the Vita uses CAPABILITY_DIRECT_SUBMIT).
  uint64_t v_now = sceKernelGetSystemTimeWide();
  uint64_t v_recv_us = (decodeUnit->enqueueTimeUs > decodeUnit->receiveTimeUs)
                       ? (decodeUnit->enqueueTimeUs - decodeUnit->receiveTimeUs) : 0;
  uint64_t v_queue_us = (v_now > decodeUnit->enqueueTimeUs)
                        ? (v_now - decodeUnit->enqueueTimeUs) : 0;
  if (config.enable_verbose_log) {
    vita_debug_log("vfr[%d] type=%c len=%d recv_us=%llu queue_us=%llu start",
                   decodeUnit->frameNumber,
                   decodeUnit->frameType == FRAME_TYPE_IDR ? 'I' : 'P',
                   decodeUnit->fullLength,
                   (unsigned long long)v_recv_us, (unsigned long long)v_queue_us);
  }

  SceAvcdecAu au = {0};
  SceAvcdecArrayPicture array_picture = {0};
  struct SceAvcdecPicture picture = {0};
  struct SceAvcdecPicture *pictures = { &picture };
  array_picture.numOfElm = 1;
  array_picture.pPicture = &pictures;

  //frame->time = decodeUnit->receiveTimeMs;

  picture.size = sizeof(picture);
  picture.frame.pixelType = 0;
  picture.frame.framePitch = image_scaling.texture_width;
  picture.frame.frameWidth = image_scaling.texture_width;
  picture.frame.frameHeight = image_scaling.texture_height;
  picture.frame.pPicture[0] = vita2d_texture_get_datap(frame_texture);

  //ensure_buf_size((void *)&decoder_buffer, &decoder_buffer_size, decodeUnit->fullLength + 64);

  if (decoder_buffer_size < (decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE)) {
    // Grow with headroom: an I-frame is several times an average frame,
    // so sizing to the exact requirement reallocs on every keyframe.
    size_t want = (size_t)decodeUnit->fullLength * 2 + AV_INPUT_BUFFER_PADDING_SIZE;
    char* grown = realloc(decoder_buffer, want);
    if (grown == NULL) {
      // exit(1) here closed the entire app. One oversized frame is not a
      // fatal session error: keep the old buffer and ask for an IDR.
      printf("decoder buffer: could not grow the bitstream buffer to %u bytes; requesting IDR\n",
             (unsigned)want);
      return DR_NEED_IDR;
    }
    decoder_buffer = grown;
    decoder_buffer_size = want;
    printf("decoder buffer grown to %u bytes\n", (unsigned)want);
  }


/*   if (decodeUnit->fullLength >= DECODER_BUFFER_SIZE + 64) {
    printf("Video decode buffer too small\n");
    exit(1);
  } */

  PLENTRY entry = decodeUnit->bufferList;
  uint32_t length = 0;
  while (entry != NULL) {
    if (entry->bufferType == BUFFER_TYPE_SPS) {
      gs_sps_fix(entry, GS_SPS_BITSTREAM_FIXUP, decoder_buffer, &length);
    } else {
      memcpy(decoder_buffer+length, entry->data, entry->length);
      length += entry->length;
    }
    entry = entry->next;
  }

  au.es.pBuf = decoder_buffer;
  au.es.size = decodeUnit->fullLength;
  au.dts.lower = 0xFFFFFFFF;
  au.dts.upper = 0xFFFFFFFF;
  au.pts.lower = 0xFFFFFFFF;
  au.pts.upper = 0xFFFFFFFF;

  int ret = 0;
  uint64_t decode_begin = sceKernelGetSystemTimeWide();
  ret = sceAvcdecDecode(decoder, &au, &array_picture);
  uint64_t decode_end = sceKernelGetSystemTimeWide();
  if (config.enable_verbose_log) {
    vita_debug_log("vfr[%d] decode_us=%llu ret=0x%x out=%d",
                   decodeUnit->frameNumber,
                   (unsigned long long)(decode_end - decode_begin),
                   ret, array_picture.numOfOutput);
  }
  if (ret < 0) {
    record_perf(decode_end - decode_begin, 0, decodeUnit->fullLength, true);
    printf("sceAvcdecDecode (len=0x%x): 0x%x numOfOutput %d\n", decodeUnit->fullLength, ret, array_picture.numOfOutput);
    return DR_NEED_IDR;
  }

  if (array_picture.numOfOutput != 1) {
    record_perf(decode_end - decode_begin, 0, decodeUnit->fullLength, true);
    static uint32_t no_output = 0;
    if ((++no_output % 10) == 1)
      vita_debug_log("avcdec produced no picture (count %u, au len %u)\n",
                     no_output, decodeUnit->fullLength);
    return DR_OK;
  }

  //TODO: Seems silly to decode the unit if we're going to drop the frame?
  // Find out why we decode or if we even need to
  if (active_video_thread) {
    if (need_drop > 0) {
      vita_debug_log("remain frameskip: %d\n", need_drop);
      // skip
      need_drop--;
    } else {
      vita2d_start_drawing();

      draw_streaming(frame_texture);
      draw_fps();
      draw_indicators();

      vita2d_end_drawing();

      // finish GPU sampling before reusing decode textures. This is
      // GPU completion, not a forced display-vblank wait. Reception proceeds
      // independently on VideoRecv while VideoDec waits here.
      vita2d_wait_rendering_done();
      vita2d_swap_buffers();

      frame_tex_idx ^= 1;
      frame_texture = frame_textures[frame_tex_idx];

      frame_count++;

      if (config.enable_verbose_log) {
        vita_debug_log("vfr[%d] present_us=%llu done",
                       decodeUnit->frameNumber,
                       (unsigned long long)(sceKernelGetSystemTimeWide() - decode_end));
      }
    }
  }

  record_perf(decode_end - decode_begin,
             sceKernelGetSystemTimeWide() - decode_end,
             decodeUnit->fullLength, false);
  return DR_OK;
}

void draw_streaming(vita2d_texture *frame_texture) {
  // ui is still rendering in the background, clear the screen first
  vita2d_clear_screen();
  vita2d_draw_texture_part(frame_texture,
                           image_scaling.origin_x,
                           image_scaling.origin_y,
                           image_scaling.region_x1,
                           image_scaling.region_y1,
                           image_scaling.region_x2,
                           image_scaling.region_y2);
}

void draw_fps() {
  if (config.show_fps) {
    vita2d_font_draw_textf(font, 40, 20, RGBA8(0xFF, 0xFF, 0xFF, 0xFF), 16, "fps: %u / %u", curr_fps[0], curr_fps[1]);
  }
}

void draw_indicators() {
  if (poor_net_indicator.activated) {
    vita2d_font_draw_text(font, 40, 500, RGBA8(0xFF, 0xFF, 0xFF, poor_net_indicator.alpha), 64, ICON_NETWORK);
    poor_net_indicator.alpha += (0x4 * (poor_net_indicator.plus ? 1 : -1));
    if (poor_net_indicator.alpha == 0) {
      poor_net_indicator.plus = !poor_net_indicator.plus;
      poor_net_indicator.alpha += (0x4 * (poor_net_indicator.plus ? 1 : -1));
    }
  }

  if (dc_tracker.currently_sprinting) {
    vita2d_font_draw_text(font, 40, 50, RGBA8(0xFF, 0xFF, 0xFF, 0xAA), 48, ICON_SPRINTING);
  }

}

void vitavideo_start() {
  active_video_thread = true;
  vita2d_set_vblank_wait(config.enable_vita_vblank_wait);
}

void vitavideo_stop() {
  vita2d_set_vblank_wait(true);
  active_video_thread = false;
}

void vitavideo_show_poor_net_indicator() {
  poor_net_indicator.activated = true;
}

void vitavideo_hide_poor_net_indicator() {
  //poor_net_indicator.activated = false;
  memset(&poor_net_indicator, 0, sizeof(indicator_status));
}

int vitavideo_initialized() {
  return video_status != NOT_INIT;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_vita = {
  .setup = vita_setup,
  .cleanup = vita_cleanup,
  .submitDecodeUnit = vita_submit_decode_unit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SLICES_PER_FRAME(2)
};
