/**
 * @file main.c
 * @brief ESP32-S3-RLCD-4.2 BBK 游戏模拟器 - 菜单系统
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "st7305.h"
#include "bbk_boot.h"
#include "menu_system.h"
#include "diagnosis.h"   /* V1.0.98: 电脑诊断页每帧轮询 diag_poll */
#include "input.h"
#include "gam4980_emu.h"
#include "user_config.h"
#include "sd_scan.h"
#include "bt_manager.h"
#include "audio_player.h"
#include "board_battery.h"
#include "book_reader.h"
#include "mini_apps.h"
#include "app_board.h"
#include "tone_player.h"   /* V1.0.68: 方波直驱音调播放器 (开机音/按键音) */
#include "engine_manager.h" /* V1.0.98: 主菜单定时复核引擎残留 (unload_all 幂等) */
#include "vibrator.h"      /* V1.0.76: 苹果 Taptic Engine 震动马达 (GPIO2) */
#include "self_test.h"
#include "display_test.h"
#include "esp_attr.h"

static const char *TAG = "BBK";
static st7305_handle_t g_lcd;
/* 20260812: 菜单状态移到 PSRAM (UI 数据均在普通任务上下文访问, PSRAM 安全),
 * 腾出 ~33KB 内部 RAM 给引擎/蓝牙/DMA. */
EXT_RAM_BSS_ATTR menu_state_t g_menu;
/* V1.0.68: 内容页拖动状态 (跨帧): 最近手指 y 与累计位移 (抑制松手误确认) */
static int s_select_drag_last_ty = 0;
static int s_select_drag_moved = 0;
/* V1.0.68: 主菜单拖动累计位移 (抑制松手帧的方向键, 物理按键放行) */
static int s_main_drag_moved = 0;

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

static int log_to_file_vprintf(const char *fmt, va_list arg) {
    /* 1) 串口照常输出 (保留原有监视能力) */
    va_list arg_console;
    va_copy(arg_console, arg);
    int n = vprintf(fmt, arg_console);
    va_end(arg_console);
    /* 2) 只缓存到文件缓冲, 不阻塞串口输出 */
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
    return n;
}

/* 主循环每帧调用: 把缓冲内容追加写入日志文件. 仅 SD 挂载且缓冲非空时写盘. */
static void log_drain(void) {
    if (!sd_is_mounted()) return;          /* 无卡/未挂载: 保留缓冲稍后再写 */
    s_log_skip = true;
    size_t len = s_log_len;
    if (len == 0) { s_log_skip = false; return; }
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
}

/* V1.0.68: 列表弹窗跟手拖动状态 (番茄钟时间列表等) */
static bool s_list_drag_active = false;
static int  s_list_drag_start_y = 0;
static int  s_list_drag_moved = 0;

/* V1.0.90: 底部屏蔽带 (仅用于返回/退出, 禁止拖动页面).
 * 手指从屏幕底部中间约 20px (y>=280, x∈[100,300]) 内按下开始,
 * 整个手势期间任何页面都不会被拖动; 点击/确认等其它操作照常, 向上滑触发返回.
 * 需要锁存"本次按下起点是否在屏蔽带内", 手指抬起后清除. */
#define TOUCH_SHIELD_Y      280   /* 距屏幕底部约 20px (屏幕总高 300) */
static bool touch_shield_blocks_drag(void) {
    /* V1.0.99: 只读查询 input_touch_in_bottom_zone (复用 input_get_touch_pos 已锁存的
     * 起点屏蔽带结果), 不再内部自调 input_get_touch_pos. 旧实现同帧内把
     * input_get_touch_pos 的 static 状态消费两次, 残留状态导致所有拖动方都无法建立
     * 拖动 (拖动全面失效, 重启即消). 各拖动分支保证本函数在 input_get_touch_pos 之后
     * 调用, 且手势期间 s_o_zone 由 input_get_touch_pos 锁存不变, 故无需再自行锁存. */
    return input_touch_in_bottom_zone();
}

