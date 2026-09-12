/*
 *  bt_map.c - 通用 HID 解码 (Bluepad32 路线, 全新重写)
 *  ---------------------------------------------------------------------------
 *  目标: 任意 HID 蓝牙手柄都能被识别、解析、映射, 不硬编码任何品牌/款式。
 *
 *  采用 Bluepad32 的主线策略 (参考其 uni_hid_parser_generic.c, 真实开源实现):
 *    L1 描述符解析 : 严格复刻 BTstack 的 btstack_hid_parser 算法, 把 report descriptor
 *                    逐项提取为 (report_id, usage_page, usage, bit_pos, bit_size,
 *                    logical_min/max), 贴近 HID 规范, 修掉位对齐/常量位/usage 偏差。
 *    语义映射      : 采用 Bluepad32 uni_hid_parser_generic 语义 —— 每帧全量状态:
 *                    轴按 logical 范围自动居中并归一化 (-512..511, max==-1 无符号修正),
 *                    十字按 logical 范围映射 8 向, 按钮按 usage 0x01..0x10 顺次解码到
 *                    逻辑按钮 0..15。
 *    不用差分基线 : 差分(以静止帧变化位当按钮)曾因基线采到半动帧而产生幽灵常驻按键,
 *                    导致"任何键都提示已被占用"。现按 Bluepad32 纯描述符解码, 取消差分。
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

static EXT_RAM_BSS_ATTR map_cap_t s_caps[MAP_MAX_CAPS];   /* 解析结果放 PSRAM, 不占核心内存 */
static int s_cap_count = 0;

/* 当前帧状态 */
static uint8_t s_virtual_buttons[4] = {0};      /* 统一按钮位(按描述符 usage 解码) */

/* 统一数字方向按键 (参考 Bluepad32: 每种输入都归一化为独立数字量, 绝无共享锁)。
 * 摇杆/十字最终都合成一个 final_hat, 再落成 4 个独立方向位 (bit0上 bit1右 bit2下 bit3左)。
 * 每位方向都是一个"数字按钮": down=当前按下, prev=该方向自己的边沿锁。
 * 每个方向只在自己"上升沿"上报一次, 上报后置位 prev 锁住, 直到该方向松开(回中)才回落、
 * 才允许下一次上报 —— 四个方向互不干扰:
 *   - 修复"摇杆只能按一次/换方向没反应": 不再用全局单锁(旧 s_locked_raw/s_armed_hat_repeat),
 *     a 方向上完斜坡回中, b 方向立刻能触发; 摇杆回中(hat==8)即清零 s_dir_down, 全部解锁。 */
static uint8_t s_dir_down = 0;    /* 当前按下的方向位 */
static uint8_t s_dir_prev = 0;    /* 各方向独立的边沿/锁 */

static uint8_t s_pad_hat_state = 8;
static int s_pad_axis_raw[BT_AXIS_MAX] = {0};

/* 按钮边沿探测 (32 位: 4 字节位图) */
static uint32_t s_prev_buttons = 0;
static uint8_t  s_prev_hat = 8;

/* hat 值 → 4 个方向位 (上/下/左/右, 斜向 = 相邻两方向同时置位; 8=中心复位) */
static uint8_t hat_to_dir_bits(uint8_t hat)
{
    uint8_t bits = 0;
    if (hat == 8) return 0;   /* 回中: 全部复位 */
    if (hat == 0 || hat == 1 || hat == 7) bits |= (1u << 0);   /* 上 */
    if (hat == 4 || hat == 3 || hat == 5) bits |= (1u << 2);   /* 下 */
    if (hat == 6 || hat == 7 || hat == 5) bits |= (1u << 3);   /* 左 */
    if (hat == 2 || hat == 1 || hat == 3) bits |= (1u << 1);   /* 右 */
    return bits;
}

/* 摇杆中心是否已采 (v3: 用 Bluepad32 归一化替代人工采集中性点, 保留接口) */
static bool s_calibrated = false;

