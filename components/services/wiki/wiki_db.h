/* wiki_db.h — 离线维基数据服务 (TF 卡 /sdcard/wiki/<库名>/)
 *
 * 职责边界（分层铁律：services 不依赖 ui / apps）：
 *   本模块只做「定位 → 读块 → 解压 → 抽纯文本 → 按 16px 宽度折行」，
 *   一个像素都不画。Lua 应用拿行号逐行 app.mix 绘制。
 *
 * 内存策略：
 *   * 索引不常驻：index.bin 每条 80 字节，二分靠 lseek 逐条读（18 次 × 80B）
 *   * 单篇整篇进 PSRAM（最大 1.76MB 实测），解压 → 原地剥 HTML → 原地折行
 *   * 不用内部 RAM 存正文；内部 RAM 只占几百字节的句柄状态
 *
 * 数据格式见 docs/format-spec.md（生成工具 nomad-wiki/tools/build_wiki_db.py）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIKI_TITLE_MAX 64          /* index.bin 里标题字段宽度 */
#define WIKI_MAX_ARTICLE (4u * 1024 * 1024)   /* 单篇解压上限，超了拒载 */

typedef struct {
    int  id;
    char title[WIKI_TITLE_MAX];
} wiki_hit_t;

/* 打开库。dir 传 NULL 或 "" 时自动取 /sdcard/wiki 下第一个含 metadata.json 的子目录。
 * 返回文章数，失败返回 0（并在 log 里说明原因）。 */
int  wiki_open(const char *dir);
void wiki_close(void);
bool wiki_is_open(void);

int  wiki_count(void);
const char *wiki_name(void);        /* 库目录名，给界面标题用 */

/* 标题前缀搜索（按索引顺序返回），返回实际写入 out 的条数 */
int  wiki_search(const char *q, wiki_hit_t *out, int max);

bool wiki_title(int id, char *buf, int cap);

/* 顺序取 n 条标题（浏览列表用）：一次开文件读完，
 * 比循环调 wiki_title 少开 n-1 次文件（FATFS 每次 open 都要走目录）。 */
int  wiki_titles(int from, int n, wiki_hit_t *out);
/* 拼音搜索。initials=1 查首字母（bj），0 查全拼（beijing）。
 * 库目录里没有 py.bin / pyi.bin 时返回 0（不报错，调用方退化成标题搜索）。 */
int  wiki_search_py(const char *q, int initials, wiki_hit_t *out, int max);


/* 载入第 id 篇并排版。成功返回行数；失败返回负错误码：
 *   -1 未打开  -2 id 越界  -3 该条构建失败  -4 块文件打不开
 *   -5 帧头坏    -6 输入/输出溢出      -7 解压失败   -8 PSRAM 不足 */
int  wiki_load(int id);

int  wiki_lines(void);
/* 取第 i 行。返回指针 + 长度（**不是** NUL 结尾的 C 字符串，用 lua_pushlstring）。
 * 越界返回 "" 且 *len = 0。 */
const char *wiki_line(int i, int *len);

#ifdef __cplusplus
}
#endif
