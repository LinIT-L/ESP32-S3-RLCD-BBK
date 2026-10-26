#include "app_board.h"
#include "gps.h"          /* GPS (MAX-M10S) 驱动 API: gps_init/gps_sleep */

#include "esp_log.h"
#include "board_pins.h"
#include "hw_cfg.h"   /* 板型表: 引脚下发 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "sd_scan.h"
#include "board_pins.h"
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
    } else if (r == ESP_FAIL) {
        /* sd_mount 用 ESP_FAIL 表示"quick_probe 没探到卡"(不是文件系统坏) —— 别提示格式化 */
        ESP_LOGI(TAG, "未检测到 TF 卡 (未插卡或卡座接触不良)");
    } else {
        /* ★ 2026-10-18: 卡被识别但挂不上(文件系统不认/需要格式化)走这里, 原因见 SDSCAN 日志 */
        ESP_LOGW(TAG, "TF 卡已识别但挂载失败: %s (见 SDSCAN 的判定行)", esp_err_to_name(r));
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
        /* ★★★★ 2026-10-18 linit **严重 bug 修复**: `gpio_deep_sleep_hold_en()` 是**全局锁存开关**,
         *   只逐个 `gpio_hold_dis(pin)` 是解不掉的 ✗
         *   ⇒ 必须**先** `gpio_deep_sleep_hold_dis()` 关掉全局锁存, 再逐脚释放 ✓
         *   症状(只在"睡过一觉后"出现): **按键读错电平 + 触摸失灵** ✓✓
         *   (深睡时 sleep.c 调了 gpio_deep_sleep_hold_en(), 醒来这里从没关过 ✗) */
        gpio_deep_sleep_hold_dis();

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
        if (bp0->vib >= 0) gpio_hold_dis((gpio_num_t)bp0->vib);   /* 震动 (按板型表, 8/48) */
        /* ★★★ 2026-10-20 严重 bug 修复: deep sleep 前把 **SD 总线**也 hold 住了
         *   (CLK 钉低 / CMD+D0 钉高), 但这里从来没释放 ⇒ 只要睡过一次再重启(烧录也算),
         *   SD 三个脚就一直被冻结: 卡收不到时钟、CMD 被硬拉高 ⇒ `sdmmc_io_reset 0x108`,
         *   表现为"TF 卡认不到, 必须断电重启"(只有真掉电才会清掉 RTC 里的 hold)。
         *   ⇒ 开机必须像 LCD/触摸那样把 SD 脚一起解锁。 */
        if (bp0->sd_clk >= 0) gpio_hold_dis((gpio_num_t)bp0->sd_clk);
        if (bp0->sd_cmd >= 0) gpio_hold_dis((gpio_num_t)bp0->sd_cmd);
        if (bp0->sd_d0  >= 0) gpio_hold_dis((gpio_num_t)bp0->sd_d0);
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
    ESP_LOGW("APP_BOARD", "★下发配置: DC=%d CS=%d SCK=%d MOSI=%d RST=%d @%dMHz (板=%s)",
             lcd_cfg.dc_gpio, lcd_cfg.spi_cs, lcd_cfg.spi_sclk, lcd_cfg.spi_mosi,
             lcd_cfg.rst_gpio, lcd_cfg.spi_freq, bp->name);
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

    /* ★★★ 2026-10-18 linit: **微雪版 LCD RST(GPIO41) 必须常拉高** ——
     *   微雪 RLCD 面板的 RESET 是"高有效(释放)"脚, 初始化完成后必须由固件
     *   强制输出高电平并保持; 否则面板可能回到未释放状态 ⇒ 花屏/全黑/触摸异常。
     *   ⛔ **只有微雪这么做** —— 小智模块的屏 RST 不接(lcd_rst = -1), 标准扩展也不同 ✓
     *   ⛔ 睡眠时例外: sleep.c 深睡前会把它拉低省电(屏已 deinit) ✓ */
    {
        const board_pins_t *bpw = board_pins_get();
        if (bpw->id == BOARD_ID_WEIXUE && bpw->lcd_rst >= 0) {
            gpio_set_direction((gpio_num_t)bpw->lcd_rst, GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)bpw->lcd_rst, 1);      /* 保持高 = 释放复位 */
            ESP_LOGW(TAG, "微雪: LCD RST(GPIO%d) 已强制拉高并保持", bpw->lcd_rst);
        }
    }
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
if (hw_cfg_enabled(HW_SD)) {
    if (xTaskCreate(sd_mount_task, "sd_mnt", 4096, board, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "SD 挂载任务创建失败, 回退为同步挂载");
        esp_err_t sd_ret = sd_mount();
        board->sd_ok = (sd_ret == ESP_OK);
    }
}

    /* 3. 初始化按键 */
