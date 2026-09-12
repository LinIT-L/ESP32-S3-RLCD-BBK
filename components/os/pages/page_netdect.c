/**
 * page_netdect.c — 网络分析 页面模块.
 *
 * 复刻"电脑诊断"的顶部步骤进度条 UI, 分步执行并最终生成网络检测报告:
 *   ①连接网络 ②扫描主机 ③端口服务 ④弱口令/漏洞 ⑤网络信息 ⑥生成报告
 * 能力:
 *   - 主机发现: ICMP 回显扫描局域网段, 在线主机记录 IP; 经 lwip ARP 表取 MAC → macoui 查厂商.
 *   - 端口/服务: 对常见端口 TCP connect, 读 banner 判定服务类型(HTTP/SSH/FTP/Telnet 等).
 *   - 弱口令: 仅对本机局域网的 FTP/Telnet 试极少量常见弱口令 (自检用途, 不爆破不对外).
 *   - 网络信息: 本机 IP/掩码/网关/DNS/接口类型; 联网经 IP 库 API 取 公网IP/ISP/所在地.
 *   - 报告: 汇总逐设备信息可滚动.
 * 后台引擎线程执行扫描, 渲染每帧读取共享状态.
 */
#include "os.h"
#include "os_hw.h"      /* os_hw_request/release (进入即开WiFi, 退出即关) */
#include "ui_common.h"
#include "input.h"
#include "macoui.h"
#include "keyboard.h"
#include "wifi_manager.h"
#include "font_zh.h"
#include "font_zh16.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "ping/ping_sock.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#define NW 400
#define NH 300

/* 顶部步骤条 (电脑诊断同款 40px) */
#define ND_BAR_H 40
#define ND_STEPS 6
#define ND_CONTENT_Y (ND_BAR_H)

#define MAX_HOSTS 32
#define MAX_PORTS 22
#define PROBE_TIMEOUT_MS 180

typedef enum {
    ST_NET,      /* 联网: 本机网络信息 */
    ST_IPINFO,   /* 信息: 本机/公网网络信息, ISP/城市 */
    ST_SCAN,     /* IP:   主机扫描(IP+MAC+厂商) */
    ST_PORT,     /* 端口: 开放端口/服务 */
    ST_CRED,     /* 口令: 弱口令自检 */
    ST_REPORT,   /* 报告 */
    ST_TOTAL
} step_t;

typedef struct { uint16_t port; const char *svc; } ps_t;
static const ps_t s_ports[MAX_PORTS] = {
    {80,"HTTP"},{443,"HTTPS"},{8080,"WEB"},{2333,"MGT"},{53,"DNS"},
    {22,"SSH"},{21,"FTP"},{23,"Telnet"},{445,"SMB"},{3389,"RDP"},
    {110,"POP3"},{143,"IMAP"},{25,"SMTP"},{3306,"MySQL"},{5432,"PgSQL"},
    {6379,"Redis"},{27017,"Mongo"},{5900,"VNC"},{8000,"WEB-A"},{8443,"WEB-S"},
    {9000,"PHP"},{8888,"WEB-B"},
};

typedef struct {
    uint32_t ip; uint8_t mac[6]; bool macok; bool local; char vendor[28]; char hname[24];
    struct { uint16_t port; char svc[10]; char banner[28]; } p[MAX_PORTS];
    int np; bool weak; char weaknote[12];
} host_t;

/* ---- 共享扫描状态 (引擎线程写, 渲染只读) ---- */
static volatile int   s_step = ST_NET;      /* 当前步骤 */
static volatile int   s_prog = 0;           /* 0..100 本步进度 */
static volatile bool  s_done = false;       /* 全部完成 */
static char     s_status[64];               /* 步骤说明 */
static int      s_nhost = 0;
static int      s_hostmax = 254;            /* 扫描末位: 按子网掩码计算的可用主机数上限 */
EXT_RAM_BSS_ATTR static host_t   s_h[MAX_HOSTS];   /* 主机表移 PSRAM, 省核心内部 RAM 14.7KB */
/* 网络信息 */
static char     s_local_ip[16], s_mask[16], s_gw[16], s_dns[16], s_nwtype[16];
static char     s_pub_ip[16], s_isp[16], s_city[40], s_isp_brand[20], s_net_scenario[20];
static char     s_ipv6[16];   /* 是否支持IPv6: "支持"/"不支持" */
static char     s_gw_rtt[12], s_pub_rtt[12];   /* 网关/公网延迟 "12 ms" */
static char     s_essaid[33], s_auth[20], s_localmac[18], s_gwmac[18], s_gwmac_vendor[24], s_gwip[16];
static step_t   s_lastdone = ST_NET;
static TaskHandle_t s_task;
/* 分析任务栈按需分配 (内部RAM, 勿放 PSRAM): 网络操作(联网/取信息/http)会冻结 PSRAM cache,
 * 若任务栈在 PSRAM 会触发 esp_cache_freeze 断言崩溃. HTTPS/TLS 需较大栈, 8/16KB 会溢出.
 * 用 xTaskCreate 动态栈: 只在网络分析运行时占用 32KB, 退出分析即释放, 不再常驻占内存. */
#define ND_TASK_STACK 8192                       /* 32KB (内部RAM, 按需 malloc/free) */
static volatile bool s_stop = false;   /* 退出即停止扫描 (勿在阻塞 socket 上 vTaskDelete) */
static volatile bool s_cancel_step = false; /* 中断当前步骤扫描(保留已采结果, 进暂停态), 由底部「取消」置位, nd_pause 清除 */
static volatile int  s_gen = 0;        /* 扫描代次: 重新进入使在途任务作废 */
static int  s_mine = 0;
static bool s_wait_start = true;       /* 进入时先弹"连接网络"确认再扫描 */
static int  s_prompt_sel = 0;          /* 列表选中行 / 选项 */
static bool s_scan_started=false;      /* 本页是否已发起过 WiFi 扫描 */
static uint32_t s_scan_last=0;         /* 上次(重)扫描时刻, 失败重试节流 */
/* ---- 统一走系统自带连接流程 (settings_wifi_connect_open / wifi_kb 键盘) ---- */
static bool s_use_sys = false;         /* 等待系统连接流程完成中 */
static bool s_prev_conn = false;       /* 上一帧连接态 (由断到连→启动分析) */
static bool s_enter_pending = false;   /* 进入延迟: 先显示本页, 至少1s后再弹联网提示 */
static bool s_start_conn = false;      /* 进入时的联网状态 (决定弹确认框还是直接连接) */
static uint32_t s_enter_t0 = 0;        /* 进入时间戳(ms) */
static uint32_t s_sys_t0 = 0;          /* 等待自动连接开始时间戳(ms) */
#define ND_ENTER_DELAY_MS 1000         /* 进入后延迟 1s 再弹联网提示 */
#define ND_AUTOCONN_TIMEOUT 8000       /* 自动连接超时: 8秒未连上, 或连接已停止 → 开手动选网 */
extern void settings_wifi_connect_open(ui_ctx_t *ctx);   /* 系统自带"连接WiFi"流程 (复用, 统一) */
static int  s_pend_idx = 0;                             /* 待连接(切换)的网络索引 */
/* ---- 连接网络子界面 (0=WiFi列表 1=输密码 2=连接中) ---- */
static int  s_conn   = 0;
static char s_ssid[33]; static char s_pass[33]; static int s_pas;   /* 待连 SSID / 密码 / 长度 */
static int  s_press_k = -1; static bool s_cl_sym=false, s_cl_shift=false;   /* 经典键盘: 符号层/大写 */
static const char *const s_clk[2][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm,." },
    { "1234567890", "!@#$%^&*()", "-_=+[]{};:'", "\\|/?<>\"~" },
};
static uint32_t s_conn_t0 = 0;         /* 开始连接时间戳(ms) */
static uint32_t s_fail_t0 = 0;         /* 连接失败提示起始(ms) */
#define CONNECT_TIMEOUT_MS 20000
static int s_scrollpx = 0;             /* 主机列表像素级滚动偏移(平滑跟手) */
static int s_rptpx = 0;                /* 报告像素级滚动偏移 */
static int s_rpt_nlin = 0;             /* 报告可取总行数 (render 缓存, 供 poll 算可滚范围) */
/* ---- 分步推进 (每步完成暂停, 需按"下一步") ---- */
static volatile bool s_step_done = false;  /* 引擎在某步完成, 等待用户 */
static volatile bool s_step_go  = false;   /* 下一步 → 引擎继续 */
static int  s_view = 0;                    /* 浏览的步骤(上一步/下一步查看) */
static bool s_nav_ovr = false;             /* 执行中用户手动切页覆盖: true=显示s_view, false=显示s_step */
static int nd_view_step(void){ return (s_nav_ovr || s_step_done) ? s_view : s_step; }  /* 当前实际显示页 */
static int nd_vis_count(void);
static int  s_finished = 0;                /* 已完成并暂停的步数 */
static int  s_step_eta = 0;                /* 当前步预计秒数 */
static uint32_t s_step_t0 = 0;             /* 当前步开始时间(ms) */
/* 角按钮 (文字贴最左/最右/最底, 三边距 10px) */
#define NBTN_H 24
#define NBTN_Y0 (NH-10-17)     /* 文字上方 4px (文字已较 10px 下移 3px) */
#define NBTN_Y1 (NH-10+3)      /* 与文字底部平齐 */
#define NBTN_L0 10               /* 距左缘 10px */
#define NBTN_L1 159
#define NBTN_R0 240
#define NBTN_R1 390              /* 距右缘 10px */
/* 恒定内容框 (所有页面通用): 框住中间内容, 下边距底部横线/NBTN 10px */
#define CT_X0 14
#define CT_X1 (NW-14)
#define CT_TOP (ND_BAR_H+6)
#define CT_BOT (NBTN_Y0-10)
/* 联网密码页融合: 框内下半键盘/按钮 */
#define ND_KBD_Y (CT_TOP+66)
#define ND_BTN_Y (CT_BOT-30)
#define ND_BTN_H 24

/* ---- 经典 5 排 16px 键盘 (融合进恒定框下半, 同设置页 WiFi 密码键盘; 英文数字 8×16) ----
 * 键 id: 行0/1 = row*10+col; 行2 = 20+col; 行3 = 29+col(29=Shift/ABC, 38=删除); 行4 = 39+col(39=&123, 40=空格, 41=回车) */
#define NDK_H 24
#define NDK_ROW 26
static void pf(st7305_handle_t *l, int x0, int y0, int x1, int y1, int c);
static int  nd16w(const char *s);
static void nd16(st7305_handle_t *l, int x, int y, const char *s, bool inv);
static void nd_do_connect(void);
static void nd_kbd_geo(int id, int *x, int *y, int *w) {
    const int W = CT_X1 - CT_X0 + 1, x0 = CT_X0 + 4;
    int row, col, kw;
    if (id < 20)      { row = id / 10; col = id % 10; kw = (W - 8) / 10; }
    else if (id < 29) { row = 2; col = id - 20; kw = (W - 8) / 9; }
    else if (id < 39) { row = 3; col = id - 29; kw = (col == 0) ? 60 : (col == 9 ? 49 : 33); }
    else              { row = 4; col = id - 39; kw = (col == 0) ? 54 : (col == 1 ? (W - 8 - 54 - 72) : 72); }
    int xx = x0;
    if (row == 0 || row == 1) xx += col * kw;
    else if (row == 2)        xx += ((W - 8 - 9 * kw) / 2) + col * kw;
    else if (row == 3)        xx += (col == 0) ? 0 : (60 + (col - 1) * 33);
    else                      xx += (col == 0) ? 0 : (col == 1) ? 54 : (54 + (W - 8 - 54 - 72));
    *x = xx; *y = ND_KBD_Y + row * NDK_ROW; *w = kw;
}
static const char *nd_kbd_label(int id) {
    static char b[2];
    const char *const *rows = s_clk[s_cl_sym ? 1 : 0];
    if (id < 29)      { b[0] = (id < 20) ? rows[id / 10][id % 10] : rows[2][id - 20]; b[1] = 0; return b; }
    if (id < 39) {
        int col = id - 29;
        if (col == 0) return s_cl_sym ? "ABC" : (s_cl_shift ? "\xe5\xb0\x8f\xe5\x86\x99" : "\xe5\xa4\xa7\xe5\x86\x99"); /* 小写/大写 */
        if (col == 9) return "\xe5\x88\xa0\xe9\x99\xa4";                                                              /* 删除 */
        b[0] = rows[3][col - 1]; b[1] = 0;
        if (b[0] >= 'a' && b[0] <= 'z' && s_cl_shift && !s_cl_sym) b[0] = (char)(b[0] - 'a' + 'A');
        return b;
    }
    int col = id - 39;
    if (col == 0) return s_cl_sym ? "ABC" : "&123";
    if (col == 1) return "\xe7\xa9\xba\xe6\xa0\xbc"; /* 空格 */
    return "\xe5\x9b\x9e\xe8\xbd\xa6";               /* 回车 */
}
static void nd_kbd_draw(st7305_handle_t *l, int press) {
    for (int id = 0; id < 42; id++) {
        int x, y, w; nd_kbd_geo(id, &x, &y, &w);
        const char *lab = nd_kbd_label(id);
        bool sel = (id == press);
        pf(l, x, y, x + w - 1, y + NDK_H - 1, sel ? 0 : 1);  /* 底: 按下黑/正常白 */
        pf(l, x + w - 1, y, x + w - 1, y + NDK_H - 1, 0);    /* 右分割线 */
        pf(l, x, y + NDK_H - 1, x + w - 1, y + NDK_H - 1, 0);/* 下分割线 */
        int tw = nd16w(lab);
        nd16(l, x + (w - tw) / 2, y + (NDK_H - 16) / 2, lab, sel);
    }
}
static int nd_kbd_hit(int x, int y) {
    if (y < ND_KBD_Y || y >= ND_KBD_Y + 5 * NDK_ROW || x < CT_X0 || x > CT_X1) return -1;
    int row = (y - ND_KBD_Y) / NDK_ROW;
    const int W = CT_X1 - CT_X0 + 1, x0 = CT_X0 + 4;
    if (row == 0 || row == 1) { int kw = (W - 8) / 10, col = (x - x0) / kw; if (col >= 0 && col < 10) return row * 10 + col; }
    else if (row == 2) { int kw = (W - 8) / 9, xx = x0 + (W - 8 - 9 * kw) / 2, col = (x - xx) / kw; if (col >= 0 && col < 9) return 20 + col; }
    else if (row == 3) {
        if (x < x0 + 60) return 29;
        if (x >= x0 + 60 + 8 * 33) return 38;
        int col = (x - (x0 + 60)) / 33; if (col >= 0 && col < 8) return 30 + col;
    } else {
        if (x < x0 + 54) return 39;
        if (x < x0 + 54 + (W - 8 - 54 - 72)) return 40;
        return 41;
    }
    return -1;
}
static void nd_kbd_act(ui_ctx_t *ctx, int id) {
    if (id < 29) {   /* 字符 */
        const char *const *rows = s_clk[s_cl_sym ? 1 : 0];
        char c = (id < 20) ? rows[id / 10][id % 10] : ((id < 29) ? rows[2][id - 20] : rows[3][id - 30]);
        if (s_cl_shift && !s_cl_sym && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c && s_pas < 32) { s_pass[s_pas++] = c; s_pass[s_pas] = 0; }
    } else if (id == 29) { s_cl_shift = !s_cl_shift; }                    /* Shift/ABC */
    else if (id == 38) { if (s_pas > 0) { s_pas--; s_pass[s_pas] = 0; } } /* 删除 */
    else if (id == 39) { s_cl_sym = !s_cl_sym; }                          /* &123/ABC */
    else if (id == 40) { if (s_pas < 32) { s_pass[s_pas++] = ' '; s_pass[s_pas] = 0; } } /* 空格 */
    else if (id == 41) { nd_do_connect(); }                               /* 回车=连接 */
}

/* ---------------- 基础绘制 ---------------- */
static void pf(st7305_handle_t *l, int x0,int y0,int x1,int y1,int c){
    if(x0>x1){int t=x0;x0=x1;x1=t;} if(y0>y1){int t=y0;y0=y1;y1=t;}
    for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
        if(x>=0&&x<NW&&y>=0&&y<NH) st7305_draw_pixel(l,x,y,(st7305_color_t)c);
    }
}
static void bar_job(st7305_handle_t *l, int x, int xmax, int y, int h, int v, int vmax){
    int w = (vmax>0)?((xmax-x)*v/vmax):0; if(w<0)w=0;
    if(w>0) pf(l,x,y,x+w-1,y+h-1,1); else pf(l,x,y,xmax,y+h-1,0);
    pf(l,x,y,xmax,y,0); pf(l,x,y+h,xmax,y+h,0);
}

/* 空心圆 / 实心圆 (照抄电脑诊断的节点画法) */
static void nd_disk(st7305_handle_t *l, int cx, int cy, int r, bool fill){
    for(int dy=-r;dy<=r;dy++) for(int dx=-r;dx<=r;dx++){
        int d2=dx*dx+dy*dy;
        if(fill){ if(d2<=r*r){ int px=cx+dx,py=cy+dy; if(px>=0&&px<NW&&py>=0&&py<NH) st7305_draw_pixel(l,px,py,ST7305_COLOR_BLACK);} }
        else if(d2<=r*r && d2>=(r-1)*(r-1)){ int px=cx+dx,py=cy+dy; if(px>=0&&px<NW&&py>=0&&py<NH) st7305_draw_pixel(l,px,py,ST7305_COLOR_BLACK);}
    }
}

