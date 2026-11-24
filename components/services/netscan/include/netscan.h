#pragma once

/* ============================================================================
 * netscan — 局域网扫描能力服务 (L1)  V1.2
 * ----------------------------------------------------------------------------
 * 把 page_netdect.c 里"只有页面能用"的网络探测能力抽成**可复用服务**,
 * 再经 Lua 桥 (page_lua.c) 开放给单文件应用:
 *   app.net.port(ip, port, ms)  → TCP 端口探测 (解锁"端口扫描")
 *   app.net.arp(ip)             → 主动 ARP 解析目标 MAC (解锁"主机扫描")
 *   app.net.vendor(mac)         → MAC 厂商 (复用 macoui)
 * 依赖: lwip (套接字/ARP) + wifi_manager(macoui) + freertos
 * ==========================================================================*/
#include <stdbool.h>
#include <stdint.h>

/* TCP 端口探测: 1=开放, 0=关闭/超时, <0=参数/套接字错误 */
int netscan_port_probe(const char *ip, uint16_t port, int timeout_ms);

/* 主动 ARP 解析: 成功写入 6 字节 MAC 并返回 true (最多等待 ~260ms) */
bool netscan_resolve_mac(const char *ip, uint8_t mac[6]);

/* 读 ARP 表 (已学到的主机): 写入 ips[i]/macs[i], 返回条数 (>=0) */
int netscan_arp_dump(char (*ips)[16], uint8_t (*macs)[6], int max);
