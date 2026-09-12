/**
 * page_term.c — 终端 页面模块 (P4 迁移: 从 menu/terminal.c 改签名为 ui_ctx + 私有 state).
 *
 * 命令行 + 52 键虚拟键盘:
 *   - 上半输出区 (触摸平滑滚动回看历史), 中部命令栏(可左右滑动), 下半 52 键 QWERTY
 *   - 支持多系统模拟 (uos/kylin/dos/powershell) + 帮助分页
 *   - 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "sd_scan.h"
#include "font_zh.h"
#include "font_zh16.h"
#include "keyboard.h"
#include "board_battery.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>

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
EXT_RAM_BSS_ATTR static char  s_buf[TM_MAXLINES][TM_LINE_LEN];
EXT_RAM_BSS_ATTR static char  s_cmd[TM_LINE_LEN];
#define LN(i) (s_buf[(i)])
static bool  s_ready = true;
static int   s_nlines = 0;
static float s_scroll = 0.0f;
static float s_scrv   = 0.0f;
static bool  s_dragout = false;
static int   s_drag_y0 = 0;
static int   s_drag_ty = 0;
static float s_drag_sc0 = 0.0f;
static int   s_ci = 0;
static int   s_cur = 0;
static kbd_t g_kbd;
static int   s_press = -1;
static int   s_bar_ofs = 0;

/* 命令栏 */
static const char *const s_bar[] = {
    "help","cls","ver","mem","sys","date","echo","dir","type","ipconfig",
    "ping","netstat","shell","uptime","whoami","uname","ps","battery",
    "calc","random","clear","free","history","reboot"
};
#define TM_BAR_N (sizeof(s_bar)/sizeof(s_bar[0]))

static const char *const s_allcmds[] = {
    "help","cls","ver","mem","sys","date","time","echo","dir","ls",
    "type","cat","ipconfig","ip","ping","netstat","shell","exit",
    "uptime","whoami","id","uname","ps","tasklist","battery","power",
    "calc","expr","random","rand","free","clear","history","reboot","shutdown",
    "os","switch"
};
#define TM_ALLCMD_N (sizeof(s_allcmds)/sizeof(s_allcmds[0]))

/* ============ 文本绘制 ============ */
#define GB_W 8
#define GB_H 16
static void px(st7305_handle_t*l,int x,int y,int c){ if(x<0||x>=TM_W||y<0||y>=TM_H)return; st7305_draw_pixel(l,x,y,c); }
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
static void line_add(const char*s){ if(!s_ready)return;
    if(s_nlines>=TM_MAXLINES){ for(int r=0;r<TM_MAXLINES-1;r++)memcpy(LN(r),LN(r+1),TM_LINE_LEN); line_set(TM_MAXLINES-1,s); }
    else { line_set(s_nlines,s); s_nlines++; } }

/* ============ 命令实现 ============ */
#define HELP_PER 12
static bool s_help_active = false;
static int  s_help_page = 0;
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