/* ===== 顶部进度条: 与「电脑诊断」1:1 同款 =====
 * 配置: 6 步名 16px(font_zh16 同应用管家), 节点, 进度线 y=DBAR_H-10, 当前步大圆实心. */
/* 16px 文本 (font_zh16 全量应用管家同款): 汉字16×16, ASCII 8×12 */
static int nd16w(const char *s){ int w=0; for(const unsigned char*p=(const unsigned char*)s;*p;){
    if((*p&0xe0)==0xe0){ w+=16; p+=3; } else { w+=8; p++; } } return w; }
static void nd16(st7305_handle_t *l,int x,int y,const char *s,bool inv){
    st7305_color_t fg=inv?ST7305_COLOR_WHITE:ST7305_COLOR_BLACK;
    int aofs=(ZH16_FONT_H-16)/2;
    for(const unsigned char*p=(const unsigned char*)s;*p;){
        if((*p&0xe0)==0xe0){
            int idx=font_zh16_find_utf8((const char*)p);
            const uint8_t *b=(idx>=0)?font_zh16_get_bitmap_by_index(idx):NULL;
            if(b) for(int r=0;r<ZH16_FONT_H;r++)for(int c=0;c<ZH16_FONT_W;c++)
                if(b[r*2+(c>>3)]&(0x80>>(c&7))) st7305_draw_pixel(l,x+c,y+r,fg);
            x+=ZH16_FONT_W; p+=3;
        } else {
            if(*p>=0x20&&*p<=0x7e){ const uint8_t *b=FONT8X12[*p-0x20];
                for(int cy=0;cy<16;cy++)for(int cc=0;cc<8;cc++) if(b[cy]&(0x80u>>cc))
                    st7305_draw_pixel(l,x+cc,y+aofs+cy,fg); }
            x+=8; p++;
        }
    }
}
static void nd16c(st7305_handle_t *l,int cx,int y,const char *s,bool inv){ nd16(l,cx-nd16w(s)/2,y,s,inv); }
/* 16px 限宽截断绘制 (双列值太长时截断, 避免溢出/重叠到另一列) */
static void nd16capped(st7305_handle_t *l,int x,int y,const char*s,int maxw){
    int w=0,i=0; char b[80];
    for(const unsigned char*p=(const unsigned char*)s; *p && i<(int)sizeof(b)-1; ){
        int cw=(*p&0xe0)==0xe0?16:8;
        if(w+cw>maxw) break;
        w+=cw;
        if(cw==16){ if(i+2<(int)sizeof(b)){ b[i++]=p[0]; b[i++]=p[1]; b[i++]=p[2]; } p+=3; }
        else b[i++]=(char)*p++;
    }
    b[i]=0; nd16(l,x,y,b,false);
}
static const char *s_stepname[ST_TOTAL] = {"网络","信息","IP","端口","口令","报告"};
/* 黑色水平线 (照抄电脑诊断 dhline: 显式 ST7305_COLOR_BLACK 保证一定可见) */
static void nd_hline(st7305_handle_t *l,int x0,int x1,int y){
    if(y<0||y>=NH) return;
    if(x0>x1){ int t=x0; x0=x1; x1=t; }
    for(int x=x0;x<=x1;x++) if(x>=0&&x<NW) st7305_draw_pixel(l,x,y,ST7305_COLOR_BLACK);
}
static void draw_stepbar(st7305_handle_t *l, step_t cur){
    const int n = ST_TOTAL;
    int xs[8];
    for(int i=0;i<n;i++) xs[i] = 30 + (NW-60)*i/(n-1);   /* DNODE_PAD=30, DW=400 */
    int lineY = ND_BAR_H - 10;                             /* 40-10=30 */
    /* 连接线: 与电脑诊断一致 — 全段 1px 黑基线; 已走过段 3px 加粗且以圆点为中心 */
    nd_hline(l, xs[0], xs[n-1], lineY);
    if(cur>=0) for(int dy=-1;dy<=1;dy++)
        nd_hline(l, xs[0], xs[cur<ST_TOTAL-1?cur:ST_TOTAL-1], lineY+dy);   /* 3px 居中 */
    for(int i=0;i<n;i++){
        int cx=xs[i];
        if(i<=cur) nd_disk(l,cx,lineY,(i==cur)?6:5,true);   /* 实心 */
        else { nd_disk(l,cx,lineY,3,false); nd_disk(l,cx,lineY,2,false); } /* 空心 */
        /* 步骤名: 与电脑诊断同款的位置(最顶 y=2, 不与进度线重叠), 统一黑色.
         * (此屏白色不可见 → 不再用白字反色; 当前节点靠实心大圆 + 4px 黑粗线标识) */
        nd16c(l, cx, 2, s_stepname[i], false);
    }
}

