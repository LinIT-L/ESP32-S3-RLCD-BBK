#!/usr/bin/env python3
"""LTM v3 读取器（用于对拍/校验；生成器另写）。
格式（小端）：
  header 64B: magic"LTM1" | ver(B) | layer(B) | nlod(H) |
              minlat,minlon,maxlat,maxlon (4×i4, 1e-6°) |
              off_lod,off_feat,off_pt,off_str,npt,nfeat,crc,rsv (8×I) | off_idx(I) | rsv
  LOD 表: nlod × 20B: cmax(B) w(B) zmin(H) f0(I) f1(I)
  要素表: nfeat × 24B: cls(B) npts(B) noff(H) poff(I) bbox(4×i4, 相对 min, 1e-6°)
  点段:  每条折线: i4 lat + i4 lon (相对 min) + 后续 (dLat,dLon)×i2 增量
"""
import struct, sys, collections

def read_ltm(path):
    d = open(path, 'rb').read()
    magic, ver, layer, nlod = struct.unpack_from('<4sBBH', d, 0)
    assert magic == b'LTM1', magic
    minlat, minlon, maxlat, maxlon = struct.unpack_from('<4i', d, 8)
    off_lod, off_feat, off_pt, off_str, npt, nfeat, crc, rsv = struct.unpack_from('<8I', d, 24)
    off_idx, = struct.unpack_from('<I', d, 56)
    lods = []
    for k in range(nlod):
        cmax, w, zmin, f0, f1 = struct.unpack_from('<BBHII', d, off_lod + k*20)
        lods.append(dict(cmax=cmax, w=w, zmin=zmin, f0=f0, f1=f1))
    return dict(raw=d, ver=ver, layer=layer, nlod=nlod, lods=lods,
                bbox=(minlat/1e6, minlon/1e6, maxlat/1e6, maxlon/1e6),
                off_lod=off_lod, off_feat=off_feat, off_pt=off_pt, off_str=off_str,
                npt=npt, nfeat=nfeat, crc=crc, off_idx=off_idx, size=len(d))

def features(m):
    """yield (cls, npts, noff, poff, bbox)"""
    d = m['raw']
    for i in range(m['nfeat']):
        o = m['off_feat'] + i*24
        cls, npts, noff, poff = struct.unpack_from('<BBHI', d, o)
        bx0, by0, bx1, by1 = struct.unpack_from('<4i', d, o+8)
        yield cls, npts, noff, poff, (bx0, by0, bx1, by1)

def points(m, poff, npts):
    """解出折线：[(lat_e6, lon_e6)]，相对文件 min 已加回"""
    d = m['raw']; base = m['off_pt'] + poff
    lat, lon = struct.unpack_from('<ii', d, base)
    lat += m['bbox'][0]*1e6 if False else 0  # 保持原始相对值, 下面统一加
    out = [(lat, lon)]
    k = 1
    while k < npts:
        rem = npts - k
        n = 8 if rem >= 8 else 4 if rem >= 4 else 2 if rem >= 2 else 1
        fmt = '<' + ('h' * (n*2))
        vals = struct.unpack_from(fmt, d, base + 8 + (k-1)*4)
        for j in range(n):
            lat += vals[j*2]; lon += vals[j*2+1]
            out.append((lat, lon))
        k += n
    return out

if __name__ == '__main__':
    m = read_ltm(sys.argv[1] if len(sys.argv) > 1 else '/tmp/fj.bin')
    print('size=%d ver=%d layer=%d nlod=%d' % (m['size'], m['ver'], m['layer'], m['nlod']))
    print('bbox lat %.6f..%.6f  lon %.6f..%.6f' % (m['bbox'][0], m['bbox'][2], m['bbox'][1], m['bbox'][3]))
    print('off_lod=%d off_feat=%d off_pt=%d off_str=%d off_idx=%d' %
          (m['off_lod'], m['off_feat'], m['off_pt'], m['off_str'], m['off_idx']))
    print('nfeat=%d npt=%d crc=%d' % (m['nfeat'], m['npt'], m['crc']))
    print('feat 段字节=%d = nfeat*24 ? %s' % (m['off_pt']-m['off_feat'], (m['off_pt']-m['off_feat']) == m['nfeat']*24))
    print('LOD:', m['lods'])
    hist = collections.Counter(); npts_hist = collections.Counter(); bad = 0
    for cls, npts, noff, poff, bb in features(m):
        hist[cls] += 1; npts_hist[min(npts, 10)] += 1
        if poff + 8 > m['off_str'] - m['off_pt']: bad += 1
    print('cls 直方图:', dict(sorted(hist.items())))
    print('点数分布(≥10 归 10):', dict(sorted(npts_hist.items())))
    print('疑似越界要素:', bad)
    # 抽样解一条
    for cls, npts, noff, poff, bb in features(m):
        if npts > 20:
            pts = points(m, poff, npts)
            print('样例 cls=%d npts=%d 首点(相对)=%s 末点=%s' % (cls, npts, pts[0], pts[-1]))
            break
