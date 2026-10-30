/* ES8311 编解码芯片驱动 - I2C 寄存器直接控制
 * 参考: 微雪官方 FactoryProgram (基于 esp_codec_dev v1.4.0)
 * 精确匹配 esp_codec_dev 的 ES8311 驱动初始化序列
 * DAC 输出 (扬声器); ADC 输入由 ES7210 (板载双麦克风) 负责 */

#include "es8311.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ES8311";
static i2c_port_t s_i2c_port = I2C_NUM_0;
static bool s_initialized = false;
static bool s_started = false;
/* 芯片是否真实存在 (I2C 探测到才为 true). 部分机型主板未焊 ES8311,
 * 此时所有寄存器读写都应跳过, 避免报错刷屏. */
static bool s_present = false;

/* V1.0.70: 探测结果查询. 供 audio_player 区分平台:
 *   有 ES8311 → 官方板 (GPIO46=PA 功放使能, 静音走软静音)
 *   无 ES8311 → 自制板 NS4168 I2S 数字功放 (GPIO46=CTRL 硬静音) */
bool es8311_is_present(void) { return s_present; }

static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val) {
    if (!s_present) return ESP_OK;   /* 芯片不存在, 静默跳过 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "I2C 写入 0x%02x=0x%02x 失败: %s", reg, val, esp_err_to_name(ret));
    }
    return ret;
}

static int es8311_read_reg(uint8_t reg, uint8_t *val) {
    if (!s_present) { if (val) *val = 0; return ESP_OK; }   /* 芯片不存在, 静默跳过 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    /* 写寄存器地址 */
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "I2C 读 0x%02x 地址阶段失败: %s", reg, esp_err_to_name(ret));
        *val = 0;
        return ret;
    }
    /* 读数据 */
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "I2C 读 0x%02x 数据阶段失败: %s", reg, esp_err_to_name(ret));
        *val = 0;
    }
    return ret;
}

/* 是否做"全 I2C 总线扫描(0x08..0x77)"。默认关 —— 那一步在无 codec 的板子上要多花约 6 秒。 */
#ifndef ES8311_I2C_FULL_SCAN
#define ES8311_I2C_FULL_SCAN 0
#endif

int es8311_init(int sda, int scl) {
    if (s_initialized) return 0;

    ESP_LOGI(TAG, "初始化 ES8311 (SDA=%d, SCL=%d)", sda, scl);

    /* 配置 I2C 主机 */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = true,
        .scl_pullup_en = true,
        .master.clk_speed = 100000,
    };
    esp_err_t ret = i2c_param_config(s_i2c_port, &conf);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "I2C 配置失败: %s", esp_err_to_name(ret));
        return -1;
    }
    ret = i2c_driver_install(s_i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "I2C 驱动安装失败: %s", esp_err_to_name(ret));
        return -1;
    }

    /* 探测 ES8311 是否存在。
     * ★★ 2026-10-05: **原来这里扫 0x08..0x77 共 112 个地址**, 每个不存在/无应答的地址都要
     *   等满 50ms 超时 ⇒ **无 codec 的板子开机白等 ≈5.6 秒**(实测整机开机多花约 7 秒)。
     *   现在只探 ES8311 的两个可能地址 0x18 / 0x1A, 各一次、超时 20ms ⇒ 总耗时 < 50ms。
     *   想再看整条 I2C 总线上有哪些设备时, 把 ES8311_I2C_FULL_SCAN 置 1 (那种场景才值得花几秒)。 */
    bool found = false;
#if ES8311_I2C_FULL_SCAN
    ESP_LOGI(TAG, "I2C 总线扫描 (全地址 0x08..0x77):");
    for (int addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  I2C 设备发现: 0x%02X", addr);
            if (addr == 0x18 || addr == 0x1A) found = true;   /* ES8311 两个可能地址 */
        }
    }
#else
    for (int i = 0; i < 2; i++) {
        int addr = (i == 0) ? 0x18 : 0x1A;
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  ES8311 在 0x%02X 有应答", addr);
            found = true;
        }
    }
    if (!found) ESP_LOGI(TAG, "  I2C 0x18/0x1A 均无应答 (已跳过全总线扫描: 那一步要多花约 6 秒)");
