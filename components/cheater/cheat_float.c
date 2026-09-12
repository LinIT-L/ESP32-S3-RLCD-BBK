/**
 * cheat_float.c — 独立"修改机浮标 + 修改机菜单任务" (引擎无关).
 *
 * 把原来散落在各引擎里的"浮动图标绘制 + 热键/点按呼出 + 菜单冻结"集中到本模块,
 * 并按用户要求把"游戏"与"修改机"彻底分成两个任务:
 *   - 引擎仅在开始/结束时 attach/detach 自己的游戏内存 region(一行), 不再有任何
 *     浮标/热键/菜单代码;
 *   - 显示合成层(与 virtual_keys_draw 同处)每帧调用 overlay_tick(), 由本模块统一
 *     绘制可拖动浮标、检测 SELECT+START 热键或点按浮标;
 *   - 一旦触发, overlay_tick() 仅唤醒下面这个独立修改机任务并**立即返回**(不再阻塞
 *     当前显示任务), 调用方应跳过本帧 flush 把屏幕让出来;
 *   - 独立修改机任务 (prio7, 独占 LCD+输入) 置 active/frozen 后阻塞运行
 *     cheat_ui_open(菜单/搜索/浏览)直至退出, 再交还屏幕;
 *   - 冻结: is_frozen()==is_active() 恒 true → 各引擎模拟任务循环顶检查停走,
 *     显示路径停止画帧/flush, 保证修改机独占期间游戏停止、搜索结果稳定.
 *
 * 安全: 搜索/读写全部经 cheater 的 region.peek/poke, 只覆盖引擎自有的游戏 RAM.
 */
#include "cheat_float.h"
#include "cheat_ui.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdbool.h>

/* 修改机"后台运行"开关 (由 os 组件把持, cheater 不依赖 os 头, 仅声明符号):
 * 默认关闭 → 各引擎 overlay_tick 不显示修改机浮标、不响应呼出;
 * 用户在选择"后台运行"后打开, 修改机才真正启动(浮标/热键可见可用). */
bool os_modtool_is_running(void);

static cheater_region_t s_reg;              /* 拷贝的活跃 region */
static const cheater_region_t *s_regp = NULL;
static cheater_bits_t s_bits = CHEAT_BITS_8;

/* 独立修改机任务: 优先级高于显示视频任务(6), 保证触发后立即接管屏幕.
 * 栈 64KB (PSRAM): 需容纳修改机菜单/搜索/输入的栈帧. */
#define CHEAT_PRIO        7
#define CHEAT_STACK_WORDS 16384
EXT_RAM_BSS_ATTR static StackType_t s_cheat_stack[CHEAT_STACK_WORDS];
static StaticTask_t  s_cheat_tcb;
static TaskHandle_t  s_cheat_task = NULL;
static SemaphoreHandle_t s_open_req = NULL; /* 呼出通知 (唤醒任务) */
static st7305_handle_t *s_lcd = NULL;
static volatile bool s_active = false;       /* 修改机任务独占屏幕/输入期间 */

static void cheat_float_task(void *arg);

bool cheat_float_is_attached(void) { return s_regp != NULL; }

/* 修改机菜单打开期间恒 true: 引擎步进停走 + 显示路径让渡. */
bool cheat_float_is_frozen(void) { return s_active; }
bool cheat_float_is_active(void) { return s_active; }

void cheat_float_attach(const cheater_region_t *reg, cheater_bits_t bits)
{
    if (reg) {
        s_reg = *reg;
        s_regp = &s_reg;
        s_bits = bits;
    } else {
        s_regp = NULL;
    }
    /* 惰性创建独立修改机任务 + 通知信号量 (首次 attach 时一次性) */
    if (!s_open_req) {
        s_open_req = xSemaphoreCreateBinary();
        if (s_open_req && !s_cheat_task) {
            s_cheat_task = xTaskCreateStatic(cheat_float_task, "cheat_menu",
                                             CHEAT_STACK_WORDS, NULL, CHEAT_PRIO,
                                             s_cheat_stack, &s_cheat_tcb);
        }
    }
    cheat_ui_float_clear();
}

void cheat_float_detach(void)
{
    s_regp = NULL;
    cheat_ui_float_clear();
}

/* 独立修改机任务体: 常驻等待呼出. 唤随后独占屏幕+输入, 跑完整菜单直至退出. */
static void cheat_float_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_open_req, portMAX_DELAY) != pdTRUE)
            continue;
        if (s_active || !s_regp || !s_lcd) continue;   /* 忽略过期/未登记的唤醒 */
        s_active = true;                               /* 冻结模拟 + 显示让渡 */
        cheat_ui_open(s_lcd, s_regp, s_bits);          /* 独占: 菜单/搜索, 直至退出 */
        s_active = false;
        cheat_ui_float_clear();
        /* 清掉残留触发: 本任务已 Take 走当前信号量, 这里把可能存在的多余 Give
         * 消费掉, 让信号量回到空态, 下一次 Give 才能再次唤醒本任务. */
        while (xSemaphoreTake(s_open_req, 0) == pdTRUE) {}
    }
}

/* 唤醒修改机任务 (仅当未激活且已登记). 由 overlay_tick 触发. */
static void cheat_float_request_open(st7305_handle_t *lcd)
{
    if (s_active || !s_regp) return;
    s_lcd = lcd;
    xSemaphoreGive(s_open_req);
}

bool cheat_float_overlay_tick(st7305_handle_t *lcd)
{
    /* 默认不启动修改机: 仅在用户于应用管家选择"后台运行"后才显示浮标/响应呼出. */
    if (!os_modtool_is_running()) return false;
    if (s_active) return false;               /* 修改机任务已独占屏幕, 显示层让渡 */
    if (!s_regp || !lcd) return false;

    /* 画可拖动浮动图标 (触摸可用时才画; 手柄连接时不画). */
    cheat_ui_float_draw(lcd);

    /* 呼出: 热键(SELECT+START) 或 点按浮标. 只唤醒独立任务后立即返回 true,
     * 调用方跳过本帧 flush 把屏幕交给修改机任务, 不再在此阻塞. */
    if (cheat_ui_hotkey() || cheat_ui_float_tick(lcd)) {
        cheat_float_request_open(lcd);
        return true;
    }
    return false;
}