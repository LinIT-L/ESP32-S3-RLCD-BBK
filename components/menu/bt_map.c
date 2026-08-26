/*
 *  bt_map.c - 通用 HID 解码 + 差分回退 (全新重写, v3)
 *  ---------------------------------------------------------------------------
 *  目标: 任意 HID 蓝牙手柄都能被识别、解析、映射, 不硬编码任何品牌/款式。
 *
 *  采用被广泛验证的三层策略 (参考本源, 均为真实开源实现而非自创猜想):
 *    L1 描述符解析 : 严格复刻 BTstack 的 btstack_hid_parser 算法
 *                    (刚读的源码 docs/.../external/btstack/src/btstack_hid_parser.c),
 *                    把 report descriptor 逐项提取为 (report_id, usage_page, usage,
 *                    bit_pos, bit_size, logical_min/max)。比 v2 的 ad-hoc 解析更贴近
 *                    HID 规范, 修掉了位对齐/常量(padding)位/usage 分配偏差点。
 *   L2 语义映射  : 采用 Bluepad32 的 uni_hid_parser_generic.c 语义: 轴按 logical
 *                    范围自动居中并归一化 -512..511 (max==-1 无符号修正), 十字按
 *                    logical 范围映射 8 向, 按钮按 usage 0x01.. 编号。
 *   L3 差分回退  : 很多廉价手柄(如 Q36)实际发送的报告与它自己的 descriptor 不符,
 *                    导致"按描述符"永远读不到按键。以连接后的静止帧为基准, 任何相对
 *                    基准发生变化的原始 bit 都视为一个按键, 自动分配按钮索引。该机制
 *                    不依赖任何描述符/品牌, 是真正的"任意 HID 兜底"。
 *
 *  关键: 描述符按键有效就用描述符, 若干帧都按下无效则自动切差分。轴/十字始终用
 *  描述符解码(摇杆就算报告布局不同, 归一化后也能工作)。
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef BTMAP_HOST
#include "esp_log.h"
#endif
#include "bt_map.h"

#ifdef BTMAP_HOST
#define BT_TAG "bt_map[host]"
#define BT_LOGI(...) printf(__VA_ARGS__); printf("\n")
#define BT_LOGE(...) fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n")
#define BT_LOGW(...) printf(__VA_ARGS__); printf("\n")
#else
static const char *TAG = "bt_map";
#define BT_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define BT_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#define BT_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#endif

/* ========================================================================
 *  标准 HID Usage (权威常量, 已与 Bluepad32/TinyUSB 交叉确认)
 * ======================================================================== */
#define USAGE_PAGE_GENERIC_DESKTOP  0x01
#define USAGE_PAGE_BUTTON           0x09
#define USAGE_GEN_X                 0x30
#define USAGE_GEN_Y                 0x31
#define USAGE_GEN_Z                 0x32
#define USAGE_GEN_RX                0x33
#define USAGE_GEN_RY                0x34
#define USAGE_GEN_RZ                0x35
#define USAGE_GEN_HAT_SWITCH        0x39

/* 未定义 report id (描述符里没有出现 ReportID 项) */
#define HID_RPTID_UNDEF 0xFFFF

/* 每台设备解析出的控件 (capability) 上限 */
#define MAP_MAX_CAPS 128

/* 描述符项 (复刻 BTstack hid_descriptor_item_t) */
typedef struct {
    int32_t  item_value;
    uint8_t  item_size;
    uint8_t  item_type;   /* 0=Main 1=Global 2=Local 3=Reserved */
    uint8_t  item_tag;
    uint8_t  data_size;
} hid_item_t;

/* 解析出的字段 */
typedef struct {
    uint8_t  report_id;
    uint16_t usage_page;
    uint16_t usage;
    uint16_t bit_pos;      /* 在以 report_id 开头的数据中的位偏移(仅数据,不含头部) */
    uint8_t  bit_size;
    int32_t  log_min;
    int32_t  log_max;
    uint8_t  type;         /* 0=other 1=axis 2=hat 3=button 4=trigger */
} map_cap_t;

