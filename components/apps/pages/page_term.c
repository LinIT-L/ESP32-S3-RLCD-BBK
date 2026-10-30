/**
 * page_term.c — 终端 页面模块 (P4 迁移: 从 menu/terminal.c 改签名为 ui_ctx + 私有 state).
 *
 * 命令行 + 52 键虚拟键盘:
 *   - 上半输出区 (触摸平滑滚动回看历史), 中部命令栏(可左右滑动), 下半 52 键 QWERTY
 *   - 支持多系统模拟 (uos/kylin/dos/powershell) + 帮助分页
 *   - 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "ui_common.h"
#include "virtual_keys.h"  /* 系统虚拟按键（GBC 同款） */
#include "input.h"
#include "gesture_back.h"  /* 底部上滑=返回 的开关: gesture_back_enable(false) */
#include "sd_scan.h"
#include "font_zh.h"
#include "font_zh16.h"
#include "keyboard.h"
#include "board_battery.h"
#include "pal_entry.h"      /* ★ 仙剑启动器: pal_launch / pal_tick / pal_stop */
#include "ui_scroll.h"      /* ★ 2026-10-24 全项目统一跟手滚动 (docs/列表滚动规范.md) */
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_mac.h"      /* whoami: 真实 MAC */
/* ★ 2026-10-30: ps 需要 FreeRTOS 任务快照 API。
 *   configUSE_TRACE_FACILITY 由 FreeRTOSConfig.h 从 CONFIG_FREERTOS_USE_TRACE_FACILITY 映射而来,
 *   不 include 这个头的话宏是**未定义**(按 0 求值), 即使 sdkconfig 开了也会走降级分支。 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"      /* whoami: 真实 MAC */
#include "esp_flash.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#define TAG "TERM"

#define TM_W ST7305_WIDTH
#define TM_H ST7305_HEIGHT

/* --- 布局 --- */
#define TM_OUT_H  128
#define TM_BAR_H  24
#define TM_KB_Y   (ST7305_HEIGHT - KBD_H)
#define TM_BAR_Y  (TM_KB_Y - TM_BAR_H)
#define TM_LINE_LEN 50
#define TM_MAXLINES 200
#define HM_ROW 16
#define ZH16_ADV_W 14

/* --- 历史缓冲 (200 行约 10KB). 放 PSRAM, 不占内部 SRAM. --- */
/* ★ 2026-10-25 内存优化: 200 行 × 50B = 10KB 原来静态常驻, 只有打开终端才用 ⇒ 按需分配 */
static char (*s_buf)[TM_LINE_LEN];
static char s_buf_dummy[TM_LINE_LEN];
static bool term_buf_ensure(void) {
    if (!s_buf) {
        s_buf = heap_caps_calloc(TM_MAXLINES, TM_LINE_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_buf) ESP_LOGW("TERM", "终端行缓冲分配失败 (%d B)", TM_MAXLINES * TM_LINE_LEN);
    }
    return s_buf != NULL;
}
EXT_RAM_BSS_ATTR static char  s_cmd[TM_LINE_LEN];
#define LN(i) (term_buf_ensure() ? s_buf[(i)] : s_buf_dummy)
static bool  s_ready = true;
static int   s_nlines = 0;
static float s_scroll = 0.0f;
static float s_scrv   = 0.0f;
/* ★★★ 2026-10-24 统一滚动: 终端三块区域（输出区 / 帮助列表 / 命令栏横向）
 *   全部交给 ui_scroll。本文件的 s_scroll/s_hscroll/s_bar_ofs 仍然保留为
 *   **渲染读的镜像量**（每帧从 ui_scroll 写回），这样渲染代码一行都不用改。 */
static ui_scroll_t s_sc_out, s_sc_help, s_sc_bar;
static bool s_sc_ready = false;
static int  s_sc_out_cap = -1, s_sc_help_cap = -1, s_sc_bar_cap = -1;
static bool  s_dragout = false;
static int   s_drag_y0 = 0;
static int   s_drag_ty = 0;
static float s_drag_sc0 = 0.0f;
static int   s_ci = 0;
static int   s_cur = 0;
static kbd_t g_kbd;
static int   s_press = -1;
static int   s_bar_ofs = 0;
/* ★ 命令列表(横向)惯性: 拖动是 1:1 跟手, 但原来**没有惯性**, 松手就"啪"地停住;
 *   加上速度衰减后才有"顺滑一滑"的手感 (linit: "只是上页下一页的滑动, 不是流畅的滑动")。
 *   注意: 左右那两个箭头仍然是整页跳 40px —— 那是"翻页"按钮, 与拖动是两回事。 */
static float s_bar_v   = 0.0f;

/* 命令栏 */
static const char *const s_bar[] = {
    "help","cls","ver","mem","sys","date","echo","dir","type","ipconfig",
    "ping","netstat","shell","uptime","whoami","uname","ps","battery",
    "calc","random","clear","free","history","reboot"
};
#define TM_BAR_N (sizeof(s_bar)/sizeof(s_bar[0]))

static const char *const s_allcmds[] = {
    "help","cls","ver","mem","sys","date","time","echo","dir","ls",
    "cd","md","rd","del","ren","copy","set","type","cat",
    "ipconfig","ip","ping","netstat","shell","exit",
    "uptime","whoami","id","uname","ps","tasklist","battery","power",
    "calc","expr","random","rand","free","clear","history","reboot","shutdown",
    "os","switch"
};
#define TM_ALLCMD_N (sizeof(s_allcmds)/sizeof(s_allcmds[0]))

/* ============ 文本绘制 ============ */
#define GB_W 8
#define GB_H 16
static int s_clip_bot = 0;   /* >0: y>=s_clip_bot 不画 —— 滚动内容不得压到输入行/命令条 */
static void px(st7305_handle_t*l,int x,int y,int c){ if(x<0||x>=TM_W||y<0||y>=TM_H)return; if(s_clip_bot&&y>=s_clip_bot)return; st7305_draw_pixel(l,x,y,c); }
static void pf(st7305_handle_t*l,int x0,int y0,int x1,int y1,int c){ int t; if(x0>x1){t=x0;x0=x1;x1=t;} if(y0>y1){t=y0;y0=y1;y1=t;} for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++)px(l,x,y,c); }
static void dascii(st7305_handle_t*l,int x,int y,const char*s,int fg){
    int cx=x; for(const unsigned char*p=(const unsigned char*)s;*p;p++){
        int idx=((*p>=' '&&*p<=0x7E)?(*p-0x20):('?'-0x20));
        const uint8_t*b=FONT8X12[idx];
        for(int cy=0;cy<GB_H;cy++){ uint8_t bits=b[cy]; for(int cc=0;cc<GB_W;cc++)
            if(bits&(0x80u>>cc)) px(l,cx+cc,y+cy,fg); }
        cx+=GB_W; }
}
static int asw(const char*s){ int w=0; for(const unsigned char*p=(const unsigned char*)s;*p;p++) w+=GB_W; return w; }
static bool line_has_cn(const char*s){ for(const unsigned char*p=(const unsigned char*)s;*p;p++) if((*p&0xe0)==0xe0) return true; return false; }
static void dline(st7305_handle_t*l,int x,int y,const char*s,int fg){
    int h=line_has_cn(s)?ZH16_FONT_H:GB_H; int aofs=(h-GB_H)/2; int cx=x;
    for(const unsigned char*p=(const unsigned char*)s;*p;){
        if((*p&0xe0)==0xe0){ int idx=font_zh16_find_utf8((const char*)p);
            if(idx>=0){ const uint8_t*b=font_zh16_get_bitmap_by_index(idx);
                if(b)for(int r=0;r<ZH16_FONT_H;r++){ const uint8_t*sp=b+r*2;
                    for(int c=0;c<ZH16_FONT_W;c++) if((sp[c>>3]>>(7-(c&7)))&1) px(l,cx+c,y+r,fg); } }
            cx+=ZH16_ADV_W; p+=3; }
        else { if(*p>=0x20&&*p<=0x7e){ const uint8_t*b=FONT8X12[*p-0x20];
                for(int cy=0;cy<GB_H;cy++){ uint8_t bits=b[cy]; for(int cc=0;cc<GB_W;cc++) if(bits&(0x80u>>cc)) px(l,cx+cc,y+aofs+cy,fg); } }
               cx+=GB_W; p++; }
    }
}
static int asw_n(const char*s,int n){ int w=0; for(int i=0;i<n&&s[i];i++) w+=GB_W; return w; }
static void dline_clip(st7305_handle_t*l,int x,int y,const char*s,int maxw){
    static char cbuf[64]; int n=0,w=0;
    const unsigned char*p=(const unsigned char*)s;
    while(*p){ int gw=((*p&0xe0)==0xe0)?ZH16_FONT_W:GB_W;
        if(gw>maxw-w || n+((*p&0xe0)==0xe0?3:1)>=sizeof(cbuf)-1) break;
        cbuf[n++]=*p; if((*p&0xe0)==0xe0){ cbuf[n++]=p[1]; cbuf[n++]=p[2]; }
        p++; w+=gw; }
    cbuf[n]=0; dline(l,x,y,cbuf,0);
}
static int line_h(const char*s){ return line_has_cn(s)?ZH16_FONT_H:GB_H; }

/* ============ 输出行缓冲 ============ */
static void line_set(int idx,const char*s){ if(!s_ready||idx<0||idx>=TM_MAXLINES)return; strncpy(LN(idx),s,TM_LINE_LEN-1); LN(idx)[TM_LINE_LEN-1]=0; }
/* ★ 2026-10-30: 输出超过 TM_MAXLINES 时**标记溢出**, 供界面提示"上面还有内容被滚掉了",
 *   不再静默丢行 (原来用户完全不知道早期输出已消失)。 */
static bool s_line_ovf = false;

