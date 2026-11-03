/* wp_module_lifeguide.c — 人生指南壁纸模块
 *
 * 把 Lua 应用《人生指南》的 676 条建议当作**屏保壁纸**显示：每分钟随机换一条。
 *
 * 数据来源：应用安装目录下的 lifeguide.lua，其中
 *   local RAW = [[  id\x1f章节\x1f等级\x1f优先级\x1f标题\x1f白话\x1f花费\x1f证据数 \n ... ]]
 *
 * 字段序（与 tools/gen_lifeguide_lua.py 的 build_data() 一一对应）：
 *   [0] id  [1] 章节名  [2] 等级 A/B/C  [3] 优先级 极高/高/一般
 *   [4] 标题  [5] 白话正文  [6] 花费  [7] 证据(文献)条数
 *   ★ 2026-10-31 修正：此前把 [2] 当"证据"、[3] 当"性价比"显示，语义是错的。
 *
 * ==== 2026-10-31 本轮修复 ----
 * 1. 路径：原来写死 /store/apps/app/，而商店默认把应用装到 TF 卡 → 壁纸项根本不出现。
 *    改为多路径探测（TF 卡优先，内置/历史目录兜底）。
 * 2. 字段错位：见上。
 * 3. 「原地切字段」的致命 bug：旧实现把缓冲区里的 \x1f 直接改成 \0，
 *    于是**同一条建议第二次显示时 8 个字段全是空串** → fn<6 提前 return →
 *    屏幕停在上一帧不动。表现就是"壁纸有时候不换内容"。
 *    现在改为只算 [起点,长度] 区间，绝不写缓冲区。
 * 4. PSRAM 生命周期：新增 preload/release（外部安全上下文调用），
 *    渲染函数**不再自己 fopen** —— 屏保任务栈在 PSRAM，碰内部 flash 会 panic。
 *
 * 与星空不同，这是**静态壁纸**：没到 60 秒就返回 0（不重绘、不 flush），
 * 避免每帧刷 15000B 白耗电。
 */
#include "wallpapers.h"
#include "ui_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"      /* ★ 2026-10-31 并发安全: 临界区 + portMUX */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LG_TAG        "WPLG"
#define LG_SEP        '\x1f'
#define LG_SWAP_MS    60000u        /* 每条停留 1 分钟 */

#define LG_W  ST7305_WIDTH
#define LG_H  ST7305_HEIGHT
#define LG_PAD 18
#define LG_LH  22

/* 候选安装路径 —— 顺序即优先级。
 *
 * ★★★ 2026-10-31 关键修正（两次都错了，这次依据源码定死）★★★
 *   ① 原写 /store/apps/app/lifeguide.lua —— 目录对，**文件名错**。
 *      商店安装路径 = AM_STORE_DST/pcat/nm（am_store.c:352），
 *      其中 nm = **包文件名**，而包文件名是显示名 `人生指南.lua`（store/pkgs/ 下就是这个名字）。
 *      `lifeguide.lua` 只是桌面源码的文件名，设备上不存在 ⇒ 壁纸从来没有命中过。
 *   ② 上一轮误以为"商店默认装 TF 卡"（依据 am_pkg_prefer 返回 1）。
 *      实际 am_store.c:354-355 是 `s_pkg[k].pref = 0; (void)am_pkg_prefer;`
 *      —— 该函数是**死代码**，当前一律装内置 /store/apps。
 *      所以内置路径排第一，TF 路径保留（pref 改回 1 或用户手工放卡时仍能用）。
 */
static const char *const LG_PATHS[] = {
    "/store/apps/app/\xE4\xBA\xBA\xE7\x94\x9F\xE6\x8C\x87\xE5\x8D\x97.lua",    /* 人生指南.lua ← 商店实际安装位置 */
    "/sdcard/apps/app/\xE4\xBA\xBA\xE7\x94\x9F\xE6\x8C\x87\xE5\x8D\x97.lua",   /* 同名，TF 卡 */
    "/store/apps/app/lifeguide.lua",      /* 英文名（手工拷贝 / 早期版本）*/
    "/sdcard/apps/app/lifeguide.lua",
};
#define LG_PATH_N ((int)(sizeof(LG_PATHS) / sizeof(LG_PATHS[0])))

