/**
 * @file main.c
 * @brief ESP32-S3-RLCD-4.2 BBK 游戏模拟器 - 菜单系统
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sd_scan.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_system.h"   /* ESP_RST_DEEPSLEEP: 深睡唤醒恢复原页面判定 */
#include "nvs_flash.h"
#include "esp_event.h"

#include "st7305.h"
#include "bbk_boot.h"
#include "input.h"
#include "gam4980_emu.h"
#include "user_config.h"
#include "sd_scan.h"
#include "bt_manager.h"
#include "audio_player.h"
#include "board_battery.h"
#include "book_reader.h"

/* page_settings.c 提供: 开机应用暗黑模式(面板反色) */
void settings_apply_dark(st7305_handle_t *lcd);

#include "app_board.h"
#include "board_rlcd.h"   /* board_shim_set_lcd: 绑定主屏, 供 NES/GBC/文曲星/ArduBoy 引擎使用 */
#include "usb_bt.h"        /* usb_bt_is_active / usb_bt_start */
#include "usb_net.h"       /* usb_net_is_active / usb_net_start */
#include "remcon.h"        /* 串口远程控制台 (远程自动化测试/排障) */
#include "tone_player.h"   /* V1.0.68: 方波直驱音调播放器 (开机音/按键音) */
#include "engine_manager.h" /* V1.0.98: 主菜单定时复核引擎残留 (unload_all 幂等) */
#include "vibrator.h"      /* V1.0.76: 苹果 Taptic Engine 震动马达 (GPIO2) */
#include "self_test.h"
#include "display_test.h"
#include "esp_attr.h"
#include "driver/gpio.h"     /* 软关机唤醒防误触: 读 PWR 电平判定是否仍按住 */

static const char *TAG = "BBK";
static st7305_handle_t g_lcd;
/* V1.5.x: 本次启动是否"软关机(深度休眠)唤醒开机" — 软开机须在电源键按下瞬间立即震动,
 * 不再等进桌面才震 (见 app_main 震动块 / os_boot_shell 桌面震动). */
static bool s_soft_boot = false;
/* 电量提示状态 (用户规则: 低电只提示一次不关机; 未接电量模块仅提示一次不关机):
 *  -1=未评估  0=已提示过低电  1=已提示未检测到电量 */
static int s_bat_low_remind = -1;
static uint32_t s_bat_check_ms = 0;   /* 电量评估节流时间戳 (每 10 秒读一次, 省电) */
/* 20260812: 菜单状态移到 PSRAM (UI 数据均在普通任务上下文访问, PSRAM 安全),
 * 腾出 ~33KB 内部 RAM 给引擎/蓝牙/DMA. */

/* === V1.0.90: 自动保存日志到 TF 卡 (排查随机重启/内存问题).
 * 通过 esp_log_set_vprintf 接管日志: 串口照常打印, 同时按行缓存进内部缓冲,
 * 再由主循环 log_drain() 追加写入 /sdcard/log/bbk.log. 只在 SD 挂载后写盘,
 * 失败静默跳过; 文件超过上限后从 0 回绕重建, 避免无限膨胀.
 * s_log_skip 用于多任务/写盘期间的重入保护, 防止日志递归写盘死循环. */
#define LOG_SAVE_BUF       4096
#define LOG_SAVE_MAX       (1024UL * 1024)   /* 单个日志文件上限 1MB */
#define LOG_SAVE_PATH      "/sdcard/log/bbk.log"
EXT_RAM_BSS_ATTR static char          s_log_buf[LOG_SAVE_BUF];
static volatile size_t    s_log_len = 0;
static volatile bool      s_log_skip = false;
/* 多任务日志互斥 (蓝牙/音频/WiFi/主循环等任务都会打日志):
 * log_to_file_vprintf 与 log_drain 对 s_log_buf/s_log_len 的并发访问用互斥锁保护,
 * 避免 vsnprintf/memmove 交错写坏缓冲. 无 ISR 场景 (确认无 IRAM 日志), 普通 mutex 即可. */