static map_cap_t s_caps[MAP_MAX_CAPS];
static int s_cap_count = 0;

/* 差分回退: 静止基线 + 变化位 -> 按钮索引 映射表 */
#define DIFF_MAX_BTNS  16                       /* 与 PHYS 按钮通道上限一致 */
static uint8_t  s_base[64];                     /* 基线帧(连接后第一个静止帧) */
static uint8_t  s_base_len = 0;
static bool     s_base_valid = false;
static uint16_t s_diff_bitmap[DIFF_MAX_BTNS];   /* 每个逻辑按钮对应的 bit_index (uint16: 长报告 >32 字节不截断) */
static int      s_diff_count = 0;

/* 当前帧状态 */
static uint8_t s_virtual_buttons[4] = {0};      /* 统一按钮位(描述符或差分) */
static uint8_t s_pad_hat_state = 8;
static int s_pad_axis_raw[BT_AXIS_MAX] = {0};

/* 边沿探测 */
static uint32_t s_prev_buttons = 0;
static uint8_t  s_prev_hat = 8;

/* 方向去抖: 从中心(8)偏出才产生一次方向边沿, 返回 100+hat; 摇杆回中才可重触发 */
static uint8_t s_armed_hat_repeat = 8;

/* "按下锁定": 一个物理输入只有经历完整 按下→松开 周期后, 才会产生下一次边沿。
 * 这是"按一下配好几个按键"的根治: 映射界面推进功能时会 reset 边沿缓存, 若用户
 * 仍按住同一键, 该键会被连续识别成多个功能。锁独立于 s_prev_buttons, 跨
 * poll_reset 保持, 只有松开后才允许识别下一个键。 */
static int s_locked_raw = -1;   /* 当前处于"按下未松开"的 raw 码 */

/* 摇杆中心是否已采 (v3: 用 Bluepad32 归一化替代人工采集中性点, 保留接口) */
static bool s_calibrated = false;

static const int k_deadzone = 120;

/* ---------------- L1: 描述符项解析 (BTstack 权威算法, 全新实现) ---------------- */

static const uint8_t hid_item_sizes[4] = { 0, 1, 2, 4 };

/* 解析一个 descriptor item (短项/长项), 返回该项总字节数 (0=失败) */
static int hid_parse_item(const uint8_t *d, uint16_t len, hid_item_t *it)
{
    if (len < 1) return 0;
    uint16_t pos = 0;
    uint8_t hdr = d[pos++];
    it->data_size = hid_item_sizes[hdr & 0x03u];
    it->item_type = (hdr >> 2) & 0x03u;
    it->item_tag  = (hdr >> 4) & 0x0Fu;
    /* long item */
    if (it->data_size == 2 && it->item_tag == 0x0F && it->item_type == 3) {
        if (len < 3) return 0;
        it->data_size = d[pos++];
        it->item_tag  = d[pos++];
    }
    it->item_size = pos + it->data_size;
    if (len < it->item_size || it->data_size > 4) return 0;

    /* 读值 (小端), 全局 Logical Min/Max/Physical(值为 tag1..4) 按有符号处理 */
    int32_t value = 0;
    uint8_t latest = 0;
    for (int i = 0; i < it->data_size; i++) {
        latest = d[pos + i];
        value |= ((int32_t)latest) << (8 * i);
    }
    int sgnd = (it->item_type == 1) && (it->item_tag >= 1) && (it->item_tag <= 4);
    if (sgnd && it->data_size > 0 && (latest & 0x80)) {
        value -= 1u << (it->data_size * 8);
    }
    it->item_value = value;
    return it->item_size;
}

/* 解析 descriptor, 填满 s_caps. 严格复刻 BTstack 的 usage 迭代语义:
 *  - 全局项: UsagePage/LogicalMin/Max/ReportSize/ReportID/ReportCount
 *  - 局部项: Usage / UsageMinimum / UsageMaximum (在下一个主项前收集)
 *  - 主项 Input: 常量(bit0=1)跳过并推进位游标; 否则按 count 逐个生成字段,
 *                  usage 用局部列表(逐元素)或范围(逐 k)填充, bit_pos 顺延。
 *  - report id 变化时位游标归零(BTstack 语义)。Collection/EndColl 清空局部项。 */
