/**
 * os_net.h — 系统网络分析 API.
 *
 * 网络分析页 (page_netdect) 用到的项目组件内部能力统一收敛到此系统 API,
 * 页面不直接 include macoui.h / wifi_manager.h 之外的组件内部头.
 * 本文件先落地 MAC OUI 厂商查询 (macoui 包装); 后续可继续补齐
 * 网关查询 / 公网 IP / ICMP RTT 等, 让 page_netdect 彻底只依赖 os_*.
 *
 * 说明: page_netdect 仍直接使用的 esp_http_client / esp_ping / lwip / esp_wifi
 * 属 ESP-IDF 公共 SDK API, 按架构约定允许应用直接使用, 不在此包装.
 */
#ifndef OS_NET_H
#define OS_NET_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MAC 厂商查询: 取 3 字节 OUI (mac 前 3 字节) 查厂商名.
 * 命中返回 true, *vendor 指向字典静态映射串 (勿释放). */
bool os_net_mac_vendor(const uint8_t mac[6], const char **vendor);

#ifdef __cplusplus
}
#endif
#endif /* OS_NET_H */