if (hw_cfg_enabled(HW_TOUCH) || hw_cfg_enabled(HW_SD)) { input_init(); }   /* 按键总在; 触摸由 input 内部按指纹判 */

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
if (hw_cfg_enabled(HW_SD)) sd_watcher_start();
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
    /* 带上内部堆余量：I2S DMA 只能从内部 RAM 拿，余量不够就会"显示正常但完全没声" */
    if (board->audio_ok) {
        ESP_LOGI(TAG, "音频硬件初始化成功（内部堆余 %u B）",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    } else {
        ESP_LOGW(TAG, "音频初始化失败 ret=%d（内部堆余 %u B）", audio_ret,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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
    /* ★★★ 2026-10-24 开机只做"停靠", 不做完整初始化:
     *   开机唯一目的是别让模块 10mA 空转 —— 不需要 autobaud、不需要 GSV 提频、
     *   不需要解析服务。gps_boot_park() 静听 300ms: 没输出=本来就在睡(不动),
     *   有输出=发 PMREQ 送睡, 乱码=完整初始化兜底; 结束时 UART 已卸载、内部 RAM 归零。
     *   应用要定位时 app.gps.start() 会自己 gps_init()+gps_wakeup() 全套重来。
     *   实测: 原同步路径 900~4200ms → 现在 ~320ms, 且不占任何常驻资源。 */
    esp_err_t ret = gps_boot_park();
    board->gps_ok = (ret == ESP_OK);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GPS 开机停靠失败: %s", esp_err_to_name(ret));
    }
}

/* ★ 2026-10-20 启动提速: GPS 初始化搬到后台任务跑 (main.c 里创建)。
 *   原因: 波特率自适应 ~1.4s + 送待机 ~0.6s, 同步跑会把桌面拖到 3.9s。
 *   board 指针来自 app_main 的栈帧, 而 app_main 进主循环后不返回 ⇒ 生命周期安全。 */
void board_gps_boot_task(void *arg)
{
    app_board_t *b = (app_board_t *)arg;
    app_board_init_gps(b);
    ESP_LOGI(TAG, "GPS 后台初始化完成 (gps_ok=%d)", b ? (int)b->gps_ok : -1);
    vTaskDelete(NULL);
}

/* ★★ GPS 一次性识别 (写完固件的第一次开机才跑一次, 见 hw_cfg_gps_probed):
 *   真探一次 UART —— 听得到 UBX/NMEA 才算"焊了", 否则 gps_init 内部会把 UART 卸掉、
 *   引脚还原并返回 ESP_ERR_NOT_FOUND (不留后台耗电)。 */
bool app_board_gps_probe(void)
{
    esp_err_t r = gps_init(NULL);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "GPS 识别: 未识别到模块 (%s)", esp_err_to_name(r));
        return false;
    }
    gps_sleep();                      /* 焊了: 立刻送软件待机 (46µA) */
    ESP_LOGW(TAG, "GPS 识别: 已识别到模块, 已送待机");
    return true;
}

static void gps_probe_task(void *arg)
{
    (void)arg;
    hw_cfg_set_gps_present(app_board_gps_probe());
    vTaskDelete(NULL);
}

void app_board_gps_probe_async(void)
{
    if (xTaskCreate(gps_probe_task, "gps_probe", 4096, NULL, 2, NULL) != pdPASS)
        hw_cfg_set_gps_present(app_board_gps_probe());   /* 建任务失败就同步探 */
}