static char     *s_base;            /* malloc 得到的整块（s_data 指向它内部，必须单独记）*/
static char     *s_data;            /* 数据段起点（RAW 之后）*/
static uint32_t *s_off;             /* 每条起始偏移 */
static int       s_n;               /* 条数 */
static int       s_cur = -1;        /* 当前显示第几条（-1=未选）*/
static uint32_t  s_last_ms;         /* 上次换条时刻 */
static char      s_path[64];        /* 命中的文件路径（空=没找到）*/

/* ==== 2026-10-31 并发安全（见文件末尾 release/reap 的说明）====
 * 屏保监督任务（栈在 PSRAM）与 UI 任务/卸载任务会同时碰这份数据：
 *   · render 只可能在屏保监督任务里跑；
 *   · release 只可能在 UI 任务或卸载任务里跑（用户操作）。
 * 只用 s_n=0 不能证明安全：那只是"计数清了"，指针还可能正被 render 用着。
 * 这里用两个标志 + 一个极短临界区，把"判可用"和"占用"做成原子。 */
static volatile bool s_pending;     /* 已登记"要释放"，等 supervisor 收尾 */
static volatile bool s_busy;        /* render 正在使用 s_base/s_off */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---------------- UTF-8 宽度 ---------------- */

static int u8len(unsigned char c)
{
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

/* 定长串的像素宽（s_off 里的行没有 \0 结尾，不能直接 strlen） */
static int strwn(const char *s, int n)
{
    int w = 0, i = 0;
    while (i < n) {
        int l = u8len((unsigned char)s[i]);
        if (i + l > n) l = n - i;
        w += (l >= 3) ? 16 : 8;
        i += l;
    }
    return w;
}

static void txt(st7305_handle_t *lcd, int x, int y, const char *s)
{
    if (!s || !*s) return;
    if (ui_text16_ready()) ui_text16(lcd, x, y, s, false);
    else                   draw_text(lcd, x, y, s, false);
}

/* 定长串画一段（拷进临时 buf 补 \0；行数据本身没有终止符） */
static void txtn(st7305_handle_t *lcd, int x, int y, const char *p, int n)
{
    char buf[96];
    if (!p || n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    memcpy(buf, p, (size_t)n);
    buf[n] = 0;
    txt(lcd, x, y, buf);
}

/* 点阵渐隐线（视觉铁律：中段实线，两端隔点） */
static void hline_fade(st7305_handle_t *lcd, int x0, int x1, int y)
{
    int span = x1 - x0;
    int a = x0 + span / 4, b = x1 - span / 4;
    for (int x = a; x <= b; x++) st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
    for (int x = x0; x < a; x += 4) {
        st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, x + 1, y, ST7305_COLOR_BLACK);
    }
    for (int x = b; x <= x1; x += 4) {
        st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
        if (x + 1 <= x1) st7305_draw_pixel(lcd, x + 1, y, ST7305_COLOR_BLACK);
    }
}

/* 定长串按像素宽折行并逐行画；返回画了几行 */
static int draw_wrapn(st7305_handle_t *lcd, int x, int y, const char *p, int n,
                      int maxw, int maxrows)
{
    int rows = 0, i = 0;
    while (i < n && rows < maxrows) {
        char buf[96];
        int m = 0, w = 0;
        while (i < n) {
            int l = u8len((unsigned char)p[i]);
            if (i + l > n) l = n - i;
            int cw = (l >= 3) ? 16 : 8;
            if (w + cw > maxw) break;
            if (m + l >= (int)sizeof(buf)) break;
            memcpy(buf + m, p + i, (size_t)l);
            m += l; w += cw; i += l;
        }
        if (m == 0) break;
        buf[m] = 0;
        txt(lcd, x, y, buf);
        y += LG_LH; rows++;
    }
    return rows;
}

/* 数一段文字能折几行（不画，只算；居中位置要用它） */
static int wraprows(const char *p, int n, int maxw)
{
    int rows = 0, i = 0;
    while (i < n) {
        int w = 0;
        while (i < n) {
            int l = u8len((unsigned char)p[i]);
            if (i + l > n) l = n - i;
            int cw = (l >= 3) ? 16 : 8;
            if (w + cw > maxw) break;
            w += cw; i += l;
        }
        if (w == 0) break;          /* 单字符就超宽（不该发生）→ 防死循环 */
        rows++;
    }
    return rows;
}

/* ---------------- 数据加载 / 释放 ---------------- */

/* 探测当前可用路径；返回命中的完整路径（static s_path），找不到返回 NULL */
static const char *lg_probe(void)
{
    for (int i = 0; i < LG_PATH_N; i++) {
        FILE *f = fopen(LG_PATHS[i], "rb");
        if (f) {
            fclose(f);
            snprintf(s_path, sizeof(s_path), "%s", LG_PATHS[i]);
            return s_path;
        }
    }
    s_path[0] = 0;
    return NULL;
}

/* ==== 释放：两段式（登记 → 收尾）====
 *
 * 为什么不能"调一次就直接 free"：
 *   数据的使用者是**屏保监督任务**（ss_supervisor_task，栈在 PSRAM）。
 *   调用释放的是 UI 任务 / 卸载任务。两者可能重叠在：
 *     supervisor 已通过 `s_n > 0` 判断、正在 draw_item 里读 s_off/s_data，
 *     而 UI 侧同时 free 掉这两个指针 ⇒ use-after-free。
 *   （用户从"唤醒屏保"到"进设置点切换"要好几秒，而 render 只有几 ms，
 *     实际几乎不可能撞上；但"几乎不可能"不等于安全，这里按可靠性要求处理。）
 *
 * 做法：
 *   ① `wp_lifeguide_release()` 只置 s_pending —— 不碰指针，谁调都安全；
 *   ② 真正的 free 只在 `wp_lifeguide_reap()` 里做，且由**屏保监督任务在非屏保分支**
 *      调用 —— 那一刻 render 确定不在跑，是唯一能确认"没人再用"的时机；
 *   ③ await 期间 render 看到 s_pending 立即改画星空，不会继续读旧数据。
 *
 * 代价：PSRAM 最长延迟 250ms（supervisor 空闲轮询周期）才归还。可接受。
 * 上下文：reap 在 PSRAM 栈的任务里执行 —— heap_caps_free 只操作堆结构（内部 RAM），
 *        不碰 flash，因此安全（与 fopen/NVS 完全不同）。
 */
void wp_lifeguide_release(void)
{
    s_pending = true;      /* 单字节状态写，天然原子；不做任何指针操作 */
    ESP_LOGI(LG_TAG, "人生指南壁纸: 已登记释放（等屏保任务收尾）");
}

void wp_lifeguide_reap(void)
{
    void *vb = NULL, *vo = NULL;

    taskENTER_CRITICAL(&s_mux);
    /* s_busy = render 正在用；s_n = 0 表示没东西可释放 */
    if (!s_pending || s_busy || s_n == 0) {
        taskEXIT_CRITICAL(&s_mux);
        return;
    }
    /* 临界区内只"摘指针"，free 留到锁外（堆操作不在关中断区间里做）*/
    vb = s_base;  vo = s_off;
    s_base = NULL; s_data = NULL; s_off = NULL;
    s_n = 0; s_cur = -1; s_pending = false;
    taskEXIT_CRITICAL(&s_mux);

    if (vo) heap_caps_free(vo);
    if (vb) heap_caps_free(vb);
    ESP_LOGI(LG_TAG, "人生指南壁纸: 数据已释放（屏保任务收尾）");
}

/* ★ 必须在内部 RAM 栈的上下文调用（UI 任务 / 开机任务）。
 *   TF 卡(SDMMC) 本身安全，但 /store 是 SPIFFS（内部 flash），
 *   在 PSRAM 栈上触发 flash 操作会禁用缓存 → panic。所以这里统一放到安全上下文。 */
bool wp_lifeguide_preload(void)
{
    /* 已有数据：直接复用，并**撤销**可能挂起的释放请求
     * （用户"切星空 → 又切回人生指南"时，release 已登记但还没收尾）*/
    taskENTER_CRITICAL(&s_mux);
    if (s_n > 0) { s_pending = false; taskEXIT_CRITICAL(&s_mux); return true; }
    s_pending = false;                        /* 本次要重新分配 ⇒ 旧释放请求作废 */
    taskEXIT_CRITICAL(&s_mux);

    const char *path = lg_probe();
    if (!path) { ESP_LOGW(LG_TAG, "未找到 lifeguide.lua（4 条候选路径都不存在）"); return false; }

    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(LG_TAG, "打不开 %s", path); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); ESP_LOGW(LG_TAG, "文件大小异常 %ld", sz); return false; }

    char *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); ESP_LOGW(LG_TAG, "PSRAM 分配失败 (%ld B)", sz); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    char *p = strstr(buf, "local RAW = [[");
    if (!p) { ESP_LOGW(LG_TAG, "数据段标记未找到"); heap_caps_free(buf); return false; }
    p += strlen("local RAW = [[");
    char *e = strstr(p, "]]");
    int   len = e ? (int)(e - p) : (int)strlen(p);

    /* 先数条数，再按真实条数分配偏移表（旧实现按 1200 上限分配，多占 2KB）*/
    int cnt = 1;
    for (int i = 0; i < len; i++) if (p[i] == '\n' && i + 1 < len) cnt++;

    uint32_t *off = heap_caps_malloc(sizeof(uint32_t) * (size_t)cnt, MALLOC_CAP_SPIRAM);
    if (!off) { heap_caps_free(buf); ESP_LOGW(LG_TAG, "偏移表分配失败"); return false; }

    int k = 0;
    off[k++] = 0;
    for (int i = 0; i < len && k < cnt; i++)
        if (p[i] == '\n' && i + 1 < len) off[k++] = (uint32_t)(i + 1);

    /* 末尾补 \0：让最后一行的长度计算收敛（buf 是本地变量，锁外写安全）*/
    p[len] = 0;

    /* 发布：与 reap 互斥，保证"刚分配完"不会被并发的释放清掉 */
    taskENTER_CRITICAL(&s_mux);
    s_base = buf;
    s_data = p;
    s_off  = off;
    s_n    = k;
    s_cur  = -1;
    s_pending = false;
    taskEXIT_CRITICAL(&s_mux);

    ESP_LOGI(LG_TAG, "人生指南壁纸: %d 条已载入 (%s, %ld KB)", s_n, path, sz / 1024);
    return s_n > 0;
}

