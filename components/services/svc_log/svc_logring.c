/* svc_logring.c — 崩溃前日志自留证（见 svc_logring.h）
 *
 * 实现要点：
 *   1. 接管 esp_log_set_vprintf：每行先抄进 RTC 环形缓冲，再交给原输出函数。
 *   2. 缓冲在 RTC_NOINIT 段 ⇒ panic / 看门狗 / 软复位后内容仍在。
 *   3. 双环：ALL 24 条 + WARN/ERROR 16 条，防止 INFO 刷屏把关键行冲掉。
 *   4. 极轻：固定栈缓冲 + vsnprintf，不 malloc、不加锁。
 *
 * ⚠ 这里在日志路径上，任何额外开销都会被放大到全部日志。改动请克制。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "svc_logring.h"

#define LR_ALL_N        24      /* 全量环条数 */
#define LR_WE_N         16      /* WARN/ERROR 环条数 */
#define LR_TEXT         92      /* 单条正文上限(含 NUL) */
#define LR_MAGIC        0x4C524C47u   /* 'LRLG' */
#define LR_LINE_BUF     192     /* 栈上格式化缓冲 */

typedef struct {
    uint32_t ms;                /* 写入时刻(开机毫秒) */
    uint8_t  lvl;               /* 0=INFO/其他 1=WARN 2=ERROR */
    char     text[LR_TEXT];
} lr_ent_t;

typedef struct {
    uint32_t magic;
    uint32_t all_head;
    uint32_t all_total;
    uint32_t we_head;
    uint32_t we_total;
    lr_ent_t all[LR_ALL_N];
    lr_ent_t we[LR_WE_N];
} lr_t;

/* RTC 保留内存: panic/看门狗/软复位不清零, 只有真掉电才丢 */
RTC_NOINIT_ATTR static lr_t s_lr;

/* 钩子状态变量：必须定义在 lr_record() 之前（lr_record 里读 s_suspend）。 */
static vprintf_like_t s_prev_vprintf = NULL;
static volatile bool  s_hooked = false;
static volatile bool  s_in_hook = false;   /* 防重入(日志库内部再打日志) */
static volatile bool  s_suspend = false;   /* dump 期间暂停记录, 否则会自我嵌套 */
static bool           s_ready = false;

/* 从一行已格式化的日志里判断级别。
 * IDF 默认格式是 "E (1234) TAG: ..."，带颜色时前面有 ESC 序列，这里跳过。 */
static int lr_classify(const char *s)
{
    if (!s) return 0;
    /* 跳过 ANSI 转义序列 "\x1b[...m" */
    while (*s == '\x1b') {
        s++;
        if (*s == '[') {
            s++;
            while (*s && !(*s >= '@' && *s <= '~')) s++;
            if (*s) s++;
        }
    }
    for (int i = 0; i < 12 && s[i]; i++) {
        if (s[i] == 'E' && s[i + 1] == ' ') return 2;
        if (s[i] == 'W' && s[i + 1] == ' ') return 1;
    }
    return 0;
}

/* 去掉行尾 CR/LF 的动作已经并入 lr_push（就地处理，不另开缓冲）。 */

static void lr_push(lr_ent_t *ring, int n, uint32_t *head, uint32_t *total,
                    int lvl, const char *text, uint32_t ms)
{
    lr_ent_t *e = &ring[*head % (uint32_t)n];
    e->ms  = ms;
    e->lvl = (uint8_t)lvl;
    strncpy(e->text, text, LR_TEXT - 1);
    e->text[LR_TEXT - 1] = '\0';
    /* 去掉行尾 CR/LF（就地处理，避免另开一份缓冲） */
    size_t tl = strlen(e->text);
    while (tl > 0 && (e->text[tl - 1] == '\n' || e->text[tl - 1] == '\r')) {
        e->text[--tl] = '\0';
    }
    *head = (*head + 1) % (uint32_t)n;
    (*total)++;
}

static void lr_record(const char *line)
{
    if (s_suspend) return;          /* dump 期间不记录, 否则把自己的输出记回去 */
    if (!s_lr.magic) {
        /* RTC_NOINIT 初值不可假定为 0；首次进入时老实初始化一遍 */
        memset(&s_lr, 0, sizeof(s_lr));
        s_lr.magic = LR_MAGIC;
    }
    if (s_lr.magic != LR_MAGIC) {           /* 上电残留垃圾 */
        memset(&s_lr, 0, sizeof(s_lr));
        s_lr.magic = LR_MAGIC;
    }

    int lvl = lr_classify(line);
    uint32_t ms = (uint32_t)esp_log_timestamp();

    lr_push(s_lr.all, LR_ALL_N, &s_lr.all_head, &s_lr.all_total, lvl, line, ms);
    if (lvl >= 1) {
        lr_push(s_lr.we, LR_WE_N, &s_lr.we_head, &s_lr.we_total, lvl, line, ms);
    }
}

