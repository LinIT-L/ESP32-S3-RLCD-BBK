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
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "TOUCH"

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
static TaskHandle_t      s_wd_task = NULL;

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
static uint8_t    s_addr = 0;
static int        s_res_x = TP_DEFAULT_RES_X;
static int        s_res_y = TP_DEFAULT_RES_Y;

/* ==================== I2C 底层 ==================== */

/* 探测 7-bit 地址是否有 ACK */
static bool tp_i2c_probe(uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(TP_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return (err == ESP_OK);
}

/* 8-bit 寄存器地址读 (CST816 / FT6236) */
static esp_err_t tp_i2c_read8(uint8_t reg, uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TP_I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

/* 16-bit 寄存器地址读 (GT911) */
static esp_err_t tp_i2c_read16(uint16_t reg, uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TP_I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

/* 16-bit 寄存器地址写 (GT911) */
static esp_err_t tp_i2c_write16(uint16_t reg, const uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    if (len) {
        i2c_master_write(cmd, (uint8_t *)buf, len, true);
    }
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TP_I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

/* ==================== GT911 复位时序 ==================== */

static void gt911_reset(void) {
    /* INT / RST 先设为输出, 走 Goodix 上电复位时序 */
    gpio_set_direction(TP_INT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(TP_RST_PIN, GPIO_MODE_OUTPUT);

    gpio_set_level(TP_INT_PIN, 0);
    gpio_set_level(TP_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    gpio_set_level(TP_RST_PIN, 1);   /* 拉高 RST */
    vTaskDelay(pdMS_TO_TICKS(6));

    gpio_set_level(TP_INT_PIN, 0);   /* INT 保持低电平选地址 */
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(TP_INT_PIN, 1);   /* 释放 INT */
    vTaskDelay(pdMS_TO_TICKS(5));

    gpio_set_direction(TP_INT_PIN, GPIO_MODE_INPUT);  /* 恢复为输入(上拉) */
}

/* ==================== 初始化 ==================== */

static tp_chip_t tp_probe_chip(void);   /* 定义在下方 (探测芯片) */

tp_chip_t touch_panel_init(void) {
    /* I2C_NUM_1 独立总线 */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TP_SDA_PIN,
        .scl_io_num = TP_SCL_PIN,
        .sda_pullup_en = true,
        .scl_pullup_en = true,
        .master.clk_speed = TP_I2C_FREQ,
    };
    esp_err_t ret = i2c_param_config(TP_I2C_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 配置失败: %s", esp_err_to_name(ret));
        return TP_CHIP_NONE;
    }
    ret = i2c_driver_install(TP_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2C 驱动安装失败: %s", esp_err_to_name(ret));
        return TP_CHIP_NONE;
    }

    /* RST 输出(初始低), INT 输入+上拉 */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << TP_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = false,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(TP_RST_PIN, 0);

    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << TP_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    /* 先做 GT911 复位时序 (对 CST816/FT6236 无害, 只是把 RST 拉高) */
    gt911_reset();
    gpio_set_level(TP_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(80));   /* 等触摸芯片上电就绪 */

    /* 按地址探测芯片 */
    tp_probe_chip();

    if (s_chip == TP_CHIP_NONE) {
        ESP_LOGW(TAG, "未检测到触摸芯片 (GT911/CST816/FT6236)");
    } else {
        const char *names[] = {"?", "GT911", "CST816", "FT6236"};
        ESP_LOGI(TAG, "触摸芯片: %s (I2C addr=0x%02X, 分辨率=%dx%d)",
                 names[s_chip], s_addr, s_res_x, s_res_y);
    }
    /* 启动后台触摸看门狗 (V1.0.68) */
    if (!s_wd_task) {
        xTaskCreate(touch_watchdog_task, "touch_wd", 4096, NULL, 2, &s_wd_task);
        ESP_LOGI(TAG, "触摸看门狗已启动");
    }
    return s_chip;
}

/* 探测 I2C 上的触摸芯片并设置 s_chip/s_addr/s_res_x/s_res_y */
static tp_chip_t tp_probe_chip(void) {
    s_chip = TP_CHIP_NONE;
    s_addr = 0;

    if (tp_i2c_probe(GT911_ADDR_A) || tp_i2c_probe(GT911_ADDR_B)) {
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
        /* 读芯片 ID/版本 (0xA7-0xAA) 便于确认具体型号 */
        uint8_t id[4] = {0};
        if (tp_i2c_read8(0xA7, id, 4) == ESP_OK) {
            ESP_LOGI(TAG, "CST816 ID: A7=%02X A8=%02X A9=%02X AA=%02X",
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
            ESP_LOGW(TAG, "等总线空闲超时 (busy=%d)", s_busy);
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
    uint32_t oldest = s_wd_recover_times[idx];
    if (now - oldest < WD_THROTTLE_MS) {
        ESP_LOGE(TAG, "3分钟内恢复次数过多, 节流暂停 (避免反复重启)");
        return false;
    }
    return true;
}

/* 非阻塞请求恢复 (input.c 在连续失败后调用, 由看门狗任务执行) */
void touch_panel_request_recover(void) {
    s_recover_request = true;
}

void touch_panel_recover(void);   /* 前向声明 (soft_recover 需要) */

/* 软恢复: 只做 RST 脉冲, 不重装 I2C (总线没卡死时最快恢复) */
static void touch_panel_soft_recover(void) {
    ESP_LOGW(TAG, "看门狗: 触摸异常, 软复位芯片 (RST 脉冲)");
    if (s_chip == TP_CHIP_GT911) {
        gt911_reset();
    } else if (s_chip != TP_CHIP_NONE) {
        gpio_set_direction(TP_RST_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(TP_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(TP_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    if (s_addr && !tp_i2c_probe(s_addr)) {
        ESP_LOGW(TAG, "软复位后无 ACK, 升级为完整恢复");
        touch_panel_recover();
    }
}

/* V1.0.68 fix: 连续 I2C 读失败 (触摸芯片挂死 / 总线卡住, 表现为触摸失灵需重启) 时
 * 自动恢复: RST 脉冲复位芯片 → 重装 I2C 驱动清除卡死总线 → 重新探测.
 * 必须在看门狗任务里调用 (s_recovering 已置位, 总线已空闲). */
void touch_panel_recover(void) {
    if (!wd_wait_idle()) return;
    ESP_LOGW(TAG, "执行完整恢复 (RST 脉冲 + 重装 I2C + 重探测)");
    /* 1. 复位芯片 */
    if (s_chip == TP_CHIP_GT911) {
        gt911_reset();
    } else {
        gpio_set_direction(TP_RST_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(TP_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(TP_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    /* 2. 重装 I2C 驱动 (SDA 被从机拉低卡死时, 重装可清掉总线状态) */
    esp_err_t del = i2c_driver_delete(TP_I2C_PORT);
    if (del != ESP_OK && del != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "i2c_driver_delete 返回 %s, 继续", esp_err_to_name(del));
    }
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TP_SDA_PIN,
        .scl_io_num = TP_SCL_PIN,
        .sda_pullup_en = true,
        .scl_pullup_en = true,
        .master.clk_speed = TP_I2C_FREQ,
    };
    i2c_param_config(TP_I2C_PORT, &conf);
    esp_err_t ins = i2c_driver_install(TP_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ins != ESP_OK && ins != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_driver_install 失败: %s", esp_err_to_name(ins));
        return;
    }
    /* 3. 重新探测 */
    tp_probe_chip();
    if (s_chip == TP_CHIP_NONE) {
        ESP_LOGE(TAG, "恢复后仍未探测到触摸芯片");
    } else {
        ESP_LOGI(TAG, "触摸恢复成功: %s", s_chip == TP_CHIP_GT911 ? "GT911" :
                 s_chip == TP_CHIP_CST816 ? "CST816" : "FT6236");
    }
}

/* 看门狗任务: 周期检查触摸健康, 异常则强制恢复 */
static void touch_watchdog_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WD_CHECK_MS));
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

        ESP_LOGW(TAG, "看门狗判定异常: fails=%u/%u stuck=%ums %s",
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

void touch_panel_get_resolution(int *max_x, int *max_y) {
    if (max_x) *max_x = s_res_x;
    if (max_y) *max_y = s_res_y;
}

/* ==================== 读点 ==================== */

static bool gt911_read_point(tp_point_t *pt) {
    uint8_t status = 0;
    if (tp_i2c_read16(GT911_REG_STATUS, &status, 1) != ESP_OK) {
        return false;
    }
    if (!(status & 0x80)) {
        /* 无新数据 */
        pt->pressed = false;
        return true;
    }
    int n = status & 0x0F;
    if (n <= 0) {
        pt->pressed = false;
        /* 清状态, 准备下一次中断 */
        uint8_t z = 0;
        tp_i2c_write16(GT911_REG_STATUS, &z, 1);
        return true;
    }
    uint8_t p[8] = {0};
    if (tp_i2c_read16(GT911_REG_POINT1, p, 8) != ESP_OK) {
        return false;
    }
    pt->pressed = true;
    pt->x = (int16_t)((p[2] << 8) | p[1]);   /* X = high<<8 | low */
    pt->y = (int16_t)((p[4] << 8) | p[3]);   /* Y = high<<8 | low */

    /* 读完清状态位, GT911 才会重新拉低 INT 上报下一次触摸 */
    uint8_t z = 0;
    tp_i2c_write16(GT911_REG_STATUS, &z, 1);
    return true;
}

static bool cst816_read_point(tp_point_t *pt) {
    uint8_t d[7] = {0};
    if (tp_i2c_read8(0x00, d, 7) != ESP_OK) {
        return false;
    }
    /* 实测寄存器布局 (此 CST816 变体比标准右移一位):
     *   0x02 = 点数(触摸标志), 0x03=X高4位, 0x04=X低8位,
     *   0x05=Y高4位, 0x06=Y低8位 */
    int n = d[2] & 0x0F;
    if (n <= 0) {
        pt->pressed = false;
        return true;
    }
    pt->pressed = true;
    pt->x = (int16_t)(((d[3] & 0x0F) << 8) | d[4]);
    pt->y = (int16_t)(((d[5] & 0x0F) << 8) | d[6]);
    return true;
}

static bool ft6236_read_point(tp_point_t *pt) {
    uint8_t d[5] = {0};
    if (tp_i2c_read8(0x00, d, 5) != ESP_OK) {
        return false;
    }
    int n = d[0] & 0x0F;   /* 0x00 = TD_STATUS 点数 */
    if (n <= 0) {
        pt->pressed = false;
        return true;
    }
    pt->pressed = true;
    pt->x = (int16_t)(((d[1] & 0x0F) << 8) | d[2]);
    pt->y = (int16_t)(((d[3] & 0x0F) << 8) | d[4]);
    return true;
}

/* 诊断: 读失败计数 (每 2s 打一次日志, 判断是 I2C 问题还是数据噪声) */
static uint32_t s_diag_fail = 0, s_diag_ok = 0, s_diag_tick = 0;

bool touch_panel_read(tp_point_t *pt) {
    if (!pt || s_chip == TP_CHIP_NONE) {
        return false;
    }
    if (s_recovering) {
        /* 看门狗正在恢复: 免阻塞直接返回, UI 不卡顿, 保持上一帧状态 */
        return false;
    }
    s_busy++;
    bool ok;
    switch (s_chip) {
        case TP_CHIP_GT911:  ok = gt911_read_point(pt);  break;
        case TP_CHIP_CST816: ok = cst816_read_point(pt); break;
        case TP_CHIP_FT6236: ok = ft6236_read_point(pt); break;
        default:             s_busy--; return false;
    }
    s_busy--;
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
        if (s_diag_fail > 0 || (s_diag_ok > 0 && (s_diag_fail + s_diag_ok) > 100)) {
            ESP_LOGI(TAG, "诊断: 2s内 读成功=%u 失败=%u (失败率=%u%%)",
                     (unsigned)s_diag_ok, (unsigned)s_diag_fail,
                     (unsigned)(s_diag_fail * 100 / ((s_diag_fail + s_diag_ok) ? (s_diag_fail + s_diag_ok) : 1)));
        }
        s_diag_tick = now;
        s_diag_fail = s_diag_ok = 0;
    }
    return ok;
}

/* V1.0.68: 禁用触摸屏: 卸载 I2C 驱动(释放驱动内存) + 复位芯片状态.
 * 之后 touch_panel_read 恒返回 false (零开销), 触摸不再工作. */
void touch_panel_deinit(void) {
    if (s_chip != TP_CHIP_NONE) {
        s_recovering = true;            /* 通知看门狗/读取任务让路 */
        wd_wait_idle();
        i2c_driver_delete(TP_I2C_PORT);
        s_chip = TP_CHIP_NONE;
        s_addr = 0;
        s_res_x = TP_DEFAULT_RES_X;
        s_res_y = TP_DEFAULT_RES_Y;
        s_wd_win_reads = s_wd_win_fails = 0;
        s_wd_stuck_ms = 0;
        s_recovering = false;
        ESP_LOGI(TAG, "触摸屏已禁用, I2C 驱动已释放");
    }
}