#endif

    if (!found) {
        /* 本机主板未焊 ES8311 (部分机型如此): 标记不存在, 跳过全部寄存器配置.
         * 返回 0 让上层继续 (I2S/ES7210 仍可用), 只是没有 DAC 扬声器输出. */
        s_present = false;
        s_initialized = true;
        s_started = false;
        ESP_LOGD(TAG, "未检测到 ES8311 (0x18/0x1A 无响应), 本机无此芯片, 跳过初始化 (无 DAC 扬声器输出)");
        return 0;
    }
    s_present = true;

    /* 尝试读取 ES8311 芯片 ID (先试 0x18, 再试 0x1A) */
    int es_addr = ES8311_ADDR;
    uint8_t chip_id = 0;
    es8311_read_reg(0x2F, &chip_id); /* 用 es8311_read_reg 但忽略其内部地址参数 */
    
    /* 更直接的方式: 直接读地址 0x18 和 0x1A 的 0x2F 寄存器 */
    for (int try_addr = 0; try_addr <= 1; try_addr++) {
        int addr = (try_addr == 0) ? 0x18 : 0x1A;
        uint8_t id = 0;
        
        /* 写寄存器地址 */
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, 0x2F, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd);
        
        if (err != ESP_OK) continue;  /* 设备不存在 */
        
        /* 读数据 */
        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
        i2c_master_read_byte(cmd, &id, I2C_MASTER_LAST_NACK);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd);
        
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  地址 0x%02X 寄存器 0x2F = 0x%02X", addr, id);
            if (id == 0x83 || addr == ES8311_ADDR) {
                chip_id = id;
                es_addr = addr;
            }
        }
    }

    /* 使用扫描到的有效地址, 初始化 ES8311 寄存器 */
    ESP_LOGI(TAG, "初始化 ES8311 寄存器 (地址 0x%02X)...", es_addr);

    /* 1. I2C noise immunity - 写两次确保成功 */
    es8311_write_reg(0x44, 0x08);
    es8311_write_reg(0x44, 0x08);

    /* 2. 初始化时钟和系统配置 (在 REG00 上电前写入) */
    es8311_write_reg(0x01, 0x30);  /* CLK_MANAGER: 选择 MCLK 时钟源 */
    es8311_write_reg(0x02, 0x00);  /* pre_div=1, pre_multi=1 */
    es8311_write_reg(0x03, 0x10);  /* fs_mode=0, ADC OSR=16 */
    es8311_write_reg(0x16, 0x24);  /* ADC gain scale */
    es8311_write_reg(0x04, 0x10);  /* DAC OSR=16 */
    es8311_write_reg(0x05, 0x00);  /* ADC divider=1, DAC divider=1 */
    /* 系统寄存器 (不设置可能导致工作异常) */
    es8311_write_reg(0x0B, 0x00);  /* SYSTEM_REG0B */
    es8311_write_reg(0x0C, 0x00);  /* SYSTEM_REG0C */
    es8311_write_reg(0x10, 0x1F);  /* SYSTEM_REG10 */
    es8311_write_reg(0x11, 0x7F);  /* SYSTEM_REG11 */

    /* 3. 上电 (REG00=0x80 = 正常模式, 从机, 无全复位) */
    es8311_write_reg(0x00, 0x80);  /* Power up, I2S slave mode */

    /* 4. 重设时钟管理 (上电后确保生效) */
    es8311_write_reg(0x01, 0x3F);  /* MCLK source, enable all clocks */

    /* 5. HP drive 和 ADC 配置 */
    es8311_write_reg(0x13, 0x10);  /* HP drive enable (line out) */
    es8311_write_reg(0x1B, 0x0A);  /* ADC_REG1B: 模拟偏置 */
    es8311_write_reg(0x1C, 0x6A);  /* ADC_REG1C: ADC EQ bypass */

    /* 6. GPIO: DAC 参考信号配置 (no_dac_ref=true → 0x08) */
    es8311_write_reg(0x44, 0x08);  /* GPIO: 无 DAC 参考信号输出 */

    /* 初始化完成, s_started=false 表示未进入播放状态 */
    s_started = false;
    s_initialized = true;
    ESP_LOGI(TAG, "ES8311 初始化完成 (等待播放时调用 start)");
    return 0;
}

/* 播放开始时的寄存器配置 (匹配 es8311_start, I2S MCLK 就绪后调用!)
 * 只在 I2S 时钟初始化后调用一次 */
