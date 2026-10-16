/**
 * remcon.c — 串口远程控制台实现.
 *
 * 命令格式 (一行一命令, 一行内可用 ';' 分隔多条, 大小写不敏感):
 *   hello                        → 回 SHOOT "REMOCON_READY <api>" (握手, PC 判定就绪)
 *   echo <text>                  → 回显 text
 *   help                         → 列出全部命令
 *   reboot | reset               → esp_restart()
 *   loglevel <0..5>              → 设置全局日志级别 (0=无 5=详细)
 *   heap                         → 打印 内部/PSRAM 空闲 + 最大块
 *   stack                        → 打印本任务栈高水位 (排障栈溢出用)
 *   uptime                       → 开机时长 (s)
 *   tasks                        → 全部任务名/状态/栈水位 (vTaskList)
 *   ls <dir>                     → 列目录 (默认 /sdcard)
 *   cat <file>                   → 打印文本文件前 400 行
 *   rm <file>                    → 删除文件 (清坏配置/日志)
 *   cp <src> <dst>               → 复制文件 (日志转存备份)
 *   diag                         → 综合诊断: uptime+内存+任务栈表+根目录
 *   page                         → 列出全部可用页面 id
 *   page <id>                    → 打开指定页面 (os_push)
 *   app <lua path>               → 直接拉起 Lua 应用 (等价应用管家点击, 供自动化回归)
 *   key <action>                 → 注入按键动作 (os_handle_action)
 *   tap <x> <y>                  → 注入触摸点击 (os_handle_touch)
 *   fbinfo                       → 帧缓冲指针/字节数/CRC (确认抓屏是否可用)
 *   fbdump                       → **抓屏**: 把 400×300 1bpp 帧缓冲以 base64 打印
 *                                  (主机侧 tools/fb_png.py 解码成 PNG)
 *   nes <path>                   → 直接拉起 NES ROM (自动复现崩溃)
 *   scan                         → 扫描 /sdcard/nes 下列出 .nes
 *   wait <ms>                    → 读任务 sleep ms (脚本节奏)
 *
 * 页面 id 名映射数名同 os_page_t, 见 s_pages 表.
 * 动作名同 os_action_t, 见 s_actions 表 (如 confirm/back/home/up/down/left/right).
 *
 * ==== fbdump 的数据格式 (主机侧解码依据) ====
 * ST7305 帧缓冲不是逐行 1bpp, 而是面板原生的"2 列 × 4 行 / 字节"打包 (见 st7305_draw_pixel):
 *     idx = (x >> 1) * 75 + ((299 - y) >> 2)        // 75 = H/4 列内 stride, 200 个字节列
 *     bit = 7 - ((((299 - y) & 3) << 1) | (x & 1))
 *     位 = 1 → 白(背景), 位 = 0 → 黑(笔画)
 * 共 200 × 75 = 15000 字节。y 是上下翻转的 (字节行 0 在屏幕最底部)。
 * 输出:
 *     FB_BEGIN 400 300 15000 <crc32-8hex>\\r\\n
 *     <base64 每行 76 字符>\\r\\n
 *     FB_END\\r\\n
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "mbedtls/base64.h"
#include "remcon.h"
#include "nes_emu.h"
#include "st7305.h"      /* 抓屏: 帧缓冲布局 + 面板尺寸 */
#include "os_hw.h"       /* wifi 命令: 走资源管家开/关射频 (与页面同一记账) */
#include "wifi_manager.h"
#include "input.h"   /* input_mark_activity(): remcon 注入按键也算"有人在用" */

#define RC_TAG      "remcon"
#define RC_LINE_MAX 256
#define RC_QUEUE_LEN 32

/* ---- 注入命令类型 ---- */
enum { RC_KEY = 1, RC_TAP, RC_PAGE, RC_APP };
typedef struct {
    uint8_t type;
    int a;   /* key=os_action_t / page=os_page_t ; tap=x */
    int b;   /* tap=y */
} rc_cmd_t;

static QueueHandle_t s_q;

/* RC_APP 的参数暂存 (Lua 脚本路径). 队列是异步派发的, 用静态缓冲避免动态分配;
 * 一次只会拉起一个应用, 无并发写。 */
static char s_rc_app_path[128];

/* 主屏句柄 (main.c 在 LCD 初始化后经 remcon_set_lcd 绑定) —— 抓屏命令用 */
static st7305_handle_t *s_lcd;

/* ST7305 帧缓冲字节数 = W/2 个字节列 × H/4 行 = 200 × 75 = 15000 */
#define RC_FB_BYTES  ((ST7305_WIDTH / 2) * (ST7305_HEIGHT / 4))

void remcon_set_lcd(void *lcd) { s_lcd = (st7305_handle_t *)lcd; }

