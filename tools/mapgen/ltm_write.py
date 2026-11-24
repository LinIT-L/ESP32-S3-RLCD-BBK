#!/usr/bin/env python3
"""LTM v3 写入器（最小可用）+ 往返自测。

只写"道路层"（layer=1）：一堆折线 (cls, [(lat_e6, lon_e6), ...])。
LOD 表按 Fj 包的做法：每个 LOD 给 (cmax, w, zmin, f0, f1)。
⚠ 本写入器故意保持"格式最小正确"，不做抽稀/分块——那是上层的事。
"""
import struct

def fletcher16(b):
    a = bb = 0
    for x in b:
        a = (a + x) % 255
        bb = (bb + a) % 255
    return bb * 256 + a

def write_ltm(path, polylines, lods, layer=1, ver=3):
    """polylines: [(cls, [(lat_e6, lon_e6), ...]), ...]
       lods: [dict(cmax,w,zmin,f0,f1)]"""
    all_pts = [p for _, p in polylines]
    lats = [la for pts in all_pts for la, _ in pts]
    lons = [lo for pts in all_pts for _, lo in pts]
    minlat, maxlat = min(lats), max(lats)
    minlon, maxlon = min(lons), max(lons)

    # ---- 点段 ----
    pt_buf = bytearray(); feat_buf = bytearray(); poff = 0
    for idx, (cls, pts) in enumerate(polylines):
        if len(pts) < 2:
            continue
        off = poff
        first = pts[0]
        pt_buf += struct.pack('<ii', first[0] - minlat, first[1] - minlon)
        prev = first
        k = 1
        while k < len(pts):
            rem = len(pts) - k
            n = 8 if rem >= 8 else 4 if rem >= 4 else 2 if rem >= 2 else 1
            for j in range(n):
                la, lo = pts[k + j]
                pt_buf += struct.pack('<hh', la - prev[0], lo - prev[1])
                prev = (la, lo)
            k += n
        poff = len(pt_buf)
        bx = [p[0] for p in pts]; by = [p[1] for p in pts]
        feat_buf += struct.pack('<BBHI', cls, len(pts), 0xFFFF, off)
        feat_buf += struct.pack('<4i', min(bx) - minlat, min(by) - minlon,
                                        max(bx) - minlat, max(by) - minlon)

    nfeat = len(feat_buf) // 24
    off_lod = 64
    off_feat = off_lod + len(lods) * 20
    off_pt = off_feat + len(feat_buf)
    off_str = off_pt + len(pt_buf)
    off_idx = 0

    hdr = struct.pack('<4sBBH', b'LTM1', ver, layer, len(lods))
    hdr += struct.pack('<4i', minlat, minlon, maxlat, maxlon)
    hdr += struct.pack('<8I', off_lod, off_feat, off_pt, off_str,
                       len(polylines), nfeat, 0, 0)
    hdr += struct.pack('<I', off_idx) + b'\x00' * 4
    assert len(hdr) == 64, len(hdr)

    # ⚠ LOD 记录是 **20 字节**: <BBHIIII (cmax,w,zmin,f0,f1,保留)
    lod_buf = b''.join(struct.pack('<BBHIIII', l['cmax'], l['w'], l['zmin'], l['f0'], l['f1'], 0, 0)
                       for l in lods)
    body = hdr + lod_buf + bytes(feat_buf) + bytes(pt_buf)
    open(path, 'wb').write(body)
    return dict(size=len(body), nfeat=nfeat, npt=len(polylines),
                bbox=(minlat, minlon, maxlat, maxlon), crc=fletcher16(body))

if __name__ == '__main__':
    import sys, random
    sys.path.insert(0, '/tmp')
    from ltm_read import read_ltm, features, points
    random.seed(7)
    base = (30000000, 120000000)     # 示例区域原点 (1e-6 度)
    lines = []
    for i in range(200):
        la, lo = base[0] + random.randint(-20000, 20000), base[1] + random.randint(-20000, 20000)
        pts = []
        for k in range(random.randint(2, 40)):
            la += random.randint(-300, 300); lo += random.randint(-300, 300)
            pts.append((la, lo))
        lines.append((random.randint(0, 1), pts))
    lods = [dict(cmax=1, w=2, zmin=0, f0=0, f1=len(lines)),
            dict(cmax=2, w=2, zmin=13, f0=0, f1=len(lines)),
            dict(cmax=3, w=2, zmin=14, f0=0, f1=len(lines)),
            dict(cmax=6, w=1, zmin=15, f0=0, f1=len(lines)),
            dict(cmax=7, w=1, zmin=17, f0=0, f1=len(lines))]
    info = write_ltm('/tmp/test.ltm', lines, lods)
    print('写入:', info)
    m = read_ltm('/tmp/test.ltm')
    print('回读: nfeat=%d npt=%d size=%d bbox=%s' % (m['nfeat'], m['npt'], m['size'], m['bbox']))
    same = 0
    for (cls, pts), (cls2, npts, noff, poff, bb) in zip(lines, features(m)):
        got = points(m, poff, npts)
        exp = [(a - info['bbox'][0], b - info['bbox'][1]) for a, b in pts]
        if got == exp and cls == cls2 and npts == len(pts):
            same += 1
    print('逐条往返一致: %d/%d' % (same, len(lines)))