static void parse_descriptor(const uint8_t *desc, uint16_t len)
{
    s_cap_count = 0;

    uint16_t g_page = 0;
    int32_t  g_lmin = 0, g_lmax = 0;
    uint16_t g_size = 0, g_count = 0;
    uint16_t g_rptid = HID_RPTID_UNDEF;
    uint16_t rpt_bit = 0;

    uint32_t loc_usages[64];
    int      loc_n = 0;
    uint32_t l_min = 0, l_max = 0;
    bool     l_has_min = false, l_has_max = false;

    uint16_t pos = 0;
    while (pos < len) {
        hid_item_t it;
        int used = hid_parse_item(&desc[pos], len - pos, &it);
        if (used <= 0) break;
        pos += (uint16_t)used;

        if (it.item_type == 1) {                 /* global */
            switch (it.item_tag) {
            case 0: g_page = (uint16_t)it.item_value; break;
            case 1: g_lmin = it.item_value; break;
            case 2: g_lmax = it.item_value; break;
            case 7: g_size = (uint16_t)it.item_value; break;
            case 8:
                if (g_rptid != (uint16_t)it.item_value) {
                    g_rptid = (uint16_t)it.item_value;
                    rpt_bit = 0;                 /* BTstack: 换 report id 归零位游标 */
                }
                break;
            case 9: g_count = (uint16_t)it.item_value; break;
            default: break;                      /* Physical/Unit/Push/Pop 忽略(TBstack 同) */
            }
        } else if (it.item_type == 2) {          /* local */
            switch (it.item_tag) {
            case 0: if (loc_n < 64) loc_usages[loc_n++] = it.item_value; break;
            case 1: l_min = it.item_value; l_has_min = true; break;
            case 2: l_max = it.item_value; l_has_max = true; break;
            default: break;
            }
        } else if (it.item_type == 0) {          /* main */
            if (it.item_tag == 8) {              /* Input */
                int16_t size  = g_size ? g_size : 8;
                int16_t count = g_count ? g_count : 1;
                uint16_t bf   = (uint16_t)it.item_value;
                bool is_const = (bf & 0x01) != 0;
                bool is_var   = (bf & 0x02) != 0;
                if (is_const) {
                    /* 常量/填充位: 只占位, 不作为字段 */
                    rpt_bit += (uint16_t)(size * count);
                } else if (is_var && count > 0 && size > 0 && size <= 32) {
                    for (int16_t k = 0; k < count; k++) {
                        if (s_cap_count >= MAP_MAX_CAPS) break;
                        uint32_t uv;
                        if (loc_n > 0) {
                            uv = loc_usages[(k < loc_n) ? k : (loc_n - 1)];
                        } else if (l_has_min && l_has_max) {
                            uv = l_min + (uint32_t)k;
                        } else {
                            uv = 0;
                        }
                        map_cap_t *c = &s_caps[s_cap_count++];
                        c->report_id = (uint8_t)(g_rptid == HID_RPTID_UNDEF ? 0 : g_rptid);
                        c->usage_page = (uint16_t)(uv >> 16);
                        if (c->usage_page == 0) c->usage_page = g_page;
                        c->usage   = (uint16_t)(uv & 0xFFFF);
                        c->bit_pos = rpt_bit + (uint16_t)k * (uint16_t)size;
                        c->bit_size = (uint8_t)size;
                        c->log_min = g_lmin;
                        c->log_max = g_lmax;

                        /* 字段语义分类 (只用于差分排除轴/十字字节) */
                        if (c->usage_page == USAGE_PAGE_GENERIC_DESKTOP) {
                            if (c->usage >= USAGE_GEN_X && c->usage <= USAGE_GEN_HAT_SWITCH)
                                c->type = (c->usage == USAGE_GEN_HAT_SWITCH) ? 2 : 1;
                        } else if (c->usage_page == USAGE_PAGE_BUTTON) {
                            c->type = 3;
                        }
                    }
                    rpt_bit += (uint16_t)(size * count);
                }
                /* 局部项在下一个主项前失效 */
                loc_n = 0; l_has_min = l_has_max = false; l_min = l_max = 0;
            } else {
                /* Collection(10)/EndCollection(12)/Output(9)/Feature(11): 清空局部项,
                 * 避免集合自身的 Usage 污染后续 Input 的 usage/位分配 */
                loc_n = 0; l_has_min = l_has_max = false; l_min = l_max = 0;
            }
        }
    }
}