static void cmd_help(void){ s_help_active=true; s_help_page=0; }
static void cmd_cls(void){ s_nlines=0; for(int i=0;i<TM_MAXLINES;i++)line_set(i,""); }
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
static void cmd_dir(const char*arg){ const char*p=(arg&&arg[0])?arg:"/sdcard"; char b[64];
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
static void cmd_ping(const char*h){ if(!h||!h[0]){line_add(" ping <地址>");return;} line_add(" ping ... (ICMP 待接入, 当前为练习模拟)"); }
static void cmd_netstat(void){ line_add(" 连接状态: (TCP 会话待接入)"); line_add("  本机已联网: 见 ipconfig"); }
static void cmd_echo(const char*a){ line_add((a&&a[0])?a:" "); }
static void cmd_uptime(void){
    int64_t us = esp_timer_get_time();
    int64_t s = us / 1000000LL; if (s < 0) s = 0;
    char b[64]; snprintf(b,64," 已运行: %lld天 %02lld:%02lld:%02lld",
                         (long long)(s/86400), (long long)((s%86400)/3600),
                         (long long)((s%3600)/60), (long long)(s%60)); line_add(b);
}
static void cmd_whoami(void){ line_add(" root  (LinTOS 系统管理员, 模拟返回)"); }
static void cmd_uname(void){ line_add(" LinTOS v1.3"); line_add(" 内核: FreeRTOS @ ESP32-S3 ESP-IDF"); }
static void cmd_ps(void){
    line_add(" PID   名称              占用");
    line_add(" 1010  bt/wifi 服务      24K");
    line_add(" 1011  菜单系统          96K");
    line_add(" 1012  touch 输入        8K");
    line_add(" 1013  audio 音频        32K");
    line_add(" (模拟进程表, 仅供练习)");
}
static void cmd_battery(void){
    board_battery_status_t st = {0};
    unsigned pct = 0;
    if (board_battery_read(&st) == ESP_OK) pct = st.percent;
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
static int help_pages(void){
    switch(s_os){ case 1:return KYLIN_PAGES; case 2:return DOS_PAGES; case 3:return PS_PAGES; default:return UOS_PAGES; }
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

static void exec_cmd(char*line){
    s_scroll=0.0f; s_scrv=0.0f;
    char d[TM_LINE_LEN]; snprintf(d,TM_LINE_LEN,"> %s",line); line_add(d);
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
        case KBD_ACT_UP:  if(s_help_active){ int hp=help_pages(); s_help_page=(s_help_page+hp-1)%hp; } break;
        case KBD_ACT_DOWN:if(s_help_active){ int hp=help_pages(); s_help_page=(s_help_page+1)%hp; } break;
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
    input_set_swipe_back(false);   /* 全屏终端屏蔽上滑退出, 仅返回键触发确认 */
    g_kbd = kbd_std; g_kbd.caps=false; g_kbd.shift=false; g_kbd.fn_mode=0;
    s_ci=0; s_cur=0; s_press=-1; s_bar_ofs=0; s_nlines=0;
    for(int i=0;i<TM_MAXLINES;i++)line_set(i,"");
    line_add(" 输入 help 查看命令");
    ctx->needs_redraw=true;
}
static void term_clear(void){
    s_ci=0; s_cur=0; s_press=-1; s_bar_ofs=0; s_nlines=0;
    for(int i=0;i<TM_MAXLINES;i++) line_set(i,"");
    g_kbd = kbd_std; g_kbd.caps=false; g_kbd.shift=false; g_kbd.fn_mode=0;
}
static void term_exit(ui_ctx_t *ctx) {
    (void)ctx;
    input_set_swipe_back(true);
    term_clear();
}

static void p_term_render(ui_ctx_t *ctx) {
    st7305_handle_t*l=ctx->lcd;
    st7305_clear(l,ST7305_COLOR_WHITE);
    if(s_help_active){
        const help_item *hl = help_list();
        int hc = help_count(), hp = help_pages();
        if(s_help_page>=hp) s_help_page=0;
        const int colw = TM_W/2;
        const int cmdw = 76;
        const int rowh = HM_ROW;
        int start = s_help_page*HELP_PER;
        int n = hc-start; if(n>HELP_PER)n=HELP_PER;
        for(int i=0;i<n;i++){
            int col=i%2, row=i/2;
            int x=2+col*colw, y=2+row*rowh;
            const help_item *it=&hl[start+i];
            dascii(l,x,y,it->c,0);
            dline_clip(l,x+cmdw,y,it->d,colw-cmdw-6);
        }
        int rows=(n+1)/2;
        char pg[64]; snprintf(pg,sizeof(pg),"-- %d/%d 页 / 上下滑翻页 --", s_help_page+1, hp);
        dline(l,2,2+rows*rowh+2,pg,0);
        draw_bar(l);
        if(g_kbd.n<=0){ g_kbd=kbd_std; g_kbd.caps=false; g_kbd.shift=false; g_kbd.fn_mode=0; }
        kbd_draw(l,TM_KB_Y,&g_kbd,s_press);
        return;
    }
    int xH = TM_OUT_H - GB_H;
    int totalpx = s_nlines*HM_ROW;
    int maxS = totalpx > xH ? totalpx - xH : 0;
    if(s_scroll > maxS) s_scroll = maxS;
    if(s_scroll < 0)    s_scroll = 0;
    int frac = (int)s_scroll % HM_ROW;
    int idx  = s_nlines - 1 - ((int)s_scroll)/HM_ROW; if(idx<0) idx=0;
    int yy   = (xH-HM_ROW) - frac;
    for(int i=idx; i>=0 && yy>=0; i--){ dline(l,2,yy,s_buf[i],0); yy-=line_h(s_buf[i]); }
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
    switch (a) {
        case OS_ACTION_BACK:
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
    uint32_t tw = xTaskGetTickCount()*portTICK_PERIOD_MS;
    static uint32_t tw_last = 0;
    if((tw/500)!=(tw_last/500)){ tw_last=tw; ctx->needs_redraw=true; }
    if(!s_dragout && s_scrv!=0.0f){
        s_scroll += s_scrv; s_scrv *= 0.85f;
        if(s_scrv>-0.6f && s_scrv<0.6f) s_scrv=0.0f;
        int xH=TM_OUT_H-GB_H; int tot=s_nlines*HM_ROW; int mx=tot>xH?tot-xH:0;
        if(s_scroll>mx){ s_scroll=(float)mx; s_scrv=0.0f; }
        if(s_scroll<0){ s_scroll=0.0f; s_scrv=0.0f; ctx->needs_redraw=true; }
        ctx->needs_redraw=true;
    }
    int tx,ty; bool down=input_get_touch_pos(&tx,&ty);
    static bool d=false, dbar=false, moved=false;
    static int  dbar_lx=0, dbar_dist=0, dbar_bi=-1;
    if(!down){
        if(d){
            if(s_dragout){ s_dragout=false; }
            else if(dbar){
                if(!moved){
                    if(dbar_bi==-2){ s_bar_ofs-=40; if(s_bar_ofs<0)s_bar_ofs=0; }
                    else if(dbar_bi==-3){ s_bar_ofs+=40; if(s_bar_ofs>bar_maxofs())s_bar_ofs=bar_maxofs(); }
                    else if(dbar_bi>=0){ char tmp[16]; strncpy(tmp,s_bar[dbar_bi],15); tmp[15]=0;
                        strncpy(s_cmd,tmp,TM_LINE_LEN-1); s_cmd[TM_LINE_LEN-1]=0; s_ci=strlen(s_cmd); s_cur=s_ci;
                        exec_cmd(s_cmd); s_ci=0; s_cur=0; s_cmd[0]=0; }
                }
                dbar=false;
            }
            s_press=-1; ctx->needs_redraw=true;
        }
        d=false; return;
    }
    if(!d){
        d=true;
        if(ty < TM_BAR_Y-1){
            s_dragout=true; s_drag_y0=ty; s_drag_ty=ty;
            s_drag_sc0=s_scroll; s_scrv=0;
            return;
        }
        if(ty>=TM_BAR_Y && ty<=TM_BAR_Y+TM_BAR_H){ s_bar_ofs>bar_maxofs()?s_bar_ofs=bar_maxofs():0; dbar=true; moved=false; dbar_lx=tx; dbar_dist=0; dbar_bi=hit_bar(tx,ty); return; }
        int ki=kbd_hit(&g_kbd,TM_KB_Y,tx,ty);
        if(ki>=0){ s_press=ki; key_input(ki); ctx->needs_redraw=true; }
        return;
    }
    if(s_dragout){
        int dy = ty - s_drag_ty; s_drag_ty = ty;
        if(s_help_active){
            int dy0 = ty - s_drag_y0; int hp=help_pages();
            if(dy0 <= -HM_ROW*2){ s_help_page=(s_help_page+1)%hp; s_drag_y0=ty; ctx->needs_redraw=true; }
            else if(dy0 >= HM_ROW*2){ s_help_page=(s_help_page+hp-1)%hp; s_drag_y0=ty; ctx->needs_redraw=true; }
        } else {
            s_scroll = s_drag_sc0 - (float)(ty - s_drag_y0);
            s_scrv = (float)(-dy);
            int xH=TM_OUT_H-GB_H; int tot=s_nlines*HM_ROW; int mx=tot>xH?tot-xH:0;
            if(s_scroll>mx) s_scroll=(float)mx;
            if(s_scroll<0)  s_scroll=0.0f;
            ctx->needs_redraw=true;
        }
        return;
    }
    if(dbar){
        int dx=tx-dbar_lx; dbar_lx=tx; if(dx<0)dbar_dist+=-dx; else dbar_dist+=dx;
        int no=s_bar_ofs-dx; if(no<0)no=0; if(no>bar_maxofs())no=bar_maxofs();
        if(no!=s_bar_ofs){ s_bar_ofs=no; ctx->needs_redraw=true; }
        if(dbar_dist>8) moved=true;
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
};

void os_page_term_register(void) { os_register(&s_mod_term); }