/* ===== 页面 id 名映射 (与 os.h 的 os_page_t 一一对应, 便于全功能测试) ===== */
static const struct { const char *name; os_page_t page; } s_pages[] = {
    {"main", OS_PAGE_MAIN},
    {"select_game", OS_PAGE_SELECT_GAME},
    {"settings", OS_PAGE_SETTINGS},
    {"gamepad", OS_PAGE_GAMEPAD},
    {"display", OS_PAGE_SETTINGS_DISPLAY},
    {"time", OS_PAGE_SETTINGS_TIME},
    {"bt", OS_PAGE_SETTINGS_BT},
    {"sd", OS_PAGE_SETTINGS_SD},
    {"info", OS_PAGE_SETTINGS_INFO},
    {"btinfo", OS_PAGE_BTINFO},
    {"key_config", OS_PAGE_KEY_CONFIG},
    {"volume", OS_PAGE_VOLUME},
    {"return_game", OS_PAGE_RETURN_GAME},
    {"mp3", OS_PAGE_MP3_PLAYER},
    {"game_settings", OS_PAGE_GAME_SETTINGS},
    {"gb", OS_PAGE_GB_GAME},
    {"book", OS_PAGE_BOOK},
    {"wallpaper", OS_PAGE_WALLPAPER},
    {"pomodoro", OS_PAGE_POMODORO},
    {"usb_hid", OS_PAGE_USB_HID},
    {"app_manager", OS_PAGE_APP_MANAGER},
    {"storage", OS_PAGE_STORAGE},
    {"gbc", OS_PAGE_GBC_GAME},
    {"nes", OS_PAGE_NES_GAME},
    {"md", OS_PAGE_MD_GAME},
    {"sms", OS_PAGE_SMS_GAME},
    {"arduboy", OS_PAGE_ARDUBOY_GAME},
    {"diagnosis", OS_PAGE_DIAGNOSIS},
    {"wqx", OS_PAGE_WQX},
    {"vpet", OS_PAGE_VPET},
    {"meshtasic", OS_PAGE_MESHTASIC},
    {"whiteboard", OS_PAGE_WHITEBOARD},
    {"terminal", OS_PAGE_TERMINAL},
    {"sponsor", OS_PAGE_SPONSOR},
    {"usb_bt", OS_PAGE_USB_BT},
    {"usb_net", OS_PAGE_USB_NET},
    {"winassist", OS_PAGE_WINASSIST},
    {"modtool", OS_PAGE_MODTOOL},
    {"logdiag", OS_PAGE_LOGDIAO},
    {"calc", OS_PAGE_CALC},
    {"wifiprobe", OS_PAGE_WIFIPROBE},
    {"morse", OS_PAGE_MORSE},
    {"netdect", OS_PAGE_NETDECT},
    { "sdr",        OS_PAGE_SDR },      /* 频谱仪 (2026-10-01 新增, 便于远程 page sdr 驱动) */
};

/* ===== 动作名映射 ===== */
static const struct { const char *name; os_action_t act; } s_actions[] = {
    {"up", OS_ACTION_UP},
    {"down", OS_ACTION_DOWN},
    {"left", OS_ACTION_LEFT},
    {"right", OS_ACTION_RIGHT},
    {"confirm", OS_ACTION_CONFIRM},
    {"enter", OS_ACTION_CONFIRM},
    {"ok", OS_ACTION_CONFIRM},
    {"back", OS_ACTION_BACK},
    {"cancel", OS_ACTION_BACK},
    {"home", OS_ACTION_HOME},
    /* 2026-10-02: 加锁屏动作 —— 否则**远程无法进入屏保**,
     * "屏保按一次键不返回"这类 bug 就设法远程回归(输入注入不走 input 层, 不会自己进屏保)。 */
    {"lock",   OS_ACTION_POWER_LOCK},
    {"unlock", OS_ACTION_POWER_RELEASE},
    {"long_left", OS_ACTION_LONG_LEFT},
    {"long_press", OS_ACTION_LONG_PRESS},
    {"fav", OS_ACTION_KEY_FAV},
    {"power_lock", OS_ACTION_POWER_LOCK},
    {"power_hint", OS_ACTION_POWER_HINT},
    {"power_release", OS_ACTION_POWER_RELEASE},
    {"bt_search", OS_ACTION_BT_SEARCH},
};

/* ===== 字符串工具 ===== */
static void rc_lower(char *s){ for (; *s; s++) *s = (char)tolower((unsigned char)*s); }
/* 取下一个 token (以空白分隔), 修改 s, 返回 token 或 NULL; head=返回指向下一段的指针 */
static char *rc_next(char **rest){
    char *s = *rest;
    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) { *rest = NULL; return NULL; }
    char *tok = s;
    while (*s && !isspace((unsigned char)*s)) s++;
    if (*s) { *s = 0; *rest = s + 1; } else *rest = NULL;
    return tok;
}

/* ===== 立即执行 (在字符串读任务上下文, 不外发到 UI) =====
 * 注意: 必须在内部复制一份再用 —— 调用方 rc_exec 里的 buf 已被 rc_next 在
 * 命令名后写过 '\0', 直接传进去会丢掉后面所有参数(如 "shutdown auto" 变成 "shutdown")。 */
