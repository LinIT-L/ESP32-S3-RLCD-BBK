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
 *   audio                        → 音频链路诊断: 音量/静音/功放/平台/I2S 状态 + ES8311 全寄存器回读
 *                                  (排查"芯片认到了但没声音"; 输出带 ADIAG / ESREG 前缀)
 *   vol [0-100]                  → 读音量; 带参数则现场设音量 (分辨"音量=0"还是"通路没开")
 *   esr <reg>                    → 回读 ES8311 单个寄存器
 *   esw <reg> <val>              → 写 ES8311 寄存器 (现场试听定位)
 *   es7 [reg]                    → 回读 **ES7210**(麦克风 ADC) 寄存器; 不带参数 = 全量 0x00~0x4C
 *   es7w <reg> <val>             → 写 ES7210 寄存器
 *   mic [ms]                     → **麦克风链路一条龙诊断**: 起麦(16k)→连抓样点→报 peak/min/max/rms
 *                                  (分离"起麦失败" / "样点恒 0" / "样点极弱" 三种故障)
 *   beep [freq] [ms]             → **纯音测试**: 绕过 MP3/解码/SD 直接让 codec 出声,
 *                                  用来一刀切开"固件问题"与"喇叭/功放硬件问题"
 *   gps                          → GPS 电源状态 (待机 46µA / 跟踪 ~10mA) + 引脚
 *   wifi [on|off|status|conn|set &lt;ssid&gt; &lt;pass&gt;]
 *                                → 射频开关 / 状态 / 用 NVS 已存配置连 / **写入凭据并连接**
 *                                  (set 是"掌机上打密码太痛苦"的免打字配网入口)
 *   pm                           → 电源管理诊断: DFS 目标频率 + **PM 锁表**
 *                                  (有 API 锁=降不了频, 有 SLEEP 锁=进不了浅睡)
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
#include <math.h>
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
#include "audio_player.h"  /* audio/vol: 音频链路诊断 + 音量现场调节 (2026-10-09) */
#include "es8311.h"        /* esr/esw: ES8311 寄存器现场回读/改写 */
#include "es7210.h"        /* es7/es7w/mic: ES7210 (麦克风 ADC) 寄存器回读 + 录音链路诊断 */
#include "tone_player.h"   /* beep: 纯音测试 (绕过 MP3/解码/SD, 分离固件 vs 喇叭硬件) */
#include "gps.h"           /* gps: 查 GPS 电源状态 (待机 46µA / 跟踪 ~10mA) */
#include "esp_pm.h"        /* pm: 打印 PM 配置 + 锁表 (看 DFS/浅睡是否被驱动锁住) */
#include "board_battery.h" /* bat: 电池 raw/脚上mV/分压系数/VBAT 现场核对 (2026-10-14) */
#include "am_store_json.h"   /* ★ am_rpkg_t (storedl 命令用) */

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

/* ===== 麦克风链路诊断 (2026-10-14) =====
 * 小智模块是 **ES8311(喇叭 DAC) + ES7210(麦克风 ADC) 两颗独立芯片**, 共一条 I2S 总线:
 *   "ES7210 初始化完成" 只证明 **I2C 通**, 完全不代表 ADC 有数据往 ESP32 送。
 * 本命令把"有没有数据"和"数据是什么"一次量出来 —— 只报事实, 不做推断。 */
