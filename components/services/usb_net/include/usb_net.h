/**
 * @file usb_net.h
 * @brief USB 网卡共享 (USB-RNDIS 有线网卡) 模块接口.
 *
 * 场景: 电脑插 USB 借设备联的 Wi-Fi 上网 (USB tethering).
 * 同 USB-BT 用 NVS 一次性标记 + 重启进入; 进入后设备枚举为 RNDIS 网卡,
 * 起 DHCP + NAT, 把电脑流量借 esp_wifi 的 STA 上行转发出去 (共享上网).
 *
 * 配置项 (NVS): 网关 / 子网掩码 / IP 地址池, 供 DHCP 分配.
 */
#ifndef USB_NET_H
#define USB_NET_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 一次性模式 ---- */
/* USB 网卡共享是否激活 (NVS 标记): true = 开机应进 RNDIS 网卡共享模式 */
bool usb_net_is_active(void);
void usb_net_set_active(bool active);
/* 是否正运行在网卡共享模式 (进入后为 true; 供状态栏显示 NET 标记) */
bool usb_net_is_running(void);

/* 启动 (写标记 + 重启进入网卡共享). 调用方先提示. 由 UI 点"启动USB网卡共享"调用. */
esp_err_t usb_net_activate(void);
/* 关闭 (清标记 + 重启回正常). 调用方先提示. 由 UI 点"关闭USB网卡共享"调用. */
void usb_net_deactivate(void);

/* ---- 网络配置 (NVS, 供 DHCP) ---- */
/* 读取/写入网关 IP (IPv4), 默认 192.168.4.1 */
void   usb_net_get_gateway(uint32_t *ip);
void   usb_net_set_gateway(uint32_t ip);
/* 读取/写入子网掩码, 默认 255.255.255.0 */
void   usb_net_get_netmask(uint32_t *mask);
void   usb_net_set_netmask(uint32_t mask);
/* 读取/写入 DHCP 地址池起始 IP (起始), 默认 192.168.4.2 */
void   usb_net_get_pool_start(uint32_t *ip);
void   usb_net_set_pool_start(uint32_t ip);

/* ---- 运行时启动 (main 检测到激活时调用): 进 RNDIS 设备 + DHCP/NAT/Wi-Fi 桥 ---- */
esp_err_t usb_net_start(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_NET_H */