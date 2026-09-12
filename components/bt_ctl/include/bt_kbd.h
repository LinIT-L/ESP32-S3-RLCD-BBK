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

/* 从 raw report descriptor 判断是否为键盘(含页 0x07 + Keyboard usage 0x06). */
bool bt_kbd_from_descriptor(const uint8_t *desc, uint16_t len);

/* 喂入一帧 INPUT 报告; 若识别为键盘, 解析并(在状态变化时)触发回调. */
void bt_kbd_feed(const uint8_t *data, uint16_t len);

/* 注册键盘事件回调 (可为空, 表示暂只 internal 记录) */
void bt_kbd_set_cb(bt_kbd_cb_t cb);

/* 工具: HID 键码→ASCII 字符 (0x04..0x5D 键区; 受修饰键 shift 影响; 返回0=无/需展开).
 * 用于"透传原信号"给文本/终端. */
char bt_kbd_hid_to_ascii(uint8_t hid_usage, uint8_t modifiers);

#ifdef __cplusplus
}
#endif

#endif /* BT_KBD_H */