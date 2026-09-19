/*
 * SD 卡存储层 (SPI 模式 + FATFS)
 *
 * 挂载流程用的是 ESP-IDF 5.5 的标准写法 (examples/storage/sd_card/sdspi):
 *   spi_bus_initialize(host.slot) -> esp_vfs_fat_sdspi_mount()
 * 两个要点:
 *
 * 1. ⚠️ SPI 主机用 **SPI3_HOST**, 不用 SDSPI_HOST_DEFAULT() 的 SPI2_HOST。
 *    SPI2 在这个项目里被 display_st7789.c 和 esparagus-audio-brick/board.c
 *    占着 —— 当前板子 (esp32s3-generic + SSD1306 走 I2C) 没用到, 但一旦以后
 *    换板子开了 ST7789 屏, SPI2 就会撞车。SPI3 在这块板子上完全空闲。
 *
 * 2. ⚠️ format_if_mount_failed = **false**。挂载失败绝不格式化 ——
 *    卡里是用户的音乐, 不能因为一次接触不良就被我们清掉。
 *
 * 没插卡是常态: 挂载失败只记一条警告, 上层 (main.c / web_server.c) 不把它
 * 当致命错误, AirPlay 和网络电台照常工作。sd_card_ensure_mounted() 允许热插卡
 * 后重试, 不用重启设备。
 */

#include "sd_card.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_card";

// VFS 路径缓冲: "/sdcard" + SD_CARD_PATH_MAX + 余量
#define SD_CARD_VFS_MAX (sizeof(SD_CARD_MOUNT_POINT) + SD_CARD_PATH_MAX + 8)
// 子项路径: VFS 路径 + '/' + 一个分量 (FATFS 的 d_name 最长 FF_MAX_LFN+1 字节)
#define SD_CARD_CHILD_MAX (SD_CARD_VFS_MAX + SD_CARD_NAME_MAX + 8)

// 热插卡重试的最小间隔。没插卡时每次拉目录都去探一次会白白卡住几百毫秒,
// 所以节流。
#define SD_CARD_RETRY_INTERVAL_US (10 * 1000 * 1000)

// s_mounted 在 #if 内外都用 (枚举/删除那几节也要判它), 所以放外面。
// 另外三个只有真正的挂载代码用, 放在 #if 里面 —— 否则关掉 SD 功能时会留下
// 三个"定义了但没人用"的静态变量。
static bool s_mounted = false;

// ---------------------------------------------------------------------------
// 路径
// ---------------------------------------------------------------------------

esp_err_t sd_card_path_check(const char *path) {
  if (!path || path[0] != '/') {
    return ESP_ERR_INVALID_ARG; // 必须是卡内绝对路径
  }
  if (strlen(path) >= SD_CARD_PATH_MAX) {
    return ESP_ERR_INVALID_ARG;
  }

  // 逐段扫: 拒绝控制字符 / 反斜杠 / ".." 段。
  // ".." 是这里唯一真正危险的东西 —— 它能跳出 /sdcard 去碰别的 VFS
  // (比如 SPIFFS 里的网页文件)。
  const char *p = path;
  while (*p) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c == '\\') {
      return ESP_ERR_INVALID_ARG;
    }
    p++;
  }

  const char *seg = path;
  while (*seg) {
    if (*seg == '/') {
      seg++;
      continue;
    }
    const char *end = strchr(seg, '/');
    size_t len = end ? (size_t)(end - seg) : strlen(seg);
    if (len == 2 && seg[0] == '.' && seg[1] == '.') {
      return ESP_ERR_INVALID_ARG;
    }
    if (!end) {
      break;
    }
    seg = end;
  }

  return ESP_OK;
}

