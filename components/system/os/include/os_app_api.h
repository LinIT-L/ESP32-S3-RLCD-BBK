#ifndef OS_APP_API_H
#define OS_APP_API_H

/* ============================================================================
 * os_app_api — 应用层唯一 API 面 (L4 应用层门面 / facade)
 * ----------------------------------------------------------------------------
 * 目的: 应用 (C 内置 / Lua / 未来下载型) 只能看到这一个受控接口面;
 *       硬件直连 (st7305/bt_manager/board_battery/wifi_manager …) 一律禁止.
 *
 * 铁律 (由 tools/check_deps.py 校验):
 *   · apps 组件只允许 #include "os_app_api.h" (+ 自身页面私有头)
 *   · 需要新能力 → 在本 facade 增一个转发, 而不是让页面直接拉驱动头
 *
 * 配套: 应用能力声明 (caps) —— 注册 os_app_t 时声明用到的能力位,
 *       运行期由 os 校验; 这是"权限职能分配"在应用侧的落点.
 *       位定义见 docs/模块化重构方案.md 第六节.
 * ==========================================================================*/

/* ---- 系统契约: 页面栈 / 模块 / 动作 / 弹窗 / 应用注册 ---- */
#include "os.h"
#include "os_internal.h"     /* 页契约: os_page_*_register / os_register */

/* ---- 绘制: 1bpp 原语 + 字体 + 图标 ---- */
#include "st7305.h"
#include "ui_common.h"
#include "font_zh.h"        /* 中文 24px 字库 (font_zh_count 等) */
#include "font_zh16.h"      /* 中文 16px 字库 */

/* ---- 输入: 按键/触摸/手柄动作 ---- */
#include "input.h"

/* ---- 存储: TF 卡 FAT + NVS 持久化 ---- */
#include "sd_scan.h"
#include "svc_nvs.h"
#include "svc_cfg.h"

/* ---- 安全凭据 (密码/密钥, 不自己明文存) ---- */
#include "keyvault.h"

/* ---- 网络: WiFi 连接状态 + MQTT/HTTP 出口 ---- */
#include "wifi_manager.h"

/* ---- 蓝牙: 手柄接入状态 ---- */
#include "bt_manager.h"

/* ---- USB 外设: 仿真键鼠 ---- */
#include "usb_hid.h"

/* ---- 音频 / 震动 / 电池 ---- */
#include "audio_player.h"
#include "tone_player.h"
#include "vibrator.h"
#include "board_battery.h"

/* ---- 引擎管家: 只在"应用中心"这类要启动引擎的页面用 ---- */
#include "engine_manager.h"

#endif /* OS_APP_API_H */
#include "os_hw.h"       /* os_hw_request/release: 应用按需征用射频 (管家引用计数) */
#include "espsdr.h"      /* ESP32-S3 原始 I/Q 采集 (频谱仪) */
