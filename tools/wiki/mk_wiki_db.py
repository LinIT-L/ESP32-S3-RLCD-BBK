#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mk_wiki_db.py — LinTOS 离线维基「生产级」数据库生成器

从 MediaWiki 官方 dump（.xml / .xml.bz2，或任意同格式 XML）生成
固件可直接读取的数据库目录：

    <out>/
      metadata.json        版本/篇数/来源/授权/统计/校验值
      index.bin            80 B/条：title[64] chunk:u32 off:u32 clen:u32 ulen:u32
      articles_%04d.dat    每篇：'WK1\\0' ulen:u32 clen:u32 crc:u32 + LZ4 block
      py.bin               全拼索引  64 B/条：key[60] + id:u32
      pyi.bin              首字母索引 64 B/条
      build.log            逐阶段日志 + 跳过原因统计
      .build_state.json    断点恢复用（生成完成后删除）
      SHA256SUMS           各文件校验值

**格式与 components/services/wiki/wiki_db.c 严格一致，不允许改动**：
  * index.bin 必须恰好 articleCount × 80 字节，否则 wiki_open() 直接拒读
  * 帧头 16 字节 magic 必须是 'W','K','1',0，ulen/clen 必须与索引一致
  * 索引按标题的 UTF-8 字节序升序（C 层用 memcmp 二分）
  * py.bin / pyi.bin 按 key 的字节序升序

真实 LZ4：用 lz4.block.compress(store_size=False) 生成标准 LZ4 block
（C 层 lz4_dec_block 只认裸 block，不认带 4 字节长度前缀的 frame 格式）。

用法示例：
  # 小样本（推荐先跑这个验证链路）
  python3 tools/wiki/mk_wiki_db.py -i zhwiki-p1.xml.bz2 -o /tmp/wiki_build/sample --limit 2000

  # 全量分片
  python3 tools/wiki/mk_wiki_db.py -i zhwiki-p1.xml.bz2 -o /tmp/wiki_build/zhwiki

依赖：lz4（必须）、pypinyin（必须，生成拼音索引）
  python3 -m pip install lz4 pypinyin

