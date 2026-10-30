/* ============================================================
 * net_client — LinTOS **唯一** HTTP(S) 客户端 (2026-10-15 linit 定规)
 *
 * 为什么要有它
 * ------------
 * 原来全工程有**两套**独立的 HTTP 实现, 各自建任务、各自起一套 TLS:
 *   ① Lua 桥 `luat_lib_net.c`   —— app.net.fetch_start/fetch_poll (航班雷达/地图包/定位)
 *   ② 应用商店 `am_store_net.c` —— 拉 index.json + 下载安装包
 * 代价: 同一个坑要修两遍 (今天 `ESP_FAIL` 初值那个 bug 就只打中 Lua 那套);
 *   两边同时跑还会**双份占内部 RAM** (各 8KB 内部栈 + 两套 TLS 上下文, 内部堆最紧时互相拖死)。
 *
 * 设计
 * ----
 *   ★ **单请求槽**: 一次只跑一个请求 (`netc_start` 在跑时返回 false = 忙) ⇒ 天然串行,
 *     调用方排队即可, 不需要各自加锁;
 *   ★ 任务栈**内部 RAM** (TLS 握手会短暂关中断/禁 cache, PSRAM 栈会炸);
 *   ★ 落盘搬运缓冲也用**内部 RAM**: 写 SPIFFS 时 flash 驱动会关 Flash+PSRAM cache,
 *     这个窗口里读 PSRAM 源缓冲会取到**垃圾** (SD 卡没这问题, 统一走内部缓冲最省心);
 *   ★ 响应体可落**内存**(小) 或**边收边写文件**(大: 省地图包 18MB / 商店安装包);
 *   ★ HTTP 状态码 + Content-Length 双校验: 4xx/5xx 与"被截断的短包"一律算失败,
 *     并且**失败时删掉半截文件** —— 绝不能让上层拿到假数据。
 *
 * 用法 (谁要联网都这么用)
 * -----------------------
 *   netc_req_t r = {0};
 *   r.url = url; r.timeout_ms = 8000;
 *   r.body = buf; r.body_cap = sizeof(buf);        // 或 r.sink_path = "/sdcard/x.bin";
 *   if (!netc_start(&r)) { 忙 / 参数错 }
 *   然后轮询 netc_state(): NETC_RUN → NETC_DONE / NETC_ERR, 取 netc_http()/netc_len()/netc_got(),
 *   读完调 netc_reset() 回到 IDLE。
 * ============================================================ */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETC_IDLE = 0,   /* 没有请求 */
    NETC_RUN,        /* 正在跑 */
    NETC_DONE,       /* 成功 (结果可取; 取走后必须 netc_reset) */
    NETC_ERR,        /* 失败 */
} netc_state_t;

typedef struct {
    const char *url;          /* 必填, http:// 或 https:// */
    int         timeout_ms;   /* 0 → 默认 10000 */
    /* —— 落点二选一 —— */
    char       *body;         /* 内存落点 (客户端会补 '\0') */
    int         body_cap;     /* body 容量 (字节, 含结尾 NUL 位) */
    const char *sink_path;    /* 文件落点: 边收边写 (大文件用) */
    /* —— 可选 —— */
    const char *user_agent;   /* NULL/空 → 默认 UA (空 UA 易被 CDN 判 403) */
    bool        no_redirect;  /* ⚠ 语义是"**禁止**跳转": 零初始化的 req 默认**跟随** 3xx。
                               *   (原字段叫 redirect 太容易踩: memset 0 就变成不跟随了) */
    bool        want_crc;     /* true → 收流时顺带算 crc32 (下载包完整性校验用) */
    /* —— 可选: 完整性闸门 (2026-10-30) ——
     *   非 0 时, 收流结束后**先校验再进入替换阶段**; 不匹配则整次下载判失败,
     *   `.part` 被删除, 目标文件(含旧版本)保持原样。
     *   语义: 0 = 不校验该项 (向后兼容, 老调用方 memset 0 行为不变)。 */
    uint32_t    expect_crc;   /* 期望 crc32; 0 = 不校验 */
    uint32_t    expect_size;  /* 期望字节数; 0 = 不校验 */
} netc_req_t;

/* 发起一次 GET。返回 false = 忙 或 参数不合法 (url 空 / 没给落点 / body_cap<=1)。 */
bool netc_start(const netc_req_t *req);

netc_state_t netc_state(void);
bool         netc_busy(void);

/* 结果读取 (NETC_DONE / NETC_ERR 时有效) */
int netc_http(void);     /* HTTP 状态码 (没拿到 = 0) */
int netc_len(void);      /* 收到的字节数 (流式=已写盘字节) */
int netc_err(void);      /* esp_err_t */
int netc_got(void);      /* 实时已收字节 (进度条用, RUN 期间也在涨) */
uint32_t netc_crc32(void); /* 收到内容的 crc32 (要求 want_crc=true, 与 zlib/binascii 一致) */
const char *netc_url(void);   /* 当前/最近一次的 URL (日志/错误文案用) */

/* DONE/ERR → IDLE (结果被上层取走后调) */
void netc_reset(void);

/* 请求取消: 任务侧尽快收尾 (已写半截文件按失败处理并删除) */
void netc_cancel(void);

/* 掉电恢复（2026-10-30）—— 供"非下载"上下文复用同一套恢复逻辑，例如应用中心
 * 扫描 /sdcard/store_pkg 时发现 `xxx.lua.bak` 就复位成 `xxx.lua`。
 * 语义（与下载路径内部的恢复**完全同一份实现**）：
 *   正式文件缺失 + .bak 存在 → 复位；正式文件存在 + .bak 存在 → 删陈旧 .bak；
 *   没有 .bak → 什么都不做（只花一次 stat）。
 * 不碰 .part（可能正在下载），不碰其它文件。自带栈缓冲，与正在进行的下载无共享状态。
 * 返回 netc_rec_t 的值：0=NONE 1=RESTORED 2=CLEANED 3=FAILED。 */
int netc_recover_path(const char *final_path);

#ifdef __cplusplus
}
#endif
