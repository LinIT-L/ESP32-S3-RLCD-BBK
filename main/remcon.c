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
 *   key <action>                 → 注入按键动作 (os_handle_action)
 *   tap <x> <y>                  → 注入触摸点击 (os_handle_touch)
 *   nes <path>                   → 直接拉起 NES ROM (自动复现崩溃)
 *   scan                         → 扫描 /sdcard/nes 下列出 .nes
 *   wait <ms>                    → 读任务 sleep ms (脚本节奏)
 *
 * 页面 id 名映射数名同 os_page_t, 见 s_pages 表.
 * 动作名同 os_action_t, 见 s_actions 表 (如 confirm/back/home/up/down/left/right).
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
#include "remcon.h"
#include "nes_emu.h"

#define RC_TAG      "remcon"
#define RC_LINE_MAX 256
#define RC_QUEUE_LEN 32

/* ---- 注入命令类型 ---- */
enum { RC_KEY = 1, RC_TAP, RC_PAGE };
typedef struct {
    uint8_t type;
    int a;   /* key=os_action_t / page=os_page_t ; tap=x */
    int b;   /* tap=y */
} rc_cmd_t;

static QueueHandle_t s_q;

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

/* ===== 立即执行 (在字符串读任务上下文, 不外发到 UI) ===== */
static void rc_cmd_immediate(char *args){
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
    } else if (strcmp(cmd, "help") == 0) {
        printf("CMDS: hello echo reboot loglevel mem blk audit stack uptime tasks ls cat rm cp diag page key tap nes scan wait help\r\n");
    }
    fflush(stdout);
}

/* ===== 注入队列 ===== */
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
        strcmp(cmd, "audit") == 0 || strcmp(cmd, "stack") == 0 ||
        strcmp(cmd, "uptime") == 0 || strcmp(cmd, "tasks") == 0 ||
        strcmp(cmd, "ls") == 0 ||
        strcmp(cmd, "cat") == 0 || strcmp(cmd, "rm") == 0 ||
        strcmp(cmd, "cp") == 0 || strcmp(cmd, "diag") == 0 ||
        strcmp(cmd, "help") == 0) {
        rc_cmd_immediate(buf);   /* 复用: 直接在此解析执行 */
        return;
    }
    if (strcmp(cmd, "key") == 0) {
        char *an = rc_next(&rest);
        if (!an) { printf("RC_ERR key? action\r\n"); return; }
        rc_lower(an);
        for (unsigned i = 0; i < sizeof(s_actions)/sizeof(s_actions[0]); i++)
            if (strcmp(an, s_actions[i].name) == 0) {
                rc_cmd_t m = { RC_KEY, (int)s_actions[i].act, 0 };
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
        }
    }
}

void remcon_init(void){
    s_q = xQueueCreate(RC_QUEUE_LEN, sizeof(rc_cmd_t));
    if (!s_q) { ESP_LOGE(RC_TAG, "queue create failed"); return; }
    BaseType_t ok = xTaskCreatePinnedToCore(rc_task, "remcon", 5120, NULL, 2, NULL, 1);
    ESP_LOGI(RC_TAG, "remote console up, task=%s", ok == pdPASS ? "ok" : "fail");
}