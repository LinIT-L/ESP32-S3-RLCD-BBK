/*
 * SPDX-FileCopyrightText: 2026
 *
 * USB 存储会话管理器 —— TF 卡"本机 / 电脑"交接的**唯一**生命周期所有者。
 * 对外接口与状态机说明见 include/usbh_msc_sdspi.h。
 *
 * 职责边界 (2026-11-25 重构后):
 *   · 本文件: 会话状态机、后台任务、交卡/收卡顺序、超时、USB 模式与串口的进出、
 *            NVS 诊断记录。
 *   · sd_scan: 只提供 sd_vfs_handoff() / sd_vfs_reclaim() 两个**有界**入口;
 *            卡的所有权标志与仲裁状态由它自己维护。
 *   · usb_hid: 只负责 PHY 归属与 Serial/JTAG 驱动装卸 (含 RECOVER_FAIL / TINYUSB_STUCK)、
 *            浅睡否决权; 本文件不碰 pad、不碰 PM。
 *   · page_storage: 只发请求 + 读快照渲染弹窗, **不持有任何生命周期状态**。
 *
 * ⛔ 不要在这里重加 "s_running" 之类第二个"是否挂载"标志: 状态机就是唯一真相。
 */
#include "usbh_msc_sdspi.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdmmc_cmd.h"
#include "tinyusb_msc.h"
#include "tusb.h"            /* tud_mounted(): 主机何时真正认到本设备 */
#include "usb_hid.h"         /* 模式切换 + PHY 归属 + 串口驱动装卸 */
#include "sd_arb.h"          /* 交卡前后校验: 本机必须已经放手 */
#include "sd_scan.h"         /* sd_vfs_handoff / sd_vfs_reclaim / sd_get_card */
#include "web_files.h"       /* web_files_quiesce / web_files_resume: 交卡前清场 */
#include "svc_log.h"         /* 挂载期间暂停串口控制台(只写文件缓冲) */
#include "driver/usb_serial_jtag.h"   /* is_connected / is_driver_installed */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps / vTaskDeleteWithCaps */

static const char *TAG = "USBH_MSC";

/* ── 统一超时 (只在这里定义, 不允许各分支自己写魔法数字) ──────────────────── */
#define MSC_LOCK_WAIT_MS       8000u   /* 等 SD 挂载锁: 首次 */
#define MSC_LOCK_RETRY_MS      4000u   /* 等 SD 挂载锁: 二次 */
#define MSC_LOCK_RELEASE_MS    1000u   /* 兜底释放所有权时等锁 */
#define MSC_WEB_QUIESCE_MS     3000u   /* 等网页共享清场 */
#define MSC_SESSION_STACK      4096u   /* 会话任务栈 (PSRAM) */
#define MSC_SESSION_PRIO       5
/* ★ 2026-11-25: 卸载前等主机侧延迟写排空。
 * tinyusb_msc_delete_storage() 在 storage->deffered_writes>0 时返回
 * ESP_ERR_INVALID_STATE 且**不修改任何状态** —— 可安全有界重试。
 * 绝不能像重构前那样失败就 s_storage=NULL: 那会丢句柄 + 留 LUN 映射 +
 * _scard 不清, 随后 usb_composite_exit 强杀 TinyUSB 任务丢掉未刷的写,
 * 紧接着本地 FatFs 重挂同卡 ⇒ 两边同写/SDMMC 半事务 ⇒ 死机重启。 */
#define MSC_DEL_RETRY_N        30u     /* 最多重试 30 次 */
#define MSC_DEL_RETRY_MS       100u    /* 每次间隔 100ms ⇒ 上限 3s */

/* ═══════════════════════════════════════════════════════════════════════════
 * 一、会话状态 (唯一真相)
 * ═══════════════════════════════════════════════════════════════════════════ */
static portMUX_TYPE s_st_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile usbh_msc_state_t   s_state    = USBH_MSC_IDLE;
static volatile usbh_msc_outcome_t s_outcome  = USBH_MSC_OP_NONE;
static volatile uint32_t           s_seq      = 0;
static volatile uint32_t           s_since_ms = 0;
static volatile int                s_err      = 0;
/* 取消是**闩锁**而不是队列项: 它必须在当前阶段跑完的瞬间就被看到,
 * 排到队尾就失去意义了。只有 UI 写、会话任务读并清除。 */
static volatile bool               s_cancel   = false;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 状态迁移: state/outcome/err/seq/since 一次写完, 读者不会看到半新半旧。 */
static void st_set(usbh_msc_state_t st, usbh_msc_outcome_t oc, int err)
{
    portENTER_CRITICAL(&s_st_mux);
    s_state    = st;
    s_outcome  = oc;
    s_err      = err;
    s_since_ms = now_ms();
    s_seq++;
    portEXIT_CRITICAL(&s_st_mux);
}

usbh_msc_status_t usbh_msc_status(void)
{
    usbh_msc_status_t o;
    portENTER_CRITICAL(&s_st_mux);
    o.state    = s_state;
    o.outcome  = s_outcome;
    o.seq      = s_seq;
    o.since_ms = s_since_ms;
    o.err      = s_err;
    portEXIT_CRITICAL(&s_st_mux);
    return o;
}