static void rc_cmd_mt(void);   /* 内部 RAM 运行时归属审计 (定义见文件后部) */
static void rc_cmd_immediate(const char *line){
    char args[RC_LINE_MAX];
    snprintf(args, sizeof(args), "%s", line);
    char *rest = args;
    char cmd[32];
    const char *c = rc_next(&rest);
    if (!c) c = "";
    snprintf(cmd, sizeof(cmd), "%s", c);
    rc_lower(cmd);

    if (strcmp(cmd, "hello") == 0) {
        printf("REMOCON_READY v1\r\n");
    } else if (strcmp(cmd, "echo") == 0) {
        printf("ECHO %s\r\n", rest ? rest : "");
    } else if (strcmp(cmd, "shutdown") == 0 || strcmp(cmd, "pwr") == 0 ||
               strcmp(cmd, "poweroff") == 0 || strcmp(cmd, "sleep") == 0) {
        /* 关机 = 深度休眠(复位式)。shutdown [auto|manual]:
         *   省略/manual → 手动关机路径(仅 PWR 键 + 按住 0.5s 开机, 显示 Logo)
         *   auto        → 模拟"休眠时间到点自动关机"(任意键唤醒, 不显示 Logo) */
        int auto_mode = (rest && strstr(rest, "auto") != NULL) ? 1 : 0;
        printf("REMOCON_SHUTDOWN %s\r\n", auto_mode ? "auto" : "manual");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(300));   /* 给串口回显留时间 */
        os_remote_shutdown(auto_mode);    /* 深度休眠, 不返回 */
    } else if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "reset") == 0) {
        printf("REMOCON_REBOOT\r\n");
        printf("REMOCON_REBOOT\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (strcmp(cmd, "loglevel") == 0) {
        char *lvl = rc_next(&rest); int v = lvl ? atoi(lvl) : -1;
        if (v >= 0 && v <= 5) { esp_log_level_set("*", (esp_log_level_t)v); printf("LOGLEVEL %d\r\n", v); }
        else printf("LOGLEVEL ERR (0..5)\r\n");
    } else if (strcmp(cmd, "wifi") == 0) {
        /* 调试/回归用: `wifi [on|off|status]` (默认 on)。
         * 为什么需要: 联网类 Lua 应用 (如航班雷达) 必须"已联网"才能跑 ——
         * lwip 是 wifi_manager_enable() 里 esp_netif_init() 建的, 没联过网时
         * 任何 socket 调用都会 assert 崩机。远程回归时用本命令先把网连上。
         * 走 os_hw_request/release, 与页面共用同一个引用计数。 */
        char *wa = rc_next(&rest);
        if (wa) rc_lower(wa);
        if (wa && strcmp(wa, "off") == 0) {
            os_hw_release(OS_HW_WIFI);
            printf("WIFI off (ref released)\r\n");
        } else if (wa && strcmp(wa, "status") == 0) {
            printf("WIFI active=%d connected=%d\r\n",
                   (int)os_hw_is_active(OS_HW_WIFI), (int)wifi_manager_is_connected());
        } else if (wa && strcmp(wa, "conn") == 0) {
            /* 用 NVS 里已存的 SSID/密码直接连 —— 远程回归没法人肉输密码,
             * 这是"不碰设备就能进联网态"的唯一入口。 */
            char ssid[33] = "", pass[65] = "";
            wifi_manager_get_saved(ssid, sizeof(ssid), pass, sizeof(pass));
            if (!ssid[0]) printf("WIFI conn: 无已存配置\r\n");
            else          printf("WIFI conn %s ok=%d\r\n", ssid, (int)wifi_manager_connect(ssid, pass));
        } else {
            printf("WIFI on ok=%d\r\n", (int)os_hw_request(OS_HW_WIFI));
        }
    } else if (strcmp(cmd, "audit") == 0) {
        /* 内存详细审计 (并入资源管家): 分类堆积 + 所有任务栈水位 + 堆块 */
        os_housekeeper_mem_audit();
    } else if (strcmp(cmd, "heap") == 0 || strcmp(cmd, "mem") == 0) {
        /* 整机内存概览: 内部/PSRAM 总量、空闲、最大连续块 */
        uint32_t it = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        uint32_t iu = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        uint32_t il = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        uint32_t pt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        uint32_t pu = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        uint32_t pl = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        uint32_t in_used = it - iu;
        uint32_t pr_used = pt - pu;
        printf("MEM internal total=%u free=%u used=%u largest=%u | psram total=%u free=%u used=%u largest=%u\r\n",
               (unsigned)it, (unsigned)iu, (unsigned)in_used, (unsigned)il,
               (unsigned)pt, (unsigned)pu, (unsigned)pr_used, (unsigned)pl);
        printf("HEAP internal=%u psram=%u\r\n", (unsigned)iu, (unsigned)pu);
    } else if (strcmp(cmd, "blk") == 0) {
        /* 内部堆块级 dump: 定位具体模块/静态缓冲占用 */
        heap_caps_dump(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    } else if (strcmp(cmd, "mt") == 0) {
        /* 内部 RAM **运行时归属**审计 (2026-10-16): 谁在用内部堆 */
        rc_cmd_mt();
    } else if (strcmp(cmd, "stack") == 0) {
        printf("STACK mine=%u\r\n",
               (unsigned)uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()));
    } else if (strcmp(cmd, "uptime") == 0) {
        printf("UPTIME %.1fs\r\n", (double)esp_timer_get_time() / 1e6);
    } else if (strcmp(cmd, "tasks") == 0) {
        char *buf = (char *)malloc(4096 * 2);
        if (!buf) printf("RC_ERR tasks oom\r\n");
        else {
            vTaskList(buf);
            printf("TASKS\r\n%sTASKS_END\r\n", buf);
            free(buf);
        }
    } else if (strcmp(cmd, "ls") == 0) {
        char *d = rc_next(&rest); if (!d || !*d) d = "/sdcard";
        DIR *dd = opendir(d);
        if (!dd) printf("RC_ERR ls %s\r\n", d);
        else {
            struct dirent *e;
            while ((e = readdir(dd)) != NULL) {
                if (e->d_name[0] == '.') continue;
                printf("LS %s/%s%s\r\n", d, e->d_name,
                       (e->d_type == DT_DIR) ? "/" : "");
            }
            closedir(dd);
            printf("LS_DONE\r\n");
        }
    } else if (strcmp(cmd, "cat") == 0) {
        char *p = rc_next(&rest);
        if (!p || !*p) { printf("RC_ERR cat <file>\r\n"); }
        else {
            FILE *f = fopen(p, "r");
            if (!f) printf("RC_ERR cat open %s\r\n", p);
            else {
                char line[256]; int n = 0;
                printf("CAT %s\r\n", p);
                while (fgets(line, sizeof line, f) && n < 400) {
                    char *s = line; printf("%s", s); n++;
                }
                printf("\r\nCAT_END\r\n");
                fclose(f);
            }
        }
    } else if (strcmp(cmd, "rm") == 0) {
        char *p = rc_next(&rest);
        if (!p || !*p) printf("RC_ERR rm <file>\r\n");
        else printf("RM %s -> %s\r\n", p, remove(p) == 0 ? "ok" : "fail");
    } else if (strcmp(cmd, "cp") == 0) {
        char *s = rc_next(&rest), *d = rc_next(&rest);
        if (!s || !d) { printf("RC_ERR cp <src> <dst>\r\n"); }
        else {
            FILE *fs = fopen(s, "rb");
            if (!fs) printf("RC_ERR cp open %s\r\n", s);
            else {
                FILE *fd = fopen(d, "wb");
                if (!fd) { printf("RC_ERR cp open %s\r\n", d); fclose(fs); }
                else {
                    char buf[1024]; size_t n; size_t tot = 0;
                    while ((n = fread(buf, 1, sizeof buf, fs)) > 0) { fwrite(buf, 1, n, fd); tot += n; }
                    fclose(fs); fclose(fd);
                    printf("CP %s -> %s (%u B)\r\n", s, d, (unsigned)tot);
                }
            }
        }
    } else if (strcmp(cmd, "diag") == 0) {
        /* 综合诊断: uptime + 内部内存 + 任务栈表 + 根目录 */
        printf("DIAG\r\n");
        printf("uptime=%.1fs\r\n", (double)esp_timer_get_time() / 1e6);
        printf("mem_internal_free=%u\r\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        {
            char *buf = (char *)malloc(8192);
            if (buf) { vTaskList(buf); printf("%s", buf); free(buf); }
        }
        {
            DIR *d = opendir("/sdcard");
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) if (e->d_name[0] != '.')
                    printf("LSROOT %s%s\r\n", e->d_name, (e->d_type == DT_DIR) ? "/" : "");
                closedir(d);
            }
        }
        printf("DIAG_END\r\n");
    } else if (strcmp(cmd, "fbinfo") == 0) {
        /* 抓屏前置检查: 确认主屏句柄已绑定、帧缓冲已分配 */
        if (!s_lcd || !s_lcd->fb) {
            printf("FB_NONE lcd=%p fb=%p\r\n", (void *)s_lcd, s_lcd ? (void *)s_lcd->fb : NULL);
        } else {
            uint32_t crc = esp_rom_crc32_le(0, s_lcd->fb, RC_FB_BYTES);
            printf("FB %dx%d bytes=%d crc=%08x ptr=%p\r\n",
                   ST7305_WIDTH, ST7305_HEIGHT, RC_FB_BYTES, (unsigned)crc, (void *)s_lcd->fb);
        }
    } else if (strcmp(cmd, "fbdump") == 0) {
        /* 抓屏: 帧缓冲 → base64 → 串口。15000B → 20000 字符 → 264 行。
         * 缓冲区从堆上取 (20KB, 不能放 5120B 的读任务栈); 编码用 IDF 自带 mbedtls。
         *
         * ★ 2026-10-03 修「CRC 恒定不符, 长度却精确」:
         *   原来 base64 与 CRC32 各自**直接读 s_lcd->fb**。当画面在逐帧重绘时
         *   (例如全屏刷新的游戏), 这两次读之间缓冲已经被改写 —— 症状是每次都
         *   "长度=15000 正确, CRC 不符, 且三次抓到三个不同的 CRC"(实测), 很容易
         *   被误判成"串口丢字节"或"固件坏了"。
         *   正确做法: **先 memcpy 一份快照, 编码与 CRC 都只认这份快照**, 并顺手把
         *   fb 指针固定下来 (渲染侧若换过缓冲, 两次读会指向不同内存)。 */
        if (!s_lcd || !s_lcd->fb) { printf("FB_NONE\r\n"); }
        else {
            size_t b64_cap = 4 * ((RC_FB_BYTES + 2) / 3) + 8;
            char *b64 = (char *)heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!b64) b64 = (char *)malloc(b64_cap);
            uint8_t *snap = (uint8_t *)heap_caps_malloc(RC_FB_BYTES, MALLOC_CAP_SPIRAM);
            if (!snap) snap = (uint8_t *)malloc(RC_FB_BYTES);
            const uint8_t *src = s_lcd->fb;
            if (snap) { memcpy(snap, src, RC_FB_BYTES); src = snap; }
            size_t b64_len = 0;
            if (!b64) {
                printf("FB_ERR no mem (%u B)\r\n", (unsigned)b64_cap);
            } else if (mbedtls_base64_encode((unsigned char *)b64, b64_cap, &b64_len,
                                             src, RC_FB_BYTES) != 0) {
                printf("FB_ERR base64 encode failed\r\n");
            } else {
                uint32_t crc = esp_rom_crc32_le(0, src, RC_FB_BYTES);
                printf("FB_BEGIN %d %d %d %08x\r\n",
                       ST7305_WIDTH, ST7305_HEIGHT, RC_FB_BYTES, (unsigned)crc);
                for (size_t i = 0; i < b64_len; i += 76) {
                    size_t n = b64_len - i; if (n > 76) n = 76;
                    fwrite(b64 + i, 1, n, stdout);
                    fwrite("\r\n", 1, 2, stdout);
                }
                printf("FB_END\r\n");
            }
            free(b64);
            free(snap);
            fflush(stdout);
        }
    } else if (strcmp(cmd, "help") == 0) {
        printf("CMDS: hello echo reboot shutdown loglevel mem blk mt audit stack uptime tasks ls cat rm cp diag page key tap fbinfo fbdump nes scan app wait help\r\n");
    }
    fflush(stdout);
}

