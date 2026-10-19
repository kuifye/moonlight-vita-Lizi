#include "ui_connect.h"
#include "i18n.h"

#include "Limelight.h"
#include "errors.h"
#include "guilib.h"
#include "ime.h"
#include "ui_settings.h"

#include "../connection.h"
#include "../configuration.h"
#include "../video.h"
#include "../config.h"
#include "../util.h"
#include "../device.h"
#include "../debug.h"

#include "client.h"
#include "errors.h"
#include "../platform.h"

#include "../power/vita.h"
#include "../input/vita.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <vita2d.h>

SERVER_DATA server;
PAPP_LIST server_applist;
int pos[2];

// ---------------------------------------------------------------------------
// Custom build (2026-10-16): persistent PIN prompt during pairing.
//
// The stock code shows the pairing PIN with flash_message(), which renders a
// single frame (~16 ms) and returns immediately, so the PIN is effectively
// invisible. gs_pair() then blocks until the PIN is entered in the Sunshine
// web UI (https://<host>:47990 -> PIN page) with no on-screen indication.
//
// This helper runs gs_pair() in a background thread and keeps the PIN plus
// the web UI URL on screen while it waits for the user to enter the PIN.
// ---------------------------------------------------------------------------
typedef struct {
  PSERVER_DATA server;
  char pin[5];
  int result;
  volatile int done;
} pin_pair_job_t;

static int pin_pair_thread_entry(SceSize args, void *argp) {
  // sceKernelStartThread() copies the argument block onto the new
  // thread's stack, so argp points at a COPY. Pass a pointer to the
  // job instead of the job itself, otherwise job->done = 1 writes
  // into the copy and the waiting loop spins forever.
  pin_pair_job_t *job = *(pin_pair_job_t **)argp;
  job->result = gs_pair(job->server, job->pin);
  job->done = 1;
  return 0;
}

static int gs_pair_with_pin_prompt(PSERVER_DATA server, char *pin) {
  // static, not stack-local: when the watchdog gives up, the helper
  // thread may still be running and must not write into a stack frame
  // that has already returned.
  static pin_pair_job_t job;
  pin_pair_job_t *jobptr = &job;
  job.server = server;
  job.pin[0] = pin[0];
  job.pin[1] = pin[1];
  job.pin[2] = pin[2];
  job.pin[3] = pin[3];
  job.pin[4] = '\0';
  job.result = GS_ERROR;
  job.done = 0;

  vita_debug_log("[pair] gs_pair start: host=%s pin=%s",
                 server->serverInfo.address, pin);
  // 1 MiB stack: gs_pair() pulls in curl + OpenSSL on top of its own
  // frame, which is a lot more than the 256 KiB the first build used.
  SceUID th = sceKernelCreateThread("pin_pair", pin_pair_thread_entry,
                                    0x10000100, 0x100000, 0, 0, NULL);
  if (th < 0) {
    // Could not spawn the helper thread: fall back to the stock behaviour.
    flash_message("Please enter the following PIN\non the target PC:\n\n%s", pin);
    return gs_pair(server, pin);
  }
  sceKernelStartThread(th, sizeof(jobptr), &jobptr);

  // Keep the PIN on screen until pairing finishes (gs_pair returns after the
  // PIN has been accepted on the PC).
  int wait_ticks = 0;
  while (!job.done) {
    flash_message("Pairing - waiting for PIN...\n\n"
                  "Enter this PIN on your PC:\n\n%s\n\n"
                  "Sunshine web UI:\nhttps://%s:47990\n(PIN page)",
                  pin, server->serverInfo.address);
    sceKernelDelayThread(50 * 1000);
    if (++wait_ticks % 20 == 0)
      vita_debug_log("[pair] still waiting for PIN approval (%d s)", wait_ticks / 20);
    // Watchdog. A helper thread that crashed (or a request that hangs
    // despite the curl timeouts) never sets job.done, and without this
    // the UI would render the PIN screen forever.
    // 3600 ticks x 50 ms = 180 s.
    if (wait_ticks >= 3600) {
      vita_debug_log("[pair] WATCHDOG: no result after %d s -- abandoning wait",
                     wait_ticks / 20);
      flash_message("Pairing did not finish within %d s.\n"
                    "See ux0:data/moonlight/moonlight.log\nand try again.",
                    wait_ticks / 20);
      sceKernelDelayThread(2500 * 1000);
      return GS_FAILED;
    }
  }

  int result = job.result;
  sceKernelWaitThreadEnd(th, NULL, NULL);
  sceKernelDeleteThread(th);
  vita_debug_log("[pair] gs_pair returned %d (%s)", result,
                 gs_error ? gs_error : "no error text");
  return result;
}

