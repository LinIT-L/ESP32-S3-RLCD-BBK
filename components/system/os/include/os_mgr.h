/**
 * os_mgr.h — 系统资源管家 (与 os_hw 平级的服务模块).
 *
 * 性能优先设计 (性能 > 优雅 > 扩展性):
 *   - 不建独立管家任务: 由主循环每帧调用 os_mgr_poll(), 内部按
 *     CONFIG_OS_MGR_POLL_MS 节流. 零任务/零锁/零 IPC, UI 操作直接在
 *     主循环线程执行 (无跨线程竞态); 空闲帧仅两次整数比较, CPU 可忽略.
 *   - 编译期裁剪: 各子功能由 CONFIG_OS_MGR_{MEM,LIFE,POWER,HEALTH} 控制,
 *     未启用整段裁剪, 零代码零内存 (CONFIG_OS_MGR 关闭时全部空实现).
 *   - 无回调表/虚函数/插件机制: 决策用直接 if 分支.
 *   - 双核分配不归管家: 静态亲和性 API 已迁至 os_core (os_core_pin_task /
 *     os_core_current_core), 属系统范围工具, 与资源管家职责分离.
 */
#ifndef OS_MGR_H
#define OS_MGR_H

#include "os.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化: 崩溃计数(NVS) + 打印配置. 主循环启动前调用一次. */
void os_mgr_init(void);

/* 主循环每帧调用 (内部节流). 必须主循环线程.
 *   ctx            ui_ctx (用于自动回主菜单等 UI 动作)
 *   now_ms         当前毫秒
 *   last_input_ms  最近一次输入时间 (外壳记录; 0=未知)
 *   in_dialog      是否有模态弹窗 (传 os_modal_active(ctx)) */
void os_mgr_poll(ui_ctx_t *ctx, uint32_t now_ms, uint32_t last_input_ms, bool in_dialog);

/* ==== 参数 (运行时调整) ==== */
/* 空闲自动回主菜单超时(ms). 0=关闭. 全屏应用/游戏页不打断. */
void os_mgr_set_idle_timeout(uint32_t ms);
/* 内部内存低水位告警阈值(B). 0=关闭. 低于时回收引擎并告警. */
void os_mgr_set_low_mem_warn(uint32_t bytes);

/* ==== 统计查询 (供诊断页/系统信息显示) ==== */
uint32_t os_mgr_mem_internal(void);   /* 内部 RAM 空闲字节 */
uint32_t os_mgr_mem_psram(void);      /* PSRAM 空闲字节 */
uint32_t os_mgr_mem_peak_used(void);  /* 内部 RAM 相对基线峰值使用(字节) */
uint32_t os_mgr_crash_count(void);    /* 累计异常复位次数 (NVS) */

#ifdef __cplusplus
}
#endif

#endif /* OS_MGR_H */
