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

#include "config.h"
#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <ini.h>
#include "input/vita.h"
#include "debug.h"

extern char* strdup(const char*);

#include <psp2/kernel/sysmem.h>

#define MOONLIGHT_PATH "/moonlight"
#define USER_PATHS "."
#define DEFAULT_CONFIG_DIR "/.config"
#define DEFAULT_CACHE_DIR "/.cache"

#define write_config_string(fd, key, value) fprintf(fd, "%s = %s\n", key, value)
#define write_config_int(fd, key, value) fprintf(fd, "%s = %d\n", key, value)
#define write_config_hex(fd, key, value) fprintf(fd, "%s = %X\n", key, value)
#define write_config_bool(fd, key, value) fprintf(fd, "%s = %s\n", key, value?"true":"false");
#define write_config_float(fd, key, value) fprintf(fd, "%s = %f\n", key, value)
#define write_config_section(fd, key) fprintf(fd, "\n[%s]\n", key)

CONFIGURATION config;
char *config_path;

bool inputAdded = false;
static bool mapped = true;
static bool burst_profile_seen = false;
const char* audio_device = NULL;

static int ini_handle(void *out, const char *section, const char *name,
                      const char *value) {
#define HEX(v) strtol((v), NULL, 16)
#define INT(v) atoi((v))
#define BOOL(v) (strcmp((v), "true") == 0 || strcmp((v), "1") == 0)
#define STR(v) strdup((v))
#define FLT(v) atof((v))

  PCONFIGURATION config = (PCONFIGURATION)out;
  if (strcmp(section, "backtouchscreen_deadzone") == 0) {
    if (strcmp(name, "top") == 0) {
      config->back_deadzone.top = INT(value);
    } else if (strcmp(name, "right") == 0) {
      config->back_deadzone.right = INT(value);
    } else if (strcmp(name, "bottom") == 0) {
      config->back_deadzone.bottom = INT(value);
    } else if (strcmp(name, "left") == 0) {
      config->back_deadzone.left = INT(value);
    }
    else if (strcmp(name, "controller_type") == 0) {
      config->controller_type = INT(value);
    } else if (strcmp(name, "swap_shoulder_buttons") == 0) {
      config->swap_shoulder_buttons = BOOL(value);
    } else if (strcmp(name, "key_dir") == 0) {
      strncpy(config->key_dir, value, sizeof(config->key_dir)-1);
      config->key_dir[sizeof(config->key_dir)-1] = '\0';
    }
  } else if (strcmp(section, "special_keys") == 0) {
    if (strcmp(name, "nw") == 0) {
      config->special_keys.nw = HEX(value);
    } else if (strcmp(name, "ne") == 0) {
      config->special_keys.ne = HEX(value);
    } else if (strcmp(name, "sw") == 0) {
      config->special_keys.sw = HEX(value);
    } else if (strcmp(name, "se") == 0) {
      config->special_keys.se = HEX(value);
    } else if (strcmp(name, "offset") == 0) {
      config->special_keys.offset = INT(value);
    } else if (strcmp(name, "size") == 0) {
      config->special_keys.size = INT(value);
    }
  } else {
    if (strcmp(name, "burst_profile_version") == 0) {
      burst_profile_seen = INT(value) >= 17;
    } else if (strcmp(name, "address") == 0) {
      config->address = STR(value);
    } else if (strcmp(name, "width") == 0) {
      config->stream.width = INT(value);
    } else if (strcmp(name, "height") == 0) {
      config->stream.height = INT(value);
    } else if (strcmp(name, "fps") == 0) {
      config->stream.fps = INT(value);
    } else if (strcmp(name, "bitrate") == 0) {
      config->stream.bitrate = INT(value);
    } else if (strcmp(name, "language") == 0) {
      config->language = INT(value);
    } else if (strcmp(name, "sops") == 0) {
      config->sops = BOOL(value);
    } else if (strcmp(name, "localaudio") == 0) {
      config->localaudio = BOOL(value);
    } else if (strcmp(name, "enable_frame_pacer") == 0) {
      config->enable_frame_pacer = BOOL(value);
    } else if (strcmp(name, "center_region_only") == 0) {
      config->center_region_only = BOOL(value);
    } else if (strcmp(name, "disable_powersave") == 0) {
      config->disable_powersave = BOOL(value);
    } else if (strcmp(name, "jp_layout") == 0) {
      config->jp_layout = BOOL(value);
    } else if (strcmp(name, "show_fps") == 0) {
      config->show_fps = BOOL(value);
    } else if (strcmp(name, "save_debug_log") == 0) {
      config->save_debug_log = BOOL(value);
    } else if (strcmp(name, "enable_verbose_log") == 0) {
      config->enable_verbose_log = BOOL(value);
    } else if (strcmp(name, "mapping") == 0) {
      config->mapping = STR(value);
    } else if (strcmp(name, "mouse_acceleration") == 0) {
      config->mouse_acceleration = INT(value);
    } else if (strcmp(name, "enable_ref_frame_invalidation") == 0) {
      config->enable_ref_frame_invalidation = BOOL(value);
    } else if (strcmp(name, "enable_remote_stream_optimization") == 0) {
      // Custom build (2026-10-18): this is a STREAM_CFG_LOCAL(0) /
      // STREAM_CFG_REMOTE(1) / STREAM_CFG_AUTO(2) value, not a boolean. The
      // value the shipped moonlight.conf carried (1) means REMOTE even on a LAN,
      // which subtracts 500 Kbps from the encoder bitrate and clears the QoS
      // DSCP marking that Windows qWave would otherwise apply.
      config->stream.streamingRemotely = INT(value);
    } else if (strcmp(name, "enable_vita_vblank_wait") == 0) {
      config->enable_vita_vblank_wait = BOOL(value);
    } else if (strcmp(name, "enable_split_decode_thread") == 0) {
      config->enable_split_decode_thread = BOOL(value);
    } else if (strcmp(name, "video_thread_priority") == 0) {
      config->video_thread_priority = atoi(value);
    } else if (strcmp(name, "net_profile") == 0) {
      config->net_profile = INT(value);
    } else if (strcmp(name, "video_max_ref_frames") == 0) {
      config->video_max_ref_frames = INT(value);
    } else if (strcmp(name, "video_slices_per_frame") == 0) {
      config->video_slices_per_frame = INT(value);
    } else if (strcmp(name, "fec_recovery_mode") == 0) {
      config->fec_recovery_mode = INT(value) == 1 ? 1 : 0;
    } else if (strcmp(name, "fec_min_required_packets") == 0) {
      config->fec_min_required_packets = INT(value);
    } else if (strcmp(name, "gpu_clock_mhz") == 0) {
      config->gpu_clock_mhz = INT(value);
    } else if (strcmp(name, "log_gpu_clock") == 0) {
      config->log_gpu_clock = INT(value);
    } else if (strcmp(name, "consecutive_drop_limit") == 0) {
      config->consecutive_drop_limit = INT(value);
    } else if (strcmp(name, "enable_motion_controls") == 0) {
      config->enable_motion_controls = BOOL(value);
    } else if (strcmp(name, "enable_front_touchzones") == 0) {
      config->enable_front_touchzones = BOOL(value);
    } else if(strcmp(name, "enable_psbutton_capture") == 0) {
      config->enable_psbutton_capture = BOOL(value);
    } else if (strcmp(name, "enable_double_tap_sprint") == 0) {
      config->enable_double_tap_sprint = BOOL(value);
    } else if (strcmp(name, "double_tap_sprint_step_time") == 0) {
      config->double_tap_sprint_step_time = INT(value);
    } else if (strcmp(name, "motion_controls_scalar_x") == 0) {
      config->motion_controls_scalar_x = FLT(value);
    } else if (strcmp(name, "motion_controls_scalar_y") == 0) {
      config->motion_controls_scalar_y = FLT(value);
    } else if (strcmp(name, "keyboard_layout") == 0) {
      config->keyboard_layout = INT(value);
    } else if (strcmp(name, "touchscreen_mode") == 0) {
      config->touchscreen_mode = atoi(value);
    } else if (strcmp(name, "controller_type") == 0) {
      config->controller_type = INT(value);
    } else if (strcmp(name, "swap_shoulder_buttons") == 0) {
      config->swap_shoulder_buttons = BOOL(value);
    } else if (strcmp(name, "packetsize") == 0) {
      // FEC only works in 16-byte chunks; Connection.c rounds this down.
      // 1440 keeps the whole UDP datagram under a 1500-byte MTU.
      int ps = INT(value);
      if (ps >= 256 && ps <= 1440) {
        config->stream.packetSize = ps;
      }
    }
  }
  return 0;
}

