/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015, 2016 Iwan Timmer
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

// Custom build (2026-10-16): upstream ships this header without an
// include guard; i18n.h needs one.
#ifndef MOONLIGHT_CONFIG_H
#define MOONLIGHT_CONFIG_H

#include <Limelight.h>

#include <stdio.h>
#include <stdbool.h>

#include <psp2/ctrl.h>

#define MAX_INPUTS 6

struct input_config {
  char* path;
  char* mapping;
};

struct touchscreen_deadzone {
  int top, bottom, left, right;
};

struct special_keys {
  int size, offset;
  unsigned int nw, ne, sw, se;
};

typedef struct _CONFIGURATION {
  // static configuration, value will be saved to config file
  STREAM_CONFIGURATION stream;
  char* app;
  char* action;
  char* address;
  char* mapping;
  char* platform;
  uint32_t model;
  char* config_file;
  char key_dir[4096];
  bool sops;
  bool localaudio;
  bool fullscreen;
  bool forcehw;
  bool unsupported_version;
  struct touchscreen_deadzone back_deadzone;
  bool enable_front_touchzones;
  struct special_keys special_keys;
  bool disable_powersave;
  bool jp_layout;
  bool show_fps;
  bool enable_frame_pacer;
  bool center_region_only;
  bool save_debug_log;
  bool enable_verbose_log;  // per-frame timing + state-transition tracing
  struct input_config inputs[MAX_INPUTS];
  int inputsCount;
  int mouse_acceleration;
  int language;  // 0 = English, 1 = Chinese
  bool enable_ref_frame_invalidation;
  bool enable_vita_vblank_wait;
  // moonlight-common-c only creates its "VideoDec" thread when the
  // renderer does NOT claim CAPABILITY_DIRECT_SUBMIT. With it set,
  // reassembleFrame() calls submitDecodeUnit() on the VideoRecv thread,
  // so receive + FEC + decode + draw are serial on one core.
  bool enable_split_decode_thread;
  int video_thread_priority;  // -1 = leave whatever the OS picked alone

  // Custom build (2026-10-18): network/robustness knobs that moonlight-common-c
  // used to hardcode for the Vita target. See README section 17.
  int net_profile;                 // 0=custom 1=low latency 2=balanced 3=loss tolerant
  int video_max_ref_frames;        // -1 = upstream behaviour, 0 = host picks, 1-16 = request
  int video_slices_per_frame;      // -1 = upstream behaviour, >=1 = explicit slices
  int fec_recovery_mode;           // 0=off, 1=bounded local repair
  int fec_min_required_packets;    // FEC repair-packet floor for small frames
  int consecutive_drop_limit;      // frames dropped in a row before forcing an IDR
  // Custom build (2026-10-18 v15): 0 = leave the governor alone; otherwise one of
  // the legal ES4 GPU steps 41/55/83/111/166/222 (see henkaku ScePower wiki).
  // log_gpu_clock writes a per-second GPU/XBAR/BUS/ARM line into the log so the
  // freeze-time clock trajectory is visible in moonlight.log.
  int gpu_clock_mhz;
  int log_gpu_clock;
  bool enable_motion_controls; //Metalface
  bool enable_psbutton_capture;
  bool enable_double_tap_sprint; //**
  uint32_t double_tap_sprint_step_time; //** -IN MILLISECONDS
  float motion_controls_scalar_x;//**
  float motion_controls_scalar_y;// **/
  FILE *log_file;
  // runtime configuration, value will be recreated at launch
  SceCtrlButtons btn_confirm;
  SceCtrlButtons btn_cancel;
  int pin;
  uint16_t port;
  int keyboard_layout; // 0=EN_US, 1=ES_ES, 2=ES_LATAM
  int touchscreen_mode; // 0=off, 1=DS4, 2=Mouse absoluto, 3=Tableta multitouch
  int controller_type; // 1: Xbox, 2: PS (default), 3: Nintendo, 4: Generic
  bool swap_shoulder_buttons; // Nuevo: swap R1/L1 <-> R2/L2
} CONFIGURATION, *PCONFIGURATION;

extern CONFIGURATION config;
extern char *config_path;

extern bool inputAdded;

bool config_file_parse(char* filename, PCONFIGURATION config);
void config_parse(int argc, char* argv[], PCONFIGURATION config);
void config_save(const char* filename, PCONFIGURATION config);
void update_layout();

#endif /* MOONLIGHT_CONFIG_H */
