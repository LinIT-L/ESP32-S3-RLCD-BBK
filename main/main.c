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
#include "svc_log.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_system.h"   /* ESP_RST_DEEPSLEEP: 深睡唤醒恢复原页面判定 */
#include "svc_nvs.h"
#include "esp_event.h"

#include "st7305.h"
#include "board_pins.h"
#include "hw_cfg.h"
#include "esp_bt.h"   /* ★经典蓝牙内存释放 */
#include "nvs_flash.h"
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
#include "espsdr.h"        /* espsdr_reclaim_boot: 频谱仪禁用则回收 64KB 采集区 */
#include "remcon.h"        /* 串口远程控制台 (远程自动化测试/排障) */
#include "tone_player.h"   /* V1.0.68: 方波直驱音调播放器 (开机音/按键音) */
#include "engine_manager.h" /* V1.0.98: 主菜单定时复核引擎残留 (unload_all 幂等) */
#include "vibrator.h"
#include "self_test.h"
#include "display_test.h"
#include "esp_attr.h"
#include "driver/gpio.h"     /* 软关机唤醒防误触: 读 PWR 电平判定是否仍按住 */
#include "apps.h"            /* V1.2 模块化: L4 应用层装配入口 apps_register_all() */

static const char *TAG = "BBK";
static st7305_handle_t g_lcd;
/* 本次启动是否跳过开机 Logo —— 仅"休眠到点自动关机"唤醒时为 true(什么都不显示, 直接进桌面)。
 * 手动关机唤醒 / 冷启动为 false → 正常显示开机 Logo。 */
static bool s_soft_boot = false;
/* 电量提示状态 (用户规则: 低电只提示一次不关机; 未接电量模块仅提示一次不关机):
 *  -1=未评估  0=已提示过低电  1=已提示未检测到电量 */
static int s_bat_low_remind = -1;
static uint32_t s_bat_check_ms = 0;   /* 电量评估节流时间戳 (每 10 秒读一次, 省电) */
/* 20260812: 菜单状态移到 PSRAM (UI 数据均在普通任务上下文访问, PSRAM 安全),
 * 腾出 ~33KB 内部 RAM 给引擎/蓝牙/DMA. */

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
#include "lintos_version.h"   /* ★ 版本管理: 固件版本/构建信息唯一来源 */
#include "lintos_version.h"   /* ★ 版本管理: 固件版本/构建信息 */
#include "os_hw.h"
#include "os_mgr.h"
#include "wallpapers.h"
#include "sleep.h"
#include "power_stat.h"   /* power_stat_init(): 电源统计 (设置→休眠与电源→电池信息) */
#include "bt_manager.h"
#include "wifi_manager.h"
#include "usb_hid.h"
#include "usb_bt.h"
#include "usb_net.h"

