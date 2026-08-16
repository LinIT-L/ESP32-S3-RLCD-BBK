/**
 * @file main.c
 * @brief ESP32-S3-RLCD-4.2 BBK 游戏模拟器 - 菜单系统
 */

#include <stdio.h>
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
#include "input.h"
#include "gam4980_emu.h"
#include "user_config.h"
#include "sd_scan.h"
#include "bt_manager.h"
#include "audio_player.h"
#include "system_rom.h"
#include "board_battery.h"
#include "book_reader.h"
#include "app_board.h"
#include "tone_player.h"   /* V1.0.68: 方波直驱音调播放器 (开机音/按键音) */
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
/* V1.0.68: 列表弹窗跟手拖动状态 (番茄钟时间列表等) */
static bool s_list_drag_active = false;
static int  s_list_drag_start_y = 0;
static int  s_list_drag_moved = 0;

void app_main(void)
{
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
    while (1) {
        /* V1.0.68: 软关机键 GPIO1 长按 2 秒软关机 */
        if (input_power_should_sleep()) {
            menu_soft_power_off(&g_lcd);
        }
        menu_check_dialog_timeout();
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
        /* V1.0.68: 软关机键 (GPIO1):
         *  点按 = 锁屏休眠; 0.5s = 弹"返回菜单"提示; 0.5s 后松手 = 返回主菜单; 2s = 关机 */
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
        } else if (action == MENU_ACTION_POWER_HINT) {
            /* 0.5s: 弹出"返回菜单"提示 (持续按住, 2s 变关机) */
            snprintf(g_menu.hint_text, sizeof(g_menu.hint_text),
                     "\xe8\xbf\x94\xe5\x9b\x9e\xe8\x8f\x9c\xe5\x8d\x95");  /* 返回菜单 */
            g_menu.hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 2000;
            g_menu.needs_redraw = true;
            action = MENU_ACTION_NONE;
            has_input = false;
        } else if (action == MENU_ACTION_POWER_RELEASE) {
            /* 松手: 清提示, 立即返回主菜单 */
            g_menu.hint_text[0] = '\0';
            g_menu.hint_until_ms = 0;
            action = MENU_ACTION_HOME;
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
            if (down && !g_menu.main_drag_active) {
                g_menu.main_drag_active = true;
                g_menu.main_drag_start_x = tx;
                g_menu.main_drag_offset = 0;
                s_main_drag_moved = 0;
                g_menu.anim_start_ms = 0;      /* 取消滑动动画, 改由拖动控制 */
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
                    /* 松手: 按拖动距离吸附到对应图标 (支持跨多格, 每 100px 一格, 过半格进位) */
                    g_menu.main_drag_active = false;
                    int off = g_menu.main_drag_offset;
                    g_menu.needs_redraw = true;
                    const int spacing = 100;
                    int steps = 0;
                    if (off < 0) steps = (-off + spacing / 2) / spacing;  /* 左拖 -> 选中右移 */
                    else         steps = -((off + spacing / 2) / spacing); /* 右拖 -> 选中左移 */
                    if (steps != 0) {
                        /* 保持 drag_offset, 让 render 动画期间把它衰减到 0 (无缝过渡) */
                        menu_drag_settle(&g_menu, steps);
                    } else {
                        g_menu.main_drag_offset = 0;   /* 未过半格: 回弹 */
                    }
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
            if (down && !g_menu.select_game_drag_active && tx >= 104) {
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
            if (down && !s_list_drag_active) {
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
        /* V1.0.43: 每5秒读取一次电池电压, 更新状态栏电量 */
        if (now_ms - battery_last_ms >= 5000) {
            battery_last_ms = now_ms;
            board_battery_status_t bat;
            if (board_battery_read(&bat) == ESP_OK) {
                g_menu.settings.battery = bat.percent;
                g_menu.needs_redraw = true;
            } else {
                /* V1.0.68: 未检测到电池 -> 255 哨兵值, 状态栏电池上显示 "?" */
                if (g_menu.settings.battery != 255) {
                    g_menu.settings.battery = 255;
                    g_menu.needs_redraw = true;
                }
            }
        }
        if (now_ms - fps_last_ms >= 1000) {
            if (render_total_us > 0) {
                ESP_LOGI(TAG, "FPS=%lu avg=%lu us",
                         (unsigned long)fps_count,
                         (unsigned long)(render_total_us / fps_count));
            } else {
                ESP_LOGI(TAG, "idle");
            }
            fps_count = 0;
            render_total_us = 0;
            fps_last_ms = now_ms;
        }
    }}
