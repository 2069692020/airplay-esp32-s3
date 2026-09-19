#pragma once

#include <stddef.h>

/**
 * 曲目标签解析 (内部头, 只给 sd_player.c 用)。
 * 实现见 tags.c —— 从 components/web_radio/web_radio.c 移植。
 */

/**
 * 读文件头, 解析出标题 / 作者 / 专辑。任何一项解不出来就留空串 (不报错)。
 * 支持 ID3v2 (MP3) 和 Vorbis comment (FLAC); 其它格式直接返回 (三个串都空)。
 *
 * @param vfs_path 可直接给 fopen 的路径 (即带 /sdcard 前缀的 GBK 路径)
 */
void sd_tags_read(const char *vfs_path, char *title, size_t title_len,
                  char *artist, size_t artist_len, char *album,
                  size_t album_len);

/**
 * 回退链最后一环: 从文件名猜标题 (去扩展名)。name 可以是全路径。
 * 结果永远非空 —— 实在没有就用 "未知曲目"。
 */
void sd_title_from_name(const char *name, char *out, size_t out_len);