const char *usbh_msc_state_str(usbh_msc_state_t st)
{
    switch (st) {
        case USBH_MSC_IDLE:      return "IDLE(本机)";
        case USBH_MSC_PREPARING: return "PREPARING(交卡中)";
        case USBH_MSC_USB:       return "USB(电脑接管)";
        case USBH_MSC_STOPPING:  return "STOPPING(停USB中)";
        case USBH_MSC_RESTORING: return "RESTORING(收卡中)";
        case USBH_MSC_FAULT:     return "FAULT(本机存储未恢复)";
        default:                 return "UNKNOWN";
    }
}

static const char *outcome_str(usbh_msc_outcome_t oc)
{
    switch (oc) {
        case USBH_MSC_OP_NONE:           return "-";
        case USBH_MSC_OP_START_FAIL_VFS: return "挂载失败(本地VFS未交出)";
        case USBH_MSC_OP_START_FAIL_USB: return "挂载失败(USB未启动)";
        case USBH_MSC_OP_START_BUSY:     return "挂载中止(网页正在传输)";
        case USBH_MSC_OP_CANCELLED:      return "已取消挂载";
        case USBH_MSC_OP_STOP_OK:        return "卸载完成";
        case USBH_MSC_OP_STOP_DEGRADED:  return "卸载完成但本机存储未恢复";
        case USBH_MSC_OP_STOP_USB_STUCK: return "USB 未退干净(卡仍归电脑)";
        case USBH_MSC_OP_STOP_DEFERRED:  return "主机仍在写(卸载已中止,U盘保持挂载)";
        default:                         return "?";
    }
}

bool usbh_msc_card_busy(void)
{
    usbh_msc_state_t st = s_state;
    return st == USBH_MSC_PREPARING || st == USBH_MSC_USB ||
           st == USBH_MSC_STOPPING  || st == USBH_MSC_RESTORING;
}

bool usbh_msc_is_running(void) { return s_state == USBH_MSC_USB; }

/* ═══════════════════════════════════════════════════════════════════════════
 * 二、会话任务 (唯一消费者) 与请求 (唯一生产者 = UI 任务)
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef enum { MSC_REQ_START = 0, MSC_REQ_STOP } msc_req_t;

static QueueHandle_t s_req_q = NULL;
static void msc_do_start(void);
static void msc_do_stop(void);

static void msc_session_task(void *arg)
{
    (void)arg;
    msc_req_t r;
    for (;;) {
        if (xQueueReceive(s_req_q, &r, portMAX_DELAY) != pdTRUE) continue;
        if (r == MSC_REQ_START) msc_do_start();
        else                    msc_do_stop();
    }
}

/* 惰性建队列 + 任务 (只由 UI 任务调, 单生产者 ⇒ 不需要额外互斥) */
static bool msc_worker_ensure(void)
{
    if (s_req_q) return true;
    s_req_q = xQueueCreate(2, sizeof(msc_req_t));
    if (!s_req_q) {
        ESP_LOGE(TAG, "请求队列创建失败 ⇒ 无法启动会话");
        return false;
    }
    if (xTaskCreateWithCaps(msc_session_task, "msc_sess", MSC_SESSION_STACK, NULL,
                            MSC_SESSION_PRIO, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "会话任务创建失败(PSRAM 不足?)");
        vQueueDelete(s_req_q);
        s_req_q = NULL;
        return false;
    }
    return true;
}

static bool msc_post(msc_req_t r)
{
    if (!msc_worker_ensure()) return false;
    if (xQueueSend(s_req_q, &r, 0) != pdTRUE) {
        ESP_LOGW(TAG, "请求队列已满 ⇒ 丢弃请求 %d", (int)r);
        return false;
    }
    return true;
}

/* 读并清除取消闩锁 (只有会话任务调) */
static bool msc_take_cancel(void)
{
    bool c;
    portENTER_CRITICAL(&s_st_mux);
    c = s_cancel;
    s_cancel = false;
    portEXIT_CRITICAL(&s_st_mux);
    return c;
}

bool usbh_msc_request_start(void)
{
    usbh_msc_state_t prev = USBH_MSC_IDLE;
    bool ok = false;

    portENTER_CRITICAL(&s_st_mux);
    if (s_state == USBH_MSC_IDLE || s_state == USBH_MSC_FAULT) {
        prev = s_state;
        s_state    = USBH_MSC_PREPARING;
        s_outcome  = USBH_MSC_OP_NONE;
        s_err      = 0;
        s_cancel   = false;      /* 上一轮的取消不能泄漏到这一次 */
        s_since_ms = now_ms();
        s_seq++;
        ok = true;
    }
    portEXIT_CRITICAL(&s_st_mux);
    if (!ok) return false;

    if (!msc_post(MSC_REQ_START)) {
        st_set(prev, USBH_MSC_OP_START_FAIL_USB, ESP_ERR_NO_MEM);   /* 没起来: 如实回退 */
        return false;
    }
    return true;
}

