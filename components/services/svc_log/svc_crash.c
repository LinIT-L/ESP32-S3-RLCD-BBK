/* svc_crash.c — 崩溃现场自留证 (见 svc_crash.h 的说明)
 *
 * 为什么不用 core dump 分区: 那要改分区表(高风险, 且擦除/读取都靠工具);
 * 这里用 RTC 保留内存 + 一个 panic handler 的包装函数, 零分区改动,
 * 重启后直接一条命令就能读出来。
 *
 * ★ 2026-11-25 两段式改造（"重启后拿不到任何现场"）:
 *   svc_crash_init()     —— 开机早期, 只读 RTC + 打印, 不碰 NVS
 *   svc_crash_persist()  —— nvs_flash_init() 之后, 转存现场 + 追加复位历史
 *   原因见 svc_crash.h 顶部注释（NVS 时序 + 非 panic 复位不落盘）。
 */
#include <string.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_private/panic_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sd_arb.h"        /* 现场要记"卡当时归谁"：纯内存读，panic 上下文安全 */
#include "svc_crash.h"
#include "svc_logring.h"

/* 异常帧：panic_info_t->frame 在 Xtensa 上就是 XtExcFrame*（IDF 的
 * panic_arch_fill_info 也是这么用的）。非 Xtensa 目标则不取寄存器。 */
#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "xtensa_context.h"
#define CRASH_HAVE_REGS 1
#else
#define CRASH_HAVE_REGS 0
#endif

#define TAG "CRASH"

/* web_files 只做状态查询, 用弱符号引用: 免得 svc_log 为一个 bool 拉进
 * web_core/wifi_manager 整条依赖链（也让不带网页服务的构建照样能链接）。 */
extern bool web_files_is_running(void) __attribute__((weak));

/* RTC_NOINIT: 软复位 / panic 复位 / 看门狗复位都不清零。
 * 只有真正断电(RTC 域掉电)才丢 —— 所以下面还要转存一份到 NVS。 */
RTC_NOINIT_ATTR static svc_crash_rec_t s_rec;
/* 区分"这份内容是本轮刚写的"还是"上电残留" —— RTC_NOINIT 初值不可假定为 0。 */
RTC_NOINIT_ATTR static uint32_t s_valid_tag;
/* 累计开机次数: 用来判断现场新旧、给 persist 做同一次开机去重。掉电归零。 */
RTC_NOINIT_ATTR static uint32_t s_boot_seq;

#define CRASH_VALID_TAG 0x5A5AA5A5u
#define CRASH_SEQ_MAX   1000000u

/* esp_panic_handler 定义在 esp_system/panic.c, 由 esp_system/port/panic_handler.c
 * 跨编译单元调用 ⇒ 链接期 --wrap 能拦到（同文件内直接调用则拦不到）。
 * 已核实: build/build.ninja 的链接命令带 -Wl,--wrap=esp_panic_handler,
 *         且 LinTOS.elf 里 panic_handler() 的反汇编确实取 __wrap_esp_panic_handler 地址。 */
void __real_esp_panic_handler(panic_info_t *info);

/* CRC 覆盖整个结构（计算时把 crc 字段本身当 0）。 */
static uint32_t crash_crc(svc_crash_rec_t *r)
{
    uint32_t save = r->crc;
    r->crc = 0;
    uint32_t c = esp_rom_crc32_le(0, (const uint8_t *)r, sizeof(*r));
    r->crc = save;
    return c;
}