/* ---- 平台状态挂钩 (os 全局服务经此读取系统状态, 解耦 os 与具体组件) ---- */
static uint8_t os_plat_battery(void) {
    board_battery_status_t bat;
    /* 60s 缓存采样: 与低电量告警/网页手柄等共用同一个节拍器 (linit 2026-09-20) */
    if (board_battery_read_cached(&bat) == ESP_OK) return bat.percent;
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


/* 关机(深睡, 复位式, 等同真关机) — 两条来源, 唤醒条件不同(见 sleep.h):
 *   MANUAL 手动(长按关机键 → 确认框): 仅 PWR 键唤醒, 且须保持按住 ≥0.5s 才开机
 *   AUTO   自动(休眠时间到点)      : 任意键唤醒, 短按即开机
 * 两者的深睡前动作相同: 先挂起屏保监督任务(PSRAM 栈持续刷屏, 深睡禁缓存瞬间会撞断言),
 * 再交给电源模块停射频 → 清屏 → esp_deep_sleep_start()。 */
static void do_manual_shutdown(ui_ctx_t *ctx) {
    (void)ctx;
    wp_screensaver_halt();
    wp_power_off(WP_OFF_MANUAL);   /* 不返回 */
}
static void do_auto_shutdown(void) {
    wp_screensaver_halt();
    wp_power_off(WP_OFF_AUTO);     /* 不返回 */
}
/* 远程关机 (remcon 命令 shutdown [auto|manual]): 便于实测两条唤醒策略。
 * auto_mode=1 走"休眠到点自动关机"(任意键唤醒, 不显示 Logo); 0 走手动路径。 */
void os_remote_shutdown(int auto_mode) {
    if (auto_mode) do_auto_shutdown();
    else           do_manual_shutdown(NULL);
}
/* 关机确认: 长按关机键弹出, 确认=关机 / 10秒超时自动关机 / 返回=取消 */
static bool s_pwr_dlg_open = false;
static void on_power_confirm(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    s_pwr_dlg_open = false;
    if (result == 0) do_manual_shutdown(ctx);
}

/* 新 OS 主循环: input -> os 内核分发(action/touch) -> 管家 -> 渲染 */
static void os_shell_loop(ui_ctx_t *ctx) {
    ESP_LOGI(TAG, "OS 主循环 (60 FPS 模式)");
    uint32_t last_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t last_input_ms = 0;   /* 最近输入时间 (管家空闲检测用) */
    uint32_t last_log_ms = 0;     /* V1.5.x: 日志写 TF 节流 (每 3 秒一次, 省电+延长卡寿命) */
    os_mgr_init();
    while (1) {
        /* 休眠时间到点 → 自动关机(任意键唤醒)。标志由屏保监督任务置位,
         * 必须在此(内部 RAM 栈)消费并深睡。 */
        if (wp_screensaver_shutdown_requested()) {
            do_auto_shutdown();   /* 不返回 */
        }
        /* 关机键 (PWR) 长按 2 秒 → 手动关机(仅 PWR 键唤醒, 须按住 0.5s 才开机)。
         * 屏保态下 LCD 被壁纸独占, 确认框画不出来 → 直接关机;
         * 桌面态弹"关机?"确认框, 确认 / 10 秒超时 → do_manual_shutdown()。 */
        if (input_power_should_sleep()) {
            if (os_screensaver_active()) {
                do_manual_shutdown(ctx);   /* 不返回 */
            }
            if (!s_pwr_dlg_open) {
                s_pwr_dlg_open = true;
                os_dialog_confirm_ex(ctx, "\xe5\x85\xb3\xe6\x9c\xba?", 10000, 0, /* 关机? */
                                     on_power_confirm, NULL);
            }
        }
        /* 屏保态: 不轮询触摸(碰 I2C/flash)、不跑 os_tick、不写日志、不跑 os_mgr_poll,
         * 屏保画面由独立监督任务渲染。 */
        if (os_screensaver_active()) {
            /* 屏保态**只轮询物理键**(更新空闲时钟 → 监督任务据此"按键唤醒壁纸").
             * touch_blocked=true 时 input_get_action 内部触摸完全短路(不碰 I2C/flash),
             * 物理键仅 gpio 读取+状态机, 无 flash 访问, 安全.
             * 跳过 os_tick/日志/os_mgr_poll/渲染 —— 屏保由独立监督任务渲染. */
            os_action_t act = (os_action_t)input_get_action();
            if (act != OS_ACTION_NONE) {
                last_input_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                os_handle_action(ctx, act);   /* 物理键 → os_screensaver_reset 唤醒壁纸 */
            }
            vTaskDelay(pdMS_TO_TICKS(32));
            continue;
        }
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms - last_log_ms >= 3000) {
            last_log_ms = now_ms;
            svc_log_drain();   /* 缓存日志每 3 秒追加写入 TF 卡一次 */
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
        /* 屏保进入/渲染由独立监视任务负责(wp_screensaver_supervisor_start),
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
                if (board_battery_read_cached(&bat) == ESP_OK) {
                    bool low = bat.percent != 255 && bat.percent <= 20;
                    if (bat.percent == 255 && s_bat_low_remind != 1) {
                        s_bat_low_remind = 1;   /* 未接模块: 仅提示一次 */
                        os_dialog_toast(ctx, "\xe6\x9c\xaa\xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0\xe7\x94\xb5\xe9\x87\x8f\xef\xbc\x88\xe4\xb8\x8d\xe6\x8c\x82\xe6\x9c\xba\xef\xbc\x89"); /* 未检测到电量（不关机） */
                        ESP_LOGD("BAT", "未检测到电量模块, 仅提示 (不关机)");
                    } else if (low && s_bat_low_remind != 0) {
                        s_bat_low_remind = 0;   /* 低电: 提示一次 */
                        os_dialog_toast(ctx, "\xe7\x94\xb5\xe9\x87\x8f\xe4\xbd\x8e\xef\xbc\x8c\xe5\x85\x88\xe5\x85\x85\xe7\x94\xb5"); /* 电量低，先充电 */
                        ESP_LOGD("BAT", "电量低 %d%%, 请充电 (不关机)", (int)bat.percent);
                    }
                }
            }
            last_ms = now_ms;
        }
    }
}