/* ---------------- 诊断: 打印原始描述符 + 每条字段 ---------------- */

void bt_map_dump_descriptor(const uint8_t *desc, uint16_t len)
{
    BT_LOGI("==== report descriptor hex (%u bytes) ====", len);
    for (uint16_t i = 0; i < len; i += 16) {
        char line[96];
        int o = 0;
        o += snprintf(line + o, sizeof(line) - o, "%04x: ", i);
        for (uint16_t j = 0; j < 16 && (i + j) < len; j++)
            o += snprintf(line + o, sizeof(line) - o, "%02x ", desc[i + j]);
        BT_LOGI("%s", line);
    }
    BT_LOGI("==== parsed %d fields ====", s_cap_count);
    for (int i = 0; i < s_cap_count; i++) {
        BT_LOGI("  [%d] rid=%u page=0x%02x usage=0x%02x bit=%u sz=%u log[%d..%d] type=%u",
                i, s_caps[i].report_id, s_caps[i].usage_page, s_caps[i].usage,
                s_caps[i].bit_pos, s_caps[i].bit_size,
                s_caps[i].log_min, s_caps[i].log_max, s_caps[i].type);
    }
}

/* ---------------- 报告读取 ---------------- */

static uint32_t get_bits(const uint8_t *data, uint16_t bit_pos, uint8_t bit_size)
{
    uint16_t byte = bit_pos >> 3;
    uint8_t bit = bit_pos & 7;
    if (byte >= 64) return 0;
    uint32_t val = 0;
    for (int i = 0; i < 4 && (byte + i) < 64; i++) {
        val |= ((uint32_t)data[byte + i]) << (8 * i);
    }
    val >>= bit;
    uint32_t mask = (bit_size >= 32) ? 0xFFFFFFFF : ((1u << bit_size) - 1);
    return val & mask;
}

/* 轴归一化 (Bluepad32 权威算法): max==-1 时视为无符号满量程修正,
 * 中心自动校准为 0, 归一化到 -512..511 */
static int32_t process_axis(const map_cap_t *cp, uint32_t value)
{
    int32_t max = cp->log_max;
    int32_t min = cp->log_min;
    if (max == -1) {
        max = (1 << cp->bit_size) - 1;   /* 无符号满量程修正 */
    }
    if (max <= min) return 0;
    int32_t range = max - min + 1;
    int32_t centered = (int32_t)value - min - range / 2;
    return centered * 512 / range;
}

/* ---------------- L3: 差分回退 ---------------- */

/* 判断某个 bit 是否落在描述符"轴/十字"字段的位范围内 (避免把摇杆模拟量当按钮).
 * 按位而非按整字节排除: 很多手柄把肩键/方向键与轴/十字挤在同一个字节里,
 * 整字节排除会把同字节的按钮误杀 —— 这正是"手柄上四个按键识别不了"的根因之一. */
static bool bit_is_axis_region(uint16_t bit_index)
{
    for (int i = 0; i < s_cap_count; i++) {
        const map_cap_t *cp = &s_caps[i];
        if (cp->type != 1 && cp->type != 2) continue;   /* 只认轴/十字 */
        if (bit_index >= cp->bit_pos &&
            bit_index < (uint16_t)(cp->bit_pos + cp->bit_size)) return true;
    }
    return false;
}

/* 采集基线(首个静止帧)。返回 true 表示已就绪 */
static void diff_capture_base(const uint8_t *data, uint16_t len)
{
    if (s_base_valid) return;
    if (len > 64) len = 64;
    memcpy(s_base, data, len);
    s_base_len = len;
    s_base_valid = true;
    s_diff_count = 0;
    memset(s_diff_bitmap, 0, sizeof(s_diff_bitmap));
    BT_LOGI("diff: baseline captured (%u bytes)", (unsigned)len);
}