static void line_add(const char*s){ if(!s_ready)return;
    if(s_nlines>=TM_MAXLINES){
        s_line_ovf = true;
        for(int r=0;r<TM_MAXLINES-1;r++)memcpy(LN(r),LN(r+1),TM_LINE_LEN);
        line_set(TM_MAXLINES-1,s);
    }
    else { line_set(s_nlines,s); s_nlines++; } }

/* ============ 命令实现 ============ */
#define HELP_PER 12
static bool  s_help_active = false;
static float s_hscroll = 0.0f;   /* 帮助列表像素滚动量 (0=顶部) */
static float s_hscrv   = 0.0f;   /* 帮助列表惯性 */
typedef struct { const char*c; const char*d; } help_item;
static const help_item s_item_uos[] = {
    {"os","切换系统"}, {"help","帮助"}, {"cls","清屏"}, {"ver","版本"}, {"mem","内存"},
    {"sys","信息"}, {"date","时间"}, {"echo","回显"}, {"dir/ls","目录"}, {"type/cat","读文件"},
    {"cd","切换"}, {"pwd","路径"}, {"mkdir","建目录"}, {"touch","建文件"}, {"rm","删除"},
    {"cp","复制"}, {"mv","移动"}, {"ln","链接"}, {"find","查找"}, {"grep","过滤"},
    {"chmod","权限"}, {"chown","属主"}, {"head","头部"}, {"tail","尾部"}, {"df","磁盘"},
    {"du","大小"}, {"top","监控"}, {"kill","杀进程"}, {"ps","进程"}, {"free","内存"},
    {"uptime","时长"}, {"whoami","用户"}, {"uname","系统"}, {"hostname","主机"}, {"lscpu","芯片"},
    {"ipconfig","网络"}, {"ping","连通"}, {"netstat","连接"}, {"apt","装包"}, {"dpkg","装包"},
    {"tar","压缩"}, {"unzip","解压"}, {"man","手册"}, {"vim","编辑"}, {"tree","树"},
    {"battery","电量"}, {"calc","计算"}, {"random","随机"}, {"history","历史"}, {"shell","退出"},
};
#define UOS_N (int)(sizeof(s_item_uos)/sizeof(s_item_uos[0]))
#define UOS_PAGES ((UOS_N+HELP_PER-1)/HELP_PER)
static const help_item s_item_kylin[] = {
    {"os","切换系统"}, {"help","帮助"}, {"cls","清屏"}, {"ver","版本"}, {"mem","内存"},
    {"sys","信息"}, {"date","时间"}, {"echo","回显"}, {"ls/dir","目录"}, {"cat/type","读文件"},
    {"pwd","路径"}, {"cd","切换"}, {"mkdir","建目录"}, {"touch","建文件"}, {"rm","删除"},
    {"cp","复制"}, {"mv","移动"}, {"ln","链接"}, {"find","查找"}, {"grep","过滤"},
    {"chmod","权限"}, {"chown","属主"}, {"head","头部"}, {"tail","尾部"}, {"df","磁盘"},
    {"du","大小"}, {"top","监控"}, {"free","内存"}, {"ps","进程"}, {"kill","杀进程"},
    {"netstat","连接"}, {"ping","连通"}, {"yum","装包"}, {"dnf","装包"}, {"rpm","rpm包"},
    {"tar","压缩"}, {"gzip","压缩"}, {"man","手册"}, {"vim","编辑"}, {"whoami","用户"},
    {"uname","系统"}, {"hostname","主机"}, {"uptime","时长"}, {"battery","电量"}, {"calc","计算"},
    {"random","随机"}, {"history","历史"}, {"shell","退出"},
};
#define KYLIN_N (int)(sizeof(s_item_kylin)/sizeof(s_item_kylin[0]))
#define KYLIN_PAGES ((KYLIN_N+HELP_PER-1)/HELP_PER)
static const help_item s_item_dos[] = {
    {"os","切换系统"}, {"help","帮助"}, {"dir","目录"}, {"cd","切换"}, {"type","读文件"},
    {"copy","复制"}, {"del","删除"}, {"ren","改名"}, {"move","移动"}, {"md","建目录"},
    {"rd","删目录"}, {"cls","清屏"}, {"ver","版本"}, {"date","日期"}, {"time","时间"},
    {"echo","回显"}, {"ipconfig","网络"}, {"ping","连通"}, {"netstat","连接"}, {"path","路径"},
    {"set","环境变量"}, {"where","定位"}, {"tasklist","进程"}, {"tree","目录树"}, {"shutdown","关机"},
    {"fc","比较"}, {"find","查找"}, {"chkdsk","检盘"}, {"vol","卷标"},
};
#define DOS_N (int)(sizeof(s_item_dos)/sizeof(s_item_dos[0]))
#define DOS_PAGES ((DOS_N+HELP_PER-1)/HELP_PER)
static const help_item s_item_ps[] = {
    {"os","切换系统"}, {"help","帮助"}, {"dir","目录"}, {"cd/sl","切换"}, {"type/gc","读文件"},
    {"copy/ci","复制"}, {"del/ri","删除"}, {"ren","改名"}, {"move","移动"}, {"md","建目录"},
    {"rd","删目录"}, {"cls/ch","清屏"}, {"ver","版本"}, {"date","日期"}, {"echo","回显"},
    {"ipconfig","网络"}, {"ping/tc","连通"}, {"get-process","进程"}, {"get-service","服务"},
    {"get-command","命令"}, {"get-help","帮助"}, {"set-location","路径"}, {"copy-item","复制"},
    {"remove-item","删除"}, {"new-item","新建"}, {"get-content","读文件"}, {"write-output","输出"},
    {"stop-process","杀进程"}, {"start-service","启服务"},
};
#define PS_N (int)(sizeof(s_item_ps)/sizeof(s_item_ps[0]))
#define PS_PAGES ((PS_N+HELP_PER-1)/HELP_PER)