static void rc_mic_probe(int ms)
{
    static int16_t *buf;
    /* ⚠ 需求字节 = frames × 2(立体声) × 2(int16) = frames × 4 —— 申请时别漏了立体声那 2 倍,
     *   否则 audio_player_mic_read() 会往堆里多写 8KB (我第一版就踩了, 直接 StoreProhibited)。 */
    if (!buf) buf = heap_caps_malloc(4096 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) { printf("MIC_ERR no mem\r\n"); return; }

    bool was = audio_player_mic_active();
    if (!was && audio_player_mic_start(16000) != 0) {
        printf("MIC_ERR start fail (es7210 addr=0x%02X)\r\n", es7210_address());
        return;
    }
    int64_t t0 = esp_timer_get_time() / 1000;
    long n = 0;
    int64_t ss = 0;                      /* ⚠ 必须 64 位: 32767² × 40960 早已超 int32 (ESP32 的 long) */
    int pk = 0, mn = 0, mx = 0, reads = 0, empty = 0;
    /* 分声道统计: ES7210 非 TDM 时 SDOUT1 左槽 = MIC1 / 右槽 = MIC2,
     * 两路幅度差很多 ⇒ 说明只焊了一个咪头(另一个槽是空脚/噪声)。 */
    long ln = 0, rn = 0;
    int64_t lss = 0, rss = 0, ldc = 0, rdc = 0;
    int lpk = 0, rpk = 0;
    while (esp_timer_get_time() / 1000 - t0 < ms) {
        int g = audio_player_mic_read(buf, 4096, 50);
        if (g <= 0) { empty++; continue; }
        reads++;
        for (int k = 0; k < g; k++) {
            int vl = buf[2 * k], vr = buf[2 * k + 1];
            ldc += vl; rdc += vr;
            lss += (int64_t)vl * vl; rss += (int64_t)vr * vr;
            int al = vl < 0 ? -vl : vl;
            int ar = vr < 0 ? -vr : vr;
            if (al > lpk) lpk = al;
            if (ar > rpk) rpk = ar;
            ln++; rn++;
            if (vl < mn) mn = vl;
            if (vl > mx) mx = vl;
            if (vr < mn) mn = vr;
            if (vr > mx) mx = vr;
            int a = al > ar ? al : ar;
            if (a > pk) pk = a;
            ss += (int64_t)vl * vl + (int64_t)vr * vr;
            n += 2;
        }
    }
    printf("MIC addr=0x%02X %ldms n=%ld reads=%d empty=%d peak=%d min=%d max=%d rms=%ld\r\n",
           es7210_address(), (long)(esp_timer_get_time() / 1000 - t0), n, reads, empty,
           pk, mn, mx, n ? (long)sqrtf((float)((double)ss / (double)n)) : -1L);
    printf("MICCH L(dBFS)=%.1f rms=%ld dc=%ld peak=%d | R(dBFS)=%.1f rms=%ld dc=%ld peak=%d\r\n",
           ln ? 20.0f * log10f(sqrtf((float)((double)lss / (double)ln)) / 32768.0f + 1e-9f) : -99.0f,
           ln ? (long)sqrtf((float)((double)lss / (double)ln)) : -1L, ln ? (long)(ldc / ln) : 0L, lpk,
           rn ? 20.0f * log10f(sqrtf((float)((double)rss / (double)rn)) / 32768.0f + 1e-9f) : -99.0f,
           rn ? (long)sqrtf((float)((double)rss / (double)rn)) : -1L, rn ? (long)(rdc / rn) : 0L, rpk);
    if (!was) audio_player_mic_stop();
}