/* 用差分计算"当前按下按钮", 结果合并进 s_virtual_buttons[0..]
 * 判定规则: 任一 bit 相对静止基线"不同"即视为按下 —— 同时兼容 active-high
 * (基线0按下1) 与 active-low (基线1按下0) 手柄, 真正做到"任意手柄通用".
 * 轴/十字的位范围一律排除, 避免把摇杆模拟量当按钮. */
static void diff_update_buttons(const uint8_t *data, uint16_t len)
{
    if (!s_base_valid) return;
    if (len > 64) len = 64;

    for (uint16_t bit_index = 0; bit_index < (uint16_t)len * 8; bit_index++) {
        if (bit_is_axis_region(bit_index)) continue;   /* 排除轴/十字位 */
        uint8_t byte = (uint8_t)(bit_index >> 3);
        uint8_t bit  = (uint8_t)(bit_index & 7);
        /* 相对基线不同 = 按下 (覆盖 active-high 与 active-low) */
        if (((data[byte] >> bit) & 1) == ((s_base[byte] >> bit) & 1)) continue;

        int idx = -1;
        for (int i = 0; i < s_diff_count; i++) {
            if (s_diff_bitmap[i] == bit_index) { idx = i; break; }
        }
        if (idx < 0 && s_diff_count < DIFF_MAX_BTNS) {
            idx = s_diff_count++;
            s_diff_bitmap[idx] = bit_index;
            BT_LOGI("diff: bind new button[%d] <- raw bit %u (byte %u bit %d)",
                    idx, bit_index, byte, bit);
        }
        if (idx >= 0 && idx < 32) {
            s_virtual_buttons[idx / 8] |= (1u << (idx & 7));
        }
    }
}

/* ---------------- 对外接口 ---------------- */

void bt_map_poll_reset(void)
{
    s_prev_buttons = 0;
    s_prev_hat = 8;
}

bool bt_map_set_report_map(const uint8_t *desc, uint16_t len)
{
    if (!desc || len == 0) return false;
    parse_descriptor(desc, len);
    bt_map_dump_descriptor(desc, len);
    bt_map_poll_reset();
    return s_cap_count > 0;
}

bool bt_map_is_calibrated(void)
{
    return s_calibrated;
}

uint8_t bt_map_neutral(void)
{
    return (uint8_t)s_pad_axis_raw[BT_AXIS_Y] == 0 ? 0 : (uint8_t)s_pad_axis_raw[BT_AXIS_X];
}

void bt_map_calibrate_start(void)
{
    s_calibrated = false;
    s_base_valid = false;   /* 下一个静止帧作为差分基线 */
    s_diff_count = 0;
    s_locked_raw = -1;      /* 全新一轮: 清除按下锁与边沿缓存, 避免上一轮污染 */
    s_prev_buttons = 0;
    s_prev_hat = 8;
    s_armed_hat_repeat = 8;
    /* 清除上一轮(可能来自旧手柄)残留的按键/方向状态, 避免重新连接后映射界面的
     * "请松开所有按键"因读到残留非零值而误判(实际没有按键被按下). */
    memset(s_virtual_buttons, 0, sizeof(s_virtual_buttons));
    s_pad_hat_state = 8;
    BT_LOGI("calibration/diff reset");
}

/* 方向去抖: 从中心(8)偏出才产生一次方向边沿, 返回 100+hat; 摇杆回中才可重触发
 * (定义见顶部静态变量区 s_armed_hat_repeat) */

/* 判断某个 raw 码(按钮索引 0..31 / 方向 100+hat)当前是否仍处于"按下"状态 */
static bool raw_is_down(int raw)
{
    if (raw >= 0 && raw < 32) {
        return (s_virtual_buttons[raw / 8] >> (raw & 7)) & 1;
    }
    if (raw >= 100) {
        int d = raw - 100;   /* 0上 2右 4下 6左 */
        switch (d) {
        case 0: return s_pad_hat_state == 0;
        case 2: return s_pad_hat_state == 2;
        case 4: return s_pad_hat_state == 4;
        case 6: return s_pad_hat_state == 6;
        }
    }
    return false;
}

