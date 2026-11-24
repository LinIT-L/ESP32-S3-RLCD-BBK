#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成中文输入法拼音表 (无词库, 单字全拼).

数据源 = 固件实际字库 font_zh.bin 的字符表 —— 只收字库里真有的字,
保证候选点上去一定能显示, 不会出现"输入法有字、屏幕没字"。

输出 components/ui/kbd_lib/ime_pinyin_data.c (约 17KB 数据, 全部 const 走 flash XIP).

结构 (反向索引: 拼音 → 字, 输入法查询正好是这个方向, 不需要 字→拼音):
  ime_syl_blob[]   400 个无调音节, 连续存放, 每个以 \\0 结尾
  ime_syl_off[]    每个音节在 blob 的起始偏移 (长度 = off[i+1]-off[i])
  ime_grp_start[]  第 i 个音节的候选字在 ime_char_cp 的起点
  ime_char_cp[]    候选字 = Unicode 码点 - 0x4E00 (2 字节, GB2312 汉字全在 0..0x51A5)

组内排序 = 常用度: 内置高频字表优先, 其余按 GB2312 一级(常用)在前.

用法: python3 tools/gen_pinyin.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pypinyin import pinyin, Style

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "components/ui/fonts/font_zh.bin")
OUT_C = os.path.join(ROOT, "components/ui/kbd_lib/ime_pinyin_data.c")
OUT_H = os.path.join(ROOT, "components/ui/kbd_lib/ime_pinyin.h")

# 高频字表 (现代汉语常用字, 大致按使用频率降序).
# 作用只有一个: 决定同一个拼音下候选字的先后 —— "zhong" 必须把"中"排第一.
FREQ = (
    "的一是了我不人在他有这个上们来到时大地为子中你说生国年着就那和要"
    "她出也得里后自以会家可下而过天去能对小多然于心学么之都好看起发当"
    "没成只如事把还用第样道想作种开美总从无情己面最女但现前些所同日手"
    "又行意动方期它头经长儿回位分爱老因很给名法间斯知世什两次使身者被"
    "高已亲其进此话常与活正感见明问力理尔点文几定本公特做外孩相西果走"
    "将月十实向声车全信重三机工物气每并别真打太新比才便夫再书部水像眼"
    "等体却加电主界门利海受听表德少克代员许先口由死安写性马光白或住难"
    "望教命花结乐色更拉东神记处让母父应直字场平报友关放至张认接告入笑"
    "内英军候民岁往何度山觉路带万男边风解叫任金快原吃妈变通师立象数四"
    "失满战远格士音轻目条呢病始达深完今提求清王化空业思切怎非找片罗钱"
    "吗语元喜曾离飞科言干流欢约各即指合反题必该论交终林请医晚制球决传"
    "画保读运及则房早院量苦火布品近坐产答星精视五连司巴奇管类未朋且婚"
    "台夜青北队久乎越观落尽形影红爸百令周吧识步希术留市半热送兴造谈容"
    "极随演收首根讲整式取照办强石古拿计您装似足双转诉米称丽客南领节衣"
    "站黑刻统断福城故历惊脸选包紧争另建维绝树系伤示愿持千史谁准联妇纪"
    "基买志静阿诗独复痛消社算义竟确酒需单治卡幸兰念举仅钟怕共毛句息功"
    "官待究跟穿室易游程号居考突皮哪费倒价图具刚脑永歌响商礼细专黄块脚"
    "味灵改据般破引食仍存众注笔甚某沉血备习校默务土微娘须试怀料调广苏"
    "显赛查密议底列富梦错座参八除跑亮假印设线温虽掉京初养香停际致阳纸"
    "李纳验助激够严证帝饭忘趣支春集丈木研班普导顿睡展跳获艺六波察群皇"
    "段急庭创区奥器谢弟店否害草排背止组州朝封睛板角况曲馆育忙质河续哥"
    "呼若推境遇雨标姐充围案伦护冷警贝著雪索剧啊船险烟依斗值帮汉慢佛肯"
    "闻唱沙局伯族低玩资屋击速顾泪洲团圣旁堂兵七露园牛哭旅街劳型烈姑陈"
    "莫鱼异抱宝权鲁简态级票怪寻杀律胜份汽右洋范床舞秘午登楼贵吸责例追"
    "较职属渐左录丝牙党继托赶章智冲叶胡吉卖坚喝肉遗救修松临藏担戏善卫"
    "药悲敢靠伊村戴词森耳差短祖云规窗散迷油旧适乡架恩投弹铁博雷府压超"
    "负勒杂醒洗采毫嘴毕九冰既状乱景席珍童顶派素脱农疑练野按犯拍征坏骨"
    "余承置彩灯巨琴免环姆暗换技翻束增忍餐洛塞缺忆判欧层付阵批岛项狗休"
    "懂武革良恶恋委拥妙探呀营退摇弄桌熟诺宣银势奖宫忽套康供优课鸟喊降"
    "夏困刘罪亡鞋健模败伴守挥鲜财孤枪禁恐伙杰迹妹遍盖副坦牌江顺秋菜划"
    "授归浪凡预奶雄升编典袋含盛济蒙棋端腿招释介烧误"
)


def load_font_chars():
    d = open(BIN, "rb").read()
    if d[:8] != b"ZH1FNT01":
        raise SystemExit("字库 magic 不对: %r" % d[:8])
    n = struct.unpack("<I", d[8:12])[0]
    if len(d) != 12 + n * 75:
        raise SystemExit("字库长度不符 count=%d len=%d" % (n, len(d)))
    return [d[12 + i * 3: 12 + i * 3 + 3].decode("utf-8") for i in range(n)]


