/**
 * @file touch_panel.c
 * @brief 电容触摸屏驱动: 自动识别 GT911 / CST816 / FT6236
 *
 * 三种芯片都是 I2C 从机, 读到的都是"点数 + 第一个点的 XY 坐标".
 * 本驱动只读点, 手势识别在 input.c 里做 (按坐标增量判方向, 与分辨率无关).
 *
 * I2C 使用独立的 I2C_NUM_1 (音频 ES8311/ES7210 占用 I2C_NUM_0), 互不干扰.
 */
#include "touch_panel.h"
#include "driver/i2c.h"          /* 仅保留 I2C_NUM_x 常量 */
#include "driver/i2c_master.h"   /* ★ 新驱动: i2c_new_master_bus / i2c_master_probe */
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

/* ---- 本板触摸引脚 (★ 2026-10-16: 由上层 touch_panel_set_pins() 设定) ----
 * 默认值 = 原来写死的小智模块板接线; 微雪原板由 input.c 从 board_pins.h 下发。 */
static tp_pins_t s_pins = { -1, -1, -1, -1 };   /* ⛔ 未配置 → init 直接跳过, 一个脚都不碰 */

void touch_panel_set_pins(const tp_pins_t *pins) {
    if (pins) s_pins = *pins;
}


#define TAG "TOUCH"

/* 常驻极值统计 (只做 4 次比较, 不影响性能) —— 串口 `tpcal` 随时查看/清零。
 * 好处: 不卡时间窗口, 用户什么时候摸都能累积下来。 */
static int s_cal_minx = 99999, s_cal_maxx = -1;
static int s_cal_miny = 99999, s_cal_maxy = -1;
static int s_cal_n = 0;

#define TP_I2C_PORT   I2C_NUM_1
#define TP_I2C_FREQ   (100 * 1000)   /* 飞线 100kHz 最稳; 稳定后可选 400kHz */

/* ==================== 后台触摸看门狗 (V1.0.68) ====================
 * 问题: 触摸芯片挂死/总线卡住时旧逻辑在 UI 任务里直接做恢复 (RST+重装I2C,
 * 阻塞 ~70-150ms) → 滑动中停顿"卡"; 重装 I2C 驱动与读取竞态还可能触发
 * 驱动断言 → 整机重启.
 * 方案: 独立看门狗任务检测异常 → 先置 s_recovering (读取立即返回, 不阻塞
 * UI) → 等总线空闲后由看门狗任务执行恢复. 恢复带节流, 防抖. */
#define WD_CHECK_MS        500      /* 看门狗检查周期 */
#define WD_FAIL_HARD       10       /* 窗口内失败 ≥ 此数 → 强制恢复 */
#define WD_READS_MIN       20       /* 窗口内读取 ≥ 此数才看失败率 */
#define WD_RATE_DIV        2        /* 失败率 > 1/WD_RATE_DIV → 软恢复 */
#define WD_STUCK_MS        10000    /* 坐标 10s 不变且按住 → 芯片卡死 */
#define WD_MAX_RECOVER     6        /* 3 分钟内最多恢复次数 */
#define WD_THROTTLE_MS     180000
#define WD_RECOVER_WAIT    200      /* 等总线空闲最长 200ms */

static volatile int      s_busy = 0;          /* 读取任务占用计数 */
static volatile bool     s_recovering = false;/* 恢复中: 读取免阻塞返回 */
static volatile bool     s_recover_request = false; /* input 请求恢复 */
static uint32_t          s_wd_win_reads = 0, s_wd_win_fails = 0;
static uint32_t          s_wd_stuck_ms = 0;
static int16_t           s_wd_last_x = -1, s_wd_last_y = -1;
static uint32_t          s_wd_last_ms = 0;
static uint32_t          s_wd_recover_times[WD_MAX_RECOVER] = {0};
static int               s_wd_recover_idx = 0;
static bool              s_wd_window_full = false;   /* 恢复记录窗口是否已填满(空位拉低"最旧", 会误判超频) */
static TaskHandle_t      s_wd_task = NULL;
static volatile bool     s_wd_paused = false;        /* [测试版] 浅睡窗口内暂停看门狗 */

static void touch_watchdog_task(void *arg);   /* 前向声明 (init 里启动) */
static void touch_panel_soft_recover(void);   /* 前向声明 */
static bool wd_wait_idle(void);

/* 7-bit 从机地址 */
#define GT911_ADDR_A   0x5D
#define GT911_ADDR_B   0x14
#define CST816_ADDR    0x15
#define FT6236_ADDR    0x38

/* GT911 寄存器 (16-bit 寄存器地址) */
#define GT911_REG_PID      0x8140   /* 产品 ID, 应为 "911" */
#define GT911_REG_STATUS   0x814E   /* bit7=数据就绪, bit0-3=点数 */
#define GT911_REG_POINT1   0x814F   /* 第 1 个点, 8 字节 */
#define GT911_REG_RES      0x8048   /* X/Y 分辨率 (0x8048/49=X, 0x804A/4B=Y) */

static tp_chip_t s_chip = TP_CHIP_NONE;
static bool s_cst836 = false;   /* true=CST836U(支持双点), false=CST816S(仅单点) */
static uint8_t    s_addr = 0;
static int        s_res_x = TP_DEFAULT_RES_X;
static int        s_res_y = TP_DEFAULT_RES_Y;

/* ★ FT6236 方向参数 (运行时, NVS 落盘): bit0 = 交换 XY, bit1 = Y 取反。
 *   默认 3 = 鱼星 RLCD 4.2(90° 旋转安装) 需要的组合。
 *   ⛔ 只作用于 ft6236_read_points, CST836U / GT911 各走各的读点函数, 完全不受影响。 */
static uint8_t s_ft_axis = 3;
static bool    s_ft_axis_loaded = false;