esp_err_t sd_card_full_path(const char *path, char *out, size_t out_len) {
  if (!out || out_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = sd_card_path_check(path);
  if (err != ESP_OK) {
    return err;
  }

  // 根目录特判: "/sdcard" 比 "/sdcard/" 干净 (opendir 两者都认, 但日志好看)
  if (strcmp(path, "/") == 0) {
    if (out_len < sizeof(SD_CARD_MOUNT_POINT)) {
      return ESP_ERR_INVALID_SIZE;
    }
    strcpy(out, SD_CARD_MOUNT_POINT);
    return ESP_OK;
  }

  // ⚠️ UTF-8 -> GBK 就这一处做。FATFS 编的是 CP936，交给 opendir()/fopen()/
  //    f_mkdir()/f_unlink() 的路径必须是 GBK 字节。以前这个方向从没在组件里做
  //    (只有 sd_player 的 rel_to_vfs 和 /api/sd/upload 各自手转了一遍)，于是
  //    list / delete / mkdir 三条路拿到的是 UTF-8 —— 中文目录进不去、也删不掉。
  //    放在这个唯一的漏斗里，调用方一律传 UTF-8，就不会再有人忘记。
  //    UTF-8 里一个汉字 3 字节、GBK 2 字节，可映射字符永远不膨胀，所以 gbk[]
  //    不会装不下 (path_check 已经按 UTF-8 长度卡过 SD_CARD_PATH_MAX)。
  char gbk[SD_CARD_PATH_MAX];
  sd_card_utf8_to_gbk(path, gbk, sizeof(gbk));

  int n = snprintf(out, out_len, "%s%s", SD_CARD_MOUNT_POINT, gbk);
  if (n < 0 || (size_t)n >= out_len) {
    return ESP_ERR_INVALID_SIZE;
  }
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// 挂载
// ---------------------------------------------------------------------------

#if CONFIG_SD_CARD_ENABLED

static bool s_bus_inited = false;
static sdmmc_card_t *s_card = NULL;
static int64_t s_last_attempt_us = 0;

/* Serialises mount attempts.
 *
 * A mount blocks for hundreds of ms (bus bring-up + FAT scan), and three
 * independent callers can start one: main.c at boot, /api/sd/mount (the web
 * "重新挂载" button) and /api/sd/list via sd_card_ensure_mounted().  With 2-3
 * concurrent httpd sockets two of those really can overlap; both would pass the
 * s_mounted check and then race spi_bus_initialize() plus the s_bus_inited /
 * s_card / s_mounted writes, leaving the state torn.
 *
 * This is a spinlock rather than a mutex because it is only ever held across
 * the flag test-and-set, never across the mount itself — a mutex would need
 * lazy creation, which is its own race. */
static portMUX_TYPE s_mount_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_mounting = false;

static esp_err_t mount_card(void) {
  esp_err_t err;

  if (!s_bus_inited) {
    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_SD_MOSI_GPIO,
        .miso_io_num = CONFIG_SD_MISO_GPIO,
        .sclk_io_num = CONFIG_SD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // 必须 >= 4096: FATFS 一次读写一个扇区 (SD 卡是 512B), 但 DMA
        // 描述符按 max_transfer_sz 切分, 太小会退化成多次传输。
        .max_transfer_sz = 4096,
    };
    err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
      ESP_LOGE(TAG,
               "SPI3 总线初始化失败: %s (CS=%d SCLK=%d MISO=%d MOSI=%d)",
               esp_err_to_name(err), CONFIG_SD_CS_GPIO, CONFIG_SD_SCLK_GPIO,
               CONFIG_SD_MISO_GPIO, CONFIG_SD_MOSI_GPIO);
      return ESP_ERR_INVALID_STATE;
    }
    s_bus_inited = true;
    ESP_LOGI(TAG, "SPI3 总线已初始化 (CS=%d SCLK=%d MISO=%d MOSI=%d)",
             CONFIG_SD_CS_GPIO, CONFIG_SD_SCLK_GPIO, CONFIG_SD_MISO_GPIO,
             CONFIG_SD_MOSI_GPIO);
  }

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI3_HOST; // ⚠️ 默认是 SPI2_HOST, 必须覆盖 —— 见文件头说明
  host.max_freq_khz = CONFIG_SD_SPI_FREQ_KHZ;

  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.host_id = SPI3_HOST;
  slot.gpio_cs = CONFIG_SD_CS_GPIO;
  // 没有卡检测 (CD) / 写保护 (WP) 引脚 —— 用户那块卡座只有 4 根线。

  esp_vfs_fat_mount_config_t mcfg = {
      // ⚠️ 绝不格式化用户的卡
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024,
      .disk_status_check_enable = false,
      .use_one_fat = false,
  };

  err = esp_vfs_fat_sdspi_mount(SD_CARD_MOUNT_POINT, &host, &slot, &mcfg,
                                &s_card);
  if (err != ESP_OK) {
    // 挂载失败时 IDF 已经把自己注册的 slot/device 收回去了, 所以下次重试
    // 是干净的。不要在这里 spi_bus_free() —— 总线留着给重试复用。
    s_card = NULL;
    return err;
  }

  s_mounted = true;
  ESP_LOGI(TAG, "SD 卡已挂载: %s (%s)", SD_CARD_MOUNT_POINT,
           s_card->cid.name);
  return ESP_OK;
}