/* ================== mt: 内部 RAM 运行时归属审计 ==================
 * 回答"**到底是什么程序在用内部 RAM**"的设备侧命令 (2026-10-16)。
 * 静态占用(编译期 .bss/.data/IRAM) → 主机命令 `python tools/mem_audit.py`
 * 运行时占用(堆) → 本命令:
 *   ① 内部堆的每个 region (起止地址), 看堆到底覆盖了哪些 SRAM
 *   ② 每个任务: 栈基址 / 栈总大小(从堆块反查) / 剩余水位 / 空隙浪费率
 *   ③ 内部堆每个已分配块 → 归属到「某任务栈」或「非栈」, 两者分别求和
 *   ④ 非栈块中 ≥1KB 的逐条列出 (拿去和源码里的缓冲尺寸对号)
 * 说明: 非栈块没有归属信息, 要看"哪个函数分配的"需开 CONFIG_HEAP_TRACING;
 *       本命令的定位是**先量出谁占大头**, 不做逐次分配追踪。 */
#define MT_MAX_TASK   24
#define MT_MAX_REGION  8
#define MT_MAX_BIG    40
typedef struct { const char *name; uint32_t base; uint32_t size; uint32_t hwm; } mt_task_t;
typedef struct { uint32_t start, end; } mt_region_t;
static mt_task_t  s_mt_task[MT_MAX_TASK];
static int        s_mt_n;
static mt_region_t s_mt_reg[MT_MAX_REGION];
static int        s_mt_reg_n;
static uint32_t   s_mt_stack_bytes, s_mt_other_bytes, s_mt_other_blocks;
static uint32_t   s_mt_big[MT_MAX_BIG];
static int        s_mt_big_n;