static SemaphoreHandle_t  s_log_lock = NULL;
/* 挂载到电脑期间: 暂停串口控制台输出 (Serial/JTAG 已卸载, 只保留文件日志),
 * 从根源避免日志写向已失效串口导致崩溃. 见 usbh_msc 挂载流程. */
static volatile bool      s_log_console_suspend = false;

void log_console_suspend(bool suspend) { s_log_console_suspend = suspend; }

static int log_to_file_vprintf(const char *fmt, va_list arg) {
    int n = 0;
    /* 1) 串口照常输出 (保留原有监视能力); 挂载期间暂停避免写已卸载串口 */
    if (!s_log_console_suspend) {
        va_list arg_console;
        va_copy(arg_console, arg);
        n = vprintf(fmt, arg_console);
        va_end(arg_console);
    }
    /* 2) 只缓存到文件缓冲, 不阻塞串口输出 (互斥锁保护多任务并发写) */
    if (s_log_lock && xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (!s_log_skip && s_log_len < LOG_SAVE_BUF) {
            s_log_skip = true;
            va_list arg_file;
            va_copy(arg_file, arg);
            int k = vsnprintf(s_log_buf + s_log_len, LOG_SAVE_BUF - s_log_len,
                              fmt, arg_file);
            va_end(arg_file);
            if (k > 0) {
                size_t need = (size_t)k;
                if (need >= LOG_SAVE_BUF - s_log_len)
                    need = LOG_SAVE_BUF - s_log_len - 1;
                s_log_len += need;
            }
            s_log_skip = false;
        }
        xSemaphoreGive(s_log_lock);
    }
    return n;
}