static void ft_axis_load(void) {
    if (s_ft_axis_loaded) return;
    s_ft_axis_loaded = true;
    nvs_handle_t h;
    uint8_t v = 3;
    if (nvs_open("os_tp", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "ft_axis", &v); nvs_close(h); }
    if (v <= 3) s_ft_axis = v;
}

uint8_t touch_panel_get_ft_axis(void) { ft_axis_load(); return s_ft_axis; }

void touch_panel_set_ft_axis(uint8_t v) {
    if (v > 3) return;
    s_ft_axis = v;
    s_ft_axis_loaded = true;
    nvs_handle_t h;
    if (nvs_open("os_tp", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ft_axis", v);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "FT6236 方向参数 = %u (bit0 交换XY=%d, bit1 Y取反=%d)",
             v, v & 1, (v >> 1) & 1);
}

/* ==================== I2C 底层 ==================== */
/* ★★★ 2026-10-22 定案（linit 实测 + 日志实锤）：
 *   旧 API (`i2c_param_config` + `i2c_driver_install` + `i2c_master_cmd_begin`)
 *   在**标准扩展版的 15/7 上完全不通** —— 驱动返回 ESP_OK、端口 1 装好，
 *   但 4 个候选地址 (0x5D/0x14/0x15/0x38) 一律 0 应答；
 *   而同一时刻 `tpscan`（新 API `i2c_new_master_bus` + `i2c_master_probe`）能扫到 `ACK 0x15`。
 *   换一块已知正常的屏也复现 ⇒ **不是硬件、不是时序，是驱动**。
 *   ⇒ 底层整体切到新驱动，与 tpscan 完全一致；上层 `*_read_points` 逻辑一行没动。 */
static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

/* 按地址建从机句柄（芯片型号探测出来后才能建） */
static void tp_dev_attach(uint8_t addr) {
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (!s_bus || addr == 0) return;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = TP_I2C_FREQ,
    };
    if (i2c_master_bus_add_device(s_bus, &dc, &s_dev) != ESP_OK) s_dev = NULL;
}

/* 重建总线 (探测失败几次后总线可能残留状态: SDA 被从机拉住时
 * i2c_master_probe 会把所有地址都误报 ACK ⇒ 误判成 GT911)。
 * hw_cfg 的探测每次都是全新总线, 所以它能命中; 这里补齐同样的干净起点。 */
static void tp_bus_rebuild(void) {
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (s_bus) { i2c_del_master_bus(s_bus);       s_bus = NULL; }
    i2c_master_bus_config_t bc = {
        .i2c_port    = TP_I2C_PORT,
        .sda_io_num  = s_pins.sda,
        .scl_io_num  = s_pins.scl,
        .clk_source  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) s_bus = NULL;
}

/* 探测 7-bit 地址是否有 ACK */
static bool tp_i2c_probe(uint8_t addr) {
    if (!s_bus) return false;
    return i2c_master_probe(s_bus, addr, 50) == ESP_OK;
}

/* 8-bit 寄存器地址读 (CST816 / FT6236) */
static esp_err_t tp_i2c_read8(uint8_t reg, uint8_t *buf, size_t len) {
    if (!s_dev) return ESP_FAIL;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 50);
}

/* 16-bit 寄存器地址读 (GT911) */
static esp_err_t tp_i2c_read16(uint16_t reg, uint8_t *buf, size_t len) {
    if (!s_dev) return ESP_FAIL;
    uint8_t r[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_dev, r, 2, buf, len, 50);
}

/* 16-bit 寄存器地址写 (GT911) */
static esp_err_t tp_i2c_write16(uint16_t reg, const uint8_t *buf, size_t len) {
    if (!s_dev || len > 16) return ESP_FAIL;
    uint8_t tmp[2 + 16];
    tmp[0] = (uint8_t)(reg >> 8);
    tmp[1] = (uint8_t)(reg & 0xFF);
    if (len) memcpy(tmp + 2, buf, len);
    return i2c_master_transmit(s_dev, tmp, 2 + len, 50);
}

/* ==================== GT911 复位时序 ==================== */