void app_main(void)
{
    /* V1.0.90: 自动保存日志到 TF (串口照常打印, 同时缓存进 /sdcard/log/bbk.log) */
    esp_log_set_vprintf(&log_to_file_vprintf);
    ESP_LOGI(TAG, "==== BBK 游戏模拟器启动 ====");
    /* V1.0.68: 判断本次启动来源 (deep sleep 唤醒 = 电源键开机) */
    {
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        if (cause == ESP_SLEEP_WAKEUP_EXT1) {
            ESP_LOGI(TAG, "电源键唤醒 (软开机)");
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

    /* V1.0.76: 震动马达 LEDC 初始化 (幂等, 无硬件也不影响运行) */
    vibrator_init();

    /* 显示开机 Logo (1.5秒, 按 A 键跳过) */
    st7305_clear(&g_lcd, ST7305_COLOR_WHITE);
    st7305_draw_bitmap_1bit(&g_lcd, 0, 0, bbk_boot_logo_w, bbk_boot_logo_h, bbk_boot_logo);
    st7305_flush(&g_lcd);

    /* 快速跳过检测: 50ms 间隔, 最多 30 次 = 1.5s */
    for (int i = 0; i < 30; i++) {
        if (gpio_get_level(BTN_GPIO_A) == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* 进入主菜单 */
    menu_init(&g_menu, &g_lcd);
    menu_render(&g_menu);

    /* V1.0.68: 方波直驱方案下播放开机提示旋律 (PWM 已由 menu_init 初始化) */
    if (g_menu.settings.audio_scheme == 1 && tone_player_ready()) {
        tone_play_theme(TONE_THEME_BOOT_UP);
    }

    /* 主菜单基线内存 (引擎未加载, 用于对比优化效果) */
    ESP_LOGI("MEM", "[MEM] 主菜单基线: 内部空闲=%u 内部最大块=%u PSRAM空闲=%u PSRAM最大块=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    /* 9.5 (已移除) 后台预加载电子词典引擎: 改为按需加载,
     * 仅在进入电子词典二级菜单时由 engine_manager 加载, 以降低主菜单期内存占用. */

    /* 延后初始化 (避免抢占内部 DMA RAM): 蓝牙 -> 音频 -> 电池 */
    app_board_init_bt();
    app_board_init_audio(&board);
    menu_apply_volume_setting();
    app_board_init_battery(&board);

#ifdef CONFIG_APP_AUTOTEST
    /* 自动化自测 (Kconfig: Self Test -> APP_AUTOTEST, 默认关闭) */
    self_test_run_all();
#endif

#ifdef CONFIG_DISPLAY_REFRESH_TEST
    /* 显示刷新自检 (Kconfig: Display Test -> DISPLAY_REFRESH_TEST, 默认关闭).
     * 20260812: 密集"一点隔一点"灰点静止测试, 确认面板对细密交替图案是否固有闪烁. */
    display_test_quick(&g_lcd);
#endif

    ESP_LOGI(TAG, "进入菜单系统 (60 FPS 模式)");

    /* === 60 FPS 主循环: 固定 16.67ms 帧间隔 === */
    /* SPI 传输 ~1.5ms + render ~5ms = ~6.5ms/帧, 远低于 16.67ms (60FPS) 预算 */
    /* 剩余时间由 vTaskDelayUntil 精确填充, 不浪费 CPU */
    uint32_t fps_count = 0;
    uint32_t fps_last_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t render_total_us = 0;
    /* V1.0.43: 电池电压采样间隔 (5秒读一次, 避免频繁ADC影响性能) */
    uint32_t battery_last_ms = 0;
    /* V1.0.98: 主菜单引擎残留复核间隔 (60秒一次, 兜底卸载未释放引擎) */
    uint32_t engine_check_last_ms = 0;
    while (1) {
        log_drain();   /* V1.0.90: 每帧把缓存日志追加写入 TF 卡 */
        /* V1.0.68: 软关机键 GPIO1 长按 2 秒软关机 */
        if (input_power_should_sleep()) {
            menu_soft_power_off(&g_lcd);
        }
        menu_check_dialog_timeout();
        /* V1.0.72: SD 晚挂载后补读 TF 配置 (一次性; 实现 TF 优先的鲁棒性) */
        menu_config_tf_reload_if_needed();
        /* 蓝牙开启后, 等 HID Host 就绪自动启动一次主动连接扫描 (无需用户操作) */
        menu_poll_bt_auto_connect(&g_menu);
        menu_action_t action = input_get_action();
        if (action != MENU_ACTION_NONE) {
            ESP_LOGI(TAG, "按键: action=%d", (int)action);
        }
        bool has_input = (action != MENU_ACTION_NONE);

        /* V1.0.68: 方波直驱方案下, 物理按键导航/确认播放短促按键音 (触摸不响) */
        if (has_input && g_menu.settings.audio_scheme == 1 &&
            tone_player_ready() && !input_touch_last_action() &&
            (action == MENU_ACTION_CONFIRM || action == MENU_ACTION_UP ||
             action == MENU_ACTION_DOWN || action == MENU_ACTION_LEFT ||
             action == MENU_ACTION_RIGHT)) {
            tone_play_effect(TONE_EFFECT_CONFIRM);
        }

        /* V1.0.68: 触摸长按 (游戏列表收藏等): 消费坐标执行, 不再产生其他动作 */
        if (action == MENU_ACTION_LONG_PRESS) {
            int tx, ty;
            if (input_consume_tap(&tx, &ty)) {
                menu_touch_long_press(&g_menu, tx, ty);
            }
            action = MENU_ACTION_NONE;
            has_input = false;
        }
        /* V1.0.69: 软关机键 (GPIO1): 点按 = 锁屏休眠; 长按 2s = 关机(input_power_should_sleep).
         * 已去掉 0.5s"返回菜单"提示中间层 (HOME 改由状态栏长按/BOOT 长按承担). */
        if (action == MENU_ACTION_POWER_LOCK) {
            if (screensaver_is_active()) {
                /* V1.0.68: 锁屏界面点按软关机键 = 立即退出休眠 */
                ESP_LOGI(TAG, "锁屏: 点按软关机键退出休眠");
                action = MENU_ACTION_NONE;
                has_input = true;   /* 交给 screensaver_check_and_render 退出 */
            } else {
                ESP_LOGI(TAG, "软关机键点按 -> 锁屏休眠");
                menu_screensaver_activate();
                action = MENU_ACTION_NONE;
                has_input = false;
            }
        }
        
        /* 音乐播放/暂停时不允许进入屏保, 且重置屏保计时
         * 注意: 严格判断 PLAYING/PAUSED, 不要用 !=STOPPED,
         * 因为 audio_player 的 s_state 初始化为 IDLE(=0), 不是 STOPPED(=3),
         * 否则即使没放音乐也会持续重置屏保, 永远到不了 3 分钟. */
        audio_state_t astate = audio_player_get_state();
        if (astate == AUDIO_STATE_PLAYING || astate == AUDIO_STATE_PAUSED) {
            screensaver_reset();
        }
        
        if (screensaver_is_active()) {
            /* V1.0.95: 删除"触摸退出"选项; 触摸只有长按 1 秒才退出壁纸,
             * 快速点击/上滑等一次性触摸动作不退出 (物理键 / 手柄 / 软关机键仍可退出).
             * 按住 ≥1s 实时放行, 无需松手. */
            if (has_input && input_touch_last_action()) {
                has_input = false;   /* 触摸的一次性动作先拦截 */
            }
            if (input_touch_hold_ms() >= 1000) {
                has_input = true;    /* 触摸长按 1 秒达标 → 退出壁纸 */
            }
            screensaver_check_and_render(&g_lcd, has_input);
            if (!screensaver_is_active()) {
                /* 屏保刚退出, 重绘菜单 + 触发蓝牙立即重连 */
                g_menu.needs_redraw = true;
                g_menu.bt_retry_next_ms = 1;  /* 强制下次轮询立即重连历史设备 */
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* V1.0.66: 主菜单跟手拖动 (手指拖动整排图标跟着平移, 松手吸附).
         * V1.0.68: 弹窗/全屏覆盖期间禁止背景拖动, 只能操作弹窗. */
        bool main_drag_suppress_lr = false;
        if (g_menu.current_page == MENU_PAGE_MAIN && !menu_modal_active(&g_menu)) {
            int tx, ty;
            bool down = input_get_touch_pos(&tx, &ty);
            if (down && !g_menu.main_drag_active && !touch_shield_blocks_drag()) {
                g_menu.main_drag_active = true;
                g_menu.main_drag_start_x = tx;
                g_menu.main_drag_offset = 0;
                s_main_drag_moved = 0;
                g_menu.main_cam_base = g_menu.main_cam_cur;  /* 相机接续当前显示位置 */
                g_menu.main_cam_anim_start = 0;              /* 取消动画, 改由手指控制 */
                g_menu.needs_redraw = true;
                screensaver_reset();           /* 拖动算操作, 重置屏保计时 */
            }
            if (g_menu.main_drag_active) {
                if (down) {
                    int off = tx - g_menu.main_drag_start_x;
                    if (off != g_menu.main_drag_offset) {
                        g_menu.main_drag_offset = off;
                        g_menu.needs_redraw = true;
                    }
                    if (off < 0) off = -off;
                    if (off > s_main_drag_moved) s_main_drag_moved = off;
                } else {
                    /* V1.0.97 松手吸附: 相机 = 拖动起点 + 像素/格间距, 向最近格吸附.
                     * 由 menu_main_cam_animate 把相机 from→round(from) 插值 → 松手
                     * 首帧与最后一帧拖动共用同一相机, 像素级衔接, 停在指下、无回弹. */
                    g_menu.main_drag_active = false;
                    const int spacing = 100;
                    float cur = g_menu.main_cam_base - (float)g_menu.main_drag_offset / (float)spacing;
                    int target = lroundf(cur);
                    int steps = (int)(target - lroundf(g_menu.main_cam_base));
                    if (steps < 0) steps = -steps;
                    uint32_t dur = 240 + (uint32_t)steps * 70;
                    if (dur > 700) dur = 700;
                    menu_main_cam_animate(&g_menu, cur, target, dur);
                    g_menu.needs_redraw = true;
                    ESP_LOGI("MAIN", "松手: cur=%.2f target=%d", cur, target);
                    /* V1.0.68: 本次触摸拖动过 → 抑制本帧 LEFT/RIGHT
                     * (拖动已接管横向切换; 物理 BOOT 短按不受影响) */
                    if (s_main_drag_moved >= 15) main_drag_suppress_lr = true;
                }
            }
            /* V1.0.68: 只忽略"拖动松手"帧的方向键; 物理 BOOT 短按(RIGHT) 正常放行,
             * 主菜单可按键右移图标 */
            if ((action == MENU_ACTION_LEFT || action == MENU_ACTION_RIGHT) &&
                main_drag_suppress_lr) {
                action = MENU_ACTION_NONE;
                has_input = false;
            }
        }
        /* V1.0.68: 内容页(游戏/电子书)右栏跟手拖动:
         *  - 任意焦点下触摸右栏都启用拖动, 并自动切焦点到右栏
         *  - 按下/拖动实时选中手指所在行 (加黑)
         *  - 列表不随偏移滚动, 松手无回弹; 选中停在手指位置
         *  - 拖动(位移≥12px)不触发确认, 避免误进游戏
         *  - 弹窗/全屏覆盖期间禁用 */
        bool drag_suppress_confirm = false;
        if (!menu_modal_active(&g_menu) &&
            (g_menu.current_page == MENU_PAGE_SELECT_GAME ||
             g_menu.current_page == MENU_PAGE_GB_GAME ||
             (g_menu.current_page == MENU_PAGE_BOOK &&
              g_menu.book_rot != 2 && g_menu.book_rot != 3))) {
            int tx, ty;
            bool down = input_get_touch_pos(&tx, &ty);
            /* V1.0.68 fix: 记录本帧开始时是否在拖动 (松手帧在下方才会清零) */
            bool content_drag_was_active = g_menu.select_game_drag_active;
            if (down && !g_menu.select_game_drag_active && tx >= 104 &&
                !touch_shield_blocks_drag()) {
                g_menu.select_game_drag_active = true;
                g_menu.select_game_drag_start_y = ty;
                g_menu.select_game_drag_offset = 0;
                g_menu.select_focus = 1;            /* 触摸内容页 → 焦点切到右栏 */
                g_menu.select_game_drag_fix = false;/* 新拖动开始, 解除固定 */
                s_select_drag_last_ty = ty;
                s_select_drag_moved = 0;
                menu_select_game_touch_track(&g_menu, ty, 0);   /* 按下即选中该游戏 */
                g_menu.needs_redraw = true;
                screensaver_reset();
            }
            if (g_menu.select_game_drag_active) {
                if (down) {
                    int off = ty - g_menu.select_game_drag_start_y;
                    if (off != g_menu.select_game_drag_offset) {
                        /* V1.0.68: 整个列表跟随手指上下移动, 选中项保持不动 */
                        g_menu.select_game_drag_offset = off;
                        g_menu.needs_redraw = true;
                    }
                    if (off < 0) off = -off;
                    if (off > s_select_drag_moved) s_select_drag_moved = off;
                } else {
                    /* 松手: 固定当前内容页位置 (scroll 补偿偏移, 不回弹不吸附) */
                    g_menu.select_game_drag_active = false;
                    if (s_select_drag_moved >= 12) drag_suppress_confirm = true;
                    int off = g_menu.select_game_drag_offset;
                    if (off > 0) g_menu.select_game_scroll -= (off + 16) / 32;
                    else if (off < 0) g_menu.select_game_scroll += ((-off) + 16) / 32;
                    if (g_menu.select_game_scroll < 0) g_menu.select_game_scroll = 0;
                    g_menu.select_game_drag_fix = true;   /* 渲染只做边界钳制, 不因选中回拉 */
                    g_menu.select_game_drag_offset = 0;
                    g_menu.needs_redraw = true;
                }
            }
            /* V1.0.68 fix: 手柄/物理键 UP/DOWN 控制二级菜单内容页选项.
             * 旧实现无条件吞掉 UP/DOWN 导致手柄方向键在这些页面失效.
             * 现在仅在"动作来自触摸滑动 且 本帧处于拖动中(含松手帧)"时忽略,
             * 因为拖动已接管内容页, 松手时的 flick 不应再额外移动选中. */
            if ((action == MENU_ACTION_UP || action == MENU_ACTION_DOWN) &&
                input_touch_last_action() && content_drag_was_active) {
                action = MENU_ACTION_NONE;
                has_input = false;
            }
        }
        /* V1.0.68: 列表弹窗跟手拖动 (番茄钟时间列表等, 标准渲染):
         * 整个列表跟随手指移动, 选中项保持; 松手固定位置不回弹.
         * 自定义渲染弹窗(时间等)保持原编辑交互. */
        if (g_menu.list_dialog_active && !g_menu.list_dialog_on_render) {
            int tx, ty;
            bool down = input_get_touch_pos(&tx, &ty);
            /* V1.0.90: 内容全部显示得下(不可滚动)时不启动拖动, 屏蔽滑动误拖 */
            bool can_scroll = menu_list_dialog_scrollable(&g_menu);
            if (down && !s_list_drag_active && can_scroll &&
                !touch_shield_blocks_drag()) {
                s_list_drag_active = true;
                s_list_drag_start_y = ty;
                s_list_drag_moved = 0;
                g_menu.list_dialog_drag_offset = 0;
                g_menu.list_dialog_drag_fix = false;
                menu_list_dialog_touch_track(&g_menu, ty);   /* 按下选中该行 */
                g_menu.needs_redraw = true;
            }
            if (s_list_drag_active) {
                if (down) {
                    int off = ty - s_list_drag_start_y;
                    if (off != g_menu.list_dialog_drag_offset) {
                        g_menu.list_dialog_drag_offset = off;   /* 整列跟手 */
                        g_menu.needs_redraw = true;
                    }
                    if (off < 0) off = -off;
                    if (off > s_list_drag_moved) s_list_drag_moved = off;
                } else {
                    s_list_drag_active = false;
                    if (s_list_drag_moved >= 12) drag_suppress_confirm = true;
                    menu_list_dialog_release(&g_menu);   /* 松手固定内容位置 */
                }
                /* 拖动期间忽略 swipe 产生的 UP/DOWN (由拖动接管) */
                if (action == MENU_ACTION_UP || action == MENU_ACTION_DOWN) {
                    action = MENU_ACTION_NONE;
                    has_input = false;
                }
            }
        }
        /* V1.0.9x: 应用管理内容页上下拖动 (触摸屏跟手滚动), 显示更多项 */
        if (g_menu.current_page == MENU_PAGE_APP_MANAGER && !menu_modal_active(&g_menu)) {
            app_manager_tick(&g_menu);   /* V1.0.9x fix: 逐帧推进回弹动画, 落位后命中才准 */
            int tx, ty;
            bool down = input_get_touch_pos(&tx, &ty);
            if (down && !app_manager_drag_is_active() && !touch_shield_blocks_drag()) {
                app_manager_drag_begin(tx, ty);
                g_menu.needs_redraw = true;
                screensaver_reset();
            }
            if (app_manager_drag_is_active()) {
                if (down) {
                    app_manager_drag_update(tx, ty);
                    g_menu.needs_redraw = true;
                } else {
                    if (app_manager_drag_end()) drag_suppress_confirm = true;
                    g_menu.needs_redraw = true;
                }
                /* 拖动期间忽略 swipe 产生的 UP/DOWN (由滚动接管), 避免误移选中 */
                if (action == MENU_ACTION_UP || action == MENU_ACTION_DOWN) {
                    action = MENU_ACTION_NONE;
                    has_input = false;
                }
            }
        }
        if (has_input) {
            /* V1.0.68: 拖动松手后抑制误触发的 CONFIRM */
            if (action == MENU_ACTION_CONFIRM && drag_suppress_confirm) {
                action = MENU_ACTION_NONE;
                has_input = false;
            }
            /* V1.0.65: 触摸"点击"优先走 hit-test (点哪进哪).
             * V1.0.67: 触摸点击未命中任何可点区域时直接忽略, 不再回退为普通确认,
             * 避免点屏幕空白/图标两侧误触发"确定"进入 (用户反馈误操作). */
            if (action == MENU_ACTION_CONFIRM) {
                int tx, ty;
                if (input_consume_tap(&tx, &ty)) {
                    if (menu_handle_touch(&g_menu, tx, ty)) {
                        /* WiFi 键盘点击已由 menu_handle_touch 直接执行按键动作,
                         * 不再重复触发 CONFIRM */
                        if (!g_menu.wifi_kb_active) {
                            menu_handle_action(&g_menu, MENU_ACTION_CONFIRM);
                        }
                    }
                    /* 未命中: 忽略这次点击 */
                } else {
                    /* 非触摸 CONFIRM (物理键/手柄): 正常确认 */
                    menu_handle_action(&g_menu, action);
                }
            } else {
                menu_handle_action(&g_menu, action);
            }
        }
        /* 电子书: 处理麦克风敲击翻页 */
        if (book_reader_poll()) {
            g_menu.needs_redraw = true;
        }
        /* 长按收藏检测: 必须在 menu_handle_action 后调用 (检查 KEY 是否持续按住) */
        menu_poll_long_press(&g_menu);
        /* V1.0.9x: 网络工具独立页每帧轮询 (定时刷新/任务推进) */
        if (nettool_poll(&g_menu)) {
            /* needs_redraw 已在 nettool_poll 内置 */
        }
        /* V1.0.98: 电脑诊断页每帧轮询 (右侧概率栏拖动滚动) */
        if (g_menu.current_page == MENU_PAGE_DIAGNOSIS) {
            diag_poll(&g_menu);
        }
        /* 按键映射模式: 每帧轮询手柄按键并捕获 (不依赖物理按键) */
        menu_poll_gamepad_mapping(&g_menu);
        /* 补充按键映射模式: 每帧轮询物理键 + 处理确定/跳过 */
        menu_poll_sup_mapping(&g_menu);
        /* GB 辅助按键映射模式: 每帧轮询 (提示确认 + 捕获/确定/跳过) */
        menu_poll_gb_auxmap(&g_menu);
        if (screensaver_check_and_render(&g_lcd, has_input)) {
            continue;
        }
        uint32_t t0 = esp_timer_get_time();
        menu_render(&g_menu);
        uint32_t t1 = esp_timer_get_time();
        if (t1 - t0 > 1) {
            ESP_LOGD(TAG, "render=%lu us has_input=%d", (unsigned long)(t1 - t0), has_input);
        }
        if (t1 - t0 > 100) {
            render_total_us += (t1 - t0);
            fps_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        /* V1.0.43: 每分钟读取一次电池电压, 更新状态栏电量.
         * V1.0.74: 模态弹窗/二级菜单打开时不强制整页重绘, 避免状态栏刷新
         * 每隔一段时间触发一次全屏闪烁 (如键鼠布局菜单). 弹窗关闭后的下一次
         * 渲染会自然带上最新电量. */
        if (now_ms - battery_last_ms >= 60000) {
            battery_last_ms = now_ms;
            board_battery_status_t bat;
            bool modal = menu_modal_active(&g_menu);
            if (board_battery_read(&bat) == ESP_OK) {
                g_menu.settings.battery = bat.percent;
                if (!modal) g_menu.needs_redraw = true;
            } else {
                /* V1.0.68: 未检测到电池 -> 255 哨兵值, 状态栏电池上显示 "?" */
                if (g_menu.settings.battery != 255) {
                    g_menu.settings.battery = 255;
                    if (!modal) g_menu.needs_redraw = true;
                }
            }
        }
        /* V1.0.98: 主菜单每 60 秒兜底复核一次引擎残留.
         * 正常路径下返回主菜单已 unload_all; 此处针对异常退出/状态残留导致
         * 未成功卸载的引擎做二次清理, 并触发内存泄漏检测. 仅在主菜单且无弹窗
         * 时执行, 避免干扰正在使用的引擎. */
        if (g_menu.current_page == MENU_PAGE_MAIN && !menu_modal_active(&g_menu) &&
            now_ms - engine_check_last_ms >= 60000) {
            engine_check_last_ms = now_ms;
            engine_manager_unload_all();
        }
        if (now_ms - fps_last_ms >= 1000) {
            if (render_total_us > 0) {
                ESP_LOGI(TAG, "FPS=%lu avg=%lu us",
                         (unsigned long)fps_count,
                         (unsigned long)(render_total_us / fps_count));
            } else {
                ESP_LOGI(TAG, "idle");
            }
            /* 栈/内存监控: 每秒打印 main 任务栈高水位 + 各内存区空闲, 辅助排查
             * 打开引擎/WiFi手柄时崩溃. DMA=WiFi能用的内部DMA段, 剩余越少越危险. */
            ESP_LOGI(TAG, "STACK main_free=%u 内部=%u DMA=%u DMA大块=%u PSRAM=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            fps_count = 0;
            render_total_us = 0;
            fps_last_ms = now_ms;
        }
    }}