/* ★ 2026-11-22 说明（一次试错，记录在此避免重走）：
 *   曾尝试"不接管 vprintf，改由 svc_log 把已格式化的行转发过来"，目的是省掉
 *   一次 vsnprintf。**实测失败**：svc_log 的内部缓冲 s_log_len 攒满 4KB 之后
 *   就不再格式化，转发随之停止 ⇒ 日志环变空 ⇒ 崩溃诊断能力直接丢失。
 *   所以恢复为**独立接管 esp_log_set_vprintf**：自备一份格式化，不依赖任何外部状态。
 *   ⚠ 代价是每行日志多一次 vsnprintf + 栈上 192B；已核实最小任务栈 12KB，可承受。
 * ⛔ 不要再改成"依赖 svc_log 转发"。 */

/* 兼容接口：保留符号，实际不再被 svc_log 调用。 */
void svc_logring_push(const char *line)
{
    if (!line || !line[0]) return;
    if (!s_ready) return;
    lr_record(line);
}

static int lr_vprintf(const char *fmt, va_list ap)
{
    if (s_ready && !s_in_hook) {
        s_in_hook = true;
        char buf[LR_LINE_BUF];
        va_list cp;
        va_copy(cp, ap);
        int n = vsnprintf(buf, sizeof(buf), fmt, cp);
        va_end(cp);
        if (n > 0) {
            buf[sizeof(buf) - 1] = '\0';    /* vsnprintf 截断时保证 NUL */
            lr_record(buf);
        }
        s_in_hook = false;
    }
    if (s_prev_vprintf) return s_prev_vprintf(fmt, ap);
    return vprintf(fmt, ap);
}

void svc_logring_init(void)
{
    if (s_hooked) return;
    if (!s_lr.magic || s_lr.magic != LR_MAGIC) {
        memset(&s_lr, 0, sizeof(s_lr));
        s_lr.magic = LR_MAGIC;
    }
    s_prev_vprintf = esp_log_set_vprintf(lr_vprintf);
    s_ready = true;
    s_hooked = true;
    ESP_LOGI("LOGRING", "日志自留证已启动 (RTC 环: 全量 %d 条 / 告警 %d 条)",
             LR_ALL_N, LR_WE_N);
}

/* 打印一个环。按写入顺序(旧→新)输出，这样时间上是顺的。
 * ⚠ 必须用 printf 而不是 ESP_LOGx：ESP_LOG 会再次经过本模块的日志钩子，
 *   把自己的输出又记回环里 ⇒ 自我嵌套、刷屏、内容变垃圾（本轮已踩过）。 */
static void lr_dump_ring(const char *name, const lr_ent_t *ring, int n,
                         uint32_t head, uint32_t total)
{
    uint32_t cnt = total < (uint32_t)n ? total : (uint32_t)n;
    if (cnt == 0) {
        printf("LOGRING --- %s: 空 ---\r\n", name);
        return;
    }
    printf("LOGRING --- %s: 共写入 %u 条, 显示最近 %u 条 ---\r\n",
           name, (unsigned)total, (unsigned)cnt);
    uint32_t start = (head + (uint32_t)n - cnt) % (uint32_t)n;
    for (uint32_t i = 0; i < cnt; i++) {
        const lr_ent_t *e = &ring[(start + i) % (uint32_t)n];
        printf("LOGRING   [%6u ms] %s\r\n", (unsigned)e->ms, e->text);
    }
}

void svc_logring_dump(const char *why)
{
    s_suspend = true;               /* 双保险: 万一 printf 也落到钩子上 */
    if (!s_lr.magic || s_lr.magic != LR_MAGIC) {
        printf("LOGRING 环形缓冲未初始化(本次开机后还没记过日志)\r\n");
        s_suspend = false;
        return;
    }
    printf("LOGRING ===== 崩溃前日志自留证 %s =====\r\n", why ? why : "");
    lr_dump_ring("ALL 最近日志", s_lr.all, LR_ALL_N, s_lr.all_head, s_lr.all_total);
    lr_dump_ring("WARN/ERROR",   s_lr.we,  LR_WE_N,  s_lr.we_head,  s_lr.we_total);
    printf("LOGRING ===== 自留证结束 =====\r\n");
    s_suspend = false;
}

size_t svc_logring_snapshot(char *out, size_t outlen)
{
    if (!out || outlen == 0) return 0;
    out[0] = '\0';
    if (!s_lr.magic || s_lr.magic != LR_MAGIC) return 0;
    size_t used = 0;
    uint32_t cnt = s_lr.we_total < LR_WE_N ? s_lr.we_total : LR_WE_N;
    uint32_t start = (s_lr.we_head + (uint32_t)LR_WE_N - cnt) % (uint32_t)LR_WE_N;
    for (uint32_t i = 0; i < cnt; i++) {
        const lr_ent_t *e = &s_lr.we[(start + i) % (uint32_t)LR_WE_N];
        int w = snprintf(out + used, outlen - used, "[%u]%s\n",
                         (unsigned)e->ms, e->text);
        if (w <= 0) break;
        if ((size_t)w >= outlen - used) { used = outlen - 1; break; }
        used += (size_t)w;
    }
    if (used < outlen) out[used] = '\0';
    return used;
}

void svc_logring_clear(void)
{
    memset(&s_lr, 0, sizeof(s_lr));
    s_lr.magic = LR_MAGIC;
}