esp_err_t sd_card_init(void) {
  if (s_mounted) {
    return ESP_OK;
  }

  portENTER_CRITICAL(&s_mount_mux);
  if (s_mounting) {
    portEXIT_CRITICAL(&s_mount_mux);
    // Another caller is already bringing the card up — do not pile on.
    return ESP_ERR_INVALID_STATE;
  }
  s_mounting = true;
  portEXIT_CRITICAL(&s_mount_mux);

  s_last_attempt_us = esp_timer_get_time();

  esp_err_t err = mount_card();

  portENTER_CRITICAL(&s_mount_mux);
  s_mounting = false;
  portEXIT_CRITICAL(&s_mount_mux);

  if (err == ESP_OK) {
    return ESP_OK;
  }

  if (err == ESP_FAIL) {
    ESP_LOGW(TAG,
             "SD 卡挂载失败 (卡在, 但不是 FAT 文件系统?)。"
             "请把卡格式化成 FAT32/exFAT 后重插 —— 本固件不会格式化你的卡。");
  } else {
    ESP_LOGW(TAG,
             "SD 卡不可用: %s。没插卡是正常的, 插好后在网页上点「重新挂载」"
             "或重试拉取目录即可, 不用重启。接线: CS=%d SCLK=%d MISO=%d MOSI=%d",
             esp_err_to_name(err), CONFIG_SD_CS_GPIO, CONFIG_SD_SCLK_GPIO,
             CONFIG_SD_MISO_GPIO, CONFIG_SD_MOSI_GPIO);
  }
  return err;
}

esp_err_t sd_card_ensure_mounted(void) {
  if (s_mounted) {
    return ESP_OK;
  }
  // 节流: 没插卡时避免每次网页轮询都去探一次 (探测要几百毫秒)
  int64_t now = esp_timer_get_time();
  if (s_last_attempt_us && now - s_last_attempt_us < SD_CARD_RETRY_INTERVAL_US) {
    return ESP_ERR_NOT_FOUND;
  }
  return sd_card_init();
}

bool sd_card_is_mounted(void) { return s_mounted; }

esp_err_t sd_card_info(uint64_t *total, uint64_t *free_bytes) {
  if (!s_mounted) {
    return ESP_ERR_INVALID_STATE;
  }

  // 用 esp_vfs_fat_info 而不是自己 f_getfree() 算: 扇区大小在 IDF 里是运行时
  // 决定的 (FF_MIN_SS=512 != FF_MAX_SS=4096), 自己算容易把 sector_size 写死
  // 成 512 而算错容量。
  uint64_t t = 0, f = 0;
  esp_err_t err = esp_vfs_fat_info(SD_CARD_MOUNT_POINT, &t, &f);
  if (err != ESP_OK) {
    return err;
  }
  if (total) {
    *total = t;
  }
  if (free_bytes) {
    *free_bytes = f;
  }
  return ESP_OK;
}

#else // !CONFIG_SD_CARD_ENABLED

// 关掉 SD 功能时给一组空实现, 这样 main.c / web_server.c 里不用到处写 #ifdef。
esp_err_t sd_card_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_ensure_mounted(void) { return ESP_ERR_NOT_SUPPORTED; }
bool sd_card_is_mounted(void) { return false; }
esp_err_t sd_card_info(uint64_t *total, uint64_t *free_bytes) {
  (void)total;
  (void)free_bytes;
  return ESP_ERR_NOT_SUPPORTED;
}

#endif // CONFIG_SD_CARD_ENABLED

// ---------------------------------------------------------------------------
// 目录枚举
// ---------------------------------------------------------------------------

// 排序: 目录在前, 同类按文件名升序。
// ⚠️ 比较的是转换后的 UTF-8 字节序 —— 对中文是 Unicode 码点序, 不是拼音序。
//    带 "01 "/"02 " 序号前缀的文件名顺序是对的, 这也是绝大多数人整理音乐的方式。
//    理由见 sd_card.h 里 sd_card_list 的说明。
static int entry_cmp(const void *a, const void *b) {
  const sd_entry_t *x = (const sd_entry_t *)a;
  const sd_entry_t *y = (const sd_entry_t *)b;
  if (x->is_dir != y->is_dir) {
    return x->is_dir ? -1 : 1;
  }
  return strcmp(x->name, y->name);
}

