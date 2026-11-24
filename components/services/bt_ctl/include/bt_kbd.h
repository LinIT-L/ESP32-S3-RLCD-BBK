#ifndef BT_KBD_H
#define BT_KBD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bt_kbd - 通用 BLE HID 键盘输入解析 (与游戏手柄解码 bt_map 完全独立).
 * -----------------------------------------------------------------------
 * 目标: 让设备能接"不同蓝牙设备"(含普通 BLE 键盘), 不做型号锁死.
 * 按用户要求: 默认输出按键"原始信号(HID 键码)"; 用户手工映射的按映射; 未映射走原信号.
 * 本模块只负责: 识别键盘描述符 + 解析 INPUT 报告(修饰键+6键) + 边沿(按下/释放)发射.
 * 不触碰 bt_map 的手柄路径, 不触碰任意 OS 页面 → 零破坏, 可独立编译验证.
 *
 * HID 键盘 Boot 报告(绝大多数 BLE 键盘兼容):
 *   byte0  = 修饰键位图 (0x01 LShift 0x02 RShift 0x04 LCtrl 0x08 RCtrl 0x10 LAlt ...) 
 *   byte1  = 保留
 *   byte2..7 = 至多 6 个按下的键 (值为 HID Usage, 如 0x04='a' 0x28=Enter 0x2A=Bksp 0xE0..E7 修饰)
 */

/* ★ 键盘动作码 —— 1:1 输入通道的"非字符键"语义。
 * 为什么定义在这里: 输入层(input)已经依赖 bt_ctl, bt_ctl 若反过来 include input.h
 * 会形成组件循环依赖。所以键盘侧先把语义定清楚, input 只负责投递。
 *   ⛔ 与软键盘的 KBD_ACT_* 是**两套东西**: 这里是"物理键盘按了什么",
 *      那边是"屏幕上哪个格子被点了"。页面按 BTK_ACT_* 做分支即可。 */
typedef enum {
    BTK_ACT_CHAR   = -1,   /* 不是动作键: 走字符路径 (bt_kbd_hid_to_ascii) */
    BTK_ACT_IGNORE =  0,   /* 不关心 (纯修饰键 / 未知键) */
    BTK_ACT_ENTER  =  1,
    BTK_ACT_BKSP   =  2,
    BTK_ACT_DEL    =  3,
    BTK_ACT_TAB    =  4,
    BTK_ACT_ESC    =  5,
    BTK_ACT_UP     =  6,
    BTK_ACT_DOWN   =  7,
    BTK_ACT_LEFT   =  8,
    BTK_ACT_RIGHT  =  9,
    BTK_ACT_HOME   = 10,
    BTK_ACT_END    = 11,
    BTK_ACT_PGUP   = 12,
    BTK_ACT_PGDN   = 13,
} bt_kbd_act_t;

/* 一帧键盘状态 (原始信号透传) */
typedef struct {
    uint8_t modifiers;          /* byte0 修饰键位图 */
    uint8_t keys[6];            /* 当前按下的键(按了<=6), 未用位置填 0x00 */
    uint8_t key_count;          /* 当前按下键个数 */
    bool    pressed;            /* 本帧是否有任意非修饰键按下 */
} bt_kbd_state_t;

/* 键盘事件回调: 每次报告发生变化(至少一个键按下/释放)时调用.
 * cur = 当前整帧状态. 调用方在此处决定"透传原始/按映射覆盖". */
typedef void (*bt_kbd_cb_t)(const bt_kbd_state_t *cur);

/* ★ 2026-11-22: 逐键**边沿**回调 —— 1:1 输入用。
 * 原来的 bt_kbd_cb_t 只给"整帧快照", 调用方要自己 diff 才能知道"哪个键刚按下",
 * 而按住一个键时 HID 报告以 ~100Hz 重复到达 (内容不变)。这里在解析层就把边沿算好,
 * 上层直接拿到"这次按下/松开了哪个 HID usage", 不必自己维护上一帧。
 *   pressed=true  → 该键刚按下 (应由上层产生一次字符/动作)
 *   pressed=false → 该键刚松开 (用于修饰键状态、长按判定)
 * ⚠ 只在状态**真的变化**时回调, 不会因为重复报告而重复触发。 */
typedef void (*bt_kbd_key_cb_t)(uint8_t hid_usage, uint8_t modifiers, bool pressed);

/* 从 raw report descriptor 判断是否为键盘(含页 0x07 + Keyboard usage 0x06). */
bool bt_kbd_from_descriptor(const uint8_t *desc, uint16_t len);

/* 喂入一帧 INPUT 报告; 若识别为键盘, 解析并(在状态变化时)触发回调. */
void bt_kbd_feed(const uint8_t *data, uint16_t len);

/* 注册键盘事件回调 (可为空, 表示暂只 internal 记录) */
void bt_kbd_set_cb(bt_kbd_cb_t cb);

/* ★ 注册逐键边沿回调 (1:1 输入通道用; 与上面的整帧回调互不影响, 可同时注册) */
void bt_kbd_set_key_cb(bt_kbd_key_cb_t cb);

/* 当前连接设备是否被判定为键盘 (描述符含 Usage Page 0x07 + Usage 0x06)。
 * ⛔ 手柄/其它 HID 设备为 false —— 调用方靠这个把键盘与手柄区分开,
 *   不要把键盘算进"有手柄" (那会让触屏默认高亮、虚拟按键显隐等逻辑全部错位)。 */
bool bt_kbd_is_keyboard(void);

/* HID usage → 键盘动作码 (见上面的 bt_kbd_act_t)。
 *   BTK_ACT_CHAR(-1)   → 该键产生字符, 用 bt_kbd_hid_to_ascii() 取
 *   BTK_ACT_IGNORE(0)  → 不关心 (纯修饰键)
 *   >0                 → 方向/Enter/Esc/退格等动作 */
int  bt_kbd_hid_to_action(uint8_t hid_usage);

/* 工具: HID 键码→ASCII 字符 (0x04..0x5D 键区; 受修饰键 shift 影响; 返回0=无/需展开).
 * 用于"透传原信号"给文本/终端. */
char bt_kbd_hid_to_ascii(uint8_t hid_usage, uint8_t modifiers);

#ifdef __cplusplus
}
#endif

#endif /* BT_KBD_H */