bool config_file_parse(char* filename, PCONFIGURATION config) {
  burst_profile_seen = false;
  int result = ini_parse(filename, ini_handle, config);
  if (!burst_profile_seen) {
    // One-time migration, persisted on the next ordinary settings save.
    // Preserve bitrate, resolution, clocks and all input/pairing settings.
    config->enable_split_decode_thread = true;
    burst_profile_seen = true;
  }
  return result;
}

void config_save(const char* filename, PCONFIGURATION config) {
  FILE* fd = fopen(filename, "w");
  if (fd == NULL) {
    fprintf(stderr, "Can't open configuration file: %s\n", filename);
    exit(EXIT_FAILURE);
  }

  write_config_int(fd, "burst_profile_version", 17);

  if (config->address)
    write_config_string(fd, "address", config->address);


  if (config->mapping)
    write_config_string(fd, "mapping", config->mapping);

  if (config->stream.width != 1280)
    write_config_int(fd, "width", config->stream.width);
  if (config->stream.height != 720)
    write_config_int(fd, "height", config->stream.height);
  if (config->stream.fps != 60)
    write_config_int(fd, "fps", config->stream.fps);
  if (config->stream.bitrate != -1)
    write_config_int(fd, "bitrate", config->stream.bitrate);
  if (config->language != 0)
    write_config_int(fd, "language", config->language);
  if (config->stream.packetSize != 1024)
    write_config_int(fd, "packetsize", config->stream.packetSize);
  if (!config->sops)
    write_config_bool(fd, "sops", config->sops);
  if (config->localaudio)
    write_config_bool(fd, "localaudio", config->localaudio);

  if (config->app && strcmp(config->app, "Steam") != 0)
    write_config_string(fd, "app", config->app);

  write_config_string(fd, "key_dir", config->key_dir); // Guardar key_dir en la ra铆z
  write_config_bool(fd, "enable_frame_pacer", config->enable_frame_pacer);
  write_config_bool(fd, "center_region_only", config->center_region_only);
  write_config_bool(fd, "disable_powersave", config->disable_powersave);
  write_config_bool(fd, "jp_layout", config->jp_layout);
  write_config_bool(fd, "show_fps", config->show_fps);
  write_config_bool(fd, "save_debug_log", config->save_debug_log);
  write_config_bool(fd, "enable_verbose_log", config->enable_verbose_log);
  write_config_bool(fd, "enable_front_touchzones", config->enable_front_touchzones);

  write_config_int(fd, "mouse_acceleration", config->mouse_acceleration);
  write_config_bool(fd, "enable_ref_frame_invalidation", config->enable_ref_frame_invalidation);
  write_config_int(fd, "enable_remote_stream_optimization", config->stream.streamingRemotely);
  write_config_bool(fd, "enable_vita_vblank_wait", config->enable_vita_vblank_wait);
  write_config_bool(fd, "enable_split_decode_thread", config->enable_split_decode_thread);
  fprintf(fd, "video_thread_priority = %d\n", config->video_thread_priority);
  fprintf(fd, "net_profile = %d\n", config->net_profile);
  fprintf(fd, "video_max_ref_frames = %d\n", config->video_max_ref_frames);
  fprintf(fd, "video_slices_per_frame = %d\n", config->video_slices_per_frame);
  fprintf(fd, "fec_min_required_packets = %d\n", config->fec_min_required_packets);
  fprintf(fd, "fec_recovery_mode = %d\n", config->fec_recovery_mode);
  fprintf(fd, "consecutive_drop_limit = %d\n", config->consecutive_drop_limit);
  fprintf(fd, "gpu_clock_mhz = %d\n", config->gpu_clock_mhz);
  fprintf(fd, "log_gpu_clock = %d\n", config->log_gpu_clock);

  write_config_bool(fd, "enable_motion_controls", config->enable_motion_controls);
  write_config_bool(fd, "enable_psbutton_capture", config->enable_psbutton_capture);
  write_config_bool(fd, "enable_double_tap_sprint", config->enable_double_tap_sprint);
  write_config_int(fd, "double_tap_sprint_step_time", config->double_tap_sprint_step_time);
  write_config_float(fd, "motion_controls_scalar_x", config->motion_controls_scalar_x);
  write_config_float(fd, "motion_controls_scalar_y", config->motion_controls_scalar_y);
  write_config_int(fd, "keyboard_layout", config->keyboard_layout);
  write_config_int(fd, "touchscreen_mode", config->touchscreen_mode);
  write_config_bool(fd, "swap_shoulder_buttons", config->swap_shoulder_buttons); // Guardar swap_shoulder_buttons en la ra铆z
  write_config_int(fd, "controller_type", config->controller_type); // Guardar controller_type en la ra铆z
  

  write_config_section(fd, "backtouchscreen_deadzone");
  write_config_int(fd, "top",     config->back_deadzone.top);
  write_config_int(fd, "right",   config->back_deadzone.right);
  write_config_int(fd, "bottom",  config->back_deadzone.bottom);
  write_config_int(fd, "left",    config->back_deadzone.left);

  write_config_section(fd, "special_keys");
  write_config_hex(fd, "nw",      config->special_keys.nw);
  write_config_hex(fd, "ne",      config->special_keys.ne);
  write_config_hex(fd, "sw",      config->special_keys.sw);
  write_config_hex(fd, "se",      config->special_keys.se);
  write_config_int(fd, "offset",  config->special_keys.offset);
  write_config_int(fd, "size",    config->special_keys.size);

  fclose(fd);
}

