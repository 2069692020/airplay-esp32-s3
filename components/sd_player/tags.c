/*
 * 曲目标签解析 (ID3v2.4 / FLAC Vorbis comment)
 *
 * ⚠️ 本文件是从 components/web_radio/web_radio.c 的标签解析部分移植过来的
 *    (函数 tag_parse_id3v2 / tag_parse_vorbis / copy_trim / syncsafe32)。
 *    那边解析的是 HTTP 预读回来的字节, 这边是 fopen 读本地文件 —— 字节流本身
 *    一样, 所以解析逻辑**刻意保持一致**。改这里时请对照那边一起改。
 *
 * 与网络播放的一点差异: 本地文件可以随便读 (没有墙钟预算、没有"服务器慢慢滴"
 * 的问题), 所以预读窗口给到 16KB, 比 web_radio 的 8KB 宽 —— FLAC 夹了封面图
 * 时 VORBIS_COMMENT 可能排在 PICTURE 之后, 窗口大一点能多救回来一些文件。
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "tags.h"

static const char *TAG = "sd_tags";

// 标签预读窗口。只读文件开头这么多字节, 读不满也无所谓。
#define TAG_PROBE_BYTES (16 * 1024)

// 拷贝并截断, 顺便去掉首尾空白和 UTF-8 BOM。
// (与 web_radio.c:481 一致)
static void copy_trim(char *dst, size_t dst_len, const char *src,
                      size_t src_len) {
  if (!dst || dst_len == 0) {
    return;
  }
  dst[0] = '\0';
  if (!src || src_len == 0) {
    return;
  }

  // UTF-8 BOM (EF BB BF)
  if (src_len >= 3 && (unsigned char)src[0] == 0xEF &&
      (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
    src += 3;
    src_len -= 3;
  }

  // 去掉尾部的 '\0' 填充 —— ID3 的文本帧常带一串 \0, 不去掉的话
  // 传到 cJSON 里会变成截断的乱码。
  while (src_len > 0 && src[src_len - 1] == '\0') {
    src_len--;
  }
  while (src_len > 0 && (src[0] == ' ' || src[0] == '\t')) {
    src++;
    src_len--;
  }
  while (src_len > 0 && (src[src_len - 1] == ' ' || src[src_len - 1] == '\t' ||
                         src[src_len - 1] == '\r' || src[src_len - 1] == '\n')) {
    src_len--;
  }

  if (src_len >= dst_len) {
    src_len = dst_len - 1;
  }
  memcpy(dst, src, src_len);
  dst[src_len] = '\0';
}

// ID3v2 的帧长是 **syncsafe** 整数: 每字节只用低 7 位 (v2.4 规范)。
// 按普通大端解会把长度算错, 后面全乱。(web_radio.c:516)
static uint32_t syncsafe32(const unsigned char *p) {
  return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

// 解析 ID3v2 文本帧。只认 UTF-8 (编码字节 0x03); UTF-16 会被跳过 ——
// 国内老文件 v2.3 用 UTF-16 的不少, 那种情况回退到文件名。
static void tag_parse_id3v2(const unsigned char *buf, size_t len, char *title,
                            size_t title_len, char *artist, size_t artist_len,
                            char *album, size_t album_len) {
  if (len < 10 || memcmp(buf, "ID3", 3) != 0) {
    return;
  }
  if (buf[5] & 0x40) {
    return; // 有扩展头, 布局复杂, 放弃 (回退到文件名)
  }

  uint32_t tag_size = syncsafe32(buf + 6);
  size_t end = 10 + tag_size;
  if (end > len) {
    end = len; // 只读到了部分标签, 尽力而为
  }

  size_t pos = 10;
  while (pos + 10 <= end) {
    const unsigned char *hdr = buf + pos;
    if (hdr[0] == 0) {
      break; // 填到 padding 区了 (全 0)
    }

    char id[5] = {(char)hdr[0], (char)hdr[1], (char)hdr[2], (char)hdr[3], 0};

    // ⚠️ ID3v2.3 用**普通**大端帧长, v2.4 才用 syncsafe。
    //    web_radio 那边只处理了 v2.4 (电台流的实测素材都是 v2.4)。
    //    本地文件里 v2.3 很常见 (老 iTunes / 老 foobar 写的), 所以这里按
    //    版本号分开解 —— 解错了会走到 padding 直接 break, 拿不到标签。
    uint32_t fsize;
    if (buf[3] >= 4) {
      fsize = syncsafe32(hdr + 4);
    } else {
      fsize = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) |
              ((uint32_t)hdr[6] << 8) | (uint32_t)hdr[7];
    }
    // ⚠️ 减法而不是加法: fsize 对 v2.3 是**普通 32 位大端**字段，直接来自文件，
    //    写成 `pos + 10 + fsize > end` 会在 fsize 接近 2^31 时回绕成一个小数、
    //    把检查整个绕过，随后 copy_trim 拿 text_len≈4GB 去读探测缓冲区外面。
    //    循环条件已保证 pos + 10 <= end，所以右边这个减法不会下溢。
    if (fsize == 0 || fsize > end - pos - 10) {
      break;
    }

    const unsigned char *body = buf + pos + 10;
    // 正文第一个字节是编码: 0x00=Latin1, 0x01=UTF-16+BOM, 0x02=UTF-16BE,
    // 0x03=UTF-8。只处理 UTF-8。
    if (fsize >= 2 && body[0] == 0x03 &&
        (strcmp(id, "TIT2") == 0 || strcmp(id, "TPE1") == 0 ||
         strcmp(id, "TALB") == 0)) {
      const char *text = (const char *)body + 1;
      size_t text_len = fsize - 1;
      if (strcmp(id, "TIT2") == 0) {
        copy_trim(title, title_len, text, text_len);
      } else if (strcmp(id, "TPE1") == 0) {
        copy_trim(artist, artist_len, text, text_len);
      } else {
        copy_trim(album, album_len, text, text_len);
      }
    }

    pos += 10 + fsize;
  }
}

// 解析 FLAC 的 VORBIS_COMMENT 块 (块类型 4)。
// ⚠️ 标签**不在文件开头**: 前面有 STREAMINFO(34B) 和可能的 SEEKTABLE/PADDING/
//    PICTURE。必须按元数据块链一路走。(web_radio.c:577)
static void tag_parse_vorbis(const unsigned char *buf, size_t len, char *title,
                             size_t title_len, char *artist, size_t artist_len,
                             char *album, size_t album_len) {
  if (len < 4 || memcmp(buf, "fLaC", 4) != 0) {
    return;
  }

  size_t pos = 4;
  while (pos + 4 <= len) {
    unsigned char hdr = buf[pos];
    bool last = (hdr & 0x80) != 0;
    unsigned char btype = hdr & 0x7F;
    uint32_t blen = ((uint32_t)buf[pos + 1] << 16) |
                    ((uint32_t)buf[pos + 2] << 8) | (uint32_t)buf[pos + 3];
    pos += 4;

    if (btype == 4) { // VORBIS_COMMENT
      if (pos + blen > len) {
        return; // 还没读全 (预读窗口不够)
      }
      const unsigned char *p = buf + pos;
      size_t remain = blen;

      // 先跳过 vendor string (小端 4 字节长度 + 内容)
      if (remain < 4) {
        return;
      }
      uint32_t vl = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
      p += 4;
      remain -= 4;
      if (vl > remain) {
        return;
      }
      p += vl;
      remain -= vl;

      // 再逐条读 comment: 小端 4 字节长度 + "KEY=VALUE"
      if (remain < 4) {
        return;
      }
      uint32_t ncomments = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
      p += 4;
      remain -= 4;

      for (uint32_t i = 0; i < ncomments && remain >= 4; i++) {
        uint32_t clen = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4;
        remain -= 4;
        if (clen > remain) {
          break;
        }

        const char *kv = (const char *)p;
        const char *eq = memchr(kv, '=', clen);
        if (eq) {
          size_t klen = (size_t)(eq - kv);
          const char *val = eq + 1;
          size_t vlen = clen - klen - 1;

          if (klen == 5 && strncasecmp(kv, "TITLE", 5) == 0) {
            copy_trim(title, title_len, val, vlen);
          } else if (klen == 6 && strncasecmp(kv, "ARTIST", 6) == 0) {
            copy_trim(artist, artist_len, val, vlen);
          } else if (klen == 5 && strncasecmp(kv, "ALBUM", 5) == 0) {
            copy_trim(album, album_len, val, vlen);
          }
        }

        p += clen;
        remain -= clen;
      }
      return; // 找到 VORBIS_COMMENT 就够
    }

    if (last) {
      return; // 元数据块链到头了
    }
    if (pos + blen > len) {
      return; // 超出预读窗口
    }
    pos += blen;
  }
}

void sd_tags_read(const char *vfs_path, char *title, size_t title_len,
                  char *artist, size_t artist_len, char *album,
                  size_t album_len) {
  if (title && title_len) {
    title[0] = '\0';
  }
  if (artist && artist_len) {
    artist[0] = '\0';
  }
  if (album && album_len) {
    album[0] = '\0';
  }
  if (!vfs_path) {
    return;
  }

  FILE *f = fopen(vfs_path, "rb");
  if (!f) {
    return; // 打不开就当没有标签, 上层会回退到文件名
  }

  // 放 PSRAM: 16KB 而已, 但内部 RAM 在这个项目里是稀缺资源
  // (见 web_radio.c 文件头第 1 条)
  unsigned char *buf = heap_caps_malloc(TAG_PROBE_BYTES, MALLOC_CAP_SPIRAM);
  if (!buf) {
    fclose(f);
    return;
  }

  size_t got = fread(buf, 1, TAG_PROBE_BYTES, f);
  fclose(f);

  if (got >= 4 && memcmp(buf, "fLaC", 4) == 0) {
    tag_parse_vorbis(buf, got, title, title_len, artist, artist_len, album,
                     album_len);
  } else {
    tag_parse_id3v2(buf, got, title, title_len, artist, artist_len, album,
                    album_len);
  }

  heap_caps_free(buf);
}

void sd_title_from_name(const char *name, char *out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!name || !name[0]) {
    return;
  }

  // 取最后一段路径 (调用者通常只传文件名, 但传全路径也不该出错)
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;

  // 去掉扩展名 —— 只认常见的音频后缀, 免得把 "01. 前奏" 里的点当扩展名切掉
  size_t len = strlen(base);
  static const char *exts[] = {".mp3", ".flac", ".wav", ".m4a",
                               ".aac", ".ogg",  ".oga"};
  for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
    size_t el = strlen(exts[i]);
    if (len > el && strcasecmp(base + len - el, exts[i]) == 0) {
      len -= el;
      break;
    }
  }

  if (len >= out_len) {
    len = out_len - 1;
  }
  memcpy(out, base, len);
  out[len] = '\0';

  if (out[0] == '\0') {
    strlcpy(out, "未知曲目", out_len);
  }
}
