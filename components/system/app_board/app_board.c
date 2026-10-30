#include "esp_timer.h"
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
    /* ★★★ 2026-10-28 linit 要求: GPS **前 3 次开机各探测一次**, 之后按结果定终身。
     *   ① 探测到(收到过有效帧) ⇒ 以后不再"探测", 但每次开机仍要停靠 ——
     *      GPS 的 VCC 常通、而 backup 模式不保留配置, ESP 重启后它是醒的,
     *      不送待机就是 10~25mA 空转;
     *   ② 3 次都没探测到 ⇒ 认为本板没装 GPS, 以后**一个引脚都不碰**
     *      (否则会去 hold/isolate 一个可能被别的功能占用的脚);
     *   ③ 计数存 NVS 跨重启保留; 用户重新焊了模块, 可在 设置→硬件 里点 GPS 重新识别
     *      (那条路径会清掉 gps_probe_n / gps_found 两个 key)。
     *   为什么要有这个: 每次开机都跑一遍探测 = 1.8s 静听 + 最多 3.2s 静默判据,
     *   对一块根本没装 GPS 的板子纯属浪费启动时间。 */
    uint8_t probe_n = 0, found = 0;
    nvs_handle_t h;
    if (svc_nvs_open("hwcfg", NVS_READONLY, &h) == ESP_OK) {
        svc_nvs_get_u8(h, "gps_probe_n", &probe_n);
        svc_nvs_get_u8(h, "gps_found",   &found);
        svc_nvs_close(h);
    }

    if (found) {
        /* 已知有 GPS: 直接停靠, 不再做"有没有"的判断 */
        esp_err_t ret = gps_boot_park();
        board->gps_ok = (ret == ESP_OK);
        if (ret != ESP_OK) ESP_LOGW(TAG, "GPS 开机停靠失败: %s", esp_err_to_name(ret));
        return;
    }
    if (probe_n >= 3) {
        /* ★★★ 2026-10-28 重要修正: 探测满 3 次只停止"探测/统计", **绝不能停止停靠**。
         *   GPS 的 VCC 是常通的, 而 backup 模式不保留配置 ⇒ **每次开机它都是醒的**,
         *   不送待机就是 10~25mA 空转。这是功耗问题, 不是启动时间问题, 一分都不能省。
         *   (第一版写成"3 次后一个引脚都不碰", 那会在没探测到模块时把 GPS 丢在醒着状态,
         *    功耗比"每次多花 2 秒探测"严重得多。) */
        esp_err_t ret = gps_boot_park();
        board->gps_ok = (ret == ESP_OK);
        if (ret != ESP_OK) ESP_LOGW(TAG, "GPS 开机停靠失败: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "GPS: 探测已满 %u 次(未确认存在), 本次只停靠不再统计", (unsigned)probe_n);
        return;
    }

    /* 第 N 次探测 */
    uint8_t n = (uint8_t)(probe_n + 1);
    esp_err_t ret = gps_boot_park();
    board->gps_ok = (ret == ESP_OK);

    bool seen = gps_seen_valid_frame();
    if (seen) {
        found = 1;
        ESP_LOGW(TAG, "GPS: 第 %u 次探测**确认存在**(收到有效帧) ⇒ 以后不再探测, 只做停靠",
                 (unsigned)n);
    } else {
        ESP_LOGW(TAG, "GPS: 第 %u/3 次探测未收到有效帧(模块可能在待机, 或本板未安装)", (unsigned)n);
    }
    if (svc_nvs_open("hwcfg", NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_set_u8(h, "gps_probe_n", n);
        if (found) svc_nvs_set_u8(h, "gps_found", 1);
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
    if (ret != ESP_OK) ESP_LOGW(TAG, "GPS 开机停靠失败: %s", esp_err_to_name(ret));
}

/* 旧名兼容: 只做一次停靠就退出(不等价于管家, 管家是常驻的) */
void board_gps_boot_task(void *arg)
{
    app_board_init_gps((app_board_t *)arg);
    vTaskDelete(NULL);
}

/* ★ 2026-10-20 启动提速: GPS 初始化搬到后台任务跑 (main.c 里创建)。
 *   原因: 波特率自适应 ~1.4s + 送待机 ~0.6s, 同步跑会把桌面拖到 3.9s。
 *   board 指针来自 app_main 的栈帧, 而 app_main 进主循环后不返回 ⇒ 生命周期安全。 */
/* ═══════════════════════════════════════════════════════════════════════════
 * ★★★ 2026-10-27 linit: GPS **管家**任务（原来只是一次性开机停靠）
 *   需求原话: "开机的时候他就要进行睡眠。只有我开 GPS 类软件的时候才会启用"、
 *            "可以重复的让他睡眠，可以用管家模块去管理他"。
 *   行为:
 *     ① 开机立刻停靠一次（gps_boot_park: 静听 300ms, 醒着才发 PMREQ）
 *     ② 之后每 30s 巡检一次:
 *          - 应用正在用(app.gps.start 起了解析服务) ⇒ 让路, 绝不打扰
 *          - 已确认入睡(gps_sleep_verified) ⇒ 什么都不做（不装 UART, 免得重配 pad 把它叫醒）
 *          - 其余情况(被噪声唤醒 / 上次没睡成 / 应用退出漏调 stop) ⇒ 再停靠一次
 *     ③ 连续失败 5 次后改成 5 分钟一次并在日志里降噪 —— 不是放弃, 是不刷屏。
 *   为什么必须"重复": 只要 GPS 的 RXD 上出现一个边沿它就醒(热插拔/干扰/别的任务碰脚),
 *   醒了自己不会再睡, 而 PMREQ 是一次性指令 ⇒ 必须有人长期盯着。
 * ═══════════════════════════════════════════════════════════════════════════ */
/* ★★ 2026-10-28 linit 否决"定期轮询复查"这个做法 ⇒ **默认关闭(0)**。
 *   理由: 轮询本身要装 UART, 装 UART 会重配 TX pad, 那一下电平跳变正是模块的唤醒条件
 *   (Data Sheet Table 10: 待机时 RXD 仍为上拉输入, 且是唤醒源) —— 拿"吵醒它"的办法去
 *   检查"它睡没睡", 方法本身就是错的。
 *   只在现场调试时用 remcon `gpsrecheck <秒>` 临时打开。 */
static int s_gps_recheck_ms = 0;
void board_gps_set_recheck_sec(int sec)
{
    if (sec < 10) sec = 10;                 /* 太密会一直占 UART, 反而把模块吵醒 */
    if (sec > 3600) sec = 3600;
    s_gps_recheck_ms = sec * 1000;
}
#define GPS_GUARD_PERIOD_MS   30000   /* 未确认时的巡检间隔 */
#define GPS_GUARD_RETRY_MS     5000   /* ★ 刚失败时 5 秒就再来一次(抓到 TX 偶发通的窗口) */
#define GPS_GUARD_BACKOFF_MS  300000   /* 连续失败太多次后降到 5 分钟, 只降噪不放弃 */
#define GPS_GUARD_FAIL_MAX    3

void board_gps_guard_task(void *arg)
{
    app_board_t *b = (app_board_t *)arg;
    extern bool gps_service_is_running(void);
    extern bool gps_sleep_verified(void);

    /* ① 开机立刻停靠 */
    app_board_init_gps(b);
    ESP_LOGI(TAG, "GPS 管家: 开机停靠完成 (ok=%d, verified=%d)",
             b ? (int)b->gps_ok : -1, (int)gps_sleep_verified());

    int fails = 0;
    int64_t last_check = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(fails >= GPS_GUARD_FAIL_MAX ? GPS_GUARD_BACKOFF_MS
                                    : (fails > 0 ? GPS_GUARD_RETRY_MS : GPS_GUARD_PERIOD_MS)));
        if (gps_service_is_running()) {          /* 应用在用 → 让路 */
            fails = 0;
            continue;
        }
        /* ★★★ 2026-10-28 关键修复(实测: 整夜掉 424mV ≈ 30mA) ★★★
         *   原来这里写的是 `if (gps_sleep_verified()) continue;` —— 把"确认过一次"当成永久事实。
         *   但模块**待机时 RXD 仍是上拉输入**(Data Sheet Table 10), 而唤醒源正是 UART RX:
         *   任何一点线上噪声/别的任务碰引脚都能把它叫醒, 醒了不会自己再睡, 而 PMREQ 是一次性指令。
         *   实测证据: PWRSTAT 睡眠占比 96.8% (7.0h), 但 24h 电压 4190→3760mV (-53.4mV/h ≈ 29.7mA)
         *   ⇒ ESP 在睡, GPS 整夜在跑。
         *   现在: 即使已确认入睡, 也**每 GPS_GUARD_RECHECK_MS 复查一次**(静听 1.8s, 醒着就重送 PMREQ)。
         *   代价: 每次约 2 秒/10 分钟 ≈ 0.3% 占空比, 换来不会整夜漏 25mA。 */
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool need_check = !gps_sleep_verified() ||
                          (s_gps_recheck_ms > 0 && (now_ms - last_check) >= s_gps_recheck_ms);
        if (!need_check) { fails = 0; continue; }
        last_check = esp_timer_get_time() / 1000;
        if (gps_sleep_verified())
            ESP_LOGI(TAG, "GPS 管家: 定期复查(间隔 %d 秒)", s_gps_recheck_ms / 1000);
        else
            ESP_LOGW(TAG, "GPS 管家: 模块不是待机状态 ⇒ 再送一次 PMREQ");
        app_board_init_gps(b);
        if (gps_sleep_verified()) {
            fails = 0;
            ESP_LOGI(TAG, "GPS 管家: 已重新送回待机");
        } else {
            fails++;
            if (fails <= GPS_GUARD_FAIL_MAX || (fails % 10) == 0) {
                ESP_LOGW(TAG, "GPS 管家: 第 %d 次仍未确认入睡 (模块可能不支持软件待机, "
                              "或 RXD 侧持续被干扰); 之后每 %d 分钟再试",
                         fails, GPS_GUARD_BACKOFF_MS / 60000);
            }
        }
    }
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
    extern bool gps_sleep_verified(void);
    gps_sleep();                      /* 焊了: 立刻送软件待机 (46µA) */
    bool slept = gps_sleep_verified();
    gps_release();                    /* 卸 UART + 归还内部 RAM; 未睡成时会再试一次 */
    ESP_LOGW(TAG, "GPS 识别: 已识别到模块, 待机确认=%s", slept ? "是" : "否(管家会继续重试)");
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
    /* ★ 2026-10-28: 用户手动"重新识别 GPS" ⇒ 必须把"前 3 次探测"的账一起清零。
     *   否则探到过一次之后 gps_found 恒为 1, 以后开机都不再探测,
     *   用户点了"重新识别"也看不到"重新判断"的效果(只是又停靠了一次)。 */
    nvs_handle_t h;
    if (svc_nvs_open("hwcfg", NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_erase_key(h, "gps_probe_n");
        svc_nvs_erase_key(h, "gps_found");
        svc_nvs_commit(h);
        svc_nvs_close(h);
        ESP_LOGW(TAG, "GPS 重新识别: 已清零探测计数/发现标记");
    }
    if (xTaskCreate(gps_probe_task, "gps_probe", 4096, NULL, 2, NULL) != pdPASS)
        hw_cfg_set_gps_present(app_board_gps_probe());   /* 建任务失败就同步探 */
}
