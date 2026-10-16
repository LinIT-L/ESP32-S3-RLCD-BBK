#include "app_board.h"
#include "gps.h"          /* GPS (MAX-M10S) 驱动 API: gps_init/gps_sleep */

#include "esp_log.h"
#include "board_pins.h"   /* 板型表: 引脚下发 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "sd_scan.h"
#include "input.h"
#include "svc_nvs.h"
#include "esp_event.h"
#include "bt_manager.h"
#include "usb_bt.h"
#include "usb_net.h"
#include "audio_player.h"
#include "board_battery.h"

static const char *TAG = "APP_BOARD";

/* 后台 TF 卡挂载 (见 app_board_init 第 2 步注释): 不阻塞启动路径。
 * board 是调用方(main)的静态结构, 生命周期覆盖本任务, 故直接持指针。 */
static void sd_mount_task(void *arg)
{
    app_board_t *b = (app_board_t *)arg;
    esp_err_t r = sd_mount();
    if (b) b->sd_ok = (r == ESP_OK);
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "TF 卡挂载成功 (后台异步)");
    } else {
        ESP_LOGW(TAG, "TF 卡挂载失败: %s (进入菜单后可手动格式化)", esp_err_to_name(r));
    }
    vTaskDelete(NULL);
}

esp_err_t app_board_init(app_board_t *board)
{
    if (!board) return ESP_ERR_INVALID_ARG;
    memset(board, 0, sizeof(*board));

    /* 0. 释放深睡时 gpio_hold 锁住的整条 LCD 控制线(41/42/11/12): 深睡为保持猫咪有意锁定.
     * 若不释放, st7305_init 的硬复位脚翻转不生效 + SPI 总线被 hold 霸住无法初始化.
     * ★ 2026-10-04: CS 从 GPIO40 挪到 **GPIO42** (小智核心模块占用 IO40)。这两处必须同步改,
     *   否则 CS 被 hold 住 → SPI 片选永远拉不动 → 屏全黑。 */
    /* ★★★ 2026-10-16: 解锁列表必须是**本板实际用的引脚**(漏改=屏全黑但串口正常)。
     *   小智: LCD DC7/CS11/SCK9/MOSI10 · 震动8 · 触摸5/13/12/6 · 按键1/0/4 */
    /* ★ 2026-10-16: 解锁列表**按板型表**给出(小智/微雪/自制各自不同), 不再写死。 */
    {
        const board_pins_t *bp0 = board_pins_get();
        gpio_hold_dis((gpio_num_t)bp0->lcd_dc);
        gpio_hold_dis((gpio_num_t)bp0->lcd_cs);
        gpio_hold_dis((gpio_num_t)bp0->lcd_sck);
        gpio_hold_dis((gpio_num_t)bp0->lcd_mosi);
        if (bp0->lcd_rst >= 0) gpio_hold_dis((gpio_num_t)bp0->lcd_rst);
        gpio_hold_dis((gpio_num_t)bp0->btn_pwr);
        gpio_hold_dis((gpio_num_t)bp0->btn_right);
        gpio_hold_dis((gpio_num_t)bp0->btn_left);
        if (bp0->tp_sda >= 0) {
            gpio_hold_dis((gpio_num_t)bp0->tp_sda); gpio_hold_dis((gpio_num_t)bp0->tp_scl);
            gpio_hold_dis((gpio_num_t)bp0->tp_int); gpio_hold_dis((gpio_num_t)bp0->tp_rst);
        }
        gpio_hold_dis((gpio_num_t)(BOARD_IS_WEIXUE ? 48 : 8));   /* 震动 */
    }

    /* 1. 初始化 LCD (必须最先, 显示开机 Logo) */
    /* ★★★ 2026-10-16 关键修复: 从板型表把引脚**下发**给驱动。
     *   之前这段被 `git checkout` 误清掉 ⇒ 屏一直用驱动默认值(小智的 7/11/9/10)
     *   去驱微雪的屏(5/40/11/12) ⇒ 全黑。v1.3(能亮)的电池日志是 GPIO4、我们却是 GPIO3,
     *   就是"没下发"的铁证。 */
    const board_pins_t *bp = board_pins_get();
    st7305_config_t lcd_cfg = st7305_default_config();
    lcd_cfg.dc_gpio  = bp->lcd_dc;
    lcd_cfg.spi_cs   = bp->lcd_cs;
    lcd_cfg.spi_sclk = bp->lcd_sck;
    lcd_cfg.spi_mosi = bp->lcd_mosi;
    lcd_cfg.rst_gpio = bp->lcd_rst;
    if (bp->lcd_freq_mhz > 0) lcd_cfg.spi_freq = bp->lcd_freq_mhz;
    audio_player_apply_pins(bp->i2s_mclk, bp->i2s_bclk, bp->i2s_ws, bp->i2s_dout,
                            bp->i2s_din, bp->codec_sda, bp->codec_scl, bp->pa_pin);
    board_battery_set_channel(bp->bat_adc_chan);
    if (bp->sd_bus == 0) sd_scan_apply_pins(bp->sd_clk, bp->sd_cmd, bp->sd_d0);
    ESP_LOGW(TAG, "板型=%s → LCD DC=%d CS=%d SCK=%d MOSI=%d RST=%d @%dMHz",
             bp->name, lcd_cfg.dc_gpio, lcd_cfg.spi_cs, lcd_cfg.spi_sclk,
             lcd_cfg.spi_mosi, lcd_cfg.rst_gpio, lcd_cfg.spi_freq);
    esp_err_t ret = st7305_init(&board->lcd, &lcd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7305 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "ST7305 初始化成功");
    /* ★★★ 2026-10-16 关键修复(第2处): 屏保门禁。
     *   wallpapers.c 屏保激活时会 st7305_set_flush_blocked(true), 此后**所有普通
     *   st7305_flush 在第一行就 return ESP_OK(静默丢弃)** ⇒ 屏收不到任何数据 ⇒ 全黑且无报错。
     *   开机这里强制打开一次, 保证 boot 阶段一定刷得出去。 */
    st7305_set_flush_blocked(false);

    /* 2. 挂载 TF 卡 —— V1.6.x: 改为**后台异步**。
     * 原同步挂载在"没插卡"时会跑满 3 次重试(实测 ~490ms), 白占启动路径;
     * 丢到小任务里做, 启动立即继续, board->sd_ok 稍后由任务置位。
     * (sd_watcher_start 本来就有补挂逻辑, 这里只是不让首次挂载等它 15s 的补挂周期。) */
    board->sd_ok = false;
    if (xTaskCreate(sd_mount_task, "sd_mnt", 4096, board, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "SD 挂载任务创建失败, 回退为同步挂载");
        esp_err_t sd_ret = sd_mount();
        board->sd_ok = (sd_ret == ESP_OK);
    }

    /* 3. 初始化按键 */
    input_init();

    /* 4. 初始化 NVS (蓝牙/WiFi 需要存储校准数据和配置) */
    ret = svc_nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(svc_nvs_flash_erase());
        ret = svc_nvs_flash_init();
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
    /* USB-BT / USB网卡 激活时: 蓝牙控制器让出 (BT 供适配器独占 / 网卡避免与 WiFi 争用),
     * 不初始化手柄蓝牙 */
    if (usb_bt_is_active() || usb_net_is_active()) {
        ESP_LOGI(TAG, "USB-BT/USB网卡激活: 跳过手柄蓝牙初始化 (控制器让出)");
        return;
    }
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
        ESP_LOGI(TAG, "电池 ADC 初始化成功 (细节见 board_battery 的日志)");
    } else {
        ESP_LOGW(TAG, "电池 ADC 初始化失败: %s", esp_err_to_name(bat_ret));
    }
}

/* 阶段5: GPS (u-blox MAX-M10S, UART). 见 gps.h 的接线与 API 说明. */
void app_board_init_gps(app_board_t *board)
{
    esp_err_t ret = gps_init(NULL);   /* NULL = 驱动内置默认配置 (43/44 ↔ GPS, EXTINT 47, 921600) */
    board->gps_ok = (ret == ESP_OK);
    if (!board->gps_ok) {
        ESP_LOGW(TAG, "GPS 驱动初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    /* 模块 VCC 常通电而固件当前不用定位 → 立刻送软件待机, 把 25~30mA 降到 µA 级.
     * 顺带减轻 3V3 负载 (与"SD/WiFi 负载突变掉电复位"是同一类隐患). */
    gps_sleep();
}