static bool mt_region_cb(walker_heap_into_t hi, walker_block_info_t bi, void *ud)
{
    (void)bi; (void)ud;
    for (int i = 0; i < s_mt_reg_n; i++)
        if (s_mt_reg[i].start == (uint32_t)hi.start) return true;
    if (s_mt_reg_n < MT_MAX_REGION) {
        s_mt_reg[s_mt_reg_n].start = (uint32_t)hi.start;
        s_mt_reg[s_mt_reg_n].end   = (uint32_t)hi.end;
        s_mt_reg_n++;
    }
    return true;
}

static bool mt_attr_cb(walker_heap_into_t hi, walker_block_info_t bi, void *ud)
{
    (void)hi; (void)ud;
    if (!bi.used) return true;
    uint32_t a = (uint32_t)bi.ptr;
    for (int i = 0; i < s_mt_n; i++) {
        if (!s_mt_task[i].size) continue;
        if (a >= s_mt_task[i].base && a < s_mt_task[i].base + s_mt_task[i].size) {
            s_mt_stack_bytes += bi.size;
            return true;
        }
    }
    s_mt_other_bytes += bi.size;
    s_mt_other_blocks++;
    if (bi.size >= 1024 && s_mt_big_n < MT_MAX_BIG) s_mt_big[s_mt_big_n++] = (uint32_t)bi.size;
    return true;
}