void __wrap_esp_panic_handler(panic_info_t *info)
{
    /* ⚠ 本函数运行在 panic 上下文: 只做栈/RTC 内存写。
     *   不 malloc、不 printf、不碰 flash —— 否则会二次崩溃或死锁。 */
    svc_crash_rec_t r;
    memset(&r, 0, sizeof(r));           /* 同时清掉 padding, CRC 才稳定 */
    r.magic = SVC_CRASH_MAGIC;
    r.ver   = SVC_CRASH_VER;
    r.boot_seq = s_boot_seq;
    r.reset_reason = (uint32_t)esp_reset_reason();   /* 本次运行是怎么起来的 */
    if (info) {
        r.exception = (uint32_t)info->exception;
        r.core      = (uint32_t)info->core;
        r.addr      = (uint32_t)(uintptr_t)info->addr;
        if (info->reason)      strncpy(r.reason, info->reason,      sizeof(r.reason) - 1);
        if (info->description) strncpy(r.desc,   info->description, sizeof(r.desc) - 1);
    }
    /* ★ 关键寄存器：异常帧里的现场比 backtrace 更硬。
     *   exccause=28/29 分别是 LoadProhibited/StoreProhibited，excvaddr 就是那个坏地址。 */
#if CRASH_HAVE_REGS
    if (info && info->frame) {
        const XtExcFrame *fr = (const XtExcFrame *)info->frame;
        r.reg_pc       = (uint32_t)fr->pc;
        r.reg_ps       = (uint32_t)fr->ps;
        r.reg_exccause = (uint32_t)fr->exccause;
        r.reg_excvaddr = (uint32_t)fr->excvaddr;
        r.reg_a0       = (uint32_t)fr->a0;
        r.reg_a1       = (uint32_t)fr->a1;
        r.reg_a2       = (uint32_t)fr->a2;
        r.reg_a3       = (uint32_t)fr->a3;
        r.reg_a4       = (uint32_t)fr->a4;
        r.reg_a5       = (uint32_t)fr->a5;
    }
#endif
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
    /* ★ 崩溃瞬间的上下文 —— 这一段是本次新增, 全部是纯读操作 */
    {
        /* FreeRTOS 的 StackType_t 在 ESP-IDF 里是 uint8_t ⇒ 返回值单位是字节 */
        UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
        r.stack_free = (hw > 0) ? (uint32_t)hw : 0;
    }
    r.heap_free  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    r.heap_min   = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    r.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    {
        sd_arb_state_t st = sd_arb_state();      /* 只读一次: 两次调用之间状态可能变 */
        r.sd_state   = (uint8_t)st;
        r.msc_owned  = (st == SD_ARB_USB) ? 1 : 0;
        int inf = sd_arb_inflight();
        r.sd_inflight = (uint8_t)(inf < 0 ? 0 : (inf > 255 ? 255 : inf));
    }
    /* 弱符号: 构建里没有 web_files 时该指针为 NULL */
    r.web_running = (web_files_is_running && web_files_is_running()) ? 1 : 0;

    /* 崩溃时已开机多久 —— 纯 RAM 读, panic 上下文安全 */
    r.uptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    r.crc = crash_crc(&r);

    s_rec = r;                 /* 结构体整体拷贝到 RTC 段 */
    s_valid_tag = CRASH_VALID_TAG;

    __real_esp_panic_handler(info);   /* 再走 IDF 原本的打印/重启流程 */
}

/* ★ 崩溃前日志快照必须在 svc_crash_init() 里抓：
 *   svc_crash_init 早于 svc_logring_init，此时 RTC 环里还是**上次崩溃前**的内容；
 *   等到 persist 阶段（NVS 就绪）再抓，环已经被本次启动的日志冲掉了。
 *   抓到后暂存在 RAM，落盘延到 persist。 */
static char   s_crash_snap[1600];
static size_t s_crash_snap_len = 0;

/* 复位原因 → 可读文字（ESP-IDF 6.0 esp_reset_reason_t） */
const char *svc_crash_rst_name(uint32_t r)
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

/* RTC 里那份记录是否可用（magic/ver 都对；CRC 允许失败但会标注） */
static bool rec_from_rtc(svc_crash_rec_t *out)
{
    if (s_valid_tag != CRASH_VALID_TAG) return false;
    if (s_rec.ver != SVC_CRASH_VER) return false;
    if (s_rec.magic != SVC_CRASH_MAGIC && s_rec.magic != SVC_CRASH_MAGIC_DONE) return false;
    *out = s_rec;
    return true;
}