/* ---------------- 一条建议的分字段与绘制 ---------------- */

/* 字段区间（只算 [起点,长度]，**不写缓冲区**）。返回字段数（最多 maxf）。 */
static int lg_split(const char *line, int n, int *fs, int *fl, int maxf)
{
    int cnt = 0, st = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || line[i] == LG_SEP) {
            if (cnt < maxf) { fs[cnt] = st; fl[cnt] = i - st; cnt++; }
            else break;
            st = i + 1;
        }
    }
    return cnt;
}

/* 第 idx 条的行长度（不含行尾 \n） */
static int lg_linelen(int idx)
{
    if (idx + 1 < s_n) return (int)(s_off[idx + 1] - s_off[idx]) - 1;
    const char *p = s_data + s_off[idx];
    int n = 0;
    while (p[n]) n++;
    return n;
}

#define LG_MAXF 8
static void draw_item(st7305_handle_t *lcd, int idx)
{
    if (idx < 0 || idx >= s_n) return;

    const char *line = s_data + s_off[idx];
    int         n    = lg_linelen(idx);
    if (n <= 0) return;

    int fs[LG_MAXF], fl[LG_MAXF];
    int fn = lg_split(line, n, fs, fl, LG_MAXF);
    if (fn < 6) return;                       /* 至少要有 章节..白话 六个字段 */

    const char *sec   = line + fs[1]; int secl = fl[1];
    const char *grade = line + fs[2]; int grl  = fl[2];   /* 等级 A/B/C */
    const char *ratio = line + fs[3]; int ral  = fl[3];   /* 优先级 极高/高/一般 */
    const char *title = line + fs[4]; int til  = fl[4];
    const char *body  = line + fs[5]; int bol  = fl[5];
    const char *cost  = (fn > 6) ? line + fs[6] : NULL; int col = (fn > 6) ? fl[6] : 0;
    const char *srcn  = (fn > 7) ? line + fs[7] : NULL; int sl  = (fn > 7) ? fl[7] : 0;

    st7305_clear(lcd, ST7305_COLOR_WHITE);

    const int inner = LG_W - LG_PAD * 2;

    /* 行数都要**先算**，否则居中位置会算错 */
    int trows = wraprows(title, til, inner); if (trows > 3) trows = 3;
    int brows = wraprows(body,  bol, inner); if (brows > 3) brows = 3;
    int has_cost = (col > 0);
    int has_meta = (grl > 0 || ral > 0 || sl > 0);

    int total = LG_LH                                     /* 章节 */
              + 8 + trows * LG_LH                         /* 标题 */
              + 10 + 8                                    /* 渐隐线 + 间距 */
              + brows * LG_LH                             /* 白话 */
              + (has_cost ? 6 + LG_LH : 0)
              + (has_meta ? 12 + LG_LH : 0);

    int y = (LG_H - total) / 2;
    if (y < LG_LH) y = LG_LH;

    /* 章节名（居中） */
    if (secl > 0) {
        int w = strwn(sec, secl);
        txtn(lcd, (LG_W - w) / 2, y, sec, secl);
    }
    y += LG_LH + 8;

    /* 标题（居中，最多 3 行） */
    {
        int i = 0, rows = 0;
        while (i < til && rows < 3) {
            char buf[96];
            int m = 0, w = 0;
            while (i < til) {
                int l = u8len((unsigned char)title[i]);
                if (i + l > til) l = til - i;
                int cw = (l >= 3) ? 16 : 8;
                if (w + cw > inner) break;
                if (m + l >= (int)sizeof(buf)) break;
                memcpy(buf + m, title + i, (size_t)l);
                m += l; w += cw; i += l;
            }
            if (m == 0) break;
            buf[m] = 0;
            txt(lcd, (LG_W - w) / 2, y, buf);
            y += LG_LH; rows++;
        }
    }
    y += 10;

    hline_fade(lcd, LG_W / 2 - 40, LG_W / 2 + 40, y);
    y += 8;

    /* 白话正文（左对齐，最多 3 行） */
    draw_wrapn(lcd, LG_PAD, y, body, bol, inner, 3);
    y += brows * LG_LH;

    /* 花费（一行，前缀"花费"，超宽按像素截断） */
    if (has_cost) {
        y += 6;
        const char *pfx = "花费 ";
        int pw = strwn(pfx, (int)strlen(pfx));
        txt(lcd, LG_PAD, y, pfx);
        char buf[80];
        int i = 0, m = 0, w = 0, room = inner - pw;
        while (i < col) {
            int l = u8len((unsigned char)cost[i]);
            if (i + l > col) l = col - i;
            int cw = (l >= 3) ? 16 : 8;
            if (w + cw > room) break;
            if (m + l >= (int)sizeof(buf) - 1) break;
            memcpy(buf + m, cost + i, (size_t)l);
            m += l; w += cw; i += l;
        }
        buf[m] = 0;
        txt(lcd, LG_PAD + pw, y, buf);
        y += LG_LH;
    }

    /* 底部：等级 / 优先级 / 证据条数 —— 用真实语义，
     * 不再把等级 A 写成"证据A"、把优先级"极高"写成"性价比极高"。 */
    if (has_meta) {
        y += 12;
        char meta[64];
        int  m = 0;
        if (grl > 0) {
            char g[8];
            int gl = (grl < (int)sizeof(g) - 1) ? grl : (int)sizeof(g) - 1;
            memcpy(g, grade, (size_t)gl); g[gl] = 0;
            m += snprintf(meta + m, sizeof(meta) - (size_t)m, "%s 级", g);
        }
        if (ral > 0) {
            char r[16];
            int rl2 = (ral < (int)sizeof(r) - 1) ? ral : (int)sizeof(r) - 1;
            memcpy(r, ratio, (size_t)rl2); r[rl2] = 0;
            m += snprintf(meta + m, sizeof(meta) - (size_t)m, "%s优先级 %s", m ? " · " : "", r);
        }
        if (sl > 0) {
            char s2[8];
            int sl2 = (sl < (int)sizeof(s2) - 1) ? sl : (int)sizeof(s2) - 1;
            memcpy(s2, srcn, (size_t)sl2); s2[sl2] = 0;
            m += snprintf(meta + m, sizeof(meta) - (size_t)m, "%s证据 %s 条", m ? " · " : "", s2);
        }
        if (m > 0) {
            int w = strwn(meta, (int)strlen(meta));
            txt(lcd, (LG_W - w) / 2, y, meta);
        }
    }
}