static int mt_cmp_desc(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x < y) ? 1 : (x > y ? -1 : 0);
}

static void rc_cmd_mt(void)
{
    /* ---- ① 堆 region (先走一遍, 拿到堆的范围; 后面反查栈块大小要靠它做保护) ---- */
    s_mt_reg_n = 0;
    heap_caps_walk(MALLOC_CAP_INTERNAL, mt_region_cb, NULL);
    printf("MT regions=%d\r\n", s_mt_reg_n);
    for (int i = 0; i < s_mt_reg_n; i++) {
        uint32_t sz = s_mt_reg[i].end - s_mt_reg[i].start;
        printf("MT  region 0x%08x..0x%08x %7u B (%u KB)\r\n",
               (unsigned)s_mt_reg[i].start, (unsigned)s_mt_reg[i].end,
               (unsigned)sz, (unsigned)(sz / 1024));
    }

    /* ---- ② 任务表: 栈基址 / 栈大小 / 水位 ---- */
    TaskStatus_t *st = (TaskStatus_t *)heap_caps_malloc(MT_MAX_TASK * sizeof(TaskStatus_t),
                                                       MALLOC_CAP_SPIRAM);
    if (!st) { printf("MT_ERR oom\r\n"); return; }
    UBaseType_t n = uxTaskGetSystemState(st, MT_MAX_TASK, NULL);
    s_mt_n = 0;
    for (UBaseType_t i = 0; i < n && s_mt_n < MT_MAX_TASK; i++) {
        if (!st[i].pxStackBase) continue;
        uint32_t base = (uint32_t)st[i].pxStackBase;
        uint32_t sz = 0;
        /* ★只有地址落在**内部堆 region** 里才能反查块大小, 否则
         *  heap_caps_get_containing_block_size 会断言崩 → 静态栈(PSRAM/.bss)一律记 0 */
        for (int r = 0; r < s_mt_reg_n; r++)
            if (base >= s_mt_reg[r].start && base < s_mt_reg[r].end) {
                sz = (uint32_t)heap_caps_get_containing_block_size((void *)base);
                break;
            }
        s_mt_task[s_mt_n].name = st[i].pcTaskName;
        s_mt_task[s_mt_n].base = base;
        s_mt_task[s_mt_n].size = sz;
        s_mt_task[s_mt_n].hwm  = (uint32_t)st[i].usStackHighWaterMark;
        s_mt_n++;
    }
    free(st);

    /* ---- ③ 归属统计 ---- */
    s_mt_stack_bytes = s_mt_other_bytes = 0; s_mt_other_blocks = 0; s_mt_big_n = 0;
    heap_caps_walk(MALLOC_CAP_INTERNAL, mt_attr_cb, NULL);

    uint32_t total_used = heap_caps_get_total_size(MALLOC_CAP_INTERNAL)
                        - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    printf("MT internal_used=%u  任务栈=%u (%.1f%%)  非栈=%u (%.1f%%)  [块开销/差 %d]\r\n",
           (unsigned)total_used, (unsigned)s_mt_stack_bytes,
           total_used ? (double)s_mt_stack_bytes * 100.0 / total_used : 0.0,
           (unsigned)s_mt_other_bytes,
           total_used ? (double)s_mt_other_bytes * 100.0 / total_used : 0.0,
           (int)total_used - (int)(s_mt_stack_bytes + s_mt_other_bytes));

    printf("MT tasks (name base size free_water waste):\r\n");
    uint32_t tsum = 0;
    for (int i = 0; i < s_mt_n; i++) {
        const mt_task_t *t = &s_mt_task[i];
        int waste = t->size ? (int)((uint64_t)t->hwm * 100 / t->size) : -1;
        tsum += t->size;
        printf("MT  %-12s 0x%08x %7u %7u %4d%%\r\n",
               t->name ? t->name : "?", (unsigned)t->base,
               (unsigned)t->size, (unsigned)t->hwm, waste);
    }
    printf("MT task_stacks_total=%u\r\n", (unsigned)tsum);

    /* ---- ④ 非栈大块 (去重后按大小降序) ---- */
    qsort(s_mt_big, s_mt_big_n, sizeof(uint32_t), mt_cmp_desc);
    printf("MT nonstack_blocks=%u  >=1KB 共 %d 块:\r\n",
           (unsigned)s_mt_other_blocks, s_mt_big_n);
    for (int i = 0; i < s_mt_big_n; i++) printf("MT  big %u\r\n", (unsigned)s_mt_big[i]);
    printf("MT_DONE\r\n");
}