int es8311_start(void) {
    if (s_started) return 0;

    ESP_LOGI(TAG, "ES8311 启动播放配置");

    /* 1. 确认电源和从机模式 */
    es8311_write_reg(0x00, 0x80);  /* Power on, I2S slave */

    /* 2. 使能 MCLK 时钟 */
    es8311_write_reg(0x01, 0x3F);  /* MCLK source, no invert, all clocks */

    /* 3. I2S 数据格式: 16-bit I2S (unmute) */
    es8311_write_reg(0x09, 0x0C);  /* DAC SDP: I2S, 16-bit */
    es8311_write_reg(0x0A, 0x0C);  /* ADC SDP: I2S, 16-bit */

    /* 4. ADC 音量 (DAC 模式, 非必须) */
    es8311_write_reg(0x17, 0xBF);  /* ADC volume (默认值) */

    /* 5. DAC 路径配置 */
    es8311_write_reg(0x0E, 0x02);  /* DAC source = I2S data */
    es8311_write_reg(0x12, 0x00);  /* DAC analog power up, soft ramp off */
    es8311_write_reg(0x14, 0x1A);  /* DAC output select */

    /* 6. 模拟供电 (关键!) */
    es8311_write_reg(0x0D, 0x01);  /* Power up DAC ref, VMID normal */

    /* 7. ADC 配置 (DAC 模式下非必须) */
    es8311_write_reg(0x15, 0x40);  /* ADC_REG15 */

    /* 8. DAC EQ bypass (关键! 不设会产生白噪声) */
    es8311_write_reg(0x37, 0x08);  /* DAC EQ bypass */

    /* 9. GPIO 确认 */
    es8311_write_reg(0x45, 0x00);  /* GPIO_REG45 */

    s_started = true;
    ESP_LOGI(TAG, "ES8311 播放启动完成");
    return 0;
}

/* 根据采样率更新 ES8311 时钟分频器 (匹配官方 coeff_div 表) */
int es8311_set_sample_rate(int sample_rate) {

    /* REG02: pre_div(3bits:5-7), pre_multi(2bits:3-4) */
    es8311_write_reg(0x02, (0 << 5) | (0 << 3));  /* pre_div=1, pre_multi=x1 */

    /* REG03: fs_mode(1bit:6), adc_osr(5bits:0-4) */
    es8311_write_reg(0x03, (0 << 6) | 0x10);      /* fs_mode=0, ADC_OSR=16 */

    /* REG04: dac_osr */
    es8311_write_reg(0x04, 0x10);                  /* DAC_OSR=16 */

    /* REG05: adc_div(4bits:4-7), dac_div(4bits:0-3) */
    es8311_write_reg(0x05, (0 << 4) | 0);          /* adc_div=1, dac_div=1 */

    /* REG06: bclk_div(5bits:0-4) */
    /* bclk_div=4 (<19, stored as 4-1=3) */
    es8311_write_reg(0x06, 0x03);                  /* BCLK divider=4 */

    /* REG07/08: LRCK divider */
    es8311_write_reg(0x07, 0x00);                  /* LRCK MSB */
    es8311_write_reg(0x08, 0xFF);                  /* LRCK LSB (divider=256) */

    ESP_LOGI("ES8311", "设置采样率: %d Hz (MCLK=%lu)", sample_rate, (unsigned long)((uint32_t)sample_rate * 256));
    return 0;
}

/* ★★ 音量寄存器上限 (dB 曲线封顶, 2026-10-14 linit: "太大的会破音") ★★
 * REG32: 0x00=静音(-95.5dB) · **0xBF=0dB(满刻度, 不再往上借数字增益)** · 0xFF=+32dB(削波)。
 * 上一版照官方 `percent*256/100-1` 铺满到 0xFF, 结果 88% 就 +16.5dB、100% 是 +32dB
 * ⇒ **满刻度素材(音乐)直接削波破音**。官方组件(esp_codec_dev)之所以敢用满量程, 是因为它用
 * `esp_codec_dev_hw_gain_t` 把功放(PA)的**模拟增益**也算进总增益里一起补偿;
 * 我们这台 PA 增益固定且没标定, 所以取保守且正确的做法:
 *   **数字端封顶 0dB(0xBF)不借增益 ⇒ 结构上不可能削波**, 响度靠收窄 dB 范围来提。
 * 可调: 想再响一点可把 AUDIO_VOL_REG_MAX 调到 0xCB(+6dB, 会轻度削波) —— 不建议。 */
#ifndef AUDIO_VOL_REG_MAX
#define AUDIO_VOL_REG_MAX 0xBF        /* 0xBF = 0dB 满刻度 (不借 +32dB, 免破音) */
#endif
/* 动态范围(dB): 100%→0dB, 1%→-AUDIO_VOL_DB_RANGE。范围越小, 中低档越响。
 * 40dB = "能调小但都能听清" 的折中 (原来铺 95.5dB ⇒ 50% 就 -48dB 才显得"没声")。 */