static bool rec_crc_ok(svc_crash_rec_t *r)
{
    if (r->crc == 0) return false;          /* 老记录没有 CRC */
    return crash_crc(r) == r->crc;
}

/* ---------------- NVS：现场存档 + 复位历史 ---------------- */

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

/* 追加一条复位历史。
 * ⚠ 去重**不能**靠 boot_seq：连续掉电时 RTC 每次都清、序号恒为 1，
 *   那样第 2 次以后的重启会被误判成"同一次开机"而全部丢弃 —— 恰恰是
 *   "反复掉电/棕断"这种最需要看清的场景。改用本次开机的 RAM 标志（见 persist）。 */
static bool hist_append(uint32_t rst, uint32_t uptime, uint32_t flags)
{
    nvs_handle_t h;
    if (nvs_open("crashlog", NVS_READWRITE, &h) != ESP_OK) return false;

    svc_crash_hist_t hist;
    memset(&hist, 0, sizeof(hist));
    size_t len = sizeof(hist);
    if (nvs_get_blob(h, "hist", &hist, &len) != ESP_OK ||
        len != sizeof(hist) || hist.magic != SVC_CRASH_HIST_MAGIC ||
        hist.ver != SVC_CRASH_VER) {
        memset(&hist, 0, sizeof(hist));
    }
    hist.magic = SVC_CRASH_HIST_MAGIC;
    hist.ver   = SVC_CRASH_VER;
    hist.head  = (uint16_t)(hist.head % SVC_CRASH_HIST_N);

    hist.e[hist.head].rst    = rst;
    hist.e[hist.head].uptime = uptime;
    hist.e[hist.head].flags  = flags;
    hist.head = (uint16_t)((hist.head + 1) % SVC_CRASH_HIST_N);
    hist.total++;

    nvs_set_blob(h, "hist", &hist, sizeof(hist));
    nvs_commit(h);
    nvs_close(h);
    return true;
}

/* ---------------- 对外接口 ---------------- */

