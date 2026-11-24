# 离线维基 · 数据库生成与部署

固件侧读取器在 `components/services/wiki/`，Lua 前端是 `apps/lua/apps/app/wiki.lua`。
本目录只负责**把合法数据源做成固件能读的数据库**。

```
原始数据（MediaWiki dump）
   │  mk_wiki_db.py        ← 本目录
   ▼
/sdcard/wiki/<库名>/        ← TF 卡，与应用本体完全分离
   metadata.json  index.bin  articles_%04d.dat  py.bin  pyi.bin
   │
   ▼
wiki_db.c → luat_lib_wiki.c → wiki.lua（只负责画）
```

---

## 1. 数据源与授权

| 项 | 说明 |
|---|---|
| 推荐源 | 中文维基百科官方 dump：`https://dumps.wikimedia.org/zhwiki/` |
| 具体文件 | `zhwiki-<日期>-pages-articles-multistream*.xml*.bz2`（分片，每片 100 篇一个 bz2 流） |
| 体积参考 | 全量 multistream 约 **3.2 GB**（压缩），解压后约 **15.8 GB**；单分片 240–600 MB |
| 内容许可 | **CC BY-SA 4.0**（部分内容另有 GFDL），允许离线分发与二次打包，但必须署名并以相同许可共享 |
| 更新频率 | 官方至少每月一次 |
| 署名要求 | 生成器会自动把来源 URL、许可、输入文件名写进 `metadata.json` 的 `source` 字段 —— **不要手改掉** |

**不要用网页爬取的内容当正式数据源**：一是违反站点条款，二是没有许可信息，三是质量不可控。

生成器只认 MediaWiki XML 格式（`.xml` / `.xml.bz2` / `.xml.gz`），所以任何同格式的
合法 dump（其他语言维基、WikiSource、企业自建 MediaWiki）都能直接喂进来。

---

## 2. 依赖

```bash
python3 -m pip install lz4 pypinyin
```

- `lz4` 必须：生成**真实 LZ4 block**（`store_size=False`，裸 block，无 4 字节长度前缀）
- `pypinyin` 必须：生成 `py.bin` / `pyi.bin`。没有它就没有拼音搜索（掌机只有 52 键 ASCII，
  打不了中文，拼音是唯一输入路径）

多音字靠 pypinyin 内置词组表，已覆盖 重庆 chóngqìng / 长沙 chángshā / 长安 cháng'ān / 厦门 xiàmén。
个别词条读音不对时，改 `mk_wiki_db.py` 里的 `PinyinIndex.CUSTOM_PHRASES`，不用动全局词典。

---

## 3. 用法

### 3.1 先跑小样本（强烈建议）

```bash
# 只收前 2000 篇，10 秒内出结果，用来验证链路
python3 tools/wiki/mk_wiki_db.py \
    -i zhwiki-p1.xml.bz2 \
    -o /tmp/wiki_build/sample \
    --limit 2000 \
    --title "中文维基百科（样本）"
```

### 3.2 全量

```bash
python3 tools/wiki/mk_wiki_db.py \
    -i zhwiki-p1.xml.bz2 \
    -o /tmp/wiki_build/zhwiki \
    --title "中文维基百科"
```

### 3.3 分批 + 断点恢复（大数据集必备）

```bash
# 每轮收 5 万篇后暂停（退出码 3），随时中断都不丢进度
python3 tools/wiki/mk_wiki_db.py -i dump.xml.bz2 -o /tmp/wiki_build/zhwiki --pause-after 50000
python3 tools/wiki/mk_wiki_db.py -i dump.xml.bz2 -o /tmp/wiki_build/zhwiki --pause-after 50000 --resume
# … 重复直到跑完（最后一次不加 --pause-after）
python3 tools/wiki/mk_wiki_db.py -i dump.xml.bz2 -o /tmp/wiki_build/zhwiki --resume
```

断点状态存在 `.build_state.json`，记录：输入文件大小（变了就拒绝续做）、已扫页数、
已收篇数、当前 chunk 与偏移、跳过统计、正文长度直方图。
**分批生成与一次性生成的产物逐字节相同**（已用 md5 验证）。

### 3.4 常用参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--limit N` | 0（不限） | 最多收录 N 篇 |
| `--min-len B` | 80 | 正文小于 B 字节的丢弃（滤掉纯数据条目） |
| `--max-len B` | 4 MB | 正文上限，**必须 ≤ 固件 `WIKI_MAX_ARTICLE`** |
| `--chunk-size N` | 250 | 每个 `articles_XXXX.dat` 放多少篇 |
| `--title-mode` | `skip` | 标题超 63 字节时：跳过 / 截断 |
| `--force` | 关 | 允许覆盖已有 `metadata.json` 的目录（危险） |

