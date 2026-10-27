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
#include <sys/stat.h>
#include <errno.h>
#include "sd_scan.h"   /* sd_is_mounted: logsave 前判断卡是否挂着 */
#include "bt_manager.h"     /* ★ 2026-10-27: bt 命令诊断用 */
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
#include "hw_cfg.h"
#include "gps.h"        /* remcon gps: 电源/引脚 */
#include "gps_nmea.h"   /* remcon gps: 定位快照 + 链路统计 */
#include "svc_nvs.h"
#include "wallpapers.h"   /* remcon wpss: 现场改息屏时长 */
#include "espnow_link.h"   /* remcon `now` 命令: ESP-NOW 现场诊断 */
#include "nes_emu.h"
#include "st7305.h"      /* 抓屏: 帧缓冲布局 + 面板尺寸 */
#include "os_hw.h"       /* wifi 命令: 走资源管家开/关射频 (与页面同一记账) */
#include "driver/gpio.h"
#include "driver/i2c_master.h"

void rc_touch_scan(void);   /* 定义在文件末尾: 触摸 I2C 总线扫描 */
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
    {"pal", OS_PAGE_PAL},   /* 仙剑: 抓屏/回归要用 (之前只能从桌面点图标) */
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
    {"fav", OS_PAGE_FAV},
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
extern bool board_battery_calibrate(uint32_t real_mv);   /* ★ 电池分压标定 */

/* ★ 2026-10-27: GPS 诊断/送睡的任务壳 —— 见调用处注释(remcon 栈不够, 会溢出重启) */
static void rc_task_gpsdiag(void *arg)
{
    extern void gps_diag(void);
    (void)arg;
    gps_diag();
    printf("GPSDIAG_DONE\r\n");
    vTaskDelete(NULL);
}
static void rc_task_gpssleep(void *arg)
{
    extern esp_err_t gps_boot_park(void);
    extern bool gps_sleep_verified(void);
    (void)arg;
    esp_err_t r = gps_boot_park();
    printf("GPSSLEEP ret=%s verified=%d\r\n", esp_err_to_name(r), (int)gps_sleep_verified());
    vTaskDelete(NULL);
}

