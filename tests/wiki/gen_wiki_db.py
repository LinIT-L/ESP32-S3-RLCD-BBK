#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_wiki_db.py — 生成一块「离线维基」测试库，供主机侧测试 wiki_db.c 用。

不依赖 lz4 / pypinyin：
  * LZ4 用 literal-only 编码（全当字面量），wiki_db.c 的 lz4_dec_block 完全支持，
    代价是 clen > ulen（不压缩），对功能测试无影响。
  * 拼音索引直接取测试数据里预置的拼音字段，不做中文转拼音。

产出目录结构（与固件 /sdcard/wiki/<库名>/ 完全一致）：
    metadata.json
    index.bin            80B/条：title[64] chunk:u32 off:u32 clen:u32 ulen:u32（小端）
    articles_0000.dat    'WK1\\0' ulen:u32 clen:u32 crc:u32 + lz4 block
    py.bin               全拼索引 64B/条：key[60] + id:u32
    pyi.bin              首字母索引 64B/条

用法： python3 tests/wiki/gen_wiki_db.py [输出目录]
默认输出 tests/wiki/data/testlib
"""
import json
import os
import struct
import sys
import zlib

IDX_REC = 80
TITLE_MAX = 64
PY_KEY = 60
PY_REC = 64
CHUNK_SIZE = 250          # 每块最多 250 篇（测试库只需 1 块）

# (标题, 全拼, 首字母, 正文, 故意标记损坏)
ARTICLES = [
    ("北京", "beijing", "bj",
     "<p>北京是中华人民共和国首都，位于华北平原北部。</p>"
     "<p>它是政治、文化、国际交往与科技创新中心。</p>", False),
    ("上海", "shanghai", "sh",
     "<p>上海位于长江入海口，是中国最大的经济中心城市。</p>"
     "<div>外滩与陆家嘴隔江相望。</div>", False),
    ("广州", "guangzhou", "gz",
     "<p>广州别称羊城，广东省省会。</p><p>粤菜与早茶闻名。</p>", False),
    ("深圳", "shenzhen", "sz",
     "<p>深圳位于珠江口东岸，1980 年设立经济特区。</p>", False),
    ("杭州", "hangzhou", "hz",
     "<p>杭州有西湖，素有人间天堂之誉。</p>", False),
    ("南京", "nanjing", "nj",
     "<p>南京古称金陵，六朝古都。</p>", False),
    ("成都", "chengdu", "cd",
     "<p>成都位于四川盆地西部，天府之国。</p>", False),
    ("重庆", "chongqing", "cq",
     "<p>重庆是直辖市，山城地形独特。</p>", False),
    ("武汉", "wuhan", "wh",
     "<p>武汉由武昌、汉口、汉阳三镇组成。</p>", False),
    ("西安", "xian", "xa",
     "<p>西安古称长安，十三朝古都，有兵马俑。</p>", False),
    ("长安", "changan", "ca",
     "<p>长安是西安的古称。</p>", False),
    ("长城", "changcheng", "cc",
     "<p>长城始建于春秋战国，秦始皇连接各段。</p>"
     "<p>现存多为明代所筑，全长两万余公里。</p>", False),
    ("黄河", "huanghe", "hh",
     "<p>黄河是中国第二长河，被称为母亲河。</p>", False),
    ("长江", "changjiang", "cj",
     "<p>长江是中国第一长河，全长约 6300 公里。</p>", False),
    ("珠穆朗玛峰", "zhumulangmafeng", "zmlmf",
     "<p>珠穆朗玛峰海拔 8848.86 米，世界最高峰。</p>", False),
    ("汉字", "hanzi", "hz",
     "<p>汉字是记录汉语的文字，现存最早为甲骨文。</p>"
     "<p>常用字约三千余个。</p>", False),
    ("拼音", "pinyin", "py",
     "<p>汉语拼音是汉字标音方案，1958 年公布。</p>", False),
    ("长城与运河", "changchengyuyunhe", "ccyyh",
     "<p>长城与京杭大运河一北一南，是中国古代两大工程。</p>", False),
    ("京杭大运河", "jinghangdayunhe", "jhdyh",
     "<p>京杭大运河北起北京，南至杭州，全长约 1794 公里。</p>", False),
    ("气候变化", "qihoubianhua", "qhbh",
     '<p>气候变化的两个符号要能正确显示：&amp; 与 &lt;与&gt;</p>'
     "<p>不间断空格测试：A&#160;B&#160;C</p>"
     "<p>中文标点：你好，世界。这是“引号”。</p>", False),
    # 超长文章：验证折行、分页、行表增长（约 120KB 正文）
    ("长文测试", "changwen ceshi", "cwcs",
     "".join("<p>第 %03d 段：这是一段用于验证长文本折行与滚动的测试文字，"
             "包含中文、English words、数字 1234567890 与标点，"
             "用来检查 384px 软折行、英文不断词、标点悬挂是否正常。</p>" % i
             for i in range(1, 401)), False),
    # 极短
    ("空", "kong", "k", "<p></p>", False),
    ("一", "yi", "y", "单字。", False),
    ("A", "a", "a", "<p>纯 ASCII 标题，测英文排序与大小写敏感性。</p>", False),
    ("AI", "ai", "ai", "<p>两个字母的标题。</p>", False),
    ("Abc Def", "abc def", "ad",
     "<p>带空格的英文标题。</p>", False),
    ("符号&amp;测试", "fuhao", "fh",
     "<p>索引里的标题含 &amp; 实体，看看搜索能不能命中。</p>", False),
    ("数学符号", "shuxuefuhao", "sxfh",
     "<p>&#x4E2D;&#x6587; 用十六进制实体写的。</p>"
     "<p>十进制实体：&#20013;&#25991;</p>", False),
    ("表格测试", "biaoge", "bg",
     "<table><tr><td>甲</td><td>乙</td></tr>"
     "<tr><td>丙</td><td>丁</td></tr></table>", False),
    ("列表测试", "liebiao", "lb",
     "<ul><li>第一项</li><li>第二项</li></ul>", False),
    # 故意损坏：chunk = 0xFFFFFFFF，wiki_load 应返回 -3
    ("坏条目", "huaidiaomu", "hdm", "", True),
]


def lz4_literal(data: bytes) -> bytes:
    """LZ4 block 的「全字面量」编码。解压器逐 token 处理，末段无 offset。"""
    n = len(data)
    out = bytearray()
    if n < 15:
        out.append(n << 4)
    else:
        out.append(0xF0)
        rem = n - 15
        while rem >= 255:
            out.append(255)
            rem -= 255
        out.append(rem)
    out += data
    return bytes(out)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "data", "testlib")
    os.makedirs(out_dir, exist_ok=True)

    # ---- 按标题的 UTF-8 字节序排序（wiki_search 二分依赖这个顺序）----
    items = sorted(ARTICLES, key=lambda a: a[0].encode("utf-8"))

    # ---- 写 articles_0000.dat，同时记下每篇的 (off, clen, ulen) ----
    chunk_path = os.path.join(out_dir, "articles_0000.dat")
    records = []
    with open(chunk_path, "wb") as f:
        off = 0
        for title, _, _, body, broken in items:
            if broken:
                records.append((0xFFFFFFFF, 0, 0, 0))
                continue
            raw = body.encode("utf-8")
            comp = lz4_literal(raw)
            crc = zlib.crc32(raw) & 0xFFFFFFFF
            hdr = b"WK1\x00" + struct.pack("<III", len(raw), len(comp), crc)
            f.write(hdr)
            f.write(comp)
            records.append((0, off, len(comp), len(raw)))
            off += len(hdr) + len(comp)

    # ---- index.bin ----
    with open(os.path.join(out_dir, "index.bin"), "wb") as f:
        for (title, _, _, _, _), (chunk, off, clen, ulen) in zip(items, records):
            tb = title.encode("utf-8")[:TITLE_MAX - 1]
            f.write(tb + b"\x00" * (TITLE_MAX - len(tb)))
            f.write(struct.pack("<IIII", chunk, off, clen, ulen))

    # ---- py.bin / pyi.bin（按 key 字节序排序）----
    def write_py(fname, key_index):
        rows = []
        for i, a in enumerate(items):
            if a[4]:
                continue
            k = a[key_index].encode("ascii", "ignore")[:PY_KEY - 1]
            rows.append((k, i))
        rows.sort(key=lambda r: r[0])
        with open(os.path.join(out_dir, fname), "wb") as f:
            for k, i in rows:
                f.write(k + b"\x00" * (PY_KEY - len(k)) + struct.pack("<I", i))

    write_py("py.bin", 1)     # 全拼
    write_py("pyi.bin", 2)    # 首字母

    # ---- metadata.json ----
    meta = {
        "version": 2,
        "articleCount": len(items),
        "chunks": {"count": 1, "pattern": "articles_%04d.dat"},
        "title": "测试库",
        "built": "host-test",
    }
    with open(os.path.join(out_dir, "metadata.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)

    total = sum(os.path.getsize(os.path.join(out_dir, x)) for x in os.listdir(out_dir))
    print("生成完成:", out_dir)
    for x in sorted(os.listdir(out_dir)):
        print("  %-20s %8d B" % (x, os.path.getsize(os.path.join(out_dir, x))))
    print("  合计 %d B | %d 篇（含 1 篇故意损坏）" % (total, len(items)))


if __name__ == "__main__":
    main()
