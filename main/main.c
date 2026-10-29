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
#include "gps.h"   /* GPS 待机看门狗 */
#include "gps_nmea.h"   /* ★ 临时 GPS 自测: 真实 NMEA 解析 */
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
#include "ui_common.h"   /* FONT8X12 / ui_text16: 适配提示 (开机阶段字库可能未就绪) */
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
#include "touch_panel.h"     /* 触摸后台补探: touch_panel_recover / is_present */

static const char *TAG = "BBK";
static st7305_handle_t g_lcd;
/* UI 产帧间隔(ms)。**0 = 自动（默认）**，>0 = 固定值（调试用 `remcon pace <ms>`）。
 * 自动策略（按"有没有人在操作"分级，输入采样每圈都跑，不受节流影响）:
 *   400ms 内有触摸/按键 → 1ms   （跟手优先，就是原来流畅的手感）
 *   还在重绘但没人碰     → 8ms   （甩动/动画继续跑，省一半）
 *   完全静止             → 33ms  （屏幕上没东西在变，纯粹省电）
 * 之前固定 32ms 会被读成"非常卡"，就是因为拖动时也被拖慢到 ~20fps。 */
static int g_ui_frame_ms = 0;

void os_ui_set_pace(int ms) { g_ui_frame_ms = (ms < 1) ? 1 : (ms > 200 ? 200 : ms); }
int  os_ui_get_pace(void)   { return g_ui_frame_ms; }
/* 本次启动是否跳过开机 Logo —— 仅"休眠到点自动关机"唤醒时为 true(什么都不显示, 直接进桌面)。
 * 手动关机唤醒 / 冷启动为 false → 正常显示开机 Logo。 */
static bool s_soft_boot = false;
/* ★ 2026-10-27: Logo 上屏时刻(esp_timer ms)。现在 Logo 在**app_board_init 一返回**就画,
 *   不等后面的 TF 卡/页面注册; os_boot_shell 用它算"Logo 已显示多久", 不再重画。 */
static uint32_t s_logo_t0_ms = 0;
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
#include "power_mgr.h"   /* 唤醒门控/来源/开机诊断 (2026-10-28 从 sleep.h 迁出) */
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
/* ★ 2026-10-28 (第 5 阶段 Step 3): 「刷白」这一步留在调用方(main), 硬件收尾全交 power_mgr。
 *   原来刷白与"关射频/音频/GPS/SD/触摸/屏/引脚"一起写在 wallpapers/sleep.c 里 ——
 *   等于让 ui 层直接编排七八个外设的关闭顺序。现在 ui 只提出请求, 顺序由 power_mgr 负责。
 *   ⚠ 刷白必须仍在所有耗时动作之前(GPS 停靠 4~15 秒), 否则用户按了关机像"没反应"。 */
static void power_off_ui(int origin) {
    st7305_clear(&g_lcd, ST7305_COLOR_WHITE);
    st7305_flush_override(&g_lcd);
    power_mgr_off(origin);   /* 不返回 */
}
static void do_manual_shutdown(ui_ctx_t *ctx) {
    (void)ctx;
    wp_screensaver_halt();
    power_off_ui(POWER_OFF_MANUAL);
}
static void do_auto_shutdown(void) {
    wp_screensaver_halt();
    power_off_ui(POWER_OFF_AUTO);
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
/* ★ FPS 诊断计数 (每秒清零打印) */
static uint32_t g_ui_frames, g_ui_us_sum;

/* ★ FPS 诊断计数 (每秒清零打印) */
static uint32_t g_ui_frames, g_ui_us_sum;

/* ★★★ 2026-10-26 临时 GPS 自测 (linit: 临时启动确认没问题再休眠)
 *   为什么要存 NVS: 真机 USB 在开机 ~800ms 重新枚举、串口句柄失效,
 *   而 GPS 自测要在 1.3s 才开始、跑 8 秒 —— 直接打印抓不到。
 *   所以: 本次自测结果存 NVS, 下次复位的 ~300ms 打印出来(一定能抓到)。
 *   置 0 即恢复原来的"识别一次 / 开机停靠"逻辑。 */
#define TASKS_PROBE 0          /* ★ 任务/栈探针: 改 1 可随时打开查看(每秒 13 行日志, 平时关) */
#define TASKS_PROBE 0          /* ★ 任务/栈探针: 改 1 可随时打开查看(每秒 13 行日志, 平时关) */
#define GPS_SELFTEST_ONCE 0   /* ★ 2026-10-26 自测已完成(模块识别OK/NMEA流正常/已送待机): 改 0 恢复正常识别+停靠逻辑。改 1 可重跑自测 */
#define GSTEST_NS "gstest"

static void gps_selftest_task(void *arg)
{
    (void)arg;
    esp_err_t e = gps_init(NULL);
    ESP_LOGW("GSTEST", "自测: gps_init => %s (baud=%u ready=%d)",
             esp_err_to_name(e), (unsigned)gps_baud(), (int)gps_is_ready());
    int total = 0, nmea = 0, ubx = 0, fixq = 0, sats = 0, hdop100 = 0;
    long lat_u6 = 0, lon_u6 = 0;
    unsigned sents = 0;
    if (e == ESP_OK) {
        gps_wakeup();
        gps_service_start();          /* ★ 关键: 让 gps_service_is_running() 为真,
                                       *   否则主循环每秒的"无人使用→送就寝"兜底
                                       *   会把正在读数据的 UART 删掉 */
        gps_nmea_reset();
        uint8_t buf[256];
        int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 30 * 1000 * 1000) {
            int n = gps_read(buf, sizeof(buf), 200);
            if (n <= 0) continue;
            total += n;
            for (int i = 0; i < n; i++) {
                if (buf[i] == '$') nmea++;
                if (i + 1 < n && buf[i] == (uint8_t)0xB5 && buf[i + 1] == 0x62) ubx++;
            }
            gps_nmea_feed(buf, (size_t)n);
        }
        gps_fix_t f;
        bool ok = gps_nmea_get(&f);
        fixq  = ok ? (int)f.fix_quality : 0;
        sats  = (int)f.sats_used;
        hdop100 = (int)(f.hdop * 100.0f);
        sents = (unsigned)f.sentences;
        lat_u6 = (long)(f.lat * 1e6);
        lon_u6 = (long)(f.lon * 1e6);
    }
    gps_service_stop();           /* 先停解析任务, 再送就寝 (顺序不能反) */
    esp_err_t se = gps_sleep();
    esp_err_t re = gps_release();
    char summary[256];
    snprintf(summary, sizeof(summary),
             "init=%s 30s收%dB NMEA=%d UBX=%d fix=%d sats=%d dim hdop=%d/100 句=%u lat=%ld lon=%ld sleep=%s release=%s 状态=%d",
             esp_err_to_name(e), total, nmea, ubx, fixq, sats, hdop100, sents,
             lat_u6, lon_u6, esp_err_to_name(se), esp_err_to_name(re),
             (int)gps_power_state());
    ESP_LOGW("GSTEST", "%s", summary);
    nvs_handle_t h;
    if (nvs_open(GSTEST_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "r", summary);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW("GSTEST", "结果已存 NVS ⇒ 复位后开机 ~300ms 会打印");
    }
    vTaskDelete(NULL);
}