#ifndef AUDIO_VOL_DB_RANGE
#define AUDIO_VOL_DB_RANGE 40
#endif

int es8311_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* ★★ REG32: DAC 数字音量 — dB 线性, 封顶 0dB ★★
     *   衰减(dB) = AUDIO_VOL_DB_RANGE × (100 - percent) / 100
     *   REG32    = 0xBF - 衰减 × 2          (寄存器 0.5dB/步, 0xBF = 0dB)
     *   100% -> 0xBF =   0.0dB (满刻度, 不削波)
     *    88% -> 0xB6 =  -4.5dB   (6 档, 比改之前的旧曲线响 ~7.5dB)
     *    68% -> 0xA6 = -12.5dB   (1 档)
     *    50% -> 0x97 = -20.0dB
     *     1% -> 0x70 = -39.5dB ; 0% = 真静音(0x00) */
    int reg;
    if (percent == 0) {
        reg = 0x00;                                  /* 0% = 真静音 */
    } else {
        int atten_half_db = (100 - percent) * AUDIO_VOL_DB_RANGE * 2 / 100;   /* 半dB 步 */
        reg = 0xBF - atten_half_db;
        if (reg > AUDIO_VOL_REG_MAX) reg = AUDIO_VOL_REG_MAX;   /* 封顶 (默认 0xBF=0dB) */
        if (reg < 1) reg = 1;
    }
    return es8311_write_reg(0x32, (uint8_t)reg) == ESP_OK ? 0 : -1;
}

void es8311_shutdown(void) {
    if (!s_initialized) {
        ESP_LOGW(TAG, "ES8311 跳过掉电: 本次开机未初始化过(芯片处于上电默认态)");
        return;
    }
    /* ★★★ 2026-10-28 linit「深睡还有 9mA」排查: 两个写寄存器的返回值原来**全被丢弃**。
     *   后果: I2C 写失败(本板 I2C 无外部上拉, 只有模组内部弱上拉)时芯片根本不会掉电,
     *   ES8311 正常工作电流 5~10mA —— 正好是深睡多出来的那个量级, 而日志一片安静,
     *   完全看不出"掉电没生效"。现在: 失败必须报错, 成功也必须留一行. */
    esp_err_t e1 = es8311_write_reg(0x32, 0x00);  /* 静音 DAC */
    esp_err_t e2 = es8311_write_reg(0x00, 0x00);  /* Power down */
    s_initialized = false;
    s_started = false;
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 掉电失败! 0x32=%s 0x00=%s ⇒ I2C 无应答, 芯片仍在工作(5~10mA)",
                 esp_err_to_name(e1), esp_err_to_name(e2));
    } else {
        ESP_LOGI(TAG, "ES8311 已掉电 (0x32=0x00, 0x00=0x00)");
    }
}

/* ================= 运行时诊断 API (remcon 串口排查用, 2026-10-09) =================
 * 用途: "芯片认到了但没声音" 时, 把寄存器**真值**打出来, 判断:
 *   ① 我们写的值到底进没进去 (reg 0x00/0x01/0x05/0x09/0x0D/0x0E/0x12/0x13/0x14/0x32/0x37)
 *   ② 芯片是否被复位成默认值 (疑似 MCLK 缺失/掉电)
 *   ③ 现场用 `esw` 直接改寄存器试听, 定位是哪一位的问题
 * 输出统一带 "ESREG " 前缀, 便于主机侧 grep。 */
int es8311_peek(uint8_t reg, uint8_t *val)
{
    if (!val) return -1;
    return (es8311_read_reg(reg, val) == ESP_OK) ? 0 : -1;
}

int es8311_poke(uint8_t reg, uint8_t val)
{
    return (es8311_write_reg(reg, val) == ESP_OK) ? 0 : -1;
}

int es8311_dump_regs(void)
{
    if (!s_present) {
        ESP_LOGD(TAG, "ESREG absent (未探测到芯片)");
        return 0;
    }
    ESP_LOGI(TAG, "ESREG begin addr=0x%02X init=%d started=%d",
             ES8311_ADDR, (int)s_initialized, (int)s_started);
    int ok = 0;
    for (uint8_t reg = 0x00; reg <= 0x4E; reg++) {
        uint8_t v = 0;
        if (es8311_read_reg(reg, &v) == ESP_OK) ok++;
        ESP_LOGI(TAG, "ESREG %02X=%02X", reg, v);
    }
    ESP_LOGI(TAG, "ESREG end ok=%d/79", ok);
    return ok;
}