int get_app_id(PAPP_LIST list, char *name) {
  while (list != NULL) {
    if (strcmp(list->name, name) == 0)
      return list->id;

    list = list->next;
  }
  return -1;
}

int get_app_name(PAPP_LIST list, int id, char *name) {
  while (list != NULL) {
    if (list->id == id) {
      strcpy(name, list->name);
      return 1;
    }

    list = list->next;
  }

  return 0;
}

void ui_connect_stream(int appId) {
  // TODO support force controller id
  vita_debug_log(TR("Running gs_start_app...", "正在启动应用..."));
  int ret = gs_start_app(&server, &config.stream, appId, config.sops, config.localaudio, 1);
  if (ret < 0) {
    if (ret == GS_NOT_SUPPORTED_4K)
      display_error("Server doesn't support 4K\n");
    else if (ret == GS_NOT_SUPPORTED_MODE)
      display_error("Server doesn't support %dx%d (%d fps)\n", config.stream.width, config.stream.height, config.stream.fps);
    else
      display_error("Errorcode starting app: %d\n", ret);

    return;
  }
  vita_debug_log(TR("App started.", "应用已启动。"));

  enum platform system = VITA;
  int drFlags = 0;

  if (config.fullscreen)
    drFlags |= DISPLAY_FULLSCREEN;

  DECODER_RENDERER_CALLBACKS *video_callback = platform_get_video(system);
  if (config.enable_ref_frame_invalidation) {
    video_callback->capabilities |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC;
  } else {
    video_callback->capabilities &= ~CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC;
  }

  LiSetVitaFecRecoveryMode(config.fec_recovery_mode);
  vita_debug_log("local FEC recovery mode=%d (0=off, 1=bounded: data<=32 missing<=2 attempts<=4/s)", config.fec_recovery_mode);
  // always reset both branches when reconnecting after a setting change.
  if (!config.enable_split_decode_thread) {
    video_callback->capabilities |= CAPABILITY_DIRECT_SUBMIT;
  }
  vita_debug_log("fec: readiness/full-data fast paths restored; split=%d IDR cooldown=250ms queue=15 frames", config.enable_split_decode_thread);
  if (config.enable_split_decode_thread) {
    video_callback->capabilities &= ~CAPABILITY_DIRECT_SUBMIT;
    vita_debug_log(TR("Decode thread split enabled.", "解码线程已拆分。"));
  }

  // --- Ajuste para soporte Host Resolution: no modificar resolución si es -1 ---
  // Eliminados g_requested_width y g_requested_height, lógica simplificada
  int orig_width = config.stream.width;
  int orig_height = config.stream.height;
  // Si se seleccionó una resolución específica, se mantiene; si es -1, se deja al host
  // (No se modifica config.stream.width/height aquí)

  // Custom build (2026-10-18): hand the network knobs to moonlight-common-c.
  // Without this, SdpGenerator.c falls back to its original hardcoded values.
  {
    CLIENT_NETWORK_TUNING tuning;
    tuning.maxNumReferenceFrames = config.video_max_ref_frames;
    tuning.slicesPerFrame = config.video_slices_per_frame;
    tuning.minRequiredFecPackets = config.fec_min_required_packets;
    tuning.consecutiveDropLimit = config.consecutive_drop_limit;
    LiSetNetworkTuning(&tuning);
    vita_debug_log(TR("network tuning: refFrames=%d slices=%d fecMinPkts=%d dropLimit=%d",
                       "网络参数：参考帧=%d 切片=%d FEC最少校验包=%d 丢帧上限=%d"),
                     tuning.maxNumReferenceFrames, tuning.slicesPerFrame,
                     tuning.minRequiredFecPackets, tuning.consecutiveDropLimit);
    vita_debug_log(TR("streamingRemotely=%d (0=LAN 1=remote 2=auto), packetSize=%d",
                       "streamingRemotely=%d（0=局域网 1=远程 2=自动），包大小=%d"),
                     config.stream.streamingRemotely, config.stream.packetSize);
  }

  vita_debug_log(TR("Running LiStartConnection...", "正在启动串流连接..."));
  ret = LiStartConnection(&server.serverInfo, &config.stream, &connection_callbacks,
                          video_callback, platform_get_audio(system),
                          NULL, drFlags, NULL, 0);
  vita_debug_log(TR("Connection started.", "连接已开始。"));

  // Restaurar resolución real para el framebuffer/render
  config.stream.width = orig_width;
  config.stream.height = orig_height;

  if (ret == 0) {
    server.currentGame = appId;
  } else {
    
    const char* connection_failed_stage_name = LiGetStageName(connection_stage);

    display_error("Failed to start stream:\nFailed stage: %s\n(error code %d)",
                  connection_failed_stage_name, ret);
    return;
  }
}

