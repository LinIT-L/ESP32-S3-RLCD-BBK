/* svc_crash.c — 崩溃现场自留证 (见 svc_crash.h 的说明)
 *
 * 为什么不用 core dump 分区: 那要改分区表(高风险, 且擦除/读取都靠工具);
 * 这里用 RTC 保留内存 + 一个 panic handler 的包装函数, 零分区改动,
 * 重启后直接一条命令就能读出来。
 */
#include <string.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_debug_helpers.h"
#include "esp_private/panic_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "svc_crash.h"
#include "svc_logring.h"

#define TAG "CRASH"

/* RTC_NOINIT: 软复位 / panic 复位 / 看门狗复位都不清零。
 * 只有真正断电(RTC 域掉电)才丢 —— 所以下面还要转存一份到 NVS。 */
RTC_NOINIT_ATTR static svc_crash_rec_t s_rec;
/* 区分"这份内容是本轮刚写的"还是"上电残留" —— RTC_NOINIT 初值不可假定为 0。 */
RTC_NOINIT_ATTR static uint32_t s_valid_tag;

#define CRASH_VALID_TAG 0x5A5AA5A5u

/* esp_panic_handler 定义在 esp_system/panic.c, 由 esp_system/port/panic_handler.c
 * 跨编译单元调用 ⇒ 链接期 --wrap 能拦到（同文件内直接调用则拦不到）。 */
void __real_esp_panic_handler(panic_info_t *info);

void __wrap_esp_panic_handler(panic_info_t *info)
{
    /* ⚠ 本函数运行在 panic 上下文: 只做栈/RTC 内存写。
     *   不 malloc、不 printf、不碰 flash —— 否则会二次崩溃或死锁。 */
    svc_crash_rec_t r;
    memset(&r, 0, sizeof(r));
    r.magic = SVC_CRASH_MAGIC;
    r.ver   = SVC_CRASH_VER;
    if (info) {
        r.exception = (uint32_t)info->exception;
        r.core      = (uint32_t)info->core;
        r.addr      = (uint32_t)(uintptr_t)info->addr;
        if (info->reason)      strncpy(r.reason, info->reason,      sizeof(r.reason) - 1);
        if (info->description) strncpy(r.desc,   info->description, sizeof(r.desc) - 1);
    }
    {
        const char *tn = pcTaskGetName(NULL);
        if (tn) strncpy(r.task, tn, sizeof(r.task) - 1);
    }
    /* backtrace: 从当前栈帧往上抓, 最多 SVC_CRASH_FRAMES 帧 */
    {
        uint32_t pc = 0, sp = 0, next_pc = 0;
        esp_backtrace_get_start(&pc, &sp, &next_pc);
        esp_backtrace_frame_t fr = { .pc = pc, .sp = sp, .next_pc = next_pc, .exc_frame = false };
        for (int i = 0; i < SVC_CRASH_FRAMES; i++) {
            r.frames[i] = fr.pc;
            r.nframes = (uint16_t)(i + 1);
            if (!esp_backtrace_get_next_frame(&fr)) break;
        }
    }
    s_rec = r;                 /* 结构体整体拷贝到 RTC 段 */
    /* ★ 崩溃时已开机多久 —— 纯 RAM 读, panic 上下文安全 */
    s_rec.uptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_valid_tag = CRASH_VALID_TAG;

    __real_esp_panic_handler(info);   /* 再走 IDF 原本的打印/重启流程 */
}