static void gt911_reset(void) {
    /* INT / RST 先设为输出, 走 Goodix 上电复位时序 */
    gpio_set_direction(s_pins.intr, GPIO_MODE_OUTPUT);
    gpio_set_direction(s_pins.rst, GPIO_MODE_OUTPUT);

    gpio_set_level(s_pins.intr, 0);
    gpio_set_level(s_pins.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    gpio_set_level(s_pins.rst, 1);   /* 拉高 RST */
    vTaskDelay(pdMS_TO_TICKS(6));

    gpio_set_level(s_pins.intr, 0);   /* INT 保持低电平选地址 */
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(s_pins.intr, 1);   /* 释放 INT */
    vTaskDelay(pdMS_TO_TICKS(5));

    gpio_set_direction(s_pins.intr, GPIO_MODE_INPUT);  /* 恢复为输入(上拉) */
}


/* ==================== 初始化 ==================== */

static tp_chip_t tp_probe_chip(void);   /* 定义在下方 (探测芯片) */

tp_chip_t touch_panel_init(void) {
    /* ★ 2026-10-16: 本板没有触摸面板时上层给 -1 ⇒ **直接跳过, 一个引脚都不碰**。
     *   微雪原板 GPIO5 是 LCD RS, 以前这里无条件把默认的 GPIO5 配成 I2C SDA
     *   (开漏+上拉) ⇒ RS 恒 3.3V、屏"设置不刷新"。 */
    if (s_pins.sda < 0 || s_pins.scl < 0) {
        ESP_LOGI(TAG, "本板未接触摸 (SDA/SCL 未设定), 跳过触摸初始化");
        return TP_CHIP_NONE;
    }
    /* ★ 新驱动建总线（与 tpscan 一致）。旧 API 在标准版 15/7 上不通，见文件头说明。 */
    if (s_bus) { i2c_del_master_bus(s_bus); s_bus = NULL; }
    i2c_master_bus_config_t bc = {
        .i2c_port    = TP_I2C_PORT,
        .sda_io_num  = s_pins.sda,
        .scl_io_num  = s_pins.scl,
        .clk_source  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bc, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C 总线创建失败(%s) sda=%d scl=%d ⇒ 触摸不可用",
                 esp_err_to_name(ret), s_pins.sda, s_pins.scl);
        s_bus = NULL;
        return TP_CHIP_NONE;
    }
    ESP_LOGW(TAG, "I2C 总线就绪 port=%d sda=%d scl=%d @%dkHz",
             TP_I2C_PORT, s_pins.sda, s_pins.scl, TP_I2C_FREQ / 1000);

    /* RST 输出(先不拉低: 芯片上电本就就绪时不需要碰它), INT 输入+上拉 */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << s_pins.rst),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = false,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(s_pins.rst, 1);   /* ★ 保持 RST 高(释放复位), 让芯片能应答 */

    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << s_pins.intr),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    /* ★★★ 2026-10-22: 三级探测, 从"对芯片打扰最小"的一级开始。
     *   ① **裸探** —— 芯片上电已就绪时不碰任何脚 (CST836U / FT6236 常见)。
     *   ② **只拉 RST 脉冲** —— 冷启动卡在复位态时救回来 (CST/FT 通用, 不碰 INT)。
     *   ③ 最后才走 `gt911_reset()` (把 INT 当输出强推 50ms 去选 I2C 地址) ——
     *      这段**只对 Goodix GT911 有意义**, 对 CST/FT 是破坏性的: 它们的 INT 是
     *      **芯片输出脚**, 被 MCU 强推会打挂采样状态。
     *   实测(标准扩展版 15/7 + CST836U): 裸探能命中 0x15, 但原来无条件先跑
     *   gt911_reset ⇒ tp_probe_chip 报"未检测到触摸芯片", 而同一时刻 tpscan 却扫得到 0x15。
     *   ⇒ 顺序错了, 不是硬件问题。 */
    if (tp_probe_chip() == TP_CHIP_NONE) {
        ESP_LOGW(TAG, "触摸探测 ①裸探 无应答");
        /* ② 只拉 RST 脉冲 (不碰 INT), 再等芯片真正启动 */
        gpio_set_direction(s_pins.rst, GPIO_MODE_OUTPUT);
        gpio_set_level(s_pins.rst, 0);
        vTaskDelay(pdMS_TO_TICKS(12));
        gpio_set_level(s_pins.rst, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        /* ★ 关键: 重建总线。上面几次探测失败会让总线残留状态, 不重建的话
         *   下一轮 4 个地址会**全部误报 ACK**(SDA 被拉住时 i2c_master_probe 的假阳性),
         *   于是被误判成 GT911、读出乱码 PID —— 实测踩过。 */
        tp_bus_rebuild();
        if (tp_probe_chip() == TP_CHIP_NONE) {
            ESP_LOGW(TAG, "触摸探测 ②RST脉冲 无应答 (sda=%d scl=%d rst=%d)",
                     s_pins.sda, s_pins.scl, s_pins.rst);
            gt911_reset();                       /* 只为 GT911 选地址, 最后才试 */
            gpio_set_level(s_pins.rst, 1);
            vTaskDelay(pdMS_TO_TICKS(80));
            tp_bus_rebuild();
            if (tp_probe_chip() == TP_CHIP_NONE)
                ESP_LOGW(TAG, "触摸探测 ③GT911复位时序 仍无应答 ⇒ 芯片未接/供电/排线");
        }
    }

    if (s_chip == TP_CHIP_NONE) {
        ESP_LOGW(TAG, "未检测到触摸芯片 (GT911/CST816/FT6236)");
    } else {
        const char *names[] = {"?", "GT911", "CST816", "FT6236"};
        ESP_LOGI(TAG, "触摸芯片: %s (I2C addr=0x%02X, 分辨率=%dx%d)",
                 names[s_chip], s_addr, s_res_x, s_res_y);
    }
    /* 启动后台触摸看门狗 (V1.0.68) */
    if (!s_wd_task) {
        /* ★2026-10-16 内存优化: 栈放 PSRAM(不涉 DMA/cache 关断路径) */
        xTaskCreateWithCaps(touch_watchdog_task, "touch_wd", 1536, NULL, 2, &s_wd_task,   /* ★ 12KB→6KB: 实测仅用 2.8KB */
                            MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "触摸看门狗已启动");
    }
    return s_chip;
}

