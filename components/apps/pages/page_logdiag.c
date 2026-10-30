/**
 * page_logdiag.c — 日志诊断 (Ghost-Audit 集成, 2026-10-14 UI 重做)
 *
 * 用途: 售后维修场景 — 设备插目标电脑(Windows)枚举为 HID 键盘 + CDC 串口,
 *       自动注入命令链收集 整机/日志/网络/账号 信息, 供维修人员判断故障。
 *
 * ★ UI 结构 (linit 2026-10-14 定稿, 400×300 1bpp):
 *     卡片网格  →  状态带(2 行)  →  横线(盖子)  →  软键 + 进度条(同一排)  →  谷线
 *   底栏几何与「网络检测」页同套 (page_netdect.c nd_draw_nav): 盖子线 y=273 / 谷线 y=294 /
 *      左右软键贴边距 16px 纯文字无框。
 *   ★ 状态带 (2026-10-30 新增, y=240..272): 常显真实状态, 不再只在出错时冒一句"未连电脑"。
 *       第一行 16px 混排: USB 启动中 / USB 未启动 / USB 已启动·未连电脑 / 已连接·就绪 /
 *                         采集中 n/N / 等待回传 rx N / 诊断完成 / 采集中断
 *       第二行 8px: 采集中显示"步数 已用时 当前命令"; 落过盘则显示已存 KB。
 *     卡片因此固定 108 高 (原来 105/121 两套高度, 起停时整页会跳一下)。
 *   ★ 左软键 = "下一步最有价值的动作"(状态驱动):
 *       空闲·无结果 → 全采   空闲·已采集 → 分析   分析完成 → 回传   采集中 → 日志
 *       文本页(结论/历史内容) → 下页 (末页: 结论页给"回传")
 *     右软键 = 启动 (USB 未就绪) / 开始 / 取消 / 返回。
 *
 * ★ 卡片 6 张: 整机信息 · 故障日志 · 网络配置 · 账号凭据 · 一键全采 · 采集历史
 * ★ 采集结果: 经 CDC 读回, 按**电脑名**累积存到 TF 卡 /sdcard/Logs/<电脑名>.txt (追加分节)。
 *
 * ★★ 2026-10-30 可用性修复 (第二阶段):
 *   ① 本页原来 on_enter/on_exit 都是 NULL —— 从不拉起 usb_hid, 而 ld_start() 又要求
 *      os_hid_is_running() ⇒ 从应用中心直接进来点任何卡片都**静默无反应**(等于整页不可用)。
 *      现在进页面在后台任务里拉起 USB (界面能如实显示"启动中"), 退出时**只关本次自己开的**
 *      那一份 —— 原本就开着的, 不动它。
 *   ② ld_start() 原来只判 os_hid_is_running() 就发 HID —— 但 usb_hid_key_tap() 在主机**未枚举**
 *      时是静默丢弃的 (kb_ready() = s_running && tud_hid_n_ready())。插在充电器上点"开始",
 *      进度条照走、结果全空。现在必须先 os_hid_connected() 才允许发命令。
 *   ③ 采集/回传改为**协作取消** (s_abort): 原来 vTaskDelete() 可能打在 os_hid_key_tap() 的
 *      "按下"与"抬起"之间, 电脑侧会留下一个按住的键 (Shift/Enter 卡住)。
 *   ④ 分析结论按"采集链"分别给结果: 用 diag_core 的 dc_parse_chains() 把回传分节归类,
 *      看每条链的 rc.txt 与**真的回来了几个数据文件**; 缺 rc → "未知", 绝不默认成功。
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "os_hid.h"
#include "diag_core.h"  /* 纯逻辑: RC/事件ID/采集链分节解析 (与主机测试共用同一实现) */
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

/* ================= 0. 运行期状态 (前置: ld_send_str 要用 s_abort) =================
 * ★ USB 归属/状态 (2026-10-30) —— 见文件头 ① */
static volatile bool   s_usb_want = false;   /* 页面仍需要 USB */
static volatile bool   s_usb_busy = false;   /* 后台正在启动 (界面可如实显示) */
static volatile bool   s_usb_err  = false;   /* 上次启动失败 (可重试) */
static volatile bool   s_abort    = false;   /* 协作取消: 采集任务尽快退出 */
static volatile bool   s_dev_lost = false;   /* 采集途中电脑掉线 */
static volatile bool   s_xfer_fail = false;  /* 回传超时 (静默/总时长) */
static volatile bool   s_fail_send = false;  /* 失败发生在回传阶段 (文案用"回传中断") */
static volatile bool   s_send_typing = true; /* 回传命令还在打字 (此间不会有任何数据) */
static volatile bool   s_xfer_done = false;  /* 已收到 "##### END" */
static uint32_t        s_last_rx_ms = 0;     /* 最后一次收到 CDC 数据的时刻 */
static volatile bool   s_sd_err   = false;   /* 落盘失败 (未插卡/写保护) */
static int             s_saved_b  = 0;       /* 本次已落盘字节数 (只用于显示, 不参与判断) */

/* ★ 每条采集链的结果 (dc_parse_chains 填充) */
static dc_chain_t s_ch[DC_CHAIN_N];

/* 每条链**应该**产出哪些文件 —— 清单在 diag_core.c 里 (s_f_sys/s_f_log/s_f_net/s_f_acc),
 * 主机单测会逐条校验, tools/check_win_cmds.py 还会拿它和命令正文做交叉校验。
 * ⛔ 增删采集命令时只改 diag_core.c 那张表, 页面不用动。 */

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
    int i = 0;
    for (; *s; s++, i++) {
        /* ★ 2026-10-30: ① 取消要能立刻生效 (回传链一条 600+ 字符 ≈ 20s);
         *   ② 每 32 字符复检一次 USB 枚举状态 —— usb_hid_key_tap() 在未枚举时是**静默返回** 的,
         *      不查就会"看起来在打字、实际全丢"。 */
        if (s_abort) return;
        if ((i & 31) == 0 && (!os_hid_is_running() || !os_hid_connected())) {
            s_dev_lost = true;
            return;
        }
        uint8_t mod, key;
        ld_char_hid(*s, &mod, &key);
        if (key == 0) continue;
        os_hid_key_tap(mod, key);
        /* ★ 2026-10-30: 去掉这里原来的 8ms —— usb_hid_key_tap() 内部已经是
         *   "按下 25ms + 抬起 25ms", 再叠 8ms 等于每字符 58ms, 一条 500 字符的
         *   PowerShell 要打 29 秒。50ms/字符 已经是保守值 (Windows 键盘处理是 ms 级)。 */
    }
}

