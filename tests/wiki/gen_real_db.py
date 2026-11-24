#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_real_db.py — 生成「真实 LZ4 压缩」的小型测试库

与 gen_wiki_db.py 的区别（两者并存，各管一段）：
  * gen_wiki_db.py  用手工 literal-only LZ4，覆盖「解压器基本路径」
  * 本脚本         用 lz4.block 真实压缩（含 match/offset），覆盖
                   「生产格式真实链路」：高重复文本、长文、1MB 边界、坏帧

产出目录结构完全同生产库，固件/主机测试都能直接读：
    metadata.json / index.bin / articles_0000.dat / articles_0001.dat
    py.bin / pyi.bin

用法： python3 tests/wiki/gen_real_db.py [输出目录]
默认 tests/wiki/data/reallib
"""
import json
import os
import struct
import sys
import zlib

import lz4.block

IDX_REC = 80
TITLE_MAX = 64
PY_KEY = 60
PY_REC = 64

CHUNK_GOOD = 0          # 正常文章
CHUNK_BAD = 1           # 故意做坏的帧（坏 magic / 截断 / 长度不符）


def lz4c(data: bytes) -> bytes:
    """真实 LZ4 block（无长度前缀）——C 层 lz4_dec_block 要的就是这个"""
    return lz4.block.compress(data, mode="high_compression", store_size=False)


def gen_cases():
    """(标题, 全拼, 首字母, 正文HTML, 类别)
    类别: ok / bad_magic / bad_trunc / bad_len"""
    cases = []

    # 1. 普通中文
    cases.append((
        "北京", "beijing", "bj",
        "<p>北京是中华人民共和国的首都，位于华北平原北部。</p>"
        "<p>它是政治、文化、国际交往与科技创新中心。</p>", "ok"))

    # 2. 高重复率文本 —— LZ4 会大量用到 match/offset，压缩率极高
    cases.append((
        "高重复测试", "gaozhongfu", "gzf",
        "<p>" + ("重复的内容重复的内容重复的内容。" * 300) + "</p>", "ok"))

    # 3. 长文 500 段 —— 测行表增长与首/中/末段
    cases.append((
        "长文", "changwen", "cw",
        "".join("<p>第 %03d 段：这是长文本折行测试，中文 mixed English 1234567890。</p>" % i
                for i in range(1, 501)), "ok"))

    # 4. HTML 实体
    cases.append((
        "实体", "shiti", "st",
        "<p>符号 &amp; 小于 &lt; 大于 &gt; 引号 &quot;</p>"
        "<p>不间断空格 A&#160;B&#160;C 与十六进制 &#x4E2D;&#x6587;</p>", "ok"))

    # 5. 边界：正文恰好 65536 字节（LZ4 offset 上限 65535 的量级）
    base = "<p>边界测试内容 ABCDEFG 0123456789</p>"
    body64k = base
    while len(body64k.encode()) < 65536:
        body64k += base
    body64k = body64k[:65536 - len("<p></p>")] + "</p>"
    cases.append(("边界64K", "bianjie64k", "bj64k", body64k, "ok"))

    # 6. 边界：1MB 正文 —— 逼近固件单篇上限
    base = "<p>一兆字节边界测试，用来验证大块 LZ4 解压与 PSRAM 分配。12345</p>"
    big = base * ((1024 * 1024) // len(base.encode()) + 2)
    big = big[:1024 * 1024 - 8] + "</p>"
    cases.append(("边界1M", "bianjie1m", "bj1m", big, "ok"))

    # 7. 空段落（只有标签，C 层会折叠成 0 行）
    cases.append(("空段", "kongduan", "kd", "<p></p><p>  </p>", "ok"))

    # 8. 多音字（生成器/拼音索引侧覆盖；这里给 C 层验证前缀命中）
    cases.append(("重庆", "chongqing", "cq", "<p>重庆是直辖市，山城地形独特。</p>", "ok"))
    cases.append(("长沙", "changsha", "cs", "<p>长沙是湖南省省会。</p>", "ok"))
    cases.append(("长安", "changan", "ca", "<p>长安是西安的古称。</p>", "ok"))
    cases.append(("厦门", "xiamen", "xm", "<p>厦门位于福建东南沿海。</p>", "ok"))

    # 9. 故意坏帧
    cases.append(("坏魔术", "huaimoshu", "hms",
                  "<p>这一篇的帧头 magic 被打坏了。</p>", "bad_magic"))
    cases.append(("坏截断", "huaijieduan", "hjd",
                  "<p>这一篇的数据在文件末尾被截断了。</p>", "bad_trunc"))
    cases.append(("坏长度", "huachangdu", "hcd",
                  "<p>这一篇帧头里的 ulen 与索引不一致。</p>", "bad_len"))

    return cases


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "data", "reallib")
    os.makedirs(out, exist_ok=True)

    cases = sorted(gen_cases(), key=lambda c: c[0].encode("utf-8"))

    # chunk0 放正常文章，chunk1 放坏帧
    f0 = open(os.path.join(out, "articles_%04d.dat" % CHUNK_GOOD), "wb")
    f1 = open(os.path.join(out, "articles_%04d.dat" % CHUNK_BAD), "wb")
    off = {CHUNK_GOOD: 0, CHUNK_BAD: 0}
    recs = []

    for title, _full, _ini, body, kind in cases:
        raw = body.encode("utf-8")
        comp = lz4c(raw)
        crc = zlib.crc32(raw) & 0xFFFFFFFF

        if kind == "bad_magic":
            hdr = b"XXX\x00" + struct.pack("<III", len(raw), len(comp), crc)
            f = f1
        elif kind == "bad_trunc":
            comp = comp[:max(1, len(comp) // 2)]      # 数据只写一半
            hdr = b"WK1\x00" + struct.pack("<III", len(raw), len(comp), crc)
            f = f1
        elif kind == "bad_len":
            hdr = b"WK1\x00" + struct.pack("<III", len(raw) + 999, len(comp), crc)
            f = f1
        else:
            hdr = b"WK1\x00" + struct.pack("<III", len(raw), len(comp), crc)
            f = f0

        chunk = CHUNK_BAD if kind != "ok" else CHUNK_GOOD
        f.write(hdr)
        f.write(comp)
        recs.append((title, chunk, off[chunk], len(comp), len(raw)))
        off[chunk] += 16 + len(comp)

    f0.close()
    f1.close()

    with open(os.path.join(out, "index.bin"), "wb") as f:
        for title, chunk, o, cl, ul in recs:
            tb = title.encode("utf-8")[:TITLE_MAX - 1]
            f.write(tb + b"\x00" * (TITLE_MAX - len(tb)))
            f.write(struct.pack("<IIII", chunk, o, cl, ul))

    def write_py(name, idx):
        rows = sorted(
            ((c[idx].encode("ascii")[:PY_KEY - 1], i) for i, c in enumerate(cases)),
            key=lambda r: r[0])
        with open(os.path.join(out, name), "wb") as f:
            for k, i in rows:
                f.write(k + b"\x00" * (PY_KEY - len(k)) + struct.pack("<I", i))

    write_py("py.bin", 1)
    write_py("pyi.bin", 2)

    meta = {
        "version": 2,
        "articleCount": len(recs),
        "chunks": {"count": 2, "pattern": "articles_%04d.dat"},
        "title": "真实压缩测试库",
        "built": "host-real-lz4",
        "note": "lz4.block high_compression, 含坏帧条目",
    }
    with open(os.path.join(out, "metadata.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)

    total = sum(os.path.getsize(os.path.join(out, n)) for n in os.listdir(out))
    print("生成完成:", out)
    for n in sorted(os.listdir(out)):
        print("  %-20s %9d B" % (n, os.path.getsize(os.path.join(out, n))))
    print("  %d 篇（正常 %d / 坏帧 3）合计 %.2f MB"
          % (len(recs), len(recs) - 3, total / 1e6))


if __name__ == "__main__":
    main()