/* ===== 立即执行 (在字符串读任务上下文, 不外发到 UI) =====
 * 注意: 必须在内部复制一份再用 —— 调用方 rc_exec 里的 buf 已被 rc_next 在
 * 命令名后写过 '\0', 直接传进去会丢掉后面所有参数(如 "shutdown auto" 变成 "shutdown")。 */
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
    } else if (strcmp(cmd, "storedl") == 0) {
        /* ★ 2026-10-18: 串口触发商店下载(绕过 UI, 用于自动化测试)。
         *   用法: storedl [包名]   不带参数则列出索引里的包 */
        extern int  am_rstore_count(void);
        extern const am_rpkg_t *am_rstore_get(int i);
        extern bool am_rstore_dl_start(int i);
        extern const char *am_rstore_dl_name(void);
        extern int  am_rstore_dl_permille(void);
        extern void am_rstore_prefetch(void);
        extern bool am_rstore_tick(void);
        int n = am_rstore_count();
        if (n == 0) {
            /* 索引还没加载(平时只在进商店时加载) ⇒ 主动拉一次内置索引 */
            am_rstore_prefetch();
            for (int w = 0; w < 20 && am_rstore_count() == 0; w++) vTaskDelay(pdMS_TO_TICKS(100));
            am_rstore_tick();
            n = am_rstore_count();
            printf("STORE_LOADED %d\r\n", n);
        }
        if (!rest || !*rest) {
            printf("STORE_PKGS %d\r\n", n);
            for (int i = 0; i < n; i++) {
                const am_rpkg_t *rp = am_rstore_get(i);
                if (rp) printf("PKG %d %s\r\n", i, rp->name);
            }
            return;
        }
        const char *want = rest;
        while (*want == ' ') want++;
        int idx = -1;
        for (int i = 0; i < n; i++) {
            const am_rpkg_t *rp = am_rstore_get(i);
            if (rp && strcmp(rp->name, want) == 0) { idx = i; break; }
        }
        if (idx < 0) { printf("STORE_ERR 找不到包 '%s' (共 %d 个)\r\n", want, n); return; }
        bool ok = am_rstore_dl_start(idx);
        printf("STORE_DL %s idx=%d -> %s\r\n", want, idx, ok ? "已启动" : "启动失败");
        if (ok) {
            for (int t = 0; t < 60; t++) {
                vTaskDelay(pdMS_TO_TICKS(500));
                int pm = am_rstore_dl_permille();
                const char *dn = am_rstore_dl_name();
                printf("STORE_PROGRESS %d %d%%\r\n", t, pm < 0 ? -1 : pm / 10);
                if (!dn || !dn[0]) {
                    /* ★ 下载完成 → 自动安装(与 UI 路径同一个函数) */
                    extern bool am_store_auto_install(const char *fname);
                    char fn[56];
                    snprintf(fn, sizeof(fn), "%s.lua", want);
                    bool inst = am_store_auto_install(fn);
                    printf("STORE_INSTALL %s\r\n", inst ? "OK" : "FAIL");
                    printf("STORE_DONE\r\n");
                    break;
                }
            }
        }
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
            else { (void)os_hw_request(OS_HW_WIFI);   /* 占住引用计数, 否则连上后没人持有会被拆掉 */
                   printf("WIFI conn %s ok=%d\r\n", ssid, (int)wifi_manager_connect(ssid, pass)); }
        } else if (wa && strcmp(wa, "set") == 0) {
            /* wifi set <ssid> <pass>  (2026-10-09)
             * 用途: 在掌机上用软键盘打 WiFi 密码很痛苦; 烧过整片镜像清了 nvs 之后
             * 更是必须重配。直接从串口写凭据 + 连接, 配完即用。
             * ⚠ 密码会进 NVS 也会出现在串口日志里 —— 仅调试用, 别在公开环境跑。 */
            char *ss = rc_next(&rest), *ps = rc_next(&rest);
            if (!ss || !ps) {
                printf("RC_ERR wifi set <ssid> <pass>\r\n");
            } else {
                printf("WIFI set ssid='%s' 连接中...\r\n", ss);
                fflush(stdout);
                (void)os_hw_request(OS_HW_WIFI);   /* 占住引用计数, 否则连上后没人持有会被拆掉 */
                bool ok = wifi_manager_connect(ss, ps);
                /* 连接是异步的: 轮询最多 ~20s, 好让串口一次看到结果 */
                for (int i = 0; i < 80 && !wifi_manager_is_connected(); i++)
                    vTaskDelay(pdMS_TO_TICKS(250));
                char ip[20] = "-";
                wifi_manager_get_ip(ip, sizeof(ip));
                printf("WIFI set '%s' start=%d connected=%d ip=%s\r\n",
                       ss, (int)ok, (int)wifi_manager_is_connected(), ip);
            }
        } else {
            printf("WIFI on ok=%d\r\n", (int)os_hw_request(OS_HW_WIFI));
        }
    } else if (strcmp(cmd, "audio") == 0) {
        /* 音频链路诊断 (2026-10-09): 软件状态 + ES8311 全寄存器回读, 排"没声音"用 */
        audio_player_diag();
    } else if (strcmp(cmd, "vol") == 0) {
        /* vol            → 读当前音量(0-100)
         * vol <0..100>   → 现场设音量 (用来分辨"音量=0"还是"通路没开") */
        char *vs = rc_next(&rest);
        if (!vs) printf("AUDIOVOL %d\r\n", audio_player_get_volume());
        else {
            audio_player_set_volume(atoi(vs));
            printf("AUDIOVOL set %s -> %d\r\n", vs, audio_player_get_volume());
        }
    } else if (strcmp(cmd, "esr") == 0) {
        /* esr <reg> → 回读 ES8311 单个寄存器; reg 收 十进制/0x 十六进制 */
        char *rs = rc_next(&rest);
        if (!rs) { printf("RC_ERR esr <reg>\r\n"); }
        else {
            uint8_t v = 0;
            int reg = (int)strtol(rs, NULL, 0);
            if (es8311_peek((uint8_t)reg, &v) == 0) printf("ESREG %02X=%02X\r\n", reg & 0xFF, v);
            else                                   printf("RC_ERR esr i2c %02X\r\n", reg & 0xFF);
        }
    } else if (strcmp(cmd, "esw") == 0) {
        /* esw <reg> <val> → 写 ES8311 寄存器 (现场试听定位用) */
        char *rs = rc_next(&rest), *vs = rc_next(&rest);
        if (!rs || !vs) { printf("RC_ERR esw <reg> <val>\r\n"); }
        else {
            int reg = (int)strtol(rs, NULL, 0), val = (int)strtol(vs, NULL, 0);
            int rc = es8311_poke((uint8_t)reg, (uint8_t)val);
            printf("ESW %02X=%02X -> %s\r\n", reg & 0xFF, val & 0xFF, rc == 0 ? "ok" : "fail");
        }
    } else if (strcmp(cmd, "es7") == 0) {
        /* es7 [reg] → 回读 ES7210 (麦克风 ADC) 寄存器; 不带参数 = 全部 0x00~0x4C */
        char *rs = rc_next(&rest);
        if (!rs) {
            es7210_dump_regs();
        } else {
            uint8_t v = 0;
            int reg = (int)strtol(rs, NULL, 0);
            if (es7210_peek((uint8_t)reg, &v) == 0) printf("ES7210REG %02X=%02X\r\n", reg & 0xFF, v);
            else                                   printf("RC_ERR es7 i2c %02X\r\n", reg & 0xFF);
        }
    } else if (strcmp(cmd, "es7w") == 0) {
        /* es7w <reg> <val> → 写 ES7210 寄存器 (现场试) */
        char *rs = rc_next(&rest), *vs = rc_next(&rest);
        if (!rs || !vs) { printf("RC_ERR es7w <reg> <val>\r\n"); }
        else {
            int reg = (int)strtol(rs, NULL, 0), val = (int)strtol(vs, NULL, 0);
            int rc = es7210_poke((uint8_t)reg, (uint8_t)val);
            printf("ES7210W %02X=%02X -> %s\r\n", reg & 0xFF, val & 0xFF, rc == 0 ? "ok" : "fail");
        }
    } else if (strcmp(cmd, "mic") == 0) {
        /* mic [ms] → **麦克风链路一条龙诊断**: 起麦(16k) → 连续抓样点 → 报峰值/最小/最大/有效值。
         *   一刀切开三种"没数据": ①起麦失败 ②I2S RX 拿到的样点恒 0 (codec→ESP 的数据线没波形)
         *   ③有样点但幅度极低 (增益/偏置问题)。 */
        char *ms = rc_next(&rest);
        rc_mic_probe(ms ? atoi(ms) : 400);
    } else if (strcmp(cmd, "beep") == 0) {        /* beep [freq] [ms] [amp] → 纯音测试 (2026-10-09)
         * 走 tone_player → audio_player_feed_pcm → I2S → codec → 功放 → 喇叭,
         * **完全绕过 MP3 解码与 TF 卡**。用途: 一刀切开"固件问题"与"喇叭/功放硬件问题"。
         * 默认 1000Hz / 300ms / amp=6000(最大), **连响 5 次**(每次间隔 200ms) —— 
         * 音量拉满 + 五连响, 现场绝不会听漏。 */
        char *fs = rc_next(&rest), *ms = rc_next(&rest), *as = rc_next(&rest);
        int f   = fs ? atoi(fs) : 1000;
        int d   = ms ? atoi(ms) : 300;
        int amp = as ? atoi(as) : 6000;
        if (f   < 50   || f   > 8000) f   = 1000;
        if (d   < 50   || d   > 3000) d   = 300;
        if (amp < 1    || amp > 6000) amp = 6000;
        printf("BEEP %dHz %dms amp=%d x5 ready=%d\r\n", f, d, amp, (int)tone_player_ready());
        fflush(stdout);
        for (int i = 0; i < 5; i++) {
            tone_beep_amp(f, d, amp);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        printf("BEEP_END\r\n");
    } else if (strcmp(cmd, "gps") == 0) {
        /* gps → GPS (MAX-M10S) 电源状态 + 引脚 (排查耗电: 待机 46µA vs 跟踪 ~10mA/峰值25mA) */
        const gps_config_t *gc = gps_default_config();
        gps_power_state_t st = gps_power_state();
        const char *sn = (st == GPS_POWER_ACTIVE) ? "ACTIVE(~10mA,peak25mA)"
                       : (st == GPS_POWER_SLEEP)  ? "SLEEP(46uA)" : "UNKNOWN";
        printf("GPS pwr=%s ready=%d tx=%d rx=%d extint=%d baud=%lu\r\n",
               sn, (int)gps_is_ready(), gc ? gc->pin_tx : -1, gc ? gc->pin_rx : -1,
               gc ? gc->pin_extint : -1, (unsigned long)(gc ? gc->baud : 0));
    } else if (strcmp(cmd, "pm") == 0) {
        /* pm → 电源管理诊断 (2026-10-09): DFS 目标频率 + **当前锁表**。
         * 锁表里出现 API 级锁 = CPU 降不了频; 出现 SLEEP 锁 = 进不了浅睡。
         * 这是"待机耗电大"最直接的证据来源。 */
        esp_pm_config_t pcfg;
        memset(&pcfg, 0, sizeof(pcfg));
        if (esp_pm_get_configuration(&pcfg) == ESP_OK) {
            printf("PM max_freq=%dMHz min_freq=%dMHz light_sleep=%d\r\n",
                   pcfg.max_freq_mhz, pcfg.min_freq_mhz, (int)pcfg.light_sleep_enable);
        } else {
            printf("PM not enabled\r\n");
        }
        esp_pm_dump_locks(stdout);
        printf("PM_END\r\n");
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
    } else if (strcmp(cmd, "bat") == 0 || strcmp(cmd, "battery") == 0) {
        /* 电池分压/满量程校准用: raw / 脚上 mV / 系数 / VBAT / 百分比 (日志行前缀 BATDIAG) */
        board_battery_diag();
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
        printf("CMDS: hello echo reboot shutdown loglevel mem blk audit stack uptime tasks ls cat rm cp diag page key tap fbinfo fbdump nes scan app wait bat esr esw es7 es7w mic help\r\n");
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
    /* ★ 2026-10-09: 任何一条 remcon 命令都算"有人在用", 刷新空闲计时。
     * 背景: 原来只有 `key` 命令调 input_mark_activity() ⇒ 远程跑回归时
     * page/nes/app/音频诊断等**不刷新**, 设备按"最后一次物理按键"起算 5 分钟
     * 空闲超时 → 自动关机(深睡), USB 串口直接消失, 把长会话打断(实测踩了两次)。
     * 这里放在入口, 一劳永逸覆盖全部命令(含尚未新增的)。
     * input_mark_activity() 只是两次赋值, 任意任务上下文安全。 */
    input_mark_activity();
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
    if (strcmp(cmd, "amcat") == 0) {
        /* amcat <0引擎|1应用|2运维|3商店> —— 调试: 切应用管家分类(验证渲染不死机) */
        extern void am_debug_set_cat(int c);
        int c = rest ? atoi(rest) : 3;
        am_debug_set_cat(c);
        printf("AMCAT %d\r\n", c);
        return;
    }
    if (strcmp(cmd, "hello") == 0 || strcmp(cmd, "echo") == 0 ||
        strcmp(cmd, "reboot") == 0 || strcmp(cmd, "reset") == 0 ||
        strcmp(cmd, "storedl") == 0 ||   /* ★ 商店下载测试 (2026-10-18) */
        strcmp(cmd, "loglevel") == 0 || strcmp(cmd, "heap") == 0 ||
        strcmp(cmd, "mem") == 0 || strcmp(cmd, "blk") == 0 ||
        strcmp(cmd, "audit") == 0 || strcmp(cmd, "stack") == 0 ||
        strcmp(cmd, "uptime") == 0 || strcmp(cmd, "tasks") == 0 ||
        strcmp(cmd, "bat") == 0 || strcmp(cmd, "battery") == 0 ||
        strcmp(cmd, "ls") == 0 || strcmp(cmd, "wifi") == 0 ||
        strcmp(cmd, "cat") == 0 || strcmp(cmd, "rm") == 0 ||
        strcmp(cmd, "cp") == 0 || strcmp(cmd, "diag") == 0 ||
        strcmp(cmd, "help") == 0 || strcmp(cmd, "shutdown") == 0 ||
        strcmp(cmd, "fbinfo") == 0 || strcmp(cmd, "fbdump") == 0 ||
        strcmp(cmd, "audio") == 0 || strcmp(cmd, "vol") == 0 ||
        strcmp(cmd, "esr") == 0 || strcmp(cmd, "esw") == 0 ||
        strcmp(cmd, "es7") == 0 || strcmp(cmd, "es7w") == 0 || strcmp(cmd, "mic") == 0 ||
        strcmp(cmd, "beep") == 0 ||
        strcmp(cmd, "gps") == 0 || strcmp(cmd, "pm") == 0 ||
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
    } else if (strcmp(cmd, "sdrstat") == 0) {
        extern void sdr_test_stat(void);
        sdr_test_stat();
    } else if (strcmp(cmd, "sdrpan") == 0) {
        /* 测试钩子: 注入"上方拖动调频"(谱线跟手平移) */
        char *v = rc_next(&rest);
        extern void sdr_test_pan(int dx);
        sdr_test_pan(v ? atoi(v) : 100);
        printf("SDRPAN %d\r\n", v ? atoi(v) : 100);
    } else if (strcmp(cmd, "sdrslide") == 0) {
        /* 测试钩子: 注入"下方拖动翻页"(整页预渲染+合成); 0 = 结束并恢复实时 */
        char *v = rc_next(&rest);
        extern void sdr_test_slide(int dx);
        sdr_test_slide(v ? atoi(v) : -80);
        printf("SDRSLIDE %d\r\n", v ? atoi(v) : -80);
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
    if (!s_q) { ESP_LOGD(RC_TAG, "queue create failed"); return; }
    BaseType_t ok = xTaskCreatePinnedToCore(rc_task, "remcon", 5120, NULL, 2, NULL, 1);
    ESP_LOGI(RC_TAG, "remote console up, task=%s", ok == pdPASS ? "ok" : "fail");
}