void svc_crash_init(void)
{
    /* 开机计数: RTC 保留则累加; 上电残留(valid_tag 对不上)或脏值则从 1 重来 */
    if (s_valid_tag != CRASH_VALID_TAG) s_boot_seq = 0;
    if (s_boot_seq == 0 || s_boot_seq > CRASH_SEQ_MAX) s_boot_seq = 0;
    s_boot_seq++;

    uint32_t rst = (uint32_t)esp_reset_reason();   /* 本次开机原因 = 上次怎么死的 */

    /* 先把"上次崩溃前"的告警日志抓下来（此时 RTC 环还没被本次启动冲掉）。
     * backtrace 在 Xtensa 上常跳帧，而"最后几行日志"几乎总能说明当时在干什么。 */
    s_crash_snap_len = svc_logring_snapshot(s_crash_snap, sizeof(s_crash_snap));

    svc_crash_rec_t r;
    bool have = rec_from_rtc(&r);

    if (!have) {
        /* RTC 里没有 panic 现场。两种可能：
         *   ① 真掉电过（RTC 被清）→ 现场只在 NVS（由 persist 阶段读）
         *   ② 上次不是 panic，而是看门狗/掉压复位 —— 这类**不走 panic handler**，
         *      所以没有 crash_rec，但**日志环还在 RTC 里**，那正是排查要的东西。 */
        bool abnormal = (rst == ESP_RST_PANIC || rst == ESP_RST_INT_WDT ||
                         rst == ESP_RST_TASK_WDT || rst == ESP_RST_WDT ||
                         rst == ESP_RST_BROWNOUT);
        if (abnormal) {
            /* ★ 没有 panic 现场，但有异常复位原因 ⇒ 日志环就是唯一现场 */
            ESP_LOGE(TAG, "上次是「%s」复位（未走 panic handler，无 backtrace）",
                     svc_crash_rst_name(rst));
            svc_logring_dump("上次异常复位前");
        } else {
            ESP_LOGI(TAG, "上次复位原因: %s（无崩溃现场）", svc_crash_rst_name(rst));
        }
        return;
    }

    if (s_rec.magic == SVC_CRASH_MAGIC_DONE) {
        /* 这份已经报过了；只提示复位原因，避免刷屏 */
        ESP_LOGI(TAG, "上次复位原因: %s（崩溃现场已报告过，用 crash 命令可重读）",
                 svc_crash_rst_name(rst));
        return;
    }

    bool crc_ok = rec_crc_ok(&r);

    ESP_LOGE(TAG, "========== 上次运行发生崩溃 (RTC 现场) ==========");
    ESP_LOGE(TAG, "复位原因=%s | 崩溃前已开机 %u ms | 开机序号 %u",
             svc_crash_rst_name(rst), (unsigned)r.uptime_ms, (unsigned)r.boot_seq);
    ESP_LOGE(TAG, "原因=%s | %s", r.reason[0] ? r.reason : "?",
             r.desc[0] ? r.desc : "?");
    ESP_LOGE(TAG, "任务=%s 核心=%u 异常码=%u 出错地址=0x%08x",
             r.task[0] ? r.task : "?", (unsigned)r.core,
             (unsigned)r.exception, (unsigned)r.addr);
    if (r.reg_pc || r.reg_exccause || r.reg_excvaddr) {
        ESP_LOGE(TAG, "PC=0x%08x PS=0x%08x EXCCAUSE=%u EXCVADDR=0x%08x",
                 (unsigned)r.reg_pc, (unsigned)r.reg_ps,
                 (unsigned)r.reg_exccause, (unsigned)r.reg_excvaddr);
        ESP_LOGE(TAG, "A0=0x%08x A1=0x%08x A2=0x%08x A3=0x%08x A4=0x%08x A5=0x%08x",
                 (unsigned)r.reg_a0, (unsigned)r.reg_a1, (unsigned)r.reg_a2,
                 (unsigned)r.reg_a3, (unsigned)r.reg_a4, (unsigned)r.reg_a5);
        /* exccause: 28=LoadProhibited 读非法 29=StoreProhibited 写非法
         *           20=InstructionFetchError 取指失败 24=LoadStoreAlignment 未对齐 */
        ESP_LOGE(TAG, "判读: EXCCAUSE 28=读非法 / 29=写非法 / 20=取指失败 / 24=未对齐");
    }
    ESP_LOGE(TAG, "栈余量=%u B | 内部堆 空闲=%u 最低=%u | PSRAM 空闲=%u",
             (unsigned)r.stack_free, (unsigned)r.heap_free,
             (unsigned)r.heap_min, (unsigned)r.psram_free);
    ESP_LOGE(TAG, "卡状态=%s 在途=%u MSC持有=%u | 网页服务=%s",
             sd_arb_state_str(), (unsigned)r.sd_inflight,
             (unsigned)r.msc_owned, r.web_running ? "运行中" : "关");
    for (int i = 0; i < r.nframes && i < SVC_CRASH_FRAMES; i++)
        ESP_LOGE(TAG, "  #%d 0x%08x", i, (unsigned)r.frames[i]);
    if (!crc_ok) ESP_LOGW(TAG, "⚠ 记录 CRC 校验不通过（可能写入时被打断），以下内容仅供参考");
    ESP_LOGE(TAG, "提示: 用 `crash` 命令可随时再读; 地址可用 addr2line 对照 LinTOS.elf");
    ESP_LOGE(TAG, "================================================");

    s_rec.magic = SVC_CRASH_MAGIC_DONE;  /* 内容保留, 只是不再重复打印 */
    /* magic 改过了, CRC 必须跟着重算 —— 否则 persist 阶段转存进 NVS 的那份
     * 永远是"CRC 校验失败"，下次真掉电回退时白白报一个假警。 */
    s_rec.crc = crash_crc(&s_rec);

    /* 打印崩溃前抓到的告警日志快照；落盘由 persist 阶段完成 */
    if (s_crash_snap_len > 0) {
        ESP_LOGE(TAG, "---- 崩溃前告警日志快照 ----");
        ESP_LOGE(TAG, "%s", s_crash_snap);
        ESP_LOGE(TAG, "---- 快照结束 ----");
    }
}