/* 探测 I2C 上的触摸芯片并设置 s_chip/s_addr/s_res_x/s_res_y */
static tp_chip_t tp_probe_chip(void) {
    s_chip = TP_CHIP_NONE;
    s_addr = 0;
    /* ★ 2026-10-22 诊断: 一次把 4 个候选地址的 ACK 全打出来, 避免"到底哪个没通"靠猜。 */
    {
        bool a = tp_i2c_probe(GT911_ADDR_A), b = tp_i2c_probe(GT911_ADDR_B);
        bool c = tp_i2c_probe(CST816_ADDR), d = tp_i2c_probe(FT6236_ADDR);
        ESP_LOGW(TAG, "probe: 0x5D=%d 0x14=%d 0x15=%d 0x38=%d", (int)a, (int)b, (int)c, (int)d);
        if (a || b)      { s_addr = a ? GT911_ADDR_A : GT911_ADDR_B; }
        else if (c)      { s_addr = CST816_ADDR; }
        else if (d)      { s_addr = FT6236_ADDR; }
        else             { return TP_CHIP_NONE; }
        tp_dev_attach(s_addr);          /* ★ 新驱动: 先建从机句柄, 后面 read/write 才能用 */
    }
    /* ── 下面按 s_addr 判定芯片型号 (原逻辑) ── */
    if (s_addr == GT911_ADDR_A || s_addr == GT911_ADDR_B) {
        /* GT911 有两种地址, 用 ACK 的那个 */
        s_addr = tp_i2c_probe(GT911_ADDR_A) ? GT911_ADDR_A : GT911_ADDR_B;
        /* 读产品 ID 验证 (0x8140..0x8143 应为 "911") */
        uint8_t pid[4] = {0};
        if (tp_i2c_read16(GT911_REG_PID, pid, 4) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 产品 ID: %c%c%c%c (addr=0x%02X)",
                     pid[0], pid[1], pid[2], pid[3], s_addr);
            if (pid[0] == '9' && pid[1] == '1' && pid[2] == '1') {
                s_chip = TP_CHIP_GT911;
            }
        }
        /* PID 读不到也按 GT911 处理 (有些面板 PID 寄存器被锁) */
        if (s_chip == TP_CHIP_NONE) {
            s_chip = TP_CHIP_GT911;
        }
        /* 读面板分辨率 (0x8048/49=X, 0x804A/4B=Y), 用于点击 hit-test 的坐标映射 */
        if (s_chip == TP_CHIP_GT911) {
            uint8_t r[4] = {0};
            if (tp_i2c_read16(GT911_REG_RES, r, 4) == ESP_OK) {
                int rx = ((int)r[1] << 8) | r[0];
                int ry = ((int)r[3] << 8) | r[2];
                if (rx >= 200 && rx <= 4096 && ry >= 200 && ry <= 4096) {
                    s_res_x = rx;
                    s_res_y = ry;
                }
            }
        }
    } else if (tp_i2c_probe(CST816_ADDR)) {
        s_addr = CST816_ADDR;
        s_chip = TP_CHIP_CST816;
        /* ★ 2026-10-20: 用型号寄存器区分 CST836U / 老款 CST816S ——
         *   否则"该读 13 字节还是 11 字节"只能靠猜, 读长了直接 NACK 触摸全死。 */
        uint8_t cid[2] = {0};
        s_cst836 = (tp_i2c_read8(0xAA, cid, 2) == ESP_OK && cid[0] == 0x13);   /* 实测 0xAA=13 0xAB=00 */
        ESP_LOGI(TAG, "CST 型号: 0xAA=%02X 0xAB=%02X ⇒ %s", cid[0], cid[1],
                 s_cst836 ? "CST836U(双点)" : "CST816S(单点)");
        /* 读芯片 ID/版本 (0xA7-0xAA) 便于确认具体型号 */
        uint8_t id[4] = {0};
        if (tp_i2c_read8(0xA7, id, 4) == ESP_OK) {
            ESP_LOGI(TAG, "CST ID: A7=%02X A8=%02X A9=%02X AA=%02X",
                     id[0], id[1], id[2], id[3]);
        }
    } else if (tp_i2c_probe(FT6236_ADDR)) {
        s_addr = FT6236_ADDR;
        s_chip = TP_CHIP_FT6236;
    }
    return s_chip;
}

