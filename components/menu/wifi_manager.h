#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

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

/* V1.0.46: 消费"刚刚连接成功"事件 (供菜单弹 0.5s 提示), 返回 true=本次刚连上 */
bool wifi_manager_consume_connected_event(void);

/* 当前状态文本: "未连接" / "连接中..." / "已连接 192.168.x.x" / "连接失败" */
const char *wifi_manager_get_status(void);

/* 保存配置并连接 (SSID/密码), 成功返回 true */
bool wifi_manager_connect(const char *ssid, const char *password);

/* 读取 NVS 保存的配置 (无保存时 ssid[0]='\0') */
void wifi_manager_get_saved(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);

/* V1.0.46: 扫描附近的 Wi-Fi 网络 (异步, 完成后结果可用) */
bool wifi_manager_scan_start(void);
/* V1.0.67: 停止正在进行的扫描 */
void wifi_manager_scan_stop(void);
bool wifi_manager_is_scan_done(void);
int  wifi_manager_get_scan_count(void);
bool wifi_manager_get_scan_ssid(int idx, char *out, size_t sz);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