/* 现场 → NVS（真掉电也能留）。开机只写一次, 量很小。 */
static void crash_save_nvs(const svc_crash_rec_t *r)
{
    nvs_handle_t h;
    if (nvs_open("crashlog", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "last", r, sizeof(*r));
    nvs_commit(h);
    nvs_close(h);
}

static bool crash_load_nvs(svc_crash_rec_t *out)
{
    nvs_handle_t h;
    if (nvs_open("crashlog", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*out);
    esp_err_t e = nvs_get_blob(h, "last", out, &len);
    nvs_close(h);
    return (e == ESP_OK && len == sizeof(*out) && out->ver == SVC_CRASH_VER);
}

/* 复位原因 → 可读文字（ESP-IDF 6.0 esp_reset_reason_t） */
static const char *crash_rst_name(uint32_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON 上电";
        case ESP_RST_EXT:       return "EXT 外部引脚复位";
        case ESP_RST_SW:        return "SW 软件重启";
        case ESP_RST_PANIC:     return "PANIC 异常/断言";
        case ESP_RST_INT_WDT:   return "INT_WDT 中断看门狗";
        case ESP_RST_TASK_WDT:  return "TASK_WDT 任务看门狗";
        case ESP_RST_WDT:       return "WDT 其他看门狗";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP 深睡唤醒";
        case ESP_RST_BROWNOUT:  return "BROWNOUT 掉压";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

void svc_crash_init(void)
{
    bool fresh = (s_valid_tag == CRASH_VALID_TAG &&
                  s_rec.ver == SVC_CRASH_VER &&          /* 布局必须对得上，见 .h 里的警告 */
                  (s_rec.magic == SVC_CRASH_MAGIC || s_rec.magic == SVC_CRASH_MAGIC_DONE));
    /* 本次开机的原因 —— 也就是"上次是怎么死的"（panic / 看门狗 / 掉压…） */
    uint32_t rst = (uint32_t)esp_reset_reason();

    if (!fresh) {
        /* RTC 里没有 panic 现场。两种可能：
         *   ① 真掉电过（RTC 被清）→ 回退读 NVS 存档
         *   ② 上次不是 panic，而是看门狗/掉压复位 —— 这类**不走 panic handler**，
         *      所以没有 crash_rec，但**日志环还在 RTC 里**，那正是排查要的东西。 */
        bool abnormal = (rst == ESP_RST_PANIC || rst == ESP_RST_INT_WDT ||
                         rst == ESP_RST_TASK_WDT || rst == ESP_RST_WDT ||
                         rst == ESP_RST_BROWNOUT);
        svc_crash_rec_t nv;
        memset(&nv, 0, sizeof(nv));
        if (crash_load_nvs(&nv)) {
            ESP_LOGW(TAG, "RTC 现场已丢失(真断电), 以下为 NVS 存档的最后一次崩溃");
            ESP_LOGW(TAG, "复位原因=%s", crash_rst_name(nv.reset_reason));
            ESP_LOGW(TAG, "原因=%s | %s | 任务=%s | 地址=0x%08x | 异常码=%u",
                     nv.reason[0] ? nv.reason : "?", nv.desc[0] ? nv.desc : "?",
                     nv.task[0] ? nv.task : "?", (unsigned)nv.addr, (unsigned)nv.exception);
            for (int i = 0; i < nv.nframes && i < SVC_CRASH_FRAMES; i++)
                ESP_LOGW(TAG, "  #%d 0x%08x", i, (unsigned)nv.frames[i]);
            /* NVS 里也存了崩溃前的告警日志快照 */
            nvs_handle_t h;
            if (nvs_open("crashlog", NVS_READONLY, &h) == ESP_OK) {
                char snap[1600];
                size_t sl = sizeof(snap) - 1;
                if (nvs_get_str(h, "logsnap", snap, &sl) == ESP_OK && snap[0]) {
                    ESP_LOGW(TAG, "---- 崩溃前告警日志快照 ----");
                    ESP_LOGW(TAG, "%s", snap);
                    ESP_LOGW(TAG, "---- 快照结束 ----");
                }
                nvs_close(h);
            }
        }
        if (abnormal) {
            /* ★ 没有 panic 现场，但有异常复位原因 ⇒ 日志环就是唯一现场 */
            ESP_LOGE(TAG, "上次是「%s」复位（未走 panic handler，无 backtrace）",
                     crash_rst_name(rst));
            svc_logring_dump("上次异常复位前");
        }
        return;
    }

    if (s_rec.magic == SVC_CRASH_MAGIC_DONE) return;   /* 这份已经报过了 */

    s_rec.reset_reason = rst;              /* 记下"本次开机原因"= 上次怎么死的 */

    ESP_LOGE(TAG, "========== 上次运行发生崩溃 (RTC 现场) ==========");
    ESP_LOGE(TAG, "复位原因=%s | 崩溃前已开机 %u ms",
             crash_rst_name(rst), (unsigned)s_rec.uptime_ms);
    ESP_LOGE(TAG, "原因=%s | %s", s_rec.reason[0] ? s_rec.reason : "?",
             s_rec.desc[0] ? s_rec.desc : "?");
    ESP_LOGE(TAG, "任务=%s 核心=%u 异常码=%u 出错地址=0x%08x",
             s_rec.task[0] ? s_rec.task : "?", (unsigned)s_rec.core,
             (unsigned)s_rec.exception, (unsigned)s_rec.addr);
    for (int i = 0; i < s_rec.nframes && i < SVC_CRASH_FRAMES; i++)
        ESP_LOGE(TAG, "  #%d 0x%08x", i, (unsigned)s_rec.frames[i]);
    ESP_LOGE(TAG, "提示: 用 `crash` 命令可随时再读; 地址可用 addr2line 对照 LinTOS.elf");
    ESP_LOGE(TAG, "================================================");

    crash_save_nvs(&s_rec);              /* 转存, 防止下次真掉电丢 */

    /* ★ 把崩溃前的告警日志一起存档 —— backtrace 在 Xtensa 上常跳帧,
     *   而"崩溃前最后几行日志"几乎总能说明当时在干什么。 */
    {
        char snap[1600];
        size_t n = svc_logring_snapshot(snap, sizeof(snap));
        if (n > 0) {
            nvs_handle_t h;
            if (nvs_open("crashlog", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_str(h, "logsnap", snap);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGE(TAG, "---- 崩溃前告警日志快照 ----");
            ESP_LOGE(TAG, "%s", snap);
            ESP_LOGE(TAG, "---- 快照结束 ----");
        }
    }

    s_rec.magic = SVC_CRASH_MAGIC_DONE;  /* 内容保留, 只是不再重复打印 */
}

bool svc_crash_get(svc_crash_rec_t *out)
{
    if (!out) return false;
    if (s_valid_tag == CRASH_VALID_TAG && s_rec.ver == SVC_CRASH_VER &&
        (s_rec.magic == SVC_CRASH_MAGIC || s_rec.magic == SVC_CRASH_MAGIC_DONE)) {
        *out = s_rec;
        return true;
    }
    memset(out, 0, sizeof(*out));
    return crash_load_nvs(out);
}
