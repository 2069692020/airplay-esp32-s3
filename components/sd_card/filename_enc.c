/*
 * 文件名编码转换: GBK <-> UTF-8
 *
 * 为什么需要这个 (背景, 免得以后忘):
 *   ESP-IDF 的 FATFS **没有内建 UTF-8 支持** —— ffunicode.c 里没有 65001 码页,
 *   Kconfig 里也没有 FATFS_API_ENCODING_UTF_8 之类的开关。
 *   SD 卡上的长文件名 (LFN) 按 FAT 规范以 UTF-16 存放, fatfs 用 FF_CODE_PAGE
 *   把它转成 OEM 单/双字节编码。项目设成 936 (CONFIG_FATFS_CODEPAGE_936),
 *   于是 readdir() 交给我们的就是 **GBK 字节**, 必须自己转成 UTF-8 才好在网页上
 *   显示; 反方向 (网页上传时传进来的 UTF-8 文件名) 也要转回 GBK 才能建文件,
 *   否则卡上会写出一堆乱码名字。
 *
 * 码表见 gbk_table.c (由 scripts/gen_gbk_table.py 生成, 完整 CP936 约 48KB flash)。
 * 用完整 CP936 而不是只做 GB2312, 是为了繁体字和 GBK 扩展字也能正确显示。
 *
 * 转换失败一律输出 '?' 并继续 —— 宁可个别字显示成问号, 也不能因为一个生僻字
 * 把整个文件名丢掉 (那样文件就打不开了)。
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gbk_table.h"
#include "sd_card.h"

// 把 buffer 的写入位置 + 剩余空间打包传递, 免得每个分支都重复边界判断。
typedef struct {
  char *buf;
  size_t cap; // 含结尾 '\0' 的容量
  size_t len; // 已写入长度 (不含 '\0')
} out_t;

static void out_char(out_t *o, char c) {
  if (o->len + 1 < o->cap) {
    o->buf[o->len++] = c;
  }
}

// 追加一个 Unicode 码点的 UTF-8 编码 (只处理 BMP, 1~3 字节)。
static void out_utf8(out_t *o, uint32_t cp) {
  if (cp < 0x80) {
    out_char(o, (char)cp);
  } else if (cp < 0x800) {
    out_char(o, (char)(0xC0 | (cp >> 6)));
    out_char(o, (char)(0x80 | (cp & 0x3F)));
  } else {
    out_char(o, (char)(0xE0 | (cp >> 12)));
    out_char(o, (char)(0x80 | ((cp >> 6) & 0x3F)));
    out_char(o, (char)(0x80 | (cp & 0x3F)));
  }
}

// 查 GBK 双字节码位。返回 0 表示该码位在 CP936 里未定义。
static uint16_t gbk_lookup(uint8_t lead, uint8_t trail) {
  if (lead < GBK_LEAD_MIN || lead > 0xFE) {
    return 0;
  }
  // 尾字节 0x40..0xFE, 但 0x7F 不是合法尾字节 —— 表里把它挖掉了,
  // 所以 0x7F 以上要整体左移一位去索引 (与 gen_gbk_table.py 的折算一致)。
  if (trail < 0x40 || trail == 0x7F) {
    return 0;
  }
  size_t idx = (trail < 0x7F) ? (size_t)(trail - 0x40)
                              : (size_t)(trail - 0x41);
  if (idx >= GBK_TRAIL_COUNT) {
    return 0;
  }
  return gbk_to_uni[lead - GBK_LEAD_MIN][idx];
}

void sd_card_gbk_to_utf8(const char *in, char *out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out_t o = {.buf = out, .cap = out_len, .len = 0};

  if (in) {
    const uint8_t *p = (const uint8_t *)in;
    while (*p) {
      uint8_t c = *p;

      if (c < 0x80) {
        out_char(&o, (char)c); // ASCII 原样
        p++;
        continue;
      }

      // 0x81..0xFE 是双字节首字节; 0x80 / 0xFF 单独出现是非法字节
      if (c >= GBK_LEAD_MIN && p[1] != '\0') {
        uint16_t uni = gbk_lookup(c, p[1]);
        if (uni) {
          out_utf8(&o, uni);
        } else {
          out_char(&o, '?');
        }
        p += 2;
        continue;
      }

      out_char(&o, '?'); // 落单的首字节 / 非法字节
      p++;
    }
  }

  o.buf[o.len] = '\0';
}

// 反查: 从码点找 GBK 双字节码。找到返回 true, 结果写进 *lead / *trail。
//
// ⚠️ 这里是**线性扫描** 126 x 190 = 23940 个条目。
//    只有上传文件名走这条路 (网页每传一个文件才调一次), 一次扫描几毫秒,
//    不值得为它再维护一张反向索引表 (那要额外 48KB flash)。
//    **播放路径不经过这里** —— 播放用的是 sd_card_gbk_to_utf8 (正向查表, O(1))。
static bool gbk_reverse_lookup(uint32_t cp, uint8_t *lead, uint8_t *trail) {
  for (int l = 0; l < GBK_LEAD_COUNT; l++) {
    for (int t = 0; t < GBK_TRAIL_COUNT; t++) {
      if (gbk_to_uni[l][t] == cp) {
        *lead = (uint8_t)(GBK_LEAD_MIN + l);
        // 索引折算的逆运算: 0x7F 被挖掉了, 所以 0x7F 及以上要补回一位
        *trail = (uint8_t)(t < 0x3F ? t + 0x40 : t + 0x41);
        return true;
      }
    }
  }
  return false;
}

void sd_card_utf8_to_gbk(const char *in, char *out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out_t o = {.buf = out, .cap = out_len, .len = 0};

  if (in) {
    const uint8_t *p = (const uint8_t *)in;
    while (*p) {
      uint32_t cp;
      int extra;

      if (p[0] < 0x80) {
        cp = p[0];
        extra = 0;
      } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F;
        extra = 1;
      } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F;
        extra = 2;
      } else {
        // 4 字节 UTF-8 (BMP 之外) 或非法首字节 —— GBK 表示不了
        out_char(&o, '?');
        p++;
        continue;
      }

      // 续字节缺失/格式不对就当作非法序列, 吃掉一个字节继续
      bool bad = false;
      for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
          bad = true;
          break;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
      }
      if (bad) {
        out_char(&o, '?');
        p++;
        continue;
      }
      p += 1 + extra;

      if (cp < 0x80) {
        out_char(&o, (char)cp);
        continue;
      }

      uint8_t lead = 0, trail = 0;
      if (gbk_reverse_lookup(cp, &lead, &trail)) {
        out_char(&o, (char)lead);
        out_char(&o, (char)trail);
      } else {
        out_char(&o, '?');
      }
    }
  }

  o.buf[o.len] = '\0';
}