void update_layout() {
  if (config.jp_layout) {
    config.btn_confirm = SCE_CTRL_CIRCLE;
    config.btn_cancel = SCE_CTRL_CROSS;
  }
  else {
    config.btn_confirm = SCE_CTRL_CROSS;
    config.btn_cancel = SCE_CTRL_CIRCLE;
  }
}

void config_parse(int argc, char* argv[], PCONFIGURATION config) {
  LiInitializeStreamConfiguration(&config->stream);

  config->stream.width = 960;
  config->stream.height = 544;
  config->stream.fps = 60;
  config->stream.bitrate = 8000;
  // 1440 + 16 RTP + 8 UDP + 20 IP = 1484 <= 1500 MTU. Fewer, larger
  // packets means fewer FEC shards per frame, which is what raises the
  // 255-shard frame ceiling from ~209 KB to ~295 KB. Connection.c still
  // caps this to 1024 automatically for real WAN IPv4 streams.
  config->stream.packetSize = 1440;
  config->stream.streamingRemotely = STREAM_CFG_AUTO;
  config->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
  config->stream.supportedVideoFormats = VIDEO_FORMAT_H264;
  config->language = 0;

  config->platform = "vita";
  config->model = sceKernelGetModelForCDialog();
  config->app = "Steam";
  config->action = NULL;
  config->address = NULL;
  config->config_file = NULL;
  config->sops = true;
  config->localaudio = false;
  config->fullscreen = true;
  config->unsupported_version = false;
  config->save_debug_log = false;
  config->enable_verbose_log = false;
  config->disable_powersave = true;
  config->jp_layout = false;
  config->show_fps = false;
  config->enable_frame_pacer = true;
  config->center_region_only = false;

  config->enable_front_touchzones = false;
  config->special_keys.nw = INPUT_SPECIAL_KEY_PAUSE | INPUT_TYPE_SPECIAL;
  config->special_keys.sw = SPECIAL_FLAG | INPUT_TYPE_GAMEPAD;
  config->special_keys.offset = 0;
  config->special_keys.size = 150;

  config->mouse_acceleration = 150;
  config->enable_ref_frame_invalidation = false;
  config->enable_vita_vblank_wait = false;
  config->enable_split_decode_thread = true; // keep reception independent of decode/present
  config->video_thread_priority = -1;
  config->net_profile = 0;   // 0 = default profile (2026-10-19)
  config->video_max_ref_frames = 3;
  config->video_slices_per_frame = 1;
  config->fec_min_required_packets = 0;
  config->fec_recovery_mode = 0; // no CPU Reed-Solomon work by default
  config->consecutive_drop_limit = 5;
  config->gpu_clock_mhz = 0;
  config->log_gpu_clock = 1;
  config->enable_psbutton_capture = true;
  config->enable_double_tap_sprint = false;

  config->double_tap_sprint_step_time = 200;

  config->enable_motion_controls = true;   // gyro on by default (2026-10-19)
  config->motion_controls_scalar_x = 1.2;
  config->motion_controls_scalar_y = 0.8;

  config->inputsCount = 0;
  config->mapping = NULL;
  // No sobrescribir key_dir si ya fue asignado por main.c
  // config->key_dir[0] = 0;
  // Valor por defecto: PS si no est谩 asignado
  if (config->controller_type < 1 || config->controller_type > 4) {
    config->controller_type = 2;
  }
  // Valor por defecto: swap desactivado
  // (si no est谩 presente en config, ser谩 false por defecto)
  // No es necesario forzar nada aqu铆

  char* config_file = config_path;
  if (config_file) {
    config_file_parse(config_file, config);
    vita_debug_log("[DEBUG] Configuraci贸n cargada: key_dir = %s, touchscreen_mode = %d, show_fps = %d", config->key_dir, config->touchscreen_mode, config->show_fps);
  }

  update_layout();

  if (config->config_file != NULL)
    config_save(config->config_file, config);

  // Solo asignar valor por defecto si sigue vac铆o
  if (config->key_dir[0] == 0x0) {
    const char *xdg_cache_dir = getenv("XDG_CACHE_DIR");
    if (xdg_cache_dir && xdg_cache_dir[0] != '\0') {
      snprintf(config->key_dir, sizeof(config->key_dir), "%s" MOONLIGHT_PATH, xdg_cache_dir);
    } else {
      const char *home_dir = getenv("HOME");
      if (home_dir && home_dir[0] != '\0') {
        snprintf(config->key_dir, sizeof(config->key_dir), "%s" DEFAULT_CACHE_DIR MOONLIGHT_PATH, home_dir);
      } else {
        snprintf(config->key_dir, sizeof(config->key_dir), "%s" DEFAULT_CACHE_DIR MOONLIGHT_PATH, "");
      }
    }
  }

  if (config->stream.fps == -1)
    config->stream.fps = config->stream.height >= 1080 ? 30 : 60;

  if (config->stream.bitrate == -1) {
    if (config->stream.height >= 1080 && config->stream.fps >= 60)
      config->stream.bitrate = 20000;
    else if (config->stream.height >= 1080 || config->stream.fps >= 60)
      // Custom build (2026-10-16): 720p60 was 10000 Kbps, which the
      // PSV WiFi + ~20% FEC overhead cannot hold; 5000 is stable.
      config->stream.bitrate = 5000;
    else
      config->stream.bitrate = 3000;
  }

  if (inputAdded) {
    if (!mapped) {
        fprintf(stderr, "Mapping option should be followed by the input to be mapped.\n");
        exit(-1);
    } else if (config->mapping == NULL) {
        fprintf(stderr, "Please specify mapping file as default mapping could not be found.\n");
        exit(-1);
    }
  }
}
