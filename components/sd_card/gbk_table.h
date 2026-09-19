#pragma once

#include <stdint.h>

/**
 * CP936 (GBK) -> Unicode 码表, 由 scripts/gen_gbk_table.py 生成。
 *
 * 直接索引: gbk_to_uni[lead - 0x81][trail 折算后的下标]
 *   trail 折算: trail < 0x7F ? trail - 0x40 : trail - 0x41   (跳过 0x7F)
 * 值为 0 表示该码位在 CP936 里未定义。
 *
 * ⚠️ 本文件是**内部**头, 只给 filename_enc.c 用。外部一律走 sd_card.h 里的
 *    sd_card_gbk_to_utf8() / sd_card_utf8_to_gbk()。
 */
#define GBK_LEAD_MIN 0x81
#define GBK_LEAD_COUNT 126
#define GBK_TRAIL_COUNT 190

extern const uint16_t gbk_to_uni[GBK_LEAD_COUNT][GBK_TRAIL_COUNT];