void svc_crash_persist(void)
{
    /* 同一次开机只做一遍（这是历史去重的唯一依据，不用 NVS 里的序号：
     *  连续掉电时 RTC 序号恒定，靠序号去重会把真正的连续重启全吃掉）。 */
    static bool s_persisted = false;
    if (s_persisted) return;
    s_persisted = true;

    /* NVS 已就绪。这里做三件事:
     *   ① RTC 现场转存（下次真掉电还有存档）
     *   ② 本次开机复位原因追加进历史环（覆盖 brownout/WDT/SW 这些不走 panic 的类型）
     *   ③ 异常复位时把日志环快照一并存档 */

    svc_crash_rec_t r;
    bool have = rec_from_rtc(&r);

    uint32_t rst = (uint32_t)esp_reset_reason();
    uint32_t uptime = have ? r.uptime_ms : 0;
    uint32_t flags = 0;
    if (have && (rst == ESP_RST_PANIC || r.exception)) flags |= 1;

    /* 历史必须**无条件**写: 这才是"看门狗/掉压也能留痕"的落脚点 */
    bool wrote = hist_append(rst, uptime, flags);

    if (!have) {
        /* RTC 没有现场 —— 可能是真掉电, 试试 NVS 里的存档 */
        svc_crash_rec_t nv;
        memset(&nv, 0, sizeof(nv));
        if (crash_load_nvs(&nv)) {
            ESP_LOGW(TAG, "RTC 现场已丢失(真断电), 以下为 NVS 存档的最后一次崩溃");
            ESP_LOGW(TAG, "复位原因=%s | 保存时开机序号=%u",
                     svc_crash_rst_name(nv.reset_reason), (unsigned)nv.boot_seq);
            ESP_LOGW(TAG, "原因=%s | %s | 任务=%s | 地址=0x%08x",
                     nv.reason[0] ? nv.reason : "?", nv.desc[0] ? nv.desc : "?",
                     nv.task[0] ? nv.task : "?", (unsigned)nv.addr);
            if (!rec_crc_ok(&nv)) ESP_LOGW(TAG, "⚠ NVS 存档 CRC 校验不通过");
        } else if (wrote) {
            ESP_LOGW(TAG, "无崩溃存档（RTC 与 NVS 都没有）—— 本次是 %s 重启，"
                          "用 `crash` 查看复位历史判断是否在循环", svc_crash_rst_name(rst));
        }
        /* 即便没有 panic 现场，也要把"上次异常重启前"的告警日志存下来 ——
         * 看门狗/掉压复位不走 panic handler，这份日志环就是唯一现场。 */
        if (s_crash_snap_len > 0) {
            nvs_handle_t h;
            if (nvs_open("crashlog", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_str(h, "logsnap", s_crash_snap);
                nvs_commit(h);
                nvs_close(h);
            }
        }
        return;
    }

    /* 有 RTC 现场: 存 NVS + 补写"崩溃前"日志快照（快照在 init 阶段就抓好了） */
    crash_save_nvs(&r);
    if (s_crash_snap_len > 0) {
        nvs_handle_t h;
        if (nvs_open("crashlog", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "logsnap", s_crash_snap);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

bool svc_crash_get(svc_crash_rec_t *out)
{
    if (!out) return false;
    if (rec_from_rtc(out)) return true;
    memset(out, 0, sizeof(*out));
    return crash_load_nvs(out);
}

bool svc_crash_hist_get(svc_crash_hist_t *out)
{
    if (!out) return false;
    nvs_handle_t h;
    if (nvs_open("crashlog", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*out);
    esp_err_t e = nvs_get_blob(h, "hist", out, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(*out)) return false;
    if (out->magic != SVC_CRASH_HIST_MAGIC || out->ver != SVC_CRASH_VER) return false;
    if (out->head >= SVC_CRASH_HIST_N) out->head = 0;
    return true;
}
