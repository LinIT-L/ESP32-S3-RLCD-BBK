#!/usr/bin/env python3
"""esp_bin_gpio_scan.py — 从 **已编译的 ESP32-S3 固件二进制** 里挖 GPIO 立即数。

用途：拿到厂商/第三方固件（merged .bin 或 app .bin），反推它的引脚分配，
     用来对照自己的板子。**不需要源码，也不需要联网。**

原理
----
Xtensa 变长指令（16/24/32 位）用 objdump 线性扫描会**错位**（`.byte` 噪音极多），
所以不反汇编。改为直接按字节模式找 `movi.n aN, imm`：

    movi.n aN, V  的机器码字节对 = [ (V>>4)<<4 | 0x0C , (V&0xF)<<4 | N ]

（V 范围 -32..95，刚好覆盖所有 GPIO 号；此编码由 objdump 在**对齐起点**上实测标定。）

于是"一串引脚立即数扎堆出现在几十字节内"就是构造函数/配置点的强特征。
再配合 movi.n 的**目标寄存器不一致**（a5/a6/a7/a8/a9…）可以进一步排除数据噪音。

实测有效案例
------------
小智 `xiaozhi-esp32` 厂商固件（格子派 ESP32-S3-AI-2E）→ 在 0x4202F596 附近一次性
读出 `PA=46 / DOUT=48 / DIN=21 / WS=45 / BCLK=39 / MCLK=40`，与模块内部原理图完全一致。

用法
----
    python3 esp_bin_gpio_scan.py <固件.bin> [--want 40,39,45,48,21,46] [--window 60] [--top 8]
    python3 esp_bin_gpio_scan.py <固件.bin> --dump 0x4202F580 0x4202F5D0   # 看某段全部 movi.n

⚠ 只对 **ESP32-S3（IROM 段 0x42xxxxxx）** 做过标定；其它芯片段地址不同，需改 SEG_KINDS。
"""
import argparse
import bisect
import struct
import sys

SEG_KINDS = [
    (0x42000000, 0x43000000, 'IROM 代码'),
    (0x3C000000, 0x3D000000, 'DROM 只读数据'),
    (0x40000000, 0x41000000, 'IRAM'),
    (0x3FC00000, 0x40000000, 'DRAM'),
]


def parse_segments(blob, app_off):
    """返回 [(file_off, vaddr, length, kind)]；自动识别 app 起点（0xE9 镜像头）。"""
    app = blob[app_off:]
    if app[0] != 0xE9:
        sys.exit(f'0x{app_off:X} 处不是 app 镜像头 (0x{app[0]:02X})')
    n = app[1]
    p = 24                                   # ESP32 镜像头 = 24 字节
    out = []
    for _ in range(n):
        la, dl = struct.unpack('<II', app[p:p + 8])
        p += 8
        kind = '?'
        for lo, hi, k in SEG_KINDS:
            if lo <= la < hi:
                kind = k
                break
        out.append((app_off + p, la, dl, kind))
        p += dl
        p += (-p) % 4                        # 段间 4 字节对齐
    return out


def scan_movin(buf, base_va):
    """收集 (pos, imm, reg)：movi.n 的字节对模式。"""
    res = []
    for i in range(len(buf) - 1):
        if (buf[i] & 0x0F) == 0x0C:
            imm = ((buf[i] >> 4) << 4) | (buf[i + 1] >> 4)
            res.append((i, imm, base_va + i, buf[i + 1] & 0x0F))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bin')
    ap.add_argument('--app-offset', default=None, help='app 在文件中的偏移（默认自动找 0xE9 镜像头）')
    ap.add_argument('--want', default='40,39,45,48,21,46', help='关心的引脚值, 逗号分隔')
    ap.add_argument('--window', type=int, default=60, help='滑窗字节数')
    ap.add_argument('--top', type=int, default=8)
    ap.add_argument('--dump', nargs=2, metavar=('VA_FROM', 'VA_TO'), help='打印该虚地址区间全部 movi.n')
    a = ap.parse_args()

    blob = open(a.bin, 'rb').read()
    if a.app_offset:
        off = int(a.app_offset, 0)
    else:                                     # 常见: 合并镜像 app @0x20000
        off = 0x20000 if blob[0] == 0xE9 else 0
    segs = parse_segments(blob, off)
    print('段:')
    for fo, va, ln, k in segs:
        print(f'  0x{va:08X} len=0x{ln:X} {k}')

    for fo, va, ln, kind in segs:
        if kind != 'IROM 代码':
            continue
        irom = blob[fo:fo + ln]
        ins = scan_movin(irom, va)
        pos = [p for p, _, _, _ in ins]

        if a.dump:
            lo, hi = int(a.dump[0], 0), int(a.dump[1], 0)
            print(f'\n--- 0x{lo:08X}~0x{hi:08X} 全部 movi.n ---')
            for _, im, v, r in ins:
                if lo <= v <= hi:
                    print(f'  0x{v:08X}: movi.n a{r}, {im}')
            continue

        want = {int(x) for x in a.want.split(',') if x.strip()}
        hits = []
        for k2, (p, _, _, _) in enumerate(ins):
            j = bisect.bisect_right(pos, p + a.window)
            vals = {im for _, im, _, _ in ins[k2:j] if im in want}
            if len(vals) >= max(2, len(want) - 2):
                hits.append((p, vals, ins[k2:j]))
        print(f'\n命中窗口 {len(hits)} 个（按引脚值命中数降序）：')
        hits.sort(key=lambda t: -len(t[1]))
        for p, vals, win in hits[:a.top]:
            print(f'\n=== 0x{va + p:08X}  命中 {sorted(vals)} ===')
            for _, im, v, r in win:
                tag = ' ←' if im in want else ''
                print(f'   0x{v:08X}: movi.n a{r}, {im:3d}{tag}')


if __name__ == '__main__':
    main()