/* ---------------- 模块入口 ---------------- */

static int render_lifeguide(st7305_handle_t *lcd, uint32_t now_ms)
{
    /* ★ 并发安全（2026-10-31）：**"判可用 + 占用"必须原子**。
     *   若拆成两步（先判 s_n>0，再置 s_busy），中间一旦被 reap 插进来把指针摘走，
     *   下面 draw_item 就会读已释放内存 —— 这是唯一真正的 UAF 入口，这里堵死。
     *   临界区里只有几条赋值，不调用任何函数、不碰 flash。 */
    taskENTER_CRITICAL(&s_mux);
    if (s_n == 0 || s_pending) {
        /* 数据没预加载（**绝不在这里 fopen** —— 本函数跑在 PSRAM 栈上，碰 flash 会 panic），
         * 或已登记释放：都直接退星空，保证屏保永远有画面。 */
        taskEXIT_CRITICAL(&s_mux);
        static bool warned = false;
        if (!warned && s_n == 0) {
            warned = true;
            ESP_LOGW(LG_TAG, "数据未预加载 ⇒ 本帧退星空");
        }
        return wp_program_render(lcd, WP_PROG_STARS, now_ms);
    }
    s_busy = true;          /* 占用：此后 reap 不会释放这两个指针 */
    taskEXIT_CRITICAL(&s_mux);

    int ret = 0;
    do {
        /* 未到 60 秒：什么都不画（不重绘、不 flush，省电） */
        if (s_cur >= 0 && (uint32_t)(now_ms - s_last_ms) < LG_SWAP_MS) break;

        s_last_ms = now_ms;
        int i = (s_n > 1) ? (rand() % s_n) : 0;
        if (s_n > 1 && i == s_cur) i = (i + 1) % s_n;
        s_cur = i;

        draw_item(lcd, i);
        ret = 1;
    } while (0);

    s_busy = false;         /* 释放占用 */
    return ret;
}

/* 退出屏保/换壁纸时清掉"当前条"，下次进入重新随机 */
static void lg_reset(void) { s_cur = -1; }

static bool s_reg;
void wp_lifeguide_register_module(void)
{
    if (s_reg) return;
    s_reg = true;
    static const wp_module_t mod = {
        .id      = WP_PROG_LIFEGUIDE,
        .name    = "\xe4\xba\xba\xe7\x94\x9f\xe6\x8c\x87\xe5\x8d\x97",   /* 人生指南 */
        .render  = render_lifeguide,
        /* ★ 静态壁纸：系统据此固定 80MHz + 浅睡 + 1000ms 轮询，
         *   不再因"换条那一帧 drew=1"升到 240MHz（每分钟两次全机调频）。 */
        .idle_ms = 1000u,
    };
    wp_register_module(&mod);
    lg_reset();
}

/* 应用是否已安装 —— 壁纸列表据此决定是否显示这一项。
 * 多路径探测（旧实现只认 /store/apps/app，装在 TF 卡时永远返回 false）。 */
bool wp_lifeguide_installed(void)
{
    return lg_probe() != NULL;
}