enum {
  CONNECT_PAIRUNPAIR = 13,
  CONNECT_DISCONNECT,
  CONNECT_QUITAPP,
  // Custom build (2026-10-16): negative, so it can never collide with
  // a Sunshine app id coming back from the server.
  CONNECT_SETTINGS = -2000
};

#define QUIT_RELOAD 2

int ui_connect_loop(int id, void *context, const input_data *input) {
  int status = connection_get_status();

  if (status == LI_DISCONNECTED) {
      goto disconnect;
  }

  menu_entry *menu = context;
  for (int i = pos[0]; i < pos[1]; i += 1) {
    menu[i].disabled = (server.currentGame != 0);
  }

  if ((input->buttons & config.btn_confirm) == 0 || input->buttons & SCE_CTRL_HOLD) {
    return 0;
  }

  int ret;

  switch (id) {
    case CONNECT_PAIRUNPAIR:
      if (server.paired) {
        flash_message(TR("Unpairing...", "正在取消配对..."));
        ret = gs_unpair(&server);
        if (ret == GS_OK) {
          if (connection_terminate()) {
            display_error(TR("Reconnect failed: %d", "重连失败：%d"), -1);
            return 0;
          }
          return QUIT_RELOAD;
        }
        display_error(TR("Unpairing failed: %d", "取消配对失败：%d"), ret);
        return 0;
      }

      char pin[5];
      sprintf(pin, "%d%d%d%d",
              (uint32_t)rand() % 10, (uint32_t)rand() % 10, (uint32_t)rand() % 10, (uint32_t)rand() % 10);
      vita_debug_log("[UI] gs_pair: calling gs_pair at %u", (unsigned)time(NULL));
      ret = gs_pair_with_pin_prompt(&server, pin);
      vita_debug_log("[UI] gs_pair: returned ret=%d at %u", ret, (unsigned)time(NULL));
      if (ret == 0) {
        connection_paired();
        // After pairing, save server MAC into known device if present
        device_info_t *dev = find_device_by_address(server.serverInfo.address);
        if (dev) {
          dev->paired = true;
          char mac[18] = {0};
          if (gs_get_server_mac(&server, mac, sizeof(mac)) == GS_OK && mac[0]) {
            strncpy(dev->mac, mac, 17);
            dev->mac[17] = '\0';
            vita_debug_log("[PAIR] MAC obtenida y guardada en device.ini: %s for device %s", dev->mac, dev->name);
          } else {
            vita_debug_log("[PAIR] No se encontró MAC en serverinfo para %s", server.serverInfo.address);
          }
          save_device_info(dev);
          vita_debug_log("[PAIR] After save_device_info(dev) - time: %u", (unsigned)time(NULL));
          // Notify user pairing succeeded: show a short message so the PIN dialog
          // (which was drawn earlier) is replaced by a success message.
          flash_message(TR("Paired: %s", "已配对：%s"), dev->name);
        } else {
          vita_debug_log("[PAIR] Dispositivo no encontrado para la IP %s al guardar MAC", server.serverInfo.address);
        }
        if (connection_terminate()) {
          display_error(TR("Reconnect failed: %d", "重连失败：%d"), -2);
          return 0;
        }
        return QUIT_RELOAD;
      }
      display_error("Pairing failed: %d\n Error: %s", ret, gs_error);
      return 0;

    // Custom build (2026-10-16): stream settings from the connected
    // menu.  QUIT_RELOAD re-enters ui_connected_menu(), which re-reads
    // the app list; the connection itself is not torn down.
    case CONNECT_SETTINGS:
      ui_settings_menu();
      ui_settings_save_config();
      return QUIT_RELOAD;

    case CONNECT_DISCONNECT:
      goto disconnect;

    case CONNECT_QUITAPP:
      flash_message(TR("Quitting...", "正在退出..."));
      ret = gs_quit_app(&server);
      if (ret == GS_OK) {
        connection_paired();
        server.currentGame = 0;
        return QUIT_RELOAD;
      }
      display_error(TR("Quitting failed: %d", "退出失败：%d"), ret);
      return 0;

    // application launcher / resume
    default:
      vitapower_config(config);
      vitainput_config(config);

      switch (status) {
        case LI_MINIMIZED:
          if (server.currentGame == id) {
            sceKernelDelayThread(500 * 1000);
            connection_resume();
            break;
          }
          // TODO: stop previous stream
        case LI_PAIRED:
          flash_message(TR("Stream starting...", "正在开始串流..."));
          ui_connect_stream(id);
          break;
      }

//mainloop:
      while (connection_is_connected()) {
        sceKernelDelayThread(500 * 1000);
      }

      int status = connection_get_status();

      if (status == LI_DISCONNECTED) {
          goto disconnect;
      }

      return QUIT_RELOAD;
  }

disconnect:
  flash_message(TR("Disconnecting...", "正在断开..."));
  connection_terminate();
  sceKernelDelayThread(1000 * 1000);
  return 1;
}