static void os_shell_loop(ui_ctx_t *ctx) {
    ESP_LOGI(TAG, "OS 主循环 (自适应帧率: 脏帧 1ms / 空闲 16ms)");
    uint32_t last_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t last_input_ms = 0;   /* 最近输入时间 (管家空闲检测用) */
    bool     was_ss = false;      /* 上一圈是否在屏保 (跳变检测, 见下) */
    uint32_t wake_force_ms = 0;   /* 屏保退出后的"强制重绘窗口"起点 */
    os_mgr_init();
    while (1) {
        /* ★ 2026-10-26 帧节拍改为“周期预算”: 记下圈首时刻,
         *   圈尾只补足剩余时间 (不再固定再等 16ms)。 */
        int64_t t_loop_us = esp_timer_get_time();
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
            was_ss = true;
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

        /* ★★ 屏保 → 界面 的跳变只能在这里判: 屏保期间主循环 continue,
         *     os_render 根本不会被调用, 所以 os_render 里那套 s_prev_ss/woke_up 永远不触发
         *     —— 这就是"解锁后停在最后一张壁纸, 不摸一下就不刷新"的真正原因。
         *     处理三件事:
         *       ① 作废"已送帧"指纹(壁纸是旁路 flush_override 刷的, 面板内容 ≠ fb,
         *          否则重绘算出来的 sum 与上次相同 → 被判"没变化"跳过送屏);
         *       ② 立刻置脏;
         *       ③ 之后 1 秒内每帧强制重绘, 盖掉壁纸任务可能迟到的最后一帧。 */
        if (was_ss) {
            was_ss = false;
            wake_force_ms = now_ms ? now_ms : 1u;
            st7305_invalidate_frame();
            ctx->needs_redraw = true;
            ESP_LOGI(TAG, "屏保退出: 作废送屏指纹 + 强制重绘 1s");
        }

        /* ★ 2026-10-24 linit: **不再定时往 TF 卡写日志** (原来每 3 秒一次, 既耗电又伤卡寿命,
         *   还会覆盖已删文件的簇 —— 误删后无法恢复)。
         *   日志现在只留在 4KB 内存缓冲里; 需要落盘时由 `remcon logsave`(或以后"导出日志")
         *   显式调用 svc_log_drain()。 */
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
        /* 唤醒窗口内每帧强制重绘(即使 UI 自认为没变化) */
        if (wake_force_ms && (uint32_t)(now_ms - wake_force_ms) < 1000u)
            ctx->needs_redraw = true;
        bool want_frame = ctx->needs_redraw;   /* poll/服务 tick 阶段是否已置脏 */
        /* FPS/渲染耗时统计 (每秒一行 UI: FPS=..) */
        { int64_t _t0 = esp_timer_get_time();
          os_render(ctx);
          g_ui_us_sum += (uint32_t)(esp_timer_get_time() - _t0);
          if (want_frame) g_ui_frames++; }
        /* 面板自刷新档位: 有绘制就回 HPM(≈32Hz), 连续 500ms 没画自动掉 LPM(≈1Hz 省电) */
        st7305_idle_tick(&g_lcd, want_frame, xTaskGetTickCount() * portTICK_PERIOD_MS);
        /* 帧节奏（自适应）:
         *   有脏帧 → 1ms（等于"立刻下一帧"，但让出 CPU 给异步任务/音频/网络）
         *   空闲   → 16ms（界面静止时不再空转，省电）
         *   弹窗拖动 → 8ms（跟手优先）
         *   屏保   → 32ms（壁纸由独立监督任务渲染）
         * 原来固定 16ms 意味着物理上限只有 ~60fps 再减渲染耗时；现在动画/游戏
         * 帧间隔由"绘制+送屏"本身决定（整帧 15000B @40MHz ≈ 3ms）。
         * TICK_RATE=1000Hz，pdMS_TO_TICKS(1) 是精确 1 tick。 */
        {
            /* ★★★ 2026-10-26 linit「帧率上不去」根因修复: **周期预算式节拍**
             *   古旧写法 `渲染+送屏(6~10ms) 后再 vTaskDelay(16ms)` ⇒ 实际周期 22~26ms (只有 38~45fps),
             *   而老工程是 16ms **周期** (上限 62fps) —— 这就是“老版本左右滑动很流畅”的差距。
             *   现在: 圈首计时, 圈尾只补足剩余时间 ⇒ 周期恒为 16ms（上限仍然是 62fps，与老版一致）。
             *   至少让出 2ms：避免工作超时时把主任务占满（音频/WiFi/触摸还要跑）。
             *   `remcon pace N` 仍可覆盖，语义从“延时”变为“周期”。
             * ★ 2026-10-26 linit「UI 要跟手」: 有脏帧时周期压到 8ms（触摸/滑动采样 125Hz，
             *   滑动跟手感直接取决于这个）；静止时仍 16ms，不白耗 CPU。 */
            int budget_ms;
            if (os_screensaver_active())      budget_ms = 32;   /* 壁纸由独立监督任务渲染 */
            else if (g_ui_frame_ms > 0)       budget_ms = g_ui_frame_ms;
            else if (want_frame)              budget_ms = 8;    /* ★ 有脏帧(拖动/动画): 8ms 周期 = 125Hz */
            else                              budget_ms = 16;   /* 静止: 16ms 空转只读输入 */
            int spent_ms = (int)((esp_timer_get_time() - t_loop_us) / 1000);
            int dly = budget_ms - spent_ms;
            if (dly < 2) dly = 2;                               /* 至少让出 2ms */
            vTaskDelay(pdMS_TO_TICKS(dly));
        }
        if (now_ms - last_ms >= 1000) {
            extern int font_zh_count(void);   /* 每秒带字体绑定状态 (0=绑定失败) */
            sdmmc_card_t *sd = sd_get_card(); /* SD 卡型号/容量 (确认挂载与卡型) */
            ESP_LOGI("UI", "FPS=%u 平均渲染=%u us/帧 (含送屏)",
                     (unsigned)g_ui_frames,
                     (unsigned)(g_ui_frames ? g_ui_us_sum / g_ui_frames : 0));
            g_ui_frames = 0; g_ui_us_sum = 0;
#if TASKS_PROBE
            /* ★ 2026-10-26 临时探针: 运行时任务数 + 每个任务栈余量
             *   (余量多 = 栈给多了; 任务不该在 = 没退干净) */
            {
                static TaskStatus_t ts[32];
                UBaseType_t n = uxTaskGetSystemState(ts, 32, NULL);
                uint32_t unused = 0;
                for (UBaseType_t i = 0; i < n; i++) unused += ts[i].usStackHighWaterMark;
                ESP_LOGI("TASKS", "运行任务=%u 栈未用合计=%lu B", (unsigned)n, (unsigned long)unused * 4);
                for (UBaseType_t i = 0; i < n; i++) {
                    if (ts[i].usStackHighWaterMark * 4 >= 1024)
                        ESP_LOGI("TASKS", "  %-14s prio=%u 栈余=%lu B",
                                 ts[i].pcTaskName, (unsigned)ts[i].uxCurrentPriority,
                                 (unsigned long)ts[i].usStackHighWaterMark * 4);
                }
            }
#endif
#if TASKS_PROBE
            /* ★ 2026-10-26 临时探针: 运行时任务数 + 每个任务栈余量
             *   (余量多 = 栈给多了; 任务不该在 = 没退干净) */
            {
                static TaskStatus_t ts[32];
                UBaseType_t n = uxTaskGetSystemState(ts, 32, NULL);
                uint32_t unused = 0;
                for (UBaseType_t i = 0; i < n; i++) unused += ts[i].usStackHighWaterMark;
                ESP_LOGI("TASKS", "运行任务=%u 栈未用合计=%lu B", (unsigned)n, (unsigned long)unused * 4);
                for (UBaseType_t i = 0; i < n; i++) {
                    if (ts[i].usStackHighWaterMark * 4 >= 1024)
                        ESP_LOGI("TASKS", "  %-14s prio=%u 栈余=%lu B",
                                 ts[i].pcTaskName, (unsigned)ts[i].uxCurrentPriority,
                                 (unsigned long)ts[i].usStackHighWaterMark * 4);
                }
            }
#endif
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
            /* ★★★ 2026-10-25 linit「打开软件启用 GPS, 关闭软件就休眠 GPS」的兜底:
             *   正常路径是 app.gps.stop() → gps_release(); 但应用被强退/切页/BACK 时可能漏调。
             *   这里每秒查一次: 解析任务没在跑(GPS 没人在用) 却还醒着 ⇒ 立即送回待机(46µA)
             *   并卸掉 UART(内部 RAM 归还)。最多多耗 1 秒电, 绝不会整晚 ~10mA。 */
            {
                extern bool gps_service_is_running(void);
                extern bool gps_sleep_verified(void);
                extern gps_power_state_t gps_power_state(void);
                extern esp_err_t gps_release(void);
                /* ★ 2026-10-27: 判据加上 gps_sleep_verified() —— gps_sleep() 现在只在
                 *   **实测静默 1.2s** 后才把状态置 SLEEP, 所以这个 1s 兜底能真正触发;
                 *   而 verified 之后就别再折腾(每装一次 UART 都会重配 TX pad, 可能把模块叫醒)。 */
                extern bool gps_is_busy(void);
                if (!gps_service_is_running() && gps_power_state() == GPS_POWER_ACTIVE &&
                    !gps_sleep_verified() && !gps_is_busy()) {
                    gps_release();
                    ESP_LOGW(TAG, "GPS 无人使用 → 已送回待机 (兜底休眠)");
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
 * V1.7.x: 软关机(深睡)唤醒跳过 Logo, 直接初始化进桌面 (用户要求快速唤醒).
 * ★ 2026-10-24 linit "优化开机速度": 600 → 300ms。实测 Logo 上屏在 T+623ms,
 *   os_init/页面注册在 T+637ms 已完成 ⇒ 原来的 600ms 里有 591ms 是纯白等。
 *   改成 300ms 后冷启动进桌面 1229ms → ~929ms (Logo 仍完整显示, 只是不等满 0.6s)。 */
#define LOGO_HOLD_MS 300

/* 开机耗时探针 (定义在 app_main 前, os_boot_shell 里也要用) */
static void boot_ts(const char *step);

/* ==================== 触摸未识别时的自适应轮询 ====================
 * linit 2026-10-22 定: 开机识别不到触摸 ⇒ **每 10 秒换一个目标**反复试,
 *   屏幕给出提示, 命中即落盘 + 重启, 直到识别到为止。
 *   目标: 0=本板触摸组  1=小智 codec(38/47)  2=微雪 codec(13/14)
 * ⛔ 只探当前板型自己的引脚(见 hw_cfg_probe_fingerprint 的说明), 不跨板。 */
static void adapt_txt(st7305_handle_t *lcd, int x, int y, const char *s, int scale)
{
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7e) continue;
        const uint8_t *b = FONT8X12[c - 0x20];
        for (int r = 0; r < 12; r++)
            for (int k = 0; k < 8; k++)
                if (b[r] & (0x80 >> k))
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++)
                            st7305_draw_pixel(lcd, x + i * 8 * scale + k * scale + dx,
                                              y + r * scale + dy, ST7305_COLOR_BLACK);
    }
}

/* first=true 时才画整屏。之后每 2 秒只擦/重画"状态行"——
 *   ⛔ 整屏 clear + flush 每 2 秒来一次, 反射屏上看着就是撕裂/闪烁。 */
static void hw_adapt_draw(st7305_handle_t *lcd, int which, int tries, bool first)
{
    static const char *TGT[3] = { "AUDIO 38/47", "AUDIO 13/14", "TOUCH 15/7" };
    char buf[64];

    if (first) {
        st7305_clear(lcd, ST7305_COLOR_WHITE);
        /* 标题: 字库已就绪用中文, 否则退回 ASCII 点阵 */
        if (ui_text16_ready()) {
            ui_text16(lcd, 116, 42, "未识别到硬件", false);
            ui_text16(lcd, 76, 74, "请检查接线后重启", false);
        } else {
            adapt_txt(lcd, 44, 34, "NO HW FOUND", 3);
            adapt_txt(lcd, 52, 84, "CHECK WIRING", 2);
        }
        adapt_txt(lcd, 30, 195, "AUTO CONTINUE WHEN FOUND", 1);
        adapt_txt(lcd, 30, 220, "HOLD LEFT KEY 3S = SKIP", 1);
    }

    /* 只重画状态行 (y 145..185) */
    fill_rect(lcd, 0, 145, ST7305_WIDTH - 1, 185, ST7305_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "PROBING %s   TRY %d", TGT[which], tries);
    adapt_txt(lcd, 30, 150, buf, 2);
    st7305_flush(lcd);
}

/* 逃生门: 按住左键 3 秒 ⇒ 不再等触摸, 用存档板型继续启动 (触摸仍为不可用)。
 *   否则换到"没有触摸的屏"时会永远停在提示界面, 只能拔电。 */
static bool adapt_skip_requested(void)
{
    static uint32_t t0 = 0;
    int pin = board_pins_get()->btn_left;
    if (pin < 0) return false;
    if (gpio_get_level((gpio_num_t)pin) != 0) { t0 = 0; return false; }
    if (t0 == 0) { t0 = xTaskGetTickCount(); return false; }
    return (xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(3000);
}

/* ★★★ 2026-10-22 linit 定的识别逻辑:
 *   ① 小智一体版: 音频 codec (38/47)  ② 微雪原版: 音频 codec (13/14)
 *   ③ 标准扩展版: 触摸 (15/7)         —— 标准版本来就没有音频 I2C, 只能靠触屏
 *   命中 ⇒ 板型固化 (落盘 board_id + board_forced, 以后开机 0 次探测) 并重启;
 *   全不中 ⇒ **一直循环**, 10s 换一个目标, 直到命中 (按住左键 3s 可跳过)。 */
static void hw_adapt_loop(st7305_handle_t *lcd)
{
    ESP_LOGW(TAG, "硬件指纹全未命中 ⇒ 识别轮询开始 (循环, 每目标 10s)");
    int tries = 0;
    for (int round = 0; ; round++) {
        for (int w = 0; w < 3; w++) {
            uint32_t t0 = xTaskGetTickCount();
            do {
                tries++;
                hw_adapt_draw(lcd, w, tries, tries == 1);
                if (hw_cfg_probe_fingerprint(w)) hw_cfg_adapt_commit(w);   /* 不返回 */
                for (int k = 0; k < 4; k++) {          /* 2s 拆成 4×500ms, 期间查逃生门 */
                    vTaskDelay(pdMS_TO_TICKS(500));
                    if (adapt_skip_requested()) {
                        ESP_LOGW(TAG, "按住左键 3s ⇒ 跳过识别, 继续启动 (硬件未全配)");
                        return;
                    }
                }
            } while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(10000));
        }
        if ((round & 7) == 7)
            ESP_LOGW(TAG, "识别轮询已跑 %d 轮仍未命中, 继续 (每 10s 换目标)", round + 1);
    }
}

/* 新 OS 启动 (初始化 + 进桌面 + 主循环, 不返回) */
/* ★ 触摸后台补探 (linit 2026-10-22 定 "识别不到就循环识别"):
 *   板型已固化, 但开机实测触摸没应答 ⇒ 不卡系统, 后台每 30s 重探一次
 *   (RST 脉冲 + 重装 I2C + 重扫地址), 命中就启用 + 落盘, 不用重启。
 *   为什么放后台: 触摸排线虚焊是最常见的故障, 补焊/换屏后用户不该被迫进设置"重新自检";
 *   为什么 30s 这么慢: 每次 recover 会 RST 脉冲 + 重装 I2C, 频繁做会干扰 codec 总线。 */
static void touch_retry_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));           /* 先让系统跑起来 */
    while (hw_cfg_touch_missing()) {
        touch_panel_recover();
        if (touch_panel_is_present()) {
            hw_cfg_touch_commit(true);
            ESP_LOGW(TAG, "触摸后台补探成功 ⇒ 触摸已启用 (无需重启)");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
    vTaskDelete(NULL);
}

static void os_boot_shell(st7305_handle_t *lcd, app_board_t *board) {
    ui_ctx_t ctx;
    /* 开机 Logo: 冷启动 / 手动关机唤醒显示; 休眠到点自动关机唤醒不显示(直接进桌面)
     * ★ 2026-10-27: 正常路径下 Logo 已经在 app_board_init 返回后**(见 main)就画好了**,
     *   这里只兜底 —— 万一 s_logo_t0_ms 仍为 0 才补画一次, 并用它算"已显示多久"。 */
    uint32_t t_logo_ms = s_logo_t0_ms;
    if (!s_soft_boot && t_logo_ms == 0) {
        st7305_clear(lcd, ST7305_COLOR_WHITE);
        bbk_boot_draw_logo(lcd);
        st7305_flush(lcd);
        t_logo_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_logo_t0_ms = t_logo_ms;
        boot_ts("Logo 上屏完成(兜底)");
    }
    os_init(&ctx, lcd);
    apps_register_all();     /* V1.2 模块化: 注册全部内置应用页 + 应用表 (原 os_init 内部调用) */
    boot_ts("os_init + 页面注册");
    power_mgr_boot_diag();   /* 复位/唤醒诊断: 此处 USB 已重新枚举, 日志不会丢 */
    remcon_init();           /* 串口远程控制台 (USB Serial/JTAG 收命令, 远程自动化测试) */
    board_shim_set_lcd(lcd);   /* 绑定主屏到 board_shim: NES/GBC/文曲星/ArduBoy 引擎前置 (board_rlcd_is_initialized) */
    os_platform_set(&s_os_platform);
    /* ★★★ 2026-10-27 开机提速: 音频/电池初始化**挪进 Logo 停留窗口内**做。
     *   原顺序: 画完桌面(991ms) → 音频+电池(~300ms) → 才进 os_shell_loop(~1315ms)
     *           ⇒ **画面已经停在桌面, 但按键/触摸 300ms 完全没反应** (用户以为死机)。
     *   现顺序: 音频+电池(约 300ms) 与 Logo 停留(LOGO_HOLD_MS=300ms) **完全重叠**
     *           ⇒ 桌面一出现主循环就在跑; 而"进主循环前音频/电池必须就绪"这个前提也没丢。
     *   代价: 内部 DMA RAM 早分配约 300ms — 桌面首帧缓冲(15KB)在更早的 app_board_init
     *         就已经分配完, 不受影响。 */
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
    boot_ts("音频 + 电池初始化完成");
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
    boot_ts("Logo 最短停留等待结束");
    os_push(&ctx, OS_PAGE_MAIN);
    boot_ts("进桌面 (os_push main)");
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
    /* ★ 2026-10-27: "音频 + 电池初始化" 已挪到 Logo 停留窗口内(见上方 os_platform_set 之后)。
     *   原来放在这里会挡住 os_shell_loop 约 300ms ⇒ 桌面已显示但按键无敌。 */
    /* MAX-M10S GPS (选装件): VCC 常通电, 固件不用定位时立刻送"软件待机"(46µA)。
     * ★★★ 2026-10-20 linit 定的策略 —— **只在"写入固件后的第一次开机"识别一次**:
     *   ① 没探过(hw_cfg_gps_probed()==false, 换固件时会被重置) ⇒ 后台真探一次 UART,
     *      有 UBX/NMEA 应答就判"焊了"并自动启用; 没有就记"未识别", 以后开机一次都不探。
     *   ② 探过且启用 ⇒ 后台初始化 + 送待机 (不占启动路径)。
     *   ③ 探过但未识别 ⇒ 什么都不做 (不装 UART、不碰引脚)。
     * 为什么必须后台: 波特率自适应 ~1.4s + 送待机 ~0.6s, 同步跑会把桌面拖到 3.9s
     *   (实测 BOOT 探针: 1740ms 音频完成 → 3181ms GPS 首行 → 3817ms 主循环)。 */
#if GPS_SELFTEST_ONCE
    /* ★ 临时自测模式: 不走"首次识别 / 开机停靠", 直接启动模块读 8 秒真实数据再送休眠。
     *   结果存 NVS ⇒ 下次开机打印。置 GPS_SELFTEST_ONCE 0 恢复正常逻辑。 */
    ESP_LOGW("MAIN", "★ GPS 临时自测模式: 启动模块 → 读 8 秒 真实数据 → 送待机");
    if (xTaskCreate(gps_selftest_task, "gps_test", 4096, NULL, 2, NULL) != pdPASS)
        ESP_LOGE("MAIN", "GPS 自测任务创建失败");
#else
    if (!hw_cfg_gps_probed()) {
        ESP_LOGW("MAIN", "GPS 首次识别 (写完固件后的第一次开机, 只探这一次)");
        app_board_gps_probe_async();
    } else {
        /* ★★★ 2026-10-24 linit: **不管 HW_GPS 开关, 开机一律停靠**。
         *   原因: 模块 VCC 常通电, 只要没被 PMREQ 送睡就持续 ~10mA 连续跟踪 ——
         *   而"硬件设置里 GPS 是关的"只是"固件不提供定位", 不等于"模块已睡"。
         *   冷上电 / 复位重启时模块一定是醒的, 必须由开机路径兜底送睡。
         *   gps_boot_park() 先静听 300ms: 已在睡(≤46µA)就不碰它, 醒着才发 PMREQ,
         *   结束即卸 UART(内部 RAM 归零) —— 全程后台, 不占启动路径、不常驻。 */
        /* ★ 2026-10-27: 由一次性"开机停靠"改成**常驻管家** —— 模块被噪声唤醒/上次没睡成时
         *   会自动再送 PMREQ; 应用在用(app.gps.start)时让路。见 board_gps_guard_task 注释。 */
        if (xTaskCreate(board_gps_guard_task, "gps_guard", 4096, board, 2, NULL) != pdPASS)
            app_board_init_gps(board);   /* 建任务失败就退回同步(至少功能不丢) */
    }
#endif
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
    if (!power_mgr_woke_from_off()) vibrator_tap(80, 120);
    os_shell_loop(&ctx);
}
#endif /* CONFIG_OS */

/* ★ 2026-10-18: 延后启动 WiFi(独立任务, 栈必须在内部 RAM —— 初始化要读写 NVS/flash)
 * ★ 2026-10-20: 2500ms → 800ms。原来留 2.5s 是怕 WiFi 射频上电的电流冲击把
 *   第一帧/3V3 拉掉; 现在桌面在 ~1.77s 就已经刷上屏, 射频在它之后才启动即可,
 *   不必再等 2.5s —— 实测这样"上电到联网"从 6.2s 缩到 ~4.3s。 */
static void wifi_boot_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    wifi_manager_autostart();
    vTaskDelete(NULL);
}
void wifi_boot_deferred_task(void *arg) { wifi_boot_task(arg); }

