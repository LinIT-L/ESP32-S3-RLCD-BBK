#include "app_board.h"

#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sd_scan.h"
#include "input.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "bt_manager.h"
#include "audio_player.h"
#include "board_battery.h"

static const char *TAG = "APP_BOARD";

esp_err_t app_board_init(app_board_t *board)
{
    if (!board) return ESP_ERR_INVALID_ARG;
    memset(board, 0, sizeof(*board));

    /* 1. 初始化 LCD (必须最先, 显示开机 Logo) */
    st7305_config_t lcd_cfg = st7305_default_config();
    esp_err_t ret = st7305_init(&board->lcd, &lcd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7305 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "ST7305 初始化成功");

    /* 2. 挂载 TF 卡 (内部 DMA RAM 最干净时优先) */
    esp_err_t sd_ret = sd_mount();
    board->sd_ok = (sd_ret == ESP_OK);
    if (board->sd_ok) {
        ESP_LOGI(TAG, "TF 卡挂载成功");
    } else {
        ESP_LOGW(TAG, "TF 卡挂载失败: %s (进入菜单后可手动格式化)", esp_err_to_name(sd_ret));
    }

    /* 3. 初始化按键 */
    input_init();

    /* 4. 初始化 NVS (蓝牙/WiFi 需要存储校准数据和配置) */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS 初始化成功");

    /* 5. 创建默认事件循环 (esp_hidh 等组件依赖) */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "事件循环创建成功");

    /* 6. 启动 SD 卡监控任务 */
    sd_watcher_start();
    return ESP_OK;
}

void app_board_init_bt(void)
{
    /* 蓝牙栈必须在内部 RAM: esp_bt_controller_init 期间 cache 禁用,
     * PSRAM 栈会触发 esp_task_stack_is_sane_cache_disabled 断言.
     * V1.0.96: 复用 bt_manager 的初始化栈 (内部 RAM 16KB), 避免再占一份. */
#if CONFIG_BT_ENABLED
    int words = 0;
    StackType_t *stack = bt_manager_get_init_stack(&words);
    static StaticTask_t bt_task_buf;
    if (!stack) {
        ESP_LOGE(TAG, "蓝牙初始化栈不可用");
        return;
    }
    TaskHandle_t bt_task = xTaskCreateStatic((TaskFunction_t)bt_manager_init, "bt_init",
                                              words, NULL, 1,
                                              stack, &bt_task_buf);
    if (bt_task == NULL) {
        ESP_LOGE(TAG, "蓝牙初始化任务创建失败!");
    } else {
        ESP_LOGI(TAG, "蓝牙初始化任务已启动 (栈=%u bytes 内部RAM 共享, 优先级=1)",
                 (unsigned)(words * sizeof(StackType_t)));
    }
#else
    ESP_LOGI(TAG, "蓝牙未启用 (CONFIG_BT_ENABLED=n), 跳过 bt_init");
#endif
}

void app_board_init_audio(app_board_t *board)
{
    int audio_ret = audio_player_init();
    board->audio_ok = (audio_ret == 0);
    if (board->audio_ok) {
        ESP_LOGI(TAG, "音频硬件初始化成功");
    } else {
        ESP_LOGW(TAG, "音频初始化返回 %d", audio_ret);
    }
}

void app_board_init_battery(app_board_t *board)
{
    esp_err_t bat_ret = board_battery_init();
    board->battery_ok = (bat_ret == ESP_OK);
    if (board->battery_ok) {
        ESP_LOGI(TAG, "电池 ADC 初始化成功 (GPIO4, 分压200K/100K)");
    } else {
        ESP_LOGW(TAG, "电池 ADC 初始化失败: %s", esp_err_to_name(bat_ret));
    }
}