int ui_connect(char *name, char *address, uint16_t port) {
  int ret;
  if (!connection_is_ready()) {
    flash_message("Connecting to:\n %s:%d...", address, port);

    char key_dir[4096];
    sprintf(key_dir, "%s/%s", config.key_dir, name);

    ret = gs_init(&server, address, port, key_dir, 0, true);
    if (ret == GS_OUT_OF_MEMORY) {
      display_error(TR("Not enough memory", "内存不足"));
      return 0;
    } else if (ret == GS_INVALID) {
      display_error("Invalid data received from server: %s\n", address, gs_error);
      return 0;
    } else if (ret == GS_UNSUPPORTED_VERSION) {
      if (!config.unsupported_version) {
        display_error("Unsupported version: %s\n", gs_error);
        return 0;
      }
    } else if (ret == GS_ERROR) {
      display_error("Gamestream error: %s\n", gs_error);
      return 0;
    } else if (ret != GS_OK) {
      display_error("Can't connect to server\n%s", address);
      return 0;
    }

    connection_reset();
  }
  return 1;
}

int ui_connected_menu() {
  int ret;
  int app_count = 0;
  if (server.paired) {
    ret = gs_applist(&server, &server_applist);
    if (ret != GS_OK) {
      display_error("Can't get applist!\n%d\n%s", ret, gs_error);
      return 0;
    }

    if (server_applist != NULL) {
      sort_app_list(server_applist);
      PAPP_LIST list = server_applist;
      while (list) {
        list = list->next;
        app_count += 1;
      }
    }
  }

  // current menu = 11 + app_count. but little more alloc ;)
  struct menu_entry menu[app_count + 16];

  int idx = 0;

#define MENU_CATEGORY(NAME) \
  do { \
    menu[idx] = (menu_entry) { .name = (NAME), .disabled = true, .separator = true }; \
    idx++; \
  } while (0)
#define MENU_ENTRY(ID, NAME) \
  do { \
    menu[idx] = (menu_entry) { .name = (NAME), .id = (ID) }; \
    idx++; \
  } while(0)
#define MENU_MESSAGE(MESSAGE, COLOR) \
  do { \
    menu[idx] = (menu_entry) { .name = (MESSAGE), .disabled = true, .color = (COLOR) }; \
    idx++; \
  } while(0)

  //header
  //MENU
  MENU_MESSAGE(TR("Connected to the server:", "已连接到服务器："), 0xffffffff);
  char server_info[256];
  unsigned short port = server.httpPort ? server.httpPort : server.httpsPort;
  snprintf(server_info, sizeof(server_info),
           TR("IP: %s  PORT: %d, GPU %s, GFE %s", "IP：%s  端口：%d，GPU %s，GFE %s"),
           server.serverInfo.address,
           port,
           server.gpuType,
           server.serverInfo.serverInfoGfeVersion);

  MENU_MESSAGE(server_info, 0xffffffff);
  MENU_MESSAGE("", 0);

  if (!server.paired) {
    // pairing
    MENU_CATEGORY(TR("Not paired", "未配对"));
    MENU_ENTRY(CONNECT_PAIRUNPAIR, TR("Pair", "配对"));

    MENU_ENTRY(CONNECT_DISCONNECT, TR("Disconnect", "断开连接"));
  } else {
    // current stream
    if (server.currentGame != 0) {
      char current_appname[256];
      char current_status[256];

      if (!get_app_name(server_applist, server.currentGame, current_appname)) {
        strcpy(current_appname, TR("unknown", "未知"));
      }
      sprintf(current_status, TR("Streaming %s", "正在串流 %s"), current_appname);

      MENU_CATEGORY(current_status);
      MENU_ENTRY(server.currentGame, TR("Resume", "恢复"));
      MENU_ENTRY(CONNECT_QUITAPP, TR("Quit", "退出"));
    }

    // pairing
    MENU_CATEGORY(TR("Paired", "已配对"));
    connection_paired();
    // FIXME: unpair not work
    // MENU_ENTRY(CONNECT_PAIRUNPAIR, TR("Unpair", "取消配对"));

    MENU_ENTRY(CONNECT_DISCONNECT, TR("Disconnect", "断开连接"));

    // Custom build (2026-10-16): the stream settings used to be
    // reachable only from the main menu, which is easy to miss once
    // you are connected.  Put them right next to the app list.
    MENU_ENTRY(CONNECT_SETTINGS, TR("Stream settings", "串流设置"));

    // app list
    if (server_applist != NULL) {
      MENU_CATEGORY(TR("Applications", "应用列表"));

      pos[0] = idx;

      PAPP_LIST list = server_applist;
      while (list) {
        MENU_ENTRY(list->id, list->name);
        list = list->next;
      }

      pos[1] = idx;
    } else {
      pos[0] = -1;
    }
  }

  return display_menu(menu, idx, NULL, &ui_connect_loop, NULL, NULL, &menu);
}

