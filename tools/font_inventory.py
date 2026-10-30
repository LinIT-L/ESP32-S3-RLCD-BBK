#!/usr/bin/env python3
"""统计每个 LBF 对 GB2312 全集的覆盖情况 + 输出清单 CSV。"""
import csv
import os
import struct

SRC = '/Users/linit/AI/LinTOS1.0/dist/fonts'
OUTCSV = os.path.expanduser('~/Desktop/字体/LinTOS_字体整理_20261030/07_清单与报告/字体清单_含服务器状态.csv')

META = {
    'fusion8':    ('缝合像素8', 8, 'OFL-1.1', '可发布'),
    'fusion10':   ('缝合像素10', 10, 'OFL-1.1', '可发布'),
    'fusion12':   ('缝合像素12', 12, 'OFL-1.1', '可发布'),
    'ark12':      ('方舟像素12', 12, 'OFL-1.1', '可发布'),
    'ark12_otf':  ('方舟像素12(OTF)', 12, 'OFL-1.1', '可发布'),
    'wqy12':      ('文泉驿点阵宋12', 12, 'GPL-2.0+字体嵌入例外', '可发布'),
    'wqy14':      ('文泉驿点阵宋14', 14, 'GPL-2.0+字体嵌入例外', '可发布'),
    'wqy16':      ('文泉驿点阵宋16', 16, 'GPL-2.0+字体嵌入例外', '可发布'),
    'unifont16':  ('GNU Unifont16', 16, 'OFL-1.1/GPL-2.0', '可发布'),
    'chill7':     ('寒蝉点阵体7', 7, 'OFL-1.1', '可发布'),
    'qiuye16':    ('秋叶圆体16', 16, 'OFL-1.1', '可发布'),
    'zhengge16':  ('正格点黑16', 16, 'OFL-1.1', '可发布'),
    'changban16': ('长坂点宋16', 16, 'OFL-1.1', '可发布'),
    'hzk24_S':    ('HZK24S 宋体', 24, '字模权利不明', '禁止上传'),
    'hzk24_K':    ('HZK24K 楷体', 24, '字模权利不明', '禁止上传'),
    'hzk24_H':    ('HZK24H 黑体', 24, '字模权利不明', '禁止上传'),
}


def gb2312_set():
    """GB2312 全部可编码字符"""
    out = []
    for hi in range(0xA1, 0xFF):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode('gb2312')
            except UnicodeDecodeError:
                continue
            out.append(ch)
    return out


def lbf_cmap(path):
    b = open(path, 'rb').read()
    cnt, = struct.unpack_from('<I', b, 20)
    off, = struct.unpack_from('<I', b, 24)
    return set(struct.unpack_from('<%dI' % cnt, b, off))


def main():
    chars = gb2312_set()
    cps = [ord(c) for c in chars]
    rows = []
    for fid in sorted(META):
        name, px, lic, pub = META[fid]
        p = os.path.join(SRC, fid + '.lbf')
        if not os.path.exists(p):
            continue
        have = lbf_cmap(p)
        miss = [c for c in cps if c not in have]
        miss_ch = ''.join(chars[cps.index(c)] for c in miss[:20])
        size = os.path.getsize(p)
        icon = os.path.exists(os.path.expanduser(
            '~/Desktop/字体/LinTOS_字体整理_20261030/08_图标/%s.png' % fid))
        srv = '已上传(pkgs/font_%s.zip)' % fid if pub == '可发布' else '未上传：授权不明'
        rows.append([name, '%dpx' % px, '有', lic, pub, len(miss), miss_ch,
                     str(size), '已生成128×128' if icon else '无', srv])
    with open(OUTCSV, 'w', newline='', encoding='utf-8-sig') as f:
        w = csv.writer(f)
        w.writerow(['字体名称', '字号', '源文件', '授权状态', '可发布', 'GB2312缺字',
                    '缺字示例(前20)', '设备文件大小(B)', '图标状态', '服务器状态'])
        w.writerows(rows)
    for r in rows:
        print('%-16s %-5s 缺%3d   %8s  %s' % (r[0], r[1], r[5], r[7], r[9]))
    print('\n写入', OUTCSV)


if __name__ == '__main__':
    main()