static void cmd_help(void){ s_help_active=true; s_hscroll=0.0f; s_hscrv=0.0f; }
static void cmd_cls(void){ s_nlines=0; s_line_ovf=false; for(int i=0;i<TM_MAXLINES;i++)line_set(i,""); }
static void cmd_ver(void){ char b[48]; snprintf(b,48," ESP-IDF %s",esp_get_idf_version()); line_add(b); line_add(" LinTOS v1.3 (系统终端)"); }
static void cmd_date(void){ time_t n=time(NULL); struct tm*t=localtime(&n); char b[48]; snprintf(b,48," %04d-%02d-%02d %02d:%02d:%02d",t->tm_year+1900,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec); line_add(b); }
static void cmd_mem(void){ char b[64];
    snprintf(b,64," 内部空闲: %u B",(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); line_add(b);
    snprintf(b,64," PSRAM空闲: %u B",(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)); line_add(b);
    snprintf(b,64," 总空闲:   %u B",(unsigned)heap_caps_get_free_size(MALLOC_CAP_32BIT)); line_add(b); }
static void cmd_sys(void){ char b[64]; esp_chip_info_t ci; esp_chip_info(&ci);
    snprintf(b,64," 芯片: ESP32-S3  核: %d",(int)ci.cores); line_add(b);
    snprintf(b,64," 主频: %d MHz",CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ); line_add(b);
    uint32_t fsz=0; if(esp_flash_get_size(NULL,&fsz)==ESP_OK){ snprintf(b,64," Flash: %u MB",(unsigned)(fsz/1048576)); line_add(b); } }
static const char *term_cwd(void);   /* 当前目录, 定义见下方 DOS 核心 */

/* ==================== 仙剑启动器 (SDLPal) ====================
 * 终端当启动器：敲 `pal` 起游戏，游戏画面接管整屏，按返回键请求退出。
 * 游戏跑在独立任务里（pal_entry.c），终端只在 poll 里调 pal_tick() 上屏，
 * 所以游戏不会把 UI 卡死。 */
static bool s_pal_mode = false;

/* ---- 仙剑输入：物理按键走"脉冲"（OS 只给按下事件），虚拟手柄走真实按下/松开 ---- */
static pal_key_t s_pal_key  = PAL_KEY_NONE;   /* 物理键当前按住的那个（脉冲用） */
static int       s_pal_hold = 0;              /* 剩余帧数，到 0 自动松开 */
static uint8_t   s_jp_prev  = 0xFF;           /* 上一帧虚拟按键 joypad 位（低有效） */

static void pal_pulse(pal_key_t k)
{
    if (s_pal_key != PAL_KEY_NONE) pal_input(s_pal_key, 0);
    s_pal_key  = k;
    s_pal_hold = 14;                 /* ~230ms @60fps：正好走一格 */
    pal_input(k, 1);
}
static void pal_release(void)
{
    if (s_pal_key != PAL_KEY_NONE) { pal_input(s_pal_key, 0); s_pal_key = PAL_KEY_NONE; }
}
/* 系统虚拟按键 joypad 掩码 -> 游戏键（低电平有效：某位清 0 = 按下）
 *   bit0=A bit1=B bit2=Select bit3=Start bit4=右 bit5=左 bit6=上 bit7=下 */
static void pal_sync_joypad(uint8_t jp)
{
    uint8_t chg = (uint8_t)(jp ^ s_jp_prev);
    if (!chg) return;
    if (chg & 0x40) pal_input(PAL_KEY_UP,    !(jp & 0x40));
    if (chg & 0x80) pal_input(PAL_KEY_DOWN,  !(jp & 0x80));
    if (chg & 0x20) pal_input(PAL_KEY_LEFT,  !(jp & 0x20));
    if (chg & 0x10) pal_input(PAL_KEY_RIGHT, !(jp & 0x10));
    if (chg & 0x01) pal_input(PAL_KEY_OK,    !(jp & 0x01));   /* A = 确认/对话 */
    if (chg & 0x02) pal_input(PAL_KEY_MENU,  !(jp & 0x02));   /* B = ESC/菜单 */
    if (chg & 0x08) pal_input(PAL_KEY_OK,    !(jp & 0x08));   /* Start = 确认 */
    if (chg & 0x04) pal_input(PAL_KEY_PGUP,  !(jp & 0x04));   /* Select = 翻页 */
    s_jp_prev = jp;
}

static void cmd_pal(void)
{
    if (pal_is_running() || s_pal_mode) { line_add(" 仙剑已在运行"); return; }
    if (!sd_is_mounted()) { line_add(" SD 未挂载"); return; }
    if (!pal_launch())   { line_add(" 启动失败: 检查 /sdcard/pal 数据文件"); return; }
    s_pal_mode = true;
    s_pal_key = PAL_KEY_NONE;
    s_jp_prev = 0xFF;
    pal_keys_enable(true);
    line_add(" 启动 仙剑奇侠传 ... (返回键退出)");
}
static void cmd_dir(const char*arg){ const char*p=(arg&&arg[0])?arg:term_cwd(); char b[64];
    if(!sd_is_mounted()){ line_add(" SD 未挂载"); return; }
    DIR*d=opendir(p); if(!d){ line_add(" 无法打开目录"); return; }
    struct dirent*e; int n=0;
    while((e=readdir(d))){
        if(e->d_name[0]=='.')continue;
        n++;
        snprintf(b,64," %.*s",32,e->d_name); line_add(b);
        if(n>=18){line_add(" ... (前18个)");break;}
    }
    closedir(d); if(!n)line_add(" (空目录)"); }
static void cmd_type(const char*f){ if(!f||!f[0]){line_add(" type <文件>");return;} if(!sd_is_mounted()){line_add(" SD 未挂载");return;}
    FILE*fp=fopen(f,"r"); if(!fp){line_add(" 文件不存在");return;}
    char b[TM_LINE_LEN]; while(fgets(b,sizeof(b),fp)){ b[strcspn(b,"\n")]=0; line_add(b);} fclose(fp); }
static void cmd_ipconfig(void){ char b[64];
    esp_netif_t*nif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(!nif)nif=esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip;
    if(!nif||esp_netif_get_ip_info(nif,&ip)!=ESP_OK){ line_add(" 未连接网络"); return; }
    snprintf(b,64," IPv4: %d.%d.%d.%d",(int)(ip.ip.addr&0xff),(int)((ip.ip.addr>>8)&0xff),(int)((ip.ip.addr>>16)&0xff),(int)((ip.ip.addr>>24)&0xff)); line_add(b);
    snprintf(b,64," 掩码: %d.%d.%d.%d",(int)(ip.netmask.addr&0xff),(int)((ip.netmask.addr>>8)&0xff),(int)((ip.netmask.addr>>16)&0xff),(int)((ip.netmask.addr>>24)&0xff)); line_add(b);
    snprintf(b,64," 网关: %d.%d.%d.%d",(int)(ip.gw.addr&0xff),(int)((ip.gw.addr>>8)&0xff),(int)((ip.gw.addr>>16)&0xff),(int)((ip.gw.addr>>24)&0xff)); line_add(b); }
static void cmd_ping(const char*h){
    if(!h||!h[0]){ line_add(" ping <地址>"); return; }
    /* ★ 2026-10-30: 原实现回一句"ping ... 练习模拟", 会让用户以为发过包 —— 属伪造结果。
     *   实测本固件**未编入 esp_ping 组件** (components/esp_netif 下无 esp_ping.h,
     *   也无 esp_ping_new_session 符号), 因此无法发真实 ICMP。
     *   这里如实说明能力边界, 并给出当前**确实可用**的判网手段。 */
    line_add(" [不可用] 本固件未编入 ICMP(ping) 组件");
    line_add(" 可用的联网判据: ipconfig 看本机地址");
    line_add(" 或访问任意网页/更新, 能通即说明链路正常");
}
static void cmd_netstat(void){
    /* ★ 2026-10-30: lwIP 没有公开的"枚举全部 TCP 连接"API。
     *   遍历 tcp_active_pcbs / tcp_bound_pcbs 属访问内部链表 (tcp_priv.h),
     *   跨 lwIP 版本不稳定, 按要求不采用。这里如实说明, 不编造连接表。 */
    line_add(" [受限] lwIP 未提供公开的 TCP 连接枚举接口");
    line_add(" 本机地址/网关请用 ipconfig 查看");
}
static void cmd_echo(const char*a){ line_add((a&&a[0])?a:" "); }
static void cmd_uptime(void){
    int64_t us = esp_timer_get_time();
    int64_t s = us / 1000000LL; if (s < 0) s = 0;
    char b[64]; snprintf(b,64," 已运行: %lld天 %02lld:%02lld:%02lld",
                         (long long)(s/86400), (long long)((s%86400)/3600),
                         (long long)((s%3600)/60), (long long)(s%60)); line_add(b);
}
static void cmd_whoami(void){
    /* ★ 2026-10-30: 原来硬写 "root (模拟返回)" —— 编造了一个不存在的账户与权限。
     *   LinTOS 是单用户嵌入式系统, 没有登录/权限模型, 这里展示**真实设备身份**。 */
    uint8_t mac[6] = {0};
    char b[64];
    line_add(" 设备: LinTOS @ ESP32-S3");
    /* ★ 复核修复: 必须检查返回值 —— 读失败时不能把全 0 当成有效 MAC 展示 */
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(b, sizeof(b), " MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        line_add(b);
    } else {
        line_add(" MAC: 读取失败 (eFuse 不可读)");
    }
    line_add(" 账户: 无 (单用户嵌入式, 无登录/权限模型)");
}
static void cmd_uname(void){ line_add(" LinTOS v1.3"); line_add(" 内核: FreeRTOS @ ESP32-S3 ESP-IDF"); }
static void cmd_ps(void){
    /* ★ 2026-10-30: 原来是硬编码的假任务表 (PID/名称/占用全是编的)。
     *   改为 FreeRTOS 真实任务快照 (uxTaskGetSystemState), 需要 configUSE_TRACE_FACILITY=1;
     *   若该选项未开, 如实告知而不回退到假数据。 */
    #if (configUSE_TRACE_FACILITY == 1)
    static TaskStatus_t tst[24];
    UBaseType_t n = uxTaskGetSystemState(tst, 24, NULL);
    if (n == 0) { line_add(" 任务表读取失败"); return; }
    char b[64];
    /* 栈余单位说明: FreeRTOS 的 usStackHighWaterMark 单位是 **字(StackType_t)**,
     * ESP32-S3 上 1 字 = 4 字节。标成"字节"会小看实际余量 4 倍, 故按字显示。 */
    snprintf(b, sizeof(b), " 任务(%u) 名          栈余字 优先", (unsigned)n);
    line_add(b);
    for (UBaseType_t i = 0; i < n && i < 12; i++) {   /* 只显示前 12 条, 避免刷屏 */
        snprintf(b, sizeof(b), " %-12.12s %5u %4u",
                 tst[i].pcTaskName ? tst[i].pcTaskName : "?",
                 (unsigned)tst[i].usStackHighWaterMark,
                 (unsigned)tst[i].uxCurrentPriority);
        line_add(b);
    }
    if (n > 12) { snprintf(b, sizeof(b), " ...共 %u 个任务", (unsigned)n); line_add(b); }
    #else
    line_add(" [不支持] 固件未开启 configUSE_TRACE_FACILITY");
    line_add(" 无法读取真实任务表, 不展示虚构数据");
    #endif
}
static void cmd_battery(void){
    board_battery_status_t st = {0};
    unsigned pct = 0;
    if (board_battery_read_cached(&st) == ESP_OK) pct = st.percent;
    char b[48]; snprintf(b,48," 电量: %u%%", pct); line_add(b);
}
static void cmd_calc(const char*arg){
    if(!arg||!arg[0]){ line_add(" calc <表达式>  如: calc 3+5*2"); return; }
    const char*p=arg; char*end=NULL;
    double a=strtod(p,&end); if(end==p){ line_add(" 表达式格式错误"); return; }
    if(!*end){ char b[48]; snprintf(b,48," = %.15g",a); line_add(b); return; }
    char op=*end++;
    double b2=strtod(end,&end);
    double r = op=='+'?a+b2 : op=='-'?a-b2 : op=='*'?a*b2 : (b2==0?0:a/b2);
    char bf[48]; snprintf(bf,48," = %.15g",r); line_add(bf);
}
static void cmd_random(const char*arg){
    int n = (arg&&arg[0])?atoi(arg):100;
    if(n<1){n=1;}
    if(n>10000000){n=10000000;}
    uint32_t r = (uint32_t)(esp_random() % (uint32_t)n);
    char b[48]; snprintf(b,48," 随机 [0-%d): %u",n,(unsigned)r); line_add(b);
}

/* == 模拟多系统终端 == */
#define OS_N 4
static const char *const s_osname[OS_N] = {"uos","kylin","dos","powershell"};
static const char *const s_osinfo[OS_N] = {
    "统信UOS (apt 包管理)", "银河麒麟 (yum/dnf 包管理)",
    "Windows CMD (安装包/.exe)", "Windows PowerShell (winget)"
};
static int s_os = 0;
static void cmd_os(const char*arg){
    if(!arg||!arg[0]){
        char b[48]; snprintf(b,48," 当前: %s",s_osinfo[s_os]); line_add(b);
        line_add(" 可用: uos kylin dos powershell");
        line_add(" 用法: os <系统名> 切换练习环境");
        return;
    }
    for(int i=0;i<OS_N;i++){
        if(!strcasecmp(arg,s_osname[i])){
            s_os=i;
            char b[48]; snprintf(b,48," 已切换至 %s",s_osinfo[i]); line_add(b);
            return;
        }
    }
    line_add(" 未识别系统名, 输入 os 查看列表");
}
static const char* os_prompt(void){
    switch(s_os){ case 1: return "root@kylin:~# "; case 2: return "C:\\> "; case 3: return "PS C:\\> "; default:return "root@uos:~# "; }
}
static const help_item *help_list(void){
    switch(s_os){ case 1:return s_item_kylin; case 2:return s_item_dos; case 3:return s_item_ps; default:return s_item_uos; }
}
static int help_count(void){
    switch(s_os){ case 1:return KYLIN_N; case 2:return DOS_N; case 3:return PS_N; default:return UOS_N; }
}
/* 帮助列表的最大滚动像素 (总行数*行高 - 可视高) */
static int help_maxs(void){
    int rows=(help_count()+1)/2; int xH=TM_OUT_H-GB_H;
    int tot=rows*HM_ROW; return tot>xH?tot-xH:0;
}

/* 学习命令表 */
typedef struct { const char *name; const char *tip; int plat; } learn_t;
static const learn_t s_learn[] = {
    {"cd"," cd <目录>: 切换目录  (cd .. 上一级 / cd ~ 家目录)",0},
    {"pwd"," pwd: 显示当前所在目录路径",0},
    {"mkdir"," mkdir <目录>: 创建目录  (mkdir -p a/b 递归创建)",0},
    {"touch"," touch <文件>: 新建空文件 / 更新时间戳",0},
    {"rm"," rm <文件>: 删除  (rm -rf 强制递归, 高危慎用!)",0},
    {"cp"," cp 源 目标: 复制  (cp -r 复制目录)",0},
    {"mv"," mv 源 目标: 移动 / 重命名",0},
    {"ln"," ln -s 源 链接: 创建符号链接",0},
    {"find"," find 路径 -name '*.log': 按名查找文件",0},
    {"grep"," grep '关键字' 文件: 文本过滤/搜索",0},
    {"chmod"," chmod 755 文件: 修改权限 (r4 w2 x1)",0},
    {"chown"," chown 用户 文件: 修改属主",0},
    {"head"," head -10 文件: 看前10行",0},
    {"tail"," tail -20 -f 文件: 看后20行/实时跟踪",0},
    {"tree"," tree: 以树形列出目录结构",2},
    {"top"," top: 实时 CPU/内存/进程监控",0},
    {"kill"," kill PID: 终止进程  (kill -9 强制)",0},
    {"df"," df -h: 查看磁盘分区使用情况",0},
    {"du"," du -sh 目录: 统计目录占用大小",0},
    {"lscpu"," lscpu: 查看 CPU 架构/核心数",0},
    {"hostname"," hostname: 显示主机名",0},
    {"env"," env: 显示环境变量",0},
    {"systemctl"," systemctl status 服务: 服务管理 (麒麟/UOS 常用)",0},
    {"service"," service 服务 restart: 老式服务控制",0},
    {"apt"," apt install <包>: 软件包管理 (UOS/Deepin 系)",0},
    {"yum"," yum install <包>: 软件包管理 (麒麟 系)",0},
    {"dnf"," dnf install <包>: 新版包管理",0},
    {"dpkg"," dpkg -i <包.deb>: 直接安装 deb 包",0},
    {"ifconfig"," ifconfig: 查看网络接口 (配 ip address)",0},
    {"ip"," ip addr: 查看 IP / 路由 (替代 ifconfig)",0},
    {"route"," route -n: 查看路由表",0},
    {"ss"," ss -tln: 查看监听的 TCP 端口",0},
    {"curl"," curl 网址: 命令行下载/请求",0},
    {"wget"," wget 网址: 下载文件",0},
    {"ssh"," ssh 用户@主机: 远程登录",0},
    {"scp"," scp 文件 用户@主机: 远程复制",0},
    {"useradd"," useradd -m 用户名: 新建用户",0},
    {"usermod"," usermod -aG 组 用户: 改用户属性",0},
    {"tar"," tar -zxvf 包.tar.gz: 解压 (c=创建 x=解压)",0},
    {"zip"," zip -r a.zip 目录: 打包为 zip",0},
    {"unzip"," unzip a.zip: 解压 zip",0},
    {"gzip"," gzip 文件: 压缩 (gunzip 解压)",0},
    {"more"," more 文件: 分屏查看 (空格翻页)",0},
    {"less"," less 文件: 滚动查看 (q 退出)",0},
    {"man"," man 命令: 查看命令手册",0},
    {"vim"," vim 文件: 编辑器 (i 插入 / Esc / :wq 保存退出)",0},
    {"who"," who: 查看当前登录用户",0},
    {"copy"," copy 源 目标: 复制文件 (Win), 同 Linux cp",1},
    {"del"," del 文件: 删除文件 (Win), 同 Linux rm",1},
    {"ren"," ren 旧名 新名: 重命名 (Win), 同 Linux mv",1},
    {"md"," md 目录: 创建目录 (Win), 同 Linux mkdir",1},
    {"rd"," rd 目录: 删除目录 (Win)",1},
    {"tasklist"," tasklist: 进程列表 (Win), 同 Linux ps",1},
    {"where"," where 程序名: 定位命令位置 (Win)",1},
    {"shutdown"," shutdown /s 关机 / /r 重启 (Win)",1},
};
#define LEARN_N (sizeof(s_learn)/sizeof(s_learn[0]))
static int learn_lookup(const char*n){ for(int i=0;i<(int)LEARN_N;i++) if(!strcasecmp(n,s_learn[i].name)) return i; return -1; }

/* ==================== DOS 核心: 当前目录 / 环境变量 / 批处理 ====================
 * 参考 RetiredWizard/PyDOS（MicroPython 版 DOS shell）的机制, 按 C 重写:
 *   · 开关解析 → 位掩码            · 引号参数跨 token 合并
 *   · %VAR% 逐字符展开             · 批处理 label 表 + 行号跳转 (PyDOS 用文件偏移 seek, 这里整文件读进内存按行号跳)
 *   PyDOS 用 MicroPython 有几十 KB 堆可用; 我们内部 RAM 紧张, 所以:
 *     .bat 上限 6KB、最多 160 行 / 16 个标签、嵌套最多 3 层。 */

#define TM_CWD_MAX   96
#define TM_ENV_MAX   10
#define TM_ENV_NLEN  20
#define TM_ENV_VLEN  40
#define TM_BAT_MAX   6144
#define TM_BAT_LINES 160
#define TM_BAT_LABS  16
#define TM_BAT_MAXDEPTH 3

static char s_cwd[TM_CWD_MAX] = "/sdcard";
static char s_envn[TM_ENV_MAX][TM_ENV_NLEN];
static char s_envv[TM_ENV_MAX][TM_ENV_VLEN];
static int  s_envcnt   = 0;
static int  s_bat_echo = 1;      /* ECHO ON/OFF */
static int  s_bat_depth = 0;
static char *s_bat_buf = NULL;   /* .bat 整文件缓冲 (PSRAM, 首次用时分配, 常驻复用) */

static bool exec_one(char *c);   /* 前向声明 (批处理要调) */

static const char *term_cwd(void) { return s_cwd; }

/* 把 arg 解析成绝对路径: 支持相对路径 / 绝对路径 / . / .. */
static void path_resolve(const char *arg, char *out, size_t n)
{
    if (!arg || !arg[0]) { snprintf(out, n, "%s", s_cwd); return; }
    char tmp[TM_CWD_MAX * 2];
    if (arg[0] == '/') snprintf(tmp, sizeof(tmp), "%s", arg);
    else               snprintf(tmp, sizeof(tmp), "%s/%s", s_cwd, arg);

    char *seg[24]; int ns = 0;
    char work[TM_CWD_MAX * 2];
    snprintf(work, sizeof(work), "%s", tmp);
    for (char *p = strtok(work, "/"); p && ns < 24; p = strtok(NULL, "/")) {
        if (!strcmp(p, ".")) continue;
        if (!strcmp(p, "..")) { if (ns) ns--; continue; }
        seg[ns++] = p;
    }
    out[0] = 0;
    for (int i = 0; i < ns; i++) {
        size_t l = strlen(out);
        if (l + 1 >= n) break;
        snprintf(out + l, n - l, "/%s", seg[i]);
    }
    if (!out[0]) snprintf(out, n, "/");
}

/* 拆两个参数 (copy/del 等): a 第一段 b 其余 */
static void args2(const char *arg, char *a, size_t an, char *b, size_t bn)
{
    a[0] = b[0] = 0;
    if (!arg) return;
    while (*arg == ' ' || *arg == '\t') arg++;
    const char *sp = strchr(arg, ' ');
    if (!sp) { snprintf(a, an, "%s", arg); return; }
    size_t l = (size_t)(sp - arg);
    if (l >= an) l = an - 1;
    memcpy(a, arg, l); a[l] = 0;
    while (*sp == ' ') sp++;
    snprintf(b, bn, "%s", sp);
}

static const char *env_get(const char *k)
{
    for (int i = 0; i < s_envcnt; i++) if (!strcasecmp(s_envn[i], k)) return s_envv[i];
    return "";
}

/* %VAR% 展开 (结果写 out, 不会原地改 in) */
static void env_expand(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; ) {
        if (*p == '%') {
            const char *q = p + 1;
            char key[TM_ENV_NLEN]; size_t k = 0;
            while (*q && *q != '%' && k + 1 < sizeof(key)) key[k++] = *q++;
            key[k] = 0;
            if (*q == '%') {                       /* 成对 ⇒ 变量 */
                const char *v = key[0] ? env_get(key) : "%";
                while (*v && o + 1 < n) out[o++] = *v++;
                p = q + 1;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

/* ---- 文件/目录命令 ---- */
static void cmd_cd(const char *arg)
{
    char p[TM_CWD_MAX]; path_resolve(arg, p, sizeof(p));
    DIR *d = opendir(p);
    if (!d) { line_add(" 目录不存在"); return; }
    closedir(d);
    snprintf(s_cwd, sizeof(s_cwd), "%s", p);
    char b[TM_LINE_LEN]; snprintf(b, sizeof(b), " %s", s_cwd); line_add(b);
}

static void cmd_md(const char *arg)
{
    if (!arg || !arg[0]) { line_add(" md <目录名>"); return; }
    if (!sd_is_mounted()) { line_add(" SD 未挂载"); return; }
    char p[TM_CWD_MAX]; path_resolve(arg, p, sizeof(p));
    if (mkdir(p, 0777) == 0) line_add(" 已创建");
    else                     line_add(" 创建失败(已存在?)");
}

static void cmd_rd(const char *arg)
{
    if (!arg || !arg[0]) { line_add(" rd <目录名>"); return; }
    if (!sd_is_mounted()) { line_add(" SD 未挂载"); return; }
    char p[TM_CWD_MAX]; path_resolve(arg, p, sizeof(p));
    if (rmdir(p) == 0) line_add(" 已删除目录");
    else               line_add(" 删除失败(非空/不存在)");
}

static void cmd_del(const char *arg)
{
    if (!arg || !arg[0]) { line_add(" del <文件名>"); return; }
    if (!sd_is_mounted()) { line_add(" SD 未挂载"); return; }
    char p[TM_CWD_MAX]; path_resolve(arg, p, sizeof(p));
    if (remove(p) == 0) line_add(" 已删除");
    else                line_add(" 删除失败(文件不存在)");
}

static void cmd_ren(const char *arg)
{
    char a[64], b[64]; args2(arg, a, sizeof(a), b, sizeof(b));
    if (!a[0] || !b[0]) { line_add(" ren <旧名> <新名>"); return; }
    if (!sd_is_mounted()) { line_add(" SD 未挂载"); return; }
    char pa[TM_CWD_MAX], pb[TM_CWD_MAX];
    path_resolve(a, pa, sizeof(pa));
    path_resolve(b, pb, sizeof(pb));
    if (rename(pa, pb) == 0) line_add(" 已重命名");
    else                     line_add(" 重命名失败");
}

static void cmd_copy(const char *arg)
{
    char a[64], b[64]; args2(arg, a, sizeof(a), b, sizeof(b));
    if (!a[0] || !b[0]) { line_add(" copy <源> <目标>"); return; }
    if (!sd_is_mounted()) { line_add(" SD 未挂载"); return; }
    char pa[TM_CWD_MAX], pb[TM_CWD_MAX];
    path_resolve(a, pa, sizeof(pa));
    path_resolve(b, pb, sizeof(pb));

    /* 目标是已存在的目录 ⇒ 自动拼上源文件名 */
    DIR *d = opendir(pb);
    if (d) {
        closedir(d);
        const char *nm = strrchr(pa, '/'); nm = nm ? nm + 1 : pa;
        size_t l = strlen(pb);
        snprintf(pb + l, sizeof(pb) - l, "/%s", nm);
    }

    FILE *fs = fopen(pa, "rb");
    if (!fs) { line_add(" 源文件不存在"); return; }
    FILE *fd = fopen(pb, "wb");
    if (!fd) { fclose(fs); line_add(" 无法写入目标"); return; }
    static char cbuf[512];
    size_t n; unsigned tot = 0;
    while ((n = fread(cbuf, 1, sizeof(cbuf), fs)) > 0) {
        if (fwrite(cbuf, 1, n, fd) != n) break;
        tot += (unsigned)n;
    }
    fclose(fs); fclose(fd);
    char o[TM_LINE_LEN]; snprintf(o, sizeof(o), " 已复制 %u 字节", tot); line_add(o);
}

static void cmd_set(const char *arg)
{
    if (!arg || !arg[0]) {                       /* 不带参数: 列出全部 */
        for (int i = 0; i < s_envcnt; i++) {
            char b[TM_LINE_LEN];
            snprintf(b, sizeof(b), " %s=%s", s_envn[i], s_envv[i]);
            line_add(b);
        }
        if (!s_envcnt) line_add(" (无环境变量)");
        return;
    }
    const char *eq = strchr(arg, '=');
    if (!eq) {
        char b[TM_LINE_LEN];
        snprintf(b, sizeof(b), " %s=%s", arg, env_get(arg));
        line_add(b);
        return;
    }
    char k[TM_ENV_NLEN]; size_t kl = (size_t)(eq - arg);
    if (kl >= sizeof(k)) kl = sizeof(k) - 1;
    memcpy(k, arg, kl); k[kl] = 0;
    while (kl && (k[kl-1] == ' ')) k[--kl] = 0;

    int idx = -1;
    for (int i = 0; i < s_envcnt; i++) if (!strcasecmp(s_envn[i], k)) { idx = i; break; }
    if (idx < 0) {
        if (s_envcnt >= TM_ENV_MAX) { line_add(" 环境变量已满"); return; }
        idx = s_envcnt++;
        snprintf(s_envn[idx], TM_ENV_NLEN, "%s", k);
    }
    snprintf(s_envv[idx], TM_ENV_VLEN, "%s", eq + 1);
    char b[TM_LINE_LEN];
    snprintf(b, sizeof(b), " %s=%s", s_envn[idx], s_envv[idx]);
    line_add(b);
}

/* ---- 批处理执行 (.bat) ---- */
static void bat_run(const char *path)
{
    if (s_bat_depth >= TM_BAT_MAXDEPTH) { line_add(" 批处理嵌套过深"); return; }
    FILE *f = fopen(path, "r");
    if (!f) { line_add(" 批处理文件不存在"); return; }
    if (!s_bat_buf) {
        s_bat_buf = heap_caps_malloc(TM_BAT_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_bat_buf) { fclose(f); line_add(" 内存不足"); return; }
    }
    size_t n = fread(s_bat_buf, 1, TM_BAT_MAX, f);
    fclose(f);
    s_bat_buf[n] = 0;

    /* 切行 */
    static char *ln[TM_BAT_LINES];
    int nl = 0;
    char *p = s_bat_buf;
    while (*p && nl < TM_BAT_LINES) {
        ln[nl++] = p;
        char *e = strchr(p, '\n');
        if (!e) break;
        *e = 0;
        p = e + 1;
    }
    /* 标签表 (预扫, 对应 PyDOS 的 batLabels) */
    static char lab[TM_BAT_LABS][TM_ENV_NLEN];
    static int  labln[TM_BAT_LABS];
    int nlab = 0;
    for (int i = 0; i < nl && nlab < TM_BAT_LABS; i++) {
        char *s = ln[i];
        while (*s == ' ' || *s == '\t') s++;
        if (*s == ':') {
            snprintf(lab[nlab], TM_ENV_NLEN, "%s", s + 1);
            labln[nlab] = i;
            nlab++;
        }
    }

    s_bat_depth++;
    int pc = 0;
    while (pc < nl) {
        char raw[128];
        snprintf(raw, sizeof(raw), "%s", ln[pc]);
        pc++;
        char *s = raw;
        while (*s == ' ' || *s == '\t') s++;
        size_t L = strlen(s);
        while (L && (s[L-1] == '\r' || s[L-1] == ' ' || s[L-1] == '\t')) s[--L] = 0;
        if (!*s || *s == ':' || *s == ';') continue;        /* 空行/label/注释 */

        if (!strncasecmp(s, "rem", 3) && (s[3] == 0 || s[3] == ' ')) continue;

        if (!strncasecmp(s, "echo", 4) && (s[4] == 0 || s[4] == ' ')) {
            char *a = s + 4; while (*a == ' ') a++;
            if (!strcasecmp(a, "on"))       s_bat_echo = 1;
            else if (!strcasecmp(a, "off")) s_bat_echo = 0;
            else { char ex[128]; env_expand(a, ex, sizeof(ex)); line_add(ex); }
            continue;
        }
        if (!strncasecmp(s, "pause", 5) && (s[5] == 0 || s[5] == ' ')) {
            line_add(" -- 按任意键继续 --");
            continue;
        }
        if (!strncasecmp(s, "goto", 4) && (s[4] == 0 || s[4] == ' ')) {
            char *a = s + 4; while (*a == ' ') a++;
            int tgt = -1;
            for (int i = 0; i < nlab; i++) if (!strcasecmp(lab[i], a)) { tgt = labln[i]; break; }
            if (tgt < 0) { line_add(" GOTO 标签不存在"); break; }
            pc = tgt + 1;
            continue;
        }
        if (!strncasecmp(s, "if", 2) && s[2] == ' ') {
            char *a = s + 2; while (*a == ' ') a++;
            int neg = 0;
            if (!strncasecmp(a, "not ", 4)) { neg = 1; a += 4; while (*a == ' ') a++; }
            int cond = 0;
            if (!strncasecmp(a, "exist ", 6)) {
                a += 6; while (*a == ' ') a++;
                char fn[64];
                char *sp = strchr(a, ' ');
                if (sp) {
                    size_t l = (size_t)(sp - a); if (l >= sizeof(fn)) l = sizeof(fn) - 1;
                    memcpy(fn, a, l); fn[l] = 0;
                    a = sp + 1; while (*a == ' ') a++;
                } else { snprintf(fn, sizeof(fn), "%s", a); a += strlen(a); }
                char pa[TM_CWD_MAX]; path_resolve(fn, pa, sizeof(pa));
                struct stat st;
                cond = (stat(pa, &st) == 0);
            } else {
                char *eq = strstr(a, "==");
                if (!eq) { line_add(" IF 语法错"); continue; }
                char l[64], r[64];
                size_t ll = (size_t)(eq - a); if (ll >= sizeof(l)) ll = sizeof(l) - 1;
                memcpy(l, a, ll); l[ll] = 0;
                while (ll && l[ll-1] == ' ') l[--ll] = 0;
                snprintf(r, sizeof(r), "%s", eq + 2);
                char *sp = strchr(r, ' ');
                if (sp) { *sp = 0; a = sp + 1; while (*a == ' ') a++; } else { a += strlen(a); }
                cond = !strcasecmp(l, r);
            }
            if (neg) cond = !cond;
            if (!cond || !*a) continue;
            char ex[128]; env_expand(a, ex, sizeof(ex));
            if (s_bat_echo) { char d[TM_LINE_LEN]; snprintf(d, sizeof(d), "> %s", ex); line_add(d); }
            exec_one(ex);
            continue;
        }

        char ex[128];
        env_expand(s, ex, sizeof(ex));
        if (s_bat_echo) { char d[TM_LINE_LEN]; snprintf(d, sizeof(d), "> %s", ex); line_add(d); }
        exec_one(ex);
    }
    s_bat_depth--;
}

static bool exec_one(char*c){
    while(*c==' '||*c=='\t')c++;
    char arg[80]; arg[0]=0; char*sp=strchr(c,' '); if(sp){*sp=0; strncpy(arg,sp+1,sizeof(arg)-1);}
    if(!c[0]) return true;
    if(!strcasecmp(c,"help")||!strcmp(c,"?"))cmd_help();
    else if(!strcasecmp(c,"cls"))cmd_cls();
    else if(!strcasecmp(c,"ver"))cmd_ver();
    else if(!strcasecmp(c,"date")||!strcasecmp(c,"time"))cmd_date();
    else if(!strcasecmp(c,"mem")||!strcasecmp(c,"meminfo")||!strcasecmp(c,"free"))cmd_mem();
    else if(!strcasecmp(c,"sys")||!strcasecmp(c,"systeminfo"))cmd_sys();
    else if(!strcasecmp(c,"dir")||!strcasecmp(c,"ls"))cmd_dir(arg);
    /* ==== DOS 核心命令 (参考 PyDOS 补齐的真实实现) ==== */
    else if(!strcasecmp(c,"cd")||!strcasecmp(c,"chdir"))cmd_cd(arg);
    else if(!strcasecmp(c,"md")||!strcasecmp(c,"mkdir"))cmd_md(arg);
    else if(!strcasecmp(c,"rd")||!strcasecmp(c,"rmdir"))cmd_rd(arg);
    else if(!strcasecmp(c,"del")||!strcasecmp(c,"erase")||!strcasecmp(c,"rm"))cmd_del(arg);
    else if(!strcasecmp(c,"ren")||!strcasecmp(c,"rename"))cmd_ren(arg);
    else if(!strcasecmp(c,"copy")||!strcasecmp(c,"cp"))cmd_copy(arg);
    else if(!strcasecmp(c,"set")||!strcasecmp(c,"setenv"))cmd_set(arg);
    /* ★ 仙剑启动器 */
    else if(!strcasecmp(c,"pal")||!strcasecmp(c,"xiuxian")||!strcasecmp(c,"sdlpal"))cmd_pal();
    else if(!strcasecmp(c,"type")||!strcasecmp(c,"cat"))cmd_type(arg);
    else if(!strcasecmp(c,"ipconfig")||!strcasecmp(c,"ip"))cmd_ipconfig();
    else if(!strcasecmp(c,"ping"))cmd_ping(arg);
    else if(!strcasecmp(c,"netstat"))cmd_netstat();
    else if(!strcasecmp(c,"echo"))cmd_echo(arg);
    else if(!strcasecmp(c,"shell")||!strcasecmp(c,"exit")){ line_add(" 返回: 按返回键退出终端(先确认)"); }
    else if(!strcasecmp(c,"uptime"))cmd_uptime();
    else if(!strcasecmp(c,"whoami")||!strcasecmp(c,"id"))cmd_whoami();
    else if(!strcasecmp(c,"uname"))cmd_uname();
    else if(!strcasecmp(c,"ps")||!strcasecmp(c,"tasklist"))cmd_ps();
    else if(!strcasecmp(c,"battery")||!strcasecmp(c,"power"))cmd_battery();
    else if(!strcasecmp(c,"calc")||!strcasecmp(c,"expr"))cmd_calc(arg);
    else if(!strcasecmp(c,"random")||!strcasecmp(c,"rand"))cmd_random(arg);
    else if(!strcasecmp(c,"os")||!strcasecmp(c,"switch"))cmd_os(arg);
    else if(!strcasecmp(c,"clear"))cmd_cls();
    else if(!strcasecmp(c,"history")){ line_add(" 暂无命令历史 (练习版)"); }
    else if(!strcasecmp(c,"reboot")||!strcasecmp(c,"shutdown")){ line_add(" 已拦截: 用软关机键关机, 勿直接重启"); }
    else {
        /* .bat 批处理: 直接敲 "文件名.bat" 就跑 (DOS 核心功能, 参考 PyDOS) */
        int cl = (int)strlen(c);
        if (cl > 4 && !strcasecmp(c + cl - 4, ".bat")) {
            char p[TM_CWD_MAX];
            path_resolve(c, p, sizeof(p));
            bat_run(p);
            return true;
        }
        int li = learn_lookup(c);
        if(li>=0){
            int win = (s_os>=2)?1:0;
            if(s_learn[li].plat==0 && win){ line_add(" 当前 Windows 环境, 无此 Linux 命令 (可用 copy/del/tasklist 等)"); return true; }
            if(s_learn[li].plat==1 && !win){ line_add(" 当前 Linux 环境, 无此 Windows 命令 (可用 cp/rm/ps 等)"); return true; }
            line_add(s_learn[li].tip); return true;
        }
        line_add(" 未知命令, 输入 help 查看更多"); return false;
    }
    return true;
}

/* ★ 2026-10-30 命令历史 (8 条):
 *   - 连续重复命令不入库 (避免 "dir dir dir" 把历史刷满);
 *   - 空命令/纯空白不入库;
 *   - 环形覆盖, 满 8 条后淘汰最旧;
 *   - 长度受 TM_LINE_LEN 限制 (与命令栏一致)。 */
#define TM_HIST_N 8
static char s_hist[TM_HIST_N][TM_LINE_LEN];
static int  s_hist_n;      /* 已有条数 */
static int  s_hist_pos;    /* 下一条写入位置 */
static int  s_hist_view;   /* 0=当前输入, >0=回看第 N 条 */

static void hist_push(const char *c)
{
    if (!c) return;
    while (*c == ' ' || *c == '\t') c++;
    if (!*c) return;
    s_hist_view = 0;
    if (s_hist_n > 0) {                       /* 与上一条相同则跳过 */
        int last = (s_hist_pos - 1 + TM_HIST_N) % TM_HIST_N;
        if (strcmp(s_hist[last], c) == 0) return;
    }
    snprintf(s_hist[s_hist_pos], TM_LINE_LEN, "%s", c);
    s_hist_pos = (s_hist_pos + 1) % TM_HIST_N;
    if (s_hist_n < TM_HIST_N) s_hist_n++;
}

/* ★ 复核修复: 回看历史前先存"正在输入但还没提交"的草稿。
 *   否则用户打了一半按 ↑, 内容被历史覆盖, 再按 ↓ 只能回到空行 —— 输入丢失。 */
static char s_hist_draft[TM_LINE_LEN];
static bool s_hist_draft_ok = false;

/* 回看上一条; 返回 false 表示没有更早的历史 */
static bool hist_prev(char *out)
{
    if (s_hist_n == 0 || s_hist_view >= s_hist_n) return false;
    if (s_hist_view == 0) {                       /* 首次上翻: 存草稿 */
        snprintf(s_hist_draft, sizeof(s_hist_draft), "%s", out);
        s_hist_draft_ok = true;
    }
    s_hist_view++;
    int idx = (s_hist_pos - s_hist_view + TM_HIST_N) % TM_HIST_N;
    snprintf(out, TM_LINE_LEN, "%s", s_hist[idx]);
    return true;
}

/* 回看下一条; 回到最新时还原草稿 */
static bool hist_next(char *out)
{
    if (s_hist_view <= 0) return false;
    s_hist_view--;
    if (s_hist_view == 0) {
        if (s_hist_draft_ok) { snprintf(out, TM_LINE_LEN, "%s", s_hist_draft); s_hist_draft_ok = false; }
        else out[0] = 0;
        return true;
    }
    int idx = (s_hist_pos - s_hist_view + TM_HIST_N) % TM_HIST_N;
    snprintf(out, TM_LINE_LEN, "%s", s_hist[idx]);
    return true;
}

static void exec_cmd(char*line){
    hist_push(line);
    s_hist_draft_ok = false;      /* 提交后草稿作废, 下次上翻重新记录 */            /* ★ 先入库再执行 (与 shell 行为一致) */
    s_scroll=0.0f; s_scrv=0.0f;
    char d[TM_LINE_LEN]; snprintf(d,TM_LINE_LEN,"> %.*s",TM_LINE_LEN-3,line); line_add(d);
    char*p=line; while(*p==' '||*p=='\t')p++;
    if(!*p){ line_add(""); return; }
    bool ok=true;
    while(*p){
        while(*p==' '||*p=='\t')p++;
        if(!*p)break;
        char*end=p; int m=0;
        while(*end){
            if(end[0]=='&'&&end[1]=='&'){m=1;break;}
            if(end[0]=='|'&&end[1]=='|'){m=2;break;}
            if(*end==';'){m=3;break;}
            end++;
        }
        char save=0; char*after=end;
        if(m){ save=*end; *end=0; after=(m==1||m==2)?end+2:end+1; }
        bool run = (m==1)? ok : (m==2)? !ok : true;
        if(run) ok = exec_one(p);
        if(m){ *end=save; p=after; } else break;
        if(m==1&&!ok) break;
        if(m==2&&ok)   break;
    }
}

/* ============ 52 键交互 ============ */
static void key_input(int i){
    if(!s_ready||i<0)return;
    char c=kbd_char(&g_kbd,i);
    switch(kbd_press(&g_kbd,i)){
        case KBD_ACT_CHAR:{ if(s_help_active) s_help_active=false; s_scroll=0.0f; s_scrv=0.0f; if(c&&s_ci<TM_LINE_LEN-2){ memmove(s_cmd+s_cur+1,s_cmd+s_cur,s_ci-s_cur+1); s_cmd[s_cur]=c; s_ci++; s_cur++; } break; }
        case KBD_ACT_ENTER:{ if(s_help_active) s_help_active=false; s_scroll=0.0f; s_scrv=0.0f; exec_cmd(s_cmd); s_ci=0; s_cur=0; s_cmd[0]=0; break; }
        case KBD_ACT_BKSP:{ if(s_help_active) s_help_active=false; s_scroll=0.0f; s_scrv=0.0f; if(s_cur>0){ memmove(s_cmd+s_cur-1,s_cmd+s_cur,s_ci-s_cur+1); s_cur--; s_ci--; } break; }
        case KBD_ACT_SPACE:{ if(s_help_active) s_help_active=false; s_scroll=0.0f; s_scrv=0.0f; if(s_ci<TM_LINE_LEN-2){ memmove(s_cmd+s_cur+1,s_cmd+s_cur,s_ci-s_cur+1); s_cmd[s_cur]=' '; s_ci++; s_cur++; } break; }
        case KBD_ACT_ESC:{ if(s_help_active) s_help_active=false; s_scroll=0.0f; s_scrv=0.0f; s_ci=0; s_cur=0; s_cmd[0]=0; break; }
        case KBD_ACT_TAB:{ if(s_help_active) s_help_active=false; s_scroll=0.0f; s_scrv=0.0f; for(int b=0;b<TM_ALLCMD_N;b++) if(strncmp(s_cmd,s_allcmds[b],s_ci)==0){strcpy(s_cmd,s_allcmds[b]);s_ci=strlen(s_cmd);s_cur=s_ci;break;} break; }
        case KBD_ACT_UP:
            if(s_help_active){ s_hscroll-=HM_ROW; if(s_hscroll<0)s_hscroll=0; s_hscrv=0; }
            else if(hist_prev(s_cmd)){ s_ci=(int)strlen(s_cmd); s_cur=s_ci; }   /* 历史回看 */
            break;
        case KBD_ACT_DOWN:
            if(s_help_active){ s_hscroll+=HM_ROW; int mx=help_maxs(); if(s_hscroll>(float)mx)s_hscroll=(float)mx; s_hscrv=0; }
            else if(hist_next(s_cmd)){ s_ci=(int)strlen(s_cmd); s_cur=s_ci; }   /* 历史前翻 */
            break;
        case KBD_ACT_LEFT:{ if(s_cur>0)s_cur--; break; }
        case KBD_ACT_RIGHT:{ if(s_cur<s_ci)s_cur++; break; }
        default: break;
    }
}

/* ============ 命令栏 ============ */
static void draw_bar(st7305_handle_t*l){
    pf(l,0,TM_BAR_Y-1,TM_W-1,TM_BAR_Y+TM_BAR_H,0);
    pf(l,0,TM_BAR_Y-1,TM_W-1,TM_BAR_Y-1,1);
    dascii(l,2,TM_BAR_Y+6,"<",1);
    int cx=4;
    for(int i=0;i<TM_BAR_N;i++){
        int w=asw(s_bar[i])+8; int bx=cx-s_bar_ofs;
        if(bx+w>4 && bx<TM_W){
            pf(l,bx,TM_BAR_Y+2,bx+w-1,TM_BAR_Y+TM_BAR_H-1,1);
            dascii(l,bx+4,TM_BAR_Y+6,s_bar[i],0);
        }
        cx+=w; }
    dascii(l,TM_W-8,TM_BAR_Y+6,">",1);
}
static int hit_bar(int sx,int sy){
    if(sy<TM_BAR_Y||sy>TM_BAR_Y+TM_BAR_H)return -1;
    if(sx<=12) return -2;
    if(sx>=TM_W-8) return -3;
    int cx=4;
    for(int i=0;i<TM_BAR_N;i++){
        int w=asw(s_bar[i])+8; int bx=cx-s_bar_ofs;
        if(sx>=bx && sx<bx+w) return i;
        cx+=w;
    }
    return -1;
}
static int bar_maxofs(void){ int tot=0; for(int i=0;i<TM_BAR_N;i++) tot+=asw(s_bar[i])+8; int m=tot-TM_W+12; return m<0?0:m; }

/* ============ 模块接口 ============ */
static void term_reset(ui_ctx_t *ctx) {
    gesture_back_enable(false);    /* 全屏终端屏蔽上滑退出, 仅返回键触发确认 */
    g_kbd = kbd_std; g_kbd.caps=false; g_kbd.shift=false; g_kbd.fn_mode=0;
    s_ci=0; s_cur=0; s_press=-1; s_bar_ofs=0; s_nlines=0; s_line_ovf=false;   /* 重进页面清溢出标记 */
    for(int i=0;i<TM_MAXLINES;i++)line_set(i,"");
    line_add(" 输入 help 查看命令");
    ctx->needs_redraw=true;
}
static void term_clear(void){
    s_ci=0; s_cur=0; s_press=-1; s_bar_ofs=0; s_nlines=0; s_line_ovf=false;   /* 重进页面清溢出标记 */
    for(int i=0;i<TM_MAXLINES;i++) line_set(i,"");
    g_kbd = kbd_std; g_kbd.caps=false; g_kbd.shift=false; g_kbd.fn_mode=0;
}
static void term_exit(ui_ctx_t *ctx) {
    (void)ctx;
    gesture_back_enable(true);
    term_clear();
    /* ★ 2026-10-24 规范化: .bat 整文件缓冲(6KB PSRAM)原来"常驻复用", 退出不还 ——
     *   违反 linit "退出就释放" 铁律。这里退出即 free, 下次进终端第一句 .bat 重分配。 */
    if (s_bat_buf) { free(s_bat_buf); s_bat_buf = NULL; }
}

static void p_term_render(ui_ctx_t *ctx) {
    st7305_handle_t*l=ctx->lcd;
    /* ★ 游戏模式: 整屏交给 SDLPal，终端一行都不画（否则清屏会把游戏画面擦掉） */
    if(s_pal_mode){ pal_tick(l); return; }
    st7305_clear(l,ST7305_COLOR_WHITE);
    if(s_help_active){
        /* ★ 2026-10-21 linit: 帮助也改成"游戏列表那种滑动" —— 像素级跟手滚动,
         *   不再按页翻。s_hscroll = 距顶部的像素量, 渲染只做平移 + 底界裁剪。 */
        const help_item *hl = help_list();
        int hc = help_count();
        if(hc <= 0){ s_help_active=false; ctx->needs_redraw=true; return; }
        const int colw = TM_W/2;
        const int cmdw = 76;
        int xH = TM_OUT_H - GB_H;
        int mx = help_maxs();
        if(s_hscroll > (float)mx) s_hscroll = (float)mx;
        if(s_hscroll < 0.0f)      s_hscroll = 0.0f;
        int first = (int)s_hscroll / HM_ROW;
        int off   = (int)s_hscroll % HM_ROW;
        int rows  = (hc + 1) / 2;
        s_clip_bot = xH;                       /* 内容不得压到输入行/命令条 */
        for(int r = first; r < rows; r++){
            int y = 2 + (r - first) * HM_ROW - off;
            if(y >= xH) break;
            for(int col = 0; col < 2; col++){
                int i = r*2 + col;
                if(i >= hc) break;
                int x = 2 + col*colw;
                dascii(l, x, y, hl[i].c, 0);
                dline_clip(l, x + cmdw, y, hl[i].d, colw - cmdw - 6);
            }
        }
        s_clip_bot = 0;
        draw_bar(l);
        if(g_kbd.n<=0){ g_kbd=kbd_std; g_kbd.caps=false; g_kbd.shift=false; g_kbd.fn_mode=0; }
        kbd_draw(l,TM_KB_Y,&g_kbd,s_press);
        return;
    }
    int xH = TM_OUT_H - GB_H;
    /* ★ 2026-10-30: 输出已滚出可视缓冲时, 右上角画一个 "↑" 提示,
     *   让用户知道上面还有更早的内容被丢弃 (原来是静默丢失)。 */
    if (s_line_ovf && s_nlines > 0) {
        dascii(l, TM_W - 10, 2, "^", 0);
    }
    int totalpx = s_nlines*HM_ROW;
    int maxS = totalpx > xH ? totalpx - xH : 0;
    if(s_scroll > maxS) s_scroll = maxS;
    if(s_scroll < 0)    s_scroll = 0;
    int frac = (int)s_scroll % HM_ROW;
    int idx  = s_nlines - 1 - ((int)s_scroll)/HM_ROW; if(idx<0) idx=0;
    /* ★★ 2026-10-21 linit: 原来写的是 base - frac, 每跨过一个行边界 idx 变一次、
     *   yy 却跳回 base ⇒ 每滚 16px 画面回跳 31px, 看起来"一格一格跳"。
     *   正确公式 y(i) = base + s + (i-(n-1))*H 化简后就是 base + frac:
     *   手指往下拖多少, 内容就往下走多少 (1:1 跟手), 不再回跳。 */
    int yy   = (xH-HM_ROW) + frac;
    s_clip_bot = xH;
    for(int i=idx; i>=0 && yy>=0; i--){ dline(l,2,yy,s_buf[i],0); yy-=line_h(s_buf[i]); }
    s_clip_bot = 0;
    uint32_t tnow = xTaskGetTickCount()*portTICK_PERIOD_MS;
    bool on = ((tnow/500)&1)!=0;
    int in_y = TM_OUT_H - GB_H;
    const char*pr = os_prompt();
    dascii(l,4,in_y,pr,0);
    int cx = 4 + asw(pr);
    dascii(l,cx,in_y,s_cmd,0);
    cx += asw_n(s_cmd,s_cur);
    if(on){ pf(l, cx, in_y, cx+GB_W-1, in_y+GB_H-1, 0); }
    draw_bar(l);
    if(g_kbd.n<=0){ g_kbd=kbd_std; g_kbd.caps=false; g_kbd.shift=false; g_kbd.fn_mode=0; }
    kbd_draw(l,TM_KB_Y,&g_kbd,s_press);
}

static bool p_term_touch(ui_ctx_t *ctx, int x, int y) {
    (void)ctx; (void)x; (void)y;
    return true;
}

static void p_term_action(ui_ctx_t *ctx, os_action_t a) {
    /* ★ 仙剑模式：物理按键全部转成游戏键，不再走终端逻辑 */
    if (s_pal_mode) {
        switch (a) {
            case OS_ACTION_BACK:    pal_release(); pal_stop(); break;
            case OS_ACTION_UP:      pal_pulse(PAL_KEY_UP);    break;
            case OS_ACTION_DOWN:    pal_pulse(PAL_KEY_DOWN);  break;
            case OS_ACTION_LEFT:    pal_pulse(PAL_KEY_LEFT);  break;
            case OS_ACTION_RIGHT:   pal_pulse(PAL_KEY_RIGHT); break;
            case OS_ACTION_CONFIRM: pal_pulse(PAL_KEY_OK);    break;
            case OS_ACTION_HOME:    pal_pulse(PAL_KEY_MENU);  break;
            default: break;
        }
        return;
    }
    switch (a) {
        case OS_ACTION_BACK:
            /* ★ 游戏模式: 返回键先请求退出仙剑（游戏任务收到 SDL_QUIT 后自行收尾），
             *   退出后 poll 检测到 not running 自动切回终端。 */
            if (s_pal_mode) { pal_stop(); break; }
            /* 终端为应用页: 按硬件返回键直接退出 (不弹"退出程序?"确认框, 与白板一致).
             * Task F 已将 os_core 全屏 BACK 门控收窄为仅游戏态拦截, 应用需自行处理退出. */
            os_pop(ctx);
            break;
        default:
            ctx->needs_redraw = true;
            break;
    }
}

static void p_term_poll(ui_ctx_t *ctx) {
    /* ★ 游戏模式: 只驱动游戏上屏，终端自己的滚动/惯性全部暂停 */
    if (s_pal_mode) {
        if (!pal_is_running()) {          /* 游戏已退出（用户按返回 / 出错）⇒ 切回终端 */
            s_pal_mode = false;
            pal_keys_enable(false);
            pal_release();
            ctx->needs_redraw = true;
            line_add(" 已退出仙剑");
        } else {
            /* ---- 系统虚拟按键（GBC 同款）：多点轮询 -> joypad 位 -> 游戏键 ---- */
            tp_point_t pts[TP_MAX_POINTS];
            int np = 0;
            input_get_touch_multi(pts, &np);
            bool d0 = (np >= 1) && pts[0].pressed;
            bool d1 = (np >= 2) && pts[1].pressed;
            uint8_t jp = virtual_keys_poll_multi(d0 ? pts[0].x : 0, d0 ? pts[0].y : 0, d0,
                                                 d1 ? pts[1].x : 0, d1 ? pts[1].y : 0, d1);
            pal_sync_joypad(jp);

            /* 物理按键是脉冲式：到点自动松开，否则游戏会以为一直按着 */
            if (s_pal_key != PAL_KEY_NONE && --s_pal_hold <= 0) pal_release();

            pal_tick(ctx->lcd);
        }
        return;
    }
    /* ---- ★★★ 2026-10-24 统一滚动: 三块区域全部由 ui_scroll 推进
     *   (惯性 ×0.86 / 越界回弹 ×0.8 / 行吸附 —— 全项目同一套手感)。
     *   本文件不再自己算 s_scrv/s_hscrv/s_bar_v, 每帧从 ui_scroll 写回镜像量。
     *   输出区与帮助列表用 LIST(行高 HM_ROW) 保留"松手吸附整行"; 命令栏横向用 HORIZ+PIXEL。 */
    {
        const int HM = HM_ROW;
        int xH  = TM_OUT_H - GB_H;
        int tot = s_nlines * HM;
        int helpmx = help_maxs();
        int barmx  = bar_maxofs();

        if (!s_sc_ready) {
            ui_scroll_config(&s_sc_out,  UI_SCROLL_LIST,  0);
            ui_scroll_config(&s_sc_help, UI_SCROLL_LIST,  0);
            ui_scroll_config(&s_sc_bar,  UI_SCROLL_PIXEL, UI_SCROLL_F_HORIZ);
            s_sc_ready = true;
        }
        /* content = 最大偏移 + 可视长 (ui_scroll 内部 max = content - view) */
        if (s_sc_out_cap  != tot    || s_sc_out_cap  < 0) { ui_scroll_init(&s_sc_out,  HM, xH, tot + xH);      s_sc_out_cap  = tot; }
        if (s_sc_help_cap != helpmx || s_sc_help_cap < 0) { ui_scroll_init(&s_sc_help, HM, xH, helpmx + xH);    s_sc_help_cap = helpmx; }
        if (s_sc_bar_cap  != barmx  || s_sc_bar_cap  < 0) { ui_scroll_init(&s_sc_bar,  1,  xH, barmx + xH);     s_sc_bar_cap  = barmx; }

        if (ui_scroll_poll(&s_sc_out))  ctx->needs_redraw = true;
        if (ui_scroll_poll(&s_sc_help)) ctx->needs_redraw = true;
        if (ui_scroll_poll(&s_sc_bar))  ctx->needs_redraw = true;

        /* ★ 2026-10-30 修「上下拖动方向反了」:
         *   ui_scroll 的 offset 语义是**从顶部算** (0=顶, max=底);
         *   而本文件渲染把 s_scroll 当**从底部算**用 (0=最新一行在底部, 变大=看更早)。
         *   两者方向相反 → 手指往上滑, 画面却往下走。这里翻转成"从底部算"。
         *   翻转量 = ui_scroll 内部的 max = content - view = (tot+xH) - xH = tot。 */
        {
            int span = s_nlines * HM_ROW;
            s_scroll = (float)(span - ui_scroll_offset(&s_sc_out));
        }
        s_hscroll = (float)ui_scroll_offset(&s_sc_help);
        s_bar_ofs = ui_scroll_offset(&s_sc_bar);
        s_scrv = 0.0f; s_hscrv = 0.0f; s_bar_v = 0.0f;
    }
    int tx,ty; bool down=input_get_touch_pos(&tx,&ty);
    static bool d=false, dbar=false, moved=false;
    static int  dbar_lx=0, dbar_dist __attribute__((unused))=0, dbar_bi=-1;
    if(!down){
        if(d){
            if(s_dragout){
                s_dragout=false;
                ui_scroll_touch_xy(&s_sc_out,  0, 0, false);
                ui_scroll_touch_xy(&s_sc_help, 0, 0, false);
            }
            else if(dbar){
                if(!moved){
                    if(dbar_bi==-2){ s_bar_ofs-=40; if(s_bar_ofs<0)s_bar_ofs=0; }
                    else if(dbar_bi==-3){ s_bar_ofs+=40; if(s_bar_ofs>bar_maxofs())s_bar_ofs=bar_maxofs(); }
                    else if(dbar_bi>=0){ char tmp[16]; strncpy(tmp,s_bar[dbar_bi],15); tmp[15]=0;
                        strncpy(s_cmd,tmp,TM_LINE_LEN-1); s_cmd[TM_LINE_LEN-1]=0; s_ci=strlen(s_cmd); s_cur=s_ci;
                        exec_cmd(s_cmd); s_ci=0; s_cur=0; s_cmd[0]=0; }
                }
                dbar=false;
                ui_scroll_touch_xy(&s_sc_bar, 0, 0, false);
            }
            s_press=-1; ctx->needs_redraw=true;
        }
        d=false; return;
    }
    if(!d){
        d=true;
        if(ty < TM_BAR_Y-1){
            s_dragout=true; s_drag_y0=ty; s_drag_ty=ty;
            s_drag_sc0 = s_help_active ? s_hscroll : s_scroll;
            s_scrv=0; s_hscrv=0;
            return;
        }
        if(ty>=TM_BAR_Y && ty<=TM_BAR_Y+TM_BAR_H){ s_bar_ofs>bar_maxofs()?s_bar_ofs=bar_maxofs():0; dbar=true; moved=false; dbar_lx=tx; dbar_dist=0; dbar_bi=hit_bar(tx,ty);
            ui_scroll_touch_xy(&s_sc_bar, tx, ty, true); return; }
        int ki=kbd_hit(&g_kbd,TM_KB_Y,tx,ty);
        if(ki>=0){ s_press=ki; key_input(ki); ctx->needs_redraw=true; }
        return;
    }
    if(s_dragout){
        /* ★ 2026-10-24 统一滚动: 1:1 跟手交给 ui_scroll（越界可拉出、松手惯性+回弹）*/
        if (s_help_active) {
            if (ui_scroll_touch_xy(&s_sc_help, tx, ty, true)) ctx->needs_redraw = true;
        } else {
            if (ui_scroll_touch_xy(&s_sc_out,  tx, ty, true)) ctx->needs_redraw = true;
        }
        return;
    }
    if(dbar){
        /* ★ 2026-10-24 统一滚动: 命令栏横向也用 ui_scroll（HORIZ + PIXEL）*/
        int old = s_bar_ofs;
        if (ui_scroll_touch_xy(&s_sc_bar, tx, ty, true)) ctx->needs_redraw = true;
        s_bar_ofs = ui_scroll_offset(&s_sc_bar);
        if (s_bar_ofs != old) ctx->needs_redraw = true;
        if (tx - dbar_lx > 8 || dbar_lx - tx > 8) moved = true;
        dbar_lx = tx;
    }
}

static const os_module_t s_mod_term = {
    .name       = "term",
    .page_id    = OS_PAGE_TERMINAL,
    .on_enter   = term_reset,
    .on_exit    = term_exit,
    .render     = p_term_render,
    .action     = p_term_action,
    .touch      = p_term_touch,
    .poll       = p_term_poll,
    .fullscreen = true,   /* 隐藏状态栏; 应用页, 按返回直接退出 (不弹确认框) */
    .exit_confirm = true,   /* 返回键先弹"退出程序?"确认 (2026-10-14 全局规范) */
    .swipe_back_disabled = true,  /* 全屏终端屏蔽上滑退出, 仅返回键触发确认 (统一由内核设置) */
};

void os_page_term_register(void) { os_register(&s_mod_term); }
