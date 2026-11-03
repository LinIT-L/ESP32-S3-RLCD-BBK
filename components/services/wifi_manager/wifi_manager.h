#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"        /* esp_err_t（AP 接口返回） */
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* V1.0.46: Wi-Fi 连网功能 (STA 模式, 连接路由器, 非热点) */

/* 初始化 WiFi 子系统 (main.c 已初始化 NVS + 事件循环) */
void wifi_manager_init(void);

/* 开启 WiFi (使用 NVS 保存的 SSID/密码自动连接), 返回是否成功启动 */
bool wifi_manager_enable(void);

/* 关闭 WiFi */
bool wifi_manager_disable(void);

/* WiFi 射频是否开启 */
bool wifi_manager_is_enabled(void);

/* 是否已连接路由器 */
bool wifi_manager_is_connected(void);
bool wifi_manager_is_connecting(void);

/* ★ 2026-10-15 (联网归网络模块): 是否配过网络 (有保存凭据) */
bool wifi_manager_has_saved(void);

/* ★ 2026-10-15: 开机自动联网 —— 有凭据则拉起射频并连接 (main 启动时调一次).
 * 之后连接/断线重连全由本模块的自愈任务负责, 上层只管查 is_connected。 */
void wifi_manager_autostart(void);

/* ★ 2026-10-18: 用户显式开关意图 (持久化) —— 设置页 WLAN 开关调用, 重启后按它决定状态。
 * get_want_on: -1=从未手动设置(auto: 有凭据才开机开), 0=手动关, 1=手动开。 */
int  wifi_manager_get_want_on(void);
void wifi_manager_set_want_on(bool on);

/* V1.0.46: 消费"刚刚连接成功"事件 (供菜单弹 0.5s 提示), 返回 true=本次刚连上 */
bool wifi_manager_consume_connected_event(void);

/* V1.6.x: 返回已连接 WiFi 的纯 IP 串 (未连接则返回 false, out 清空) */
bool wifi_manager_get_ip(char *out, size_t sz);

/* 当前状态文本: "未连接" / "连接中..." / "已连接 192.168.x.x" / "连接失败" */
const char *wifi_manager_get_status(void);

/* 2026-10-31 (仅调试/诊断): 临时关掉或恢复射频省电。
 * 背景: 连上后默认进 WIFI_PS_MIN_MODEM(按 DTIM 睡射频), 待机省电但**大文件传输**时
 *   实测会丢包(局域网 ping 丢 37.5%, RTT 22~276ms), 41 MB 的资源包会中途断流。
 *   正常业务不要调它 —— 只在"串口诊断/性能测量"时用 `wifi ps 0` 临时关掉。
 *   ⚠ 日常下载已经由 wifi_manager_ps_hold() 自动处理, 不需要调用本函数。 */
void wifi_manager_set_ps_diag(bool ps_off);

/* ★ 2026-10-31 linit 定规: **任何下载/取数期间都不许进 DTIM 省电**。
 *   调用方: net_client 在每次 HTTP 请求开始 hold(true)、结束 hold(false)。
 *   语义: 引用计数。
 *     · 计数 0→1: 立刻 WIFI_PS_NONE(射频全醒), 保证不丢包;
 *     · 计数 1→0: **延迟 8 秒**再回 MIN_MODEM ——
 *       109 个文件连续下载时不会每个文件都切一次模式(频繁切 DTIM 反而更差),
 *       而单次临时取数结束后又能及时省电。
 *   真机依据: 省电开着 ping 丢包 37.5%/RTT 144ms, 关掉 0%/22ms; 41.7MB 包在省电下
 *   传到 7.5MB 断流。详见 docs/专题/功能专题/电源与休眠.md */
void wifi_manager_ps_hold(bool on);

/* 保存配置并连接 (SSID/密码), 成功返回 true */
bool wifi_manager_connect(const char *ssid, const char *password);

/* 清空已保存的 SSID/密码 (连接失败后清空重输用) */
void wifi_manager_clear_saved(void);

/* 读取 NVS 保存的配置 (无保存时 ssid[0]='\0') */
void wifi_manager_get_saved(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);
/* ==== 连接记录 (最多 5 条, 最近的在前) ====
 * 供"网络配置"页展示历史 + 逐个忘记。自动连接仍只用 get_saved() 那条。 */
int  wifi_manager_history_count(void);
bool wifi_manager_history_get(int i, char *ssid, size_t ssz, char *pass, size_t psz);
bool wifi_manager_forget(const char *ssid);

/* V1.0.46: 扫描附近的 Wi-Fi 网络 (异步, 完成后结果可用) */
bool wifi_manager_scan_start(void);
/* V1.0.67: 停止正在进行的扫描 */
void wifi_manager_scan_stop(void);
bool wifi_manager_is_scan_done(void);
int  wifi_manager_get_scan_count(void);
bool wifi_manager_get_scan_ssid(int idx, char *out, size_t sz);
/* V1.5.x: 返回扫描结果原始记录数组 (含 rssi/channel/authmode), 未扫描时 NULL.
 * 注意: 结果由 wifi_manager 内部持有, 供嗅探模块(wifi_probe)直接读取, 不重复抽取. */
const wifi_ap_record_t *wifi_manager_get_scan_records(void);

/* ================= AP 热点（2026-11-01 收口） =================
 * 为什么收进本模块：
 *   改造前 网页手柄 / 收藏夹 / 密钥管理器 **各自** esp_wifi_init() + set_mode(WIFI_MODE_AP)，
 *   三处都会把已连接的 STA 打掉，还各自建热点、各自做 8.8.8.8 + DNS 劫持。
 *   现在 AP 只由这里开：内部用 **WIFI_MODE_APSTA**，已连接的 STA 不受影响
 *   （AP 信道自动跟随 STA），退出时只关 AP、不回退 STA。
 *
 * 参数：
 *   ssid/pass  热点名与密码；pass 为 NULL 或不足 8 字符 ⇒ 开放热点
 *   channel    1~13；与已连接 STA 不同信道时由驱动自动对齐
 *   max_conn   最多接入设备数
 *   captive    true ⇒ DHCP 下发 captive 门户 URL + DNS 全劫持（浏览器自动弹页）
 * 返回 ESP_OK 表示热点已开（重复调用幂等）。
 */
esp_err_t wifi_manager_ap_start(const char *ssid, const char *pass,
                                uint8_t channel, uint8_t max_conn, bool captive);

/* 关闭热点（同时停掉本模块开的 DNS）。已连接的 STA 保持不动。
 * 若射频已无其他用途且 STA 也关着，才真正 esp_wifi_stop()。 */
void wifi_manager_ap_stop(void);

bool wifi_manager_ap_is_on(void);

/* 当前 AP 的 IPv4（"8.8.8.8"）；未开热点返回 false。 */
bool wifi_manager_ap_get_ip(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