### 3.5 安全约束

- 目标目录已有 `metadata.json` 时**直接拒绝**，必须换新目录或显式 `--force`
- 只清理**自己产出的文件**（`index.bin` / `py*.bin` / `articles_*.dat`），不会 `rm -rf`
- 生成完成后删掉 `.build_state.json` 与中间索引

---

## 4. 标题长度策略（64 字节硬约束）

`index.bin` 每条 80 字节，标题字段 64 字节（最后 1 字节留给 `NUL`，实际最多 63 字节）。
C 层用 `memcmp` 在索引上二分，**排序键必须唯一**。

所以超长标题不能随便截断：截断会让两条不同词条变成同一个键，二分立刻错乱。

| 模式 | 行为 | 适用 |
|---|---|---|
| `skip`（默认） | 整条跳过，计入 `long_title` | 推荐。中文维基 >63 字节的标题极少（<0.01%） |
| `truncate` | 按 UTF-8 边界截到 63 字节 + 记账 | 标题普遍很长的小语种 |

无论哪种模式，排序后都会再做一次相邻同名检测（`dup_title` 计数），有重复只留第一条。

---

## 5. 输出的文件

| 文件 | 格式 | 说明 |
|---|---|---|
| `metadata.json` | JSON | `version` 必须是 2、`articleCount` 必须与 index 一致；另含来源/许可/统计/分位/校验备注 |
| `index.bin` | 80 B × N | 按标题 **UTF-8 字节序**升序 |
| `articles_%04d.dat` | 帧序列 | `'WK1\0'` + ulen(clen(crc 各 u32 LE) + LZ4 裸 block |
| `py.bin` / `pyi.bin` | 64 B × M | `key[60]` + `id u32`，按 key 升序 |
| `SHA256SUMS` | 文本 | 各文件 sha256，便于分发后核对 |
| `build.log` | 文本 | 逐阶段日志 + 跳过原因统计（不参与读取） |

⚠ `crc32` 字段**当前写入但不校验**（读取器只比对 ulen/clen）。想加校验属于行为变更，
需要单独评估——1.7 MB 文章多算一次 crc32 在 S3 上约 2–4 ms。

---

## 6. 体积与内存参考

实测（中文维基前 600 页真实数据，458 篇）：

| 指标 | 数值 |
|---|---|
| 正文（去标签后） | 10.2 MB |
| LZ4 压缩后 | 5.8 MB（**压缩率 0.56**） |
| 正文长度 p50 / p90 / p99 | 11 KB / 54 KB / 120 KB |
| 单篇最长（样本内） | 472 KB wikitext → 101 KB 纯文本 → 1815 行 |
| 读取峰值 PSRAM（最长篇） | **184 KB** |

按 0.56 的压缩率估算：**553 MB 数据库约对应 1 GB 纯文本、约 10–15 万篇中等长度词条**。

固件侧峰值内存 = `clen + ulen`（压缩块 + 解压结果），1 MB 文章约 1.8 MB 峰值，
远低于 8 MB PSRAM。超过可用 PSRAM 时会返回 `-8` 并打印"需多少 / 最多能给多少"。

---

## 7. 部署到设备

```bash
# 1) 生成（或拿到已生成目录）
# 2) 拷到 TF 卡：<卡>/wiki/<库名>/
#    目录名随意，固件会取 /sdcard/wiki 下第一个含 metadata.json 的子目录；
#    有多个库时用 app.wiki.open("/sdcard/wiki/<库名>") 指定
# 3) 校验
shasum -a 256 -c SHA256SUMS
```

TF 卡建议 exFAT 或 FAT32。553 MB 级数据库用 FAT32 单文件 4 GB 限制也够。

**数据库永远不放 LAPP、不放 Flash**：它和应用的安装目录完全分离，卸载应用不会碰它。

---

## 8. 更新数据

1. 下新 dump → 生成到**新目录**（别覆盖正在用的）
2. 校验通过后，把旧目录改名备份，新目录改成正式名
3. 设备上重开 Wiki 应用即可（`wiki_open` 每次重新读 metadata）

生成器不认识"增量更新"：每次都从 dump 重建。因为索引必须全局有序，
局部替换会让二分失效。

---

## 9. 测试

```bash
bash tests/wiki/run_all.sh
# 附带一个真实生产库验收：
REAL_DB=/tmp/wiki_real1 REAL_LONG_TITLE=中华人民共和国 bash tests/wiki/run_all.sh
```

覆盖：基础语义 52 项、真实 LZ4 + ASan 60 项、损坏检测 10 项、生产库验收 23 项。