esp_err_t sd_card_list(const char *dir, sd_entry_t **out, size_t *count,
                       bool *truncated) {
  if (out) {
    *out = NULL;
  }
  if (count) {
    *count = 0;
  }
  if (truncated) {
    *truncated = false;
  }
  if (!dir) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!s_mounted) {
    return ESP_ERR_INVALID_STATE;
  }

  char full[SD_CARD_VFS_MAX];
  esp_err_t err = sd_card_full_path(dir, full, sizeof(full));
  if (err != ESP_OK) {
    return err;
  }

  // 放 PSRAM: 512 条 x ~272 字节 ≈ 136KB, 内部 RAM 撑不住 (见 web_radio.c
  // 文件头第 1 条: 给文件/网络缓冲用内部 RAM 会把 httpd 挤爆)。
  sd_entry_t *list =
      heap_caps_calloc(SD_CARD_LIST_MAX, sizeof(sd_entry_t), MALLOC_CAP_SPIRAM);
  if (!list) {
    ESP_LOGE(TAG, "目录列表缓冲分配失败 (PSRAM, %u 字节)",
             (unsigned)(SD_CARD_LIST_MAX * sizeof(sd_entry_t)));
    return ESP_ERR_NO_MEM;
  }

  DIR *d = opendir(full);
  if (!d) {
    ESP_LOGW(TAG, "打不开目录: %s", full);
    heap_caps_free(list);
    return ESP_ERR_NOT_FOUND;
  }

  size_t n = 0;
  bool trunc = false;
  struct dirent *e;

  while ((e = readdir(d)) != NULL) {
    // "." / ".." 必须跳过 —— 上层拿去做 URL 会变成死循环的上一级
    if (e->d_name[0] == '\0' ||
        (e->d_name[0] == '.' &&
         (e->d_name[1] == '\0' ||
          (e->d_name[1] == '.' && e->d_name[2] == '\0')))) {
      continue;
    }

    if (n >= SD_CARD_LIST_MAX) {
      trunc = true; // 不静默丢弃, 由上层告诉用户
      break;
    }

    char child[SD_CARD_CHILD_MAX];
    int w = snprintf(child, sizeof(child), "%s/%s", full, e->d_name);
    if (w < 0 || (size_t)w >= sizeof(child)) {
      continue; // 路径过长, 跳过
    }

    struct stat st;
    if (stat(child, &st) != 0) {
      continue; // 读不到就当它不存在, 不阻断整个目录
    }

    sd_card_gbk_to_utf8(e->d_name, list[n].name, sizeof(list[n].name));
    if (list[n].name[0] == '\0') {
      continue; // 转换后为空 (纯非法字节), 跳过
    }
    list[n].is_dir = S_ISDIR(st.st_mode);
    list[n].size = list[n].is_dir ? 0 : (uint64_t)st.st_size;
    n++;
  }
  closedir(d);

  qsort(list, n, sizeof(sd_entry_t), entry_cmp);

  if (truncated) {
    *truncated = trunc;
  }
  if (trunc) {
    ESP_LOGW(TAG, "目录 %s 超过 %d 条, 已截断", dir, SD_CARD_LIST_MAX);
  }

  *out = list;
  if (count) {
    *count = n;
  }
  return ESP_OK;
}

void sd_card_free_list(sd_entry_t *list) { heap_caps_free(list); }

// ---------------------------------------------------------------------------
// 删除 / 建目录
// ---------------------------------------------------------------------------

esp_err_t sd_card_delete(const char *path) {
  if (!s_mounted) {
    return ESP_ERR_INVALID_STATE;
  }
  // 不允许删挂载根本身
  if (strcmp(path, "/") == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  char full[SD_CARD_VFS_MAX];
  esp_err_t err = sd_card_full_path(path, full, sizeof(full));
  if (err != ESP_OK) {
    return err;
  }

  struct stat st;
  if (stat(full, &st) != 0) {
    return ESP_ERR_NOT_FOUND;
  }

  // 目录要用 rmdir (VFS 的 remove/unlink 对目录返回 EISDIR)。
  // 非空目录 rmdir 会失败 —— 这是有意的: 不做递归删除, 免得一次误点
  // 把整张专辑目录清掉。
  int rc = S_ISDIR(st.st_mode) ? rmdir(full) : remove(full);
  if (rc != 0) {
    ESP_LOGW(TAG, "删除失败: %s (目录非空?)", full);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "已删除: %s", path);
  return ESP_OK;
}

esp_err_t sd_card_mkdir(const char *dir) {
  if (!s_mounted) {
    return ESP_ERR_INVALID_STATE;
  }

  char full[SD_CARD_VFS_MAX];
  esp_err_t err = sd_card_full_path(dir, full, sizeof(full));
  if (err != ESP_OK) {
    return err;
  }

  if (mkdir(full, 0777) != 0) {
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "已建目录: %s", dir);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// 音频后缀
// ---------------------------------------------------------------------------

// 大小写无关的后缀比较。ext 传小写。
static bool has_ext(const char *name, const char *ext) {
  if (!name || !ext) {
    return false;
  }
  size_t nl = strlen(name);
  size_t el = strlen(ext);
  if (nl < el) {
    return false;
  }
  return strcasecmp(name + nl - el, ext) == 0;
}

bool sd_card_is_audio_file(const char *name) {
  // 认这几种就够了 —— 解码器那边 (esp_audio_simple_dec) 的覆盖面见 sd_player.c。
  // 不支持 APE/WMA/DSD, 那些列出来也放不了, 不如不列。
  return has_ext(name, ".mp3") || has_ext(name, ".flac") ||
         has_ext(name, ".wav") || has_ext(name, ".m4a") ||
         has_ext(name, ".aac") || has_ext(name, ".ogg") ||
         has_ext(name, ".oga");
}
