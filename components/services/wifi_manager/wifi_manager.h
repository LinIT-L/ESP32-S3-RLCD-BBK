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
/* ★ 2026-11-24 省电模式「Wi-Fi 闲置自动关闭」的**统一活动状态接口**:
 *   true = 射频正被业务使用, 不许自动关。
 *   覆盖: 已连接 / 正在连接 / 正在扫描 / AP 热点 / 频谱仪测量 / 最近 60s 内有网络活动上报。
 *   为什么必须有它: 只看 is_connected() 会把"扫描中""正在连接""开着热点"当成闲置,
 *   一关就打断用户正在做的事; 而各业务自己判断又会各写一套(任务书明确要求避免)。 */
bool wifi_manager_is_busy(void);

/* === 网络状态只读接口 (2026-11-22 收敛) ===
 * 页面 / Lua / 其它模块**一律用这三个**, 不要再自己调 esp_wifi_*。
 * 读操作本身无害, 但直连一旦被写成 stop/deinit 就会关掉别人的射频;
 * 而 scan_stop 必须成对 clear_ap_list(否则驱动内存泄漏) —— 只在这里维护。
 *   get_ap_info: 当前 AP 的信号/加密/信道; 未连接返回 false(rssi/auth 不写)。
 *   get_ssid:    当前连接 SSID; 未连接返回 false 且 out[0]='\0'。
 *   get_mac:     STA MAC。 */
bool wifi_manager_get_ap_info(int *rssi, int *auth, uint8_t *channel);
bool wifi_manager_get_ssid(char *out, size_t sz);
bool wifi_manager_get_mac(uint8_t mac[6]);

/* ★ 2026-10-15 (联网归网络模块): 是否配过网络 (有保存凭据) */
bool wifi_manager_has_saved(void);

/* ★ 2026-10-15: 开机自动联网 —— 有凭据则拉起射频并连接 (main 启动时调一次).
 * 之后连接/断线重连全由本模块的自愈任务负责, 上层只管查 is_connected。 */
void wifi_manager_autostart(void);

/* ★ 2026-10-18: 用户显式开关意图 (持久化) —— 设置页 WLAN 开关调用, 重启后按它决定状态。
 * get_want_on: -1=从未手动设置(auto: 有凭据才开机开), 0=手动关, 1=手动开。 */
int  wifi_manager_get_want_on(void);
void wifi_manager_set_ps_mode(int max_modem);   /* 1=MAX_MODEM 深省电 / 0=MIN_MODEM 默认 */
void wifi_manager_set_ps_auto(int on);          /* 空闲自动降档开关 (默认开) */
bool wifi_manager_ps_auto_enabled(void);
/* 网络活动上报: 下载/网页/商店/校时等**开始收发前**调一次, 立即回 MIN_MODEM 档。 */
void wifi_manager_note_net_activity(void);
void wifi_manager_set_want_on(bool on);

/* V1.0.46: 消费"刚刚连接成功"事件 (供菜单弹 0.5s 提示), 返回 true=本次刚连上 */
bool wifi_manager_consume_connected_event(void);

/* V1.6.x: 返回已连接 WiFi 的纯 IP 串 (未连接则返回 false, out 清空) */
bool wifi_manager_get_ip(char *out, size_t sz);

/* 当前状态文本: "未连接" / "连接中..." / "已连接 192.168.x.x" / "连接失败" */
const char *wifi_manager_get_status(void);

/* ==================== 统一省电策略（linit 2026-11-07）====================
 * 全项目**只有一个**正常运行省电档, 由本模块独家管理。业务层不再拥有切换权。
 *
 *   · 正常联网            → wifi_manager_apply_ps() 施加统一档 (默认 MIN_MODEM)
 *   · 下载 / 局域网页 / 扫描 / 网络检测 → **一律不再切 NONE**
 *   · 唯一例外: 频谱仪 promiscuous 连续抓包 → rf_measure_begin/end, 退出必恢复
 *
 * 为什么默认 MIN_MODEM 而非 MAX_MODEM: 见 wifi_manager.c 的注释 —— 本机 2026-10-31
 * 实测 MIN_MODEM 已有 37.5% ping 丢包与 41.7MB 包断流, MAX_MODEM 的 listen_interval
 * 只会更差。验证通过后改 wifi_manager.c 里 WIFI_PS_UNIFIED 一行即可全局切档。 */
const char *wifi_manager_ps_name(void);      /* 编译期选定的统一档名 */
const char *wifi_manager_ps_current(void);   /* 只读: 驱动里真实生效的档位 */
void        wifi_manager_apply_ps(void);     /* 重新施加统一档 (唯一入口) */

/* 射频测量窗口 (唯一允许 NONE): 进 begin / 出 end, 计数配对, 结束自动回统一档。 */
void wifi_manager_rf_measure_begin(void);
void wifi_manager_rf_measure_end(void);
bool wifi_manager_rf_measure_active(void);

/* 关机/深睡专用: 无条件停掉射频并 deinit (不看开关、不看引用计数)。
 * 用于"标志与实际射频状态可能脱节"的场景 —— 例如频谱仪把模式切成 NULL 后
 * s_enabled 已是 false, 但射频仍在 start 状态, 深睡会因此多耗 ~20mA。 */
void wifi_manager_force_off(void);

/* 连一个网络 (SSID/密码), 已发起返回 true (结果异步).
 * ★ 2026-11-22: 凭据**只在真正拿到 IP 后才写 NVS** —— 密码输错不会污染
 *   自动连接凭据与连接记录。失败时用 wifi_manager_discard_pending() 丢弃。 */
bool wifi_manager_connect(const char *ssid, const char *password);

/* 连接失败: 丢弃内存里这份未保存的凭据并回退到上次落盘的那份 (不擦 NVS). */
void wifi_manager_discard_pending(void);

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
