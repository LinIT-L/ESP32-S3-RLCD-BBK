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
#include <stdint.h>

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

/* ★ 2026-11-25: 是否**正在传输**(上传/下载流)。
 * 给省电系统用: 局域网传文件时浏览器那头没有按键/触摸, 按"输入空闲"判断会
 * 1 分钟就息屏、3 分钟关射频 —— 射频一关, httpd 的 socket 全断, 传输直接被腰斩。
 * 只读 s_xfer 计数, 纯内存访问, 不加锁、不碰 flash, 可以在 PSRAM 栈的
 * 屏保监督任务里安全调用。传输结束立刻返回 false, 不会长期阻止省电。 */
bool web_files_busy(void);

/* ★ 2026-11-25: "有用户在用网页文件共享"的保活判据 (省电专用)。
 * 在 busy(仅流式传输)之外, 把最近 30s 内的任意 API 请求(翻目录/建目录/删除/改名)
 * 也算活动 —— 避免浏览器翻目录的停顿被误判闲置、进壁纸/关射频把会话砍断。
 * 纯内存时间戳比较, 可在 PSRAM 栈的屏保监督任务里调用; 传完 30s 自动 false,
 * 不长期阻止省电。 */
bool web_files_active(void);

/* ─────────────────────────────────────────────────────────────────────
 * USB MSC「挂载到电脑」前后的清场 / 恢复
 *
 * 2026-11-25 起, 这几个调用的**顺序由 usbh_msc 的会话状态机统一负责**
 * (见 services/usbh_msc/include/usbh_msc_sdspi.h): 会话任务在 PREPARING 阶段先
 *   web_files_quiesce() → sd_vfs_handoff() → 起 USB,
 * 在 RESTORING 阶段 sd_vfs_reclaim() → web_files_resume()。
 * 页面/其它模块**不要**再自己拼这套顺序 —— 漏掉一次 quiesce 就是
 * "电脑一访问就重启": handler 手里的 FILE* 跨过了 VFS 注销点。
 * ───────────────────────────────────────────────────────────────────── */

/* 停止受理并等正在跑的传输收尾。返回 false = 超时(仍有传输未退出)。
 * ⚠ 调用后必须配 web_files_resume(), 否则共享会一直返回 503。 */
bool web_files_quiesce(uint32_t timeout_ms);
void web_files_resume(void);

/* 开机自动恢复: 若上次是"开着共享"关机的, 连上 WiFi 后自动拉起。
 * 由 main 在启动阶段调用一次, 内部只在需要时建一个低优先级等待任务。 */
void web_files_boot_restore(void);

/* 取当前可访问地址："192.168.1.7" 或 "8.8.8.8"；还没有地址返回 false。 */
bool web_files_get_addr(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WEB_FILES_H */