static void rc_cmd_immediate(const char *line){
    char args[RC_LINE_MAX];
    snprintf(args, sizeof(args), "%s", line);
    char *rest = args;
    char cmd[32];
    const char *c = rc_next(&rest);
    if (!c) c = "";
    snprintf(cmd, sizeof(cmd), "%s", c);
    rc_lower(cmd);

    /* ★ 2026-10-22: 触摸总线诊断 (同步执行, 只读)。必须放这里 ——
     *   原来加在 rc_cmd_immediate() 里, 那条路不负责分发普通命令 ⇒ 永远 unknown。 */
    if (strcmp(cmd, "tpscan") == 0) {
        rc_touch_scan();
        printf("TPSCAN done\r\n");
        return;
    }
    if (strcmp(cmd, "tpnaked") == 0) {
        extern void rc_tp_naked(void);
        rc_tp_naked();
        return;
    }

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
        char *lvl = rc_next(&rest); int v = -1;
        if (lvl) {
            /* ★ 支持名字: `loglevel debug` 以前被 atoi 成 0(=NONE) ⇒ 整个日志静默,
             *   排查时看起来像"设备死了"。数字 0..5 仍然可用。 */
            rc_lower(lvl);
            if      (strcmp(lvl, "none") == 0 || strcmp(lvl, "off") == 0) v = 0;
            else if (strcmp(lvl, "err")  == 0 || strcmp(lvl, "error") == 0) v = 1;
            else if (strcmp(lvl, "warn") == 0) v = 2;
            else if (strcmp(lvl, "info") == 0) v = 3;
            else if (strcmp(lvl, "debug") == 0) v = 4;
            else if (strcmp(lvl, "verbose") == 0) v = 5;
            else v = atoi(lvl);
        }
        if (v >= 0 && v <= 5) { esp_log_level_set("*", (esp_log_level_t)v); printf("LOGLEVEL %d\r\n", v); }
        else printf("LOGLEVEL ERR (0..5 | none/err/warn/info/debug/verbose)\r\n");
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
    } else if (strcmp(cmd, "pal") == 0) {
        /* ★ 2026-10-24 linit 定: 仙剑引擎**不在任何界面显示**, 只从终端敲 `pal` 启动。
         *   这里给串口留同一条入口 (远程自测用)。数据目录必须是 /sdcard/pal。 */
        extern bool pal_launch(void);
        printf("RC_OK pal (data=/sdcard/pal)\r\n");
        if (!pal_launch()) printf("RC_ERR pal 启动失败: 检查 /sdcard/pal\r\n");
    } else if (strcmp(cmd, "gps") == 0) {
        /* gps → GPS (MAX-M10S) 电源状态 + 引脚 (排查耗电: 待机 46µA vs 跟踪 ~10mA/峰值25mA) */
        const gps_config_t *gc = gps_default_config();
        gps_power_state_t st = gps_power_state();
        const char *sn = (st == GPS_POWER_ACTIVE) ? "ACTIVE(~10mA,peak25mA)"
                       : (st == GPS_POWER_SLEEP)  ? "SLEEP(46uA)" : "UNKNOWN";
        printf("GPS pwr=%s ready=%d tx=%d rx=%d extint=%d baud=%lu\r\n",
               sn, (int)gps_is_ready(), gc ? gc->pin_tx : -1, gc ? gc->pin_rx : -1,
               gc ? gc->pin_extint : -1, (unsigned long)(gc ? gc->baud : 0));
        /* 链路 + 定位快照: "模块通不通 / 有没有定位" 全看这几行 */
        gps_fix_t f;
        if (gps_nmea_get(&f)) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            printf("GPS link: rx=%lu ubx=%lu sentences=%lu badcs=%lu age_rx=%ld ms proto=%s\r\n",
                   (unsigned long)f.rx_bytes, (unsigned long)f.ubx_frames,
                   (unsigned long)f.sentences, (unsigned long)f.bad_checksum,
                   (long)(f.last_rx_ms ? (now - f.last_rx_ms) : -1),
                   f.sentences ? "NMEA" : (f.ubx_frames ? "UBX" : "-"));
            printf("GPS fix: valid=%d demo=%d dim=%d quality=%d sats=%d hdop=%.1f alt=%.0fm\r\n",
                   (int)f.valid, (int)f.demo, (int)f.fix_dim, (int)f.fix_quality,
                   (int)f.sats_used, f.hdop, f.alt_m);
            printf("GPS pos: lat=%.6f lon=%.6f speed=%.1fkm/h course=%.0f age=%ld ms\r\n",
                   f.lat, f.lon, f.speed_mps * 3.6f, f.course_deg,
                   (long)(f.last_fix_ms ? (now - f.last_fix_ms) : -1));
            printf("GPS time: %04d-%02d-%02d %02d:%02d:%02d UTC\r\n",
                   (int)f.year, (int)f.month, (int)f.day, (int)f.hour, (int)f.min, (int)f.sec);
        } else {
            printf("GPS fix: (无快照, 服务未启动或没收到过句子)\r\n");
        }
        {
            gps_sat_t sl[40];
            int nv = gps_nmea_sats(sl, 40);          /* GSV 里的可见星 (与定位无关) */
            static const char *SN[7] = { "GP", "GL", "GA", "GB", "GQ", "GI", "GN" };
            printf("GPS sats: visible=%d", nv);
            for (int i = 0; i < nv && i < 8; i++)
                printf(" [%s%02d el%02d az%03d snr%02d%s]",
                       SN[sl[i].sys < 7 ? sl[i].sys : 6], (int)sl[i].prn,
                       (int)sl[i].elev, (int)sl[i].azim, (int)sl[i].snr,
                       sl[i].used ? "*" : "");
            printf("\r\n");
        }
        printf("GPS_END\r\n");
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
        /* ★ 2026-10-18 linit: 同时报**醒着/睡眠累计时长** —— 判断"到底睡没睡"的直接证据
         *   (linit: 500mAh 12 小时耗光 ⇒ 需要看真实睡眠占比) */
        {
            extern void power_stat_get(void *out);
            extern void power_stat_diag(void);
            power_stat_diag();      /* 有则打印, 无则忽略 */
        }
    } else if (strcmp(cmd, "gpsrecheck") == 0) {
        char *av = rc_next(&rest);
        int sec = av ? atoi(av) : 0;
        if (sec <= 0) {
            printf("GPSRECHECK 用法: gpsrecheck <秒>  (默认 600)\r\n");
        } else {
            extern void board_gps_set_recheck_sec(int);
            board_gps_set_recheck_sec(sec);
            printf("GPSRECHECK 已设为 %d 秒\r\n", sec);
        }
    } else if (strcmp(cmd, "gpssleep") == 0 || strcmp(cmd, "gpsrecheck") == 0 || strcmp(cmd, "gpsdiag") == 0 ||
               strcmp(cmd, "gpsprobe") == 0) {
        /* ★★★ 2026-10-27 linit: **必须扔到独立任务里跑**。
         *   实测: 直接在 remcon 任务里调用 → "A stack overflow in task remcon has been
         *   detected" → 板子重启。remcon 栈只有几 KB, 而诊断要 160B 缓冲 + 一大堆日志格式化,
         *   装 UART/autobaud 还会再吃栈。这也是之前 gpsprobe "一按就重启"的真因(不是 UART 抢用)。 */
        extern void gps_diag(void);
        bool want_diag = (strcmp(cmd, "gpssleep") != 0);
        printf("GPS%s 已启动, 请等 15 秒...\r\n", want_diag ? "DIAG" : "SLEEP");
        BaseType_t ok = xTaskCreate(want_diag ? rc_task_gpsdiag : rc_task_gpssleep,
                                    want_diag ? "gpsdiag" : "gpssleep",
                                    6144, NULL, 3, NULL);
        if (ok != pdPASS) printf("GPSERR 任务创建失败(内存不足)\r\n");
    } else if (strcmp(cmd, "gpsprobe_old") == 0) {
        /* 逐档波特率试听 300ms, 判定 GPS 接线/供电是否正常 (只读, 不发任何命令) */
        extern void gps_probe_bauds(void);
        gps_probe_bauds();
        printf("GPSPROBE_END\r\n");
    } else if (strcmp(cmd, "bt") == 0) {
        /* ★ 2026-10-27 linit「蓝牙显示开且关不掉」诊断用。
         *   bt            打印状态 (enabled/ready/connected/scanning)
         *   bt on / off   等价于点设置页的开关 (走同一组 bt_manager_* API) */
        char *av = rc_next(&rest);
        if (!av || !*av) {
            printf("BT enabled=%d ready=%d connected=%d scanning=%d\r\n",
                   (int)bt_manager_is_enabled(), (int)bt_manager_is_ready(),
                   (int)bt_manager_is_connected(), (int)bt_manager_is_scanning());
        } else if (strcmp(av, "on") == 0) {
            bt_manager_enable();
            printf("BT on -> enabled=%d ready=%d\r\n",
                   (int)bt_manager_is_enabled(), (int)bt_manager_is_ready());
        } else if (strcmp(av, "off") == 0) {
            bt_manager_disable();
            printf("BT off -> enabled=%d ready=%d\r\n",
                   (int)bt_manager_is_enabled(), (int)bt_manager_is_ready());
        } else {
            printf("BT_ERR 用法: bt | bt on | bt off\r\n");
        }
    } else if (strcmp(cmd, "batcal") == 0) {
        /* ★ 2026-10-27: 用外部仪表(万用表/INA226)的真实 VBAT 标定分压比。
         *   用法: batcal <真实mV>   例: batcal 4038
         *   内部取**同一时刻**脚上 mV 反算 k = 真实/脚上, 写 NVS 后立即生效。
         *   ⚠ 要在电池电压稳定时标 (充电中/大负载时 VBAT 本身在变)。 */
        char *av = rc_next(&rest);
        uint32_t real_mv = av ? (uint32_t)atoi(av) : 0;
        if (real_mv == 0) {
            printf("BATCAL_ERR 用法: batcal <真实mV>  (例 batcal 4038)\r\n");
        } else {
            bool ok = board_battery_calibrate(real_mv);
            printf("BATCAL %s\r\n", ok ? "OK" : "FAIL");
            board_battery_diag();     /* 标完立刻打一遍新读数, 便于核对 */
        }
    } else if (strcmp(cmd, "batstat") == 0 || strcmp(cmd, "standby") == 0) {
        /* ==== 待机(深睡)耗电实测 ====
         * 用法: 拔掉 USB 放置一段时间 → 插上后立刻发 batstat。
         * 打印三个窗口的电压斜率 mV/h (跨深睡段的空白由首末两点自动跨过去算)。
         * 折算: 电池 500mAh / 满空电压差 ~900mV ⇒ 1mA ≈ 1.8mV/h。 */
        static bat_sample_t bw[BOARD_BATTERY_WAVE_MAX];
        const uint32_t win[3] = { 3600u, 6u * 3600u, 24u * 3600u };
        board_battery_status_t bs;
        if (board_battery_read_cached(&bs) == ESP_OK)
            printf("BATSTAT  当前=%u mV %u%%  历史跨度=%lu s\r\n",
                   (unsigned)bs.voltage_mv, (unsigned)bs.percent,
                   (unsigned long)board_battery_hist_span_s());
        else
            printf("BATSTAT  当前电压读取失败  历史跨度=%lu s\r\n",
                   (unsigned long)board_battery_hist_span_s());
        for (int k = 0; k < 3; k++) {
            int n = board_battery_wave(bw, BOARD_BATTERY_WAVE_MAX, win[k]);
            if (n < 2) { printf("BATSTAT %2luh: 数据不足 (%d 点)\r\n", (unsigned long)(win[k]/3600), n); continue; }
            int mv0 = bw[0].mv, mv1 = bw[n-1].mv;
            uint32_t dt = (bw[n-1].t > bw[0].t) ? (bw[n-1].t - bw[0].t) : 1;
            float mvph = (float)(mv1 - mv0) * 3600.0f / (float)dt;
            printf("BATSTAT %2luh: %d→%d mV  跨 %lu s  %+.1f mV/h  ≈ %.1f mA\r\n",
                   (unsigned long)(win[k]/3600), mv0, mv1, (unsigned long)dt,
                   mvph, -mvph / 1.8f);
        }
        {
            extern void power_stat_diag(void);
            power_stat_diag();
        }
    } else if (strcmp(cmd, "rst") == 0) {
        /* rst —— 读"最近几次异常复位原因"(NVS 环形, 只记非 USB 复位)。
         * 收藏夹会开 USB HID ⇒ 串口没了, 崩了看不到 panic; 重启后发这条即可取证。 */
        static const char *nm[] = {"UNKNOWN","POWERON","EXT","SW","PANIC","INT_WDT",
                                   "TASK_WDT","WDT","DEEPSLEEP","BROWNOUT","SDIO","USB",
                                   "JTAG","EFUSE","PWR_GLITCH","CPU_LOCKUP"};
        nvs_handle_t h;
        uint8_t hist[8] = {0}; size_t n = sizeof(hist);
        if (svc_nvs_open("pst", NVS_READONLY, &h) == ESP_OK) {
            svc_nvs_get_blob(h, "rst", hist, &n);
            svc_nvs_close(h);
        }
        printf("RST_HIST idx=%u:", (unsigned)hist[0]);
        for (int i = 0; i < 7; i++) {   /* hist[0]=写指针, 记录只有 hist[1..7] 七个 ⇒ 原 i<8 会读 hist[8] 越界 */
            if (!hist[1+i]) continue;
            int v = hist[1+i];
            printf(" %s", (v >= 0 && v < 16) ? nm[v] : "?");
        }
        printf("\r\n");
    } else if (strcmp(cmd, "wpss") == 0) {
        /* wpss <息屏分钟> [休眠分钟] —— 现场改屏保/自动关机时长(只影响本次运行, 不写 NVS)。
         * 排查"放一会儿就重启/息屏"时用: 设 1 分钟即可在几十秒内复现。 */
        char *a1 = rc_next(&rest);
        char *a2 = rc_next(&rest);
        int m1 = a1 ? atoi(a1) : -1;
        if (m1 < 0) { printf("WPSS_ERR 用法: wpss <息屏分钟> [休眠分钟]\r\n"); }
        else {
            wp_screensaver_set_delay_ms((uint32_t)m1 * 60000u);
            if (a2) wp_screensaver_set_run_ms((uint32_t)atoi(a2) * 60000u);
            printf("WPSS 息屏=%d 分 休眠=%s 分\r\n", m1, a2 ? a2 : "不变");
        }
    } else if (strcmp(cmd, "now") == 0 || strcmp(cmd, "espnow") == 0) {
        /* `now` 系列 —— ESP-NOW 现场诊断 (双机联机前先在这验证通路):
         *   now start | stop | info
         *   now send <mac> <text>    点对点 (先 start; 会自动 add_peer)
         *   now bcast <text>         广播
         *   now recv                 取一条 (打印 mac + 内容), 没有则报空
         *   now stats                收发计数 */
        char *sub = rc_next(&rest);
        if (!sub) { printf("NOW_ERR 用法: now start|stop|info|send|bcast|recv|stats\r\n"); }
        else if (strcmp(sub, "start") == 0) {
            printf("NOW_START %d\r\n", (int)espnow_link_start());
        } else if (strcmp(sub, "stop") == 0) {
            espnow_link_stop(); printf("NOW_STOP\r\n");
        } else if (strcmp(sub, "info") == 0 || strcmp(sub, "mac") == 0) {
            uint8_t m[6] = {0};
            espnow_link_self_mac(m);
            printf("NOW mac=%02X:%02X:%02X:%02X:%02X:%02X ch=%u run=%d peers=%d\r\n",
                   m[0],m[1],m[2],m[3],m[4],m[5],
                   (unsigned)espnow_link_channel(), (int)espnow_link_is_running(),
                   espnow_link_peer_count());
        } else if (strcmp(sub, "bcast") == 0) {
            char *txt = rc_next(&rest);
            if (!txt) printf("NOW_ERR bcast 缺内容\r\n");
            else printf("NOW_BCAST %d\r\n", (int)espnow_link_broadcast(txt, (int)strlen(txt)));
        } else if (strcmp(sub, "send") == 0) {
            char *macs = rc_next(&rest);
            char *txt  = rc_next(&rest);
            unsigned v[6];
            if (!macs || !txt ||
                sscanf(macs, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) {
                printf("NOW_ERR 用法: now send aa:bb:cc:dd:ee:ff <text>\r\n");
            } else {
                uint8_t m[6] = { (uint8_t)v[0],(uint8_t)v[1],(uint8_t)v[2],
                                 (uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5] };
                espnow_link_add_peer(m, NULL);
                printf("NOW_SEND %d\r\n", (int)espnow_link_send(m, txt, (int)strlen(txt)));
            }
        } else if (strcmp(sub, "recv") == 0) {
            uint8_t m[6]; static char buf[ESPNOW_MAX_PAYLOAD + 1];
            int n = espnow_link_recv(m, buf, ESPNOW_MAX_PAYLOAD);
            if (n <= 0) printf("NOW_RECV 空\r\n");
            else {
                buf[n] = 0;
                printf("NOW_RECV from=%02X:%02X:%02X:%02X:%02X:%02X len=%d [%s]\r\n",
                       m[0],m[1],m[2],m[3],m[4],m[5], n, buf);
            }
        } else if (strcmp(sub, "stats") == 0) {
            espnow_link_stats_t st; espnow_link_get_stats(&st);
            printf("NOW_STATS rx=%lu drop=%lu tx=%lu fail=%lu rssi=%d pending=%d\r\n",
                   (unsigned long)st.rx_ok, (unsigned long)st.rx_drop,
                   (unsigned long)st.tx_ok, (unsigned long)st.tx_fail,
                   (int)st.last_rssi, espnow_link_pending());
        } else {
            printf("NOW_ERR 未知子命令 %s\r\n", sub);
        }
    } else if (strcmp(cmd, "luasmoke") == 0) {
        /* Lua 冒烟自检 (原开机自动跑, 占 248ms 启动时间 → 2026-10-20 改成手动触发):
         * 挂 SPIFFS + 加载执行 /store/test.lua, 结果只看串口 LUA 行。 */
        extern void lua_smoke_test(void);
        lua_smoke_test();
    } else if (strcmp(cmd, "tasks") == 0) {
        char *buf = (char *)malloc(4096 * 2);
        if (!buf) printf("RC_ERR tasks oom\r\n");
        else {
            vTaskList(buf);
            printf("TASKS\r\n%sTASKS_END\r\n", buf);
            free(buf);
        }
    } else if (strcmp(cmd, "fps") == 0) {
        /* fps [reset] → 累计帧数 + 区间真实帧率 + 上帧"绘制 vs 送屏"拆分
         * 用法: `fps reset` → 操作 → `fps`, 得到这段时间**真正画了多少帧/秒**。 */
        void os_get_render_stats(uint32_t*, uint32_t*, uint32_t*, uint32_t*);
        void os_get_frame_gap(uint32_t*, uint32_t*, uint32_t*);
        void os_reset_frame_gap(void);
        uint32_t n = 0, avg = 0, ld = 0, lf = 0, gapmax = 0, slow = 0, cur = 0;
        os_get_render_stats(&n, &avg, &ld, &lf);
        os_get_frame_gap(&gapmax, &slow, &cur);
        static uint32_t snap_n = 0;
        static int64_t  snap_us = 0;
        char *a = rc_next(&rest);
        if (a && !strcmp(a, "reset")) {
            snap_n = n; snap_us = esp_timer_get_time();
            os_reset_frame_gap();
            printf("FPS reset @frames=%lu (现在开始操作, 完事再发 fps)\r\n", (unsigned long)n);
        } else if (snap_us) {
            int64_t dt = esp_timer_get_time() - snap_us;
            uint32_t dn = n - snap_n;
            printf("FPS 区间: %lu 帧 / %.2fs = %.1f fps\r\n",
                   (unsigned long)dn, (double)dt / 1e6,
                   dt > 0 ? (double)dn * 1e6 / (double)dt : 0.0);
            printf("FPS 累计: frames=%lu avg=%luus (%.1f fps) last: draw=%luus flush=%luus\r\n",
                   (unsigned long)n, (unsigned long)avg,
                   avg ? 1000000.0 / (double)avg : 0.0,
                   (unsigned long)ld, (unsigned long)lf);
            printf("FPS 抖动: 最大间隔=%luus, >30ms 的帧=%lu 次\r\n",
                   (unsigned long)gapmax, (unsigned long)slow);
        } else {
            printf("FPS frames=%lu avg=%luus (%.1f fps) last: draw=%luus flush=%luus\r\n",
                   (unsigned long)n, (unsigned long)avg,
                   avg ? 1000000.0 / (double)avg : 0.0,
                   (unsigned long)ld, (unsigned long)lf);
        }
    } else if (strcmp(cmd, "pin") == 0) {
        /* pin <n> [up|down|none] → 把某个 GPIO 配成输入(指定上下拉)并读电平。
         * 专门用来判断"这个脚到底能不能当输入用"(浮空+上拉应读到 1)。 */
        char *a = rc_next(&rest); char *b = rc_next(&rest);
        if (!a) printf("RC_ERR pin <n> [up|down|none]\r\n");
        else {
            int n = (int)strtol(a, NULL, 0);
            gpio_config_t io = {
                .pin_bit_mask = (1ULL << n),
                .mode         = GPIO_MODE_INPUT,
                .pull_up_en   = (b && !strcmp(b, "down")) ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
                .pull_down_en = (b && !strcmp(b, "down")) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            esp_err_t e = gpio_config(&io);
            vTaskDelay(pdMS_TO_TICKS(5));
            printf("PIN %d cfg=%s level=%d\r\n", n, esp_err_to_name(e),
                   e == ESP_OK ? gpio_get_level((gpio_num_t)n) : -1);
        }
    } else if (strcmp(cmd, "partial") == 0) {
        /* partial [0|1] → 局刷开关(默认 0=整帧)。开之前先确认不会出现"某几行横移"。 */
        char *a = rc_next(&rest);
        if (!a || !*a) printf("PARTIAL %d\r\n", st7305_get_partial());
        else { st7305_set_partial((int)strtol(a, NULL, 0) != 0);
               printf("PARTIAL %d -> ok\r\n", st7305_get_partial()); }
    } else if (strcmp(cmd, "chrg") == 0) {
        /* chrg → 读充电检测脚 (板型表 .chrg)。低电平 = 正在充电。
         * 没接线时: 内部上拉 ⇒ 一直读到 1(未充电), 不会误报。 */
        bool board_battery_charging(void); int board_battery_chrg_pin(void);
        int pin = board_battery_chrg_pin();
        if (pin < 0) printf("CHRG 本板未接 (pin=-1)\r\n");
        else {
            int lvl = gpio_get_level((gpio_num_t)pin);
            printf("CHRG GPIO%d level=%d => %s\r\n", pin, lvl,
                   board_battery_charging() ? "充电中" : "未充电");
        }
    } else if (strcmp(cmd, "pace") == 0) {
        /* pace [ms] → UI 产帧间隔。0=自动(默认): 操作中 1ms / 惯性 8ms / 静止 33ms。
         * 固定值只用于对比: 1=最跟手, 32=省电但明显卡。 */
        void os_ui_set_pace(int); int os_ui_get_pace(void);
        char *a = rc_next(&rest);
        if (!a || !*a) printf("PACE %s (%d ms)\r\n",
                              os_ui_get_pace() ? "固定" : "自动", os_ui_get_pace());
        else { os_ui_set_pace((int)strtol(a, NULL, 0));
               printf("PACE %s -> ok\r\n", os_ui_get_pace() ? "固定" : "自动(0)"); }
    } else if (strcmp(cmd, "logsave") == 0) {
        /* logsave → 把内存里的日志缓冲**一次性**追加写入 /sdcard/data/log/bbk.log。
         * 平时不再自动写卡(省电/护卡/保数据), 只有需要备份或排查时手动触发。 */
        void svc_log_drain(void);
        if (!sd_is_mounted()) printf("RC_ERR logsave: TF 未挂载\r\n");
        else { svc_log_drain(); printf("LOGSAVE ok -> /sdcard/data/log/bbk.log\r\n"); }
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
    } else if (strcmp(cmd, "asset") == 0) {
        /* asset <zip> <target> —— 资源包解包自测 (不联网也能验证 am_asset) */
        extern bool am_asset_install(const char *zip_path, const char *target_dir);
        char *z = rc_next(&rest);
        char *t = rc_next(&rest);
        if (!z || !*z || !t || !*t) printf("RC_ERR asset <zip> <dir>\r\n");
        else printf("ASSET %s -> %s : %s\r\n", z, t,
                    am_asset_install(z, t) ? "OK" : "FAIL");
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
    } else if (strcmp(cmd, "tpscan") == 0) {
        /* tpscan —— 触摸 I2C 总线诊断: 两组候选引脚各扫一遍全地址.
         * 输出: 引脚电平 + 每个应答的地址 + 是否随 RST 变化. */
        extern void rc_touch_scan(void);
        rc_touch_scan();
        printf("TPSCAN done\r\n");
    } else if (strcmp(cmd, "ftaxis") == 0 || strncmp(cmd, "ftaxis ", 7) == 0) {
        /* ftaxis [0..3] —— FT6236 方向参数: bit0 交换XY, bit1 Y取反。落盘, 不重编。
         * 只对 FT6236 生效; CST836U/GT911 完全不受影响。 */
        extern uint8_t touch_panel_get_ft_axis(void);
        extern void    touch_panel_set_ft_axis(uint8_t v);
        const char *a = cmd + 6;
        while (*a == ' ') a++;
        if (*a >= '0' && *a <= '3') {
            touch_panel_set_ft_axis((uint8_t)(*a - '0'));
            printf("FTAXIS 已设为 %c (bit0 交换XY, bit1 Y取反) → 摸一下屏幕试方向\r\n", *a);
        } else {
            uint8_t v = touch_panel_get_ft_axis();
            printf("FTAXIS 当前=%u (bit0 交换XY=%u, bit1 Y取反=%u)\r\n"
                   "  用法: ftaxis 0..3   0=不动 1=换XY 2=Y取反 3=换XY+Y取反\r\n",
                   v, v & 1, (v >> 1) & 1);
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

    /* ★ 2026-10-22: 触摸总线诊断 (只读, 同步)。必须放在 rc_exec 入口 ——
     *   文件里有两段一模一样的 cmd 解析, 前一段在 rc_cmd_immediate(死路)。 */
    if (strcmp(cmd, "tpscan") == 0) {
        rc_touch_scan();
        printf("TPSCAN done\r\n");
        return;
    }

    if (strcmp(cmd, "wait") == 0) {
        char *ms = rc_next(&rest); int m = ms ? atoi(ms) : 0;
        if (m > 0 && m < 120000) vTaskDelay(pdMS_TO_TICKS(m));
        return;
    }
    if (strcmp(cmd, "amgeo") == 0) {
        extern void am_debug_geo(void);
        am_debug_geo();
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
    /* ==== 仙剑画面曲线旋钮 (2026-10-25) ====
     * ⚠ 必须放在 rc_exec 里: 文件前面那段 rc_cmd_immediate() 是**死路**,
     *   命令写在那里等于没写 (之前 palgray/palcon 全都没反应就是这个原因)。 */
    if (strcmp(cmd, "palgam") == 0) {
        extern void pal_port_set_gamma(int x100);
        extern int  pal_port_get_gamma(void);
        char *v = rc_next(&rest);
        if (v) pal_port_set_gamma(atoi(v));
        printf("PALGAM %d\r\n", pal_port_get_gamma());
        return;
    }
    if (strcmp(cmd, "palcon") == 0) {
        extern void pal_port_set_contrast(int x100);
        extern int  pal_port_get_contrast(void);
        char *v = rc_next(&rest);
        if (v) pal_port_set_contrast(atoi(v));
        printf("PALCON %d\r\n", pal_port_get_contrast());
        return;
    }
    if (strcmp(cmd, "palwhite") == 0) {
        extern void pal_port_set_white_point(int pt);
        extern int  pal_port_get_white_point(void);
        char *v = rc_next(&rest);
        if (v) pal_port_set_white_point(atoi(v));
        printf("PALWHITE %d\r\n", pal_port_get_white_point());
        return;
    }
    if (strcmp(cmd, "palbr") == 0) {
        extern void pal_port_set_brightness(int pct);
        extern int  pal_port_get_brightness(void);
        char *v = rc_next(&rest);
        if (v) pal_port_set_brightness(atoi(v));
        printf("PALBR %d\r\n", pal_port_get_brightness());
        return;
    }
    if (strcmp(cmd, "palgray") == 0) {
        extern void pal_port_set_auto_level(bool on);
        extern bool pal_port_get_auto_level(void);
        char *v = rc_next(&rest);
        if (v) pal_port_set_auto_level(atoi(v) != 0);
        printf("PALGRAY %d\r\n", pal_port_get_auto_level() ? 1 : 0);
        return;
    }
    if (strcmp(cmd, "palblack") == 0) {
        extern void pal_port_set_black_point(int pt);
        extern int  pal_port_get_black_point(void);
        char *v = rc_next(&rest);
        if (v) pal_port_set_black_point(atoi(v));
        printf("PALBLACK %d\r\n", pal_port_get_black_point());
        return;
    }
    if (strcmp(cmd, "paldump") == 0) {
        extern void pal_port_dump_frame(void);
        pal_port_dump_frame();
        return;
    }
    if (strcmp(cmd, "palstats") == 0) {
        extern void pal_port_dump_stats(void);
        pal_port_dump_stats();
        return;
    }
    if (strcmp(cmd, "palmus") == 0) {
        /* palmus [0|1] —— 仙剑音乐总开关(默认 0; OPL 软合成在本机跑不动),
         * 下次进游戏生效。无参 = 查询。音效(VOC)不受影响。 */
        extern void pal_music_enable(bool on);
        extern bool pal_music_enabled(void);
        char *v = rc_next(&rest);
        if (!v) { printf("PALMUS %d\r\n", pal_music_enabled() ? 1 : 0); return; }
        pal_music_enable(atoi(v) != 0);
        printf("PALMUS %d (下次进游戏生效)\r\n", pal_music_enabled() ? 1 : 0);
        return;
    }
    if (strcmp(cmd, "palaud") == 0) {
        /* palaud —— 仙剑音频链路状态 (音乐/音效播放器 + 音量 + RIX 探针) */
        extern void pal_audio_diag(void);
        pal_audio_diag();
        return;
    }
    if (strcmp(cmd, "hello") == 0 || strcmp(cmd, "echo") == 0 ||
        strcmp(cmd, "reboot") == 0 || strcmp(cmd, "reset") == 0 ||
        strcmp(cmd, "storedl") == 0 ||   /* ★ 商店下载测试 (2026-10-18) */
        strcmp(cmd, "loglevel") == 0 || strcmp(cmd, "heap") == 0 ||
        strcmp(cmd, "mem") == 0 || strcmp(cmd, "blk") == 0 ||
        strcmp(cmd, "audit") == 0 || strcmp(cmd, "stack") == 0 ||
        strcmp(cmd, "uptime") == 0 || strcmp(cmd, "tasks") == 0 ||
        strcmp(cmd, "bat") == 0 || strcmp(cmd, "battery") == 0 || strcmp(cmd, "batcal") == 0 ||
        strcmp(cmd, "bt") == 0 || strcmp(cmd, "gpsprobe") == 0 || strcmp(cmd, "gpsdiag") == 0 || strcmp(cmd, "gpssleep") == 0 ||
        strcmp(cmd, "ls") == 0 || strcmp(cmd, "asset") == 0 || strcmp(cmd, "wifi") == 0 ||
        strcmp(cmd, "cat") == 0 || strcmp(cmd, "rm") == 0 ||
        strcmp(cmd, "cp") == 0 || strcmp(cmd, "diag") == 0 ||
        strcmp(cmd, "help") == 0 || strcmp(cmd, "shutdown") == 0 ||
        strcmp(cmd, "fbinfo") == 0 || strcmp(cmd, "fbdump") == 0 ||
        strcmp(cmd, "audio") == 0 || strcmp(cmd, "vol") == 0 ||
        strcmp(cmd, "esr") == 0 || strcmp(cmd, "esw") == 0 ||
        strcmp(cmd, "es7") == 0 || strcmp(cmd, "es7w") == 0 || strcmp(cmd, "mic") == 0 ||
        strcmp(cmd, "beep") == 0 ||
        strcmp(cmd, "gps") == 0 || strcmp(cmd, "pal") == 0 || strcmp(cmd, "pm") == 0 ||
        strcmp(cmd, "pwr") == 0 || strcmp(cmd, "poweroff") == 0 ||
        strcmp(cmd, "batstat") == 0 || strcmp(cmd, "standby") == 0 ||
        strcmp(cmd, "luasmoke") == 0 ||
        strcmp(cmd, "wpss") == 0 ||
        strcmp(cmd, "rst") == 0 ||
        strcmp(cmd, "now") == 0 || strcmp(cmd, "espnow") == 0 ||
        strcmp(cmd, "logsave") == 0 || strcmp(cmd, "fps") == 0 ||
        strcmp(cmd, "pace") == 0 || strcmp(cmd, "chrg") == 0 || strcmp(cmd, "pin") == 0 || strcmp(cmd, "partial") == 0 ||
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
        scan_nes_tree("/sdcard/roms/nes");
        printf("SCAN_DONE\r\n");
    } else if (strcmp(cmd, "favweb") == 0) {
        extern void fav_web_debug_start(void);
        fav_web_debug_start();
        printf("FAVWEB start requested\r\n");
    } else if (strcmp(cmd, "favclean") == 0) {
        extern void fav_debug_clean_empty(void);
        fav_debug_clean_empty();
        printf("FAVCLEAN ok\r\n");
    } else if (strcmp(cmd, "kbdshow") == 0) {
        char *a = rc_next(&rest);
        extern void fav_debug_kbd_show(int which);
        fav_debug_kbd_show(a ? atoi(a) : 0);
        printf("KBDSHOW ok\r\n");
    } else if (strcmp(cmd, "favedit") == 0) {
        /* favedit [idx] —— 打开收藏夹编辑页 (抓图/回归用, 跳过 PIN) */
        char *a = rc_next(&rest);
        extern void fav_debug_edit(int idx);
        fav_debug_edit(a ? atoi(a) : 0);
        printf("FAVEDIT ok\r\n");
    } else if (strcmp(cmd, "slptest") == 0) {
        /* slptest [0|1|2] —— 画睡壁纸 + 按指定方式关屏, 看画面能不能留住。
         *   0 = 不发关屏命令(面板继续自刷新) 1 = 只 Sleep In 2 = Display OFF + Sleep In
         * 不带参数 = 沿用当前默认。测试后屏可能停在关屏态, 再发一次 slptest 会先唤醒。 */
        char *a = rc_next(&rest);
        if (a) st7305_set_sleep_mode(atoi(a));
        extern void wp_sleep_wallpaper_draw(st7305_handle_t *lcd);
        st7305_wake_out(s_lcd);
        vTaskDelay(pdMS_TO_TICKS(60));
        wp_sleep_wallpaper_draw(s_lcd);
        st7305_sleep_in(s_lcd);
        printf("SLPTEST mode=%d (0=不关屏 1=只SLPIN 2=28+10)\r\n", st7305_get_sleep_mode());
    } else if (strcmp(cmd, "sleepwp") == 0) {
        /* 预览"休眠壁纸": 不动电源, 只把那一帧画到屏上 (真睡下去就看不到串口了)。
         * 休眠到点(AUTO) 走的就是这个函数; 长按关机(MANUAL) 仍是白屏。 */
        extern void wp_sleep_wallpaper_draw(st7305_handle_t *lcd);
        wp_sleep_wallpaper_draw(s_lcd);
        printf("SLEEPWP drawn\r\n");
    } else if (strcmp(cmd, "board") == 0) {
        /* 串口切板型: board            → 打印当前
         *            board xiaozhi|weixue|std → 手动指定
         *            board auto       → 重新自检(I2C 指纹) */
        char *a = rc_next(&rest);
        if (!a) {
            printf("BOARD id=%d %s det=0x%02X\r\n", hw_cfg_board_id(), hw_cfg_board_name(), hw_cfg_det_mask());
        } else {
            rc_lower(a);
            if      (strcmp(a, "auto")    == 0) hw_cfg_rescan();
            else if (strcmp(a, "xiaozhi") == 0 || strcmp(a, "0") == 0) hw_cfg_set_board(HW_BOARD_XIAOZHI);
            else if (strcmp(a, "weixue")  == 0 || strcmp(a, "1") == 0) hw_cfg_set_board(HW_BOARD_WEIXUE);
            else if (strcmp(a, "std")     == 0 || strcmp(a, "2") == 0) hw_cfg_set_board(HW_BOARD_STD);
            else { printf("RC_ERR board xiaozhi|weixue|std|auto\r\n"); fflush(stdout); return; }
            printf("BOARD id=%d %s det=0x%02X (reboot 后完全生效)\r\n",
                   hw_cfg_board_id(), hw_cfg_board_name(), hw_cfg_det_mask());
        }
    } else if (strcmp(cmd, "tpraw") == 0) {
        extern void touch_panel_debug_dump(void);
        touch_panel_debug_dump();
    } else if (strcmp(cmd, "favexp") == 0) {
        extern int fav_debug_export(void);
        fav_debug_export();
    } else if (strcmp(cmd, "favimp") == 0) {
        extern int fav_debug_import(void);
        int r = fav_debug_import();
        printf("FAVIMP rc=%d (0=ok -1=no file -3=bad format)\r\n", r);
    } else if (strcmp(cmd, "favopen") == 0) {
        extern void fav_web_debug_open(void);
        fav_web_debug_open();
        printf("FAVWEB opened\r\n");
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


/* ===== put: 串口上传文件 =====
 * 主机: put <path> <总字节>\r\n → 设备回 RC_PUT_READY
 *       每 <=4096 字节一块, 设备每块回 RC_PUT_ACK <已收>
 *       收满回 RC_PUT_DONE <字节> <crc32>
 *
 * ⚠ 2026-10-24: 原来直接在 remcon 任务(5KB 栈)里 fwrite 到 SD, 传几 KB 后
 *   **整机静默死掉**(连串口都不回)。SD/FATFS + stdio 的栈开销远超 5KB。
 *   现在: 单开一个 12KB 栈的上传任务, 用无缓冲 POSIX write(), remcon 任务
 *   只阻塞等它结束(不能并发读 stdin, 否则会抢走数据字节)。
 */
#define RC_PUT_CHUNK 4096

typedef struct { char path[128]; long total; } rc_put_arg_t;

static void rc_put_task(void *p)
{
    rc_put_arg_t *a = (rc_put_arg_t *)p;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(RC_PUT_CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (uint8_t *)malloc(RC_PUT_CHUNK);
    if (!buf) { printf("RC_ERR nomem\r\n"); printf("RC_PUT_END\r\n"); free(a); vTaskDelete(NULL); return; }

    /* 父目录逐级建 */
    {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s", a->path);
        for (char *q = tmp + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(tmp, 0777); *q = '/'; }
    }
    int fd = open(a->path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        printf("RC_ERR open %s (%d)\r\n", a->path, errno);
        printf("RC_PUT_END\r\n");
        free(buf); free(a); vTaskDelete(NULL); return;
    }
    printf("RC_PUT_READY %ld\r\n", a->total);
    fflush(stdout);

    long got = 0;
    uint32_t crc = 0xFFFFFFFFu;
    while (got < a->total) {
        size_t want = (size_t)((a->total - got) > RC_PUT_CHUNK ? RC_PUT_CHUNK : (a->total - got));
        size_t have = 0;
        int idle = 0;
        while (have < want) {
            ssize_t r = read(STDIN_FILENO, buf + have, want - have);
            if (r > 0) { have += (size_t)r; idle = 0; }
            else { vTaskDelay(pdMS_TO_TICKS(5)); if (++idle > 1200) break; }   /* 6s */
        }
        if (have == 0) {
            printf("RC_ERR timeout at %ld\r\n", got);
            printf("RC_PUT_END\r\n");
            break;
        }
        if (write(fd, buf, have) != (ssize_t)have) {
            printf("RC_ERR write at %ld\r\n", got);
            printf("RC_PUT_END\r\n");
            break;
        }
        for (size_t i = 0; i < have; i++) {
            crc ^= buf[i];
            for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
        got += (long)have;
        printf("RC_PUT_ACK %ld\r\n", got);
        fflush(stdout);
    }
    close(fd);
    if (got == a->total) printf("RC_PUT_DONE %ld %08x\r\n", got, (unsigned)(crc ^ 0xFFFFFFFFu));
    printf("RC_PUT_END\r\n");
    printf("RC_PUT_STACK %u\r\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    fflush(stdout);
    free(buf); free(a);
    vTaskDelete(NULL);
}

static void rc_put(const char *args)
{
    static rc_put_arg_t a;
    if (sscanf(args, "%127s %ld", a.path, &a.total) != 2 || a.total <= 0) {
        printf("RC_ERR put <path> <bytes>\r\n"); return;
    }
    TaskHandle_t h = NULL;
    if (xTaskCreate(rc_put_task, "rc_put", 12 * 1024, &a, 3, &h) != pdPASS) {
        printf("RC_ERR task\r\n"); return;
    }
    /* 等上传任务收尾; 期间 remcon 任务不得再读 stdin (会抢字节) */
    while (h && eTaskGetState(h) != eDeleted) vTaskDelay(pdMS_TO_TICKS(50));
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
                if (strncmp(p, "put ", 4) == 0) rc_put(p + 4);   /* 二进制上传, 不按行解析 */
                else if (*p) rc_exec(p);
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
    BaseType_t ok = xTaskCreatePinnedToCore(rc_task, "remcon", 4096, NULL, 2, NULL, 1);   /* ★ 20KB→16KB: 实测峻值 12.2KB */
    ESP_LOGI(RC_TAG, "remote console up, task=%s", ok == pdPASS ? "ok" : "fail");
}

/* ===== tpscan: 触摸 I2C 总线诊断 (只读, 不改任何配置) =====
 * 两组候选引脚各扫一遍 0x08~0x77, 带 RST 上电时序. 用于区分
 *   "芯片不在这两根线上 / 接线问题" 还是 "固件没去读". */
void rc_touch_scan(void)
{
    static const struct { const char *n; int sda, scl, intr, rst; } B[] = {
        { "\xe5\xb0\x8f\xe6\x99\xba 5/13/12/6",  5, 13, 12, 6 },   /* 小智 5/13/12/6 */
        { "\xe6\xa0\x87\xe5\x87\x86 15/7/17/2", 15,  7, 17, 2 },   /* 标准 15/7/17/2 */
    };
    /* ⛔ 2026-10-22: **只扫当前板型的触摸组**。
     *   跨板扫会碰对方的 LCD 引脚 (小智 GPIO7=LCD DC / 标准&微雪 GPIO5=LCD DC),
     *   `gpio_reset_pin()` 归还成输入后 LCD 驱动不会重配 ⇒ 当场花屏。
     *   要强扫另一组: `tpscan all` (提示: 大概率花屏, 需重启恢复)。 */
    int only = hw_cfg_board_id();
    int b0   = (only == HW_BOARD_XIAOZHI) ? 0 : 1;   /* 0=小智组 1=标准组 */
    for (int b = b0; b <= b0; b++) {
        printf("TPSCAN[%s]\r\n", B[b].n);

        /* ① 引脚电平: 先关内部上拉(看外部有没有上拉/芯片), 再开上拉 */
        gpio_config_t g = {
            .pin_bit_mask = (1ULL << B[b].sda) | (1ULL << B[b].scl) | (1ULL << B[b].intr),
            .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&g); vTaskDelay(pdMS_TO_TICKS(8));
        printf("  \xe6\xb5\xae\xe7\xa9\xba: SDA=%d SCL=%d INT=%d\r\n",
               gpio_get_level(B[b].sda), gpio_get_level(B[b].scl), gpio_get_level(B[b].intr));
        g.pull_up_en = GPIO_PULLUP_ENABLE; gpio_config(&g); vTaskDelay(pdMS_TO_TICKS(8));
        printf("  \xe4\xb8\x8a\xe6\x8b\x89: SDA=%d SCL=%d INT=%d\r\n",
               gpio_get_level(B[b].sda), gpio_get_level(B[b].scl), gpio_get_level(B[b].intr));

        /* ② RST: 低 12ms -> 高 120ms (释放复位) */
        gpio_config_t r = {
            .pin_bit_mask = (1ULL << B[b].rst), .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&r);
        gpio_set_level(B[b].rst, 0); vTaskDelay(pdMS_TO_TICKS(12));
        gpio_set_level(B[b].rst, 1); vTaskDelay(pdMS_TO_TICKS(120));

        /* ③ 全地址扫描 */
        i2c_master_bus_config_t bc = {
            .i2c_port = -1, .sda_io_num = B[b].sda, .scl_io_num = B[b].scl,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        i2c_master_bus_handle_t bus = NULL;
        if (i2c_new_master_bus(&bc, &bus) != ESP_OK) {
            printf("  I2C \xe6\x80\xbb\xe7\xba\xbf\xe5\x88\x9b\xe5\xbb\xba\xe5\xa4\xb1\xe8\xb4\xa5\r\n");
            continue;
        }
        int n = 0;
        for (int a = 0x08; a < 0x78; a++) {
            if (i2c_master_probe(bus, (uint16_t)a, 15) == ESP_OK) { printf("  ACK 0x%02X\r\n", a); n++; }
        }
        i2c_del_master_bus(bus);
        printf("  \xe5\xba\x94\xe7\xad\x94\xe6\x95\xb0=%d\r\n", n);
    }
}

/* ===== tpnaked: 最原始探测 (不碰 RST/不碰 INT, 只建总线问地址) =====
 * 用途: 和 probe_touch_on 对照。若这里也无 ACK ⇒ 触摸硬件确实没通(固件无辜);
 *       若这里有 ACK 而板级探测没有 ⇒ 探测逻辑有问题。 */
void rc_tp_naked(void)
{
    static const struct { const char *n; int sda, scl; } G[] = {
        { "\xe5\xb0\x8f\xe6\x99\xba 5/13",  5, 13 },
    };
    static const uint8_t CA[] = { 0x15, 0x5D, 0x14, 0x38, 0x70, 0x48 };
    for (int g = 0; g < 1; g++) {
        printf("TPNAKED[%s]\r\n", G[g].n);
        i2c_master_bus_config_t bc = {
            .i2c_port = -1, .sda_io_num = G[g].sda, .scl_io_num = G[g].scl,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        i2c_master_bus_handle_t bus = NULL;
        esp_err_t e = i2c_new_master_bus(&bc, &bus);
        printf("  bus=%s\r\n", esp_err_to_name(e));
        if (e != ESP_OK) continue;
        for (size_t i = 0; i < sizeof(CA); i++) {
            esp_err_t r = i2c_master_probe(bus, CA[i], 50);
            printf("  0x%02X -> %s\r\n", CA[i], r == ESP_OK ? "ACK" : "no");
        }
        i2c_del_master_bus(bus);
    }
    printf("TPNAKED done\r\n");
}
