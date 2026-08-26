#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pack_games.py — 把 games/ 目录打包成 FAT16 只读镜像 (支持中文长文件名 LFN)
用法: python3 tools/pack_games.py <games_dir> <out_fat>
  games_dir/ 下: gam/*.gam  gb/*.gb  (烧录后挂载到 /games, 与 SD 卡游戏合并显示)
"""
import os, sys, struct

SECTOR = 512
CLUSTER = 4096          # 4KB 簇 (3MB 分区 -> ~768 簇, FAT16 合法)
RESERVED = 1            # boot sector 占用 1 簇
FAT_COUNT = 2
ROOT_ENTRIES = 256      # 根目录项数 (LFN 会多占, 256 项够 60+ 文件)

def utf8_to_utf16le(name):
    return name.encode('utf-16-le')

def lfn_checksum(short_name):
    s = 0
    for c in short_name:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s

def build_short_name(idx, ext):
    """生成 8.3 短名: GAME0001.GAM (ASCII, 与 LFN 配对)"""
    base = f"GAME{idx:04d}"
    ext = ext.upper()
    return base + "." + ext

def make_lfn_entries(name, short_name):
    """生成 LFN 条目 (0x0F), 每条目 13 字符 (UTF-16LE), 从后往前"""
    u16 = utf8_to_utf16le(name)
    # 补齐到 13 的倍数 + 终止 0x0000
    total = len(u16) // 2
    checksum = lfn_checksum(short_name.encode('ascii'))
    entries = []
    parts = [u16[i:i+26] for i in range(0, len(u16), 26)]  # 13 chars * 2B
    for i in range(len(parts) - 1, -1, -1):
        chunk = parts[i]
        last = (i == len(parts) - 1)
        e = bytearray(32)
        e[0] = 0x40 | (i + 1) if last else (i + 1)   # 末条 0x40|N, 其余 N
        # 名称 1-5 (10B)
        c = chunk[0:10]; e[1:11] = c + b'\xff' * (10 - len(c))
        e[11] = 0x0F
        e[12] = 0
        e[13] = checksum
        c = chunk[10:22]; e[14:26] = c + b'\xff' * (12 - len(c))
        e[26:28] = b'\x00\x00'                       # 首簇 = 0
        c = chunk[22:26]; e[28:32] = c + b'\xff' * (4 - len(c))
        entries.append(bytes(e))
    return entries

def make_sfn_entry(short_name, first_cluster, size, attr=0x20):
    e = bytearray(32)
    name, ext = os.path.splitext(short_name)
    ext = ext.lstrip('.')
    raw = name.ljust(8)[:8] + ext.ljust(3)[:3]
    e[0:11] = raw.encode('ascii')
    e[11] = attr
    e[12:26] = b'\x00' * 14       # reserved/时间戳 (只读 FAT, 可全 0)
    e[26:28] = struct.pack('<H', first_cluster)   # FAT16: 首簇在偏移 26
    e[28:32] = struct.pack('<I', size)            # 文件大小
    return bytes(e)

def build_fat16(files):
    """files: [(dir, short_name, lfn_name, data_bytes)] → 返回镜像 bytes"""
    # 布局计算
    fats_size = 0
    # 先假设根目录区: 根目录在 FAT 之后
    root_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
    # 数据区起始 (扇区): boot + fats + root
    # 簇数 = ceil(分区大小 / 簇)  — 分区大小未知, 用数据量反推
    data_bytes = sum(len(f[3]) for f in files)
    total_clusters = max(32, (data_bytes + CLUSTER - 1) // CLUSTER + 2)
    total_clusters = min(total_clusters, 0xFFF0)  # FAT16 上限
    fats_bytes = total_clusters * 2
    fats_sectors = (fats_bytes + SECTOR - 1) // SECTOR
    data_start_sector = RESERVED * (CLUSTER // SECTOR) + fats_sectors * FAT_COUNT + root_sectors
    # 实际镜像大小 = 数据区到最后一个簇
    total_sectors = data_start_sector + total_clusters * (CLUSTER // SECTOR)
    img = bytearray(total_sectors * SECTOR)

    # Boot sector
    bs = bytearray(SECTOR)
    bs[0:3] = b'\xEB\x3C\x90'
    bs[3:11] = b'MSDOS5.0'
    bs[11:13] = struct.pack('<H', SECTOR)
    bs[13] = CLUSTER // SECTOR
    bs[14:16] = struct.pack('<H', RESERVED)
    bs[16] = FAT_COUNT
    bs[17:19] = struct.pack('<H', ROOT_ENTRIES)
    bs[19:21] = struct.pack('<H', total_sectors if total_sectors < 0x10000 else 0)
    bs[21] = 0xF8
    bs[22:24] = struct.pack('<H', fats_sectors)
    bs[24:26] = struct.pack('<H', 63)
    bs[26:28] = struct.pack('<H', 0)
    bs[36:40] = struct.pack('<I', total_sectors)
    bs[40] = 0x80
    bs[62] = 0x29
    bs[66:70] = struct.pack('<I', 0x12345678)
    bs[70:74] = b'GAMES   '
    bs[74:82] = b'FAT16   '
    bs[510:512] = b'\x55\xAA'
    img[0:SECTOR] = bs

    # FAT 表
    fat = bytearray(fats_bytes)
    def fat_set(cluster, val):
        off = cluster * 2
        fat[off:off+2] = struct.pack('<H', val & 0xFFFF)
    fat_set(0, 0xFFF8)
    fat_set(1, 0xFFFF)
    free_cluster = 2

    # 分配文件
    entries = []   # (dir, first_cluster, size)
    fat_off = RESERVED * (CLUSTER // SECTOR) * SECTOR
    for idx, (adir, short, lfn, data) in enumerate(files):
        first = free_cluster
        clusters = (len(data) + CLUSTER - 1) // CLUSTER
        for c in range(clusters):
            nxt = free_cluster + c + 1
            fat_set(free_cluster + c, nxt if c < clusters - 1 else 0xFFFF)
        free_cluster += clusters
        # 写入数据
        data_off = data_start_sector * SECTOR + (first - 2) * CLUSTER
        img[data_off:data_off + len(data)] = data
        entries.append((adir, short, lfn, first, len(data)))

    # 写入 FAT1/FAT2
    for i in range(FAT_COUNT):
        img[fat_off + i * fats_sectors * SECTOR : fat_off + (i+1) * fats_sectors * SECTOR] = fat

    # 根目录
    root_off = (RESERVED * (CLUSTER // SECTOR) + fats_sectors * FAT_COUNT) * SECTOR
    root_pos = root_off
    for idx, (adir, short, lfn, first, size) in enumerate(entries):
        lfns = make_lfn_entries(lfn, short)
        for e in lfns:
            if root_pos + 32 > root_off + ROOT_ENTRIES * 32:
                raise RuntimeError("根目录满")
            img[root_pos:root_pos+32] = e
            root_pos += 32
        img[root_pos:root_pos+32] = make_sfn_entry(short, first, size)
        root_pos += 32
    return bytes(img)

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    games_dir = sys.argv[1]
    out = sys.argv[2]
    files = []
    idx = 1
    for sub, ext in (("gam", ".gam"), ("gb", ".gb")):
        d = os.path.join(games_dir, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.lower().endswith(ext):
                continue
            with open(os.path.join(d, name), "rb") as f:
                data = f.read()
            short = build_short_name(idx, ext.lstrip('.'))
            files.append((sub, short, name, data))
            print(f"  [{sub}] {name} ({len(data)}B) -> {short}")
            idx += 1
    if not files:
        print("没有找到游戏文件 (games/gam/*.gam 或 games/gb/*.gb)")
        return 1
    img = build_fat16(files)
    with open(out, "wb") as f:
        f.write(img)
    print(f"生成 {out}: {len(img)} 字节, {len(files)} 个文件, FAT16")

if __name__ == "__main__":
    sys.exit(main())
