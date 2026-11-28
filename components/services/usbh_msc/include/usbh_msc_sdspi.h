#ifndef USBH_MSC_SDSPI_H
#define USBH_MSC_SDSPI_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * USB 存储会话 —— TF 卡"本机 / 电脑"交接的**唯一生命周期所有者**
 *
 * 为什么要有这一层 (2026-11-25 重构):
 *   重构前, 同一段生命周期被 3 个模块各记一套状态:
 *     · page_storage: s_mount_result / s_unmounting / s_vfs_restoring /
 *       s_mount_t0 / s_unmount_t0 / s_vfs_t0 / s_mount_cancel (7 个变量)
 *       + 两个后台任务(mount_worker / unmount_worker) + 三处超时
 *     · usbh_msc:     s_running (与上面那套并行的第二个"是否挂载")
 *     · sd_scan:      s_usb_owned + 仲裁状态 (第三个"卡归谁")
 *   结果就是典型的多标志驱动: 每个新问题都靠再加一个 bool/一个超时解决,
 *   于是出现"界面卡在存储恢复中, 只能重启"、"卸载成功但弹窗不关"、
 *   "超时后放行入口, 后台却还在跑"这类**状态互相矛盾**的故障。
 *
 * 现在: 一个状态机 + 一个后台任务 + 一组有界等待。谁拥有卡由 state 唯一表达,
 *   其它模块只能"请求"和"读快照", 不许自己改状态。
 *
 * 状态与所有权:
 *   IDLE       本机(或未挂载)。允许 start; UI 可显示"挂载到电脑?"。
 *   PREPARING  清场+交卡+起 USB。**卡在交接中**, 本机访问被 sd_arb 拒绝。
 *   USB        USB MSC 已启动, 卡归电脑。允许 stop。
 *   STOPPING   停 USB(删 storage → 退模式 → 恢复串口)。
 *   RESTORING  卡已回本机, 正在重挂 VFS(= 用户视角"卸载已成功, 存储恢复中")。
 *   FAULT      USB 已退, 本机 VFS 没恢复。卡未挂载, 允许 start(重试) 或等探测。
 *
 * 转换 (箭头上的条件是唯一入口, 其它组合一律拒绝并返回 false):
 *   IDLE/FAULT ──request_start──> PREPARING ──成功──> USB
 *                                    │  ├─清场失败/交卡失败──> IDLE     (outcome 说明原因)
 *                                    │  └─取消──────────────> IDLE     (outcome=CANCELLED)
 *   USB ──request_stop──> STOPPING ──> RESTORING ──成功──> IDLE
 *                                            └──失败/锁超时──> FAULT
 *
 * 生产者约束: 请求函数**只能由 UI 任务(单生产者)**调用 —— 这是刻意的简化,
 *   避免在请求路径里再引入一把跨任务锁。后台只有一个消费者(会话任务)。
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    USBH_MSC_IDLE = 0,
    USBH_MSC_PREPARING,
    USBH_MSC_USB,
    USBH_MSC_STOPPING,
    USBH_MSC_RESTORING,
    USBH_MSC_FAULT,
} usbh_msc_state_t;

/* 最近一次"结束"的操作结果 —— 只用于 UI 决定提示文案, 不参与状态判断。
 * 进入 USB 是稳定态, 不产生 outcome(UI 看 state 就知道)。 */
typedef enum {
    USBH_MSC_OP_NONE = 0,
    USBH_MSC_OP_START_FAIL_VFS,   /* 本地 VFS 卸载失败 ⇒ 卡仍在本机, 未交卡 */
    USBH_MSC_OP_START_FAIL_USB,   /* USB 侧启动失败 ⇒ 已把卡收回本机 */
    USBH_MSC_OP_START_BUSY,       /* 网页共享正在传文件 ⇒ 未交卡 */
    USBH_MSC_OP_CANCELLED,        /* 挂载过程中取消 ⇒ 已把卡收回本机 */
    USBH_MSC_OP_STOP_OK,          /* 卸载完成, 本机存储已恢复 */
    USBH_MSC_OP_STOP_DEGRADED,    /* 卸载完成(USB 已退), 但本机存储恢复失败 ⇒ state=FAULT */
    USBH_MSC_OP_STOP_USB_STUCK,   /* ★ USB 没退干净(TinyUSB 卸载未确认) ⇒ 卡仍归电脑, 需重启 */
    USBH_MSC_OP_STOP_DEFERRED,    /* ★ 主机延迟写未排空 ⇒ 本次卸载中止, U 盘保持挂载, 可重试 */
} usbh_msc_outcome_t;

/* 无锁快照: 一次读完 state/outcome/seq/since, 不会读到"半个状态"。
 * seq 每次 (state 或 outcome) 变化 +1; UI 用它判断"自打开弹窗以来有没有新事件",
 * 因此**不需要**在界面层再存一份"我上次看到什么"。 */
typedef struct {
    usbh_msc_state_t   state;
    usbh_msc_outcome_t outcome;
    uint32_t           seq;         /* 单调递增; 0 = 开机以来从未变过 */
    uint32_t           since_ms;    /* 进入当前状态的时刻 (系统 uptime, ms) */
    int                err;         /* 失败时的原始错误码 (esp_err_t 或 sd_scan 负数) */
} usbh_msc_status_t;

usbh_msc_status_t usbh_msc_status(void);
const char       *usbh_msc_state_str(usbh_msc_state_t st);

/* 卡现在是否不在本机手里 (PREPARING/USB/STOPPING/RESTORING) ——
 * 格式化、sd_mount 之外的"要独占卡"的动作必须看这个, 不能只看 state==USB。 */
bool usbh_msc_card_busy(void);

/* 兼容旧调用: 是否已挂载到电脑 (等价 state == USB)。 */
bool usbh_msc_is_running(void);

/* ── 请求 (立即返回, 不阻塞 UI) ─────────────────────────────────────────────
 * 返回 false = 当前状态不接受该请求(例如 USB 中再 start)。调用方按 state 提示即可,
 * **不要**自己再维护"正在挂载"之类的标志。 */
bool usbh_msc_request_start(void);
bool usbh_msc_request_stop(void);
/* PREPARING 中撤销挂载(在阶段边界安全收手); 其它状态下等价 request_stop()。 */
bool usbh_msc_request_cancel(void);

/* ═══ 诊断: MSC 生命周期记录 (仅 RAM, 掉电即失) ═══════════════════════════
 * 为什么**不落 NVS**: nvs_commit() 写 flash 会关双核 cache, 卸载时 SD 刚重挂、
 *   多任务在跑 —— 实测这会把状态机卡在 RESTORING(界面"存储恢复中")或重启。
 *   卸载关键路径里一行 flash 写都不许有, 诊断只留 RAM。
 * 读取: remcon `mscstat` / `mscstat clear`。 */
void usbh_msc_diag_dump(void);
void usbh_msc_diag_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* USBH_MSC_SDSPI_H */
