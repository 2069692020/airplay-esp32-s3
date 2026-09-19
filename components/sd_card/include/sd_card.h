#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * SD 卡存储 (SPI 模式, FATFS)
 *
 * 把 SD 卡挂到 /sdcard, 并提供目录枚举 / 删除 / 建目录 + 文件名转码。
 * 播放逻辑在 sd_player 组件里, 本组件只管"存储"这一层 —— 和
 * components/spiffs_storage 的定位一样。
 *
 * ⚠️ 路径约定: 本组件对外的路径一律是**卡内相对路径**, 以 '/' 开头, 例如
 *    "/Music/周杰伦/七里香.mp3"。/sdcard 前缀只在本组件内部拼, 上层(网页、
 *    sd_player)看不到它 —— 这样面包屑显示出来才是 "/Music", 而不是
 *    "/sdcard/Music"。
 *
 * ⚠️ 文件名编码: ESP-IDF 的 FATFS 没有内建 UTF-8 支持, readdir() 返回的是
 *    FF_CODE_PAGE 对应的字节。本项目设成 936 (见 config/sdkconfig.defaults 的
 *    CONFIG_FATFS_CODEPAGE_936), 所以拿到的是 GBK 字节, 本组件负责转成 UTF-8
 *    再返回; 反方向 (上传文件名 UTF-8 -> 写盘用的 GBK) 也由本组件负责。
 *    见 filename_enc.c 和 scripts/gen_gbk_table.py 的文件头说明。
 */

/** 挂载点。上层不要直接拼这个前缀, 用 sd_card_full_path()。 */
#define SD_CARD_MOUNT_POINT "/sdcard"

/** 单个文件名 (UTF-8) 的缓冲上限, 含结尾 '\0'。 */
#define SD_CARD_NAME_MAX 256

/** 卡内相对路径的缓冲上限, 含结尾 '\0'。 */
#define SD_CARD_PATH_MAX 256

/** 单次枚举的条目上限。超出置 truncated, 不静默丢弃。 */
#define SD_CARD_LIST_MAX 512

/** 目录条目。name 已转成 UTF-8。 */
typedef struct {
  char name[SD_CARD_NAME_MAX];
  uint64_t size; // 目录填 0
  bool is_dir;
} sd_entry_t;

/**
 * 初始化 SPI 总线并尝试挂载 SD 卡。可重复调用。
 *
 * ⚠️ 没插卡是**常态**, 不是错误 —— 返回非 ESP_OK 时上层只记一条警告即可,
 *    AirPlay 和其它功能不受影响。挂载失败时**不会**格式化卡
 *    (format_if_mount_failed = false): 用户的音乐不能被我们清掉。
 *
 * @return ESP_OK 已挂载; ESP_ERR_NOT_FOUND 没插卡/卡不识别;
 *         ESP_ERR_INVALID_STATE SPI 总线起不来; ESP_FAIL 挂载失败
 */
esp_err_t sd_card_init(void);

/**
 * 未挂载时重试挂载 —— 让用户"热插卡"不用重启设备。
 *
 * 带 10 秒节流: 每次调用都会看上次尝试的时间, 间隔不到就直接返回上次的结果。
 * 网页上每次拉目录/状态都可以放心调它。
 */
esp_err_t sd_card_ensure_mounted(void);

bool sd_card_is_mounted(void);

/**
 * 取卡容量。未挂载返回 ESP_ERR_INVALID_STATE。
 * @param total      总字节数, 可为 NULL
 * @param free_bytes 剩余字节数, 可为 NULL
 */
esp_err_t sd_card_info(uint64_t *total, uint64_t *free_bytes);

/**
 * 枚举一个目录。目录排前面, 同类按文件名 strcmp 升序。
 *
 * ⚠️ 排序用的是**转换后的 UTF-8 字节序**, 对中文是按 Unicode 码点排 (不是拼音)。
 *    文件名带 "01 "/"02 " 这类序号前缀时顺序是正确的 —— 这也是绝大多数人整理
 *    音乐的方式。若纯中文名且没有序号, 顺序看起来会有点随机, 但稳定。
 *    (按 GBK 字节排才是拼音序, 但那需要额外缓存一份 GBK 名字, 多占 ~130KB
 *     PSRAM, 不划算。)
 *
 * @param dir       卡内相对路径, "/" 表示根目录
 * @param out       输出数组 (PSRAM 分配), 调用者用 sd_card_free_list() 释放
 * @param count     输出条目数
 * @param truncated 非 NULL 时输出是否因为超过 SD_CARD_LIST_MAX 被截断
 */
esp_err_t sd_card_list(const char *dir, sd_entry_t **out, size_t *count,
                       bool *truncated);

/** 释放 sd_card_list() 返回的数组。传 NULL 安全。 */
void sd_card_free_list(sd_entry_t *list);

/** 删除文件或**空**目录。 */
esp_err_t sd_card_delete(const char *path);

/** 建目录 (父目录必须已存在)。 */
esp_err_t sd_card_mkdir(const char *dir);

/** 按后缀判断是不是能播的音频文件 (.mp3 .flac .wav .m4a .aac .ogg .oga)。 */
bool sd_card_is_audio_file(const char *name);

/**
 * 校验一个卡内相对路径: 必须以 '/' 开头、不含 ".."、不含控制字符、长度够。
 * 网页传来的路径必须先过这一关。返回 ESP_OK 才算安全。
 */
esp_err_t sd_card_path_check(const char *path);

/**
 * 拼接出可直接给 fopen()/opendir() 用的 VFS 路径 (即 "/sdcard" + path)。
 *
 * ⚠️ **传 UTF-8，不要自己转 GBK。** 本函数内部会做 UTF-8 -> GBK (FATFS 码页
 *    936)，所以 sd_card_list() / sd_card_delete() / sd_card_mkdir() 的调用方
 *    一律给 UTF-8 —— 和它们回给网页的编码一致，路径可以原样回传。
 *    调用方**预转**会变成二次编码，中文名字直接坏掉。
 */
esp_err_t sd_card_full_path(const char *path, char *out, size_t out_len);

/** GBK -> UTF-8。遇到无法映射的字节输出 '?'。out 一定以 '\0' 结尾。 */
void sd_card_gbk_to_utf8(const char *in, char *out, size_t out_len);

/** UTF-8 -> GBK。遇到 GBK 表示不了的字符输出 '?'。out 一定以 '\0' 结尾。 */
void sd_card_utf8_to_gbk(const char *in, char *out, size_t out_len);