bool usbh_msc_request_stop(void)
{
    bool ok = false;
    portENTER_CRITICAL(&s_st_mux);
    if (s_state == USBH_MSC_USB) {
        s_state    = USBH_MSC_STOPPING;
        s_outcome  = USBH_MSC_OP_NONE;
        s_err      = 0;
        s_since_ms = now_ms();
        s_seq++;
        ok = true;
    }
    portEXIT_CRITICAL(&s_st_mux);
    if (!ok) return false;
    if (!msc_post(MSC_REQ_STOP)) {
        /* 队列满(理论上不会: 单生产者+状态门控) ⇒ 回退到"已挂载", 让用户再点一次,
         * 绝不能留在 STOPPING 而没有任何任务在处理。 */
        st_set(USBH_MSC_USB, USBH_MSC_OP_NONE, 0);
        return false;
    }
    return true;
}

bool usbh_msc_request_cancel(void)
{
    bool preparing = false;
    portENTER_CRITICAL(&s_st_mux);
    if (s_state == USBH_MSC_PREPARING) {
        s_cancel  = true;
        preparing = true;
    }
    portEXIT_CRITICAL(&s_st_mux);
    if (preparing) return true;
    return usbh_msc_request_stop();     /* 其它状态下取消 == 卸载 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 三、诊断记录 (NVS) —— 写入方只有会话任务, 一次流程一条
 * ═══════════════════════════════════════════════════════════════════════════ */
#define MSC_DIAG_VER          1u
#define MSC_DIAG_VFS_PENDING  (0x7FFF)   /* VFS 阶段没跑完就落盘(异常) */
#define MSC_DIAG_NVS_NS       "mscdiag"
#define MSC_DIAG_NVS_KEY      "last"

typedef struct {
    uint16_t ver;        /* = MSC_DIAG_VER */
    uint16_t flags;      /* bit0 = 含 VFS 阶段 */
    uint32_t seq;        /* 每完成一次流程 +1; 不涨 ⇒ 上次没走完 */
    uint32_t up_ms;      /* 提交时的 uptime, 用于和串口日志对齐 */
    int32_t  enter_ms;   /* 进 USB 模式(卸 SJ + 装 TinyUSB)耗时 */
    int32_t  lun_ms;     /* LUN/介质映射耗时 */
    int32_t  enum_ms;    /* 主机枚举耗时; -1 = 60s 内没认到 */
    int32_t  stop_ms;    /* 停 USB(删 storage + 退模式 + 恢复串口)耗时 */
    int32_t  exit_rc;    /* usb_composite_exit() 返回值 */
    int32_t  vfs_rc;     /* 本地 VFS 收回结果; MSC_DIAG_VFS_PENDING = 未完成 */
    int32_t  vfs_ms;     /* 本地 VFS 收回耗时 */
    uint8_t  usj_ok;     /* 提交时 Serial/JTAG 驱动是否在位 */
    uint8_t  rsv[3];
} msc_diag_t;

static msc_diag_t s_diag;
static bool       s_diag_open = false;   /* 本次流程的记录还没落盘 */

static int32_t msc_ms_since(int64_t t0, int64_t t1)
{
    if (!t0 || !t1 || t1 < t0) return -1;
    return (int32_t)((t1 - t0) / 1000);
}

static void msc_diag_begin(void)
{
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.ver     = MSC_DIAG_VER;
    s_diag.enum_ms = -1;
    s_diag.exit_rc = -1;
    s_diag.vfs_rc  = MSC_DIAG_VFS_PENDING;
    s_diag.vfs_ms  = -1;
    s_diag_open    = true;
}

static void msc_diag_end(int vfs_rc, int vfs_ms)
{
    s_diag.vfs_rc = vfs_rc;
    s_diag.vfs_ms = vfs_ms;
    s_diag.flags |= 1u;
}

static void msc_diag_print(const msc_diag_t *d, const char *src, int round, int total)
{
    char enum_buf[40];
    if (d->enum_ms >= 0) snprintf(enum_buf, sizeof(enum_buf), "%dms", (int)d->enum_ms);
    else                 snprintf(enum_buf, sizeof(enum_buf), "未认到");

    char vfs_buf[40];
    if (d->vfs_rc == MSC_DIAG_VFS_PENDING) snprintf(vfs_buf, sizeof(vfs_buf), "未完成");
    else snprintf(vfs_buf, sizeof(vfs_buf), "%dms/rc=%d", (int)d->vfs_ms, (int)d->vfs_rc);

    if (total > 1)
        ESP_LOGI(TAG, "[MSC 时序 %d/%d]%s seq=%u up=%ums | 进USB %dms | 建LUN %dms | "
                      "主机枚举 %s | 卸载 %dms(exit=%d) | 本地VFS %s | usj=%u",
                 round, total, src, (unsigned)d->seq, (unsigned)d->up_ms,
                 (int)d->enter_ms, (int)d->lun_ms, enum_buf,
                 (int)d->stop_ms, (int)d->exit_rc, vfs_buf, (unsigned)d->usj_ok);
    else
        ESP_LOGI(TAG, "[MSC 时序]%s seq=%u up=%ums | 进USB %dms | 建LUN %dms | "
                      "主机枚举 %s | 卸载 %dms(exit=%d) | 本地VFS %s | usj=%u",
                 src, (unsigned)d->seq, (unsigned)d->up_ms,
                 (int)d->enter_ms, (int)d->lun_ms, enum_buf,
                 (int)d->stop_ms, (int)d->exit_rc, vfs_buf, (unsigned)d->usj_ok);
}

/* 落 NVS: 覆盖上一条, seq = 上一条 seq + 1。
 * 失败(未初始化/分区不存在)只告警 —— 记录仍在 RAM 里, 本次 `mscstat` 可读。 */
/* ★★ 2026-11-25 定稿: 诊断记录**只留 RAM, 绝不写 NVS** ★★
 * 四轮实测教训: 只要卸载路径里出现 nvs_commit() → esp_partition_write() →
 * spi_flash_write(), 就会通过 IPC 关掉双核 flash cache。此刻 SD 刚重挂、
 * web/watcher 刚恢复、播报任务刚建 —— 任一任务在 cache 关闭窗口访 flash
 * 就 panic, 或写操作互相拖住 ⇒ 状态机停在 RESTORING(界面"存储恢复中")出不来。
 * 最简且唯一可靠: 这个关键路径里**一行 flash 写都不许有**。
 * mscstat 改成读 RAM 里的 s_diag; 掉电即失, 但换来了卸载绝不被诊断拖死。
 * 需要持久化时另起低优先级任务在"系统完全空闲"时写, 不放这里。 */
static void msc_diag_commit(void)
{
    if (!s_diag_open) return;
    s_diag_open = false;
    s_diag.ver    = MSC_DIAG_VER;
    s_diag.up_ms  = now_ms();
    s_diag.usj_ok = usb_serial_jtag_is_driver_installed() ? 1 : 0;
    s_diag.seq++;
}

/* 播报: 用**副本**在独立任务里重复 6 轮, 每轮先查"串口是否真的可写"。
 * 为什么不能直接打: 卸载瞬间主机还没重新枚举出 CDC, IDF 串口 VFS write()
 *   第一句就是 if (!is_connected()) return EIO ⇒ 整行被丢弃(实测两次都是 0 字节)。
 * 为什么用副本: 避免与下一次流程写 s_diag 竞争(否则会打出半新半旧的混合记录)。 */
static void msc_diag_report_task(void *arg)
{
    msc_diag_t *p = (msc_diag_t *)arg;
    msc_diag_t d = *p;
    heap_caps_free(p);

    for (int r = 1; r <= 6; r++) {
        if (usb_serial_jtag_is_connected())
            msc_diag_print(&d, "", r, 6);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDeleteWithCaps(NULL);
}

static void msc_diag_report_async(void)
{
    msc_diag_t *p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM);
    if (!p) { ESP_LOGD(TAG, "诊断副本分配失败(仅少一条串口播报)"); return; }
    *p = s_diag;
    if (xTaskCreateWithCaps(msc_diag_report_task, "msc_diag", 3072, p, 2, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        heap_caps_free(p);
        ESP_LOGD(TAG, "诊断播报任务创建失败(记录已落 NVS)");
    }
}

void usbh_msc_diag_dump(void)
{
    if (s_diag.ver != MSC_DIAG_VER) {
        printf("MSCSTAT 无记录 (本次开机尚无完整卸载/挂载流程)\r\n");
        return;
    }
    msc_diag_print(&s_diag, "(RAM) ", 0, 1);
}

void usbh_msc_diag_clear(void)
{
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag_open = false;
    printf("MSCSTAT 已清除(RAM)\r\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 四、USB 侧的开/关 (PHY 与串口的真实交接)
 * ═══════════════════════════════════════════════════════════════════════════ */
static bool s_driver_installed = false;          /* MSC 驱动装一次, 跨会话复用 */
static tinyusb_msc_storage_handle_t s_storage = NULL;

static int64_t  s_t_start_us   = 0;              /* 本次流程起点 */
static int64_t  s_t_enter_us   = 0;
static int64_t  s_t_storage_us = 0;
static volatile int32_t s_enum_ms = -1;

/* 枚举观测任务: 只读 tud_mounted(), 60s 上限。
 * 生命周期用两个标志显式管理 —— 绝不能在 tinyusb deinit 之后还调 tud_*()。 */
static volatile bool s_enum_watch_run   = false;
static volatile bool s_enum_watch_alive = false;

static void msc_enum_watch_task(void *arg)
{
    (void)arg;
    const int64_t t0 = s_t_enter_us;
    for (int i = 0; i < 3000 && s_enum_watch_run; i++) {
        /* ★ 双保险: 即使卸载握手(msc_enum_watch_stop)超时, 只要已不在 TinyUSB 模式
         *   就不再碰协议栈 —— tud_*() 在 deinit 之后是 use-after-free。 */
        if (!usb_hid_mode_owns_phy()) break;
        if (tud_mounted()) {
            s_enum_ms = (int32_t)((esp_timer_get_time() - t0) / 1000);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_enum_watch_alive = false;
    vTaskDeleteWithCaps(NULL);
}

/* 停观测并**等它真的退出**(标志握手, 不靠盲等): 最多 100ms。 */
static void msc_enum_watch_stop(void)
{
    s_enum_watch_run = false;
    for (int i = 0; i < 20 && s_enum_watch_alive; i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (s_enum_watch_alive)
        ESP_LOGW(TAG, "枚举观测任务未在 100ms 内退出(继续卸载, 它已不再调 tud_*)");
}

/* 启动 USB MSC。前置: 卡已由 sd_vfs_handoff() 交出 (仲裁必须已是 USB)。 */
static esp_err_t msc_usb_open(void)
{
    if (s_storage) return ESP_ERR_INVALID_STATE;   /* 不该发生: 说明上一轮没关干净 */

    /* 最后一道闸: 卡句柄 + 仲裁状态。OFF = 没有可用卡, 不让挂(否则 PC 看到 0 字节盘)。 */
    if (sd_get_card() == NULL) {
        ESP_LOGE(TAG, "没有可交出的 card 句柄 ⇒ 拒绝启动 USB");
        return ESP_ERR_INVALID_STATE;
    }
    if (sd_arb_state() != SD_ARB_USB) {
        ESP_LOGE(TAG, "本机仍持有 TF 卡 (%s) ⇒ 拒绝启动 USB", sd_arb_state_str());
        return ESP_ERR_INVALID_STATE;
    }

    /* 挂载期间只写文件缓冲, 不碰已卸载的 Serial/JTAG */
    svc_log_console_suspend(true);

    esp_err_t ret = usb_composite_enter_msc();
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "进入 MSC 模式失败: %s", esp_err_to_name(ret));
        svc_log_console_suspend(false);
        return ret;
    }
    s_t_enter_us    = esp_timer_get_time();
    s_diag.enter_ms = msc_ms_since(s_t_start_us, s_t_enter_us);

    if (!s_driver_installed) {
        const tinyusb_msc_driver_config_t dc = { .user_flags = { .auto_mount_off = 1 } };
        ret = tinyusb_msc_install_driver(&dc);
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "tinyusb_msc_install_driver 失败: %s", esp_err_to_name(ret));
            (void)usb_composite_exit();       /* 回滚: 装回 Serial/JTAG */
            svc_log_console_suspend(false);
            return ret;
        }
        s_driver_installed = true;
    }

    if (s_storage == NULL) {
        const tinyusb_msc_storage_config_t cfg = {
            .medium.card = sd_get_card(),
            .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        };
        ret = tinyusb_msc_new_storage_sdmmc(&cfg, &s_storage);
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "tinyusb_msc_new_storage_sdmmc 失败: %s", esp_err_to_name(ret));
            (void)usb_composite_exit();
            svc_log_console_suspend(false);
            return ret;
        }
    }

    s_t_storage_us = esp_timer_get_time();
    s_diag.lun_ms  = msc_ms_since(s_t_enter_us, s_t_storage_us);

    /* 观测任务: 先置 alive 再建任务 ⇒ msc_enum_watch_stop() 不会因为"任务还没起来"
     * 就提前返回, 而让它在 TinyUSB deinit 之后继续跑。 */
    s_enum_ms = -1;
    s_enum_watch_run   = true;
    s_enum_watch_alive = true;
    if (xTaskCreateWithCaps(msc_enum_watch_task, "msc_enum", 2560, NULL, 2, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_enum_watch_run = s_enum_watch_alive = false;
        ESP_LOGD(TAG, "枚举观测任务创建失败 (仅少一条时序记录, 不影响挂载)");
    }
    return ESP_OK;
}

/* 关 USB: 删 storage → 停观测 → 退模式 → 恢复控制台 → (必要时)补装串口驱动。 */
/* ── 卸载阶段的专门错误码 (与 esp_err_t 空间不冲突, 用负数低段) ──────────── */
#define MSC_ERC_DEFERRED  (-6)   /* 主机延迟写未在期限内排空 ⇒ 拒绝销毁 storage/退 USB */

/* 关闭 USB。
 * 返回 ESP_OK        = storage 已销毁、TinyUSB 已退, 可以收卡;
 *       MSC_ERC_DEFERRED = 主机仍在写(延迟写没排空), **USB 与卡都原样保留**, 调用方
 *                          必须中止本次卸载并保持 USB 挂载状态, 稍后重试;
 *       其它 esp_err_t = usb_composite_exit() 的失败(STUCK/RECOVER_FAIL 等),
 *                          TinyUSB 归属不确定, 调用方按 usb_hid_tinyusb_gone() 决定。
 *
 * ★★ 2026-11-25 死机根因修复 ★★
 * 重构前: delete_storage 失败(延迟写未完)只打一行 LOGD 就把 s_storage 置 NULL,
 *   然后照常 usb_composite_exit → 强杀 TinyUSB 任务 → 未刷的写丢失, LUN/_scard
 *   残留, 紧接着本地 FatFs 重挂同一张卡 ⇒ 主机与本机同写/SDMMC 半事务 ⇒ 重启。
 * 现在: 失败即**整体中止**, 句柄/USB/卡一样都不动, 保持电脑侧 U 盘继续可用
 *   (用户可正常弹出后再点卸载)。延迟写是瞬时状态, 有界重试等它自然排空。 */
static esp_err_t msc_usb_close(void)
{
    const int64_t t0 = esp_timer_get_time();

    /* ① 销毁 storage —— 必须先等延迟写排空; 只有成功才清句柄。 */
    if (s_storage) {
        esp_err_t r = tinyusb_msc_delete_storage(s_storage);
        for (int i = 0; i < MSC_DEL_RETRY_N && r == ESP_ERR_INVALID_STATE; i++) {
            vTaskDelay(pdMS_TO_TICKS(MSC_DEL_RETRY_MS));
            r = tinyusb_msc_delete_storage(s_storage);   /* 失败不改状态, 可安全重试 */
        }
        if (r != ESP_OK) {
            /* 主机仍在写或 storage 状态异常 ⇒ 整体中止, 一样都不销毁。
             * s_storage 保留 ⇒ 下次卸载/重启可重试, 不会泄漏 LUN 或丢写。 */
            s_diag.stop_ms = msc_ms_since(t0, esp_timer_get_time());
            s_diag.exit_rc = MSC_ERC_DEFERRED;
            ESP_LOGE(TAG, "[MSC] 延迟写未排空/删 storage 失败(%s, 重试 %u×%ums) ⇒ "
                          "中止卸载: USB 与卡均保持原状, 请在电脑上弹出 U 盘后重试",
                     esp_err_to_name(r), MSC_DEL_RETRY_N, MSC_DEL_RETRY_MS);
            return MSC_ERC_DEFERRED;
        }
        s_storage = NULL;
    }

    msc_enum_watch_stop();          /* 必须在 deinit 之前: 它还在调 tud_mounted() */

    esp_err_t ex = usb_composite_exit();
    svc_log_console_suspend(false); /* 恢复日志出口(能否写由主机枚举决定, 见下) */

    if (ex == ESP_OK)
        ESP_LOGI(TAG, "[MSC] USB 已退出, Serial/JTAG 已恢复");
    else
        ESP_LOGE(TAG, "[MSC] USB 已退出, 但串口恢复失败(%s) ⇒ 状态已保留可重试",
                 esp_err_to_name(ex));

    /* 串口补装: 只在"TinyUSB 已确认卸净但驱动没回来"(RECOVER_FAIL)时补一次。
     * ★ STUCK(TinyUSB 可能还占 PHY)时**绝不**走这里 —— 重复 usb_composite_exit
     *   会在半拆状态下再调 tinyusb_driver_uninstall, 与未完成的 tusb_write_func 并发。 */
    if (ex != ESP_OK && usb_hid_tinyusb_gone() &&
        !usb_serial_jtag_is_driver_installed()) {
        ESP_LOGW(TAG, "[MSC] TinyUSB 已卸净但 Serial/JTAG 驱动不在位 ⇒ 补装一次");
        esp_err_t re = usb_composite_exit();   /* RECOVER_FAIL 分支: 只装 SJ, 不重卸 TinyUSB */
        if (re == ESP_OK) {
            ESP_LOGI(TAG, "[MSC] 串口驱动已补装成功");
            ex = re;                            /* 诊断记最终结果, 不谎报失败 */
        } else {
            ESP_LOGE(TAG, "[MSC] 串口驱动补装仍失败(%s) ⇒ 状态保留, 下次退出会再试",
                      esp_err_to_name(re));
        }
    }

    s_diag.stop_ms = msc_ms_since(t0, esp_timer_get_time());
    s_diag.exit_rc = (int32_t)ex;
    s_diag.enum_ms = s_enum_ms;
    return ex;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 五、流程实现 (只在会话任务内运行, 单线程 ⇒ 不需要任何附加互斥)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── "卡收不回来"的专门错误码 (与 sd_scan 的 -1..-4 不冲突) ───────────────── */
#define MSC_ERC_USB_ALIVE  (-5)   /* TinyUSB 未确认卸净 ⇒ 不能把卡挂回本机 */

/* 把卡收回本机。返回 0 = 已挂回; 否则为错误码(负数)。
 * 锁等待有界: 8s → 4s → 兜底释放所有权 1s。全程不"硬来" —— 拿不到锁就不碰 VFS。 */
static int msc_reclaim_card(void)
{
    /* ★ 安全前置(2026-11-25): TinyUSB 必须**已确认卸净**。
     *   TINYUSB_STUCK 时设备栈可能还在跑, 主机仍可能读写扇区 —— 此时把卡挂回本地
     *   FatFs 就是"两边同时写同一张卡"(目录项/簇链互相踩)。宁可让卡暂时不可本机访问,
     *   也不能冒丢数据的风险; 恢复手段是重新挂载(会重走一次卸载)或重启。 */
    if (!usb_hid_tinyusb_gone()) {
        ESP_LOGE(TAG, "[MSC] TinyUSB 未确认卸净 ⇒ 拒绝收回 TF 卡(避免与主机同时写)");
        return MSC_ERC_USB_ALIVE;
    }

    int rc = sd_vfs_reclaim(MSC_LOCK_WAIT_MS);
    if (rc == SD_REMOUNT_EBUSY) {
        ESP_LOGW(TAG, "[MSC] 挂载锁 %ums 未释放 ⇒ 再等 %ums 重试",
                 (unsigned)MSC_LOCK_WAIT_MS, (unsigned)MSC_LOCK_RETRY_MS);
        vTaskDelay(pdMS_TO_TICKS(200));
        rc = sd_vfs_reclaim(MSC_LOCK_RETRY_MS);
    }
    if (rc == SD_REMOUNT_EBUSY) {
        /* 两次都拿不到锁 ⇒ 有别的任务正持锁操作卡。既不能自己去碰 VFS(那才是并发重挂),
         * 也不能把所有权永久挂在"归电脑"上(那样本机永远挂不上)。
         * 只在锁内改所有权/状态标志, 不碰 VFS/FatFs, 之后走已验证的重新探测路径。 */
        rc = sd_usb_ownership_release(MSC_LOCK_RELEASE_MS);
        if (rc == 0)
            ESP_LOGW(TAG, "[MSC] 挂载锁持续被占 ⇒ 所有权已交回本机, 等待重新探测");
        else
            ESP_LOGE(TAG, "[MSC] 挂载锁持续被占且收尾也失败(%d) ⇒ 卡暂不可本机访问", rc);
    }
    return rc;
}

/* 交卡失败/取消后的公共收尾: 让网页共享与 watcher 回到"卡在本机"的常态。 */
static void msc_resume_local(void)
{
    sd_watcher_set_paused(false);
    web_files_resume();
}

static void msc_finish(usbh_msc_state_t st, usbh_msc_outcome_t oc, int err)
{
    st_set(st, oc, err);
    if (err)
        ESP_LOGW(TAG, "[MSC] 流程结束: %s / %s (err=%d)",
                 usbh_msc_state_str(st), outcome_str(oc), err);
    else
        ESP_LOGI(TAG, "[MSC] 流程结束: %s / %s",
                 usbh_msc_state_str(st), outcome_str(oc));
}

static void msc_do_start(void)
{
    const int64_t t0 = esp_timer_get_time();
    s_t_start_us = t0;
    msc_diag_begin();

    /* ① 交卡前必须让网页文件服务清场: 浏览器正在下载/上传时, httpd handler 手里
     *    握着 /sdcard 上的 FILE*, 交卡会 free 掉 VFS 上下文 ⇒ 下一次 fread/fwrite
     *    就是悬垂指针(这是"电脑一访问就重启"的入口之一)。清不掉就不交卡。 */
    sd_watcher_set_paused(true);
    if (!web_files_quiesce(MSC_WEB_QUIESCE_MS)) {
        msc_resume_local();
        msc_diag_end(0, 0);          /* 未交卡: VFS 阶段空记录 */
        msc_diag_commit();
        msc_finish(USBH_MSC_IDLE, USBH_MSC_OP_START_BUSY, 0);
        return;
    }

    /* ② 还没碰卡: 取消就是纯回退, 不动任何东西 */
    if (msc_take_cancel()) {
        msc_resume_local();
        msc_diag_end(0, 0);
        msc_diag_commit();
        msc_finish(USBH_MSC_IDLE, USBH_MSC_OP_CANCELLED, 0);
        return;
    }

    /* ③ 交卡 (有界等待; 拿不到锁 ⇒ 什么都没改, 直接判失败) */
    int rc = sd_vfs_handoff(MSC_LOCK_WAIT_MS);
    if (rc == SD_REMOUNT_EBUSY) rc = sd_vfs_handoff(MSC_LOCK_RETRY_MS);
    if (rc != 0) {
        ESP_LOGW(TAG, "[MSC] 交卡失败 rc=%d ⇒ 卡仍在本机, 本次挂载中止", rc);
        msc_resume_local();
        msc_diag_end(rc, 0);
        msc_diag_commit();
        msc_finish(USBH_MSC_IDLE, USBH_MSC_OP_START_FAIL_VFS, rc);
        return;
    }

    /* ④ 交卡后、起 USB 前再看一次取消: 用户等不及点了取消, 这里收手(不启 USB) */
    if (msc_take_cancel()) {
        int rr = msc_reclaim_card();
        msc_resume_local();
        msc_diag_end(rr, 0);
        msc_diag_commit();
        msc_finish(rr == 0 ? USBH_MSC_IDLE : USBH_MSC_FAULT, USBH_MSC_OP_CANCELLED, rr);
        return;
    }

    /* ⑤ 起 USB */
    esp_err_t e = msc_usb_open();
    if (e != ESP_OK) {
        /* 卡已交出但 USB 没起来 ⇒ 必须收回, 不能留在"归电脑但没人用"的状态。
         * 若连回滚都没能把 TinyUSB 卸干净, reclaim 会拒绝收卡(见 msc_reclaim_card),
         * 这时如实报 STUCK, 让用户重启 —— 绝不在 PHY 归属不明时挂本地 FatFs。 */
        int rr = msc_reclaim_card();
        msc_resume_local();
        usbh_msc_outcome_t oc = (rr == MSC_ERC_USB_ALIVE) ? USBH_MSC_OP_STOP_USB_STUCK
                                                          : USBH_MSC_OP_START_FAIL_USB;
        msc_diag_end(rr, 0);
        msc_diag_commit();
        msc_finish(rr == 0 ? USBH_MSC_IDLE : USBH_MSC_FAULT, oc, (int)e);
        return;
    }

    st_set(USBH_MSC_USB, USBH_MSC_OP_NONE, 0);
    ESP_LOGI(TAG, "[MSC] 已挂载到电脑 (%lldms, 卡归电脑; 串口已暂停)",
             (long long)((esp_timer_get_time() - t0) / 1000));

    /* ⑥ 起 USB 期间才到达的取消 ⇒ 立刻走正常卸载路径(不假装没挂过) */
    if (msc_take_cancel()) {
        ESP_LOGW(TAG, "[MSC] 挂载完成时已请求取消 ⇒ 立即卸载");
        msc_do_stop();
    }
}

static void msc_do_stop(void)
{
    const int64_t t0 = esp_timer_get_time();

    st_set(USBH_MSC_STOPPING, USBH_MSC_OP_NONE, 0);   /* 幂等: request_stop 已置过 */

    const int64_t tc0 = esp_timer_get_time();
    esp_err_t cret = msc_usb_close();
    const int64_t tc1 = esp_timer_get_time();

    /* ★★ 延迟写未排空 ⇒ 整体中止卸载, 保持 USB 挂载 + 卡归电脑 ★★
     *   不能收卡(主机还在写)、不能 resume web(卡不在本机)、不能弹"已卸载"。
     *   状态回到 USB, 用户可正常在电脑上弹出 U 盘后再点卸载; 本次不写 NVS
     *   (TinyUSB 仍活跃, 写 flash 关 cache 会打断它 → 重启)。 */
    if (cret == MSC_ERC_DEFERRED) {
        msc_diag_end(MSC_ERC_DEFERRED, 0);
        ESP_LOGW(TAG, "[MSC] 卸载中止耗时 %lldms: 主机延迟写未完, U 盘保持挂载, 请稍后重试",
                 (long long)((tc1 - tc0) / 1000));
        msc_finish(USBH_MSC_USB, USBH_MSC_OP_STOP_DEFERRED, MSC_ERC_DEFERRED);
        return;
    }
    ESP_LOGI(TAG, "[MSC] USB 已关闭 %lldms (接下来确认 PHY 归属并恢复本地存储)",
             (long long)((tc1 - tc0) / 1000));

    /* ★ 先确认 TinyUSB 真的卸干净了, 再谈"卸载成功"。
     *   否则状态会骗人: 界面弹"已卸载", 实际上设备栈还在跑、主机还在读写扇区,
     *   而本机又不能挂载(所有权仍归电脑)。宁可如实报 FAULT 让用户重启。 */
    if (!usb_hid_tinyusb_gone()) {
        msc_resume_local();     /* watcher/网页共享恢复常态(卡不可用, 它们自己会被仲裁拒掉) */
        msc_diag_end(MSC_ERC_USB_ALIVE, -1);
        msc_diag_commit();
        msc_diag_report_async();
        ESP_LOGE(TAG, "[MSC] TinyUSB 未确认卸净 ⇒ 卡仍归电脑, 本机暂不可访问 "
                      "(重新挂载会重试卸载, 否则请重启)");
        msc_finish(USBH_MSC_FAULT, USBH_MSC_OP_STOP_USB_STUCK, MSC_ERC_USB_ALIVE);
        return;
    }

    /* ★ 状态切到 RESTORING == 用户视角的"卸载已完成"(USB 侧确实断了)。
     *   UI 在这一刻关弹窗/给提示, 不再被后面可能很慢的 VFS 重挂拖住 ——
     *   这就是"电脑上已卸载、机器上弹窗不关"那个 bug 的结构性根治。 */
    st_set(USBH_MSC_RESTORING, USBH_MSC_OP_NONE, 0);

    const int64_t tv0 = esp_timer_get_time();
    int rc = msc_reclaim_card();
    msc_resume_local();
    const int vfs_ms = (int)((esp_timer_get_time() - tv0) / 1000);

    msc_diag_end(rc, vfs_ms);
    msc_diag_commit();
    msc_diag_report_async();

    usbh_msc_outcome_t oc;
    if (rc == 0)                          oc = USBH_MSC_OP_STOP_OK;
    else if (rc == MSC_ERC_USB_ALIVE)     oc = USBH_MSC_OP_STOP_USB_STUCK;
    else                                  oc = USBH_MSC_OP_STOP_DEGRADED;

    /* 卡未挂上 ⇒ 让后台按已验证的探测路径自愈(此刻 USB 已确认放手, 探测不会被 s_usb_owned 拦)。 */
    if (rc != 0) sd_probe_async();

    const int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "[MSC] 卸载耗时: 停USB %lldms | 本地VFS %dms/rc=%d | 合计 %lldms",
             (long long)((tc1 - t0) / 1000), vfs_ms, rc,
             (long long)((t1 - t0) / 1000));

    msc_finish(rc == 0 ? USBH_MSC_IDLE : USBH_MSC_FAULT, oc, rc);
}
