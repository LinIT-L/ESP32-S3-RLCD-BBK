#ifndef BT_MAP_H
#define BT_MAP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 差分回退键位的编码基址。
 * 很多廉价手柄(如 Q36)实际发送的报告与它们自己的 report descriptor 不符, 导致
 * "按描述符解码"永远读不到按键。为此增加纯"差分"回退: 以映射开始时的静止帧为基准,
 * 任何相对该基准发生变化的原始 bit 位都视为一个按键, 编码为 BT_RAWCODE_BASE + bit_index
 * (bit_index = byte*8 + bit)。该编码不依赖任何品牌/报告布局, 兼容任意手柄。
 */
#define BT_RAWCODE_BASE 200

/*
 *  bt_map - 通用 HID 解码 + 按键映射 (全新重写)
 *  ---------------------------------------------------------------------------
 *  动态解析每台手柄自带的 report descriptor, 得到各控件 (按钮/摇杆/十字/扳机)
 *  的 bit 位与逻辑范围, 据此解码 INPUT 报告。
 *  不硬编码任何一款手柄, 兼容大部分标准 HID 手柄。
 *  本层还维护"当前按下"状态, 供上层查询 (bt_manager_is_key_pressed 等)。
 */

/* 与 bt_manager.h 一致的物理输入枚举, 这里定义中间层的"含义"位 */
enum {
    BT_AXIS_X = 0,
    BT_AXIS_Y,
    BT_AXIS_RX,
    BT_AXIS_RY,
    BT_AXIS_Z,     /* 左扳机 */
    BT_AXIS_RZ,    /* 右扳机 */
    BT_AXIS_MAX,
};

/* 解码结果: 方向(8向), 摇杆归一化值, 按键位图 */
typedef struct {
    uint8_t hat;           /* 8向: 0上 1右上 2右 3右下 4下 5左下 6左 7左上 8=空 */
    int     axis_x;        /* 归一化 -1000..1000 (0=中) */
    int     axis_y;
    int     axis_rx;
    int     axis_ry;
    int     axis_z;        /* 左扳机 0..1000 */
    int     axis_rz;       /* 右扳机 0..1000 */
    uint32_t buttons;      /* 物理按钮位 (bit 与报告内顺序无关, 为本层统一编号) */
    bool    have_axes;     /* 是否有有效摇杆数据 (用于校准) */
} bt_pad_frame_t;

/* 校准 */
void bt_map_calibrate_start(void);     /* 在 connect 后调用, 开始采集中性点 */
bool bt_map_is_calibrated(void);
uint8_t bt_map_neutral(void);

/*
 * 连接建立后调用: 解析该设备的 raw report descriptor。
 * 成功返回 true, 失败 false (解码层将无法工作, 但连接仍可用).
 */
bool bt_map_set_report_map(const uint8_t *desc, uint16_t len);

/* 诊断: 打印原始 report descriptor 十六进制 + 解析出的每条字段 (串口定位用) */
void bt_map_dump_descriptor(const uint8_t *desc, uint16_t len);

/*
 * 喂入一帧 INPUT 报告, 更新内部"当前按下"状态, 并填充 pad.
 * 返回 true 表示成功解析出至少一个输入字段.
 */
bool bt_map_feed(uint8_t report_id, const uint8_t *data, uint16_t len, bt_pad_frame_t *pad);

/* 查询当前某含义是否按下 (供 bt_manager 映射到逻辑功能) */
bool bt_map_btn_pressed(uint8_t btn_code);
bool bt_map_hat_dir(int dir);   /* 0上1下2左3右 */
bool bt_map_axis_active(int axis); /* 该轴当前是否超死区 */

/* 取一帧“尚未消费”的按键边沿, 供映射捕获. 返回一个新的 phys 码或 -1 */
int bt_map_poll_press(void);

/* 重置边沿缓存 (映射开始/取消时调用) */
void bt_map_poll_reset(void);

/* 当前是否有任意物理输入处于按下状态 (按钮或十字方向).
 * 用于映射启动时等待所有按键松开, 防止把"进入映射用的确认键"误当第一个映射. */
bool bt_map_any_button_down(void);

#ifdef __cplusplus
}
#endif

#endif /* BT_MAP_H */