/* 等总线空闲 (读取任务计数归零), 返回 true 表示就绪 */
static bool wd_wait_idle(void) {
    uint32_t t0 = xTaskGetTickCount();
    while (s_busy > 0) {
        if ((uint32_t)(xTaskGetTickCount() - t0) > pdMS_TO_TICKS(WD_RECOVER_WAIT)) {
            ESP_LOGD(TAG, "等总线空闲超时 (busy=%d)", s_busy);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
}

/* 记录一次恢复时间戳, 超过节流上限返回 false */
static bool wd_recover_throttle(void) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int idx = s_wd_recover_idx;
    s_wd_recover_times[idx] = now;
    s_wd_recover_idx = (idx + 1) % WD_MAX_RECOVER;
    if (s_wd_recover_idx == 0) s_wd_window_full = true;   /* 一圈填满 */
    if (!s_wd_window_full) return true;                    /* 窗口未满, 不节流 */
    uint32_t oldest = s_wd_recover_times[s_wd_recover_idx];
    if (now - oldest < WD_THROTTLE_MS) {
        ESP_LOGD(TAG, "3分钟内恢复次数过多, 节流暂停 (避免反复重启)");
        return false;
    }
    return true;
}

/* 非阻塞请求恢复 (input.c 在连续失败后调用, 由看门狗任务执行) */
/* 深睡前关断触摸 IC：RST 拉低并 hold。
 * 触摸芯片待机也有 1~2mA，而本机唤醒源是 EXT1 按键（不是触摸），
 * 所以睡死它没有副作用；唤醒后是整片复位，init 会重新拉高 RST。 */
void touch_panel_power_down(void) {
    if (s_pins.rst < 0) return;
    gpio_set_direction(s_pins.rst, GPIO_MODE_OUTPUT);
    gpio_set_level(s_pins.rst, 0);
    gpio_hold_en(s_pins.rst);
}

void touch_panel_request_recover(void) {
    s_recover_request = true;
}

void touch_panel_recover(void);   /* 前向声明 (soft_recover 需要) */

/* 软恢复: 只做 RST 脉冲, 不重装 I2C (总线没卡死时最快恢复) */
static void touch_panel_soft_recover(void) {
    ESP_LOGD(TAG, "看门狗: 触摸异常, 软复位芯片 (RST 脉冲)");
    if (s_chip == TP_CHIP_GT911) {
        gt911_reset();
    } else if (s_chip != TP_CHIP_NONE) {
        gpio_set_direction(s_pins.rst, GPIO_MODE_OUTPUT);
        gpio_set_level(s_pins.rst, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(s_pins.rst, 1);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    if (s_addr && !tp_i2c_probe(s_addr)) {
        ESP_LOGD(TAG, "软复位后无 ACK, 升级为完整恢复");
        touch_panel_recover();
    }
}

/* 运行时重探测触摸芯片 (供应用管家重检; 轻量 — 仅重扫 I2C 地址/读 ID, 不重装驱动) */
bool touch_panel_reprobe(void) {
    tp_probe_chip();
    if (s_chip == TP_CHIP_NONE) { ESP_LOGD(TAG, "触摸重探测: 仍未检测到芯片"); return false; }
    const char *names[] = {"?", "GT911", "CST816", "FT6236"};
    ESP_LOGI(TAG, "触摸重探测: %s", names[s_chip]);
    return true;
}

/* V1.0.68 fix: 连续 I2C 读失败 (触摸芯片挂死 / 总线卡住, 表现为触摸失灵需重启) 时
 * 自动恢复: RST 脉冲复位芯片 → 重装 I2C 驱动清除卡死总线 → 重新探测.
 * 必须在看门狗任务里调用 (s_recovering 已置位, 总线已空闲). */
void touch_panel_recover(void) {
    if (!wd_wait_idle()) return;
    ESP_LOGD(TAG, "执行完整恢复 (RST 脉冲 + 重装 I2C + 重探测)");
    /* 1. 复位芯片 */
    if (s_chip == TP_CHIP_GT911) {
        gt911_reset();
    } else {
        gpio_set_direction(s_pins.rst, GPIO_MODE_OUTPUT);
        gpio_set_level(s_pins.rst, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(s_pins.rst, 1);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    /* 2. 重建 I2C 总线 (新驱动; SDA 被从机拉低卡死时, 重建可清掉总线状态) */
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (s_bus) { i2c_del_master_bus(s_bus);       s_bus = NULL; }
    i2c_master_bus_config_t bc = {
        .i2c_port    = TP_I2C_PORT,
        .sda_io_num  = s_pins.sda,
        .scl_io_num  = s_pins.scl,
        .clk_source  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) {
        ESP_LOGD(TAG, "恢复: 重建 I2C 总线失败");
        s_bus = NULL;
        return;
    }
    /* 3. 重新探测 */
    tp_probe_chip();
    if (s_chip == TP_CHIP_NONE) {
        ESP_LOGD(TAG, "恢复后仍未探测到触摸芯片");
    } else {
        ESP_LOGI(TAG, "触摸恢复成功: %s", s_chip == TP_CHIP_GT911 ? "GT911" :
                 s_chip == TP_CHIP_CST816 ? "CST816" : "FT6236");
    }
}

/* 看门狗任务: 周期检查触摸健康, 异常则强制恢复 */
static void touch_watchdog_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WD_CHECK_MS));
        if (s_wd_paused) continue;               /* [测试版] 浅睡窗口内暂停: 不碰 I2C */
        if (s_recovering || s_chip == TP_CHIP_NONE) {
            if (s_recover_request) s_recover_request = false;   /* 已禁用触摸, 丢弃请求 */
            s_wd_win_reads = s_wd_win_fails = 0;
            s_wd_stuck_ms = 0;
            continue;
        }
        uint32_t fails = s_wd_win_fails, reads = s_wd_win_reads;
        uint32_t stuck = s_wd_stuck_ms;   /* 不清零: 由读取路径累计/复位 */
        s_wd_win_reads = s_wd_win_fails = 0;

        bool do_full = s_recover_request;
        bool do_soft = false;
        if (s_recover_request) s_recover_request = false;
        if (fails >= WD_FAIL_HARD) { do_full = true; }
        else if (reads >= WD_READS_MIN && fails * WD_RATE_DIV > reads) { do_soft = true; }
        else if (stuck >= WD_STUCK_MS) { do_soft = true; }
        if (!do_full && !do_soft) continue;

        ESP_LOGD(TAG, "看门狗判定异常: fails=%u/%u stuck=%ums %s",
                 (unsigned)fails, (unsigned)reads, (unsigned)stuck,
                 do_full ? "→完整恢复" : "→软恢复");
        if (!wd_recover_throttle()) continue;
        s_recovering = true;
        if (do_full) touch_panel_recover();
        else         touch_panel_soft_recover();
        s_recovering = false;
    }
}

tp_chip_t touch_panel_get_chip(void) {
    return s_chip;
}

/* V1.0.69: 是否检测到触摸屏 (有触摸机型 = true, 无触摸机型 = false).
 * 供 input.c 按机型分支物理键语义用. */
bool touch_panel_is_present(void) {
    return (s_chip != TP_CHIP_NONE);
}

/* [二分定位·测试版] 暂停/恢复触摸看门狗 (浅睡窗口内暂停, 不碰 I2C) */
void touch_panel_set_paused(bool paused) {
    s_wd_paused = paused;
}

void touch_panel_get_resolution(int *max_x, int *max_y) {
    if (max_x) *max_x = s_res_x;
    if (max_y) *max_y = s_res_y;
}

/* ==================== 读点 ==================== */

/* 读取多点 (每点 8 字节, 点1@0x814F; GT911 最多 5 点) */
static bool gt911_read_points(tp_point_t pts[TP_MAX_POINTS], int *count) {
    uint8_t status = 0;
    if (tp_i2c_read16(GT911_REG_STATUS, &status, 1) != ESP_OK) {
        return false;
    }
    if (!(status & 0x80)) {
        /* 无新数据 */
        for (int i = 0; i < TP_MAX_POINTS; i++) pts[i].pressed = false;
        *count = 0;
        return true;
    }
    int n = status & 0x0F;
    if (n <= 0) {
        for (int i = 0; i < TP_MAX_POINTS; i++) pts[i].pressed = false;
        *count = 0;
        /* 清状态, 准备下一次中断 */
        uint8_t z = 0;
        tp_i2c_write16(GT911_REG_STATUS, &z, 1);
        return true;
    }
    /* 一次读满 TP_MAX_POINTS 个点的原始数据 (5*8=40 字节) */
    uint8_t p[TP_MAX_POINTS * 8] = {0};
    if (tp_i2c_read16(GT911_REG_POINT1, p, sizeof(p)) != ESP_OK) {
        return false;
    }
    int cnt = (n > TP_MAX_POINTS) ? TP_MAX_POINTS : n;
    for (int i = 0; i < cnt; i++) {
        const uint8_t *pp = p + i * 8;
        pts[i].pressed = true;
        pts[i].x = (int16_t)((pp[2] << 8) | pp[1]);   /* X = high<<8 | low */
        pts[i].y = (int16_t)((pp[4] << 8) | pp[3]);   /* Y = high<<8 | low */
    }
    for (int i = cnt; i < TP_MAX_POINTS; i++) pts[i].pressed = false;
    *count = cnt;
    /* 读完清状态位, GT911 才会重新拉低 INT 上报下一次触摸 */
    uint8_t z = 0;
    tp_i2c_write16(GT911_REG_STATUS, &z, 1);
    return true;
}

static bool cst816_read_points(tp_point_t pts[TP_MAX_POINTS], int *count) {
    /* ★ 2026-10-20: 恢复到**原来能用的读法** —— 一次读 11 字节,
     *   点数在 d[2]、点1 坐标 d[3..6]。实测本面板 0xAA=13 / 0xAB=00,
     *   不能按 CST816S 老布局(d[1]/d[2..5])解析, 否则永远 0 点。
     *   第二点(仅 CST836U 的双点模式才有)用**另一次独立读**取 d[9..12]:
     *   失败就只回单点, 绝不让"多读几个字节"把整次触摸读废掉。 */
    uint8_t d[11] = {0};
    if (tp_i2c_read8(0x00, d, sizeof(d)) != ESP_OK) return false;

    for (int i = 0; i < TP_MAX_POINTS; i++) pts[i].pressed = false;

    int n = d[2] & 0x0F;
    if (n <= 0) { *count = 0; return true; }

    int x1 = (int)(((d[3] & 0x0F) << 8) | d[4]);
    int y1 = (int)(((d[5] & 0x0F) << 8) | d[6]);
    if (x1 > 4095) x1 = 0;
    if (y1 > 4095) y1 = 0;
    pts[0].pressed = true;
    pts[0].x = (int16_t)x1;
    pts[0].y = (int16_t)y1;

    int cnt = 1;
    if (n >= 2) {
        uint8_t e[4] = {0};
        if (tp_i2c_read8(0x09, e, sizeof(e)) == ESP_OK) {
            int x2 = (int)(((e[0] & 0x0F) << 8) | e[1]);
            int y2 = (int)(((e[2] & 0x0F) << 8) | e[3]);
            if (x2 > 0 && x2 <= 4095 && y2 > 0 && y2 <= 4095) {
                pts[1].pressed = true;
                pts[1].x = (int16_t)x2;
                pts[1].y = (int16_t)y2;
                cnt = 2;
            }
        }
    }
    *count = cnt;
    return true;
}

/* ===== 调试: 触摸量程标定 =====
 * 用法: 串口 `tpcal 20` → 在这 20 秒内**用手指把整个屏幕划一遍**(左右到边、上下到边),
 *       结束时打印 X/Y 的最小最大值 —— 用来推算面板真实量程和轴方向。 */
void touch_panel_debug_cal(int seconds)
{
    if (seconds <= 0) {
        /* 只打印累积值 (用户已随手摸过) */
        ESP_LOGW(TAG, "TPCAL 累积: X=[%d..%d] Y=[%d..%d] touch=%d",
                 s_cal_maxx < 0 ? -1 : s_cal_minx, s_cal_maxx,
                 s_cal_maxy < 0 ? -1 : s_cal_miny, s_cal_maxy, s_cal_n);
        return;
    }
    if (seconds > 300) seconds = 300;
    s_cal_minx = s_cal_miny = 99999; s_cal_maxx = s_cal_maxy = -1; s_cal_n = 0;
    ESP_LOGW(TAG, "TPCAL 清零, 采样 %ds", seconds);
    for (int i = 0; i < seconds * 10; i++) {
        uint8_t d[8] = {0};
        if (tp_i2c_read8(0x00, d, 8) == ESP_OK && (d[2] & 0x0F)) {
            int rx = ((d[3] & 0x0F) << 8) | d[4];
            int ry = ((d[5] & 0x0F) << 8) | d[6];
            if (rx < s_cal_minx) s_cal_minx = rx;
            if (rx > s_cal_maxx) s_cal_maxx = rx;
            if (ry < s_cal_miny) s_cal_miny = ry;
            if (ry > s_cal_maxy) s_cal_maxy = ry;
            s_cal_n++;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "TPCAL 结果: X=[%d..%d] Y=[%d..%d] touch=%d",
             s_cal_maxx < 0 ? -1 : s_cal_minx, s_cal_maxx,
             s_cal_maxy < 0 ? -1 : s_cal_miny, s_cal_maxy, s_cal_n);
}

/* ===== 调试: 监控触摸 (只在读到触点/数据变化时打印) =====
 * 用法: 串口 `tpwatch 30` → 30 秒内按住屏幕, 有触点就会打印。 */
void touch_panel_debug_watch(int seconds)
{
    if (seconds <= 0)  seconds = 30;
    if (seconds > 180) seconds = 180;
    int iters = seconds * 10, printed = 0;
    ESP_LOGW(TAG, "TPWATCH start %ds chip=%d", seconds, (int)s_chip);
    for (int i = 0; i < iters; i++) {
        uint8_t d[16] = {0};
        esp_err_t e = tp_i2c_read8(0x00, d, sizeof(d));
        if (e != ESP_OK) {
            if (printed < 15) { ESP_LOGW(TAG, "TPW[%03d] FAIL %s", i, esp_err_to_name(e)); printed++; }
        } else {
            int n2 = d[2] & 0x0F, n0 = d[0] & 0x0F;
            if (n2 || n0 || d[3] || d[4] || d[5] || d[6]) {
                tp_point_t pts[TP_MAX_POINTS]; int cnt = 0;
                touch_panel_read_points(pts, &cnt);
                int rawx = ((d[3] & 0x0F) << 8) | d[4];
                int rawy = ((d[5] & 0x0F) << 8) | d[6];
                ESP_LOGW(TAG, "TPW[%03d] n=%d/%d  %02X %02X %02X %02X %02X %02X %02X  RAW=(%d,%d)  EV=%d",
                         i, n2, n0, d[0],d[1],d[2],d[3],d[4],d[5],d[6],
                         rawx, rawy, (d[3] >> 6) & 0x03);
                if (++printed >= 15) break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "TPWATCH end (printed=%d)", printed);
}

/* ===== 调试: 连续读 0x00..0x0F 原始寄存器 =====
 * 用途: 判断 FT6236 的点数/坐标到底落在哪几个寄存器上。
 * 用法: 串口发 `tpreg 30`, 然后**按住屏幕**不放, 日志会刷出每次读到的 16 字节。 */
void touch_panel_debug_regs(int times)
{
    if (times <= 0) times = 20;
    if (times > 200) times = 200;
    ESP_LOGW(TAG, "TPREG start chip=%d addr=0x%02X sda=%d scl=%d int=%d rst=%d",
             (int)s_chip, s_addr, s_pins.sda, s_pins.scl, s_pins.intr, s_pins.rst);
    for (int k = 0; k < times; k++) {
        uint8_t d[16] = {0};
        esp_err_t e = tp_i2c_read8(0x00, d, sizeof(d));
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "TPREG[%02d] read FAIL %s", k, esp_err_to_name(e));
        } else {
            ESP_LOGW(TAG, "TPREG[%02d] %02X %02X %02X %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X %02X %02X %02X",
                     k, d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7], d[8],d[9],d[10],d[11],d[12],d[13],d[14],d[15]);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    ESP_LOGW(TAG, "TPREG end");
}

static bool ft6236_read_points(tp_point_t pts[TP_MAX_POINTS], int *count) {
    uint8_t d[16] = {0};
    if (tp_i2c_read8(0x00, d, sizeof(d)) != ESP_OK) {
        return false;
    }
    ft_axis_load();          /* 首次调用时从 NVS 取方向参数 */

    /* ★★★ 2026-10-22 修 "FT6236 认到了但触摸完全没反应":
     *   FT6x36 系列(FocalTech)的标准寄存器布局是
     *     0x00 = DEVICE_MODE (上电恒为 0x00)
     *     0x01 = GEST_ID
     *     0x02 = TD_STATUS  ← **点数在这里**
     *     0x03..0x06 = P1_XH / P1_XL / P1_YH / P1_YL
     *     0x07..0x0A = P2_...
     *   原来代码把 d[0](DEVICE_MODE, 永远 0) 当成点数 ⇒ 永远判 0 点 ⇒ 触摸完全没反应。
     *   实测静止时读出来的正是 00 00 00 ... (0x0F 是 0xFF 保留位), 与手册一致。
     *   少数变种把 TD_STATUS 放在 0x00 ⇒ 保留一条兼容分支。 */
    int n = d[2] & 0x0F;
    int base = 3;
    if (n == 0) {
        int alt = d[0] & 0x0F;
        if (alt > 0 && alt <= 5) { n = alt; base = 1; }
    }

    if (n <= 0) {
        for (int i = 0; i < TP_MAX_POINTS; i++) pts[i].pressed = false;
        *count = 0;
        return true;
    }

    int cnt = (n > 2) ? 2 : n;   /* 硬件最多 2 点 */
    for (int i = 0; i < cnt; i++) {
        const uint8_t *q = d + base + i * 6;    /* FT: 每点 6 字节 (XH XL YH YL WHO RES) */
        int rx = (int)(((q[0] & 0x0F) << 8) | q[1]);
        int ry = (int)(((q[2] & 0x0F) << 8) | q[3]);
        if (rx > 4095) rx = 0;
        if (ry > 4095) ry = 0;

        /* 常驻统计原始坐标极值 (在交换之前记录) */
        if (rx < s_cal_minx) s_cal_minx = rx;
        if (rx > s_cal_maxx) s_cal_maxx = rx;
        if (ry < s_cal_miny) s_cal_miny = ry;
        if (ry > s_cal_maxy) s_cal_maxy = ry;
        s_cal_n++;

        /* ★★★ 2026-10-22 FT6236 方向修正 (只在本分支生效):
         *   鱼星 RLCD 4.2 那块屏是 **90° 旋转安装** ⇒ 交换 XY + Y 取反
         *   (昨天实测: 先"上下滑变左右滑"⇒需 swap; 再"上下颠倒"⇒需 flip)。
         *   ⛔ 用 **NVS 运行时参数**(键 `ft_axis`: bit0=交换XY, bit1=Y取反),
         *      不用编译期宏 —— 编译期宏会让"换一种朝向的屏就要重编"。
         *   ⚠ 这段只写在 ft6236_read_points 内部 ⇒ **对 CST836U / GT911 零影响**,
         *      它们的读点函数各走各的, 一个字节都不共用。 */
        int x, y;
        if (s_ft_axis & 0x01) { x = ry; y = rx; } else { x = rx; y = ry; }
        if (s_ft_axis & 0x02) { y = (TP_DEFAULT_RES_Y - 1) - y; }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        pts[i].pressed = true;
        pts[i].x = (int16_t)x;
        pts[i].y = (int16_t)y;
    }
    for (int i = cnt; i < TP_MAX_POINTS; i++) pts[i].pressed = false;
    *count = cnt;
    return true;
}

/* 诊断: 读失败计数 (每 2s 打一次日志, 判断是 I2C 问题还是数据噪声) */
static uint32_t s_diag_fail = 0, s_diag_ok = 0, s_diag_tick = 0;

bool touch_panel_read_points(tp_point_t pts[TP_MAX_POINTS], int *count) {
    if (!pts || !count || s_chip == TP_CHIP_NONE) {
        return false;
    }
    if (s_recovering) {
        /* 看门狗正在恢复: 免阻塞直接返回, UI 不卡顿, 保持上一帧状态 */
        return false;
    }
    s_busy++;
    bool ok;
    switch (s_chip) {
        case TP_CHIP_GT911:  ok = gt911_read_points(pts, count);  break;
        case TP_CHIP_CST816: ok = cst816_read_points(pts, count); break;
        case TP_CHIP_FT6236: ok = ft6236_read_points(pts, count); break;
        default:             s_busy--; return false;
    }
    s_busy--;
    tp_point_t *pt = &pts[0];
    if (ok) { s_diag_ok++; if (pt->pressed) s_diag_ok++; }
    else    { s_diag_fail++; }
    /* 看门狗统计 */
    {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        s_wd_win_reads++;
        if (!ok) {
            s_wd_win_fails++;
        } else if (pt->pressed) {
            if (pt->x == s_wd_last_x && pt->y == s_wd_last_y) {
                s_wd_stuck_ms += (now - s_wd_last_ms);
            } else {
                s_wd_stuck_ms = 0;
                s_wd_last_x = pt->x;
                s_wd_last_y = pt->y;
            }
        } else {
            s_wd_stuck_ms = 0;
        }
        s_wd_last_ms = now;
    }
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - s_diag_tick >= 2000) {
        /* V1.2: 原先判据 `total > 100` 在正常采样 (≈56Hz → 2s 112 次) 下恒真,
         * 导致控制台每 2 秒必有一条日志 (115200 下白占带宽/抖动).
         * 改为: 有失败 → 立即报 (异常要看得见); 全成功 → 60 秒一条心跳即可. */
        static uint32_t s_diag_ok_log_ms = 0;
        bool anomaly = (s_diag_fail > 0);
        if (anomaly || (now - s_diag_ok_log_ms >= 60000)) {
            s_diag_ok_log_ms = now;
            ESP_LOGI(TAG, "诊断: 2s内 读成功=%u 失败=%u (失败率=%u%%)",
                     (unsigned)s_diag_ok, (unsigned)s_diag_fail,
                     (unsigned)(s_diag_fail * 100 / ((s_diag_fail + s_diag_ok) ? (s_diag_fail + s_diag_ok) : 1)));
        }
        s_diag_tick = now;
        s_diag_fail = s_diag_ok = 0;
    }
    return ok;
}

bool touch_panel_read(tp_point_t *pt) {
    tp_point_t pts[TP_MAX_POINTS];
    int count = 0;
    if (!pt) return false;
    bool ok = touch_panel_read_points(pts, &count);
    *pt = pts[0];
    return ok;
}

/* V1.0.68: 禁用触摸屏: 卸载 I2C 驱动(释放驱动内存) + 复位芯片状态.
 * 之后 touch_panel_read 恒返回 false (零开销), 触摸不再工作. */
/* 调试: 打印原始报文 (供串口 tpraw 使用) */
void touch_panel_debug_dump(void)
{
    if (s_chip != TP_CHIP_CST816) { printf("TPRAW not CST (chip=%d)\r\n", (int)s_chip); return; }
    uint8_t d[11] = {0}, e[4] = {0};
    esp_err_t r1 = tp_i2c_read8(0x00, d, sizeof(d));
    esp_err_t r2 = tp_i2c_read8(0x09, e, sizeof(e));
    printf("TPRAW r1=%d d=", (int)r1);
    for (int i = 0; i < 11; i++) printf("%02X ", d[i]);
    printf("| r2=%d e=", (int)r2);
    for (int i = 0; i < 4; i++) printf("%02X ", e[i]);
    printf("| n=%d x1=%d y1=%d x2=%d y2=%d\r\n", d[2] & 0x0F,
           ((d[3] & 0x0F) << 8) | d[4], ((d[5] & 0x0F) << 8) | d[6],
           ((e[0] & 0x0F) << 8) | e[1], ((e[2] & 0x0F) << 8) | e[3]);
}

/* 深睡前: 把触摸 IC 按进复位态 (省掉它在整个睡眠期间的扫描电流)。
 * 只动 RST 电平, 不写 I2C 寄存器 —— 深睡路径上不能碰任何依赖 cache/flash 的驱动代码。
 * 唤醒后是完整重启, app_board 会 gpio_deep_sleep_hold_dis() 再重新 init, 不受影响。 */
void touch_panel_sleep(void) {
    if (s_pins.rst >= 0) {
        gpio_set_level((gpio_num_t)s_pins.rst, 0);
        gpio_hold_en((gpio_num_t)s_pins.rst);   /* 深睡期间钉在低电平 */
    }
    if (s_pins.intr >= 0) {
        gpio_pullup_dis((gpio_num_t)s_pins.intr);
        gpio_pulldown_dis((gpio_num_t)s_pins.intr);
    }
    if (s_pins.sda >= 0) gpio_pullup_dis((gpio_num_t)s_pins.sda);
    if (s_pins.scl >= 0) gpio_pullup_dis((gpio_num_t)s_pins.scl);
    ESP_LOGI(TAG, "触摸 IC 已按入复位态 (RST=%d hold 低)", s_pins.rst);
}

void touch_panel_deinit(void) {
    if (s_chip != TP_CHIP_NONE) {
        s_recovering = true;            /* 通知看门狗/读取任务让路 */
        wd_wait_idle();
        if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
        if (s_bus) { i2c_del_master_bus(s_bus);       s_bus = NULL; }
        s_chip = TP_CHIP_NONE;
        s_addr = 0;
        s_res_x = TP_DEFAULT_RES_X;
        s_res_y = TP_DEFAULT_RES_Y;
        s_wd_win_reads = s_wd_win_fails = 0;
        s_wd_stuck_ms = 0;
        s_recovering = false;
        ESP_LOGI(TAG, "触摸屏已禁用, I2C 总线已释放");
    }
}
