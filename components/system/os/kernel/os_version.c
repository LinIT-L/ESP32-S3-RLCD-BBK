/**
 * os_version.c — 固件版本信息取用接口（唯一来源见 include/lintos_version.h）
 *
 * 版本号是**编译期常量**（改版本只改 lintos_version.h）；
 * 构建信息（git 描述 / 迭代号 / 构建时刻）由 CMake 配置期注入，取不到时退化。
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "lintos_version.h"

#ifndef LINTOS_GIT_DESC
#define LINTOS_GIT_DESC "unknown"
#endif

#ifndef LINTOS_GIT_ITER
#define LINTOS_GIT_ITER "0"
#endif

#ifndef LINTOS_BUILD_STAMP
/* ★★★ 2026-11-21 linit【固件时间默认 = 固件更新时间】★★★
 *   构建戳 = **编译本文件的时刻**（__DATE__/__TIME__），不再用 CMake configure 时间
 *   （那个只在配置阶段更新，增量编译时会落后好几天，实测偏差一天多）。
 *   build.sh 每次构建前会 `touch` 本文件，保证它等于本次镜像的生成时间。 */
#define LINTOS_BUILD_STAMP __DATE__ " " __TIME__
#endif

/* 内部完整版本 "1.3.1+12": 三段 + git 迭代号。
 * 用静态缓冲缓存 —— 本函数可能被渲染路径反复调用，不做重复 snprintf。 */
static char s_full[24];

/* 构建戳的解析结果（只解析一次；同时供 os_build_stamp / os_build_epoch 用） */
typedef struct { int y, mo, d, h, mi, s; bool ok; } build_time_t;
static build_time_t s_bt;
static bool         s_bt_done = false;
static char         s_bt_iso[24];

static void build_stamp_parse(void)
{
    if (s_bt_done) return;
    s_bt_done = true;

    /* ① ISO 形式（手工 -DLINTOS_BUILD_STAMP="2026-11-21 20:55" 时） */
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, se = 0;
    if (sscanf(LINTOS_BUILD_STAMP, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &se) == 6 ||
        sscanf(LINTOS_BUILD_STAMP, "%d-%d-%d %d:%d",    &Y, &M, &D, &h, &mi)    == 5) {
        s_bt = (build_time_t){ Y, M, D, h, mi, se, true };
    } else {
        /* ② __DATE__ 形式: "Nov 21 2026 20:55:12"（日用空格填充） */
        static const char *const k_mon[12] = {
            "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
        };
        char m3[4] = {0};
        int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
        if (sscanf(LINTOS_BUILD_STAMP, "%3s %d %d %d:%d:%d", m3, &d, &y, &hh, &mm, &ss) == 6) {
            for (int i = 0; i < 12; i++) {
                if (m3[0] == k_mon[i][0] && m3[1] == k_mon[i][1] && m3[2] == k_mon[i][2]) {
                    s_bt = (build_time_t){ y, i + 1, d, hh, mm, ss, true };
                    break;
                }
            }
        }
    }

    if (s_bt.ok) {
        snprintf(s_bt_iso, sizeof(s_bt_iso), "%04d-%02d-%02d %02d:%02d:%02d",
                 s_bt.y, s_bt.mo, s_bt.d, s_bt.h, s_bt.mi, s_bt.s);
    } else {
        snprintf(s_bt_iso, sizeof(s_bt_iso), "%s", LINTOS_BUILD_STAMP);   /* 原样兜底 */
    }
}

const char *os_version_public(void) { return LINTOS_VERSION_PUBLIC; }
const char *os_version_str(void)    { return LINTOS_VERSION_STR;    }
const char *os_version_iter(void)   { return LINTOS_GIT_ITER;       }
const char *os_version_date(void)   { return LINTOS_VERSION_DATE;   }
const char *os_version_name(void)   { return LINTOS_VERSION_NAME;   }
const char *os_git_desc(void)       { return LINTOS_GIT_DESC;       }
const char *os_build_stamp(void)    { build_stamp_parse(); return s_bt_iso; }

const char *os_version_full(void)
{
    if (!s_full[0]) snprintf(s_full, sizeof(s_full), "%s+%s", LINTOS_VERSION_STR, LINTOS_GIT_ITER);
    return s_full;
}

/* ==================== 构建时刻 → UTC epoch ====================
 * ★★★ 2026-11-21 linit 定规【固件时间默认 = 固件更新时间】★★★
 *   用途: RTC/内部时钟都没有可信时间时, 用"这个固件是什么时候做出来的"当默认时间;
 *        同时作为"RTC 落后于镜像 ⇒ 芯片时间不可信"的判据基准。
 *
 * 格式: CMake 注入的是 "YYYY-MM-DD HH:MM" (本机本地时间 = 北京时间 UTC+8);
 *       没走 CMake 时兜底成 __DATE__ " " __TIME__ = "Nov 21 2026 20:52:28"。
 *       ⇒ 两种都认, 解析失败返回 0 (调用方必须跳过, 绝不拿 0 当时间)。
 *
 * ⚠ 这里刻意不用 mktime: 它依赖进程 TZ, 而本函数可能在 svc_time_init 设时区之前被调。
 *   纯算术 → 结果与运行环境无关。 */
static int64_t fw_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t os_build_epoch(void)
{
    static int64_t s_epoch = -1;          /* -1 = 尚未计算 */
    if (s_epoch >= 0) return s_epoch;
    s_epoch = 0;

    build_stamp_parse();                  /* 与 os_build_stamp() 共用同一次解析 */
    const build_time_t *b = &s_bt;

    /* 合法性: 2020~2099 之间才算数, 其余一律当没解析出来 (返回 0) */
    if (!b->ok || b->y < 2020 || b->y > 2099 || b->mo < 1 || b->mo > 12 ||
        b->d < 1 || b->d > 31 || b->h < 0 || b->h > 23 ||
        b->mi < 0 || b->mi > 59 || b->s < 0 || b->s > 60) {
        return s_epoch;                   /* 0 */
    }

    /* 构建戳按北京时间记录 ⇒ 减 8 小时得到 UTC */
    s_epoch = fw_days_from_civil(b->y, (unsigned)b->mo, (unsigned)b->d) * 86400
            + (int64_t)b->h * 3600 + (int64_t)b->mi * 60 + b->s
            - 8 * 3600;
    if (s_epoch <= 0) s_epoch = 0;
    return s_epoch;
}