/* ---------------- ICMP 回显扫描 ---------------- */
static uint16_t icmp_cks(const void *b, int n){
    const uint16_t *p=b; uint32_t sum=0;
    while(n>1){ sum+=*p++; n-=2; }
    if(n) sum+=*(const uint8_t*)p;
    while(sum>>16) sum=(sum&0xffff)+(sum>>16);
    return (uint16_t)~sum;
}
/* 点分掩码(如 255.255.255.0)转前缀位数; 空/失败默认 /24 */
static int mask_prefix(const char *mask){
    struct in_addr mi; if(!mask || !mask[0] || !inet_aton(mask,&mi)) return 24;
    uint32_t m = ntohl(mi.s_addr); int p=0;
    for(int b=31;b>=0;b--){ if(m & (1u<<b)) p++; else break; }
    return p;
}
/* 返回在线主机数, 结果写入 up[]. (每台发2个echo提高命中, 多等一会) */
static int icmp_sweep(uint32_t base, uint8_t up[], int hn){   /* hn=子网主机上限(不含0/广播) */
    int sd=socket(AF_INET,SOCK_RAW,IPPROTO_ICMP);
    if(sd<0) return 0;
    struct timeval tv={3,500000}; setsockopt(sd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    int n=0;
    for(int k=1;k<hn;k++){
        uint32_t ip=base|(uint32_t)k;
        struct sockaddr_in to; memset(&to,0,sizeof(to));
        to.sin_family=AF_INET; to.sin_addr.s_addr=htonl(ip);
        for(int rnd=0;rnd<2;rnd++){
            uint8_t pkt[36]; memset(pkt,0,sizeof(pkt));
            pkt[0]=8; pkt[1]=0;                      /* echo request */
            uint16_t id=(uint16_t)(0x1337+rnd), seq=(uint16_t)k;
            memcpy(pkt+4,&id,2); memcpy(pkt+6,&seq,2);
            uint16_t ck=icmp_cks(pkt,36); memcpy(pkt+2,&ck,2);
            sendto(sd,pkt,36,0,(struct sockaddr*)&to,sizeof(to));
        }
    }
    for(;;){
        struct sockaddr_in from; socklen_t fl=sizeof(from);
        uint8_t buf[64];
        int r=recvfrom(sd,buf,sizeof(buf),0,(struct sockaddr*)&from,&fl);
        if(r<=0) break;
        if(buf[0]==0){                            /* echo reply */
            uint32_t rip=ntohl(from.sin_addr.s_addr);
            uint8_t o=(uint8_t)(rip&0xff);
            if(o>=1&&o<255&&!up[o]){ up[o]=1; n++; }
        }
    }
    closesocket(sd);
    return n;
}
/* 对单台 ip 发一次 ICMP echo, ~300ms 超时. 命中返回 true (存活). */
static bool nd_ping_ok(uint32_t ip){
    int sd=socket(AF_INET,SOCK_RAW,IPPROTO_ICMP);
    if(sd<0) return false;
    struct timeval tv={0,300000}; setsockopt(sd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    struct sockaddr_in to; memset(&to,0,sizeof(to));
    to.sin_family=AF_INET; to.sin_addr.s_addr=htonl(ip);
    uint8_t pkt[36]; memset(pkt,0,sizeof(pkt));
    pkt[0]=8; pkt[1]=0;                          /* echo request */
    uint16_t id=0x6060, seq=(uint16_t)(ip&0xff); memcpy(pkt+4,&id,2); memcpy(pkt+6,&seq,2);
    uint16_t ck=icmp_cks(pkt,36); memcpy(pkt+2,&ck,2);
    sendto(sd,pkt,36,0,(struct sockaddr*)&to,sizeof(to));
    uint8_t buf[64];
    int r=recvfrom(sd,buf,sizeof(buf),0,NULL,NULL);
    closesocket(sd);
    return (r>0 && buf[0]==0);
}
/* 对 ip 发 ICMP echo 测 RTT, 返回最小往返 ms; 不可达返回 -1 */
static int ping_rtt(uint32_t ip, int tries){
    int sd=socket(AF_INET,SOCK_RAW,IPPROTO_ICMP);
    if(sd<0) return -1;
    struct timeval tv={0,400000}; setsockopt(sd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    int best=-1;
    for(int t=0;t<tries;t++){
        struct sockaddr_in to; memset(&to,0,sizeof(to));
        to.sin_family=AF_INET; to.sin_addr.s_addr=htonl(ip);
        uint8_t pkt[36]; memset(pkt,0,sizeof(pkt));
        pkt[0]=8; pkt[1]=0;
        uint16_t id=(uint16_t)(0x5050), seq=(uint16_t)t; memcpy(pkt+4,&id,2); memcpy(pkt+6,&seq,2);
        uint16_t ck=icmp_cks(pkt,36); memcpy(pkt+2,&ck,2);
        int64_t t0=esp_timer_get_time();
        sendto(sd,pkt,36,0,(struct sockaddr*)&to,sizeof(to));
        uint8_t r[64]; socklen_t fl=sizeof(struct sockaddr_in); struct sockaddr_in fr;
        int rcv=recvfrom(sd,r,sizeof(r),0,(struct sockaddr*)&fr,&fl);
        if(rcv>0 && r[0]==0){ int ms=(int)((esp_timer_get_time()-t0)/1000); if(best<0||ms<best)best=ms; }
    }
    closesocket(sd);
    return best;
}
static int tcp_probe(uint32_t ip, uint16_t port, int to_ms);
/* 官方 esp_ping 测 RTT (netdect 任务栈现为内部RAM, 不会触发 cache 冻结断言, 可安全调用) */
static volatile int s_ndp_done=0, s_ndp_ms=-1;
static void ndp_ok(esp_ping_handle_t h, void *a){ (void)a; uint32_t t=0;
    esp_ping_get_profile(h,ESP_PING_PROF_TIMEGAP,&t,sizeof(t)); s_ndp_ms=(int)t; }
static void ndp_end(esp_ping_handle_t h, void *a){ (void)h;(void)a; s_ndp_done=1; }
static int nd_ping_rtt(uint32_t ip, int tries){
    ip_addr_t t; IP_ADDR4(&t,(uint8_t)(ip>>24),(uint8_t)(ip>>16),(uint8_t)(ip>>8),(uint8_t)ip);
    esp_ping_config_t cfg=ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr=t; cfg.count=tries; cfg.interval_ms=100; cfg.timeout_ms=300;
    cfg.task_stack_size=4096; cfg.task_prio=4;
    esp_ping_callbacks_t cb={ .on_ping_success=ndp_ok, .on_ping_end=ndp_end };
    esp_ping_handle_t h=NULL;
    s_ndp_done=0; s_ndp_ms=-1;
    if(esp_ping_new_session(&cfg,&cb,&h)!=ESP_OK) return -1;
    esp_ping_start(h);
    uint32_t t0=(uint32_t)(esp_timer_get_time()/1000);
    while(!s_ndp_done && (uint32_t)(esp_timer_get_time()/1000)-t0<(uint32_t)(tries*200+1000)) vTaskDelay(5);
    esp_ping_delete_session(h);
    return (s_ndp_done && s_ndp_ms>=0)? s_ndp_ms : -1;
}
/* 官方 esp_ping 探活: 每台最多 3 轮独立探测, 每轮 1 次 echo(超时200ms), 命中即返.
 * 在线主机通常首轮几十 ms 即回 → 快; 离线主机最多 3 轮 ≈0.9s → 不慢也够准.
 * 安全: 每轮都是独立完整会话, 必须等 on_ping_end(s_ndp_done) 才 delete_session —
 * esp_ping_delete_session 只清 INIT 标志、不等 ping 任务退出; 提前删会话会让 ping 任务
 * 继续用已释放的 ep 发包/回调 → use-after-free → "stack overflow in task ping" 重启. */
static bool nd_ping_alive(uint32_t ip){
    for(int attempt=0; attempt<3; attempt++){
        ip_addr_t t; IP_ADDR4(&t,(uint8_t)(ip>>24),(uint8_t)(ip>>16),(uint8_t)(ip>>8),(uint8_t)ip);
        esp_ping_config_t cfg=ESP_PING_DEFAULT_CONFIG();
        cfg.target_addr=t; cfg.count=1; cfg.interval_ms=50; cfg.timeout_ms=200;
        cfg.task_stack_size=4096; cfg.task_prio=4;
        esp_ping_callbacks_t cb={ .on_ping_success=ndp_ok, .on_ping_end=ndp_end };
        esp_ping_handle_t h=NULL;
        s_ndp_done=0; s_ndp_ms=-1;
        if(esp_ping_new_session(&cfg,&cb,&h)!=ESP_OK) return false;
        esp_ping_start(h);
        uint32_t budget = (uint32_t)cfg.timeout_ms+200;   /* 等完整一轮(on_ping_end), 不提前删会话 */
        uint32_t t0=(uint32_t)(esp_timer_get_time()/1000);
        while(!s_ndp_done && (uint32_t)(esp_timer_get_time()/1000)-t0<budget) vTaskDelay(4);
        esp_ping_delete_session(h);
        vTaskDelay(pdMS_TO_TICKS(20));   /* 等ping任务回到等待态, 避免与下一轮新会话回调重叠 */
        if(s_ndp_done && s_ndp_ms>=0) return true;   /* 本轮命中 */
    }
    return false;
}
static void tcp_alive_sweep(uint32_t base, uint8_t up[], const int *ports, int np, int hn){
    for(int k=1;k<hn;k++){
        if(up[k]) continue;
        uint32_t ip=base|(uint32_t)k;
        for(int pi=0;pi<np;pi++){
            if(tcp_probe(ip, (uint16_t)ports[pi], 100)==0){ up[k]=1; break; }
        }
    }
}
static int nd_have(uint32_t ip){ for(int i=0;i<s_nhost;i++) if(s_h[i].ip==ip) return 1; return 0; }
static bool nd_snif_mac_of(uint32_t ip, uint8_t mac[6]);
/* 读取目标MAC: etharp_request 每台发3轮+多等待, 防ARP表被后续ping挤掉; sta与netif_list双查 */
static bool nd_resolve_mac(uint32_t ip, uint8_t mac[6]){
    ip4_addr_t a; a.addr=htonl(ip);
    struct eth_addr *m=NULL; const ip4_addr_t *hip=NULL;
    struct netif *sta=NULL;
    esp_netif_t *en=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(en) sta=(struct netif *)esp_netif_get_netif_impl(en);
    if(!sta) for(struct netif *n=netif_list;n;n=n->next)
        if(netif_is_up(n) && n->ip_addr.u_addr.ip4.addr){ sta=n; break; }
    if(!sta) return false;
    static bool slog=false;
    err_t req_err = etharp_request(sta, &a);
    if(!slog){ ESP_LOGI("netdect","[mac] sta=%p req_ret=%d arpend=0x%lx",(void*)sta,(int)req_err,(unsigned long)a.addr); slog=true; }
    for(int r=0;r<2;r++){
        if(r) etharp_request(sta, &a);
        for(int i=0;i<12;i++){                       /* 每轮~120ms, 快到不卡扫描 */
            if(etharp_find_addr(NULL,&a,&m,&hip)==ERR_OK && m){ memcpy(mac,m->addr,6); return true; }  /* 参考项目: NULL可读全表 */
            if(sta){ if(etharp_find_addr(sta,&a,&m,&hip)==ERR_OK && m){ memcpy(mac,m->addr,6); return true; } }
            for(struct netif *n=netif_list;n;n=n->next)
                if(etharp_find_addr(n,&a,&m,&hip)==ERR_OK && m){ memcpy(mac,m->addr,6); return true; }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelay(pdMS_TO_TICKS(20));               /* 轮间间隔 */
    }
    return false;
}
/* 英文OUI厂商→中文短名 (缩短+中文化, 避免长英文盖住IP/MAC) */
static void nd_vendor_zh(const char *en, char *out, int cap){
    out[0]=0; if(!en){ return; }
    static const struct { const char*sub; const char*zh; } map[]={
        {"Xiaomi","\xe5\xb0\x8f\xe7\xb1\xb3"},{"Beijing Xiaomi","\xe5\xb0\x8f\xe7\xb1\xb3"},
        {"Huawei","\xe5\x8d\x8e\xe4\xb8\xba"},{"Espressif","\xe4\xb9\x90\xe9\x91\xab"},
        {"TP-Link","TP-LINK"},{"Tenda","\xe8\x85\xbe\xe8\xbe\xbe"},
        {"Hon Hai","\xe9\xb4\xbb\xe6\xb5\xb7"},{"Foxconn","\xe9\xb4\xbb\xe6\xb5\xb7"},
        {"Cisco","Cisco"},{"Micro-Star","MSI"},{"ASUSTek","\xe5\x8d\x8e\xe7\xa1\x95"},{"ASUS","ASUS"},
        {"Samsung","\xe4\xb8\x89\xe6\x98\x9f"},{"Apple","\xe8\x8b\xb9\xe6\x9e\x9c"},
        {"Espressif Inc","\xe4\xb9\x90\xe9\x91\xab"},{"Hikvision","\xe6\xb5\xb7\xe5\xba\xb7"},
        {"D-Link","\xe5\x8f\x8b\xe8\xbf\x85"},{"Netcore","\xe8\x85\xbe\xe8\xbe\xbe"},
        {"Realtek","Realtek"},{"Liteon","\xe5\x85\x89\xe5\xae\x9d"},{"Lite-On","\xe5\x85\x89\xe5\xae\x9d"},
        {"zhejiang Dahua","\xe5\xa4\xa7\xe5\x8d\x8e"},{"Dahua","\xe5\xa4\xa7\xe5\x8d\x8e"},
    };
    for(unsigned i=0;i<sizeof(map)/sizeof(map[0]);i++){
        if(strstr(en,map[i].sub)){ snprintf(out,cap,"%s",map[i].zh); return; }
    }
    snprintf(out,cap,"%s",en);
}

/* 入库一台主机: 立即解析MAC→反查品牌(MAC OUI); 超时(不可达)则跳过品牌 */
static void nd_add_host(uint32_t ip, bool is_lip){
    if(s_nhost>=MAX_HOSTS) return;
    host_t *h=&s_h[s_nhost]; memset(h,0,sizeof(*h));
    h->ip=ip;
    if(is_lip){ h->local=true; h->macok=true; esp_wifi_get_mac(WIFI_IF_STA, h->mac); }
    else {
        h->macok = nd_resolve_mac(ip, h->mac);          /* 先ARP */
        if(!h->macok) h->macok = nd_snif_mac_of(ip, h->mac);   /* ARP没到→查嗅探表 */
    }
    if(h->macok){ const char *v=NULL; char zh[24]; if(macoui_lookup(h->mac,&v)&&v){ nd_vendor_zh(v,zh,sizeof(zh)); snprintf(h->vendor,sizeof(h->vendor),"%s",zh); } }
    else snprintf(h->vendor,sizeof(h->vendor),"?");
    ESP_LOGI("netdect","[found] %u.%u.%u.%u macok=%d mac=%02X:%02X:%02X:%02X:%02X:%02X vendor=%s",
        (unsigned)((ip>>24)&0xff),(unsigned)((ip>>16)&0xff),(unsigned)((ip>>8)&0xff),(unsigned)(ip&0xff),
        h->macok, h->mac[0],h->mac[1],h->mac[2],h->mac[3],h->mac[4],h->mac[5], h->vendor[0]?h->vendor:"?");
    s_nhost++;
}
/* ---------------- TCP 探测 / banner ---------------- */
static int tcp_probe(uint32_t ip, uint16_t port, int to_ms){
    int sd=socket(AF_INET,SOCK_STREAM,0); if(sd<0) return -1;
    struct timeval tv={0,to_ms*1000};
    setsockopt(sd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    setsockopt(sd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    int fl=fcntl(sd,F_GETFL,0); fcntl(sd,F_SETFL,fl|O_NONBLOCK);
    struct sockaddr_in to; memset(&to,0,sizeof(to));
    to.sin_family=AF_INET; to.sin_addr.s_addr=htonl(ip); to.sin_port=htons(port);
    int r=connect(sd,(struct sockaddr*)&to,sizeof(to));
    int open=0;
    if(r==0) open=1;
    else if(errno==EINPROGRESS){
        fd_set w; FD_ZERO(&w); FD_SET(sd,&w);
        struct timeval wait={0,to_ms*1000};
        if(select(sd+1,NULL,&w,NULL,&wait)>0){
            int so=0; socklen_t sl=sizeof(so);
            getsockopt(sd,SOL_SOCKET,SO_ERROR,&so,&sl);
            open=(so==0);
        }
    }
    if(open){ /* 已确认可连; 关闭以交还给 banner 读取 */ }
    closesocket(sd);
    return open?0:-1;
}
/* 读 banner (文本), 返回长度 */
static int read_banner(uint32_t ip, uint16_t port, char *out, int cap){
    int sd=socket(AF_INET,SOCK_STREAM,0); if(sd<0) return 0;
    struct timeval tv={0,700000}; setsockopt(sd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    setsockopt(sd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    struct sockaddr_in to; memset(&to,0,sizeof(to));
    to.sin_family=AF_INET; to.sin_addr.s_addr=htonl(ip); to.sin_port=htons(port);
    if(connect(sd,(struct sockaddr*)&to,sizeof(to))<0){ closesocket(sd); return 0; }
    if(port==80||port==81||port==8080){
        const char *g="GET / HTTP/1.0\r\nHost: x\r\n\r\n";
        send(sd,g,strlen(g),0);
    }
    out[0]=0; int n=0;
    uint8_t b;
    while(n<cap-1){ int r=recv(sd,&b,1,0); if(r<=0)break; out[n++]=(char)b; }
    out[n]=0;
    /* 规整成单行可见 */
    for(int i=0;i<n;i++){ char c=out[i]; if(c=='\r'||c=='\n') c=' '; if(c<32||c>126) c='.'; out[i]=c; }
    closesocket(sd);
    return n;
}
static void classify(const char *banner, uint16_t port, char *svc, int svc_cap, char *ver, int ver_cap){
    strncpy(svc, "TCP", svc_cap-1); svc[svc_cap-1]=0;
    ver[0]=0;
    if(port==22){ strncpy(svc,"SSH",svc_cap-1); }
    else if(port==21){ strncpy(svc,"FTP",svc_cap-1); }
    else if(port==23){ strncpy(svc,"Telnet",svc_cap-1); }
    else if(port==2333){ strncpy(svc,"MGT",svc_cap-1); }    /* 远程维护口 */
    else if(port==445){ strncpy(svc,"SMB",svc_cap-1); }
    else if(port==3389){ strncpy(svc,"RDP",svc_cap-1); }
    else if(port==53){ strncpy(svc,"DNS",svc_cap-1); }
    else if(strncmp(banner,"HTTP",4)==0){ strncpy(svc,"HTTP",svc_cap-1); }
    /* 抓版本: SSH banner "SSH-2.0-..." */
    if(strncmp(banner,"SSH-",4)==0){ snprintf(ver,ver_cap,"%s",banner); }
    /* 窃取可能版本号片段短留 */
    if(!ver[0] && port==21 && strncmp(banner,"220",3)==0){
        char *t=strchr(banner+3,' ');
        if(t) snprintf(ver,ver_cap,"%s",t+1);
    }
}

/* ---------------- 网络信息: 公网IP/ISP ---------------- */
static size_t nd_http_get(const char *url, char *out, int cap){
    out[0]=0;
    esp_http_client_config_t cfg=(esp_http_client_config_t){
        .url=url, .timeout_ms=6000, .buffer_size=1024,
        .crt_bundle_attach=esp_crt_bundle_attach,   /* https 需要证书, 否则 TLS 校验失败 */
    };
    esp_http_client_handle_t c=esp_http_client_init(&cfg);
    if(!c) return 0;
    int off=0;
    esp_http_client_set_method(c,HTTP_METHOD_GET);
    esp_err_t e=esp_http_client_open(c,0);
    int len=(e==ESP_OK)? esp_http_client_fetch_headers(c):-1;
    if(len>=0){
        int r;
        while(off<cap-1 && (r=esp_http_client_read(c,out+off,cap-1-off))>0) off+=r;
        out[off]=0;
    }
    esp_http_client_close(c); esp_http_client_cleanup(c);
    return (size_t)off;
}
/* 从 JSON 取字符串字段: "key":"value" → out (UTF-8 安全) */
static int json_get_str(const char *json, const char *key, char *out, int cap){
    char pat[32]; snprintf(pat,sizeof(pat),"\"%s\":\"",key);
    const char *p=strstr(json,pat);
    if(!p){ out[0]=0; return 0; }
    p+=strlen(pat);
    int n=0; while(n<cap-1 && *p && *p!='"'){ out[n++]=*p++; }
    out[n]=0; return n;
}
/* 运营商规范成"中国电信/联通/移动/铁通" (与百度显示一致) */
static void map_isp_china(void){
    static const struct { const char *sub; const char *full; } mp[]={
        {"\xe7\x94\xb5\xe4\xbf\xa1","\xe4\xb8\xad\xe5\x9b\xbd\xe7\x94\xb5\xe4\xbf\xa1"},  /* 电信→中国电信 */
        {"\xe8\x81\x94\xe9\x80\x9a","\xe4\xb8\xad\xe5\x9b\xbd\xe8\x81\x94\xe9\x80\x9a"},  /* 联通→中国联通 */
        {"\xe7\xa7\xbb\xe5\x8a\xa8","\xe4\xb8\xad\xe5\x9b\xbd\xe7\xa7\xbb\xe5\x8a\xa8"},  /* 移动→中国移动 */
        {"\xe9\x93\x81\xe9\x80\x9a","\xe4\xb8\xad\xe5\x9b\xbd\xe9\x93\x81\xe9\x80\x9a"},  /* 铁通→中国铁通 */
    };
    if(!s_isp_brand[0]) return;
    for(unsigned i=0;i<sizeof(mp)/sizeof(mp[0]);i++)
        if(strstr(s_isp_brand,mp[i].sub)){ snprintf(s_isp_brand,sizeof(s_isp_brand),"%s",mp[i].full); return; }
}
static void fetch_pub_info(void){
    char resp[1024];
    s_pub_ip[0]=0; s_isp_brand[0]=0; s_city[0]=0; s_net_scenario[0]=0;
    /* ① 首选 ip9.com.cn/get: UTF-8 中文, 设备实测可达 (福建/莆田/中国电信) */
    if(nd_http_get("https://ip9.com.cn/get", resp, sizeof(resp))){
        char ip[20]="", prov[24]="", city[24]="", isp[24]="";
        json_get_str(resp,"ip",ip,sizeof(ip));
        json_get_str(resp,"prov",prov,sizeof(prov));
        json_get_str(resp,"city",city,sizeof(city));
        json_get_str(resp,"isp",isp,sizeof(isp));
        if(ip[0]) snprintf(s_pub_ip,sizeof(s_pub_ip),"%s",ip);
        if(prov[0]||city[0]){ char tmp[64]="\xe4\xb8\xad\xe5\x9b\xbd";   /* 中国 */
            if(prov[0]){ strcat(tmp," "); strncat(tmp,prov,sizeof(tmp)-strlen(tmp)-1); }
            if(city[0]){ strcat(tmp," "); strncat(tmp,city,sizeof(tmp)-strlen(tmp)-1); }
            snprintf(s_city,sizeof(s_city),"%s",tmp); }
        if(isp[0]) snprintf(s_isp_brand,sizeof(s_isp_brand),"%s",isp);
        ESP_LOGI("netdect","[info/ip9] ip=%s city=%s isp=%s", s_pub_ip, s_city, s_isp_brand);
    }
    if(s_isp_brand[0]) map_isp_china();
    /* ② 同时请求 myip.ipip.net 作字段级融合: 各字段取"信息更多(更长)"的非空值 */
    if(nd_http_get("https://myip.ipip.net/json", resp, sizeof(resp))){
        char mip[16]="", mpr[24]="", mct[24]="", mis[24]="";
        json_get_str(resp,"ip",mip,sizeof(mip));
        const char *loc = strstr(resp, "\"location\"");
        const char *p = loc ? strchr(loc, '[') : NULL;
        if(p){ char el[5][24]={{0}}; int n=0; p++;
            while(*p && n<5){ while(*p && *p!='"' && *p!=']') p++; if(*p!='"') break; p++; int i=0;
                while(*p && *p!='"'){ if(i<23) el[n][i++]=(char)*p; p++; } if(*p=='"') p++; n++;
                while(*p && (*p==',' || *p==']')) p++; }
            if(el[1][0]) snprintf(mpr,sizeof(mpr),"%s",el[1]);
            if(el[2][0]) snprintf(mct,sizeof(mct),"%s",el[2]);
            if(el[4][0]) snprintf(mis,sizeof(mis),"%s",el[4]);
        }
        /* 融合: 更长的值信息更多 → 覆盖 */
        if(!s_pub_ip[0] && mip[0]) snprintf(s_pub_ip,sizeof(s_pub_ip),"%s",mip);
        if(mis[0] && strlen(mis)>strlen(s_isp_brand)) snprintf(s_isp_brand,sizeof(s_isp_brand),"%s",mis);
        char cm[56]="";
        if(mpr[0]){ snprintf(cm,sizeof(cm),"%s",mpr);
            if(mct[0] && strcmp(mct,mpr)!=0){ strcat(cm," "); strncat(cm,mct,sizeof(cm)-strlen(cm)-1); } }
        if(cm[0] && strlen(cm)>strlen(s_city)) snprintf(s_city,sizeof(s_city),"%s",cm);
        ESP_LOGI("netdect","[info/myip融合] ip=%s city=%s isp=%s", s_pub_ip, s_city, s_isp_brand);
    }
    ESP_LOGI("netdect","[info/result] ip=%s isp=%s city=%s", s_pub_ip, s_isp_brand, s_city);
}

static void nd_get_local(void){
    esp_netif_t *ni=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(!ni) ni=esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if(ni){
        esp_netif_ip_info_t ip;
        if(esp_netif_get_ip_info(ni,&ip)==ESP_OK){
            snprintf(s_local_ip,sizeof(s_local_ip),IPSTR,IP2STR(&ip.ip));
            snprintf(s_mask,sizeof(s_mask),IPSTR,IP2STR(&ip.netmask));
            snprintf(s_gw,sizeof(s_gw),IPSTR,IP2STR(&ip.gw));
            esp_netif_dns_info_t dns;
            if(esp_netif_get_dns_info(ni,ESP_NETIF_DNS_MAIN,&dns)==ESP_OK)
                snprintf(s_dns,sizeof(s_dns),IPSTR,IP2STR(&dns.ip.u_addr.ip4));
            const char *d=esp_netif_get_desc(ni);
            snprintf(s_nwtype,sizeof(s_nwtype),"%s", d?d:"?");
        }
    }
}

/* ---------------- 弱口令 (仅 FTP/Telnet, 极少量, 自检) ---------------- */
typedef struct { const char *u,*p; } cred_t;
static const cred_t s_cred[] = {
    {"root",""},{"root","root"},{"root","123456"},{"root","admin"},{"root","password"},{"root","toor"},{"root","root123"},
    {"admin",""},{"admin","admin"},{"admin","123456"},{"admin","12345"},{"admin","12345678"},{"admin","password"},{"admin","root"},{"admin","admin123"},
    {"administrator",""},{"administrator","admin"},{"administrator","123456"},
    {"user","user"},{"test","test"},{"guest","guest"},{"oracle","oracle"},{"postgres","postgres"},{"ftp","ftp"},
};
#define CRED_N (sizeof(s_cred)/sizeof(s_cred[0]))

static int ftp_try(uint32_t ip, const char*u,const char*p, char *out,int cap){
    int sd=socket(AF_INET,SOCK_STREAM,0); if(sd<0)return 0;
    struct timeval tv={0,800000}; setsockopt(sd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    struct sockaddr_in to; memset(&to,0,sizeof(to));
    to.sin_family=AF_INET; to.sin_addr.s_addr=htonl(ip); to.sin_port=htons(21);
    int open=0;
    int fl=fcntl(sd,F_GETFL,0); fcntl(sd,F_SETFL,fl|O_NONBLOCK);
    if(connect(sd,(struct sockaddr*)&to,sizeof(to))<0 && errno==EINPROGRESS){
        fd_set w; FD_ZERO(&w); FD_SET(sd,&w); struct timeval wt={0,800000};
        if(select(sd+1,NULL,&w,NULL,&wt)>0){ int so=0;socklen_t sl=sizeof(so);getsockopt(sd,SOL_SOCKET,SO_ERROR,&so,&sl); open=(so==0);}
    } else if(errno==0) open=1;
    fcntl(sd,F_SETFL,fl);
    if(!open){ closesocket(sd); return 0; }
    /* 读 220 */
    uint8_t b; int i=0; char greet[40]; memset(greet,0,sizeof(greet));
    while(i<39){ int r=recv(sd,&b,1,0); if(r<=0)break; greet[i++]=(char)b; }
    char cmd[96];
    snprintf(cmd,sizeof(cmd),"USER %s\r\nPASS %s\r\n",u,p);
    send(sd,cmd,strlen(cmd),0);
    int ok=0; memset(out,0,cap);
    int n=0;
    while(n<cap-1){ int r=recv(sd,&b,1,0); if(r<=0)break; out[n++]=(char)b; }
    out[n]=0;
    if(strstr(out,"230")) ok=1;
    closesocket(sd);
    return ok;
}

/* ---------------- 引擎主流程 ---------------- */
#define ND_ABORT (s_stop || s_gen != s_mine)
/* 暂停直到用户按"下一步" */
static void nd_pause(int si){
    s_step=si; s_step_done=true; if(si+1>s_finished)s_finished=si+1;
    if(s_finished>ST_TOTAL-1)s_finished=ST_TOTAL-1;
    s_view=si; s_nav_ovr=false;           /* 显示刚完成的那一步, 取消切换覆盖 */
    s_cancel_step=false;                  /* 一步一旦入暂停即清除取消标志, 下一步从零扫描 */
    while(!ND_ABORT && !s_step_go) vTaskDelay(10);
    s_step_go=false; s_step_done=false;
}

/* ---- 802.11 嗅探补 MAC: 物理收听客户端→AP 数据帧, 读源MAC+源IP, 绕开AP隔离 ---- */
#define ND_HO_TABLE 256
EXT_RAM_BSS_ATTR static uint8_t s_snif_mac[ND_HO_TABLE][6];
EXT_RAM_BSS_ATTR static uint8_t s_snif_ok[ND_HO_TABLE];
static volatile int s_snif_on = 0;
static volatile int s_snif_total = 0;   /* 数据帧计数 */
static volatile int s_snif_stored = 0;  /* 成功映射 IP→MAC 数 */
static void nd_snif_rx(void *buf, wifi_promiscuous_pkt_type_t type){
    if(!s_snif_on) return;
    if(type != WIFI_PKT_DATA) return;
    s_snif_total++;
    wifi_promiscuous_pkt_t *p = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *f = p->payload; int fl = p->rx_ctrl.sig_len;
    if(fl < 52) return;
    uint8_t fc0=f[0], fc1=f[1];
    if(((fc0>>2)&3) != 2) return;                    /* type=data */
    int ds = (fc1 & 0x03);                           /* 0=IBSS,1=ToDS,2=FromDS,3=DSDS */
    int hdr = 24; if((fc0 & 0x0f) & 0x8) hdr += 2;   /* QoS→+2 */
    if(fl < hdr+8+20) return;
    uint8_t *llc = f+hdr;
    if(llc[0]!=0xAA || llc[1]!=0xAA || llc[2]!=0x03) return;
    uint16_t et=(uint16_t)((llc[6]<<8)|llc[7]);
    if(et != 0x0800) return;
    uint8_t *ip = llc+8;
    if((ip[0]>>4) != 4) return;
    /* 双方向: 设备MAC与设备IP */
    uint8_t ho=0; const uint8_t *dm=NULL;
    if(ds==1){ ho=ip[15]; dm=f+10; }                 /* 客户端→AP: MAC=addr2, IP=src */
    else if(ds==2){ ho=ip[19]; dm=f+4; }             /* AP→客户端: MAC=addr1, IP=dst */
    else return;
    if(ho<1 || ho>254) return;
    if(!(dm[0]|dm[1]|dm[2]|dm[3]|dm[4]|dm[5])) return;
    if(!s_snif_ok[ho]) s_snif_stored++;
    memcpy(s_snif_mac[ho], dm, 6); s_snif_ok[ho]=1;
}
static void nd_snif_start(void){
    /* 不开启 promiscuous: 开启会劫持接收路径, 让 lwIP 收不到 ARP 回包 → 取不到 MAC.
     * ARP 应走正常网络栈(与 nmap/arp-scan 同机制). */
    memset(s_snif_mac,0,sizeof(s_snif_mac)); memset(s_snif_ok,0,sizeof(s_snif_ok));
}
static void nd_snif_stop(void){
    ESP_LOGI("netdect","[snif] data=%d stored=%d", (int)s_snif_total, (int)s_snif_stored);
    s_snif_on=0; esp_wifi_set_promiscuous(false);
}
static bool nd_snif_mac_of(uint32_t ip, uint8_t mac[6]){
    uint8_t ho=(uint8_t)(ip&0xff);
    if(s_snif_ok[ho]){ memcpy(mac,s_snif_mac[ho],6); return true; }
    return false;
}

static void netdect_run(void *arg){
    uint32_t base=0;
    s_mine = ++s_gen;                     /* 本代次标记; 退出/重进会使其失效 */
    /* 1 联网: 本机网络信息 */
    s_step=ST_NET; s_prog=0; s_step_eta=3; s_step_t0=(uint32_t)(esp_timer_get_time()/1000); snprintf(s_status,sizeof(s_status),"联网 · 获取本机信息");
    { esp_netif_t *ni=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      if(!ni) ni=esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
      if(ni){ esp_netif_ip_info_t ip; if(esp_netif_get_ip_info(ni,&ip)==ESP_OK){
            base=ntohl(ip.ip.addr) & ntohl(ip.netmask.addr);   /* 统一转主机序: 后文 socket 用 htonl, 显示用位移, 均按主机序整数 */
            snprintf(s_local_ip,sizeof(s_local_ip),IPSTR,IP2STR(&ip.ip));
            snprintf(s_mask,sizeof(s_mask),IPSTR,IP2STR(&ip.netmask));
            { int pre=mask_prefix(s_mask); int hb=32-pre;   /* 按点分掩码精确限子网主机范围 */
              int maxh=(hb>=8)?254:((1<<hb)-1);            /* >=/24 封顶254台; 更窄只扫子网内 */
              s_hostmax=(maxh<1)?1:maxh; }
            snprintf(s_gw,sizeof(s_gw),IPSTR,IP2STR(&ip.gw));
            esp_netif_dns_info_t dns;
            if(esp_netif_get_dns_info(ni,ESP_NETIF_DNS_MAIN,&dns)==ESP_OK)
                snprintf(s_dns,sizeof(s_dns),IPSTR,IP2STR(&dns.ip.u_addr.ip4));
            const char *d2=esp_netif_get_desc(ni);
            snprintf(s_nwtype,sizeof(s_nwtype),"%s", d2?d2:"?");
      }}}
      /* 是否支持IPv6: STA 有全局 IPv6 地址即为支持 (需路由器下发 SLAAC) */
      snprintf(s_ipv6,sizeof(s_ipv6),"\xe4\xb8\x8d\xe6\x94\xaf\xe6\x8c\x81");   /* 不支持 */
      { esp_netif_t *ni6=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if(ni6){ esp_ip6_addr_t g6; if(esp_netif_get_ip6_global(ni6,&g6)==ESP_OK)
            snprintf(s_ipv6,sizeof(s_ipv6),"\xe6\x94\xaf\xe6\x8c\x81"); } }     /* 支持 */
      /* 无线身份: SSID / 加密 / 本机 & 网关 MAC+厂商 (供详细报告) */
      if(esp_wifi_get_mac(WIFI_IF_STA,(unsigned char*)s_localmac)==ESP_OK)
          snprintf(s_localmac,sizeof(s_localmac),"%02X:%02X:%02X:%02X:%02X:%02X",s_localmac[0]&0xff,s_localmac[1]&0xff,s_localmac[2]&0xff,s_localmac[3]&0xff,s_localmac[4]&0xff,(unsigned char)s_localmac[5]);
      wifi_ap_record_t ap; memset(&ap,0,sizeof(ap));
      if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){
          snprintf(s_essaid,sizeof(s_essaid),"%s",ap.ssid);
          switch(ap.authmode){ case WIFI_AUTH_OPEN: snprintf(s_auth,sizeof(s_auth),"开放"); break;
              case WIFI_AUTH_WEP: snprintf(s_auth,sizeof(s_auth),"WEP"); break;
              case WIFI_AUTH_WPA_PSK: snprintf(s_auth,sizeof(s_auth),"WPA"); break;
              case WIFI_AUTH_WPA2_PSK: snprintf(s_auth,sizeof(s_auth),"WPA2"); break;
              case WIFI_AUTH_WPA_WPA2_PSK: snprintf(s_auth,sizeof(s_auth),"WPA/WPA2"); break;
              default: snprintf(s_auth,sizeof(s_auth),"?"); break; }
          /* 路由器/网关MAC: 家用路由下, 所连AP的BSSID即为路由器无线MAC (可靠, 免ARP) */
          snprintf(s_gwmac,sizeof(s_gwmac),"%02X:%02X:%02X:%02X:%02X:%02X",
                   ap.bssid[0]&0xff,ap.bssid[1]&0xff,ap.bssid[2]&0xff,
                   ap.bssid[3]&0xff,ap.bssid[4]&0xff,ap.bssid[5]&0xff);
          const char *v=NULL; if(macoui_lookup(ap.bssid,&v)&&v){ snprintf(s_gwmac_vendor,sizeof(s_gwmac_vendor),"%s",v); }
      }
      snprintf(s_gwip,sizeof(s_gwip),"%s",s_gw);
      /* 网关延迟 (esp_ping RTT) */
      snprintf(s_gw_rtt,sizeof(s_gw_rtt),"--");
      if(s_gw[0]){ struct in_addr g; inet_aton(s_gw,&g); int ms=nd_ping_rtt(ntohl(g.s_addr),3);
          if(ms>=0) snprintf(s_gw_rtt,sizeof(s_gw_rtt),"%d ms",ms); }
      ESP_LOGI("netdect","[gw] ip=%s rtt=%s mac=%s %s", s_gwip, s_gw_rtt, s_gwmac, s_gwmac_vendor);
    s_prog=100; if(ND_ABORT) goto fin;   /* 联网步只采集本机信息: 不停, 直接进"信息"页(停在那等手动下一步) */
    /* 2 信息: 公网 IP / ISP / 城市 */
    s_step=ST_IPINFO; s_prog=0; s_step_eta=10; s_step_t0=(uint32_t)(esp_timer_get_time()/1000); snprintf(s_status,sizeof(s_status),"信息 · 获取公网信息");
    if(!ND_ABORT) fetch_pub_info();
    if(!s_pub_ip[0] && !ND_ABORT){ vTaskDelay(pdMS_TO_TICKS(300)); if(!ND_ABORT) fetch_pub_info(); }   /* 未取到则重试一次 */
    /* 公网延迟 (esp_ping RTT): ping 稳定国内公网主机 阿里DNS 223.5.5.5 */
    snprintf(s_pub_rtt,sizeof(s_pub_rtt),"--");
    { struct in_addr pi; if(inet_aton("223.5.5.5",&pi)){ int ms=nd_ping_rtt(ntohl(pi.s_addr),3);
        if(ms>=0) snprintf(s_pub_rtt,sizeof(s_pub_rtt),"%d ms",ms); } }
    ESP_LOGI("netdect","[pub] rtt=%s", s_pub_rtt);
    s_prog=100; nd_pause(ST_IPINFO); if(ND_ABORT) goto fin;
    /* 3 IP: 主机扫描 (逐台扫描, 显示当前 IP, 扫到一台→加进列表→扫下一台) */
    s_step=ST_SCAN; s_prog=0; s_step_eta=100; s_step_t0=(uint32_t)(esp_timer_get_time()/1000);
    s_nhost=0; s_scrollpx=0; s_rptpx=0; s_rpt_nlin=0;
    nd_snif_start();                              /* 并行嗅探收MAC(绕开AP隔离) */
    uint32_t local_ip=0;
    { esp_netif_ip_info_t li; esp_netif_t *ni=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      if(!ni) ni=esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
      if(ni && esp_netif_get_ip_info(ni,&li)==ESP_OK) local_ip=ntohl(li.ip.addr); }
    if(base){
        for(int o=1;o<=s_hostmax && s_nhost<MAX_HOSTS;o++){
            if(ND_ABORT || s_cancel_step) break;
            uint32_t ip=base|(uint32_t)o;
            snprintf(s_status,sizeof(s_status),"正在扫描 %u.%u.%u.%u",
                     (unsigned)((ip>>24)&0xff),(unsigned)((ip>>16)&0xff),
                     (unsigned)((ip>>8)&0xff),(unsigned)(ip&0xff));
            s_prog=(s_hostmax>0)?(int)((long)o*100/s_hostmax):0;
            if(nd_ping_alive(ip)){        /* 官方 esp_ping 探活(裸 ICMP socket 在此平台收不到回包) */
                nd_add_host(ip, local_ip && ip==local_ip);   /* 扫到本机自动标 (本机) */
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        /* ARP 表补全: 抓屏蔽 ping 但同在网内在线的设备 (有MAC即视为在线) */
        for(int o=1;o<=s_hostmax && s_nhost<MAX_HOSTS;o++){
            if(ND_ABORT || s_cancel_step) break;
            uint32_t ip=base|(uint32_t)o;
            if(nd_have(ip)) continue;
            struct eth_addr *m=NULL; const ip4_addr_t *hip=NULL; ip4_addr_t a; a.addr=htonl(ip);
            for(struct netif *n=netif_list;n;n=n->next)
                if(etharp_find_addr(n,&a,&m,&hip)==ERR_OK && m){ nd_add_host(ip, local_ip && ip==local_ip); break; }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        /* 嗅探收尾: 补MAC + 补没回ping但发过帧的设备 */
        for(int o=1;o<=s_hostmax && s_nhost<MAX_HOSTS;o++){
            if(ND_ABORT || s_cancel_step) break;
            uint32_t ip=base|(uint32_t)o; uint8_t m6[6];
            if(!nd_snif_mac_of(ip,m6)) continue;
            int idx=-1; for(int i=0;i<s_nhost;i++) if(s_h[i].ip==ip){ idx=i; break; }
            if(idx>=0){ host_t *h=&s_h[idx];
                if(!h->macok){ memcpy(h->mac,m6,6); h->macok=true; h->vendor[0]=0;
                    const char *v=NULL; char zh[24]; if(macoui_lookup(h->mac,&v)&&v){ nd_vendor_zh(v,zh,sizeof(zh)); snprintf(h->vendor,sizeof(h->vendor),"%s",zh); } else snprintf(h->vendor,sizeof(h->vendor),"?");
                    }
                } else {
                nd_add_host(ip, local_ip && ip==local_ip);
                host_t *h=&s_h[s_nhost-1];
                if(!h->macok && h->ip==ip){ memcpy(h->mac,m6,6); h->macok=true; h->vendor[0]=0;
                    const char *v=NULL; char zh[24]; if(macoui_lookup(h->mac,&v)&&v){ nd_vendor_zh(v,zh,sizeof(zh)); snprintf(h->vendor,sizeof(h->vendor),"%s",zh); } else snprintf(h->vendor,sizeof(h->vendor),"?");
                }
            }
        }
        /* 二次补抓(对无MAC主机重试ARP): 可被「取消」随时中断 */
        {
            uint32_t t0=(uint32_t)(esp_timer_get_time()/1000);
            uint32_t t_any=t0;   /* 最后一次成功解析到新MAC的时刻 (1分钟内无进展则结束) */
            while(!ND_ABORT && !s_cancel_step &&
                  (uint32_t)(esp_timer_get_time()/1000)-t0 < 300000 &&
                  (uint32_t)(esp_timer_get_time()/1000)-t_any < 60000){
                int miss=0;
                for(int i=0;i<s_nhost;i++){ host_t*h=&s_h[i]; if(!h->macok && !h->local) miss++; }
                if(miss==0) break;
                snprintf(s_status,sizeof(s_status),"\xe6\x89\xab\xe6\x8f\x8f MAC \xe5\x9c\xb0\xe5\x9d\x80 %d/%d", s_nhost-miss, s_nhost); /* 扫描MAC地址 x/y */
                int any=0;
                for(int i=0;i<s_nhost;i++){
                    if(ND_ABORT || s_cancel_step) break;
                    host_t *h=&s_h[i];
                    if(h->macok || h->local) continue;
                    if(nd_resolve_mac(h->ip, h->mac)){
                        h->macok=true; h->vendor[0]=0;
                        const char *v=NULL; char zh[24]; if(macoui_lookup(h->mac,&v)&&v){ nd_vendor_zh(v,zh,sizeof(zh)); snprintf(h->vendor,sizeof(h->vendor),"%s",zh); } else snprintf(h->vendor,sizeof(h->vendor),"?");
                        any=1;
                        s_prog=(s_nhost>0)? (int)((long)(s_nhost-miss)*100/s_nhost) : 0;
                    }
                }
                if(any) t_any=(uint32_t)(esp_timer_get_time()/1000);
                if(!any && miss==0) break;
                /* 拆成 300ms×10 小睡, 点「取消」→0~0.3s 内即响应, 不必等满 3s */
                for(int w=0; w<10 && !s_cancel_step && !ND_ABORT; w++) vTaskDelay(pdMS_TO_TICKS(300));
            }
        }
    }
    nd_snif_stop();
    ESP_LOGI("netdect","主机扫描: 发现 %d 台", s_nhost);
    s_prog=100; nd_pause(ST_SCAN); if(ND_ABORT) goto fin;
    /* 4 端口: 开放端口/服务 */
    s_step=ST_PORT; s_prog=0; s_scrollpx=0; s_step_eta=20; s_step_t0=(uint32_t)(esp_timer_get_time()/1000); snprintf(s_status,sizeof(s_status),"端口 · 服务识别");
    for(int hi=0;hi<s_nhost;hi++){
        if(ND_ABORT || s_cancel_step) break;
        host_t *h=&s_h[hi]; h->np=0;
        snprintf(s_status,sizeof(s_status),"正在扫描 %u.%u.%u.%u",(unsigned)((h->ip>>24)&0xff),(unsigned)((h->ip>>16)&0xff),(unsigned)((h->ip>>8)&0xff),(unsigned)(h->ip&0xff));
        for(int pi=0;pi<MAX_PORTS && h->np<MAX_PORTS;pi++){
            if(ND_ABORT || s_cancel_step) break;
            uint16_t p=s_ports[pi].port;
            if(tcp_probe(h->ip,p,PROBE_TIMEOUT_MS)==0){
                int idx=h->np++;
                h->p[idx].port=p;
                char banner[64]; read_banner(h->ip,p,banner,sizeof(banner));
                char svc[10],ver[24];
                classify(banner,p,svc,sizeof(svc),ver,sizeof(ver));
                snprintf(h->p[idx].svc,sizeof(h->p[idx].svc),"%s",svc);
                if(ver[0]) snprintf(h->p[idx].banner,sizeof(h->p[idx].banner),"%s",ver);
                else snprintf(h->p[idx].banner,sizeof(h->p[idx].banner),"%s",banner);
            }
        }
        if(s_nhost>0) s_prog=(hi+1)*100/s_nhost;
    }
    s_prog=100; nd_pause(ST_PORT); if(ND_ABORT) goto fin;
    /* 5 口令: 弱口令自检 */
    s_step=ST_CRED; s_prog=0; s_scrollpx=0; s_step_eta=8; s_step_t0=(uint32_t)(esp_timer_get_time()/1000); snprintf(s_status,sizeof(s_status),"口令 · 弱口令自检");
    int donecred=0;
    for(int hi=0;hi<s_nhost && donecred<3;hi++){
        host_t* h=&s_h[hi];
        for(int pi=0;pi<h->np;pi++){
            if(ND_ABORT) goto fin;
            if(s_cancel_step) goto cred_pause;
            if(strcmp(h->p[pi].svc,"FTP")==0){
                vTaskDelay(pdMS_TO_TICKS(2000));   /* 每 IP 先停 2s → 单 IP 至少 3 秒, 别一闪而过 */
                for(unsigned ci=0;ci<CRED_N && !h->weak;ci++){
                    if(ND_ABORT) goto fin;
                    if(s_cancel_step) goto cred_pause;
                    snprintf(s_status,sizeof(s_status),"\xe6\xad\xa3\xe5\x9c\xa8\xe6\xb5\x8b\xe8\xaf\x95 %u.%u.%u.%u:%u %s/%s",
                             (unsigned)((h->ip>>24)&0xff),(unsigned)((h->ip>>16)&0xff),(unsigned)((h->ip>>8)&0xff),(unsigned)(h->ip&0xff),
                             h->p[pi].port,s_cred[ci].u,s_cred[ci].p);   /* 正在测试 IP:端口 用户/密码 */
                    vTaskDelay(pdMS_TO_TICKS(1000));   /* 每条口令停1s, 底部可见"正在测试" */
                    char r[64];
                    if(ftp_try(h->ip,s_cred[ci].u,s_cred[ci].p,r,sizeof(r))){
                        h->weak=true;
                        snprintf(h->weaknote,sizeof(h->weaknote),"%s/%s",s_cred[ci].u,s_cred[ci].p);
                        donecred++;
                    }
                }
                break;
            }
        }
    }
cred_pause:
    s_prog=100; nd_pause(ST_CRED);
fin:
    nd_snif_stop();
    /* 仅当前代的引擎才允许写最终态, 否则(已被上一步中止)会让旧任务把界面抢到报告页 */
    if(s_mine==s_gen){ s_done=true; s_step=ST_REPORT; s_step_done=false; s_view=ST_REPORT; }
    vTaskDelete(NULL);
}

/* ---------------- 连接网络 子流程 (WiFi 扫描列表 → 免密码/输密码 → 连接) ---------------- */
/* 返回连接网络页: 停止当前分析, 回到 WiFi 列表 (信息等步骤点"上一步"跳回此处) */
static void nd_back_to_connect(ui_ctx_t *ctx){
    s_stop=true; s_gen++;                 /* 终止在途分析任务 */
    s_wait_start=true; s_conn=0; s_prompt_sel=0;
    s_done=false; s_step_done=false; s_step_go=false; s_finished=0; s_view=0;
    s_prev_conn = wifi_manager_is_connected();
    if(wifi_manager_get_scan_count()<=0) wifi_manager_scan_start();   /* 有列表则复用, 避免卡顿 */
    ctx->needs_redraw=true;
}
/* 报告导出到 TF 卡 /sdcard/log/netreport.txt */
static void fmt_ip(uint32_t ip, char *b, int cap);   /* 前向声明 */
static char s_export_msg[48]="";
static void nd_export_report(void){
    mkdir("/sdcard/log",0755);
    FILE *f=fopen("/sdcard/log/netreport.txt","w");
    if(!f){ snprintf(s_export_msg,sizeof(s_export_msg),"SD 导出失败"); return; }
    fprintf(f,"网络分析报告\n");
    if(s_essaid[0]) fprintf(f,"无线 %s [%s]\n",s_essaid,s_auth);
    fprintf(f,"本机 %s %s\n本机MAC %s\n",s_local_ip,s_nwtype,s_localmac);
    if(s_gw[0]) fprintf(f,"网关 %s %s%s\n",s_gwip,s_gwmac[0]?s_gwmac:"",s_gwmac_vendor[0]?" ":"");
    if(s_dns[0]) fprintf(f,"DNS %s\n",s_dns);
    fprintf(f,"网关延迟 %s\n公网延迟 %s\nIPv6 %s\n",s_gw_rtt,s_pub_rtt,s_ipv6);
    if(s_pub_ip[0]) fprintf(f,"公网 %s %s %s\n",s_pub_ip,s_isp_brand,s_city);
    else fprintf(f,"公网未取到\n");
    fprintf(f,"主机 %d 台\n",s_nhost);
    char ipa[16];
    for(int i=0;i<s_nhost;i++){
        fmt_ip(s_h[i].ip,ipa,sizeof(ipa));
        fprintf(f,"#%d %s %s\n",i+1,ipa,s_h[i].vendor[0]?s_h[i].vendor:"?");
        fprintf(f,"   MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
            s_h[i].mac[0],s_h[i].mac[1],s_h[i].mac[2],s_h[i].mac[3],s_h[i].mac[4],s_h[i].mac[5]);
        for(int pi=0;pi<s_h[i].np;pi++) fprintf(f,"   开放 %d:%s\n",s_h[i].p[pi].port,s_h[i].p[pi].svc);
    }
    fprintf(f,"\n评价: 请查看设备报告页\n");
    fclose(f);
    snprintf(s_export_msg,sizeof(s_export_msg),"已导出 /sdcard/log/netreport.txt");
}
static void nd_start_engine(void){
    s_wait_start=false; s_nhost=0; s_scrollpx=0; s_rptpx=0; s_rpt_nlin=0; s_done=false;
    s_step_done=false; s_step_go=false; s_finished=0; s_view=0; s_step=ST_NET;
    s_gen++; s_stop=false; s_cancel_step=false;
    for(int i=0;i<MAX_HOSTS;i++)memset(&s_h[i],0,sizeof(s_h[i]));
    s_pub_ip[0]=s_isp_brand[0]=s_city[0]=s_net_scenario[0]=s_ipv6[0]=0;
    if(!s_task || eTaskGetState(s_task)==eDeleted){
        BaseType_t ok = xTaskCreate(netdect_run,"netdect",ND_TASK_STACK,NULL,6,&s_task);
        if(ok!=pdPASS) s_task=NULL;   /* 栈分配失败(内部RAM不足)→本代不扫描, 下次重试 */
    }
}
static void nd_do_connect(void){
    wifi_manager_scan_stop();
    vTaskDelay(pdMS_TO_TICKS(500));   /* 等扫描彻底落定, 避免 esp_wifi_connect 在扫描中失败 */
    s_conn=2; s_conn_t0=(uint32_t)(esp_timer_get_time()/1000);
    wifi_manager_connect(s_ssid, s_pass);
}
static void nd_try_connect(ui_ctx_t *ctx,int idx);
static void nd_switch_cb(ui_ctx_t *ctx, int result, void *ud);
static void nd_try_connect(ui_ctx_t *ctx,int idx){
    char ssid[33];
    if(!wifi_manager_get_scan_ssid(idx, ssid, sizeof(ssid)) || !ssid[0]) return;
    /* 已是当前连接的网络 → 直接开始分析 (忽略大小写比较, 避免误进输密码页) */
    if(wifi_manager_is_connected()){
        wifi_ap_record_t ap; memset(&ap,0,sizeof(ap));
        if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){
            size_t l1=strlen(ssid), l2=strlen((char*)ap.ssid);
            if(l1==l2 && l1>0 && strncasecmp(ssid,(char*)ap.ssid,l1)==0){ nd_start_engine(); return; }
        }
    }
    snprintf(s_ssid,sizeof(s_ssid),"%s",ssid);
    const wifi_ap_record_t *rec=wifi_manager_get_scan_records();
    bool open = rec && rec[idx].authmode==WIFI_AUTH_OPEN;
    if(open){ s_pass[0]=0; s_pas=0; nd_do_connect(); }   /* 开放网络直接连 */
    else {
        /* 已保存密码: 命中上次用的 SSID 且有密码 → 自动用历史密码重连, 不弹输密码 */
        char ss[32]="", pp[64]="";
        wifi_manager_get_saved(ss,sizeof(ss),pp,sizeof(pp));
        if(ss[0] && strcmp(ssid,ss)==0 && pp[0]){
            s_pas=(int)strlen(pp); snprintf(s_pass,sizeof(s_pass),"%s",pp);
            nd_do_connect();
        } else {
            /* 需密码(或历史密码不对) → 停扫描, 框内融合输密码 (s_conn==1, 不跳全屏页) */
            wifi_manager_scan_stop();
            s_conn=1; s_pas=0; s_pass[0]=0;
            ctx->needs_redraw=true;
        }
    }
}
/* 切换确认回调: 确认 → 预填SSID进框内输密码 (s_conn==1), 连接后回本页自动分析 */
static void nd_switch_cb(ui_ctx_t *ctx, int result, void *ud){
    (void)ud;
    if(result==0){
        char ssid[33];
        if(wifi_manager_get_scan_ssid(s_pend_idx,ssid,sizeof(ssid)) && ssid[0]){
            snprintf(s_ssid,sizeof(s_ssid),"%s",ssid);
            wifi_manager_scan_stop();   /* 进框内输密码前停扫描 */
            s_conn=1; s_pas=0; s_pass[0]=0;
        }
    }
    ctx->needs_redraw=true;
}
/* 每帧: 连接网络阶段刷新扫描列表 / 检测连接结果; 分析阶段刷新进度显示 */
static void nd_enter_do(ui_ctx_t *ctx);
static void nd_ask_clearpass(ui_ctx_t *ctx);
static void netdect_poll(ui_ctx_t *ctx){
    if(s_enter_pending){                    /* 先显示本页, 满 1s 再弹联网提示/开连接 */
        uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
        if(now - s_enter_t0 >= ND_ENTER_DELAY_MS){
            s_enter_pending=false;
            nd_enter_do(ctx);
        }
        return;
    }
    if(s_use_sys){                           /* 等待自动连接/系统连接流程: 由断到连→自动启动分析 */
        uint32_t tnow=(uint32_t)(esp_timer_get_time()/1000);
        bool c=wifi_manager_is_connected();
        if(c && !s_prev_conn){ s_use_sys=false; nd_start_engine(); ctx->needs_redraw=true; }
        else if(!c && (tnow - s_sys_t0 > ND_AUTOCONN_TIMEOUT || !wifi_manager_is_connecting())){
            /* 未联网且超时/连接已停止: 打开手动选网 UI, 避免一直转圈等 */
            s_use_sys=false;
            settings_wifi_connect_open(ctx);
            ctx->needs_redraw=true;
        }
        s_prev_conn=c;
        return;
    }
    if(s_wait_start){
        uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
        /* 键盘松手恢复白色 */
        if(s_conn==1 && input_has_touch()){
            static bool prev=false; bool down=input_get_touch_pos(NULL,NULL);
            if(!down && prev && s_press_k>=0){ s_press_k=-1; ctx->needs_redraw=true; }
            prev=down;
        }
        if(s_conn==0){
            s_prev_conn=wifi_manager_is_connected();
            /* 仅等 STA 稳定后发一次首扫(不再周期重扫, 避免扫描与连接/认证互相抢) */
            if(!s_scan_started && !wifi_manager_is_connecting()){
                if(now - s_scan_last >= 800){
                    wifi_manager_scan_start();
                    s_scan_started=true;
                    s_scan_last=now;
                }
            }
            static int last=-1; int c=wifi_manager_get_scan_count();
            if(c!=last){ last=c; if(c>0 && s_prompt_sel>=c) s_prompt_sel=c-1; ctx->needs_redraw=true; }
        } else if(s_conn==2){
            if(wifi_manager_is_connected()){ nd_start_engine(); return; }
            if(!wifi_manager_is_connecting() || now - s_conn_t0 > CONNECT_TIMEOUT_MS){
                s_conn=3; s_fail_t0=now; ctx->needs_redraw=true;   /* 连接失败 → 提示 */
            }
        }
        /* s_conn==3: 停留等用户处理(不再自动返回), 点击弹"清空密码重输" */
        return;
    }
    if(s_done){
        /* ---- 报告像素级跟手拖动 (与列表同款: poll 每帧采样触控而非 touch 事件) ---- */
        {   int tx,ty; bool down=input_get_touch_pos(&tx,&ty);
            const int st=24, ys=CT_TOP+2, vh=(CT_BOT-22-ys);
            int maxpx = (s_rpt_nlin*st>vh)? (s_rpt_nlin*st-vh):0;
            if(maxpx<0) maxpx=0;
            static int r0=-1, r0px=0; static bool rdr=false;
            if(down && ty>=CT_TOP && ty<=CT_BOT){
                if(!rdr){ rdr=true; r0=ty; r0px=s_rptpx; }
                else { int off=r0px + (r0-ty); if(off<0)off=0; if(off>maxpx)off=maxpx; if(off!=s_rptpx){ s_rptpx=off; ctx->needs_redraw=true; } }
            } else { rdr=false; }
        }
        return;
    }
    /* ---- 列表像素级跟手拖动 (参考电脑诊断: 在每帧 poll 采样触控, 而非 touch 事件) ---- */
    {   int vs=nd_view_step();
        if(vs==ST_SCAN || vs==ST_PORT || vs==ST_CRED){
            int tx,ty; bool down=input_get_touch_pos(&tx,&ty);
            const int line=18, vh=CT_BOT-CT_TOP-2;
            int cnt = (vs==ST_SCAN)? s_nhost : nd_vis_count();
            int maxpx = (cnt*line>vh)? (cnt*line-vh):0;
            static int dy0p=-1, dpx0p=0; static bool dragp=false;
            if(down && ty>=CT_TOP && ty<=CT_BOT){
                if(!dragp){ dragp=true; dy0p=ty; dpx0p=s_scrollpx; }
                else { int off=dpx0p + (dy0p-ty); if(off<0)off=0; if(off>maxpx)off=maxpx; if(off!=s_scrollpx){ s_scrollpx=off; ctx->needs_redraw=true; } }
            } else { dragp=false; }
        }
    }
    {   /* 分析中: 步切换立即刷新; 同一步内按 33ms 刷新(逐IP顺序显示, 不再跳) */
        static uint32_t lastT=0; static int lastSD=-1; static int lastStep=-1;
        uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
        int sd=(int)s_step_done;
        if(sd!=lastSD || (int)s_step!=lastStep || now-lastT>33){
            lastSD=sd; lastStep=(int)s_step; lastT=now;
            ctx->needs_redraw=true;
        }
    }
}

static void fmt_ip(uint32_t ip, char *b, int cap){ snprintf(b,cap,"%u.%u.%u.%u",(unsigned)((ip>>24)&0xff),(unsigned)((ip>>16)&0xff),(unsigned)((ip>>8)&0xff),(unsigned)(ip&0xff)); }

/* 主机列表渲染: 左IP, 括号=本机或MAC(不发设备名); 品牌右对齐; 像素级平滑滚动 */
static void nd_draw_hostlist(st7305_handle_t *l){
    const int line=18, y0=CT_TOP+2;
    int yoff = s_scrollpx % line;
    int idx  = s_scrollpx / line;
    int y = y0 - yoff;
    for(int i=idx; i<s_nhost && y<=CT_BOT-2; i++, y+=line){
        if(y < y0 || y+line-1 > CT_BOT-2) continue;   /* 越出内容框的行跳过, 屏蔽滑动时界外残留 */
        host_t *h=&s_h[i];
        char ipb[16]; fmt_ip(h->ip,ipb,sizeof(ipb));
        const char *note=NULL; char macnote[24];
        if(h->local) note="\xe6\x9c\xac\xe6\x9c\xba";       /* 本机 */
        else if(h->macok){ snprintf(macnote,sizeof(macnote),"%02X:%02X:%02X:%02X:%02X:%02X",h->mac[0],h->mac[1],h->mac[2],h->mac[3],h->mac[4],h->mac[5]); note=macnote; } /* 完整MAC */
        char left[44];
        if(note) snprintf(left,sizeof(left),"%s (%s)",ipb,note);
        else snprintf(left,sizeof(left),"%s",ipb);
        nd16(l,CT_X0+2,y,left,false);
        if(!h->local && h->macok && h->vendor[0] && h->vendor[0]!='?')
            nd16(l, CT_X1-2-nd16w(h->vendor), y, h->vendor, false);   /* 品牌右对齐 */
    }
}

/* 端口/口令页 共用: 左侧IP(+品牌), 右侧按需的端口/口令文本, 支持s_scrollpx滚动 */
static void nd_draw_hostrows(st7305_handle_t *l, void (*rightfn)(char*,int,const host_t*)){
    const int line=18, y0=CT_TOP+2;
    int yoff = s_scrollpx % line;
    int idx  = s_scrollpx / line;
    int y = y0 - yoff;
    for(int i=idx; i<s_nhost && y<=CT_BOT-2; i++, y+=line){
        if(y < y0 || y+line-1 > CT_BOT-2) continue;   /* 越出内容框的行跳过 */
        host_t *h=&s_h[i];
        char ipb[16]; fmt_ip(h->ip,ipb,sizeof(ipb));
        char r[32]; rightfn(r,sizeof(r),h);
        char row[80];
        if(r[0]) snprintf(row,sizeof(row),"%-12s %s",ipb,r);
        else snprintf(row,sizeof(row),"%s",ipb);
        nd16(l,CT_X0+2,y,row,false);
    }
}
static void nd_right_ports(char*r,int c,const host_t*h){
    int k=0; for(int i=0;i<h->np && i<7 && k<c-1;i++) k+=snprintf(r+k,c-k,"%d/%s ",h->p[i].port,h->p[i].svc[0]?h->p[i].svc:"?");
    if(!k){ r[0]=0; return; } r[k-1]=0;
}
static void nd_right_cred(char*r,int c,const host_t*h){
    if(h->weak) snprintf(r,c,"\xe5\xbc\xb1\xe5\x8f\xa3\xe4\xbb\xa4 %s",h->weaknote);   /* 弱口令 */
    else snprintf(r,c,"\xe6\x97\xa0");                                                  /* 无 */
}

/* 端口/口令页: 仅显示"有开放端口"的主机; 可见位置<->真实host索引映射 */
static int nd_vis_count(void){ int n=0; for(int i=0;i<s_nhost;i++) if(s_h[i].np>0) n++; return n; }
static int nd_vis_idx(int k){ int c=0; for(int i=0;i<s_nhost;i++) if(s_h[i].np>0){ if(c==k) return i; c++; } return -1; }
static void nd_draw_portcred(st7305_handle_t *l, void (*rf)(char*,int,const host_t*)){
    int line=18, y0=CT_TOP+2, vc=nd_vis_count();
    int yoff=s_scrollpx%line, idx=s_scrollpx/line, y=y0-yoff;
    for(int k=idx;k<vc && y<=CT_BOT-2;k++,y+=line){
        if(y < y0 || y+line-1 > CT_BOT-2) continue;   /* 越出内容框的行跳过 */
        int hi=nd_vis_idx(k); if(hi<0) break; host_t *h=&s_h[hi];
        char ipb[16]; fmt_ip(h->ip,ipb,sizeof(ipb)); char r[32]; rf(r,sizeof(r),h);
        char row[80]; if(r[0]) snprintf(row,sizeof(row),"%-12s %s",ipb,r); else snprintf(row,sizeof(row),"%s",ipb);
        nd16(l,CT_X0+2,y,row,false);
    }
}

/* ---- 设备详情弹窗 (点端口页某行) ---- */
EXT_RAM_BSS_ATTR static char  s_dlg_buf[22][52];
EXT_RAM_BSS_ATTR static const char *s_dlg_items[22];
static void nd_host_detail_cb(ui_ctx_t*ctx,int result,void*ud){(void)ctx;(void)result;(void)ud;}
static void nd_show_host_detail(ui_ctx_t *ctx, int hi){
    if(hi<0||hi>=s_nhost) return;
    host_t *h=&s_h[hi];
    int n=0;
    snprintf(s_dlg_buf[n],52,"IP  %u.%u.%u.%u",(unsigned)((h->ip>>24)&0xff),(unsigned)((h->ip>>16)&0xff),(unsigned)((h->ip>>8)&0xff),(unsigned)(h->ip&0xff)); s_dlg_items[n]=s_dlg_buf[n]; n++;
    if(h->macok){
        snprintf(s_dlg_buf[n],52,"MAC %02X:%02X:%02X:%02X:%02X:%02X",h->mac[0],h->mac[1],h->mac[2],h->mac[3],h->mac[4],h->mac[5]); s_dlg_items[n]=s_dlg_buf[n]; n++;
        if(h->vendor[0]&&h->vendor[0]!='?'){ snprintf(s_dlg_buf[n],52,"\xe5\x93\x81\xe7\x89\x8c %s",h->vendor); s_dlg_items[n]=s_dlg_buf[n]; n++; }
    } else { snprintf(s_dlg_buf[n],52,"MAC \xe6\x9c\xaa\xe8\xa7\xa3\xe6\x9e\x90"); s_dlg_items[n]=s_dlg_buf[n]; n++; }
    for(int i=0;i<h->np && n<21;i++){
        snprintf(s_dlg_buf[n],52,"%d/%s %s",h->p[i].port,h->p[i].svc[0]?h->p[i].svc:"?",h->p[i].banner[0]?h->p[i].banner:""); s_dlg_items[n]=s_dlg_buf[n]; n++;
    }
    if(h->weak){ snprintf(s_dlg_buf[n],52,"\xe5\xbc\xb1\xe5\x8f\xa3\xe4\xbb\xa4 %s",h->weaknote); s_dlg_items[n]=s_dlg_buf[n]; n++; }
    if(n>22) n=22;
    os_dialog_list(ctx,"\xe8\xae\xbe\xe5\xa4\x87\xe8\xaf\xa6\xe6\x83\x85",s_dlg_items,n,0,nd_host_detail_cb,NULL); /* 设备详情 */
}

/* ---------------- 渲染 ---------------- */
static void dr_line(st7305_handle_t *l,int x,int y,const char*s,bool inv,int xend){
    if(y<ND_BAR_H) return;
    (void)inv;
    nd16capped(l,x,y,s,xend-x+1);   /* 统一 16px 字体 */
}
/* 信息步实时/摘要展示: 双列, 每条"标题：值"冒号上下对齐; 采到哪个显示哪个 */
static void nd_draw_info(st7305_handle_t *l){
    /* 左列 / 右列 标题+取值 (实时已采的才显示, 未采到显示 "-") */
    const char *ll[10]={ "\xe5\x9c\xb0\xe5\x8c\xba",   /* 地区 */
                         "\xe7\xbd\x91\xe5\x85\xb3",   /* 网关 */
                         "\xe6\x8e\xa9\xe7\xa0\x81",   /* 掩码 */
                         "DNS",
                         "\xe6\x8e\xa5\xe5\x8f\xa3",   /* 接口 */
                         "\xe5\x8a\xa0\xe5\xaf\x86" }; /* 加密 */
    const char *lv[10]={ 0 };
    lv[0]=s_city[0]?s_city:"-";
    lv[1]=s_gw[0]?s_gw:"-";
    lv[2]=s_mask[0]?s_mask:"-";
    lv[3]=s_dns[0]?s_dns:"-";
    lv[4]=s_nwtype[0]?s_nwtype:"-";
    lv[5]=s_auth[0]?s_auth:"-";
    const char *rl[10]={ "\xe8\xbf\x90\xe8\x90\xa5\xe5\x95\x86", /* 运营商 */
                         "\xe5\x85\xac\xe7\xbd\x91IP",            /* 公网IP */
                         "\xe6\x9c\xac\xe6\x9c\xbaIP",            /* 本机IP */
                         "\xe5\x85\xac\xe7\xbd\x91\xe5\xbb\xb6\xe8\xbf\x9f", /* 公网延迟 */
                         "\xe7\xbd\x91\xe5\x85\xb3\xe5\xbb\xb6\xe8\xbf\x9f", /* 网关延迟 */
                         "IPv6" };
    const char *rv[10]={ 0 };
    rv[0]=s_isp_brand[0]?s_isp_brand:(s_isp[0]?s_isp:"-");
    rv[1]=s_pub_ip[0]?s_pub_ip:"-";
    rv[2]=s_local_ip[0]?s_local_ip:"-";
    rv[3]=s_pub_rtt[0]?s_pub_rtt:"-";
    rv[4]=s_gw_rtt[0]?s_gw_rtt:"-";
    rv[5]=s_ipv6[0]?s_ipv6:"-";
    int nl=6, nr=6;
    int mL=0,mR=0;
    for(int i=0;i<nl;i++){ int w=nd16w(ll[i]); if(w>mL)mL=w; }
    for(int i=0;i<nr;i++){ int w=nd16w(rl[i]); if(w>mR)mR=w; }
    const int lx=CT_X0, rx=CT_X0+182;   /* 右列右移2个中文, 左列值更宽 */
    const char *colon="\xef\xbc\x9a";   /* ： */
    const int lvalx=lx+mL+22, rvalx=rx+mR+22;   /* 值放到冒号(满宽~16)+间隙之后, 避免压在冒号上 */
    const int lvalw=(rx-6)-lvalx, rvalw=(CT_X1-6)-rvalx;
    int M = nl>nr?nl:nr;
    int y=CT_TOP+2;
    for(int i=0;i<M;i++){
        if(i<nl){
            nd16(l,lx,y,ll[i],false);
            nd16(l,lx+mL,y,colon,false);
            if(lvalw>0) nd16capped(l,lvalx,y,lv[i],lvalw);
        }
        if(i<nr){
            nd16(l,rx,y,rl[i],false);
            nd16(l,rx+mR,y,colon,false);
            if(rvalw>0) nd16capped(l,rvalx,y,rv[i],rvalw);
        }
        y+=18;
    }
    /* 底部居中: 路由MAC / 网关MAC / 本机MAC 三行 */
    { char mb[52];
      const char *rm=s_gwmac_vendor[0]?s_gwmac_vendor:s_gwmac;      /* 路由MAC: 厂商优先 */
      snprintf(mb,sizeof(mb),"\xe8\xb7\xaf\xe7\x94\xb1MAC %s",rm[0]?rm:"-"); /* 路由MAC */
      nd16(l,(NW-nd16w(mb))/2, CT_BOT-60, mb, false);
      snprintf(mb,sizeof(mb),"\xe7\xbd\x91\xe5\x85\xb3MAC %s",s_gwmac[0]?s_gwmac:"-"); /* 网关MAC */
      nd16(l,(NW-nd16w(mb))/2, CT_BOT-42, mb, false);
      snprintf(mb,sizeof(mb),"\xe6\x9c\xac\xe6\x9c\xbaMAC %s",s_localmac[0]?s_localmac:"-"); /* 本机MAC */
      nd16(l,(NW-nd16w(mb))/2, CT_BOT-24, mb, false); }
}
/* 左下右上: 左键(第一页=刷新/分析=上一步) + 下一步, 文字贴最左/最右/最底(三边距10px), 无方框;
 * 盖子: 左右上横杠贴屏幕边框, 斜坡内移到离文字10px处, 接通到距底5px谷线 */
static void nd_draw_nav(st7305_handle_t *l, bool refresh_left, bool export_right, bool cancel_right){
    const char *up = refresh_left ? "\xe5\x88\xb7\xe6\x96\xb0" : "\xe4\xb8\x8a\xe4\xb8\x80\xe6\xad\xa5";  /* 刷新 / 上一步 */
    const char *dn = cancel_right ? "\xe5\x8f\x96\xe6\xb6\x88" : (export_right ? "\xe5\xaf\xbc\xe5\x87\xba" : "\xe4\xb8\x8b\xe4\xb8\x80\xe6\xad\xa5");  /* 取消 / 导出 / 下一步 */
    int ty=NH-10-16+3;                     /* 文字顶 y (比 10px 距底再下移 3px) */
    int wu=nd16w(up), wd=nd16w(dn);
    int xu=NBTN_L0, xd=NBTN_R1-wd+1;        /* 左键左缘10 / 下一步右缘10 */
    nd16(l, xu, ty, up, false);
    nd16(l, xd, ty, dn, false);
    /* 盖子/坡度 */
    int lidY=ty-4, valleyY=NH-6, slope=valleyY-lidY;
    int Lend=xu+wu+10;                      /* 左斜坡起点: 文字右侧 10px */
    int Rstart=xd-10;                       /* 右斜坡起点: 文字左侧 10px */
    nd_hline(l, 0,     Lend,   lidY);       /* 左横杠: 贴左屏幕边框 */
    nd_hline(l, Rstart, NW-1,   lidY);      /* 右横杠: 贴右屏幕边框 */
    int vl=Lend+slope, vr=Rstart-slope;
    if(vl<vr) nd_hline(l, vl, vr, valleyY); /* 底部谷线 */
    for(int d=0; d<=slope && d<40; d++){    /* 斜坡 */
        st7305_draw_pixel(l, Lend+d, lidY+d, ST7305_COLOR_BLACK);
        st7305_draw_pixel(l, Rstart-d, lidY+d, ST7305_COLOR_BLACK);
    }
}
/* ---- 评分/环境推断/组合点评建议 (报告) ---- */
static void nd_append_eval(char lines[][72], int *nlin){
    int nhost=s_nhost, weak=0, ports=0, hi_risk=0;
    bool open = s_auth[0] && strstr(s_auth,"\xe5\xbc\x80\xe6\x94\xbe"); /* 开放 */
    for(int i=0;i<nhost;i++){ host_t*h=&s_h[i]; ports+=h->np; if(h->weak)weak++;
        for(int pi=0;pi<h->np;pi++){ uint16_t p=h->p[pi].port;
            if(p==445||p==3389||p==23||p==21||p==2333||p==5900||p==3306||p==5432) hi_risk++; } }
    /* 环境推断 (按主机数+设备特点) */
    const char *env;
    if(nhost<=3) env="\xe7\xbb\x9d\xe5\xb0\x8f\xe5\x9e\x8b\xe5\xae\xb6\xe5\xba\xad\xe7\xbd\x91\xe7\xbb\x9c"; /* 极小型家庭网络 */
    else if(nhost<=8) env="\xe5\xb0\x8f\xe5\x9e\x8b\xe5\xae\xb6\xe5\xba\xad/\xe4\xb8\xaa\xe4\xba\xba\xe5\x8a\x9e\xe5\x85\xac\xe7\xbd\x91\xe7\xbb\x9c"; /* 小型家庭/个人办公网络 */
    else if(nhost<=15) env="\xe6\x99\xae\xe9\x80\x9a\xe5\xae\xb6\xe5\xba\xad\xe6\x88\x96\xe5\xb0\x8f\xe5\x9e\x8b\xe5\x8a\x9e\xe5\x85\xac\xe5\xae\xa4,\xe6\x99\xba\xe8\x83\xbd\xe5\xae\xb6\xe5\xb1\x85/\xe5\x8a\x9e\xe5\x85\xac\xe8\xae\xbe\xe5\xa4\x87\xe6\xb7\xb7\xe5\x90\x88"; /* 普通家庭或小型办公室 */
    else if(nhost<=40) env="\xe5\xb0\x8f\xe5\x9e\x8b\xe4\xbc\x81\xe4\xb8\x9a/\xe4\xb8\xad\xe5\x9e\x8b\xe5\x8a\x9e\xe5\x85\xac\xe7\xbd\x91\xe7\xbb\x9c,\xe6\x9c\x89\xe4\xb8\x80\xe5\xae\x9a\xe8\xa7\x84\xe6\xa8\xa1"; /* 小型企业/中型办公网络 */
    else if(nhost<=100) env="\xe4\xb8\xad\xe5\x9e\x8b\xe6\x9c\xba\xe6\x9e\x84/\xe5\x9b\xad\xe5\x8c\xba\xe7\xbd\x91\xe7\xbb\x9c,\xe5\xbb\xba\xe8\xae\xae\xe5\x88\x86\xe5\x8c\xba\xe9\x9a\x94\xe7\xa6\xbb"; /* 中型机构/园区网络 */
    else env="\xe5\xa4\xa7\xe5\x9e\x8b/\xe5\xa4\x9a\xe5\x88\x86\xe6\x94\xaf\xe7\xbd\x91\xe7\xbb\x9c,\xe5\xbb\xba\xe8\xae\xae\xe4\xb8\x93\xe4\xb8\x9a\xe9\x98\xb2\xe7\x81\xab\xe5\xa2\x99+VLAN"; /* 大型/多分支网络 */
    /* 评分 */
    int score=100;
    if(nhost<=1) score-=5;
    if(open) score-=25;
    if(weak>0) score-=weak*12;
    if(hi_risk>6) score-=12; else if(hi_risk>0) score-=6;
    if(ports>15) score-=6; else if(ports>6) score-=3;
    if(!s_pub_ip[0]) score-=6;
    if(score<0) score=0;
    const char *grade = score>=92?"\xe4\xbc\x98\xe7\xa7\x80 A":score>=80?"\xe8\x89\xaf\xe5\xa5\xbd B":score>=65?"\xe4\xb8\xad\xe7\xad\x89 C":score>=45?"\xe4\xba\xa4\xe5\xb7\xae D":"\xe5\x8d\xb1\xe9\x99\xa9 E"; /* 优秀A..危险E */
    int nk=*nlin;
    snprintf(lines[nk++],72,"---- \xe8\xaf\x84\xe5\x88\x86 ----"); /* 评分 */
    snprintf(lines[nk++],72,"\xc2\xb7 \xe7\xbd\x91\xe7\xbb\x9c\xe5\xae\x89\xe5\x85\xa8\xe8\xaf\x84\xe5\x88\x86: %d/100\xe5\x88\x86",score); /* 评分 X/100分 */
    snprintf(lines[nk++],72,"\xc2\xb7 \xe7\xad\x89\xe7\xba\xa7: %s (%d\xe5\x88\x86)", grade,score); /* · 等级: xx (X分) */
    snprintf(lines[nk++],72,"---- \xe7\x8e\xaf\xe5\xa2\x83\xe5\x88\xa4\xe6\x96\xad ----"); /* 环境判断 */
    snprintf(lines[nk++],72,"\xc2\xb7 \xe6\x8e\xa8\xe6\x96\xad: %s",env); /* · 推断: */
    snprintf(lines[nk++],72,"  (\xe5\x85\xb1 %d \xe5\x8f\xb0\xe8\xae\xbe\xe5\xa4\x87, %d \xe4\xb8\xaa\xe5\xbc\x80\xe6\x94\xbe\xe7\xab\xaf\xe5\x8f\xa3)",nhost,ports); /* (共N台设备, X个开放端口) */
    /* 组合点评 (按情况给不同模板) */
    snprintf(lines[nk++],72,"---- \xe7\x82\xb9\xe8\xaf\x84 ----"); /* 点评 */
    if(nhost<=3) snprintf(lines[nk++],72,"\xc2\xb7 \xe8\xae\xbe\xe5\xa4\x87\xe5\xbe\x88\xe5\xb0\x91,\xe7\xae\xa1\xe7\x90\x86\xe7\xae\x80\xe5\x8d\x95,\xe6\xb3\xa8\xe6\x84\x8f\xe8\xb7\xaf\xe7\x94\xb1\xe5\x99\xa8\xe4\xb8\x80\xe4\xbd\x93\xe7\xae\xa1\xe7\x90\x86\xe5\x8d\xb3\xe5\x8f\xaf"); /* 设备很少,管理简单 */
    else if(nhost<=15) snprintf(lines[nk++],72,"\xc2\xb7 \xe5\xae\xb6\xe5\xba\xad/\xe5\xb0\x8f\xe5\x8d\x95\xe4\xbd\x8d\xe7\xbd\x91\xe7\xbb\x9c,\xe9\x87\x8d\xe7\x82\xb9\xe5\x85\xb3\xe6\xb3\xa8\xe6\x99\xba\xe8\x83\xbd\xe4\xb8\x8e\xe6\x91\x84\xe5\x83\x8f\xe5\xa4\xb4\xe5\xae\x89\xe5\x85\xa8"); /* 家庭/小单位,关注智能与摄像头 */
    else snprintf(lines[nk++],72,"\xc2\xb7 \xe8\xae\xbe\xe5\xa4\x87\xe8\xbe\x83\xe5\xa4\x9a,\xe5\xbb\xba\xe8\xae\xae\xe5\xae\x9e\xe6\x96\xbd VLAN \xe5\x88\x86\xe5\x8c\xba\xe4\xb8\x8e\xe6\x9d\x83\xe9\x99\x90\xe6\x8e\xa7\xe5\x88\xb6"); /* 设备较多,建议VLAN分区 */
    if(open) snprintf(lines[nk++],72,"\xc2\xb7 \xe6\x98\xaf\xe5\xbc\x80\xe6\x94\xbe\xe7\xbd\x91\xe7\xbb\x9c,\xe4\xbb\xbb\xe4\xbd\x95\xe4\xba\xba\xe5\x8f\xaf\xe8\xbf\x9e,\xe9\xa3\x8e\xe9\x99\xa9\xe9\xab\x98"); /* 是开放网络,风险高 */
    if(weak>0) snprintf(lines[nk++],72,"\xc2\xb7 \xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0 %d \xe4\xb8\xaa\xe5\xbc\xb1\xe5\x8f\xa3\xe4\xbb\xa4,\xe5\xbb\xba\xe8\xae\xae\xe5\x85\xb3\xe9\x97\xad\xe5\xaf\xb9\xe5\xba\x94 FTP/Telnet \xe6\x9c\x8d\xe5\x8a\xa1\xe5\xb9\xb6\xe6\x94\xb9\xe5\xbc\xba\xe5\xaf\x86\xe7\xa0\x81",weak);
    if(hi_risk>0) snprintf(lines[nk++],72,"\xc2\xb7 \xe5\x90\xab %d \xe4\xb8\xaa\xe9\xab\x98\xe9\xa3\x8e\xe9\x99\xa9\xe7\xab\xaf\xe5\x8f\xa3(23/21/445/3389/2333\xe7\xad\x89),\xe5\xbb\xba\xe8\xae\xae\xe5\x85\xb3\xe9\x97\xad\xe6\x88\x96\xe9\x99\x90\xe6\x9d\xa5\xe6\xba\x90",hi_risk);
    /* 风险统计 */
    snprintf(lines[nk++],72,"---- \xe9\xa3\x8e\xe9\x99\xa9\xe7\xbb\x9f\xe8\xae\xa1 ----"); /* 风险统计 */
    int hi=hi_risk + (open?1:0); int mi=(ports>10?1:0)+(weak>0?1:0); int lo=(nhost>0?1:0);
    snprintf(lines[nk++],72,"\xc2\xb7 \xe9\xab\x98\xe5\x8d\xb1 %d / \xe4\xb8\xad\xe5\x8d\xb1 %d / \xe4\xbd\x8e\xe5\x8d\xb1 %d",hi,mi,lo);
    snprintf(lines[nk++],72,"\xc2\xb7 \xe5\xbc\xb1\xe5\x8f\xa3\xe4\xbb\xa4 %d | \xe5\xbc\x80\xe6\x94\xbe\xe7\xab\xaf\xe5\x8f\xa3 %d | \xe9\xab\x98\xe5\x8d\xb1\xe7\xab\xaf\xe5\x8f\xa3 %d",weak,ports,hi_risk);
    snprintf(lines[nk++],72,"\xc2\xb7 \xe5\x85\xac\xe7\xbd\x91: %s | IPv6: %s", s_pub_ip[0]?"\xe5\x8f\xaf\xe8\xbe\xbe":"\xe6\x9c\xaa\xe8\x8e\xb7\xe5\x8f\x96", s_ipv6[0]?s_ipv6:"-"); /* 公网: 可达/未获取 */
    /* 主要发现 (OpenVAS风格) */
    snprintf(lines[nk++],72,"---- \xe4\xb8\xbb\xe8\xa6\x81\xe5\x8f\x91\xe7\x8e\xb0 ----"); /* 主要发现 */
    if(weak>0) snprintf(lines[nk++],72,"\xc2\xb7 \xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0\xe5\xbc\xb1\xe5\x8f\xa3\xe4\xbb\xa4,\xe6\x9c\x89\xe8\xa2\xab\xe7\x9b\x9f\xe7\x81\xaf\xe5\x85\xa5\xe4\xbe\xb5\xe9\xa3\x8e\xe9\x99\xa9,\xe5\xbb\xba\xe8\xae\xae\xe7\xab\x8b\xe5\x8d\xb3\xe4\xbf\xae\xe6\x94\xb9"); /* 弱口令,有被爆破风险 */
    if(open) snprintf(lines[nk++],72,"\xc2\xb7 \xe6\x97\xa0\xe7\xba\xbf\xe4\xb8\xba\xe5\xbc\x80\xe6\x94\xbe\xe7\xbd\x91\xe7\xbb\x9c,\xe4\xbb\xbb\xe4\xbd\x95\xe4\xba\xba\xe5\x8f\xaf\xe8\xbf\x9e,\xe9\xa3\x8e\xe9\x99\xa9\xe8\xbe\x83\xe9\xab\x98"); /* 开放网络 */
    if(hi_risk>0) snprintf(lines[nk++],72,"\xc2\xb7 \xe5\xbc\x80\xe6\x94\xbe\xe4\xba\x86 SMB/RDP/Telnet \xe7\xad\x89\xe7\x9b\xae\xe6\xa0\x87\xe7\xab\xaf\xe5\x8f\xa3,\xe5\x86\x85\xe7\xbd\x91\xe8\xa2\xad\xe5\x87\xbb\xe9\x9d\xa2\xe5\xa2\x9e\xe5\xa4\xa7"); /* 高危端口 */
    if(!s_pub_ip[0]) snprintf(lines[nk++],72,"\xc2\xb7 \xe6\x9c\xaa\xe8\x8e\xb7\xe5\x8f\x96\xe5\x85\xac\xe7\xbd\x91IP,\xe5\x8f\xaf\xe8\x83\xbd\xe5\xa4\x84\xe4\xba\x8e NAT \xe8\x83\x8c\xe5\x90\x8e,\xe5\xa4\x96\xe9\x83\xa8\xe7\x9b\xb4\xe8\xbf\x9e\xe9\x9a\xbe\xe5\xba\xa6\xe5\xa4\xa7"); /* 处于NAT */
    if(s_ipv6[0] && strstr(s_ipv6,"\xe6\x94\xaf\xe6\x8c\x81")) snprintf(lines[nk++],72,"\xc2\xb7 \xe7\xbd\x91\xe7\xbb\x9c\xe6\x94\xaf\xe6\x8c\x81 IPv6,\xe8\xaf\xb7\xe7\xa1\xae\xe8\xae\xa4\xe8\xb7\xaf\xe7\x94\xb1\xe5\x99\xa8\xe9\x98\xb2\xe7\x81\xab\xe5\x9f\xba\xe6\x9c\xac\xe5\xb7\xb2\xe9\x85\x8d\xe5\xa5\xbd"); /* 支持IPv6 */
    /* 标准参考 */
    snprintf(lines[nk++],72,"\xc2\xb7 \xe5\x8f\x82\xe7\x85\xa7: \xe7\xad\x89\xe4\xbf\x9d2.0 / NIST / CIS \xe5\xae\x89\xe5\x85\xa8\xe5\x9f\xba\xe7\xba\xbf"); /* 参照标准 */
    /* 推荐方案 */
    snprintf(lines[nk++],72,"---- \xe6\x8e\xa8\xe8\x8d\x90 ----"); /* 推荐 */
    snprintf(lines[nk++],72,"\xc2\xb7 \xe8\xb7\xaf\xe7\x94\xb1\xe5\x99\xa8\xe7\x94\xa8\xe5\xbc\xba\xe5\xaf\x86\xe7\xa0\x81+WPA2/WPA3"); /* 路由器强密码 */
    snprintf(lines[nk++],72,"\xc2\xb7 \xe5\x85\xb3\xe9\x97\xad\xe6\x97\xa0\xe7\x94\xa8\xe7\xab\xaf\xe5\x8f\xa3\xe4\xb8\x8e\xe8\xbf\x9c\xe7\xa8\x8b\xe7\xbb\xb4\xe6\x8a\xa4\xe9\x80\x9a\xe9\x81\x93"); /* 关闭无用端口与远程维护通道 */
    snprintf(lines[nk++],72,"\xc2\xb7 \xe5\xbc\x80\xe5\x90\xaf\xe5\xae\xa2\xe6\x88\xb7\xe7\xab\xaf\xe9\x9a\x94\xe7\xa6\xbb,\xe9\x99\x90\xe5\x88\xb6\xe5\x86\x85\xe7\xbd\x91\xe4\xba\x92\xe8\xae\xbf"); /* 开启客户端隔离,限制内网互访 */
    if(hi_risk>0) snprintf(lines[nk++],72,"\xc2\xb7 \xe5\xaf\xb9 445/3389 \xe7\xad\x89\xe5\xbc\x80\xe6\x94\xbe\xe5\xa5\x87\xe5\xae\x87\xe8\xbf\x9b\xe8\xa1\x8c\xe6\x89\x93\xe8\xa1\xa5\xe4\xb8\x8e\xe9\x99\x90\xe6\x9d\xa5\xe6\xba\x90"); /* 对高危端口封堵 */
    snprintf(lines[nk++],72,"\xc2\xb7 \xe5\xae\x9a\xe6\x9c\x9f\xe6\x9b\xb4\xe6\x96\xb0\xe5\x9b\xba\xe4\xbb\xb6/\xe9\x87\x8d\xe6\x96\xb0\xe6\x8e\x92\xe6\x9f\xa5\xe6\x9c\xaa\xe7\x9f\xa5\xe8\xae\xbe\xe5\xa4\x87"); /* 定期更新固件/排查未知设备 */
    if(nhost<=15) snprintf(lines[nk++],72,"\xc2\xb7 \xe5\xbb\xba\xe8\xae\xae\xe5\xa2\x9e\xe8\xae\xbe: \xe5\x8f\xaf\xe6\xb7\xbb NAS/NVR(\xe9\x87\x8d\xe8\xa6\x81\xe6\x95\xb0\xe6\x8d\xae\xe5\xa4\x87\xe4\xbb\xbd)"); /* 建议设备: NAS/NVR */
    else snprintf(lines[nk++],72,"\xc2\xb7 \xe5\xbb\xba\xe8\xae\xae\xe5\xa2\x9e\xe8\xae\xbe: \xe4\xbc\x81\xe4\xb8\x9a\xe7\xba\xa7\xe9\x98\xb2\xe7\x81\xab\xe5\xa2\x99+Ac\xe5\x8a\xa0\xe6\x8e\xa7\xe4\xba\xa4\xe6\x8d\xa2\xe6\x9c\xba/\xe5\xae\x89\xe5\x85\xa8\xe4\xba\x8b\xe4\xbb\xb6\xe7\xae\xa1\xe7\x90\x86"); /* 建议设备: 防火墙+AC+交换机/安管 */
    *nlin=nk;
}

static void netdect_render(ui_ctx_t *ctx){
    st7305_handle_t*l=ctx->lcd; if(!l) return;
    st7305_clear(l,ST7305_COLOR_WHITE);
    draw_stepbar(l, s_step_done ? (step_t)s_view : (step_t)s_step);   /* 暂停浏览时进度条高亮 s_view */
    /* 恒定内容框: 所有页面 (联网/扫描/端口/报告) 都显示, 内容都画在框内 */
    pf(l,CT_X0,CT_TOP,CT_X1,CT_TOP,0); pf(l,CT_X0,CT_BOT,CT_X1,CT_BOT,0);
    pf(l,CT_X0,CT_TOP,CT_X0,CT_BOT,0); pf(l,CT_X1,CT_TOP,CT_X1,CT_BOT,0);

    /* 入口: 连接网络 (Wi-Fi 扫描列表 → 输密码 → 连接) */
    if(s_wait_start){
        draw_stepbar(l,ST_NET);
        if(s_conn==1){
            /* 融合: 上半 SSID + 密码明文, 下半经典键盘, 全 16px (英文 8×16), 都在恒定框内 */
            int cx=(CT_X0+CT_X1)/2;
            nd16c(l,cx,CT_TOP+4, s_ssid[0]?s_ssid:"...", false);
            char pp[40]; snprintf(pp,sizeof(pp),"\xe5\xaf\x86\xe7\xa0\x81:%s", s_pass); /* 密码:明文 */
            nd16c(l,cx,CT_TOP+30, pp, false);
            pf(l,CT_X0+4,CT_TOP+58,CT_X1-4,CT_TOP+58,0);   /* 分隔线 */
            nd_kbd_draw(l, s_press_k);   /* 经典 5 排 16px 键盘 (回车=连接) */
        } else {
            /* 网络列表常驻 + 底部导航; 正在连接/失败等提示无需弹窗, 放下面横框上方 */
            int cnt=wifi_manager_get_scan_count();
            char cur[33]="";
            wifi_ap_record_t ap; if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){ snprintf(cur,sizeof(cur),"%s",ap.ssid); }
            int bx0=CT_X0, bx1=CT_X1, top=CT_TOP+2, bot=CT_BOT-2, rh=26;   /* 用恒定内容框 */
            if(cnt>0){
                int y=top+4;
                for(int i=0;i<cnt;i++){
                    if(y+rh+2>bot) break;
                    char ssid[33]; if(!wifi_manager_get_scan_ssid(i,ssid,sizeof(ssid))||!ssid[0]) snprintf(ssid,33,"?");
                    bool conn = cur[0] && strcmp(ssid,cur)==0;
                    nd16(l,(NW-nd16w(ssid))/2,y+3,ssid,false);
                    if(conn) nd16(l,bx1-6-nd16w("\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5"),y+3,"\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5",false); /* 已连接 */
                    y+=rh;
                }
            }
            nd_draw_nav(l,true,false,false);   /* 第一页: 左键=刷新 */
            /* 提示: 连接中 / 失败 / 扫描, 一律在下面横框上方显示 */
            char tip[64]; const char *tp=NULL;
            if(s_conn==2){ snprintf(tip,sizeof(tip),"\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5 %s...",s_ssid); tp=tip; }      /* 正在连接 */
            else if(s_conn==3){ tp="\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5\x20-\xe7\x82\xb9\xe5\x87\xbb\xe6\xb8\x85\xe7\xa9\xba\xe5\xaf\x86\xe7\xa0\x81\xe9\x87\x8d\xe6\x96\xb0\xe8\xbe\x93\xe5\x85\xa5"; }   /* 连接失败 - 点击清空密码重新输入 */
            else if(cnt<=0){ tp="\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f WiFi..."; }                                       /* 正在扫描 */
            if(tp){ nd16(l,(NW-nd16w(tp))/2, NH-10-16, tp, false); }  /* 提示: 底部中间, 距底10px */
        }
        return;
    }
    if(s_use_sys){   /* 等待 WiFi 自动连接: 简化提示, 连上即启动分析 */
        const char *wtext="\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5\xe7\xbd\x91\xe7\xbb\x9c...";   /* 正在连接网络... */
        nd_draw_nav(l,false,false,false);
        nd16(l,(NW-nd16w(wtext))/2, NH-10-16, wtext, false);   /* 提示统一放底部中间 */
        return;
    }
    if(!s_done){
        int vv=nd_view_step();
        if(vv<0){ vv=0; } if(vv>ST_TOTAL-1){ vv=ST_TOTAL-1; }
        const char *tip=NULL; char tipb[64];
        char cbuf[32];
        if(vv==ST_IPINFO || vv==ST_NET){ nd_draw_info(l); }                                     /* 网络信息框架 */
        else if(vv==ST_SCAN){ nd_draw_hostlist(l);                                              /* 主机列表 */
            snprintf(cbuf,sizeof(cbuf),"\xe5\x8f\x91\xe7\x8e\xb0\xe4\xb8\xbb\xe6\x9c\xba %d \xe5\x8f\xb0",s_nhost); /* 发现主机 N 台 */
            tip=cbuf;
        } else if(vv==ST_PORT){                                                  /* 端口: 左IP右端口 */
            /* 扫描进行中: 先显示 IP 页扫到的全部主机; 完成后隐藏没有开放端口的主机 */
            if(s_step==ST_PORT && !s_step_done) nd_draw_hostrows(l, nd_right_ports);
            else                                 nd_draw_portcred(l, nd_right_ports);
            snprintf(cbuf,sizeof(cbuf),"\xe5\xbc\x80\xe6\x94\xbe\xe7\xab\xaf\xe5\x8f\xa3 - \xe7\x82\xb9\xe8\xa1\x8c\xe7\x9c\x8b\xe8\xaf\xa6\xe6\x83\x85"); /* 开放端口-点行看详情 */
            tip=cbuf;
        } else if(vv==ST_CRED){ nd_draw_portcred(l, nd_right_cred);                             /* 口令: 左IP右弱口令测试结果 */
            int wc=0; for(int i=0;i<s_nhost;i++) if(s_h[i].weak) wc++;
            snprintf(cbuf,sizeof(cbuf),"\xe5\xbc\xb1\xe5\x8f\xa3\xe4\xbb\xa4 %d \xe4\xb8\xaa - \xe5\xbb\xba\xe8\xae\xae\xe5\x85\xb3\xe9\x97\xad\xe5\xaf\xb9\xe5\xba\x94\xe6\x9c\x8d\xe5\x8a\xa1",wc); /* 弱口令N个-建议关闭对应服务 */
            tip=cbuf;
        } else { tip=s_status; }
        if(!s_step_done){ tip=s_status; }   /* 执行中: 底部显示当前运行进度 */
        nd_draw_nav(l,false,false,!s_step_done);
        if(tip) nd16(l,(NW-nd16w(tip))/2, NH-10-16, tip, false);   /* 下面中间提示位 */
    } else {
        /* 报告滚动 (缓冲放PSRAM, 避免大数组挤爆主任务栈) */
        EXT_RAM_BSS_ATTR static char lines[64][72]; int nlin=0;
        snprintf(lines[nlin++],72,"网络分析报告");
        if(s_essaid[0]) snprintf(lines[nlin++],72,"无线 %s [%s]",s_essaid,s_auth[0]?s_auth:"?");
        snprintf(lines[nlin++],72,"本机 %s  %s",s_local_ip,s_nwtype);
        if(s_localmac[0]) snprintf(lines[nlin++],72,"本机MAC %s",s_localmac);
        if(s_gw[0]) snprintf(lines[nlin++],72,"网关 %s %s%s",s_gwip,s_gwmac[0]?s_gwmac:"",s_gwmac_vendor[0]?" ":"");
        if(s_gwmac_vendor[0]) snprintf(lines[nlin++],72,"  厂商 %s",s_gwmac_vendor);
        if(s_dns[0]) snprintf(lines[nlin++],72,"DNS %s",s_dns);
        if(s_gw_rtt[0]) snprintf(lines[nlin++],72,"网关延迟 %s",s_gw_rtt);
        if(s_pub_rtt[0]) snprintf(lines[nlin++],72,"公网延迟 %s",s_pub_rtt);
        if(s_ipv6[0]) snprintf(lines[nlin++],72,"IPv6 %s",s_ipv6);
        if(s_pub_ip[0]) snprintf(lines[nlin++],72,"公网 %s %s %s",s_pub_ip,s_isp_brand,s_city);
        else snprintf(lines[nlin++],72,"公网未取到(可能未联网)");
        snprintf(lines[nlin++],72,"主机 %d 台",s_nhost);
        for(int hi=0;hi<s_nhost && nlin<60;hi++){
            host_t*h=&s_h[hi];
            char ipa[16]; fmt_ip(h->ip,ipa,sizeof(ipa));
            char ipd[32]; if(h->local) snprintf(ipd,sizeof(ipd),"%s (\xe6\x9c\xac\xe6\x9c\xba)",ipa); else if(h->hname[0]) snprintf(ipd,sizeof(ipd),"%s (%s)",ipa,h->hname); else snprintf(ipd,sizeof(ipd),"%s",ipa);   /* (本机)/(主机名) */
            char macs[20]; if(h->macok) snprintf(macs,20,"%02X:%02X:%02X:%02X:%02X:%02X",h->mac[0],h->mac[1],h->mac[2],h->mac[3],h->mac[4],h->mac[5]); else snprintf(macs,20,"--");
            snprintf(lines[nlin++],72,"#%d %s",hi+1,ipd);
            snprintf(lines[nlin++],72,"  %s %s",macs,h->vendor[0]?h->vendor:"?");
            char ps[60]; ps[0]=0; for(int pi=0;pi<h->np&&pi<3;pi++){ char b[14]; snprintf(b,14,"%d:%s ",h->p[pi].port,h->p[pi].svc); if(strlen(ps)+strlen(b)<sizeof(ps)) strcat(ps,b); }
            snprintf(lines[nlin++],72,"  开放:%s",ps[0]?ps:"(无)");
            if(h->weak) snprintf(lines[nlin++],72,"  !弱口令 %s",h->weaknote);
        }
        /* 评分/环境推断/组合点评与推荐 */
        nd_append_eval(lines, &nlin);
        if(nlin&1){ lines[nlin][0]=0; nlin++; }
        {   /* 报告: 像素级滚动 (s_rptpx), 总行数缓存给 poll 算可滚范围 */
            const int st=24, ys=CT_TOP+2, bot=CT_BOT-22;
            int vh=bot-ys; int maxpx=(nlin*st>vh)?(nlin*st-vh):0; if(maxpx<0)maxpx=0;
            s_rpt_nlin=nlin;
            if(s_rptpx>maxpx) s_rptpx=maxpx;
            if(s_rptpx<0)     s_rptpx=0;
            int idx=s_rptpx/st, yoff=s_rptpx%st;
            for(int i=idx, y=ys-yoff; i<nlin && y<=bot; i++, y+=st){
                if(y < ys || y+st-1 > bot) continue;   /* 越出内容框的行跳过 */
                dr_line(l,CT_X0+2,y,lines[i],false,CT_X1);
            }
            char fh[48]; snprintf(fh,sizeof(fh),"UP/DOWN %d/%d", idx+1, nlin);
            nd16(l,CT_X0+2,bot,fh,false);
        }
        nd_draw_nav(l,false,true,false);   /* 报告页: 右键=导出 */
        const char *xt = s_export_msg[0] ? s_export_msg : "\xe7\x94\xa8\xe3\x80\x8c\xe5\xaf\xbc\xe5\x87\xba\xe3\x80\x8d\xe5\xad\x98\xe8\xbf\x9bTF\xe5\x8d\xa1"; /* 用「导出」存进TF卡 */
        nd16(l,(NW-nd16w(xt))/2, NH-10-16, xt, false);
    }
}

static void netdect_action(ui_ctx_t*ctx,os_action_t a){
    if(s_wait_start && a==OS_ACTION_BACK){
        /* 密码/连接中/失败 → 返回列表 (保留扫描结果, 不重扫); 列表 → 退出页面 */
        if(s_conn==1 || s_conn==2 || s_conn==3){
            s_conn=0; s_ssid[0]=0; s_pass[0]=0; s_pas=0;
            ctx->needs_redraw=true; return;
        }
        os_pop(ctx); return;
    }
    if(a==OS_ACTION_BACK||a==OS_ACTION_HOME){ os_pop(ctx); return; }
    if(s_wait_start){
        if(s_conn==0){
            int c=wifi_manager_get_scan_count(); if(c>10)c=10;
            if(a==OS_ACTION_UP && s_prompt_sel>0) s_prompt_sel--;
            if(a==OS_ACTION_DOWN && c>0 && s_prompt_sel<c-1) s_prompt_sel++;
            if(a==OS_ACTION_CONFIRM){ if(wifi_manager_is_connected()) nd_start_engine(); else if(c>0) nd_try_connect(ctx,s_prompt_sel); }
        } else if(s_conn==1){
            if(a==OS_ACTION_CONFIRM){ nd_do_connect(); }
        } else if(s_conn==3){
            if(a==OS_ACTION_CONFIRM){ nd_ask_clearpass(ctx); }
        }
        ctx->needs_redraw=true;
        return;
    }
    if(s_step_done){                    /* 分步暂停: 上一步查看 / 下一步继续 */
        if(a==OS_ACTION_UP || a==OS_ACTION_LEFT){
            if(s_view<=0) nd_back_to_connect(ctx);   /* 已到最前(联网/信息) → 回连接页 */
            else s_view--;
        }
        else if(a==OS_ACTION_DOWN || a==OS_ACTION_RIGHT || a==OS_ACTION_CONFIRM){ s_step_go=true; }
        ctx->needs_redraw=true;
        return;
    }
    /* 执行中: 也可直接切上一步/下一步 (s_view 立即可见, 暂停时回到该步) */
    if(a==OS_ACTION_UP || a==OS_ACTION_LEFT){ if(s_view>0){ s_view--; s_nav_ovr=true; } ctx->needs_redraw=true; return; }
    if(a==OS_ACTION_DOWN || a==OS_ACTION_RIGHT){ if(s_view<ST_TOTAL-1){ s_view++; s_nav_ovr=true; } ctx->needs_redraw=true; return; }
    /* 执行中: CONFIRM(右下「取消」) → 中断当前扫描并进入暂停态 */
    if(a==OS_ACTION_CONFIRM){ s_cancel_step=true; ctx->needs_redraw=true; return; }
    if(s_done){
        if(a==OS_ACTION_UP && s_rptpx>0) s_rptpx-=24;
        if(a==OS_ACTION_DOWN) s_rptpx+=24;
        if(a==OS_ACTION_CONFIRM||a==OS_ACTION_RIGHT){ nd_export_report(); }
        ctx->needs_redraw=true;
    }
}
/* ---- 连接失败: 清空保存密码重新输入 ---- */
static void nd_clearpass_cb(ui_ctx_t*ctx,int result,void*ud){
    (void)ud;
    if(result==0){            /* 确定: 清空已保存密码, 框内重新输入 */
        wifi_manager_clear_saved();
        s_conn=1; s_pas=0; s_pass[0]=0;
    } else {                  /* 取消: 回列表 (保留结果, 不重扫) */
        s_conn=0; s_prompt_sel=0;
    }
    ctx->needs_redraw=true;
}
static void nd_ask_clearpass(ui_ctx_t*ctx){
    os_dialog_confirm_ex(ctx, "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x8c\xe6\x98\xaf\xe5\x90\xa6\xe6\xb8\x85\xe7\xa9\xba\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98\xe5\xaf\x86\xe7\xa0\x81\xe9\x87\x8d\xe6\x96\xb0\xe8\xbe\x93\xe5\x85\xa5\xef\xbc\x9f", 0, 0, nd_clearpass_cb, NULL);
} /* 连接失败，是否清空已保存密码重新输入？ */
static bool netdect_touch(ui_ctx_t*ctx,int x,int y){
    if(x<0||y<0) return false;
    if(s_wait_start){
        if(s_conn==1){
            if(y>=ND_BTN_Y && y<=ND_BTN_Y+ND_BTN_H){   /* 底部 连接/清空 (保留) */
                if(x < (CT_X0+CT_X1)/2) nd_do_connect();
                else { s_pas=0; s_pass[0]=0; }
                ctx->needs_redraw=true; return true;
            }
            int id=nd_kbd_hit(x,y);   /* 经典键盘 */
            if(id<0) return false;
            s_press_k=id;
            nd_kbd_act(ctx,id);
            ctx->needs_redraw=true;
            return true;
        }
        if(s_conn==3){ nd_ask_clearpass(ctx); return true; }
        /* WiFi 列表: 触屏点击行 → 连接/切换; 底部"下一步"→ 连当前选中 */
        if(s_conn==0){
            if(y>=NBTN_Y0 && y<=NBTN_Y1){
                if(x>=NBTN_R0 && x<=NBTN_R1){          /* 下一步: 已联网→直接开始收集; 否则连接选中 */
                    if(wifi_manager_is_connected()) nd_start_engine();
                    else if(wifi_manager_get_scan_count()>0) nd_try_connect(ctx, s_prompt_sel);
                    ctx->needs_redraw=true; return true;
                }
                if(x>=NBTN_L0 && x<=NBTN_L1){          /* 左键: 刷新 WiFi 列表 */
                    wifi_manager_scan_start();
                    ctx->needs_redraw=true; return true;
                }
                return false;
            }
            int cnt=wifi_manager_get_scan_count();
            int rh=26, y0=ND_BAR_H+10+3;
            if(cnt>0 && x>=0 && x<NW && y>=y0 && y<y0+cnt*rh){
                int idx=(y-y0)/rh;
                if(idx>=cnt) idx=cnt-1;
                if(y<=y0+idx*rh+rh){
                    s_prompt_sel=idx; nd_try_connect(ctx, idx);
                    ctx->needs_redraw=true; return true;
                }
            }
            return false;
        }
        return false;
    }
    if(!s_done && nd_view_step()==ST_SCAN){        /* 扫描列表: 拖动在poll处理; 此处只点行弹详情 */
        if(y>=CT_TOP && y<=CT_BOT){
            int line=18;
            static int dy0=-1, tapy=-1; static bool moved=false;
            int ty;
            if(input_get_touch_pos(NULL,&ty)){
                if(dy0<0){ dy0=ty; tapy=ty; moved=false; }
                else { if(ty!=dy0) moved=true; }
                return true;
            }
            if(dy0>=0 && !moved && tapy>=0){
                int yoff=s_scrollpx%line, idx=s_scrollpx/line;
                int k=(tapy-(CT_TOP+2)+yoff)/line + idx;
                if(k>=0 && k<s_nhost) nd_show_host_detail(ctx,k);
            }
            dy0=-1; return true;
        }
        /* 落到底部按钮区(在下方 CT_BOT 之下)时继续走 nav 处理 */
    }
    if(!s_done && (nd_view_step()==ST_PORT || nd_view_step()==ST_CRED)){   /* 端口/口令列表: 拖动在poll处理; 此处只点端口行弹详情 */
        if(y>=CT_TOP && y<=CT_BOT){
            int line=18;
            static int dy0=-1, tapy=-1; static bool moved=false;
            int ty;
            if(input_get_touch_pos(NULL,&ty)){
                if(dy0<0){ dy0=ty; tapy=ty; moved=false; }
                else { if(ty!=dy0) moved=true; }
                return true;
            }
            if(dy0>=0 && !moved && nd_view_step()==ST_PORT && tapy>=0){
                int yoff=s_scrollpx%line, idx=s_scrollpx/line;
                int k=(tapy-(CT_TOP+2)+yoff)/line + idx;
                int hi=nd_vis_idx(k);
                if(hi>=0 && hi<s_nhost) nd_show_host_detail(ctx,hi);
            }
            dy0=-1; return true;
        }
    }
    /* 扫描执行中 (未暂停): 底部角按钮 — 左「上一步」浏览/右「取消」中断当前扫描并暂停.
     * (这里放在列表区处理之后, 因此只有落在按钮区(NBTN)的点击才命中) */
    if(!s_done && !s_step_done){
        if(y>=NBTN_Y0 && y<=NBTN_Y1){
            if(x>=NBTN_R0 && x<=NBTN_R1){ s_cancel_step=true; ctx->needs_redraw=true; return true; }  /* 取消 */
            if(x>=NBTN_L0 && x<=NBTN_L1){ if(s_view>0){ s_view--; s_nav_ovr=true; } ctx->needs_redraw=true; return true; }  /* 上一步浏览 */
        }
        return false;
    }
    if(s_step_done){                    /* 角按钮 上一步/下一步 */
        if(y>=NBTN_Y0 && y<=NBTN_Y1){
            if(x>=NBTN_L0 && x<=NBTN_L1){ if(s_view<=1) nd_back_to_connect(ctx); else s_view--; }
            else if(x>=NBTN_R0 && x<=NBTN_R1){ s_step_go=true; }
            ctx->needs_redraw=true; return true;
        }
        return false;
    }
    if(s_done){                         /* 报告: 滚动已迁至 poll(poll 每帧采样触控跟手); 此处仅右键=导出 */
        if(y>=NBTN_Y0 && y<=NBTN_Y1){
            if(x>=NBTN_R0 && x<=NBTN_R1){ nd_export_report(); }
            ctx->needs_redraw=true; return true;
        }
        return false;
    }
    return false;
}
static void netdect_enter(ui_ctx_t*ctx){
    s_gen++; s_stop=false; s_cancel_step=false;                 /* 使在途旧任务作废 */
    s_step=ST_NET;s_prog=0;s_done=false;s_nhost=0;s_scrollpx=0;s_rptpx=0;s_rpt_nlin=0;
    s_step_done=false; s_step_go=false; s_finished=0; s_view=0;   /* 清掉上次停留状态 */
    s_status[0]=0; s_ssid[0]=s_pass[0]=0; s_pas=0; s_conn=0;
    os_hw_request(OS_HW_WIFI);             /* 进入即打开 WiFi (引用计数, 退出再释放) */
    s_wait_start=true;                     /* 进场即显示网络页(不再闪信息页) */
    s_use_sys=false; s_prev_conn=wifi_manager_is_connected();
    s_enter_pending=false;
    s_scan_started=false; s_scan_last=0;   /* 触发首扫(等STA稳定再启动) */
    ctx->needs_redraw=true;
}
/* 进入确认后: 停在「搜索网络」界面 (WiFi 扫描列表), 扫描由 poll 等到 STA 稳定再启动 */
static void nd_enter_do(ui_ctx_t *ctx){
    s_wait_start=true; s_conn=0; s_prompt_sel=0;
    ctx->needs_redraw=true;
}
static void netdect_exit(ui_ctx_t*ctx){
    s_stop=true;
    wifi_manager_scan_stop();
    os_hw_release(OS_HW_WIFI);             /* 退出即关闭 WiFi (打开才加载, 返回应用管家=真正退出) */
    ctx->needs_redraw=true;
}

static const os_module_t s_netdect={
    .name="netdect", .page_id=OS_PAGE_NETDECT,
    .on_enter=netdect_enter, .on_exit=netdect_exit,
    .render=netdect_render, .action=netdect_action, .touch=netdect_touch,
    .poll=netdect_poll,
    .fullscreen=true,
};
void os_page_netdect_register(void){ os_register(&s_netdect); }