static void rc_enqueue(const rc_cmd_t *c){
    if (s_q) xQueueSend(s_q, c, pdMS_TO_TICKS(100));
}

/* 递归扫描目录下所有 .nes 并打印完整路径 */
static void scan_nes_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { printf("SCAN open fail: %s\r\n", dir); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[RC_LINE_MAX];
        int pl = snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);   /* -Werror 截断: 检查返回值 */
        if (pl < 0 || (size_t)pl >= sizeof(p)) continue;
        if (e->d_type == DT_DIR) {
            scan_nes_tree(p);
        } else {
            size_t n = strlen(e->d_name);
            if (n > 4 && strcasecmp(e->d_name + n - 4, ".nes") == 0)
                printf("NES %s\r\n", p);
        }
    }
    closedir(d);
}

/* 解析并派发一条命令: 立即命令直执行; UI 命令入队 */
static void rc_exec(const char *line){
    char buf[RC_LINE_MAX];
    snprintf(buf, sizeof(buf), "%s", line);
    char *rest = buf;
    char cmd[32];
    const char *c = rc_next(&rest);
    if (!c) return;
    snprintf(cmd, sizeof(cmd), "%s", c);
    rc_lower(cmd);

    if (strcmp(cmd, "wait") == 0) {
        char *ms = rc_next(&rest); int m = ms ? atoi(ms) : 0;
        if (m > 0 && m < 120000) vTaskDelay(pdMS_TO_TICKS(m));
        return;
    }
    if (strcmp(cmd, "hello") == 0 || strcmp(cmd, "echo") == 0 ||
        strcmp(cmd, "reboot") == 0 || strcmp(cmd, "reset") == 0 ||
        strcmp(cmd, "loglevel") == 0 || strcmp(cmd, "heap") == 0 ||
        strcmp(cmd, "mem") == 0 || strcmp(cmd, "blk") == 0 ||
        strcmp(cmd, "mt") == 0 ||
        strcmp(cmd, "audit") == 0 || strcmp(cmd, "stack") == 0 ||
        strcmp(cmd, "uptime") == 0 || strcmp(cmd, "tasks") == 0 ||
        strcmp(cmd, "ls") == 0 || strcmp(cmd, "wifi") == 0 ||
        strcmp(cmd, "cat") == 0 || strcmp(cmd, "rm") == 0 ||
        strcmp(cmd, "cp") == 0 || strcmp(cmd, "diag") == 0 ||
        strcmp(cmd, "help") == 0 || strcmp(cmd, "shutdown") == 0 ||
        strcmp(cmd, "fbinfo") == 0 || strcmp(cmd, "fbdump") == 0 ||
        strcmp(cmd, "pwr") == 0 || strcmp(cmd, "poweroff") == 0 ||
        strcmp(cmd, "sleep") == 0) {
        rc_cmd_immediate(line);   /* 传原始行(含参数); buf 已被 rc_next 截断 */
        return;
    }
    if (strcmp(cmd, "key") == 0) {
        char *an = rc_next(&rest);
        if (!an) { printf("RC_ERR key? action\r\n"); return; }
        rc_lower(an);
        for (unsigned i = 0; i < sizeof(s_actions)/sizeof(s_actions[0]); i++)
            if (strcmp(an, s_actions[i].name) == 0) {
                rc_cmd_t m = { RC_KEY, (int)s_actions[i].act, 0 };
                /* ⚠ remcon 注入的按键**不走 input_get_action()**, 所以不会刷新空闲计时 →
                 *   远程跑长回归时会撞上屏保/自动休眠, 表现为"设备突然黑屏没反应"。
                 *   这里显式标一次"有人在用"。 */
                input_mark_activity();
                rc_enqueue(&m); return;
            }
        printf("RC_ERR unknown action '%s'\r\n", an);
    } else if (strcmp(cmd, "tap") == 0) {
        char *sx = rc_next(&rest), *sy = rc_next(&rest);
        if (!sx || !sy) { printf("RC_ERR tap <x> <y>\r\n"); return; }
        rc_cmd_t m = { RC_TAP, atoi(sx), atoi(sy) };
        rc_enqueue(&m);
    } else if (strcmp(cmd, "nes") == 0) {
        /* 直接拉起指定 NES ROM (整行剩余作路径, 支持含空格, 如含空格名 ROM).
         * 模拟任务独立运行 → 崩溃 panic 打到同串口, 便于自动化复现/回归. */
        char *p = rest;
        while (p && *p && (isspace((unsigned char)*p))) p++;
        if (!p || !*p) { printf("RC_ERR nes <path>\r\n"); return; }
        size_t pl = strlen(p);
        while (pl && (p[pl-1]=='\r' || p[pl-1]=='\n' || isspace((unsigned char)p[pl-1]))) p[--pl]=0;
        esp_err_t e = nes_emu_start(p);
        printf("NESSTART '%s' -> %d (%s)\r\n", p, (int)e, esp_err_to_name(e));
    } else if (strcmp(cmd, "scan") == 0) {
        /* 递归扫描 /sdcard/nes 下所有 .nes, 打印路径 (定位霸王的大陆等独立 ROM) */
        scan_nes_tree("/sdcard/nes");
        printf("SCAN_DONE\r\n");
    } else if (strcmp(cmd, "page") == 0) {
        char *pn = rc_next(&rest);
        if (!pn) {   /* page 无参 → 打印可用页列表 */
            for (unsigned i = 0; i < sizeof(s_pages)/sizeof(s_pages[0]); i++)
                printf("PAGE %s\r\n", s_pages[i].name);
            fflush(stdout);
            return;
        }
        rc_lower(pn);
        int found = 0;
        for (unsigned i = 0; i < sizeof(s_pages)/sizeof(s_pages[0]); i++)
            if (strcmp(pn, s_pages[i].name) == 0) {
                rc_cmd_t m = { RC_PAGE, (int)s_pages[i].page, 0 };
                rc_enqueue(&m); found = 1; break;
            }
        if (!found) printf("RC_ERR unknown page '%s'\r\n", pn);
    } else if (strcmp(cmd, "app") == 0) {
        /* 直接拉起指定 Lua 应用 (等价于在应用管家点这一项):
         * 设置 boot 路径 + 进 Lua 页。用途 = 自动化回归: 远程打开应用后
         * 注入按键 + fbdump 抓屏, 验证能导航、不崩溃。 */
        char *p = rest;
        while (p && *p && isspace((unsigned char)*p)) p++;
        if (!p || !*p) { printf("RC_ERR app <lua path>\r\n"); return; }
        size_t pl = strlen(p);
        while (pl && (p[pl-1] == '\r' || p[pl-1] == '\n' ||
                      isspace((unsigned char)p[pl-1]))) p[--pl] = 0;
        snprintf(s_rc_app_path, sizeof(s_rc_app_path), "%s", p);
        rc_cmd_t m = { RC_APP, 0, 0 };
        rc_enqueue(&m);
    } else {
        printf("RC_ERR unknown cmd '%s' (help)\r\n", cmd);
    }
}