device_info_t* ui_connect_and_pairing(device_info_t *info) {
  flash_message(TR("Test connecting to:\n %s...", "正在测试连接：\n %s..."), info->internal);
  vita_debug_log("[CONNECT] Intentando conectar: name=%s, ip=%s, port=%d", info->name, info->internal, info->port);
  char key_dir[4096];
  sprintf(key_dir, "%s/%s", config.key_dir, info->name);
  sceIoMkdir(key_dir, 0777);

  int ret = gs_init(&server, info->internal, info->port, key_dir, 0, true);

  if (ret == GS_OUT_OF_MEMORY) {
    display_error(TR("Not enough memory", "内存不足"));
    return NULL;
  } else if (ret == GS_INVALID) {
    display_error("Invalid data received from server: %s\n", info->internal, gs_error);
    return NULL;
  } else if (ret == GS_UNSUPPORTED_VERSION) {
    if (!config.unsupported_version) {
      display_error("Unsupported version: %s\n", gs_error);
      return NULL;
    }
  } else if (ret == GS_ERROR) {
    display_error("Gamestream error: %s\n", gs_error);
    return NULL;
  } else if (ret != GS_OK) {
    display_error("Can't connect to server\n%s", info->internal);
    return NULL;
  }

  connection_reset();
  vita_debug_log("[CONNECT] gs_init done: paired=%d httpPort=%u httpsPort=%u majorVer=%d",
                 (int)server.paired, (unsigned)server.httpPort,
                 (unsigned)server.httpsPort, (int)server.serverMajorVersion);

  device_info_t *p = append_device(info);
  if (p == NULL) {
    // Already in the list (a retry after a failed pairing, or a
    // leftover folder). append_device() returned NULL, so `info`
    // still points at the caller's local struct -- every write
    // below, including info->paired = true, would land on that
    // stack copy and never reach the list. Re-point at the live
    // entry so the paired flag survives.
    p = find_device(info->name);
    if (p == NULL) {
      display_error(TR("Could not update device info", "无法更新设备信息"));
      connection_terminate();
      return NULL;
    }
    update_device(info);
  }
  info = p;

  // connectable address
  save_device_info(info);
  vita_debug_log("[PAIR] After save_device_info(info) - time: %u", (unsigned)time(NULL));
  // Notify user pairing succeeded
  flash_message(TR("Paired: %s", "已配对：%s"), info->name);

  if (server.paired) {
    // no more need, move next action
    goto paired;
  }

  char pin[5];
  sprintf(pin, "%d%d%d%d",
          (int)rand() % 10, (int)rand() % 10, (int)rand() % 10, (int)rand() % 10);
  ret = gs_pair_with_pin_prompt(&server, pin);
  if (ret != GS_OK) {
    display_error(TR("Pairing failed: %d", "配对失败：%d"), ret);
    connection_terminate();
    return NULL;
  }

paired:
  connection_paired();

  info->paired = true;

  // Preferimos usar la MAC ya obtenida en serverinfo (XML) si está disponible
  char mac[18] = {0};
  if (gs_get_server_mac(&server, mac, sizeof(mac)) == GS_OK && mac[0]) {
    strncpy(info->mac, mac, 17);
    info->mac[17] = '\0';
    vita_debug_log("[PAIR] MAC obtenida tras pairing (serverinfo): %s", info->mac);
  } else {
    vita_debug_log("[PAIR] No se encontró MAC en serverinfo; no se guarda MAC automáticamente");
  }
  save_device_info(info);

  if (connection_terminate()) {
    display_error(TR("Reconnect failed: %d", "重连失败：%d"), -2);
    return info;
  }

  return info;
}