/* ---- 摇杆自动校准 (适配不同手柄的归零点/阈值/Jitter, 且仅输出四向) ---- */
static bool  s_axis_cal_done = false;   /* 摇杆校准是否完成 */
static int   s_axis_center[2] = {0, 0}; /* X/Y 校准归零点 (相对逻辑中点) */
static int   s_axis_thr = 40;           /* 校准阈值 (覆盖前用保守默认) */
static int   s_cal_min[2], s_cal_max[2];/* 采样窗口 min/max */
static int   s_cal_frames = 0;          /* 已采样帧数 */

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

static uint32_t get_bits(const uint8_t *data, uint16_t bit_pos, uint8_t bit_size, uint16_t len)
{
    uint16_t byte = bit_pos >> 3;
    uint8_t bit = bit_pos & 7;
    if (byte >= len) return 0;              /* 按实际报告长度界定, 防读取越界(如短键盘报告) */
    uint32_t val = 0;
    for (int i = 0; i < 4 && (byte + i) < len; i++) {
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

/* ---------------- 对外接口 ---------------- */

void bt_map_poll_reset(void)
{
    s_prev_buttons = 0;
    s_prev_hat = 8;
    /* 只清"边沿/锁"记录, 不清方向 down(由 feed 每帧刷新)。映射界面在松开后才调用本函数,
     * 此时 s_dir_down 已为 0, 四向全部可重触发, 不会把同一键连配到多个功能。 */
    s_dir_prev = 0;
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
    s_prev_buttons = 0;
    s_prev_hat = 8;
    /* 全新一轮: 清除方向 down/边沿锁与虚拟按钮, 避免上一轮残留污染新映射 */
    s_dir_down = 0;
    s_dir_prev = 0;
    /* 清除上一轮(可能来自旧手柄)残留的按键/方向状态, 避免重新连接后映射界面的
     * "请松开所有按键"因读到残留非零值而误判(实际没有按键被按下). */
    memset(s_virtual_buttons, 0, sizeof(s_virtual_buttons));
    s_pad_hat_state = 8;
    /* 重置摇杆自动校准 (归零点/阈值需按新手柄重测) */
    s_axis_cal_done = false;
    s_axis_center[0] = s_axis_center[1] = 0;
    s_axis_thr = 40;
    s_cal_min[0] = s_cal_min[1] =  0x7FFFFFFF;
    s_cal_max[0] = s_cal_max[1] = -0x7FFFFFFF;
    s_cal_frames = 0;
    BT_LOGI("calibration/diff reset");
}

int bt_map_poll_press(void)
{
    /* 统一按钮位图 -> 32 位便于逐按钮做上升沿 */
    uint32_t v32 = 0;
    for (int i = 0; i < 4; i++) v32 |= ((uint32_t)s_virtual_buttons[i]) << (8 * i);

    /* 1) 按钮: 每按钮独立上升沿, 只在"上次未按"时上报一次 */
    for (int b = 0; b < 32; b++) {
        uint32_t bit = 1u << b;
        if ((v32 & bit) && !(s_prev_buttons & bit)) {
            s_prev_buttons |= bit;
            return b;                 /* 按钮 raw 码 = 索引 0..31 */
        }
    }
    /* 已松开的按钮: prev 回落, 下次再按可重新上报 */
    s_prev_buttons &= v32;

    /* 2) 4 个独立方向: 每个方向自身上升沿上报一次, 各自锁, 互不影响.
     *    摇杆回中 → s_dir_down 归零 → 各方向 prev 回落, 下个方向立即可用 */
    for (int dir = 0; dir < 4; dir++) {
        uint8_t m = (uint8_t)(1u << dir);
        if ((s_dir_down & m) && !(s_dir_prev & m)) {
            s_dir_prev |= m;          /* 该方向已上报, 锁住直至松开 */
            return 100 + dir * 2;     /* dir 0上1右2下3左 → raw 100/102/104/106, 与 bt_manager.raw_to_phys 一致 */
        }
    }
    /* 已松开的方向: prev 回落(独立锁解除), 摇杆回中即全部可重触发 */
    s_dir_prev &= s_dir_down;
    return -1;
}

bool bt_map_btn_pressed(uint8_t btn_code)
{
    if (btn_code < 32) return (s_virtual_buttons[btn_code / 8] >> (btn_code & 7)) & 1;
    return false;
}

bool bt_map_any_button_down(void)
{
    /* 任一按钮位被置位, 或任一方向(摇杆/十字)被推动 */
    for (int i = 0; i < 4; i++) {
        if (s_virtual_buttons[i]) return true;
    }
    return s_dir_down != 0;
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
        uint32_t v = get_bits(data, cp->bit_pos, cp->bit_size, len);

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
            }                   /* 关闭 switch */
        } else if (cp->usage_page == USAGE_PAGE_BUTTON) {
            /* Bluepad32 语义: 按钮 usage 0x01..0x10 → 逻辑按钮 0..15 (物理按键 1..16).
             * 与旧版 Q36 布局一致(其描述符按钮就在 byte5/byte6). */
            int bi = cp->usage - 0x01;        /* usage 0x01 = 按键1 的源位 */
            if (bi >= 0 && bi < 32 && v) {
                s_virtual_buttons[bi / 8] |= (1u << (bi & 7));
            }
        }
    }

    /* 方向合成: hat 只有在"非空"(真的按了十字)时才优先, 否则回落到模拟摇杆。
     * 很多手柄同时有 hat 和左右双摇杆, 若 hat 为空(8)却仍霸占方向通道, 会导致
     * 推摇杆不产生任何方向 —— 这是实测到摇杆无响应的根因.
     * 摇杆走自动校准 + 四向吸附, 兼容不同手柄的归零点/阈值/Jitter. */
    uint8_t final_hat = 8;
    if (have_hat && hat != 8) {
        final_hat = hat;   /* 十字优先: 保留手柄原始 HAT (4/8 向) */
    } else if (f.have_axes) {
        /* 采样窗口: 连接/映射开始后的首若干帧采集摇杆原始量, 求归零点与阈值 */
        if (!s_axis_cal_done && s_cal_frames < 8) {
            int ax = f.axis_x, ay = f.axis_y;
            if (ax < s_cal_min[0]) s_cal_min[0] = ax;
            if (ax > s_cal_max[0]) s_cal_max[0] = ax;
            if (ay < s_cal_min[1]) s_cal_min[1] = ay;
            if (ay > s_cal_max[1]) s_cal_max[1] = ay;
            s_cal_frames++;
            if (s_cal_frames >= 8) {
                s_axis_center[0] = (s_cal_min[0] + s_cal_max[0]) / 2;
                s_axis_center[1] = (s_cal_min[1] + s_cal_max[1]) / 2;
                int spx = s_cal_max[0] - s_cal_min[0];
                int spy = s_cal_max[1] - s_cal_min[1];
                int thr = (spx > spy ? spx : spy) * 3 / 2 + 24;
                if (thr < 40) thr = 40;
                if (thr > 400) thr = 400;
                s_axis_thr = thr;
                s_axis_cal_done = true;
                BT_LOGI("axis cal: center=%d,%d thr=%d", s_axis_center[0], s_axis_center[1], thr);
            }
        }
        int dx = f.axis_x - s_axis_center[0];
        int dy = f.axis_y - s_axis_center[1];
        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        if (adx > s_axis_thr || ady > s_axis_thr) {
            /* 只输出四向: 取主分量方向, 斜向吸附到最近的上下左右 */
            if (adx >= ady) final_hat = dx > 0 ? 2 : 6;   /* 右 / 左 */
            else            final_hat = dy < 0 ? 0 : 4;   /* 上 / 下 */
        }
    }
    /* 按钮已由上面的描述符解码得到 (全量状态), 不再使用差分/基线.
     * 差分函数保留但不再调用 —— 按 Bluepad32 路线, 避免基线脆弱引起的幽灵按键. */
    if (pad) *pad = f;

    s_pad_hat_state = final_hat;
    /* 方向落成 4 个独立数字方向位 (摇杆回中 final_hat==8 → 全部清零解锁) */
    s_dir_down = hat_to_dir_bits(final_hat);
    s_calibrated = true;

    /* 打印关键状态(每帧一次, 便于串口定位) */
    BT_LOGI("feed rid=%u hat=%u axisx=%d axisy=%d btns=0x%02x%02x%02x%02x",
            report_id, final_hat, f.axis_x, f.axis_y,
            s_virtual_buttons[3], s_virtual_buttons[2], s_virtual_buttons[1], s_virtual_buttons[0]);
    return true;
}