/* ===== stdin 读任务: 逐行(含 ';' 分隔)读取并派发 ===== */
static void rc_task(void *arg){
    (void)arg;
    char acc[RC_LINE_MAX]; size_t n = 0;
    for (;;) {
        char ch;
        ssize_t r = read(STDIN_FILENO, &ch, 1);
        if (r <= 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        /* '\n'、'\r'、';' 均视为命令分隔 */
        if (ch == '\n' || ch == '\r' || ch == ';') {
            if (n > 0) {
                acc[n] = 0;
                char *p = acc;
                /* 跳过前导空白 */
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p) rc_exec(p);
                n = 0;
            }
        } else if (n < RC_LINE_MAX - 1) {
            acc[n++] = ch;
        }
    }
}

/* ===== 主循环派发: 取出队列内命令交给 os 内核 ===== */
void remcon_tick(ui_ctx_t *ctx){
    rc_cmd_t m;
    if (s_q && xQueueReceive(s_q, &m, 0) == pdTRUE) {
        switch (m.type) {
        case RC_KEY:  os_handle_action(ctx, (os_action_t)m.a); break;
        case RC_TAP:  os_handle_touch(ctx, m.a, m.b);          break;
        case RC_PAGE: os_push(ctx, (os_page_t)m.a);            break;
        case RC_APP:  os_page_lua_set_boot(s_rc_app_path);      /* 先设 boot 路径 */
                      os_push(ctx, OS_PAGE_LUA);               break;
        }
    }
}

void remcon_init(void){
    s_q = xQueueCreate(RC_QUEUE_LEN, sizeof(rc_cmd_t));
    if (!s_q) { ESP_LOGE(RC_TAG, "queue create failed"); return; }
    BaseType_t ok = xTaskCreatePinnedToCore(rc_task, "remcon", 5120, NULL, 2, NULL, 1);
    ESP_LOGI(RC_TAG, "remote console up, task=%s", ok == pdPASS ? "ok" : "fail");
}