static void ld_win_cmd(const char *cmd)
{
    os_hid_key_tap(MOD_LGUI, HIDK_R);
    /* ★ 2026-10-30: 350ms → 700ms。Win+R 的"运行"对话框要等 explorer 把窗口创建出来,
     *   冷启动/机械盘/杀软在跑时 350ms 明显不够 —— 不够的后果不是"慢一点", 而是
     *   **整条命令打到别的窗口里** (比如打到 Word 文档里), 后面全乱。
     *   这里没法回读确认 (HID 是单向的), 只能给足余量。 */
    vTaskDelay(pdMS_TO_TICKS(700));
    ld_send_str(cmd);
    vTaskDelay(pdMS_TO_TICKS(150));
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
 * 命令末尾统一 exit → 关掉那个最小化的黑窗, 不打扰用户。
 *
 * ★★ 2026-10-30 命令层整改 (三条硬规矩, 改命令前先读):
 *   ① **只用 ASCII**: 整条命令只允许 0x20..0x7E —— HID 键盘映射里没有中文,
 *      也没有中文标点; 中文路径一律走 %USERPROFILE% / $env: 展开。
 *   ② **双引号里只用单引号**: 整条命令被 cmd 的 `-c "..."` 包着, 里面再出现双引号
 *      必须写成 \" 才不被 cmd 吃掉。所以 PowerShell 里的字符串统一用 '...',
 *      并且不要出现单引号本身 (用户名带 ' 属病态, 已在报告里登记)。
 *   ③ **中文系统不靠英文标签取数据**: 不再从 systeminfo 的 "Host Name:" / "物理内存总量:"
 *      这类**本地化文本**里抓值, 一律用 $env:COMPUTERNAME / CIM 对象属性。
 *      原始 systeminfo 仍然收, 但只作为**给人看的原文**, 不作为唯一数据来源。
 *   ④ **先 cd 进目录, 之后一律用相对文件名** (2026-10-30 第二轮):
 *      ★ 命令里不再出现任何用户路径 ⇒ 用户名带**空格/中文/单引号**都不会破坏命令语法
 *        (原来把 %USERPROFILE% 拼进 PowerShell 的单引号字符串, 路径里有一个 ' 就整条语法错)。
 *      ★ 每条命令省掉 ~40 字符的绝对路径, 四链合计少打 1000 多个字符 (≈50 秒)。
 *      cd 失败只可能是"目录没建起来", 那样文件会落到别处 → 回传时找不到 → 判"无数据",
 *      不会假装成功。
 *   ⑤ 编码 (2026-10-30 第三轮定稿, 两种来源两种解, 不混为一谈):
 *      · **PowerShell 写的结构化文件**: `Out-File -Encoding utf8` → PS5.1 带 BOM 的 UTF-8。
 *      · **cmd `>` 重定向 / 原生控制台程序** (systeminfo/ipconfig/netstat/wevtutil/reg/
 *        driverquery/dir/type): 按 **OEM 代码页** (GetOEMCP) 落盘 —— 不是 ANSI(ACP)。
 *        简中/日文/韩文下 OEM==ANSI(936/932/949), 西文下不同 (437 vs 1252), 混用会把
 *        带重音的机器名解成花码。
 *      · 回传侧一条规则就能分开: **有 BOM 按 BOM 解 (StreamReader 自动且会吃掉 BOM),
 *        没有 BOM 按 $o=OEMCP 解**; 串口一律 UTF-8 发出; 设备原样存盘。
 *   ⑥ **每条链只有一个 cmd 进程** (2026-10-30 第三轮核实的执行模型, 改链前必须懂):
 *      `ld_run_chain()` 里 i==0 走 `ld_win_cmd()` = Win+R 起**新** cmd;
 *      i>=1 走 `ld_send_line()` = 在当前焦点窗口打字 + 回车 ⇒ **同一个进程**。
 *      所以 `cd /d` 从第 2 条起一直有效, 换链才换进程 (每条链各自 cd 一次)。
 *      ⛔ 别在一链中间再插 `cmd` —— 那会新开一个窗口, cd 与后面的相对文件名全部失效。
 *      每条链的 rc.txt 里额外写 `DIR=%CD%`, 万一 cd 没生效, 这是唯一能查的证据。
 *
 * ⛔ 不要把 wmic 写回来: Win11 24H2 起 wmic.exe 默认不随系统安装 (按需功能),
 *    原来 5 条 wmic 在那种机器上**一条文件都不产出**, 却因为 exit code 是 0 而被判成功。
 *    现在全部换 Get-CimInstance (Win7 SP1+ 起自带, 10/11 都有)。
 * ⛔ Out-File 一律带 `-Encoding utf8`:
 *    不带时 PS5.1 默认写 **UTF-16LE**, 回传按单字节读会变成一堆 NUL;
 *    而写 utf8 时 PS5.1 会带 **BOM**, 正好让回传脚本据此区分"UTF-8 的 PS 文件"
 *    和"ANSI/OEM 的 cmd 重定向文件", 一种规则解两种来源。
 *    ⚠️ 若哪天改调 pwsh 7: `utf8` 是**不带 BOM** 的, 上面那条 BOM 判据会失效,
 *       必须同时改回传脚本的判定 (或显式用 utf8BOM)。
 */
#define DIRS_CMD "for %i in (Sys Log Net Acc) do @md \"%USERPROFILE%\\Desktop\\Logs\\%i\" 2>nul"

/* 链首: Win+R 打开一个最小化的 cmd。2026-10-30 标题不再伪装成系统更新 (合规要求)。 */
#define CHAIN_HEAD "cmd /k \"mode con: cols=20 lines=1 & color 08 & title LinTOS-LogCollect\""

/* 每条链的收尾: 先取退出码 → 写 rc.txt → 再打一行给人看的显示命令。
 * ⛔ "set RC" 必须紧跟**最后一条写文件的命令**。原来它排在 `ipconfig | findstr` 后面,
 *    而 findstr 在"没有 IPv4 字样"时返回 1 (纯 IPv6、或某些虚拟网卡) ⇒ 一次完整、
 *    成功的采集会被判成"失败"。现在 rc 只反映真正的采集命令。
 * ⛔ 别为了省两行把它写成宏 (曾经是 RC_TAIL(dir)): 宏把字符串切碎再拼接, 静态检查脚本
 *    与 grep 都看不见完整命令, 一条路径写错只能等真机踩。这里宁可重复。 */

static const char *const s_c_sys[] = {
    CHAIN_HEAD,
    DIRS_CMD,
    "cd /d \"%USERPROFILE%\\Desktop\\Logs\\Sys\"",
    /* 电脑名: 取环境变量, 不依赖 systeminfo 的本地化字段名 */
    "powershell -nop -c \"'COMPUTERNAME='+$env:COMPUTERNAME|Out-File -Encoding utf8 name.txt\"",
    /* CPU: Name / 物理核心 / 逻辑处理器 / 主频 */
    "powershell -nop -c \"Get-CimInstance Win32_Processor|Format-List Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed|Out-File -Encoding utf8 CPU.txt\"",
    /* 内存: 逐条列出 (多根就是多段), Capacity 单位是字节, 由设备侧换算 */
    "powershell -nop -c \"Get-CimInstance Win32_PhysicalMemory|Format-List BankLabel,Capacity,Speed,Manufacturer,PartNumber|Out-File -Encoding utf8 RAM.txt\"",
    "powershell -nop -c \"Get-CimInstance Win32_BaseBoard|Format-List Manufacturer,Product,SerialNumber|Out-File -Encoding utf8 Board.txt\"",
    /* 磁盘: 全部分区 (不是只取第一个) */
    "powershell -nop -c \"Get-CimInstance Win32_LogicalDisk|Format-List DeviceID,DriveType,FileSystem,Size,FreeSpace,VolumeName|Out-File -Encoding utf8 Disk.txt\"",
    "powershell -nop -c \"Get-CimInstance Win32_OperatingSystem|Format-List Caption,Version,BuildNumber,OSArchitecture,InstallDate,LastBootUpTime|Out-File -Encoding utf8 OS.txt\"",
    "systeminfo > SystemInfo.txt",
    "powershell -nop -c \"Get-CimInstance Win32_StartupCommand|Format-List Name,Command,Location|Out-File -Encoding utf8 Startup.txt\"",
    "reg query HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall /s | findstr DisplayName > Software.txt",
    "set RC=%errorlevel%",
    "echo RC=%RC% DIR=%CD% > rc.txt",
    "ipconfig | findstr /i \"IPv4\"",              /* 只给人看: 设备侧不解析 */
    "exit",
};
#define SYS_N (int)(sizeof(s_c_sys)/sizeof(s_c_sys[0]))
/* 延时 = 该命令**自己**的余量; 但 HID 打字本身要 50ms/字符, 一条 200 字符的命令
 * 打完就要 10s, 这段时间前一条命令早就跑完了 —— 所以这里不需要"按最坏情况给"。 */
static const uint16_t s_d_sys[] = { 1500, 400, 200, 900, 1200, 1200, 1000, 1200, 1200, 8000, 1800, 3500, 200, 300, 600, 300 };
_Static_assert(sizeof(s_d_sys)/sizeof(s_d_sys[0]) == sizeof(s_c_sys)/sizeof(s_c_sys[0]),
               "延时表与命令表条数不一致: ld_run_chain() 用 delay[i] 逐个取, 少了就越界读");

static const char *const s_c_log[] = {
    CHAIN_HEAD,
    DIRS_CMD,
    "cd /d \"%USERPROFILE%\\Desktop\\Logs\\Log\"",
    "wevtutil qe System /q:*[System[(Level=2)]] /f:txt /c:100 > SystemErrors.txt",
    "wevtutil qe Application /q:*[System[(Level=2)]] /f:txt /c:100 > AppErrors.txt",
    "wevtutil qe System /q:*[System[(Level=1)]] /f:txt /c:50 > Critical.txt",
    "dir C:\\Windows\\Minidump > Minidump.txt 2>nul & dir C:\\Windows\\LiveKernelReports > KernelReports.txt 2>nul",
    "driverquery /v /fo table > Drivers.txt",
    "set RC=%errorlevel%",
    "echo RC=%RC% DIR=%CD% > rc.txt",
    "wevtutil qe System /q:*[System[(Level=2)]] /f:txt /c:3 | findstr /i \"Date Provider\"",   /* 只给人看 */
    "exit",
};
#define LOG_N (int)(sizeof(s_c_log)/sizeof(s_c_log[0]))
static const uint16_t s_d_log[] = { 1500, 400, 200, 5000, 5000, 4000, 1200, 6000, 200, 300, 600, 300 };
_Static_assert(sizeof(s_d_log)/sizeof(s_d_log[0]) == sizeof(s_c_log)/sizeof(s_c_log[0]),
               "延时表与命令表条数不一致: ld_run_chain() 用 delay[i] 逐个取, 少了就越界读");

static const char *const s_c_net[] = {
    CHAIN_HEAD,
    DIRS_CMD,
    "cd /d \"%USERPROFILE%\\Desktop\\Logs\\Net\"",
    "ipconfig /all > IP.txt & ipconfig /displaydns > DNS.txt",
    "arp -a > ARP.txt & route print > Route.txt & netstat -ano > Ports.txt",
    "netsh advfirewall show allprofiles > Firewall.txt & net share > Shares.txt",
    "reg query \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\" | findstr /i \"Proxy\" > Proxy.txt & type C:\\Windows\\System32\\drivers\\etc\\hosts > Hosts.txt",
    "set RC=%errorlevel%",
    "echo RC=%RC% DIR=%CD% > rc.txt",
    "ipconfig | findstr /i \"IPv4 Gateway\"",      /* 只给人看 */
    "exit",
};
#define NET_N (int)(sizeof(s_c_net)/sizeof(s_c_net[0]))
static const uint16_t s_d_net[] = { 1500, 400, 200, 2200, 4000, 3000, 1800, 200, 300, 600, 300 };
_Static_assert(sizeof(s_d_net)/sizeof(s_d_net[0]) == sizeof(s_c_net)/sizeof(s_c_net[0]),
               "延时表与命令表条数不一致: ld_run_chain() 用 delay[i] 逐个取, 少了就越界读");

static const char *const s_c_acc[] = {
    CHAIN_HEAD,
    DIRS_CMD,
    "cd /d \"%USERPROFILE%\\Desktop\\Logs\\Acc\"",
    "net user > Users.txt & net localgroup administrators > Admins.txt",
    /* 2026-10-30 隐私整改: cmdkey /list 默认**不采集** —— 它会把用户名与凭据目标
     * 落盘, 售后诊断并不需要。确需采集时由维修人员另行手动执行, 固件不自动做。
     * 保留该行注释作为能力记录, 不参与自动链。 */
    /* "cmdkey /list > \"%USERPROFILE%\\Desktop\\Logs\\Acc\\Credentials.txt\"", */
    "schtasks /query /fo table > Tasks.txt",
    "reg query \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Terminal Server\" | findstr /i \"fDenyTSConnections\" > RDP.txt & net share > Shares.txt",
    "set RC=%errorlevel%",
    "echo RC=%RC% DIR=%CD% > rc.txt",
    /* 显示行: 中文系统的 net user 里是"上次登录", findstr "Last" 匹配不到 —— 属已知限制,
     * 数据以 Users.txt 全量原文为准 (HID 只能打 ASCII, 没法把中文关键词塞进命令)。 */
    "net user %USERNAME% | findstr /i \"Last\"",
    "exit",
};
#define ACC_N (int)(sizeof(s_c_acc)/sizeof(s_c_acc[0]))
static const uint16_t s_d_acc[] = { 1500, 400, 200, 1200, 3000, 1600, 200, 300, 800, 300 };
_Static_assert(sizeof(s_d_acc)/sizeof(s_d_acc[0]) == sizeof(s_c_acc)/sizeof(s_c_acc[0]),
               "延时表与命令表条数不一致: ld_run_chain() 用 delay[i] 逐个取, 少了就越界读");

/* 回传: 让目标机把 Logs 下所有 txt 经 CDC 串口发回设备 (设备落 TF 卡)
 * 2026-10-30 第三轮定稿 (每条都对应一个"数据到不了/到了看不懂"的实证问题):
 *   ① $d 用 **Join-Path** 拼, 遍历用 **-LiteralPath** —— 路径含空格/中文/单引号/方括号
 *      都不会被当成通配符或提前闭合字符串; 不再用 `$env:USERPROFILE+'\Desktop\Logs'`
 *      这种"变量与字符串相邻"的写法 (看不出拼出什么, 用户名带 ' 直接语法错)。
 *   ② 解码: `ReadAllText($f,$o)` = StreamReader(检出 BOM) —— **有 BOM 按 BOM 解** (PS 的
 *      UTF-8 文件), **没有 BOM 按 $o 解**; $o 取 **OEMCP**(控制台/GetOEMCP), 不是 ANSI。
 *      cmd 重定向 (systeminfo/ipconfig/wevtutil/reg/dir...) 落的就是 OEM 代码页。
 *      取不到就退回 [Text.Encoding]::Default 并在 `##### ENC` 行如实报出两个代码页。
 *   ③ 逐文件 try: 读不了 → 写 `##### SKIP <路径>` 继续下一个 (原来整段 try 包着 foreach,
 *      一个文件抛异常 = 后面全不发、END 也不写), 收尾写 `##### END <成功数> <跳过数>`。
 *      ⛔ END 只证明"脚本把内容写进了串口", **不证明设备已落盘**。
 *   ④ 只写 ESP32 那个口 —— 按 PnP 设备 ID 的 VID_303A (乐鑫) 过滤; 只有"整机仅一个串口"
 *      才退回那一个口, 多口就放弃 (不把日志灌进虚拟串口/蓝牙串口)。 */
static const char *const s_c_send[] = {
    "cmd",
    "powershell -nop -w hidden -c \"$d=Join-Path $env:USERPROFILE 'Desktop\\Logs';"
    "$o=[Text.Encoding]::Default;"
    "try{$o=[Text.Encoding]::GetEncoding([int](gp 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage').OEMCP)}catch{};"
    "$p=@(Get-CimInstance Win32_PnPEntity|?{$_.DeviceID-match'VID_303A'}|%{if($_.Name-match'\\((COM\\d+)\\)'){$matches[1]}});"
    "if(!$p){$q=@((New-Object IO.Ports.SerialPort).GetPortNames());if($q.Count -eq 1){$p=$q}};"
    "foreach($x in $p){try{$s=New-Object IO.Ports.SerialPort($x,115200);"
    "$s.Encoding=[Text.Encoding]::UTF8;$s.Open();$n=0;$e=0;"
    "$s.WriteLine('##### ENC '+$o.CodePage+' '+[Text.Encoding]::Default.CodePage);"
    "Get-ChildItem -LiteralPath $d -Recurse -Filter *.txt|%{$f=$_.FullName;try{"
    "$t=[IO.File]::ReadAllText($f,$o);"
    "$s.WriteLine('##### '+$f);$s.Write($t);$s.WriteLine('');$n++}"
    "catch{$e++;$s.WriteLine('##### SKIP '+$f)}};"
    "$s.WriteLine('##### END '+$n+' '+$e);$s.Close()}catch{}}\"",
    "exit",
};
#define SEND_N (int)(sizeof(s_c_send)/sizeof(s_c_send[0]))

/* ===== CDC 回传超时 (2026-10-30 第二轮) =====
 * 数据量: 四链全采后 Logs 下约 250-500KB (systeminfo 一份就 20-60KB)。
 * 设备侧 ld_poll 每帧最多读 511B, 有脏帧时 ~125Hz ⇒ 上限 ~60KB/s, 实际 10-30KB/s,
 * 也就是**正常传输 10-50 秒**。
 *   IDLE 20s  : 正常时每帧都有数据, 静默 20s 只可能是"脚本没起来/端口选错/链路断了"。
 *   TOTAL 150s: 最坏 500KB ÷ 10KB/s + 脚本启动 ≈ 60s, 再留 2.5 倍余量。
 * 两个都不能省: 空闲超时抓"没数据", 总超时抓"数据在流但永远流不完"。 */
#define LD_SEND_IDLE_MS   20000u
#define LD_SEND_TOTAL_MS  180000u
/* ⛔ 空闲计时**不能**从本轮开始算: 那条 PS 命令 800+ 字符, 打完要 40 秒,
 *    这 40 秒设备一个字节都收不到 —— 从本轮开始计时必然在第 20 秒误判"空闲超时",
 *    把还在打字的任务取消掉 (2026-10-30 第三轮修复的正是这个)。
 *    现在: s_send_typing 期间只查总超时, 打完命令才开始计空闲 (见 ld_run_chain / ld_poll)。
 * 180s = 打字 40s + 脚本启动 15s + 最坏 500KB ÷ 10KB/s 50s + 余量。 */
/* 这条 PS 约 700 字符 ⇒ 打完要 ~35s; 之后的 15s 给"读文件 + 串口写"收尾。
 * 收尾期间设备侧还有空闲/总超时兜底 (见 ld_poll), 不会无限等。 */
static const uint16_t s_d_send[] = { 500, 15000, 0 };
_Static_assert(sizeof(s_d_send)/sizeof(s_d_send[0]) == sizeof(s_c_send)/sizeof(s_c_send[0]),
               "延时表与命令表条数不一致: ld_run_chain() 用 delay[i] 逐个取, 少了就越界读");

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
      "\xe8\x93\x9d\xe5\xb1\x8f\xc2\xb7\xe9\xa9\xb1\xe5\x8a\xa8\xe9\x94\x99\xe8\xaf\xaf",   /* 蓝屏·驱动错误 */
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
      "\xe7\xba\xa6 4 \xe5\x88\x86\xe9\x92\x9f",                                                              /* 约 4 分钟 (HID 50ms/字符, 实测口径见报告) */
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
/* 本次回传到的原文 (分析用): 24KB, 放 PSRAM 不占内部 RAM。
 * ★ 2026-10-30: 8KB → 24KB。SystemInfo.txt 一份就常常 20-60KB, 8KB 时"一键全采"的
 *   回传从第一个文件就被截断, 后面的链根本没进缓冲 —— 结论只能基于半份样本。 */
static EXT_RAM_BSS_ATTR char s_rx[24576];
static int  s_rx_len = 0;
/* ★ 2026-10-30: 接收缓冲达上限时置位。原实现静默丢弃超出部分却仍当"采集完整"用,
 *   会基于残缺数据下结论。现在这个标记会一路传到分析结论与落盘记录。 */
static bool s_rx_trunc = false;
static TaskHandle_t s_task = NULL;

/* 历史浏览 */
static int  s_view = 0;             /* 0=主网格 1=历史列表 2=历史内容 3=分析结论 */
static char s_hist[24][40];
static int  s_hist_n = 0;
static int  s_hist_sel = 0;
static char s_text[15][44];
static int  s_text_n = 0;
static int  s_text_pg = 0;          /* 结论/历史的翻页序号 (0 起) */
static bool s_text_more = false;    /* 后面还有内容 (历史文件按需读, 事先不知总行数) */
static bool s_text_win  = false;    /* true = s_text 里只装了**当前页** (历史按需读) */
static char s_hist_path[96] = "";   /* 正在看的历史文件 */
static int  s_hist_ln = 0;          /* 当前页起始行号 (0 起) */

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

/* 卡片几何
 * ★ 2026-10-30: 原来是"采集时 105 / 空闲 121"两套高度 —— 一按开始整页卡片会跳一下,
 *   而且空闲态第二排卡片一直画到 y=262, 会把底部提示文字压在卡片上。
 *   现在固定 108: 两排卡片 10..117 / 132..239, 下方 240..272 是**固定状态带**
 *   (状态行 242 + 明细行 260), 与底栏 273 不重叠, 起停时布局不再跳动。 */
#define CARD_X(i) (10 + (i) * 128)
static int card_h(void) { return 108; }
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

/* 把回传到的文本追加进 /sdcard/Logs/<电脑名>.txt
 * ★ 2026-10-30: 原来是 void + 失败直接 return —— 没插卡时采集结果**静默丢失**,
 *   用户还以为存下来了。现在记 s_sd_err, 分析结论里明确写"TF 卡不可用"。 */
static bool ld_hist_append(const char *txt, int len)
{
    char path[96];
    snprintf(path, sizeof(path), LOGDIR "/%s.txt", s_pcname);
    FILE *f = fopen(path, "a");
    if (!f) { s_sd_err = true; return false; }
    size_t w = fwrite(txt, 1, (size_t)len, f);
    fclose(f);
    if ((int)w != len) { s_sd_err = true; return false; }
    s_saved_b += (int)w;
    return true;
}

static void ld_hist_write_head(void)
{
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr),
                     "\r\n================ 采集 %s ================\r\n",
                     s_pcname);
    ld_hist_append(hdr, n);
}

/* ================= 6.5 USB 生命周期 (本页临时占用) =================
 * 为什么由页面负责: 本页全部能力 (HID 打字 + CDC 回传) 都在 usb_hid 里, 内核不会替页面拉起;
 *   原来 on_enter=NULL ⇒ 从应用中心进来点卡片全部静默返回, 整页形同不存在。
 * 所有权: 只关"本次由我们打开的"。
 *   · 进页面时 USB 已经开着 (别人/上一次没退) → 记为"不是我们的", 退出时不动它;
 *   · 原本关着 → os_hid_start() 拉起, 记 s_usb_owned=true, 退出时 os_hid_stop() 还回去。
 *   这样既不会把别的模块正在用的 USB 关掉, 也不会因为管家状态残留而永远起不来。
 *   (os_hid_start/stop 内部就是 os_hw 的引用计数, 成对调用不会泄漏计数。)
 * 为什么放后台任务: usb_composite_enter_hid() 要几百 ms, 同步跑在 on_enter 里界面会先卡住、
 *   等回来时结果已经定了, "正在启动"这个状态**根本没法如实画在屏上**。
 *   放后台 → 屏上看到的 "USB 启动中" 是真实进行中的状态, 也不拖慢进入页面。 */
static TaskHandle_t   s_usb_task  = NULL;
static volatile bool  s_usb_owned = false;   /* 本次由我们拉起 (退出时才关) */

static void ld_usb_task(void *arg)
{
    (void)arg;
    if (!s_usb_want) { s_usb_busy = false; vTaskDelete(NULL); return; }

    if (os_hid_is_running()) {                  /* 已经开着: 不重复初始化, 也不据为己有 */
        s_usb_owned = false;
        s_usb_err   = false;
    } else if (os_hid_start() == ESP_OK) {
        s_usb_owned = true;
        s_usb_err   = false;
    } else {
        s_usb_owned = false;
        s_usb_err   = true;                     /* 失败态留给界面如实显示 + 允许重试 */
    }
    s_usb_busy = false;
    bool ok = os_hid_is_running();
    if (ok) ESP_LOGI(TAG, "USB 键鼠就绪 (电脑已枚举=%d, 本次由本页拉起=%d)",
                     (int)os_hid_connected(), (int)s_usb_owned);
    else    ESP_LOGW(TAG, "USB 键鼠启动失败 ⇒ 本页无法采集/回传");

    if (!s_usb_want && s_usb_owned) {           /* 启动途中页面已退出: 立刻还回去 */
        os_hid_stop();
        s_usb_owned = false;
        ESP_LOGI(TAG, "页面已退出, USB 随即还回");
    }
    s_usb_task = NULL;
    vTaskDelete(NULL);
}

/* 起一次; 幂等 (已就绪 / 正在起 都直接返回) */
static void ld_usb_kick(void)
{
    s_usb_want = true;
    if (os_hid_is_running() || s_usb_busy) return;
    s_usb_busy = true;
    s_usb_err  = false;
    if (xTaskCreate(ld_usb_task, "ldusb", 4096, NULL, 3, &s_usb_task) != pdPASS) {
        s_usb_task = NULL; s_usb_busy = false; s_usb_err = true;
        ESP_LOGE(TAG, "ldusb 任务创建失败");
    }
}

/* ================= 7. 采集任务 ================= */

/* 分片等待: 回传链单条命令 6s, 期间必须能立刻响应"取消" (原来整段 vTaskDelay 无法打断) */
static bool ld_wait(uint32_t ms)
{
    while (ms > 0 && !s_abort) {
        /* 回传链: 已经收到 END 就不用再把这 15s 等满 (典型 3-5s 就传完了) */
        if (s_xfer_done) break;
        uint32_t step = (ms > 50) ? 50 : ms;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
    return !s_abort;
}

/* ★ 发命令的两层门禁: usb 起着 ≠ 电脑连上了。
 *   usb_hid_key_tap() 内部是 kb_ready() = s_running && tud_hid_n_ready(),
 *   主机未枚举时**静默丢弃** —— 只判 is_running 会打出"进度条走完、结果全空"的假成功。 */
static bool ld_ready(void)
{
    return os_hid_is_running() && os_hid_connected();
}

static void ld_run_chain(const char *const *cmds, int n, const uint16_t *delay)
{
    s_total = n; s_prog = 0;
    for (int i = 0; i < n; i++) {
        if (s_abort || s_task == NULL || s_dev_lost) break;
        if (!ld_ready()) { s_dev_lost = true; break; }      /* 电脑掉线: 不再空打 */
        s_prog = i;
        snprintf(s_lastcmd, sizeof(s_lastcmd), "%s", cmds[i]);
        if (i == 0) ld_win_cmd(cmds[i]);
        else        ld_send_line(cmds[i]);
        /* 回传链: 第 1 条 = 那条 PowerShell 命令, 它**打完**的时刻才是"可能有数据"的起点。
         * 在这里翻 flag 并重置空闲计时, 而不是在开始处 —— 否则 40 秒打字会被当空闲超时。 */
        if (i == 1) {
            if (s_send_typing) {
                s_send_typing = false;
                s_last_rx_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            }
        }
        if (delay[i] > 0 && !ld_wait(delay[i])) break;
    }
    s_prog = n;
}

static void ld_task(void *arg)
{
    (void)arg;
    int start = s_run_all;
    ld_hist_write_head();
    for (int c = start; c < 4; c++) {
        if (s_abort || s_dev_lost) break;
        const ld_card_t *k = &s_cards[c];
        ld_run_chain(k->cmds, k->ncmds, k->delay);
        if (s_abort || s_dev_lost) break;
    }
    s_run = -1; s_run_all = 0;
    /* ★ 这里只表示"本轮跑过采集", 不表示"采到了" —— 采到没采到由 s_rx_len 判断,
     *   分析结论里会明说"无回传数据"。⛔ 不要用 s_rx_len 决定 s_have: 采集阶段数据还在
     *   目标机硬盘上, 要等"回传"才进设备; 若这里判成 false, 左软键会从"分析"变回"全采",
     *   操作员就没路走到"回传"了。 */
    s_have = !s_abort;
    s_analyzed = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

/* 协作取消: 先置标志让任务自己退 (≤300ms), 兜底才强删, 最后清一次按键状态。
 * ★ 原来直接 vTaskDelete(): 若正好删在 os_hid_key_tap() 的"按下"与"抬起"之间,
 *   电脑侧会留一个**按住的键** (Shift/Enter 卡住就是它)。 */
static void ld_cancel(void)
{
    s_abort = true;
    for (int i = 0; i < 12 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(25));
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
        ESP_LOGW(TAG, "采集任务未在 300ms 内退出, 已强制删除");
    }
    s_run = -1; s_run_all = 0;
    os_hid_key_release();                    /* 防止按下未抬起留下卡键 */
}

/* 一次采集/回传开始前的公共复位 (原来没有清 s_rx, 上一轮的数据会混进本轮分析) */
static void ld_begin_round(void)
{
    s_abort = false; s_dev_lost = false; s_xfer_fail = false;
    s_fail_send = false; s_xfer_done = false; s_send_typing = false;
    s_last_rx_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_rx_len = 0; s_rx[0] = 0; s_rx_trunc = false;
    s_rx_lines = 0;
    s_saved_b = 0; s_sd_err = false;
    s_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* 刚切到 HID 模式时主机要重新枚举 (几十~几百 ms), 期间 tud_mounted() 还是 false。
 * 这里给一小段宽限, 免得用户"刚插上就点开始"被一句"电脑未连接"顶回来。 */
static bool ld_wait_connected(int ms)
{
    for (int i = 0; i < ms / 50; i++) {
        if (os_hid_connected()) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return os_hid_connected();
}

static void ld_start(ui_ctx_t *ctx, int card)
{
    if (s_task) return;
    if (s_cards[card].action == 2) return;      /* 历史: 由 touch/action 自己处理 */

    if (!os_hid_is_running()) {                 /* 没起 USB: 明确告知 + 顺手再拉一次 */
        os_dialog_toast(ctx, "USB \xe6\x9c\xaa\xe5\xb0\xb1\xe7\xbb\xaa");  /* USB 未就绪 */
        ld_usb_kick();
        return;
    }
    if (!ld_wait_connected(300)) {               /* 起了 USB 但电脑没枚举 */
        os_dialog_toast(ctx, "\xe7\x94\xb5\xe8\x84\x91\xe6\x9c\xaa\xe8\xbf\x9e\xe6\x8e\xa5");  /* 电脑未连接 */
        return;
    }
    ld_begin_round();
    s_have = false; s_analyzed = false;
    s_run = card;
    s_run_all = (card == CARD_ALL) ? 0 : card;  /* 一键全采: 从第 0 条开始跑满 4 条 */
    if (xTaskCreate(ld_task, "ldi", 4096, NULL, 2, &s_task) != pdPASS) {
        s_task = NULL; s_run = -1;
        os_dialog_toast(ctx, "\xe4\xbb\xbb\xe5\x8a\xa1\xe5\x88\x9b\xe5\xbb\xba\xe5\xa4\xb1\xe8\xb4\xa5");  /* 任务创建失败 */
    }
}

/* ---- 回传: 让目标机把 Logs\*.txt 经 CDC 发回, 设备落 TF 卡 ---- */
#define RUN_SEND 9                  /* s_run 的"回传"标记 (不是卡片下标) */

static void ld_send_task(void *arg)
{
    (void)arg;
    ld_run_chain(s_c_send, SEND_N, s_d_send);
    s_run = -1; s_task = NULL;
    /* 收到新数据 → 上一份结论作废, 左软键回到"分析"
     * (否则 s_analyzed 一直为 true, 左软键永远停在"回传", 出不来) */
    if (s_rx_len > 0) { s_have = true; s_analyzed = false; }
    vTaskDelete(NULL);
}

static void ld_backhaul(ui_ctx_t *ctx)
{
    if (s_task) return;
    if (!os_hid_is_running()) {
        os_dialog_toast(ctx, "USB \xe6\x9c\xaa\xe5\xb0\xb1\xe7\xbb\xaa");
        ld_usb_kick();
        return;
    }
    if (!ld_wait_connected(300)) {
        os_dialog_toast(ctx, "\xe7\x94\xb5\xe8\x84\x91\xe6\x9c\xaa\xe8\xbf\x9e\xe6\x8e\xa5");
        return;
    }
    ld_begin_round();
    s_send_typing = true;            /* 从这一刻到"打完 PS 命令"之间不会有数据 (见 ld_run_chain) */
    s_run = RUN_SEND;
    if (xTaskCreate(ld_send_task, "lds", 4096, NULL, 2, &s_task) != pdPASS) {
        s_task = NULL; s_run = -1;
        os_dialog_toast(ctx, "\xe4\xbb\xbb\xe5\x8a\xa1\xe5\x88\x9b\xe5\xbb\xba\xe5\xa4\xb1\xe8\xb4\xa5");
    }
}

/* ================= 8. 分析 (本地结论) ================= */

#define LD_TEXT_MAX  15                  /* 结论最多 15 行 (9 行/页, 分页浏览) */
#define LD_TEXT_ROWS 9

/* 有界追加: 原来多处 s_text[s_text_n++] 不检查上限, 条目一多就越界 */
static void ld_add(const char *s)
{
    if (s_text_n >= LD_TEXT_MAX) return;
    str_trunc(s_text[s_text_n++], 44, s);
}

/* 单条采集链的结论词 —— 判定在 dc_chain_status() (主机可测), 这里只负责中文 */
static const char *ld_chain_word(const dc_chain_t *c)
{
    switch (dc_chain_status(c)) {
    case DC_CHAIN_OK:      return "\xe6\x88\x90\xe5\x8a\x9f";     /* 成功 */
    case DC_CHAIN_FAIL:    return "\xe5\xa4\xb1\xe8\xb4\xa5";     /* 失败 */
    case DC_CHAIN_EMPTY:   return "\xe6\x97\xa0\xe6\x95\xb0\xe6\x8d\xae";  /* 无数据 */
    case DC_CHAIN_PARTIAL: return "\xe4\xb8\x8d\xe5\x85\xa8";     /* 不全 */
    default:               return "\xe6\x9c\xaa\xe7\x9f\xa5";     /* 未知 */
    }
}

static void ld_analyze(void)
{
    s_text_n = 0;
    s_text_pg = 0;
    s_text_more = false;      /* 结论是整份文本, 分页由总行数算, 不靠增量读 */
    s_text_win  = false;

    /* 采集链分节: 每条链的 rc 码 + **真的回传了几个数据文件** (后者比退出码更硬) */
    int secs = dc_parse_chains(s_rx, s_ch);
    int expect_total = 0, empty_total = 0;
    for (int i = 0; i < DC_CHAIN_N; i++) {                 /* 差几个文件 = 差几项 */
        s_ch[i].expect = dc_chain_expect_len(i);
        expect_total += s_ch[i].expect;
        empty_total  += s_ch[i].empty;
    }
    int ok_n = 0, fail_n = 0;
    dc_parse_rc(s_rx, &ok_n, &fail_n);

    char b[64];
    snprintf(b, sizeof(b), "\xe7\x94\xb5\xe8\x84\x91: %s", s_pcname);              /* 电脑: xxx */
    ld_add(b);

    if (s_rx_len == 0) {
        ld_add("\xe6\x97\xa0\xe5\x9b\x9e\xe4\xbc\xa0\xe6\x95\xb0\xe6\x8d\xae");                          /* 无回传数据 */
        ld_add("\xe8\xaf\xb7\xe5\x85\x88\xe9\x87\x87\xe9\x9b\x86, \xe5\x86\x8d\xe5\x9b\x9e\xe4\xbc\xa0"); /* 请先采集, 再回传 */
        if (s_dev_lost) ld_add("\xe9\x87\x87\xe9\x9b\x86\xe6\x97\xb6\xe7\x94\xb5\xe8\x84\x91\xe6\x96\xad\xe5\xbc\x80"); /* 采集时电脑断开 */
        if (s_sd_err)   ld_add("TF\xe5\x8d\xa1\xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8");                   /* TF卡不可用 */
        s_analyzed = true; s_view = 3; return;
    }
    if (s_rx_trunc) {
        /* 数据被截断: 必须明说, 否则等于拿残缺样本当完整结论 */
        char t[44];
        snprintf(t, sizeof(t), "! \xe6\x95\xb0\xe6\x8d\xae\xe4\xb8\x8d\xe5\xae\x8c\xe6\x95\xb4(\xe8\xb6\x85%dKB)",
                 (int)(sizeof(s_rx) / 1024));            /* 数据不完整(超NKB) */
        ld_add(t);
    }

    /* ★ 传输完成标记: 回传脚本最后写 "##### END <成功数> <跳过数>"。
     *   没有它 = 串口中途断了 / 脚本被杀 → 哪怕收了几个文件也不是完整样本。
     *   跳过数单列: "文件在磁盘上但读不出来" 与 "根本没生成" 是两种故障, 不能混说。 */
    {
        int end_n = 0;
        bool end_ok = dc_parse_end(s_rx, &end_n);
        int skips = dc_count_skips(s_rx);        /* 文件在磁盘上但读不出来的个数 */
        char t[44];
        if (end_ok && skips == 0)
            snprintf(t, sizeof(t), "\xe5\x9b\x9e\xe4\xbc\xa0\xe5\xae\x8c\xe6\x95\xb4: %d \xe4\xb8\xaa\xe6\x96\x87\xe4\xbb\xb6", end_n); /* 回传完整: N 个文件 */
        else if (end_ok)
            snprintf(t, sizeof(t), "\xe5\x9b\x9e\xe4\xbc\xa0 %d \xe4\xb8\xaa, \xe8\xb7\xb3\xe8\xbf\x87 %d", end_n, skips); /* 回传 N 个, 跳过 M */
        else if (skips > 0)
            snprintf(t, sizeof(t), "! \xe5\x9b\x9e\xe4\xbc\xa0\xe6\x9c\xaa\xe5\xae\x8c\xe6\x88\x90, \xe8\xb7\xb3\xe8\xbf\x87 %d", skips); /* ! 回传未完成, 跳过 M */
        else
            snprintf(t, sizeof(t), "! \xe5\x9b\x9e\xe4\xbc\xa0\xe6\x9c\xaa\xe5\xae\x8c\xe6\x88\x90(\xe6\x97\xa0\xe7\xbb\x93\xe6\x9d\x9f\xe6\xa0\x87\xe8\xae\xb0)"); /* ! 回传未完成(无结束标记) */
        ld_add(t);
        if (s_xfer_fail) ld_add("! \xe5\x9b\x9e\xe4\xbc\xa0\xe4\xb8\xad\xe6\x96\xad: \xe8\xb6\x85\xe6\x97\xb6");   /* ! 回传中断: 超时 */
    }

    /* 每条链给结论 (两行四链) */
    snprintf(b, sizeof(b), "%s %s  %s %s",
             "\xe6\x95\xb4\xe6\x9c\xba", ld_chain_word(&s_ch[DC_CHAIN_SYS]),
             "\xe6\x97\xa5\xe5\xbf\x97", ld_chain_word(&s_ch[DC_CHAIN_LOG]));
    ld_add(b);
    snprintf(b, sizeof(b), "%s %s  %s %s",
             "\xe7\xbd\x91\xe7\xbb\x9c", ld_chain_word(&s_ch[DC_CHAIN_NET]),
             "\xe8\xb4\xa6\xe5\x8f\xb7", ld_chain_word(&s_ch[DC_CHAIN_ACC]));
    ld_add(b);

    int files = 0;
    for (int i = 0; i < DC_CHAIN_N; i++) files += s_ch[i].files;
    if (secs == 0) {
        ld_add("? \xe5\x9b\x9e\xe4\xbc\xa0\xe6\xa0\xbc\xe5\xbc\x8f\xe5\xbc\x82\xe5\xb8\xb8");            /* ? 回传格式异常 */
    } else {
        if (empty_total > 0)
            snprintf(b, sizeof(b), "\xe6\x96\x87\xe4\xbb\xb6 %d/%d \xe7\xa9\xba %d",
                     files, expect_total, empty_total);      /* 文件 到/应有 空 N */
        else
            snprintf(b, sizeof(b), "\xe6\x96\x87\xe4\xbb\xb6 %d/%d \xe8\xbf\x94\xe5\x9b\x9e\xe7\xa0\x81 %d/%d",
                     files, expect_total, ok_n, ok_n + fail_n);
        ld_add(b);
    }
    if (s_sd_err) ld_add("TF\xe5\x8d\xa1\xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8");                     /* TF卡不可用 */

    /* ---- 在回传到的原文里找关键词, 统计"异常指纹" ---- */
    struct { const char *zh; const char *en; int hit; const char *adv; } k[] = {
        { "\xe9\x94\x99\xe8\xaf\xaf", "Error",    0, "\xe6\x9f\xa5\xe7\x9c\x8b\xe4\xba\x8b\xe4\xbb\xb6\xe6\x97\xa5\xe5\xbf\x97" }, /* 错误 / 查看事件日志 */
        { "\xe4\xb8\xa5\xe9\x87\x8d", "Critical", 0, "\xe4\xbc\x98\xe5\x85\x88\xe5\xa4\x84\xe7\x90\x86\xe5\xb4\xa9\xe6\xba\x83" }, /* 严重 / 优先处理崩溃 */
        { "\xe8\x93\x9d\xe5\xb1\x8f", "BugCheck", 0, "\xe5\x86\x85\xe5\xad\x98\xe6\x88\x96\xe9\xa9\xb1\xe5\x8a\xa8\xe5\xbc\x82\xe5\xb8\xb8" }, /* 蓝屏 / 内存或驱动异常 */
        { "\xe9\xa9\xb1\xe5\x8a\xa8", "driver",   0, "\xe9\x87\x8d\xe8\xa3\x85\xe9\xa9\xb1\xe5\x8a\xa8" },                 /* 驱动 / 重装驱动 */
        { "\xe7\xa3\x81\xe7\x9b\x98", "disk",     0, "\xe6\xa3\x80\xe6\x9f\xa5\xe7\xa1\xac\xe7\x9b\x98\xe5\x81\xa5\xe5\xba\xb7" },                 /* 磁盘 / 检查硬盘健康 */
        { "\xe6\x8b\x92\xe7\xbb\x9d", "denied",   0, "\xe6\x9d\x83\xe9\x99\x90\xe9\x97\xae\xe9\xa2\x98" },                     /* 拒绝 / 权限问题 */
        { "\xe5\xa4\xb1\xe8\xb4\xa5", "fail",     0, "\xe6\x9f\xa5\xe7\x9c\x8b\xe5\x85\xb7\xe4\xbd\x93\xe6\xad\xa5\xe9\xaa\xa4" },                 /* 失败 / 查看具体步骤 */
    };
    int nk = (int)(sizeof(k) / sizeof(k[0]));
    int total = 0;
    for (int i = 0; i < nk; i++) k[i].hit = 0;

    /* ★ 2026-10-29 修两个缺陷:
     *   ① 原来中文词和英文词各扫一遍全篇, 同一行同时含 "错误"/"Error" 会被**重复计 2 次**,
     *      一行的事实被当成两条证据, 异常数虚高。
     *   ② 原来健康分 = 100 - 2*总命中, 与日志长度无关 —— 1000 行日志必然"很不健康",
     *      10 行日志永远"健康", 缺乏可比性。
     *   现在: 逐行扫描, 同一行内每个关键词最多计 1 次; 健康分改用**异常行密度**。 */
    int lines = dc_count_lines(s_rx);
    if (lines < 1) lines = 1;

    const char *ls = s_rx;
    while (ls && *ls) {
        const char *eol = strchr(ls, '\n');
        size_t len = eol ? (size_t)(eol - ls) : strlen(ls);
        char lbuf[256];
        size_t cp = (len < sizeof(lbuf) - 1) ? len : sizeof(lbuf) - 1;
        memcpy(lbuf, ls, cp); lbuf[cp] = '\0';
        for (int i = 0; i < nk; i++) {
            if (strstr(lbuf, k[i].en) || strstr(lbuf, k[i].zh)) {
                if (k[i].hit < 99) k[i].hit++;
            }
        }
        ls = eol ? eol + 1 : NULL;
    }
    for (int i = 0; i < nk; i++) total += k[i].hit;

    /* 异常密度: 每 100 行里命中的异常行数; 1 个异常行扣 3 分, 最低 30 分 */
    int per100 = (lines > 0) ? (total * 100 / lines) : 0;
    int score = 100 - per100 * 3;
    if (score < 30) score = 30;
    if (score > 100) score = 100;

    /* ★ 事件 ID 频次 (2026-10-30 改用 diag_core 实现): 只认行首 "Event ID:" 字段,
     *   避免正文里出现同样字样被误计。重复出现的同一 ID 比"数量多"更有诊断意义
     *   (例如反复的 6008 意外关机 / 41 内核掉电 / 7 磁盘坏块)。
     *   局限: 只做 ID 频次, 不解析 Provider/时间; 非中文/英文 Windows 的字段名可能本地化。 */
    {
        int ids[24], cnt[24];
        int nid = dc_parse_event_ids(s_rx, ids, cnt, 24);
        for (int rank = 0; rank < 3; rank++) {
            int best = -1;
            for (int i = 0; i < nid; i++)
                if (cnt[i] > 0 && (best < 0 || cnt[i] > cnt[best])) best = i;
            if (best < 0 || cnt[best] < 2) break;   /* 只报重复出现的 ID */
            snprintf(b, sizeof(b), "\xe4\xba\x8b\xe4\xbb\xb6ID %d x%d", ids[best], cnt[best]);  /* 事件ID N xM */
            ld_add(b);
            cnt[best] = -1;
        }
    }

    snprintf(b, sizeof(b), "\xe5\x81\xa5\xe5\xba\xb7\xe5\x88\x86: %d  \xe5\xbc\x82\xe5\xb8\xb8: %d", score, total);   /* 健康分 / 异常 */
    ld_add(b);

    /* 取命中最多的三项给结论 */
    for (int rank = 0; rank < 3; rank++) {
        int best = -1;
        for (int i = 0; i < nk; i++) if (k[i].hit > 0 && (best < 0 || k[i].hit > k[best].hit)) best = i;
        if (best < 0) break;
        snprintf(b, sizeof(b), "%s x%d  %s", k[best].zh, k[best].hit, k[best].adv);
        ld_add(b);
        k[best].hit = 0;                        /* 用掉就不参与下一轮 */
    }
    if (total == 0) ld_add("\xe6\x9c\xaa\xe5\x8f\x91\xe7\x8e\xb0\xe6\x98\x8e\xe6\x98\xbe\xe5\xbc\x82\xe5\xb8\xb8");   /* 未发现明显异常 */
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

/* 是否还有下一页: 分析结论是已知总行数; 历史文件按需读, 用 s_text_more 标记 */
static bool ld_text_more(void)
{
    return s_text_more || ((s_text_pg + 1) * LD_TEXT_ROWS < s_text_n);
}

/* 按需从历史文件读一页 (不再一次只读 9 行就完事 —— 一份日志几十上百行) */
static void ld_hist_load(int from_line)
{
    s_text_n = 0;
    s_text_more = false;
    s_text_win = true;                  /* 窗口模式: s_text 只装这一页 */
    s_hist_ln = (from_line > 0) ? from_line : 0;
    FILE *f = fopen(s_hist_path, "r");
    if (!f) return;
    char line[64];
    int ln = 0, got = 0;
    while (fgets(line, sizeof(line), f)) {
        if (ln++ < s_hist_ln) continue;
        if (got >= LD_TEXT_ROWS) { s_text_more = true; break; }   /* 多读一行即知有没有下页 */
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        str_trunc(s_text[s_text_n++], 44, line);
        got++;
    }
    fclose(f);
}

/* 文本页 (分析结论 / 历史内容): 9 行一页, 多页给"下页"软键。
 * ★ 原来 s_view==2 (历史内容) **没有渲染分支** —— 点"打开"后画面还停在卡片网格,
 *   而且所有卡片命中都被 `if (s_view != 0) return true;` 吞掉, 等于卡死在网格上。
 *   现在两种文本视图都用这一个函数。 */
static void ld_render_text(st7305_handle_t *l, const char *title,
                           const char *left_last, const char *right)
{
    ld_zh(l, 16, 10, title, false);
    draw_hline(l, 10, 389, 34, ST7305_COLOR_BLACK);

    int pages = (s_text_n + LD_TEXT_ROWS - 1) / LD_TEXT_ROWS;
    if (pages < 1) pages = 1;
    bool more = ld_text_more();
    if (!s_text_win) {                                  /* 整份文本: 翻过头就夹回来 */
        if (s_text_pg >= pages) { s_text_pg = pages - 1; more = false; }
        if (s_text_pg < 0) s_text_pg = 0;
    }
    int base = s_text_win ? 0 : s_text_pg * LD_TEXT_ROWS;
    for (int i = 0; i < LD_TEXT_ROWS; i++) {
        int idx = base + i;
        if (idx >= s_text_n) break;
        draw_text(l, 16, 44 + i * 24, s_text[idx], false);
    }
    if (s_text_n == 0) {
        ld_zh_center(l, 130, "\xe6\x9a\x82\xe6\x97\xa0\xe5\x86\x85\xe5\xae\xb9", false);   /* 暂无内容 */
    }
    if (s_text_n > LD_TEXT_ROWS) {                      /* 页角 2/3, 不占正文位置 */
        char pg[24];
        snprintf(pg, sizeof(pg), "%d/%d", s_text_pg + 1, pages);
        ld_asc(l, 392 - (int)strlen(pg) * 8, 12, pg);
    }
    const char *left = more ? "\xe4\xb8\x8b\xe9\xa1\xb5" : left_last;   /* 下页 */
    ld_bar(l, left, right);
}

/* ---- 状态判定: 全部来自真实标志/真实 USB 状态, 没有一处靠延时"演"进度 ----
 * 判定逻辑在 dc_ld_state() (纯函数, 主机可测); 这里只把页面标志喂进去。 */
#define LD_STATUS_Y  242
#define LD_DETAIL_Y  260

static dc_ld_state_t ld_state(void)
{
    dc_ld_ctx_t c = {
        .usb_busy     = s_usb_busy,
        .usb_running  = os_hid_is_running(),
        .pc_connected = os_hid_connected(),      /* 注意: 与"USB 已启动"是两件事 */
        .dev_lost     = s_dev_lost,
        .xfer_fail    = s_xfer_fail,
        .sending      = (s_run == RUN_SEND),
        .collecting   = (s_run >= 0 && s_run != RUN_SEND),
        .analyzed     = s_analyzed,
    };
    return dc_ld_state(&c);
}

static void ld_draw_status(st7305_handle_t *l)
{
    char s[64];
    switch (ld_state()) {
    case DC_LD_BOOT:
        snprintf(s, sizeof(s), "USB \xe5\x90\xaf\xe5\x8a\xa8\xe4\xb8\xad");                       /* USB 启动中 */
        break;
    case DC_LD_USB_OFF:
        snprintf(s, sizeof(s), "%s",
                 s_usb_err ? "USB \xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5"            /* USB 启动失败 */
                           : "USB \xe6\x9c\xaa\xe5\x90\xaf\xe5\x8a\xa8");                          /* USB 未启动 */
        break;
    case DC_LD_NO_PC:
        snprintf(s, sizeof(s), "USB \xe5\xb7\xb2\xe5\x90\xaf\xe5\x8a\xa8/\xe6\x9c\xaa\xe8\xbf\x9e\xe7\x94\xb5\xe8\x84\x91"); /* USB 已启动/未连电脑 */
        break;
    case DC_LD_RUN:
        snprintf(s, sizeof(s), "\xe9\x87\x87\xe9\x9b\x86\xe4\xb8\xad %d/%d", s_prog + 1, s_total);  /* 采集中 n/N */
        break;
    case DC_LD_SEND:
        snprintf(s, sizeof(s), "\xe7\xad\x89\xe5\xbe\x85\xe5\x9b\x9e\xe4\xbc\xa0 rx %d", s_rx_lines); /* 等待回传 rx N */
        break;
    case DC_LD_FAIL:
        if (s_xfer_fail)
            snprintf(s, sizeof(s), "\xe5\x9b\x9e\xe4\xbc\xa0\xe4\xb8\xad\xe6\x96\xad: \xe8\xb6\x85\xe6\x97\xb6"); /* 回传中断: 超时 */
        else if (s_fail_send)
            snprintf(s, sizeof(s), "\xe5\x9b\x9e\xe4\xbc\xa0\xe4\xb8\xad\xe6\x96\xad: \xe7\x94\xb5\xe8\x84\x91\xe6\x96\xad\xe5\xbc\x80"); /* 回传中断: 电脑断开 */
        else
            snprintf(s, sizeof(s), "\xe9\x87\x87\xe9\x9b\x86\xe4\xb8\xad\xe6\x96\xad: \xe7\x94\xb5\xe8\x84\x91\xe6\x96\xad\xe5\xbc\x80"); /* 采集中断: 电脑断开 */
        break;
    case DC_LD_DONE: {
        int fail = 0;
        for (int i = 0; i < DC_CHAIN_N; i++)
            if (dc_chain_status(&s_ch[i]) != DC_CHAIN_OK) fail++;   /* 未知/无数据/不全 都算有问题 */
        if (fail > 0)
            snprintf(s, sizeof(s), "\xe8\xaf\x8a\xe6\x96\xad\xe5\xae\x8c\xe6\x88\x90 %d \xe5\xa4\xb1\xe8\xb4\xa5", fail); /* 诊断完成 N 失败 */
        else
            snprintf(s, sizeof(s), "\xe8\xaf\x8a\xe6\x96\xad\xe5\xae\x8c\xe6\x88\x90");          /* 诊断完成 */
        break;
    }
    default:
        snprintf(s, sizeof(s), "%s", s_have ? "\xe5\xb7\xb2\xe9\x87\x87\xe9\x9b\x86/\xe5\x8f\xaf\xe5\x88\x86\xe6\x9e\x90"  /* 已采集/可分析 */
                                           : "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5/\xe5\xb0\xb1\xe7\xbb\xaa");            /* 已连接/就绪 */
        break;
    }
    ld_mix_center(l, LD_STATUS_Y, s);

    /* 采集/回传中: 第二行给"步数 + 已用时 + 当前命令", 8px 单行放得下 */
    if (s_run >= 0) {
        char a[56];
        uint32_t el = ((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - s_start_ms) / 1000;
        int cur = (s_prog < s_total) ? s_prog + 1 : s_total;      /* 收尾时 s_prog==s_total, 别显示 n+1/n */
        int n = snprintf(a, sizeof(a), "%d/%d %us %s", cur, s_total, (unsigned)el, s_lastcmd);
        if (n > 47) { a[44] = '.'; a[45] = '.'; a[46] = '.'; a[47] = 0; }
        ld_asc_center(l, LD_DETAIL_Y, a);
    } else if (s_saved_b > 0) {
        char a[48];
        snprintf(a, sizeof(a), "saved %d KB", s_saved_b / 1024);
        ld_asc_center(l, LD_DETAIL_Y, a);
    }
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
    if (s_view == 2) {                                   /* 历史内容 */
        ld_render_text(l, "\xe9\x87\x87\xe9\x9b\x86\xe5\x86\x85\xe5\xae\xb9",            /* 采集内容 */
                       NULL, "\xe8\xbf\x94\xe5\x9b\x9e");
        return;
    }
    if (s_view == 3) {                                   /* 分析结论 */
        ld_render_text(l, "\xe5\x88\x86\xe6\x9e\x90\xe7\xbb\x93\xe8\xae\xba",
                       s_analyzed ? "\xe5\x9b\x9e\xe4\xbc\xa0" : NULL,                   /* 末页才给 回传 */
                       "\xe8\xbf\x94\xe5\x9b\x9e");
        return;
    }

    ld_render_cards(l);
    ld_draw_status(l);

    /* 底栏: 左软键按状态变, 右软键 启动/开始/取消 */
    const char *left = NULL, *right;
    if (s_usb_busy)                      right = NULL;                         /* 启动中: 不给重复入口 */
    else if (!os_hid_is_running())       right = "\xe5\x90\xaf\xe5\x8a\xa8";  /* 启动 (USB 未就绪时先起 USB) */
    else if (s_run >= 0)                 right = "\xe5\x8f\x96\xe6\xb6\x88";  /* 取消 */
    else                                 right = "\xe5\xbc\x80\xe5\xa7\x8b";  /* 开始 */

    if (s_run == RUN_SEND) {
        left = NULL;                                                   /* 回传中: 左键留空 */
    } else if (s_run >= 0) {
        left = "\xe6\x97\xa5\xe5\xbf\x97";                             /* 日志 */
    } else if (s_analyzed) {
        left = "\xe5\x9b\x9e\xe4\xbc\xa0";                             /* 回传 */
    } else if (s_have) {
        left = "\xe5\x88\x86\xe6\x9e\x90";                             /* 分析 */
    } else {
        left = "\xe5\x85\xa8\xe9\x87\x87";                             /* 全采 */
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
                snprintf(s_hist_path, sizeof(s_hist_path), LOGDIR "/%s", s_hist[s_hist_sel]);
                s_text_pg = 0;
                ld_hist_load(0);
                s_view = 2;
                ctx->needs_redraw = true;
                return true;
            }
            if (s_view == 2) {                           /* 历史内容: 翻页 */
                if (ld_text_more()) {
                    s_text_pg++;
                    ld_hist_load(s_text_pg * LD_TEXT_ROWS);
                    ctx->needs_redraw = true;
                }
                return true;
            }
            if (s_view == 3) {                           /* 分析结论: 翻页 / 末页回传 */
                if (ld_text_more()) {
                    s_text_pg++;
                } else if (s_analyzed) {
                    ld_backhaul(ctx);
                    if (s_run == RUN_SEND) s_view = 0;   /* 回传开始 → 回主视图看进度, 别停在结论页干等 */
                }
                ctx->needs_redraw = true;
                return true;
            }
            if (s_run >= 0) {                                    /* 采集中: 左键=日志 (可回头看历史, 不影响本次采集) */
                ld_hist_scan();
                s_hist_sel = 0;
                s_view = 1;
                ctx->needs_redraw = true;
                return true;
            }
            if (s_run < 0 && s_analyzed) ld_backhaul(ctx);       /* 分析完 → 回传 */
            else if (s_run < 0 && s_have) ld_analyze();          /* 有结果 → 分析 */
            else if (s_run < 0) ld_start(ctx, CARD_ALL);         /* 空闲 → 一键全采 */
            ctx->needs_redraw = true;
            return true;
        }
        if (x > 310) {                                   /* 右软键 */
            if (s_view != 0) { s_view = 0; s_text_pg = 0; ctx->needs_redraw = true; return true; }
            if (s_usb_busy) return true;                 /* 启动中: 与"软键留空"一致, 忽略点击 */
            if (s_run >= 0) {
                ld_cancel();                             /* 取消: 协作退出, 不留卡键 */
            } else if (!os_hid_is_running()) {
                ld_usb_kick();                           /* USB 没起来: 先起 USB */
            } else {
                ld_start(ctx, s_sel);
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
            else ld_start(ctx, i);
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
        if (s_view == 0 && s_run < 0) ld_start(ctx, s_sel);
        ctx->needs_redraw = true;
        return;
    }
    if (a == OS_ACTION_BACK) {
        if (s_view != 0) { s_view = 0; s_text_pg = 0; ctx->needs_redraw = true; return; }
        os_request_exit_confirm(ctx);
        return;
    }
}

/* ================= 11. 轮询: 读 CDC 回传 ================= */
static void ld_poll(ui_ctx_t *ctx)
{
    static uint8_t buf[512];
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    /* ★ 回传守卫: 无论有没有数据都要每帧检查一次 —— 否则"一个字节都收不到"时永远不触发。 */
    if (s_run == RUN_SEND && !s_abort) {
        if (!os_hid_is_running() || !os_hid_connected()) {
            s_dev_lost  = true;                      /* USB 掉了: 不是"超时", 是断线 */
            s_fail_send = true;                      /* 文案要说"回传中断", 不是"采集中断" */
            ld_cancel();
            ctx->needs_redraw = true;
            return;
        }
        /* ★ 打字阶段 (s_send_typing) 只能查总超时:
         *   那条 PS 命令 800+ 字符要打 40 秒, 这期间**本来就没有数据**,
         *   拿空闲超时去卡它 = 每轮必死在打字途中。空闲计时从"打完命令"才开始。 */
        uint32_t since_rx = s_send_typing ? 0u : (now - s_last_rx_ms);
        uint32_t idle_lim = s_send_typing ? 0u : LD_SEND_IDLE_MS;   /* 0 = 不判断 */
        dc_xfer_t v = dc_xfer_state(since_rx, now - s_start_ms, idle_lim, LD_SEND_TOTAL_MS);
        if (v != DC_XFER_OK) {
            s_xfer_fail = true;                      /* 超时: 保留已收到的数据, 但标记不完整 */
            s_fail_send = true;
            ESP_LOGW(TAG, "回传超时 (%s): 已收 %d 字节 / %d 行",
                     v == DC_XFER_TOTAL_TIMEOUT ? "总时长" : "空闲", s_rx_len, s_rx_lines);
            ld_cancel();
            ctx->needs_redraw = true;
            return;
        }
    }

    size_t n = os_hid_cdc_read(buf, sizeof(buf) - 1);
    if (n == 0) return;
    s_last_rx_ms = now;
    buf[n] = 0;
    /* 首包若带 "COMPUTERNAME=<名字>" (来自 name.txt) 就认下电脑名。
     * ★ 2026-10-30: 跳过 '=' —— 新链写的是 'COMPUTERNAME='+$env:COMPUTERNAME,
     *   原来只跳空格, 会把 "=PC-01" 整个当成名字。 */
    char *p = strstr((char *)buf, "COMPUTERNAME");
    if (p) {
        p += 12;
        while (*p == ' ' || *p == '=') p++;
        int i = 0;
        while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < 31) s_pcname[i++] = *p++;
        s_pcname[i] = 0;
    }
    ld_hist_append((const char *)buf, (int)n);          /* 失败会置 s_sd_err, 不再静默 */
    if (s_rx_len + (int)n < (int)sizeof(s_rx) - 1) {
        memcpy(s_rx + s_rx_len, buf, n);
        s_rx_len += (int)n;
        s_rx[s_rx_len] = 0;
    } else if (s_rx_len < (int)sizeof(s_rx) - 1) {
        int room = (int)sizeof(s_rx) - 1 - s_rx_len;    /* 只收能装下的尾巴 */
        s_rx_trunc = true;                              /* 并在结论里明说"数据不完整" */
        memcpy(s_rx + s_rx_len, buf, (size_t)room);
        s_rx_len += room;
        s_rx[s_rx_len] = 0;
    } else {
        s_rx_trunc = true;
    }
    for (size_t i = 0; i < n; i++) if (buf[i] == '\n') s_rx_lines++;
    /* END = 这轮真的传完了 → ld_wait 提前收尾, 不用把 15s 等满。
     * 先在**这一小片**里做便宜的字符串判断, 命中才全量解析 (24KB 全文扫每包都做没必要)。 */
    if (!s_xfer_done && s_run == RUN_SEND) {
        /* 扫**缓冲尾部**而不是"这一小片": END 只有 9 字节, 有 ~2% 概率被 511B 的读边界切开,
         * 只看这一片会漏掉它 (漏了不影响正确性, 只是白等 15s)。尾部 80 字节足够。 */
        const char *tail = s_rx + (s_rx_len > 80 ? s_rx_len - 80 : 0);
        if (strstr(tail, "##### END") != NULL) {
            int end_n = 0;
            if (dc_parse_end(s_rx, &end_n)) {
                s_xfer_done = true;
                ESP_LOGI(TAG, "回传完成标记: %d 个文件, 跳过 %d", end_n, dc_count_skips(s_rx));
            }
        }
    }
    if (s_run < 0) s_have = true;
    ctx->needs_redraw = true;
}

/* ================= 12. 页面生命周期 ================= */
/* 进页面是否自动拉起 USB。
 * 1 = 自动 (本页默认): 一进来就就绪, 屏上先显示"USB 启动中"。
 * 0 = 手动: 停在"USB 未启动", 由右软键"启动"拉起。
 * 置 0 的场景: 需要在页面里保持 Serial/JTAG 控制台 —— USB 一旦切成 HID+CDC,
 *   控制台会搬到 CDC 口 (原串口不再有输出, 抓日志/remcon 会断)。 */
#define LOGDIAG_USB_AUTO 1

static void ld_enter(ui_ctx_t *ctx)
{
    s_view = 0;
    s_sel = CARD_SYS;
    s_text_pg = 0;
    s_text_more = false;
#if LOGDIAG_USB_AUTO
    ld_usb_kick();          /* 后台拉起 USB; 屏上先显示"USB 启动中" (真实状态) */
#endif
    ctx->needs_redraw = true;
}

static void ld_exit(ui_ctx_t *ctx)
{
    (void)ctx;
    s_usb_want = false;                             /* 后台任务若还在跑, 它会自己在收尾时还回去 */
    ld_cancel();                                    /* 采集中退出: 先协作停止, 再清按键 */
    for (int i = 0; i < 20 && s_usb_busy; i++) vTaskDelay(pdMS_TO_TICKS(25));   /* 等启动收尾 ≤500ms */
    if (s_usb_owned) {                              /* 只还"本次由我们开的"那一份 */
        os_hid_stop();
        s_usb_owned = false;
    }
    s_view = 0;
    s_text_pg = 0;
    s_abort = false;
    ESP_LOGI(TAG, "退出日志诊断 (USB 已按进入前状态还原)");
}

static const os_module_t s_mod_logdiag = {
    .name = "logdiag", .page_id = OS_PAGE_LOGDIAO,
    .on_enter = ld_enter, .on_exit = ld_exit,
    .render = ld_render, .action = ld_action, .touch = ld_touch, .poll = ld_poll,
    .fullscreen = true,
    .exit_confirm = true,   /* 返回键先弹"退出程序?"确认 (2026-10-14 全局规范) */
    .swipe_back_disabled = true,  /* 采集中误触上滑退出 = 采集中断; 退出只走返回键确认 */
};
void os_page_logdiag_register(void) { os_register(&s_mod_logdiag); }