void ui_connect_resume() {
  while (ui_connected_menu() == QUIT_RELOAD);
}

void ui_connect_manual() {
  device_info_t info = {0};
  if (ime_dialog_string(info.name, TR("Enter Name:", "输入名称："), "") != 0) {
    return;
  }
  if (ime_dialog_string(info.internal, TR("Enter IP or Address:", "输入 IP 或地址："), "") != 0) {
    return;
  }
  info.port = 47989;
  ui_connect_and_pairing(&info);
}

bool check_connection(const char *name, char *addr, uint16_t port) {
  // someone already connected
  if (connection_is_ready()) {
    return false;
  }

  flash_message("Check connecting to:\n %s:%d...", addr, port);

  int log_level = 0;
  if (config.save_debug_log) {
    log_level = 3;
  }

  char key_dir[4096];
  sprintf(key_dir, "%s/%s", config.key_dir, name);

  if (gs_init(&server, addr, port, key_dir, log_level, true) != GS_OK) {
    return false;
  }

  vita_debug_log(TR("Connection check succeded.", "连接检查成功。"));

  connection_terminate();
  return true;
}

void ui_connect_paired_device(device_info_t *info) {
  if (!info->paired) {
    display_error("Unpaired device\n%s", info->name);
    return;
  }

  char *addr = NULL;
  if (info->prefer_external && check_connection(info->name, info->external, info->port)) {
    addr = info->external;
  } else if (check_connection(info->name, info->internal, info->port)) {
    addr = info->internal;
  } else if (check_connection(info->name, info->external, info->port)) {
    addr = info->external;
  }
  info->prefer_external = addr == info->external;
  save_device_info(info);

  if (addr == NULL) {
    display_error("Can't connect to server\n%s", info->name);
    return;
  }

  if (!ui_connect(info->name, addr, info->port)) {
    return;
  }

  while (ui_connected_menu() == QUIT_RELOAD);
}

bool ui_connect_connected() {
  return connection_is_ready();
}

void ui_connect_address(char *addr) {
  strcpy(addr, server.serverInfo.address);
}