/* 主循环每帧调用: 把缓冲内容追加写入日志文件. 仅 SD 挂载且缓冲非空时写盘. */
static void log_drain(void) {
    if (!sd_is_mounted()) return;          /* 无卡/未挂载: 保留缓冲稍后再写 */
    if (!s_log_lock) return;
    if (xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;   /* 写盘竞争让位 */
    s_log_skip = true;
    size_t len = s_log_len;
    if (len == 0) { s_log_skip = false; xSemaphoreGive(s_log_lock); return; }
    /* 超限回绕: 重建文件保留最新内容 */
    struct stat st;
    if (stat(LOG_SAVE_PATH, &st) == 0 &&
        st.st_size + (off_t)len > (off_t)LOG_SAVE_MAX) {
        remove(LOG_SAVE_PATH);
    }
    FILE *f = fopen(LOG_SAVE_PATH, "ab");
    if (f) {
        fwrite(s_log_buf, 1, len, f);
        fclose(f);
    }
    /* 移走已写部分, 保留写盘期间新追加的日志 */
    if (s_log_len >= len) s_log_len -= len; else s_log_len = 0;
    if (s_log_len && len) memmove(s_log_buf, s_log_buf + len, s_log_len);
    s_log_skip = false;
    xSemaphoreGive(s_log_lock);
}

/* V1.0.68: 列表弹窗跟手拖动状态 (番茄钟时间列表等) — 拖动逻辑已下沉 input 层,
 * 本文件不再持有跨帧拖动状态 (V1.0.99 起由 input_get_touch_pos 统一锁存). */

/* V1.0.90: 底部屏蔽带 (仅用于返回/退出, 禁止拖动页面).
 * 手指从屏幕底部中间约 20px (y>=280, x∈[100,300]) 内按下开始,
 * 整个手势期间任何页面都不会被拖动; 点击/确认等其它操作照常, 向上滑触发返回.
 * 需要锁存"本次按下起点是否在屏蔽带内", 手指抬起后清除. */
#define TOUCH_SHIELD_Y      280   /* 距屏幕底部约 20px (屏幕总高 300) */
/* V1.0.99: 屏蔽带判定下沉 input_touch_in_bottom_zone (本文件旧封装
 * touch_shield_blocks_drag 已移除, 各拖动分支直接调用 input 层查询). */

#if CONFIG_OS
/* ============================================================================
 * 新模块化 OS 启动路径 (components/os)
 * 硬约束: CONFIG_OS=1 走新 os 内核, 否则 (默认) 回退旧 menu_system.
 * 本路径只做"外壳"职责: 平台状态挂钩注入 + 硬件延后初始化 + 主循环喂 input.
 * ========================================================================== */
#include "os.h"
#include "os_hw.h"
#include "os_mgr.h"
#include "wallpapers.h"
#include "bt_manager.h"
#include "wifi_manager.h"
#include "usb_hid.h"
#include "usb_bt.h"
#include "usb_net.h"

/* ---- 平台状态挂钩 (os 全局服务经此读取系统状态, 解耦 os 与具体组件) ---- */
static uint8_t os_plat_battery(void) {
    board_battery_status_t bat;
    if (board_battery_read(&bat) == ESP_OK) return bat.percent;
    return 255;   /* 未检测到电池哨兵 -> 状态栏显示 "?" */
}
static int  os_plat_volume(void)      { return audio_player_get_volume(); }
static bool os_plat_muted(void)       { return audio_player_is_muted(); }
static bool os_plat_bt_enabled(void)  { return bt_manager_is_ready(); }
static bool os_plat_bt_connected(void){ return bt_manager_is_connected(); }
static bool os_plat_wifi_enabled(void){ return wifi_manager_is_enabled(); }
static bool os_plat_wifi_connected(void){ return wifi_manager_is_connected(); }
static bool os_plat_usb_data(void) {
    /* 常驻复合设备 (MSC+CDC / HID+CDC): USB 已枚举即视为数据口在用 */
    return usb_composite_is_connected();
}
static const os_platform_ops_t s_os_platform = {
    .get_battery      = os_plat_battery,
    .get_volume       = os_plat_volume,
    .is_muted         = os_plat_muted,
    .bt_enabled       = os_plat_bt_enabled,
    .bt_connected     = os_plat_bt_connected,
    .wifi_enabled     = os_plat_wifi_enabled,
    .wifi_connected   = os_plat_wifi_connected,
    .usb_data_active  = os_plat_usb_data,
};

/* 软关机: 清屏后进入深度休眠. 
 * V1.5.x 修复"软关机变重启": 之前 ext1 唤醒源含 BOOT/KEY 三键, deep sleep 后任一
 * 按键抖动/松开触发 ANY_LOW 立即唤醒复位 → 表现为自动重启. 软关机仅保留开机键
 * (PWR) 作为唤醒源, 彻底关机直到按开机键才上电.
 * 休眠态断电 USB-Serial(省电); 唤醒=按 PWR(GPIO1) 硬重启, 启动流程自动重建 USB. */
static void do_soft_shutdown(ui_ctx_t *ctx) {
    ESP_LOGI(TAG, "软关机: 清屏并深度休眠 (仅开机键可唤醒)");
    if (ctx->lcd) {
        st7305_clear(ctx->lcd, ST7305_COLOR_WHITE);
        /* 屏保(壁纸)态下门禁开启, 普通 flush 被挡; 用 override 强制刷出白屏 */
        st7305_flush_override(ctx->lcd);
    }
    /* 只保留 PWR 开机键 (GPIO1, 与 input.c BTN_GPIO_PWR 一致) 作为唤醒源,
     * 去掉 BOOT/KEY — 真正软关机, 不再自动重启 */
    esp_sleep_enable_ext1_wakeup_io((1ULL << GPIO_NUM_1), ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}
/* 软关机确认: 长按关机键弹出, 确认=关机 / 10秒超时自动关机 / 返回=取消 */
static bool s_pwr_dlg_open = false;
static void on_power_confirm(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    s_pwr_dlg_open = false;
    if (result == 0) do_soft_shutdown(ctx);
}

/* 新 OS 主循环: input -> os 内核分发(action/touch) -> 管家 -> 渲染 */
static void os_shell_loop(ui_ctx_t *ctx) {
    ESP_LOGI(TAG, "OS 主循环 (60 FPS 模式)");
    uint32_t last_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t last_input_ms = 0;   /* 最近输入时间 (管家空闲检测用) */
    uint32_t last_log_ms = 0;     /* V1.5.x: 日志写 TF 节流 (每 3 秒一次, 省电+延长卡寿命) */
    os_mgr_init();
    /* V1.5.x: 深睡待机唤醒(重启) → 恢复深睡前所在页面(回原位).
     * 深睡前 wp 层已把 os_current_page() 写入 NVS "os_wp/last_page";
     * 放置在全部启动初始化(音频/电池等)之后, 页面 on_enter 的资源已就绪. */
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        nvs_handle_t h;
        int32_t lp = -1;
        if (nvs_open("os_wp", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_i32(h, "last_page", &lp);
            nvs_close(h);
        }
        if (lp >= 0 && lp < OS_PAGE_COUNT && lp != (int32_t)OS_PAGE_MAIN) {
            os_push(ctx, (os_page_t)lp);
            ESP_LOGI(TAG, "深睡唤醒: 恢复原页面 page=%d", (int)lp);
            /* 一次性消费标记 */
            if (nvs_open("os_wp", NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_key(h, "last_page");
                nvs_commit(h);
                nvs_close(h);
            }
        }
    }
    while (1) {
        /* 软关机键 GPIO1 长按 2 秒 → 软关机.
         * 屏保(壁纸)态下直接软关机: 屏保期间 LCD 被门禁独占, 关机确认弹窗
         * 画不出来也点不到(与独立监视任务抢屏), 会陷入死锁/反复重启. 壁纸态
         * 本质已是熄屏待机, 长按关机应真正关机, 无需再确认. */
        if (input_power_should_sleep()) {
            if (os_screensaver_active()) {
                do_soft_shutdown(ctx);   /* 壁纸态: 直接软关机 (不返回) */
            }
            /* V1.3: 关机确认弹窗若被非回调路径 (如长按 HOME 清栈 os_dialog_clear_all)
             * 关闭, s_pwr_dlg_open 会滞留 true 导致电源键软关机失效. 检测到当前已无弹窗时复位. */
            if (s_pwr_dlg_open && os_dialog_depth() == 0) s_pwr_dlg_open = false;
            if (!s_pwr_dlg_open) {
                s_pwr_dlg_open = true;
                os_dialog_confirm_ex(ctx, "\xe5\x85\xb3\xe6\x9c\xba?", 10000, 0, /* 关机? */
                                     on_power_confirm, NULL);
            }
        }
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms - last_log_ms >= 3000) {
            last_log_ms = now_ms;
            log_drain();   /* 缓存日志每 3 秒追加写入 TF 卡一次 */
        }
        os_tick(ctx);  /* 服务 tick + 当前模块 poll */
        os_action_t action = (os_action_t)input_get_action();
        if (action != OS_ACTION_NONE) {
            last_input_ms = now_ms;
            if (action == OS_ACTION_CONFIRM) {
                /* 触摸点击: 先走 hit-test (点哪进哪); 未命中忽略.
                 * 震动策略: 仅当 os_handle_touch 消费成功(命中有效 UI 元素)才振动 —
                 * 点空白/无效区不震无反应(规范). */
                int tx, ty;
                if (input_consume_tap(&tx, &ty)) {
                    if (os_handle_touch(ctx, tx, ty)) vibrator_click();
                } else {
                    os_handle_action(ctx, action);   /* 物理键/手柄确认 */
                }
            } else {
                os_handle_action(ctx, action);
            }
        }
        /* 触摸按下也算输入 (拖动/绘图等持续手势) */
        { int tx, ty; if (input_get_touch_pos(&tx, &ty)) last_input_ms = now_ms; }
        /* 游戏 joypad 活动: 任意游戏界面 (GB/NES/文曲星/ArduBoy) 有键按住即视为活动.
         * 屏保激活时游戏键唤醒 (与桌面一致); 否则刷新空闲计时 → 正在玩不睡, 放下手柄睡. */
        if (input_consume_activity()) {
            if (os_screensaver_active()) os_screensaver_reset();
            else last_input_ms = now_ms;
        }
        /* 屏保进入/渲染/软关机统一由独立监视任务负责(wp_screensaver_supervisor_start),
         * 不随主循环/游戏阻塞而遗漏; 此处不再判定, 避免与监视任务抢进入. */
        /* 管家: 内存/生命周期/电源/健康 (低频节流, 内部裁剪) */
        os_mgr_poll(ctx, now_ms, last_input_ms, os_modal_active(ctx));
        remcon_tick(ctx);   /* 串口远程控制台: 派发 page/key/tap 到 os 内核 */
        os_render(ctx);
        /* 屏保态主循环放慢省一点CPU(壁纸由独立监督任务渲染, 输入唤醒不受影响);
             * 正常/游戏态保持 16ms 高响应. */
        vTaskDelay(pdMS_TO_TICKS(os_screensaver_active() ? 32 : 16));
        if (now_ms - last_ms >= 1000) {
            extern int font_zh_count(void);   /* 每秒带字体绑定状态 (0=绑定失败) */
            sdmmc_card_t *sd = sd_get_card(); /* SD 卡型号/容量 (确认挂载与卡型) */
            ESP_LOGI(TAG, "STACK main_free=%u 内部=%u PSRAM=%u zh=%d SD=%s %uMB",
                     (unsigned)uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     font_zh_count(),
                     sd ? (const char *)sd->cid.name : "-",
                     sd ? (unsigned)(sd->csd.capacity / 2048) : 0);
            /* 电量提示 (用户规则): 低电只提示一次不关机; 未接电量模块(percent==255)仅提示一次不关机.
             * 5% 关机由壁纸监督任务处理, 此处仅负责"提醒". 每 10 秒评估一次省电. */
            if (s_bat_low_remind != 0 && (now_ms - s_bat_check_ms >= 10000)) {
                s_bat_check_ms = now_ms;
                board_battery_status_t bat;
                if (board_battery_read(&bat) == ESP_OK) {
                    bool low = bat.percent != 255 && bat.percent <= 20;
                    if (bat.percent == 255 && s_bat_low_remind != 1) {
                        s_bat_low_remind = 1;   /* 未接模块: 仅提示一次 */
                        os_dialog_toast(ctx, "\xe6\x9c\xaa\xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0\xe7\x94\xb5\xe9\x87\x8f\xef\xbc\x88\xe4\xb8\x8d\xe6\x8c\x82\xe6\x9c\xba\xef\xbc\x89"); /* 未检测到电量（不关机） */
                        ESP_LOGW("BAT", "未检测到电量模块, 仅提示 (不关机)");
                    } else if (low && s_bat_low_remind != 0) {
                        s_bat_low_remind = 0;   /* 低电: 提示一次 */
                        os_dialog_toast(ctx, "\xe7\x94\xb5\xe9\x87\x8f\xe4\xbd\x8e\xef\xbc\x8c\xe5\x85\x88\xe5\x85\x85\xe7\x94\xb5"); /* 电量低，先充电 */
                        ESP_LOGW("BAT", "电量低 %d%%, 请充电 (不关机)", (int)bat.percent);
                    }
                }
            }
            last_ms = now_ms;
        }
    }
}

/* 新 OS 启动 (初始化 + 进桌面 + 主循环, 不返回) */
static void os_boot_shell(st7305_handle_t *lcd, app_board_t *board) {
    ui_ctx_t ctx;
    os_init(&ctx, lcd);
    remcon_init();           /* 串口远程控制台 (USB Serial/JTAG 收命令, 远程自动化测试) */
    board_shim_set_lcd(lcd);   /* 绑定主屏到 board_shim: NES/GBC/文曲星/ArduBoy 引擎前置 (board_rlcd_is_initialized) */
    os_platform_set(&s_os_platform);
    /* 统一进桌面. USB-BT 激活时: 蓝牙适配器后台运行(状态栏显示图标), 不进 USB-BT 页;
     * 需关闭时到 应用管家 → 运维 → USB-BT. */
    os_push(&ctx, OS_PAGE_MAIN);
    /* 字体启动自检: 主动触发 appdata 字库绑定, 打印 FONTP/FONTZH 日志 (排查字体 X) */
    {
        extern int font_zh_count(void);
        extern int font_zh16_count(void);
        ESP_LOGI("FONT", "启动自检: zh_count=%d zh16_count=%d",
                 font_zh_count(), font_zh16_count());
    }
    ESP_LOGI("MEM", "[MEM] OS 桌面基线: 内部空闲=%u PSRAM空闲=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* 延后初始化 (避免抢占内部 DMA RAM): 音频 -> 电池.
     * V1.5.x 省电: 蓝牙按需开启 — 开机不再初始化蓝牙 (进蓝牙/手柄设置页时由 os_hw 触发). */
    app_board_init_audio(board);
    app_board_init_battery(board);
    if (usb_bt_is_active()) {
        /* USB-BT 激活 (开机早期进 BTH 模式): 蓝牙控制器供 USB 蓝牙适配器独占,
         * 不启动手柄蓝牙, 直接初始化 BTH. 这是官方 usb_dongle 方式(开机早期进),
         * 避免运行中切换导致 USB 枚举失败. */
        usb_bt_start();
        ESP_LOGI(TAG, "USB-BT 模式已启动 (BTH 蓝牙适配器运行中)");
    } else if (usb_net_is_active()) {
        /* USB 网卡共享激活: 设备枚举为 RNDIS 网卡, 借 WiFi(STA) 上行共享上网.
         * WiFi 扫描/连接由 usb_net_start 内部按需处理, 不启动手柄蓝牙. */
        usb_net_start();
        ESP_LOGI(TAG, "USB 网卡共享模式已启动");
    } else {
        /* 开机短暂自动回连: 有配对手柄历史 → 短窗口(30s)低占空比直连上次手柄;
         * 连上保持; 手柄不在则窗口超时自动关栈释放(省内部RAM). 无历史 → 不开.
         * 需要时(进蓝牙/手柄页)由 os_hw_request 首次启用. */
        bt_manager_start_boot_reconnect(30000);
    }
    /* V1.0.72: 开机不播旋律, 改为震动一下提示.
     * 原因: tone_mel 任务栈(8KB)在首次 feed_pcm → audio_out_start →
     * es8311_start/set_sample_rate 的 I2C 深链路爆栈 panic, 导致开机无限重启.
     * (tone_mel/tone_cont 栈已 2048→4096 words 加固, 开机音仍可随时手动触发) */
    /* V1.5.x: 软关机唤醒开机已在上电瞬间震过, 进桌面不再重复震 */
    if (!s_soft_boot) vibrator_tap(80, 120);
    os_shell_loop(&ctx);
}
#endif /* CONFIG_OS */

void app_main(void)
{
    /* V1.0.90: 自动保存日志到 TF (串口照常打印, 同时缓存进 /sdcard/log/bbk.log) */
    s_log_lock = xSemaphoreCreateMutex();   /* 多任务日志互斥锁 (创建失败则跳过文件缓存) */
    esp_log_set_vprintf(&log_to_file_vprintf);
    ESP_LOGI(TAG, "==== BBK 游戏模拟器启动 ====");

    /* V1.7.x: 统一北京时间时区 (东八区). 状态栏/日期/校时后显示都用 localtime → 北京时间 */
    setenv("TZ", "UTC-8", 1);
    tzset();

    /* 默认时间 2026-09-01: 设备未联网校时(无真实时间, 早于2025)时用该默认值做日期兜底;
     * 联网后 wifi_manager 的 SNTP 会自动校时覆盖 (今天的真实时间晚于该默认, 故不冲突). */
    {
        time_t t0 = time(NULL);
        if (t0 < 1735689600) {   /* 2025-01-01 00:00 UTC 之前 = 未校时/初始 1970 */
            struct timeval tv = { 1788220800, 0 };   /* 2026-09-01 00:00 UTC */
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "未联网校时, 设默认时间 2026-09-01");
        }
    }

    /* V1.0.68: 判断本次启动来源 (deep sleep 唤醒 = 电源键开机) */
    {
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        /* 关机时间到且无人唤醒: 深睡定时器唤醒 → 转"彻底关机", 只能 PWR 键开机. */
        if (cause == ESP_SLEEP_WAKEUP_TIMER) {
            ESP_LOGW(TAG, "关机时间到, 无人唤醒 → 彻底关机 (仅 PWR 键可开机)");
            esp_sleep_enable_ext1_wakeup_io((1ULL << GPIO_NUM_1), ESP_EXT1_WAKEUP_ANY_LOW);   /* 不留定时器 */
            esp_deep_sleep_start();   /* 不返回 */
        }
        if (cause == ESP_SLEEP_WAKEUP_EXT1) {
            ESP_LOGI(TAG, "电源键唤醒 (软开机)");
            /* V1.5.x: 软关机开机须长按 0.5s — 防误触开机.
             * deep sleep 的 ext1 唤醒在按键按下瞬间即触发, 无法硬件限定时长.
             * 此处软件把关: 唤醒后若 PWR(GPIO1) 此刻已松开 (点一下就放开 = 误触,
             * 按住时长不足 0.5s), 立即再次深度休眠; 仍按住则正常开机 (按住 0.5s 即
             * 开机, 无需等松手 — 系统启动过程本身已满足按住时间). */
            uint32_t t0 = esp_timer_get_time() / 1000;
            /* 软件把关 500ms 长按: 唤醒由按下瞬间触发, 从 t0 起须连续按住满 500ms
             * 才放行开机. 期间一旦松开 (按住不足 0.5s = 误触点按) → 立即回睡.
             * 一直按住不放 → 满 500ms 正常开机 (无需等松手). */
            while ((esp_timer_get_time() / 1000 - t0) < 500) {
                if (gpio_get_level(GPIO_NUM_1) != 0) {   /* 已松开 (<500ms) → 误触 */
                    ESP_LOGI(TAG, "开机键按住不足0.5s (误触), 重新休眠");
                    esp_sleep_enable_ext1_wakeup_io((1ULL << GPIO_NUM_1), ESP_EXT1_WAKEUP_ANY_LOW);
                    esp_deep_sleep_start();   /* 不返回 */
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            /* 连续按住满 500ms → 正常开机 */
            ESP_LOGI(TAG, "开机键按住≥0.5s, 正常开机");
            s_soft_boot = true;   /* 软关机唤醒开机: 标记, 用于开机瞬间立即震动 */
        }
    }

    /* === 硬件初始化 (app_board 模块化封装, 顺序敏感见 components/app_board) === */
    app_board_t board;
    esp_err_t ret = app_board_init(&board);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "硬件初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    g_lcd = board.lcd;

    /* 暗黑模式: 开机即应用面板反色 (黑↔白, 含开机画面, 全局强制) */
    settings_apply_dark(&g_lcd);

    /* V1.0.76: 震动马达 LEDC 初始化 (幂等, 无硬件也不影响运行) */
    vibrator_init();
    /* V1.5.x: 软关机(深度休眠)唤醒开机 → 电源键按下瞬间立即震动提示.
     * 正常开机(硬上电/USB)仍由 os_boot_shell 进桌面时震动 (见下). */
    if (s_soft_boot) vibrator_tap(80, 120);

    /* V1.0.70: 音调播放器全局初始化 (PCM→I2S, 两板通用).
     * 声音是固件全局功能, 不绑定任何引擎; 开机音/按键音/闹铃随时可播. */
    tone_player_init();

    /* 开机画面: 硬上电/正常开机显示 Logo (1.5秒, 按 A 键跳过);
     * 深睡唤醒(软开机, s_soft_boot)跳过 Logo 直接进桌面 — 唤醒当"回原位"而非重新开机.
     * 说明: 浅睡唤醒是继续运行(不重启), 天然不显示 Logo, 这里只管深睡/关机唤醒. */
    if (!s_soft_boot) {
        st7305_clear(&g_lcd, ST7305_COLOR_WHITE);
        bbk_boot_draw_logo(&g_lcd);
        st7305_flush(&g_lcd);
        /* 快速跳过检测: 50ms 间隔, 最多 30 次 = 1.5s */
        for (int i = 0; i < 30; i++) {
            if (gpio_get_level(BTN_GPIO_A) == 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

#ifdef CONFIG_APP_AUTOTEST
    self_test_run_all();
#endif
#ifdef CONFIG_DISPLAY_REFRESH_TEST
    display_test_quick(&g_lcd);
#endif
    /* 新模块化 OS: 启动并进入主循环 (不返回) — 旧 menu_system 已移除 */
    os_boot_shell(&g_lcd, &board);
}
