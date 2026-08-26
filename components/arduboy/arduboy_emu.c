/**
 * @file arduboy_emu.c
 * @brief Arduboy 模拟器入口 - 游戏选择与主循环
 *
 * 主循环 (30fps, ~33ms/帧):
 *   1. arduboy_poll_input()       采样按键, 计算 held/edge
 *   2. 检测 BOOT 长按 > 1s         退出游戏
 *   3. game->update()              逻辑更新
 *   4. arduboy_clear()             清 fb
 *   5. game->render()              绘制到 128x64 fb
 *   6. arduboy_display()           缩放 blit 到 ST7305 + flush
 *   7. vTaskDelay(33ms)            帧率控制
 */
#include "arduboy_emu.h"
#include "arduboy.h"
#include "input.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ARDUBOY";

/* 各游戏实现 (由 game_*.c 提供) */
extern const arduboy_game_impl_t breakout_game;
extern const arduboy_game_impl_t snake_game;

static const arduboy_game_impl_t *const s_games[ARDUBOY_GAME_COUNT] = {
    [ARDUBOY_GAME_BREAKOUT] = &breakout_game,
    [ARDUBOY_GAME_SNAKE]    = &snake_game,
};

static st7305_handle_t *s_lcd = NULL;
static const arduboy_game_impl_t *s_current = NULL;
static volatile bool s_running = false;

/* BOOT 长按退出阈值 (毫秒) */
#define EXIT_LONG_PRESS_MS  1000
/* 帧间隔 (毫秒), 约 30fps */
#define FRAME_INTERVAL_MS   33

void arduboy_emu_init(st7305_handle_t *lcd) {
    s_lcd = lcd;
    arduboy_init(lcd);
    s_current = NULL;
    s_running = false;
    ESP_LOGI(TAG, "Arduboy 模拟器初始化完成");
}

void arduboy_emu_select_game(arduboy_game_id_t game) {
    if (game < 0 || game >= ARDUBOY_GAME_COUNT) {
        ESP_LOGE(TAG, "无效游戏 ID: %d", (int)game);
        s_current = NULL;
        return;
    }
    s_current = s_games[game];
    ESP_LOGI(TAG, "已选择游戏: %s", s_current ? s_current->name : "(null)");
}

void arduboy_emu_run(void) {
    if (!s_current) {
        ESP_LOGE(TAG, "未选择游戏, 无法运行");
        return;
    }
    if (!s_lcd) {
        ESP_LOGE(TAG, "未初始化 (lcd=NULL)");
        return;
    }

    ESP_LOGI(TAG, "开始运行: %s (BOOT 长按 %dms 退出, %dms/帧)",
             s_current->name, EXIT_LONG_PRESS_MS, FRAME_INTERVAL_MS);

    /* 初始化游戏 */
    s_current->init();

    s_running = true;
    uint32_t boot_hold_ms = 0;
    uint32_t last_ms = arduboy_millis();
    uint32_t fps_count = 0;
    uint32_t fps_last = last_ms;

    while (s_running) {
        uint32_t now = arduboy_millis();

        /* 1. 采样按键 */
        arduboy_poll_input();

        /* 2. 检测 BOOT 长按退出 (用 input_is_held 直接读电平, 与 edge 独立) */
        if (input_is_held(1)) {
            boot_hold_ms += now - last_ms;
            if (boot_hold_ms >= EXIT_LONG_PRESS_MS) {
                ESP_LOGI(TAG, "BOOT 长按, 退出游戏");
                break;
            }
        } else {
            boot_hold_ms = 0;
        }
        last_ms = now;

        /* 3. 逻辑更新 */
        s_current->update();

        /* 4-5. 清屏 + 渲染 */
        arduboy_clear();
        s_current->render();

        /* 6. 缩放 blit 到 ST7305 */
        arduboy_display();

        /* 7. 帧率控制 + 看门狗喂狗 */
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(FRAME_INTERVAL_MS));

        /* FPS 统计 */
        fps_count++;
        uint32_t cur = arduboy_millis();
        if (cur - fps_last >= 1000) {
            ESP_LOGI(TAG, "%s FPS=%lu", s_current->name,
                     (unsigned long)fps_count);
            fps_count = 0;
            fps_last = cur;
        }
    }

    s_running = false;
    ESP_LOGI(TAG, "游戏退出: %s", s_current->name);
}

void arduboy_emu_stop(void) {
    s_running = false;
}

const char *arduboy_emu_get_game_name(int idx) {
    if (idx < 0 || idx >= ARDUBOY_GAME_COUNT) return NULL;
    return s_games[idx] ? s_games[idx]->name : NULL;
}

int arduboy_emu_get_game_count(void) {
    return ARDUBOY_GAME_COUNT;
}
