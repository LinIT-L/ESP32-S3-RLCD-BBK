/**
 * web_files.h — TF 卡网页文件管理（第三回合接入统一 Web 服务）。
 *
 * 定位：把「浏览器里管理 /sdcard」这件事挂到 web_core 的 /files/ 前缀下。
 *   · 不新建 HTTP Server（全系统仍然只有 web_core 那一个 httpd）
 *   · 不初始化 Wi-Fi（热点/STA 全走 wifi_manager；AP 用 APSTA，不踢掉已连接 STA）
 *   · 认证复用 web_core 的 PIN→Token→Bearer（PIN 取 keyvault，与收藏夹同一套）
 *
 * 页面：GET  /files/        （公开，用于显示 PIN 登录框）
 *       POST /files/login   （公开，body=PIN → {"ok":true,"t":"<token>"}）
 *       ANY  /files/api/... （必须 Bearer；上传/下载/删除/改名/建目录/列目录）
 *
 * 文件传输（为什么这么设计，见 web_files.c 里各 handler 的注释）：
 *   · 上传：请求体就是文件本身，**边收边写**到同目录的隐藏临时文件，收完才改名；
 *           内存只用一块 PSRAM 搬运缓冲，任何大小的文件都不会占满 RAM。
 *   · 下载：分块读 + 分块发（chunked），不把整个文件读进内存。
 */
#ifndef WEB_FILES_H
#define WEB_FILES_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 开启文件共享。已连 WiFi → 局域网访问；未连 → 由 wifi_manager 开热点（APSTA）。
 * 幂等：已在运行直接返回 ESP_OK。 */
esp_err_t web_files_start(void);

/* 关闭。只注销自己并减 web_core 引用计数（别人还在用时服务不会停）；
 * 热点只关"本模块开的那一个"。 */
void web_files_stop(void);

bool web_files_is_running(void);

/* 取当前可访问地址："192.168.1.7" 或 "8.8.8.8"；还没有地址返回 false。 */
bool web_files_get_addr(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WEB_FILES_H */