/* V1.6.x 启动提速: 开机 Logo **最短**显示时长 (ms). 与 OS 初始化并行计时:
 * 先画 Logo 上屏(不等待) → 期间做 os_init/页面注册等初始化 → 最后只补等"Logo 至少显示
 * 这么久"。原实现是"固定死等 1.5s(30×50ms)", 实测白占启动 1.18s, 是主循环被拖到 3.4s 的主因。
 * 按 A/BOOT 键仍可随时跳过。想让 Logo 停久一点就调大这个值。
 * V1.7.x: 软关机(深睡)唤醒跳过 Logo, 直接初始化进桌面 (用户要求快速唤醒). */
#define LOGO_HOLD_MS 1000

/* 新 OS 启动 (初始化 + 进桌面 + 主循环, 不返回) */
static void os_boot_shell(st7305_handle_t *lcd, app_board_t *board) {
    (void)board;
    ui_ctx_t ctx;
    /* 开机 Logo: 冷启动 / 手动关机唤醒显示; 休眠到点自动关机唤醒不显示(直接进桌面) */
    uint32_t t_logo_ms = 0;
    if (!s_soft_boot) {
        st7305_clear(lcd, ST7305_COLOR_WHITE);
        bbk_boot_draw_logo(lcd);
        st7305_flush(lcd);
        t_logo_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    os_init(&ctx, lcd);
    apps_register_all();     /* V1.2 模块化: 注册全部内置应用页 + 应用表 (原 os_init 内部调用) */
    wp_sleep_boot_diag();   /* 复位/唤醒诊断: 此处 USB 已重新枚举, 日志不会丢 */
    remcon_init();           /* 串口远程控制台 (USB Serial/JTAG 收命令, 远程自动化测试) */
    board_shim_set_lcd(lcd);   /* 绑定主屏到 board_shim: NES/GBC/文曲星/ArduBoy 引擎前置 (board_rlcd_is_initialized) */
    os_platform_set(&s_os_platform);
    /* 补等 Logo 显示时长 (仅硬上电; 软开机直接进桌面, 不等) */
    if (!s_soft_boot) {
        while ((uint32_t)(esp_timer_get_time() / 1000 - t_logo_ms) < LOGO_HOLD_MS) {
            /* ★ 2026-10-18: 按当前板型表的确认键判断（原 BTN_GPIO_A 是编译期 GPIO4，
             *   微雪板上该脚是电池 ADC、确认键在 18 ⇒ 换板后"按键跳过 Logo"失效）。 */
            int a_pin = board_pins_get()->btn_left;
            if (a_pin >= 0 && gpio_get_level((gpio_num_t)a_pin) == 0) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
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
if (hw_cfg_enabled(HW_AUDIO)) {
        app_board_init_audio(board);
    } else {
        ESP_LOGW("MAIN", "跳过 app_board_init_audio (硬件未检测到/被关闭)");
    }
if (hw_cfg_enabled(HW_BATTERY)) {
        app_board_init_battery(board);
    } else {
        ESP_LOGW("MAIN", "跳过 app_board_init_battery (硬件未检测到/被关闭)");
    }
    /* MAX-M10S GPS (2026-09-29 新增硬件): VCC 常通电, 固件当前不用定位 →
     * 初始化后立刻送"软件待机"(UBX-RXM-PMREQ), 把常驻 25~30mA 降到 µA 级.
     * 模块化: 驱动在 components/drivers/gps (gps.h API), 板级封装在 app_board. */
if (hw_cfg_enabled(HW_GPS)) {
        app_board_init_gps(board);
    } else {
        ESP_LOGW("MAIN", "跳过 app_board_init_gps (硬件未检测到/被关闭)");
    }
    /* ★★ 2026-10-15 linit "联网归网络模块管": 开机由**网络模块**自动联网 ——
     *   有保存凭据就拉起射频并连接, 之后断线重连全归它的自愈任务。
     *   应用/商店只管查 wifi_manager_is_connected(), 不再各自弹门禁去连网
     *   (原来没人主动开射频 ⇒ 不手动进设置页就一直"未联网")。 */
    /* ★★★ 2026-10-18 linit: 原来是**同步**调用, 界面要等 WiFi 初始化完(~4.9s)才出来。
     *   改成延后 2.5s 由独立任务启动 —— 首帧优先, 射频晚一点拉不影响使用。 */
    {
        extern void wifi_boot_deferred_task(void *arg);
        xTaskCreatePinnedToCore(wifi_boot_deferred_task, "wifi_boot", 4096, NULL, 3, NULL, 0);
    }
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
    /* 关机唤醒已在按键瞬间震过, 进桌面不再重复震; 冷启动在此震一次 */
    if (!wp_power_woke_from_off()) vibrator_tap(80, 120);
    os_shell_loop(&ctx);
}
#endif /* CONFIG_OS */

/* ★ 2026-10-18: 延后启动 WiFi(独立任务, 栈必须在内部 RAM —— 初始化要读写 NVS/flash) */
static void wifi_boot_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2500));
    wifi_manager_autostart();
    vTaskDelete(NULL);
}
void wifi_boot_deferred_task(void *arg) { wifi_boot_task(arg); }