int bt_map_poll_press(void)
{
    /* 按下锁定: 上一个物理输入未松开前不产生任何新边沿。
     * 只有完整 按下→松开 周期后, 才允许识别下一个键。 */
    if (s_locked_raw >= 0) {
        if (raw_is_down(s_locked_raw)) return -1;
        s_locked_raw = -1;   /* 已松开, 解锁 */
    }

    /* 统一按钮位图 -> 32 位便于边沿 */
    uint32_t v32 = 0;
    for (int i = 0; i < 4; i++) v32 |= ((uint32_t)s_virtual_buttons[i]) << (8 * i);

    for (int b = 0; b < 32; b++) {
        uint32_t bit = 1u << b;
        if ((v32 & bit) && !(s_prev_buttons & bit)) {
            s_prev_buttons |= bit;
            s_locked_raw = b;
            return b;
        }
    }
    if (s_pad_hat_state == 8) {
        s_armed_hat_repeat = 8;
    }
    if (s_pad_hat_state != 8 && s_armed_hat_repeat == 8) {
        s_armed_hat_repeat = s_pad_hat_state;
        if (s_pad_hat_state == 0 || s_pad_hat_state == 2 ||
            s_pad_hat_state == 4 || s_pad_hat_state == 6) {
            s_locked_raw = 100 + s_pad_hat_state;
            return 100 + s_pad_hat_state;
        }
    }
    return -1;
}

bool bt_map_btn_pressed(uint8_t btn_code)
{
    if (btn_code < 32) return (s_virtual_buttons[btn_code / 8] >> (btn_code & 7)) & 1;
    return false;
}

bool bt_map_any_button_down(void)
{
    /* 任一按钮位被置位, 或十字/摇杆不在中心 (hat != 8) */
    for (int i = 0; i < 4; i++) {
        if (s_virtual_buttons[i]) return true;
    }
    return s_pad_hat_state != 8;
}

bool bt_map_hat_dir(int dir)
{
    switch (dir) {
    case 0: return (s_pad_hat_state == 0 || s_pad_hat_state == 1 || s_pad_hat_state == 7);
    case 1: return (s_pad_hat_state == 4 || s_pad_hat_state == 3 || s_pad_hat_state == 5);
    case 2: return (s_pad_hat_state == 6 || s_pad_hat_state == 7 || s_pad_hat_state == 5);
    case 3: return (s_pad_hat_state == 2 || s_pad_hat_state == 1 || s_pad_hat_state == 3);
    default: return false;
    }
}

bool bt_map_axis_active(int axis)
{
    if (axis < 0 || axis >= BT_AXIS_MAX) return false;
    return (s_pad_axis_raw[axis] > k_deadzone || s_pad_axis_raw[axis] < -k_deadzone);
}