/* ★ 2026-10-20 开机耗时探针: 每步打 T+xxx ms, 直接指出"哪一步在拖慢启动"。
 *   只打 8 个点, 开销可忽略; 平时不影响使用, 排查时看 BOOT 行即可。 */
static uint32_t s_boot_t0 = 0;
static void boot_ts(const char *step)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_boot_t0 == 0) s_boot_t0 = now;
    ESP_LOGW("BOOT", "T+%5ums  %s", (unsigned)(now - s_boot_t0), step);
}

void app_main(void)
{
    boot_ts("app_main 入口");
    /* 关机唤醒门控 —— 必须最先执行: 手动关机唤醒时要求 PWR 键保持按住 ≥0.5s,
     * 不足(碰一下)判为误触, 直接回深睡(不返回); 自动关机 / 上电启动直接放行。 */
    power_mgr_wake_gate();

    /* 电源统计: 结算上次深睡时长, 起新的"醒着段" (设置→休眠与电源→电池信息)。
     * 必须早调: 放在门控之后、其它初始化之前, 免得把初始化耗时也算进上一段睡眠。 */
    power_stat_init();

    /* V1.0.90: 自动保存日志到 TF (串口照常打印, 同时缓存进 /sdcard/data/log/bbk.log) */
    svc_log_init();
    boot_ts("svc_log_init");
    /* ★ 版本管理整合 (2026-10-02): 开机横幅带版本 + git 提交 + 构建时刻。
     * 烧录后一眼可确认"设备里跑的到底是哪一版/哪个提交" (串口日志即可)。 */
    ESP_LOGI(TAG, "==== LinTOS 对外 %s / 内部 %s (%s) ====",
             os_version_public(), os_version_full(), os_version_name());
    ESP_LOGI(TAG, "     date=%s  build=%s  git=%s",
             os_version_date(), os_build_stamp(), os_git_desc());
    /* 唤醒来源诊断延后到 os_boot_shell(USB 重新枚举完成后)打印, 否则早期日志会丢 */

    /* V1.7.x: 统一北京时间时区 + 默认时间兜底 (收拢到 svc_time) */
    { extern void svc_time_init(void); svc_time_init(); }
    boot_ts("svc_time_init");



    /* 本次唤醒来源决定开机观感:
     *   自动关机(休眠到点)唤醒 → 不显示任何东西, 直接进桌面;
     *   手动关机(长按关机键)唤醒 / 冷启动 → 显示开机 Logo。 */
    /* ★ 2026-10-27 linit「自动关机和长按电源键效果一模一样」:
     *   原来自动关机唤醒走"快速开机"(不显示 Logo, 直接进桌面) —— 和手动唤醒观感不同。
     *   现在统一: 两种唤醒**都显示 Logo**、都要按住 PWR ≥0.5s(门控在 sleep.c)。
     *   保留 power_mgr_wake_origin() 的调用只为日志/诊断, 不再影响开机观感。 */
    s_soft_boot = false;

    /* Lua 集成冒烟测试: 挂载 SPIFFS + 加载执行 /store/test.lua。
     * ★ 2026-10-20 启动提速: 原来是**每次开机都跑**, 实测独占 **248ms** (SPIFFS 挂载
     *   + 读脚本), 纯粹是开发自检, 生产没必要。改成按需: 串口发 `luasmoke` 才跑。 */
    /* ★★★ 2026-10-23 修 (linit: 应用中心看不到《人生指南》, 没图标没名字):
     *   SPIFFS 挂载原先藏在 lua_smoke_test() 里, 而 smoke_test 因启动提速被注释掉
     *   ⇒ `/store` 从未挂载 ⇒ apps 分区里的 Lua 应用在应用中心**全部不可见**
     *   (设备上仅有的那几个 Lua 应用来自 TF 卡 /sdcard/apps)。
     *   挂载约 40ms, 必须早于应用中心预热扫描(开机 5s 后)。 */
    { extern bool store_fs_mount(void); store_fs_mount(); }
    boot_ts("SPIFFS /store 挂载");

    /* === 硬件初始化 (app_board 模块化封装, 顺序敏感见 components/app_board) === */
    app_board_t board;
    /* ★★★ 2026-10-16 自适应: 开机先探测芯片指纹定板型, 再让 app_board 按板型下发引脚
     * ⚠ hw_cfg 要用 NVS 存板型/en_mask ⇒ **必须先把 NVS 起来**(否则 IDF 断言重启) */
    /* ★★★ 2026-10-16 内存优化(官方): S3 没有经典蓝牙, 但默认仍占着它的内存池
     *   在 BT 初始化之前释放掉 ⇒ 立即回收(官方 API, 反复释放安全) */
    { esp_err_t brel = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
      ESP_LOGW("MAIN", "★释放经典蓝牙内存池 ret=%d (S3 无经典蓝牙, 纯回收)", brel); }
    boot_ts("释放经典蓝牙内存池");

    { esp_err_t nret = nvs_flash_init();
      if (nret == ESP_ERR_NVS_NO_FREE_PAGES || nret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
          nvs_flash_erase(); nvs_flash_init();
      } }
    hw_cfg_init();
    boot_ts("NVS + hw_cfg");
#if GPS_SELFTEST_ONCE
    /* ★ 临时: 打印上次 GPS 自测结果。放在这里(~300ms)是因为串口在 800ms
     *   就被 USB 重新枚举带走了。 */
    {
        nvs_handle_t gh;
        if (nvs_open(GSTEST_NS, NVS_READONLY, &gh) == ESP_OK) {
            char prev[256] = {0};
            size_t plen = sizeof(prev);
            if (nvs_get_str(gh, "r", prev, &plen) == ESP_OK && prev[0])
                ESP_LOGW("GSTEST", "上次自测: %s", prev);
            else
                ESP_LOGW("GSTEST", "上次自测: (无记录)");
            nvs_close(gh);
        }
    }
#endif

    /* ★ 频谱仪 64KB 采集区回收: 必须在堆分配铺开之前一次性决定 (无 remove_region,
     *   只能开机按 NVS 决定)。禁用/无触摸 ⇒ heap_caps_add_region 加回内部堆 (+64KB)。
     *   touch_ok: 微雪原版无触摸屏 → 频谱仪全触屏操作没法用 → 强制回收。 */
    espsdr_reclaim_boot(hw_cfg_detected(HW_TOUCH));

    esp_err_t ret = app_board_init(&board);
    boot_ts("app_board_init (LCD/触摸/SD 启动)");

    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "硬件初始化失败: %s", esp_err_to_name(ret));
        return;
    }

    g_lcd = board.lcd;
    /* 把主屏句柄交给串口远程控制台 → `fbinfo` / `fbdump` 可抓屏 (真机/QEMU 通用) */
    remcon_set_lcd(&g_lcd);

    /* 暗黑模式: 开机即应用面板反色 (黑↔白, 含开机画面, 全局强制)
     * ⚠ 必须在画 Logo **之前**: 反色是面板级设置, 画完再反会把 Logo 反成黑底。 */
    settings_apply_dark(&g_lcd);

    /* ★★★ 2026-10-27 linit「能不能让 Logo 提前显示, 后续必须在 Logo 期间处理」:
     *   LCD 一初始化完就**立刻**上 Logo, 然后才去等 TF 卡、注册页面、初始化音频/电池。
     *   原来 Logo 要等到 656ms(前面全是黑屏), 现在提前到 LCD init 结束(~520ms),
     *   而且后面这些"必须在进桌面前完成"的活儿全部落在 Logo 显示期间, 不额外多等。 */
    if (!s_soft_boot) {
        st7305_clear(&g_lcd, ST7305_COLOR_WHITE);
        bbk_boot_draw_logo(&g_lcd);
        st7305_flush(&g_lcd);
        s_logo_t0_ms = (uint32_t)(esp_timer_get_time() / 1000);
        boot_ts("Logo 上屏完成(提前)");
    }

    /* ★ 2026-10-27 linit「第一次检测正常过后就没必要再检测」:
     *   开机已不再用 I2C 探测判触摸(存档即信, 省 ~160ms/次), 这里用**触摸驱动自己的
     *   init 结果**兜底 —— 真没屏/排线断才置 missing, 由后台补探任务恢复。 */
    if (hw_cfg_enabled(HW_TOUCH) && !touch_panel_is_present())
        hw_cfg_touch_mark_missing();

    /* ★★ 2026-10-24: TF 卡挂载是 app_board 里的**异步任务**, 而页面/引擎/MP3 一进桌面就会
     *   opendir("/sdcard") —— 挂载没完成就扫, 拿到空列表且不再重扫 (linit: "存储信息全空"、
     *   "引擎/MP3 读不到卡")。这里在进桌面之前等它收尾(上限 1.5s, 已挂载则立即返回),
     *   让所有消费者天然看到"卡已就绪", 不再依赖各自的运气。
     *   ★ 2026-10-27: 这段等待现在发生在 **Logo 已经显示之后**, 用户看到的是 Logo 不是黑屏。 */
    if (hw_cfg_enabled(HW_SD)) {
        bool ok = sd_wait_ready(1500, false);
        boot_ts(ok ? "TF 卡就绪" : "TF 卡未就绪(无卡/挂载失败)");
    }

    /* ★ 触摸没识别到 ⇒ 进入自适应轮询 (屏幕上提示, 每 10 秒换目标, 直到命中)。
     *   有触摸(固定死)的机器完全跳过这一步, 不影响开机速度。 */
    if (hw_cfg_identify_failed()) {
#ifdef LINTOS_EMU_MODE
        /* ★ 2026-10-25 [LINTOS] 模拟器专用：esp-emu 没有 I2C 触摸模型，
         *   i2c_driver_install 必然失败 ⇒ hw_cfg_identify_failed() 恒为真 ⇒
         *   hw_adapt_loop 每 10s 换一个探测目标无限循环，表现为"开机就在
         *   app_board_init 之后卡死 60s 无日志"。这里直接跳过，用默认板型继续启动。
         *   ⚠ 只在 -DLINTOS_EMU_MODE=1 时编入，真机固件不含这段。 */
        ESP_LOGW("MAIN", "[模拟器] 跳过板型自适应循环 (无 I2C 触摸模型), 用默认板型继续");
#else
        hw_adapt_loop(&g_lcd);       /* 板型未定 ⇒ 一直循环识别, 命中即固化重启 */
#endif
    }
    /* 板型已定但触摸没探到 ⇒ 后台补探 (不阻塞, 命中即启用) */
    if (hw_cfg_touch_missing()) {
        xTaskCreate(touch_retry_task, "tp_retry", 3072, NULL, 1, NULL);
    }

    /* V1.0.76: 震动马达 LEDC 初始化 (幂等, 无硬件也不影响运行) */
    vibrator_init();
    /* 关机唤醒(由按键触发) → 立即震动反馈; 冷启动则由进桌面流程震动。 */
    if (power_mgr_woke_from_off()) vibrator_tap(80, 120);

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
