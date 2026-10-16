#!/usr/bin/env python3
# 从 IEEE MA-L oui.csv 生成紧凑 OUI 字典 bin (appdata os_db 槽, 运行时 mmap 二分查).
# 用法: python3 tools/gen_oui_db.py  <ieee_oui.csv>  <out.bin>
# 布局:
#   [4B "OUID"][u32 count LE][u32 strblob_size LE]
#   [records: count * 7B = 3B OUI + 4B 厂商串相对 strblob 偏移(LE)]
#   [strblob: NUL 结尾厂商名串]
import csv, sys, struct

def main():
    src, dst = sys.argv[1], sys.argv[2]
    rows = []
    with open(src, encoding='utf-8', errors='replace') as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            if len(row) >= 3 and len(row[1]) == 6:
                try:
                    oui = bytes.fromhex(row[1])
                except ValueError:
                    continue
                name = (row[2] or '').strip()
                # 限 ASCII 可打印, 其余替换下降级
                name = ''.join(c if 32 <= ord(c) < 127 else ' ' for c in name).strip()
                if not name:
                    continue
                rows.append((oui, name))
    # 去重同名
    uni = sorted(set(n for _, n in rows))
    strmap = {n: i for i, n in enumerate(uni)}
    strblob = bytearray()
    for n in uni:
        strmap[n] = len(strblob)
        strblob += n.encode('ascii') + b'\x00'
    # 按前缀排序
    recs = sorted(set(rows), key=lambda r: r[0])
    recs = [(oid, strmap[n]) for oid, n in recs]
    count = len(recs)
    data = bytearray()
    data += b'OUID'
    data += struct.pack('<II', count, len(strblob))
    for oid, off in sorted(recs, key=lambda r: r[0]):
        data += oid + struct.pack('<I', off)
    data += strblob
    with open(dst, 'wb') as f:
        f.write(data)
    print(f'OUI 记录: {count}  文件: {dst}  {len(data)} 字节 (%.2f KB)' % (len(data)/1024))

if __name__ == '__main__':
    main()