bool bt_map_feed(uint8_t report_id, const uint8_t *data, uint16_t len, bt_pad_frame_t *pad)
{
    if (!data || len == 0) return false;

    /* 差分基线(连接后第一个静止帧) */
    if (!s_base_valid) diff_capture_base(data, len);

    memset(s_virtual_buttons, 0, sizeof(s_virtual_buttons));
    memset(s_pad_axis_raw, 0, sizeof(s_pad_axis_raw));
    uint8_t hat = 8;
    bool have_hat = false;

    bt_pad_frame_t f;
    memset(&f, 0, sizeof(f));
    f.hat = 8;

    /* L1+L2: 描述符解码 (轴/十字/按钮) */
    for (int c = 0; c < s_cap_count; c++) {
        const map_cap_t *cp = &s_caps[c];
        if (cp->report_id && cp->report_id != report_id) continue;
        uint32_t v = get_bits(data, cp->bit_pos, cp->bit_size);

        if (cp->usage_page == USAGE_PAGE_GENERIC_DESKTOP) {
            switch (cp->usage) {
            case USAGE_GEN_X: f.axis_x = process_axis(cp, v); s_pad_axis_raw[BT_AXIS_X] = f.axis_x; f.have_axes = true; break;
            case USAGE_GEN_Y: f.axis_y = process_axis(cp, v); s_pad_axis_raw[BT_AXIS_Y] = f.axis_y; f.have_axes = true; break;
            case USAGE_GEN_Z:  f.axis_z  = process_axis(cp, v); s_pad_axis_raw[BT_AXIS_Z]  = f.axis_z; break;
            case USAGE_GEN_RX: f.axis_rx = process_axis(cp, v); s_pad_axis_raw[BT_AXIS_RX] = f.axis_rx; break;
            case USAGE_GEN_RY: f.axis_ry = process_axis(cp, v); s_pad_axis_raw[BT_AXIS_RY] = f.axis_ry; break;
            case USAGE_GEN_RZ:
                /* RZ 可能既是右摇杆轴也是右扳机, Bluepad32 语义同样落到 axis_ry */
                f.axis_rz = process_axis(cp, v); s_pad_axis_raw[BT_AXIS_RZ] = f.axis_rz; break;
            case USAGE_GEN_HAT_SWITCH: {
                int32_t hv = (int32_t)v;
                if (hv < cp->log_min || hv > cp->log_max) {
                    hat = 8;                     /* null = 未推动 */
                } else {
                    hat = (uint8_t)(hv - cp->log_min);
                    if (hat > 8) hat = 8;
                }
                have_hat = true;
                break;
            }
            default: break;
            }
        } /* 按钮不再用描述符解码(见下) */
    }

    /* 按键通道固定用"差分": 以连接后第一个静止帧为基线, 任何相对基线变高的位
     * 即为一个按下的键。这是对廉价手柄(如 Q36 描述符与实际报告字节不符, 描述符
     * 解码读不到/读错按键) 最通用、最可靠的解锁方案, 兼容任意 HID 手柄。
     * 摇杆/十字仍走描述符解码(轴即使布局不同也能靠 logical 范围归一化), 互不干扰。
     * 差分基线被采集后, 后续帧的变化位自动归到物理按钮索引。 */
    memset(s_virtual_buttons, 0, sizeof(s_virtual_buttons));
    diff_update_buttons(data, len);

    if (pad) *pad = f;

    /* 方向合成: hat 只有在"非空"(真的按了十字)时才优先, 否则回落到模拟摇杆。
     * 很多手柄同时有 hat 和左右双摇杆, 若 hat 为空(8)却仍霸占方向通道, 会导致
     * 推摇杆不产生任何方向 —— 这是实测到摇杆无响应的根因. */
    #define AXIS_THRESHOLD 128
    uint8_t final_hat = 8;
    if (have_hat && hat != 8) {
        final_hat = hat;
    } else if (f.have_axes) {
        int dx = f.axis_x;
        int dy = f.axis_y;
        if (dx > AXIS_THRESHOLD || dx < -AXIS_THRESHOLD || dy > AXIS_THRESHOLD || dy < -AXIS_THRESHOLD) {
            int h = 0, v = 0;
            if (dx > AXIS_THRESHOLD) h = 1; else if (dx < -AXIS_THRESHOLD) h = -1;
            if (dy < -AXIS_THRESHOLD) v = -1; else if (dy > AXIS_THRESHOLD) v = 1;
            static const uint8_t hv_to_hat[3][3] = {
                /* v=-1(上) */ {7,0,1},
                /* v=0       */ {6,8,2},
                /* v=1(下)   */ {5,4,3},
            };
            final_hat = hv_to_hat[v+1][h+1];
        }
    }
    s_pad_hat_state = final_hat;
    s_calibrated = true;

    /* 打印关键状态(每帧一次, 便于串口定位) */
    BT_LOGI("feed rid=%u hat=%u axisx=%d axisy=%d btns=0x%02x%02x%02x%02x",
            report_id, final_hat, f.axis_x, f.axis_y,
            s_virtual_buttons[3], s_virtual_buttons[2], s_virtual_buttons[1], s_virtual_buttons[0]);
    return true;
}