def gb_rank(ch):
    """GB2312 一级汉字(区 16-55 = 3755 个常用字)返回区*94+位, 其余返回大数."""
    try:
        b = ch.encode("gb2312")
    except UnicodeEncodeError:
        return 900000
    if len(b) != 2:
        return 900000
    qu = b[0] - 0xA0
    if 16 <= qu <= 55:
        return (qu - 16) * 94 + (b[1] - 0xA0)
    return 600000 + qu * 94 + (b[1] - 0xA0)


def main():
    chars = load_font_chars()
    cjk = sorted({c for c in chars if "\u4e00" <= c <= "\u9fa5"})
    freq_pos = {}
    for i, c in enumerate(FREQ):
        freq_pos.setdefault(c, i)

    groups = {}
    for c in cjk:
        py = pinyin(c, style=Style.NORMAL, heteronym=False, errors="ignore")
        if not py or not py[0]:
            continue
        s = py[0][0]
        if not (s.isascii() and s.isalpha()) or len(s) > 6:
            continue
        groups.setdefault(s, []).append(c)

    def sort_key(c):
        p = freq_pos.get(c)
        return (0, p, 0) if p is not None else (1, gb_rank(c), 0)

    syls = sorted(groups.keys())
    blob = bytearray()
    offs = []
    grp = []
    cps = []
    for s in syls:
        offs.append(len(blob))
        blob += s.encode("ascii") + b"\x00"
        grp.append(len(cps))
        for c in sorted(groups[s], key=sort_key):
            cps.append(ord(c) - 0x4E00)
    offs.append(len(blob))
    grp.append(len(cps))

    ns = len(syls)
    nc = len(cps)
    assert ns <= 4096 and nc <= 65535

    def wrap(items, per, fmt):
        out = []
        for i in range(0, len(items), per):
            out.append("    " + ",".join(fmt % v for v in items[i:i + per]) + ",")
        return "\n".join(out)

    # 音节 blob: 按字符串字面量拼, \0 在中间是合法的
    blob_lines = []
    line = []
    for s in syls:
        line.append(s + "\\0")
        if len(line) >= 16:
            blob_lines.append('    "' + "".join(line) + '"')
            line = []
    if line:
        blob_lines.append('    "' + "".join(line) + '"')

    src = []
    src.append("/* 自动生成, 请勿手改 —— python3 tools/gen_pinyin.py")
    src.append(" *")
    src.append(" * 中文输入法拼音表: %d 个无调音节 / %d 个汉字 (取自 font_zh.bin 实际字符表)." % (ns, nc))
    src.append(" * 反向索引(拼音→字), 组内按常用度排序. 全部 const, 走 flash XIP, 不占 RAM.")
    src.append(" */")
    src.append('#include "ime_pinyin.h"')
    src.append("")
    src.append("const uint16_t ime_syl_n = %d;" % ns)
    src.append("const uint16_t ime_char_n = %d;" % nc)
    src.append("")
    src.append("/* 音节字符串表 (400 个, 每个以 0 结尾) */")
    src.append("const char ime_syl_blob[] =")
    src.append("\n".join(blob_lines))
    src.append(";")
    src.append("")
    src.append("/* 音节 → blob 偏移, 共 %d 项, 末项 = blob 总长 */" % (ns + 1))
    src.append("const uint16_t ime_syl_off[IME_SYL_N + 1] = {")
    src.append(wrap(offs, 12, "%d"))
    src.append("};")
    src.append("")
    src.append("/* 音节 → 候选字区间起点, 共 %d 项, 末项 = %d */" % (ns + 1, nc))
    src.append("const uint16_t ime_grp_start[IME_SYL_N + 1] = {")
    src.append(wrap(grp, 12, "%d"))
    src.append("};")
    src.append("")
    src.append("/* 候选字 = Unicode 码点 - 0x4E00 */")
    src.append("const uint16_t ime_char_cp[IME_CHAR_N] = {")
    src.append(wrap(cps, 16, "0x%04X"))
    src.append("};")
    src.append("")
    open(OUT_C, "w").write("\n".join(src))

    hdr = """/* 自动生成, 请勿手改 —— python3 tools/gen_pinyin.py */
#ifndef _IME_PINYIN_H
#define _IME_PINYIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IME_SYL_N   %d
#define IME_CHAR_N  %d

extern const uint16_t ime_syl_n;
extern const uint16_t ime_char_n;

extern const char     ime_syl_blob[];
extern const uint16_t ime_syl_off[IME_SYL_N + 1];
extern const uint16_t ime_grp_start[IME_SYL_N + 1];
extern const uint16_t ime_char_cp[IME_CHAR_N];

#ifdef __cplusplus
}
#endif
#endif /* _IME_PINYIN_H */
""" % (ns, nc)
    open(OUT_H, "w").write(hdr)

    data_bytes = len(blob) + (ns + 1) * 2 + (ns + 1) * 2 + nc * 2
    print("音节 %d / 汉字 %d" % (ns, nc))
    print("blob %d B, off %d B, grp %d B, cps %d B" %
          (len(blob), (ns + 1) * 2, (ns + 1) * 2, nc * 2))
    print("数据合计 %.1f KB" % (data_bytes / 1024.0))
    for probe in ("zhong", "shi", "wo", "ni", "de"):
        if probe in groups:
            top = sorted(groups[probe], key=sort_key)[:8]
            print("  %-6s -> %s" % (probe, "".join(top)))


if __name__ == "__main__":
    main()