授权提示：中文维基百科正文为 CC BY-SA 4.0（部分内容另有 GFDL）。
分发时必须保留来源与许可信息 —— 本工具会自动写进 metadata.json。
"""

import argparse
import bz2
import gzip
import hashlib
import html
import json
import os
import re
import shutil
import struct
import sys
import time
import zlib
from datetime import datetime, timezone

# ---------------------------------------------------------------- 格式常量
IDX_REC = 80
TITLE_MAX = 64          # 索引里标题字段宽度（含结尾 NUL ⇒ 最多 63 字节）
PY_KEY = 60
PY_REC = 64
FRAME_HDR = 16
MAX_ARTICLE = 4 * 1024 * 1024      # 与 wiki_db.h 的 WIKI_MAX_ARTICLE 一致

STATE_NAME = ".build_state.json"
LOG_NAME = "build.log"


# ---------------------------------------------------------------- 日志
class Log:
    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8")

    def __call__(self, msg):
        line = "[%s] %s" % (datetime.now().strftime("%H:%M:%S"), msg)
        print(line, flush=True)
        self.f.write(line + "\n")
        self.f.flush()

    def close(self):
        self.f.close()


# ---------------------------------------------------------------- 文本清洗
# MediaWiki 的 <text> 里是 wikitext。读取器只会剥 HTML 标签，
# 所以这里必须把 wikitext 变成「带 <p>/<h2>/<li> 的纯文本 HTML」：
#   * <p>/<h2>/<li> 都是 wiki_db.c tag_is_block() 认的块级标签 → 触发换行
#   * 内容里的 & < > 统一转义，C 层 html_to_text() 会解码回原字符

RE_COMMENT = re.compile(r"<!--.*?-->", re.S)
RE_REF = re.compile(r"<ref[^>]*?/>|<ref[^>]*?>.*?</ref>", re.S | re.I)
RE_TAG = re.compile(r"</?[a-zA-Z][^>]{0,400}>", re.S)      # 剩余 HTML 标签
RE_TEMPLATE = re.compile(r"\{\{[^{}]*\}\}", re.S)          # 单层模板
RE_TABLE = re.compile(r"\{\|.*?\|\}", re.S)
RE_EXT_LINK = re.compile(r"\[(?:https?:)?//[^\]]*\]", re.S)
RE_HEADING = re.compile(r"^(={1,6})\s*(.+?)\s*\1\s*$")
RE_LIST = re.compile(r"^[*#:;]+\s*")
RE_MAGICLINK = re.compile(r"__[A-Z]+__")

# 这些命名空间的内链不是正文（文件/分类/模板…），整段删掉
NS_PREFIX_DROP = (
    "file:", "image:", "category:", "media:", "special:", "help:", "template:",
    "wikipedia:", "portal:", "draft:", "module:", "user:", "talk:", "mediawiki:",
    "文件:", "檔案:", "分类:", "分類:", "图像:", "圖像:", "图片:", "圖片:",
    "模板:", "帮助:", "幫助:", "维基百科:", "維基百科:", "用户:", "用戶:",
    "讨论:", "討論:", "主题:", "主題:", "特殊:", "媒体:", "媒體:",
)

# wikitext 里常见、但必须转成真实字符的命名实体
ENT_MAP = {
    "nbsp": " ", "ensp": " ", "emsp": " ", "thinsp": " ",
    "amp": "&", "lt": "<", "gt": ">", "quot": '"', "apos": "'",
    "middot": "·", "mdash": "—", "ndash": "–", "hellip": "…",
    "ldquo": "“", "rdquo": "”", "lsquo": "‘", "rsquo": "’",
    "times": "×", "divide": "÷", "deg": "°", "plusmn": "±",
}
RE_ENT = re.compile(r"&(#x?[0-9a-fA-F]+|[a-zA-Z]+);")


def _ent_sub(m):
    name = m.group(1)
    if name.startswith("#"):
        try:
            cp = int(name[2:], 16) if name[1:2].lower() == "x" else int(name[1:])
            if cp == 160:
                return " "
            if cp in (0x200B, 0x200C, 0x200D, 0xFEFF):
                return ""
            if 0 < cp <= 0x10FFFF:
                return chr(cp)
        except ValueError:
            return m.group(0)
        return m.group(0)
    low = name.lower()
    if low in ENT_MAP:
        return ENT_MAP[low]
    return m.group(0)


def _strip_templates(s, rounds=3):
    """模板可以嵌套：{{a|{{b}}}}。逐层剥离比写递归解析器稳。"""
    for _ in range(rounds):
        new = RE_TEMPLATE.sub("", s)
        if new == s:
            break
        s = new
    return s


def _int_link(m):
    """[[目标|显示文字]] → 显示文字；[[目标]] → 目标；文件/分类整段丢弃。"""
    body = m.group(1)
    target, _, label = body.partition("|")
    tl = target.strip().lower()
    for p in NS_PREFIX_DROP:
        if tl.startswith(p):
            return ""
    label = label.strip()
    # 形如 [[A|B]] 里 B 可能还有 [[..]]，已由非贪婪匹配处理
    return label if label else target.strip()


RE_INT_LINK = re.compile(r"\[\[([^\[\]]{1,300})\]\]")


def wikitext_to_html(text):
    """wikitext → 极简 HTML（只保留 <p> <h2> <li> 三种块级标签）"""
    if not text:
        return ""

    s = RE_COMMENT.sub("", text)
    s = RE_REF.sub("", s)
    s = RE_TABLE.sub("", s)
    s = _strip_templates(s)

    # 内链可能层层嵌套（[[A|[[B]]]]），扫两遍足够覆盖实际语料
    for _ in range(2):
        s2 = RE_INT_LINK.sub(_int_link, s)
        if s2 == s:
            break
        s = s2

    s = RE_EXT_LINK.sub("", s)
    s = RE_MAGICLINK.sub("", s)
    s = RE_TAG.sub("", s)
    s = s.replace("'''", "").replace("''", "")

    # 剩下 wikitext 自带的 HTML 实体（XML 已解过一层，这里再解一层）
    s = RE_ENT.sub(_ent_sub, s)

    # 内容与标签分开组装：标签原样输出，内容单独转义。
    # 先拼后转义会把标签自己的 < > 一起转掉（上一版就踩了这个坑）。
    def esc(s):
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

    out = []
    para = []

    def flush():
        if para:
            chunk = " ".join(para).strip()
            if chunk:
                out.append("<p>" + esc(chunk) + "</p>")
            para.clear()

    for raw in s.split("\n"):
        line = raw.strip()
        if not line:
            flush()
            continue
        m = RE_HEADING.match(line)
        if m:
            flush()
            lvl = min(len(m.group(1)) + 1, 6)
            title = m.group(2).strip()
            if title:
                out.append("<h%d>%s</h%d>" % (lvl, esc(title), lvl))
            continue
        if line[0] in "*#;:":
            flush()
            item = RE_LIST.sub("", line).strip()
            if item:
                out.append("<li>" + esc(item) + "</li>")
            continue
        if line[0] in "{|}":
            continue
        para.append(line)

    flush()
    return "".join(out)


# ---------------------------------------------------------------- 拼音
class PinyinIndex:
    """标题 → 全拼 / 首字母。

    多音字靠 pypinyin 的内置词组表（重庆 chóngqìng、长沙 chángshā、
    长安 cháng'ān、厦门 xiàmén 都覆盖）。个别词条读音不对时可以往
    CUSTOM_PHRASES 里加一条，不用改全局。
    """

    # 额外兜底：pypinyin 版本差异导致个别常见词读错的，在这里补
    CUSTOM_PHRASES = {}

    def __init__(self):
        from pypinyin import lazy_pinyin, Style
        try:
            from pypinyin import phrases_dict
            if self.CUSTOM_PHRASES:
                for k, v in self.CUSTOM_PHRASES.items():
                    phrases_dict.pinyin_dict.setdefault(ord(k[0]), {})
        except Exception:
            pass
        self._lazy = lazy_pinyin
        self._normal = Style.NORMAL
        self._first = Style.FIRST_LETTER

    def full(self, title):
        out = self._lazy(title, style=self._normal, errors="ignore")
        s = "".join(out).lower()
        return re.sub(r"[^a-z0-9]", "", s)

    def initials(self, title):
        out = self._lazy(title, style=self._first, errors="ignore")
        s = "".join(out).lower()
        return re.sub(r"[^a-z0-9]", "", s)


# ---------------------------------------------------------------- LZ4
def lz4_compress(data):
    import lz4.block
    # store_size=False 才是裸 block；默认会带 4 字节长度前缀，C 层解不了
    return lz4.block.compress(data, mode="high_compression", store_size=False)


# ---------------------------------------------------------------- 流式解析
RE_PAGE_OPEN = re.compile(rb"<page>")
RE_PAGE_CLOSE = re.compile(rb"</page>")
RE_TITLE = re.compile(rb"<title>(.*?)</title>", re.S)
RE_NS = re.compile(rb"<ns>(\d+)</ns>")
RE_TEXT = re.compile(rb"<text[^>]*>(.*?)</text>", re.S)
RE_REDIRECT = re.compile(rb"<redirect\b")


def open_maybe_compressed(path):
    """按扩展名 + magic 判断压缩格式。bz2 可能是 multistream（维基分片就是）。"""
    with open(path, "rb") as f:
        magic = f.read(3)
    if magic[:2] == b"\x1f\x8b":
        return gzip.open(path, "rb")
    if magic == b"BZh":
        # Python 的 bz2 能顺序读完多个 concatenated stream
        return bz2.open(path, "rb")
    return open(path, "rb")


def iter_pages(f, skip=0, chunk=1 << 20):
    """流式产出 (title_bytes, ns, text_bytes)。
    skip: 跳过前 N 个 <page>（断点恢复用，不做任何解码）。"""
    buf = b""
    seen = 0
    while True:
        data = f.read(chunk)
        if not data:
            break
        buf += data
        while True:
            m = RE_PAGE_OPEN.search(buf)
            if not m:
                # 保留可能被切断的半个 '<page>' 前缀
                if len(buf) > 16:
                    buf = buf[-16:]
                break
            start = m.end()
            e = RE_PAGE_CLOSE.search(buf, start)
            if not e:
                buf = buf[m.start():]        # 等更多数据
                break
            block = buf[start:e.start()]
            buf = buf[e.end():]
            seen += 1
            if seen <= skip:
                continue
            yield block


def parse_page(block):
    """返回 (title_bytes, ns, text_bytes) 或 None"""
    mt = RE_TITLE.search(block)
    if not mt:
        return None
    title = html.unescape(mt.group(1).decode("utf-8", "replace")).strip()
    mn = RE_NS.search(block)
    ns = int(mn.group(1)) if mn else 0
    if RE_REDIRECT.search(block):
        return title, ns, None, "redirect"
    mt2 = RE_TEXT.search(block)
    if not mt2:
        return title, ns, None, "no_text"
    text = html.unescape(mt2.group(1).decode("utf-8", "replace"))
    return title, ns, text, None


# ---------------------------------------------------------------- 主流程
def clean_title(t, mode):
    """返回 (title_bytes, None) 或 (None, 失败原因)。

    64 字节是硬约束（索引字段宽 64，最后 1 字节留给 NUL）。
    默认策略「skip」：超长标题直接跳过 —— 截断会让两条不同词条变成
    同一个排序键，二分索引立刻出错，这个风险不能接受。
    """
    t = t.replace("\u0000", "").strip()
    t = re.sub(r"[\r\n\t]", " ", t)
    t = re.sub(r"\s{2,}", " ", t)
    if not t:
        return None, "empty_title"
    b = t.encode("utf-8")
    if len(b) <= TITLE_MAX - 1:
        return b, None
    if mode == "truncate":
        b = b[:TITLE_MAX - 1]
        while b and (b[-1] & 0xC0) == 0x80:     # 不切断 UTF-8 字符
            b = b[:-1]
        if not b:
            return None, "long_title"
        return b, "truncated"
    return None, "long_title"


def sha256_file(path, bufsize=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(bufsize)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description="生成 LinTOS 离线维基数据库")
    ap.add_argument("-i", "--input", required=True, help="MediaWiki XML dump（.xml / .xml.bz2 / .xml.gz）")
    ap.add_argument("-o", "--out", required=True, help="输出目录（必须为空或不存在）")
    ap.add_argument("--limit", type=int, default=0, help="最多收录多少篇（0 = 不限）")
    ap.add_argument("--min-len", type=int, default=80, help="正文最小字节数（默认 80，滤掉极短词条）")
    ap.add_argument("--max-len", type=int, default=MAX_ARTICLE, help="正文最大字节数（默认 4MB，与固件上限一致）")
    ap.add_argument("--chunk-size", type=int, default=250, help="每个 articles_XXXX.dat 放多少篇（默认 250）")
    ap.add_argument("--title-mode", choices=["skip", "truncate"], default="skip",
                    help="标题超过 63 字节时的策略（默认 skip，最安全）")
    ap.add_argument("--resume", action="store_true", help="从 .build_state.json 断点续做")
    ap.add_argument("--pause-after", type=int, default=0, metavar="N",
                    help="收满 N 篇后暂停并保存状态（配合 --resume 分批生成）")
    ap.add_argument("--force", action="store_true", help="允许写入已有 metadata.json 的目录（危险）")
    ap.add_argument("--title", default="", help="库显示名（写进 metadata.json）")
    ap.add_argument("--source-url", default="https://dumps.wikimedia.org/zhwiki/", help="数据来源 URL")
    ap.add_argument("--license", default="CC BY-SA 4.0", help="内容许可")
    args = ap.parse_args()

    t0 = time.time()
    out = args.out
    os.makedirs(out, exist_ok=True)

    log = Log(os.path.join(out, LOG_NAME))
    log("=" * 68)
    log("生成离线维基数据库")
    log("  输入: %s" % args.input)
    log("  输出: %s" % out)
    log("  上限: %s 篇" % (args.limit or "不限"))

    # ---- 拒绝覆盖已有正式库 ----
    meta_path = os.path.join(out, "metadata.json")
    state_path = os.path.join(out, STATE_NAME)
    if os.path.exists(meta_path) and not args.force:
        log("!! 目标目录已有 metadata.json（看起来是已完成的库），拒绝覆盖。")
        log("   换个新目录，或确认后加 --force。")
        log.close()
        return 2

    # ---- 断点状态 ----
    st = None
    if args.resume and os.path.exists(state_path):
        with open(state_path, encoding="utf-8") as f:
            st = json.load(f)
        if st.get("input_size") != os.path.getsize(args.input):
            log("!! 输入文件大小与上次不一致，不能续做（请去掉 --resume 重来）")
            log.close()
            return 2
        log("断点恢复：已扫 %d 页 / 已收 %d 篇 / chunk=%d off=%d"
            % (st["pages_seen"], st["articles"], st["chunk"], st["chunk_off"]))
    else:
        # 清掉上次残留（只清本工具自己的产物，不 rm -rf 整个目录）
        for name in ("index.bin", "py.bin", "pyi.bin", "SHA256SUMS", STATE_NAME):
            p = os.path.join(out, name)
            if os.path.exists(p):
                os.remove(p)
        for name in os.listdir(out):
            if re.match(r"articles_\d{4}\.dat$", name):
                os.remove(os.path.join(out, name))

    # ---- 中间产物：未排序索引 + 分块数据 ----
    partial_path = os.path.join(out, "index.partial.bin")
    pages_seen = 0
    articles = 0
    chunk_idx = 0
    chunk_off = 0
    chunk_path = os.path.join(out, "articles_%04d.dat" % chunk_idx)
    chunk_articles = 0

    stats = {
        "pages_read": 0, "non_main_ns": 0, "redirect": 0, "no_text": 0,
        "empty_title": 0, "long_title": 0, "truncated": 0,
        "short_body": 0, "long_body": 0, "empty_html": 0,
        "kept": 0, "dup_title": 0,
    }
    body_lens_hist = {}          # 1KB 分桶直方图：断点恢复时体积恒定，不会越存越大
    compressed_total = 0
    raw_total = 0

    if st:
        pages_seen = st["pages_seen"]
        articles = st["articles"]
        chunk_idx = st["chunk"]
        chunk_off = st["chunk_off"]
        chunk_articles = st.get("chunk_articles", 0)
        stats.update(st.get("stats", {}))
        # JSON 里字典的键一定是字符串，读回来必须转 int ——
        # 否则与本次新写入的 int 键混在一起，sorted() 会抛 TypeError。
        body_lens_hist = {int(k): v for k, v in st.get("body_hist", {}).items()}
        raw_total = st.get("raw_total", 0)
        compressed_total = st.get("compressed_total", 0)
        chunk_path = os.path.join(out, "articles_%04d.dat" % chunk_idx)
        partial = open(partial_path, "ab")
        cf = open(chunk_path, "ab")
    else:
        partial = open(partial_path, "wb")
        cf = open(chunk_path, "wb")

    py = PinyinIndex()
    start_articles = articles      # --pause-after 按「本轮新增」计数，而不是累计
    entries = []            # [(title_bytes, chunk, off, clen, ulen)]  —— 排序用
    if st:
        # 恢复：把已写好的 partial 索引读回内存（每 80 字节一条）
        with open(partial_path, "rb") as f:
            while True:
                rec = f.read(IDX_REC)
                if len(rec) < IDX_REC:
                    break
                title = rec[:TITLE_MAX].rstrip(b"\x00")
                c, o, cl, ul = struct.unpack("<IIII", rec[TITLE_MAX:TITLE_MAX + 16])
                entries.append((title, c, o, cl, ul))

    def save_state():
        with open(state_path, "w", encoding="utf-8") as f:
            json.dump({
                "input_size": os.path.getsize(args.input),
                "pages_seen": pages_seen,
                "articles": articles,
                "chunk": chunk_idx,
                "chunk_off": chunk_off,
                "chunk_articles": chunk_articles,
                "stats": stats,
                "body_hist": body_lens_hist,
                "raw_total": raw_total,
                "compressed_total": compressed_total,
                "ts": datetime.now(timezone.utc).isoformat(),
            }, f)

    # ---- 主循环 ----
    log("开始解析 …")
    last_report = time.time()
    with open_maybe_compressed(args.input) as src:
        for block in iter_pages(src, skip=pages_seen):
            pages_seen += 1
            stats["pages_read"] += 1

            pr = parse_page(block)
            if pr is None:
                stats["no_text"] += 1
                continue
            title, ns, text, why = pr
            if why == "redirect":
                stats["redirect"] += 1
                continue
            if why == "no_text" or text is None:
                stats["no_text"] += 1
                continue
            if ns != 0:
                stats["non_main_ns"] += 1
                continue

            tb, tres = clean_title(title, args.title_mode)
            if tb is None:
                stats[tres] = stats.get(tres, 0) + 1
                continue
            if tres == "truncated":
                stats["truncated"] += 1

            body = wikitext_to_html(text)
            if not body:
                stats["empty_html"] += 1
                continue
            raw = body.encode("utf-8")
            if len(raw) < args.min_len:
                stats["short_body"] += 1
                continue
            if len(raw) > args.max_len:
                stats["long_body"] += 1
                continue

            comp = lz4_compress(raw)
            if len(comp) > MAX_ARTICLE:
                stats["long_body"] += 1
                continue

            crc = zlib.crc32(raw) & 0xFFFFFFFF
            cf.write(struct.pack("<4sIII", b"WK1\x00", len(raw), len(comp), crc))
            cf.write(comp)

            # 未排序索引：title[64] + chunk/off/clen/ulen（id 排序后才确定）
            partial.write(tb + b"\x00" * (TITLE_MAX - len(tb)))
            partial.write(struct.pack("<IIII", chunk_idx, chunk_off, len(comp), len(raw)))

            entries.append((tb, chunk_idx, chunk_off, len(comp), len(raw)))
            b = len(raw) // 1024
            body_lens_hist[b] = body_lens_hist.get(b, 0) + 1
            raw_total += len(raw)
            compressed_total += len(comp)

            chunk_off += FRAME_HDR + len(comp)
            chunk_articles += 1
            articles += 1
            stats["kept"] += 1

            if chunk_articles >= args.chunk_size:
                cf.close()
                chunk_idx += 1
                chunk_articles = 0
                chunk_off = 0
                chunk_path = os.path.join(out, "articles_%04d.dat" % chunk_idx)
                cf = open(chunk_path, "wb")

            if args.limit and articles >= args.limit:
                log("达到 --limit %d，停止解析" % args.limit)
                break

            if args.pause_after and (articles - start_articles) >= args.pause_after:
                log("本轮新收 %d 篇（累计 %d），按 --pause-after 暂停；--resume 可继续"
                    % (articles - start_articles, articles))
                cf.close()
                partial.close()
                save_state()
                log.close()
                return 3

            if time.time() - last_report > 5:
                log("  … %d 页 / 收录 %d 篇 / 原始 %.1f MB → 压缩 %.1f MB"
                    % (pages_seen, articles, raw_total / 1e6, compressed_total / 1e6))
                save_state()
                last_report = time.time()

    cf.close()
    partial.close()
    save_state()
    log("解析完成：%d 页，收录 %d 篇，耗时 %.1f s" % (pages_seen, articles, time.time() - t0))

    if articles == 0:
        log("!! 一篇都没收到，检查输入格式 / --min-len")
        log.close()
        return 1

    # ---- 按标题 UTF-8 字节序排序（C 层 memcmp 二分的唯一前提）----
    log("排序索引 …")
    entries.sort(key=lambda e: e[0])

    # 防御：排序后相邻同名说明数据异常，只保留第一条并记账
    dedup = []
    prev = None
    for e in entries:
        if prev is not None and e[0] == prev:
            stats["dup_title"] += 1
            continue
        dedup.append(e)
        prev = e[0]
    entries = dedup
    articles = len(entries)

    # ---- 写 index.bin ----
    with open(os.path.join(out, "index.bin"), "wb") as f:
        for title, c, o, cl, ul in entries:
            f.write(title + b"\x00" * (TITLE_MAX - len(title)))
            f.write(struct.pack("<IIII", c, o, cl, ul))
    log("index.bin: %d 条 × 80 = %d 字节" % (articles, articles * IDX_REC))

    # ---- 拼音索引 ----
    log("生成拼音索引 …")
    py_rows = []
    for i, (title, _c, _o, _cl, _ul) in enumerate(entries):
        t = title.decode("utf-8", "replace")
        full = py.full(t)[:PY_KEY - 1]
        ini = py.initials(t)[:PY_KEY - 1]
        if full:
            py_rows.append(("py.bin", full, i))
        if ini:
            py_rows.append(("pyi.bin", ini, i))

    for fname in ("py.bin", "pyi.bin"):
        rows = sorted([(k.encode("ascii"), i) for f, k, i in py_rows if f == fname])
        with open(os.path.join(out, fname), "wb") as f:
            for k, i in rows:
                f.write(k + b"\x00" * (PY_KEY - len(k)) + struct.pack("<I", i))
        log("%s: %d 条" % (fname, len(rows)))

    # ---- metadata.json ----
    # ⚠ wiki_db.c 的 json_int() 用 strstr 找 "version" / "articleCount"，
    #   所以这两个键必须排在最前，且全文不得出现同名键。
    # 分位数从 1KB 分桶直方图算（断点恢复后仍然准确，且状态体积恒定）
    def quantile(p):
        items = sorted((int(k), v) for k, v in body_lens_hist.items())
        total = sum(v for _, v in items)
        if not total:
            return 0
        target = total * p / 100.0
        acc = 0
        for b, cnt in items:
            acc += cnt
            if acc >= target:
                return b * 1024
        return items[-1][0] * 1024

    dist = {"p%g" % p: quantile(p) for p in (50, 90, 99, 99.9)}

    meta = {
        "version": 2,
        "articleCount": articles,
        "chunks": {"count": chunk_idx + 1, "pattern": "articles_%04d.dat"},
        "title": args.title or os.path.basename(os.path.abspath(out)),
        "built": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "generator": "LinTOS mk_wiki_db.py v1",
        "source": {
            "url": args.source_url,
            "input": os.path.basename(args.input),
            "inputBytes": os.path.getsize(args.input),
            "license": args.license,
            "attribution": "内容来自维基百科（Wikipedia®），由维基媒体基金会社区编写，"
                           "按 %s 许可分发。" % args.license,
        },
        "stats": dict(stats),
        "size": {
            "articles": articles,
            "rawBytes": raw_total,
            "compressedBytes": compressed_total,
            "ratio": round((compressed_total / raw_total) if raw_total else 0, 4),
        },
        "bodyLenBytes": dist,
        "skipped": {k: v for k, v in stats.items()
                    if k in ("non_main_ns", "redirect", "no_text", "empty_title",
                             "long_title", "truncated", "short_body", "long_body",
                             "empty_html", "dup_title") and v},
    }
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)

    # ---- 校验值 ----
    lines = []
    for name in sorted(os.listdir(out)):
        if name in (LOG_NAME, STATE_NAME, "SHA256SUMS", "index.partial.bin"):
            continue
        if not os.path.isfile(os.path.join(out, name)):
            continue
        lines.append("%s  %s" % (sha256_file(os.path.join(out, name)), name))
    with open(os.path.join(out, "SHA256SUMS"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    # ---- 收尾 ----
    os.remove(partial_path)
    if os.path.exists(state_path):
        os.remove(state_path)

    total = sum(os.path.getsize(os.path.join(out, n))
                for n in os.listdir(out) if os.path.isfile(os.path.join(out, n)))

    log("-" * 68)
    log("完成：%d 篇 / %d 个 chunk / 数据库 %.2f MB（原始 %.2f MB，压缩率 %.2f）"
        % (articles, chunk_idx + 1, total / 1e6, raw_total / 1e6,
           (compressed_total / raw_total) if raw_total else 0))
    log("跳过统计：" + json.dumps(meta["skipped"], ensure_ascii=False))
    log("正文长度分位（字节）：" + json.dumps(dist, ensure_ascii=False))
    log("总耗时 %.1f s" % (time.time() - t0))
    log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