void app_main(void)
{
    /* 关机唤醒门控 —— 必须最先执行: 手动关机唤醒时要求 PWR 键保持按住 ≥0.5s,
     * 不足(碰一下)判为误触, 直接回深睡(不返回); 自动关机 / 上电启动直接放行。 */
    wp_power_wake_gate();

    /* 电源统计: 结算上次深睡时长, 起新的"醒着段" (设置→休眠与电源→电池信息)。
     * 必须早调: 放在门控之后、其它初始化之前, 免得把初始化耗时也算进上一段睡眠。 */
    power_stat_init();

    /* V1.0.90: 自动保存日志到 TF (串口照常打印, 同时缓存进 /sdcard/log/bbk.log) */
    svc_log_init();
    /* ★ 版本管理整合 (2026-10-02): 开机横幅带版本 + git 提交 + 构建时刻。
     * 烧录后一眼可确认"设备里跑的到底是哪一版/哪个提交" (串口日志即可)。 */
    ESP_LOGI(TAG, "==== LinTOS 对外 %s / 内部 %s (%s) ====",
             os_version_public(), os_version_full(), os_version_name());
    ESP_LOGI(TAG, "     date=%s  build=%s  git=%s",
             os_version_date(), os_build_stamp(), os_git_desc());
    /* 唤醒来源诊断延后到 os_boot_shell(USB 重新枚举完成后)打印, 否则早期日志会丢 */

    /* V1.7.x: 统一北京时间时区 + 默认时间兜底 (收拢到 svc_time) */
    { extern void svc_time_init(void); svc_time_init(); }



    /* 本次唤醒来源决定开机观感:
     *   自动关机(休眠到点)唤醒 → 不显示任何东西, 直接进桌面;
     *   手动关机(长按关机键)唤醒 / 冷启动 → 显示开机 Logo。 */
    s_soft_boot = (wp_power_wake_origin() == WP_OFF_AUTO);

    /* Lua 集成冒烟测试: 挂载 SPIFFS(/lua)+加载执行 test.lua (失败不影响主系统) */
    { extern void lua_smoke_test(void); lua_smoke_test(); }

    /* === 硬件初始化 (app_board 模块化封装, 顺序敏感见 components/app_board) === */
    app_board_t board;
    /* ★★★ 2026-10-16 自适应: 开机先探测芯片指纹定板型, 再让 app_board 按板型下发引脚
     * ⚠ hw_cfg 要用 NVS 存板型/en_mask ⇒ **必须先把 NVS 起来**(否则 IDF 断言重启) */
    /* ★★★ 2026-10-16 内存优化(官方): S3 没有经典蓝牙, 但默认仍占着它的内存池
     *   在 BT 初始化之前释放掉 ⇒ 立即回收(官方 API, 反复释放安全) */
    { esp_err_t brel = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
      ESP_LOGW("MAIN", "★释放经典蓝牙内存池 ret=%d (S3 无经典蓝牙, 纯回收)", brel); }

    { esp_err_t nret = nvs_flash_init();
      if (nret == ESP_ERR_NVS_NO_FREE_PAGES || nret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
          nvs_flash_erase(); nvs_flash_init();
      } }
    hw_cfg_init();

    /* ★ 频谱仪 64KB 采集区回收: 必须在堆分配铺开之前一次性决定 (无 remove_region,
     *   只能开机按 NVS 决定)。禁用/无触摸 ⇒ heap_caps_add_region 加回内部堆 (+64KB)。
     *   touch_ok: 微雪原版无触摸屏 → 频谱仪全触屏操作没法用 → 强制回收。 */
    espsdr_reclaim_boot(hw_cfg_detected(HW_TOUCH));

    esp_err_t ret = app_board_init(&board);

    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "硬件初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    g_lcd = board.lcd;
    /* 把主屏句柄交给串口远程控制台 → `fbinfo` / `fbdump` 可抓屏 (真机/QEMU 通用) */
    remcon_set_lcd(&g_lcd);

    /* 暗黑模式: 开机即应用面板反色 (黑↔白, 含开机画面, 全局强制) */
    settings_apply_dark(&g_lcd);

    /* V1.0.76: 震动马达 LEDC 初始化 (幂等, 无硬件也不影响运行) */
    vibrator_init();
    /* 关机唤醒(由按键触发) → 立即震动反馈; 冷启动则由进桌面流程震动。 */
    if (wp_power_woke_from_off()) vibrator_tap(80, 120);

    /* V1.0.70: 音调播放器全局初始化 (PCM→I2S, 两板通用).
     * 声音是固件全局功能, 不绑定任何引擎; 开机音/按键音/闹铃随时可播. */
    tone_player_init();

    /* 开机 Logo 已移入 os_boot_shell 顶部 (与 OS 初始化并行, 见 LOGO_HOLD_MS 注释)。
     * 原此处"固定死等 1.5s"会白占启动时间。 */

#ifdef CONFIG_APP_AUTOTEST
    self_test_run_all();
#endif
#ifdef CONFIG_DISPLAY_REFRESH_TEST
    display_test_quick(&g_lcd);
#endif
    /* 新模块化 OS: 启动并进入主循环 (不返回) — 旧 menu_system 已移除 */
    os_boot_shell(&g_lcd, &board);
}
