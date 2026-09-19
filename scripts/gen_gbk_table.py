#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 CP936 (GBK) -> Unicode 码表, 供 SD 卡文件名转码用。

用法:
    python scripts/gen_gbk_table.py            # 写入 components/sd_card/gbk_table.c
    python scripts/gen_gbk_table.py --check    # 只校验现有文件是否最新, 不写

为什么需要这张表 (背景, 免得以后忘):
    ESP-IDF 的 FATFS **没有内建 UTF-8 支持** —— ffunicode.c 里没有 65001 码页,
    Kconfig 里也没有 FATFS_API_ENCODING_UTF_8 这样的开关。
    SD 卡上的长文件名 (LFN) 按 FAT 规范以 UTF-16 存放, fatfs 用 FF_CODE_PAGE
    把它转成 OEM 单/双字节编码。项目里把 FF_CODE_PAGE 设成 936 (见
    config/sdkconfig.defaults 的 CONFIG_FATFS_CODEPAGE_936), 于是 readdir()
    交给我们的是 **GBK 字节**, 要显示在网页上必须先转成 UTF-8。

为什么用**完整 CP936** 而不是只做 GB2312:
    GB2312 只有 6763 个简化字。繁体字 (樂 / 響 / 臺灣) 和 GBK 扩展汉字
    (如 '囧' 之外的生僻字) 全都不在里面 —— 卡里一旦有繁体歌名就会变成问号。
    实测 '樂' 的 GBK 码是 0x98B7, 确实落在 GB2312 范围 (0xA1-0xF7 首字节) 之外。

表的形状 (直接索引, 无查找):
    首字节 0x81..0xFE  → 126 个
    尾字节 0x40..0xFE 去掉 0x7F → 0x40..0x7E (63) + 0x80..0xFE (127) = 190 个
    gbk_to_uni[126][190], 0 表示该码位未定义
    126 × 190 × 2 字节 ≈ 47.9KB flash (app 分区 4MB, 完全放得下)

⚠️ 与 fatfs 内置表的差异: fatfs 的 ffunicode.c 里也有一份 CP936 表, 但那是
   static 的, 拿不到。本表由 Python 的 'gbk' 编解码器生成, 与 fatfs 的表在极少数
   边缘码位上可能不一致 —— 影响仅限于个别生僻字显示成 '?', 不影响功能。
"""

import argparse
import os
import sys

# CP936 的合法码位范围
LEAD_MIN, LEAD_MAX = 0x81, 0xFE   # 126 个首字节
TRAIL_MIN, TRAIL_MAX = 0x40, 0xFE  # 尾字节, 跳过 0x7F
TRAIL_SKIP = 0x7F

LEAD_COUNT = LEAD_MAX - LEAD_MIN + 1                 # 126
TRAIL_COUNT = (TRAIL_MAX - TRAIL_MIN + 1) - 1        # 190

OUT_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "components", "sd_card", "gbk_table.c")

HEADER = """/*
 * 由 scripts/gen_gbk_table.py 生成 —— **不要手工编辑**。
 * 重新生成: python scripts/gen_gbk_table.py
 *
 * CP936 (GBK) -> Unicode 码表, 直接索引:
 *   gbk_to_uni[lead - 0x81][trail 折算后的下标]
 *   trail 折算: trail < 0x7F ? trail - 0x40 : trail - 0x41   (跳过 0x7F)
 *   值为 0 表示该码位在 CP936 里未定义。
 *
 * 表大小: %d x %d x 2 字节 = %d 字节
 *
 * ⚠️ 数组本体用 clang-format off 包住 —— 每行 %d 个数字必然超过
 *    .clang-format 的 ColumnLimit(80), 不关掉的话 CI 的 format-check 会失败,
 *    而按 80 列折行会把文件撑到 30 万行。生成器输出必须与关闭区间共存。
 */

#include <stdint.h>

#include "gbk_table.h"

// clang-format off
const uint16_t gbk_to_uni[%d][%d] = {
""" % (LEAD_COUNT, TRAIL_COUNT, LEAD_COUNT * TRAIL_COUNT * 2, TRAIL_COUNT,
       LEAD_COUNT, TRAIL_COUNT)

FOOTER = """};
// clang-format on
"""


def trail_index(trail):
    """尾字节 -> 0..TRAIL_COUNT-1 的下标 (跳过 0x7F)。"""
    return trail - TRAIL_MIN if trail < TRAIL_SKIP else trail - TRAIL_MIN - 1


def build_table():
    """返回 [126][190] 的 Unicode 码点表, 未定义处为 0。

    已知但无法映射的码位也记 0 —— 转换时统一按"未定义"处理, 输出 '?'。
    """
    table = []
    for lead in range(LEAD_MIN, LEAD_MAX + 1):
        row = [0] * TRAIL_COUNT
        for trail in range(TRAIL_MIN, TRAIL_MAX + 1):
            if trail == TRAIL_SKIP:
                continue
            try:
                ch = bytes([lead, trail]).decode("gbk")
            except UnicodeDecodeError:
                continue
            # 只收 BMP: 表格是 uint16。BMP 之外的字 (极少) 记 0。
            cp = ord(ch)
            if cp > 0xFFFF:
                continue
            row[trail_index(trail)] = cp
        table.append(row)
    return table


def render(table):
    out = [HEADER]
    for row in table:
        # 每行一条, 16 进制。这一整段在 clang-format off 区间里, 不会被折行。
        cells = ", ".join("0x%04X" % v for v in row)
        out.append("    {%s},\n" % cells)
    out.append(FOOTER)
    return "".join(out)


def count_defined(table):
    return sum(1 for row in table for v in row if v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="只校验现有文件是否为最新, 不写盘")
    args = ap.parse_args()

    table = build_table()
    text = render(table)
    defined = count_defined(table)

    if args.check:
        if not os.path.exists(OUT_PATH):
            print("缺少 %s" % OUT_PATH, file=sys.stderr)
            return 1
        with open(OUT_PATH, "r", encoding="utf-8") as f:
            if f.read() != text:
                print("%s 已过期, 请重新运行本脚本" % OUT_PATH, file=sys.stderr)
                return 1
        print("gbk_table.c 是最新的 (%d 个已定义码位)" % defined)
        return 0

    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    print("已写入 %s" % OUT_PATH)
    print("  表: %d x %d, %d 个已定义码位, %d 字节 flash"
          % (LEAD_COUNT, TRAIL_COUNT, defined, LEAD_COUNT * TRAIL_COUNT * 2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
