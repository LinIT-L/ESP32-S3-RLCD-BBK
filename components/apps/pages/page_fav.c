/**
 * page_fav.c — 收藏夹 v2 (网站账号密码 一键模拟键盘输入)
 *
 * 全新 UI:
 *   - 阅读器/密钥管理器风格全屏页. 进入先过独立 PIN 九宫格 (收藏夹独立 NVS 槽).
 *   - 左栏: 纯文字分类列表 (论坛网站/工作技术/知识学习/个人娱乐 等, 可增删, 52px/项),
 *           选中反白, 支持上下像素滚动.
 *   - 右栏: 收藏项行高 52px = 行首 48×48 图标 + 名称(24px) + 介绍(16px) + 最右 32×32 编辑按钮.
 *     · 点行 → 本行下方像素滑动展开"编辑抽屉", 可编辑 名称/介绍/选图标/增删改账号密码.
 *     · 分类为空 → 列表第一行显示「＋ 添加收藏」.
 *   - 编辑键盘: 独立浮动 kbd_edit (压低 1/3, 删除并入退格, 空格补位, 回车改名).
 */
#include "os.h"
#include "os_hw.h"      /* os_hw_is_active/request/release */
#include "os_pane.h"
#include "ui_common.h"
#include "ui_scroll.h"   /* ★ 2026-10-24 统一跟手滚动 */
#include "input.h"
#include "keyboard.h"
#include "os_hid.h"
#include "keyvault.h"
#include "os_keyvault.h"   /* 统一 PIN 文案 */
#include "applock.h"       /* 通用应用锁 (九宫格 PIN) */
#include "ui_col.h"        /* 整列离屏缓冲: 滚动只抄窗 (商店同款) */
#include "wallpapers.h"    /* wp_idle_hold: 设置管理界面里别息屏 */
#include "wifi_manager.h"   /* is_connected/is_connecting/get_ip */
#include "net_client.h"     /* 设备侧抓网页标题/描述 (不经浏览器 CORS) */
#include "esp_log.h"
#include "esp_timer.h"

#include "svc_nvs.h"
#include "icons_main_enum.h"   /* MAIN_ICON_COUNT (图标选择器/收藏图标范围) */
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif_ip_addr.h"
#include "lwip/ip4_addr.h"
#include "dns_server.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include "str_trunc.h"   /* str_trunc(): 短字段的有意截断拷贝 */

#define S_W 400
#define S_H 300
#define HDR_H 26
#define LIST_X 112          /* 左栏右缘 (右栏起点) */

#define FAV_MAX 256  /* 变长存储后 256 条约 15KB (原来定长 635B/条 撑不住) */
#define CR_MAX  10

#define NVS_NS  "linfav"
#define NVS_DATA "blob"
#define NVS_ENC  "enc"    /* ★ 密码单独加密存放 (AES-256-GCM, 密钥来自 applock) */
#define NVS_PIN "pinsset"
#define NVS_CATS "categories"
#define NVS_PINBLOB "pinblob"

typedef struct { char user[26]; char pass[26]; } cred_t;
#define FAV_URL_SZ  56
#define FAV_NAME_SZ 24
#define FAV_DESC_SZ 32
typedef struct {
    uint8_t icon; uint8_t cat;
    char name[FAV_NAME_SZ]; char desc[FAV_DESC_SZ];
    char url[FAV_URL_SZ];        /* ★ 2026-10-19 新增: 网址 (网页编辑/取图标用) */
    uint8_t nc;
    cred_t cr[CR_MAX];
} fav_t;

/* ★ v1 旧布局 (无 url) —— 仅用于迁移读取, 千万别改 */
typedef struct {
    uint8_t icon; uint8_t cat;
    char name[14]; char desc[20];
    uint8_t nc;
    cred_t cr[CR_MAX];
} fav_old_t;

/* ★ v2 布局 (有 url, 但 name14/desc20) —— 迁移读取用 */
typedef struct {
    uint8_t icon; uint8_t cat;
    char name[14]; char desc[20];
    char url[FAV_URL_SZ];
    uint8_t nc;
    cred_t cr[CR_MAX];
} fav_v2_t;

/* UTF-8 安全截断拷贝: 绝不切半个汉字 */
static void fav_u8cpy(char *d, size_t cap, const char *src)
{
    if (!d || cap == 0) return;
    size_t o = 0;
    if (src) for (size_t i = 0; src[i]; ) {
        unsigned char c = (unsigned char)src[i];
        size_t n = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        size_t j = 1; while (j < n && src[i + j]) j++;
        if (j < n) break;
        if (o + n >= cap) break;
        memcpy(d + o, src + i, n); o += n; i += n;
    }
    d[o] = 0;
}

/* 分类上限 = os_pane 左栏槽位数 (os_pane.h: char folders[24][64]), 超了也显示不出来 */
#define CAT_MAX 24
#define CAT_SZ  25   /* 分类名最大字节数: 容纳 8 个汉字(UTF8 3B/字)+1, 原10B只能存3汉字会截断 */
static char s_cats[CAT_MAX][CAT_SZ] = {
    "\xe8\xae\xba\xe5\x9d\x9b\xe7\xbd\x91\xe7\xab\x99",
    "\xe5\xb7\xa5\xe4\xbd\x9c\xe6\x8a\x80\xe6\x9c\xaf",
    "\xe7\x9f\xa5\xe8\xaf\x86\xe5\xad\xa6\xe4\xb9\xa0",
    "\xe4\xb8\xaa\xe4\xba\xba\xe5\xa8\xb1\xe4\xb9\x90",
};
static int s_cat_n = 4;

/* ★ 2026-10-25 内存优化: 原为静态 163KB(256条×636B, 每条含 10 组账号密码)。
 *   只有真正进收藏页/用收藏接口时才分配 —— 开机不占。容量与字段一个没改。 */
static fav_t *s_fav;
static bool fav_ensure(void) {
    if (s_fav) return true;
    s_fav = heap_caps_calloc(FAV_MAX, sizeof(fav_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fav) ESP_LOGW("FAV", "收藏表分配失败 (%u B)", (unsigned)(FAV_MAX * sizeof(fav_t)));
    return s_fav != NULL;
}
static int   s_n = 0;

static int s_cat = 0;          /* -1=设置(左栏首项) 0..=分类 */
static int  s_scroll = 0;
static int  s_open  = -1;      /* ★ 展开抽屉的条目 (全局 index), -1=未展开 */
static bool s_need_clear = true;     /* 内容/分类变化时才整屏清 (滚动时不清, 否则每帧 12 万像素) */
static ui_col_t s_fcol;              /* 右栏整列缓冲 (滚动只抄窗) */
static bool s_fcol_dirty = true;     /* 内容变化 → 重建整列 */
static int  s_r_anchor, s_r_base;   /* 右栏拖动: 起点 y / 起始滚动量 */
static bool s_r_drag;                /* 已判定为拖动 (12px) — 保留仅作兼容 */
static ui_scroll_t s_fav_sc;         /* ★ 2026-10-24 统一滚动控制器（右栏列表）*/
static bool s_fav_sc_rdy = false;
static int  s_fav_sc_cap = -1;
static int  s_sel = 0;
static int  s_drawer = -1;
static bool s_edit_focus = false;
static int  s_field = 0;
static char s_buf[64]; static int s_blen = 0;   /* 64: 网址最长 56 字节, 28 装不下 */
static bool s_show_cats = false;   /* 抽屉内: true=图标选择 */

/* 左栏: 一比一复用阅读器 os_pane 模板 (文字+水管分割线+选中框, 行高32) */
/* ★ LINTOS 2026-10-25: 列表 pane(约 59KB) 原来静态常驻 PSRAM, 改为进页面才分配/退出即还 */
static os_pane_t *s_pane__p;
static os_pane_t *s_pane__get(void) {
    if (!s_pane__p) {
        s_pane__p = heap_caps_calloc(1, sizeof(os_pane_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pane__p) ESP_LOGW("PANE", "列表 pane 分配失败 (%u B)", (unsigned)sizeof(os_pane_t));
    }
    if (!s_pane__p) s_pane__p = heap_caps_calloc(1, sizeof(os_pane_t), MALLOC_CAP_8BIT);   /* 兜底: 内部 RAM */
    return s_pane__p;
}
static inline void s_pane__free(void) { if (s_pane__p) { heap_caps_free(s_pane__p); s_pane__p = NULL; } }
#define s_pane (*s_pane__get())
static int pane_mem_folders(os_pane_t *p) {
    for (int i = 0; i < s_cat_n && i < CAT_MAX; i++)
        snprintf(p->folders[i], sizeof(p->folders[i]), "%.*s", (int)sizeof(p->folders[i]) - 1, s_cats[i]);
    p->folder_count = s_cat_n;
    p->hide_all = true;
    return p->folder_count;
}
/* 左栏显示序号 ↔ 状态: 0=设置, 1..=分类 */
static int pane_sel(void)      { return (s_cat < 0) ? 0 : s_cat + 1; }
/* ★ 2026-10-20: 左栏停在「设置管理」时不息屏也不被空闲弹回 —— 用户在那儿翻配置。 */
static void fav_idle_hold_sync(void) { wp_idle_hold(s_cat < 0); }
static bool fav_hid_ensure(void);   /* 前向: 按需起 HID */
static void pane_set_sel(int idx) { s_cat = (idx <= 0) ? -1 : idx - 1; fav_idle_hold_sync(); }

static bool  s_authed = false;
static void  fav_hid_sync(void);      /* 前向: 分类页开键鼠 / 设置页还串口 */
static void  fav_json_esc(char *d, size_t cap, const char *src);   /* 前向: list 里转义 */

/* 系统"连接WiFi"流程 (局域网手柄同款: 扫描列表+密码键盘, 复用) */

/* ---- 备份 / 网页配置 (热点 AP / IP STA) ---- */
/* ★ 2026-10-20: 备份统一放 /sdcard/backup/, 可存多份 (fav_01.bak, fav_02.bak ...) */
#define FAV_BAK_DIR     "/sdcard/backup"
#define FAV_BAK_OLD     "/sdcard/fav.bak"     /* 老位置, 兼容读取 */
#define FAV_BAK_MAX     16
#define FAV_BACKUP_PATH FAV_BAK_DIR "/fav_01.bak"   /* 仅用于默认路径常量 */
#define FAV_AP_SSID     "LinTOS-FAV"
static httpd_handle_t s_fav_httpd = NULL;
static void          *s_fav_dns   = NULL;
static bool           s_web_on    = false;   /* 网页服务启动标志 */
static bool           s_web_allow = false;   /* ★ 是否对外可访问 (预热后仍要用户点「局域网配置」才开放) */
static bool           s_web_open_req = false;/* 用户点了开, 但服务还在启动中 */
static int            s_web_phase = 0;        /* 0=空闲 1=启动中 2=已启动 3=失败 */
static int            s_web_mode  = 0;        /* 0=热点配置(AP) 1=IP配置(STA) */
static bool           s_lan_pending = false;  /* IP配置: 等待 WiFi 连接后自动启动服务 */
static bool           s_lan_toast_pending = false; /* IP服务启动完成后弹 IP 提示 */
static uint32_t       s_lan_t0 = 0;            /* 等待 WiFi 连接的超时起点 (60s 放弃) */
static bool           s_fav_import_dlg = false; /* 防重复弹导入确认框 */

/* poll 拖动 */
static bool s_press=false; static bool s_press_on=false; static int s_press_x=0,s_press_y=0; static uint32_t s_press_t=0;
static int  s_cat_drag_last=-1;
static int  s_lpane_acc=0;      /* 左栏拖动: 像素累加器 (满 24px 走一行, os_pane 只有整行滚动) */

#define FAV_LSHIFT 0x02
#define FAV_ENTER  0x28
#define FAV_PLUS_TXT "\xef\xbc\x8b"   /* ＋ 全角加号 */

static void redraw(ui_ctx_t *ctx) { ctx->needs_redraw = true; }

/* 前向声明 */
static void add_cred(ui_ctx_t*ctx,int gi);
static void apply_field(void);
static void edit_load(void);

/* ================= NVS ================= */
/* [归整] 账号收藏沿用 "linfav" 命名空间; catn 与 categories blob 同事务写, 保持 svc_nvs 直写 (数据格式不变) */
static void cats_save(void){
    nvs_handle_t h; if(svc_nvs_open(NVS_NS,NVS_READWRITE,&h)==ESP_OK){ svc_nvs_set_blob(h,NVS_CATS,s_cats,sizeof(s_cats)); svc_nvs_set_i32(h,"catn",s_cat_n); svc_nvs_commit(h); svc_nvs_close(h); }
}
static void cats_load(void){
    nvs_handle_t h; int32_t n=0; bool got=false;
    static char tmp[CAT_MAX][CAT_SZ];      /* 600B 静态: 别放栈 (httpd/UI 任务栈都不宽裕) */
    size_t sz=sizeof(tmp);
    if(svc_nvs_open(NVS_NS,NVS_READONLY,&h)==ESP_OK){
        /* ★★ 2026-10-20: 不再硬比 `sz == sizeof(s_cats)` —— CAT_MAX 从 8 提到 24 后,
         *   老存档只有 8*25 字节, 长度不等就被整份丢弃 ⇒ 用户分类全回默认。
         *   改为按 CAT_SZ 整除解析: 存了几个槽就认几个槽, 下次 cats_save() 自动升到 24 槽。 */
        if(svc_nvs_get_blob(h,NVS_CATS,tmp,&sz)==ESP_OK && sz>0 && sz<=sizeof(tmp) &&
           (sz%CAT_SZ)==0 && svc_nvs_get_i32(h,"catn",&n)==ESP_OK){
            int stored=(int)(sz/CAT_SZ);
            if(n>stored) n=stored;         /* catn 不能超过档案里真正存下的槽数 */
            if(n>0){ memcpy(s_cats,tmp,sz); s_cat_n=(int)n; got=true; }
        }
        svc_nvs_close(h);
    }
    if(!got||s_cat_n<=0||s_cat_n>CAT_MAX) s_cat_n=4;
}
/* 密码段加密缓冲: 每条 [present 1B][len 2B][nonce12+ct+tag16] */
#define FAV_ENC_MAXB (24u * 1024u)   /* ⛔ 别按 FAV_MAX*CR_MAX 算: 256 条会变 142KB 静态 */
static uint8_t s_encbuf[FAV_ENC_MAXB];   /* 密码段共用缓冲(必须在内部 RAM: NVS 写时关 cache) */

/* 把 s_fav 里的密码加密成 blob; 成功返回长度, 失败 0 */
static size_t fav_build_enc(uint8_t *out, size_t cap){
    if(!applock_key()) return 0;          /* 未解锁不写, 防止把明文换成空 */
    size_t o = 0;
    for(int i=0;i<s_n;i++){
        for(int c=0;c<s_fav[i].nc && c<CR_MAX;c++){
            const char *pw = s_fav[i].cr[c].pass;
            size_t pl = strlen(pw);
            if(pl == 0){ if(o+1>cap) return 0; out[o++]=0; continue; }
            size_t cn = applock_encrypt(pw, pl, out + o + 3);
            if(cn == 0 || o + 3 + cn > cap) return 0;
            out[o] = 1;
            out[o+1] = (uint8_t)(cn & 0xFF);
            out[o+2] = (uint8_t)((cn >> 8) & 0xFF);
            o += 3 + cn;
        }
    }
    return o;
}

static void fav_save(void){
    nvs_handle_t h; if(svc_nvs_open(NVS_NS,NVS_READWRITE,&h)==ESP_OK){
        /* ★★ 2026-10-20 变长序列化 (V4):
         *   定长 fav_t 635B/条 里 520B 是 10 组账号空槽 ⇒ 256 条 162KB 装不下 NVS。
         *   这里只写**实际内容**: 每条 = icon/cat/nc + 三个"长度+字节"串 + 每个账号的 user。
         *   密码不在这里(仍走 NVS_ENC 单独加密存)。典型 45B/条 ⇒ 256 条约 12KB。
         * ⚠ 缓冲必须在**内部 RAM**: NVS 写 flash 会关 cache, 源在 PSRAM 会 panic。 */
        size_t need = 3 + 2;                       /* magic + n(u16) */
        for(int i=0;i<s_n;i++){
            fav_t *f=&s_fav[i];
            need += 3 + 1+strlen(f->name) + 1+strlen(f->desc) + 1+strlen(f->url);
            for(int c=0;c<f->nc && c<CR_MAX;c++) need += 1+strlen(f->cr[c].user);
        }
        uint8_t *blob = heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if(!blob){ svc_nvs_close(h); return; }
        size_t o=0;
        blob[o++]=0xFA; blob[o++]=0x56; blob[o++]=0x04;      /* magic "FV4" */
        blob[o++]=(uint8_t)(s_n & 0xFF); blob[o++]=(uint8_t)((s_n>>8)&0xFF);
        for(int i=0;i<s_n;i++){
            fav_t *f=&s_fav[i];
            blob[o++]=f->icon; blob[o++]=f->cat; blob[o++]=f->nc;
            #define PUTS(field) do{ size_t l=strlen(f->field); if(l>255)l=255; \
                blob[o++]=(uint8_t)l; memcpy(blob+o,f->field,l); o+=l; }while(0)
            PUTS(name); PUTS(desc); PUTS(url);
            for(int c=0;c<f->nc && c<CR_MAX;c++){
                size_t l=strlen(f->cr[c].user); if(l>255)l=255;
                blob[o++]=(uint8_t)l; memcpy(blob+o,f->cr[c].user,l); o+=l;
            }
            #undef PUTS
        }
        svc_nvs_set_blob(h,NVS_DATA,blob,o);
        free(blob);

        uint8_t *enc = s_encbuf;
        size_t el = fav_build_enc(enc, sizeof(enc));
        if(el > 0) svc_nvs_set_blob(h,NVS_ENC,enc,el);
        else if(el == 0 && s_n == 0) svc_nvs_set_blob(h,NVS_ENC,enc,0);
        svc_nvs_commit(h); svc_nvs_close(h);
    }
}

/* 载入后把密码解密回内存 (需要已解锁; 未解锁时留空, 解锁后再调一次) */
static bool fav_load_enc(void){
    if(!applock_key()) return false;
    nvs_handle_t h;
    if(svc_nvs_open(NVS_NS,NVS_READONLY,&h)!=ESP_OK) return false;
    uint8_t *enc = s_encbuf;
    size_t el = sizeof(enc);
    bool ok = (svc_nvs_get_blob(h,NVS_ENC,enc,&el)==ESP_OK);
    svc_nvs_close(h);
    if(!ok) return false;
    size_t o = 0;
    for(int i=0;i<s_n && o<el;i++){
        for(int c=0;c<s_fav[i].nc && c<CR_MAX && o<el;c++){
            uint8_t present = enc[o++];
            if(!present) continue;
            if(o+2>el) return true;
            size_t cn = (size_t)enc[o] | ((size_t)enc[o+1] << 8); o += 2;
            if(o+cn>el) return true;
            char tmp[64];
            size_t pl = applock_decrypt(enc+o, cn, tmp);
            o += cn;
            if(pl && pl < sizeof(tmp)){ tmp[pl]=0; str_trunc(s_fav[i].cr[c].pass, sizeof(s_fav[i].cr[c].pass), tmp); }
        }
    }
    return true;
}
static int fav_hexv(char c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* 就地修复历史脏数据: 早期 /api/add 没解码, 中文被存成 %E8%AE%BA... */
static void fav_pct_fix(char *s, size_t cap)
{
    if (!s || !s[0] || !strchr(s, '%')) return;
    char out[128]; size_t o = 0;
    for (char *p = s; *p && o + 1 < sizeof(out); p++) {
        if (*p == '%' && fav_hexv(p[1]) >= 0 && fav_hexv(p[2]) >= 0) {
            out[o++] = (char)((fav_hexv(p[1]) << 4) | fav_hexv(p[2]));
            p += 2;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
    if (o) str_trunc(s, cap, out);
}

static void fav_load(void){
    if (!fav_ensure()) return;                 /* 按需分配 (见 fav_ensure) */
    nvs_handle_t h; memset(s_fav, 0, (size_t)FAV_MAX * sizeof(fav_t)); s_n=0;
    /* ★★ 2026-10-20 必须**先**载分类: 下面解析每条时要用 s_cat_n 判断 cat 是否越界,
     *   放在后面时 s_cat_n 还是初值 4 ⇒ 4/5/6 号分类的网站全被当成越界清成 0
     *   ⇒ "把网站拖到新分类, 重启后又回原分类"。 */
    cats_load();
    /* ★★ 2026-10-20: 先问长度再分配 —— 定长时代的 1+635*256=162KB 根本分配不出来。
     * 内部 RAM 分配 (NVS 读要关 cache, PSRAM 缓冲会 panic)。 */
    size_t sz = 0;
    uint8_t *p = NULL;
    if (svc_nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (svc_nvs_get_blob(h, NVS_DATA, NULL, &sz) == ESP_OK && sz > 0)
            p = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        svc_nvs_close(h);
    }
    if(p){
        if(svc_nvs_open(NVS_NS,NVS_READONLY,&h)==ESP_OK){
            if(svc_nvs_get_blob(h,NVS_DATA,p,&sz)==ESP_OK&&sz>=5 &&
               p[0]==0xFA && p[1]==0x56 && p[2]==0x04){
                /* ---------- V4 变长 ---------- */
                int n = p[3] | (p[4]<<8);
                if(n > FAV_MAX) n = FAV_MAX;
                size_t o = 5;
                for(int i=0;i<n && o+3<=sz;i++){
                    fav_t *f=&s_fav[i];
                    f->icon=p[o++]; f->cat=p[o++]; f->nc=p[o++];
                    if(f->nc > CR_MAX) f->nc = CR_MAX;
                    #define GETS(field,cap) do{ if(o>=sz) break; int l=p[o++];                         if(l>(int)(cap)-1) l=(int)(cap)-1;                         if(o+(size_t)l>sz) l=(int)(sz-o);                         memcpy(f->field,p+o,l); f->field[l]=0; o+=l; }while(0)
                    GETS(name, sizeof(f->name));
                    GETS(desc, sizeof(f->desc));
                    GETS(url,  sizeof(f->url));
                    for(int c=0;c<f->nc;c++) GETS(cr[c].user, sizeof(f->cr[c].user));
                    #undef GETS
                    if(f->icon >= MAIN_ICON_COUNT) f->icon = 0;
                    if(f->cat  >= s_cat_n) f->cat = 0;
                }
                s_n = n;
                ESP_LOGI("FAV","V4 变长载入 %d 条 (%u B) 分类=%d", s_n, (unsigned)sz, s_cat_n);
            } else if(sz>=1){
                int n=p[0]; if(n>FAV_MAX)n=FAV_MAX;
                /* ★ 2026-10-19: 布局迁移 —— 旧档(无 url)按 fav_old_t 逐字段搬, 不丢数据 */
                if(sz == 1u + sizeof(fav_old_t)*FAV_MAX){
                    fav_old_t *o=(fav_old_t*)&p[1];
                    for(int i=0;i<n;i++){
                        memset(&s_fav[i],0,sizeof(fav_t));
                        s_fav[i].icon=o[i].icon; s_fav[i].cat=o[i].cat;
                        memcpy(s_fav[i].name,o[i].name,sizeof(o[i].name));
                        memcpy(s_fav[i].desc,o[i].desc,sizeof(o[i].desc));
                        s_fav[i].nc=o[i].nc;
                        memcpy(s_fav[i].cr,o[i].cr,sizeof(o[i].cr));
                        if(s_fav[i].icon >= MAIN_ICON_COUNT) s_fav[i].icon = 0;
                    }
                    s_n=n;
                    ESP_LOGI("FAV","布局迁移 v1->v3 (%d 条)", s_n);
                    fav_save();
                } else if(sz == 1u + sizeof(fav_v2_t)*FAV_MAX){
                    fav_v2_t *o=(fav_v2_t*)&p[1];
                    for(int i=0;i<n;i++){
                        memset(&s_fav[i],0,sizeof(fav_t));
                        s_fav[i].icon=o[i].icon; s_fav[i].cat=o[i].cat;
                        memcpy(s_fav[i].name,o[i].name,sizeof(o[i].name));
                        memcpy(s_fav[i].desc,o[i].desc,sizeof(o[i].desc));
                        memcpy(s_fav[i].url, o[i].url, sizeof(o[i].url));
                        s_fav[i].nc=o[i].nc;
                        memcpy(s_fav[i].cr,o[i].cr,sizeof(o[i].cr));
                        if(s_fav[i].icon >= MAIN_ICON_COUNT) s_fav[i].icon = 0;
                    }
                    s_n=n;
                    ESP_LOGI("FAV","布局迁移 v2->v3 (%d 条)", s_n);
                    fav_save();
                } else if (sz == 1u + sizeof(fav_t)*16) {
                    /* ★ 2026-10-20: FAV_MAX 16→32 的老档迁移 (按 16 条读, 存成新布局) */
                    fav_t *arr = (fav_t *)&p[1];
                    for (int i = 0; i < n && i < 16; i++) s_fav[i] = arr[i];
                    s_n = (n > 16) ? 16 : n;
                    ESP_LOGI("FAV", "布局迁移 16→%d 条上限", FAV_MAX);
                    fav_save();
                } else if(sz >= 1u + sizeof(fav_t)*FAV_MAX){
                    fav_t*arr=(fav_t*)&p[1];
                    for(int i=0;i<n;i++){
                        s_fav[i]=arr[i];
                        /* 图标索引随固件重排会失效: 越界一律回退到 0, 避免画出错/越界 */
                        if(s_fav[i].icon >= MAIN_ICON_COUNT) s_fav[i].icon = 0;
                    }
                    s_n=n;
                }
            }
            svc_nvs_close(h);
        }
        free(p);
    }
    if(s_cat>=s_cat_n) s_cat=0;
    /* 修历史脏数据 (%XX 未解码) */
    for (int i = 0; i < s_n; i++) {
        fav_pct_fix(s_fav[i].name, sizeof(s_fav[i].name));
        fav_pct_fix(s_fav[i].desc, sizeof(s_fav[i].desc));
        fav_pct_fix(s_fav[i].url,  sizeof(s_fav[i].url));
    }
    for (int i = 0; i < s_cat_n; i++) fav_pct_fix(s_cats[i], CAT_SZ);

    /* ★ 2026-10-19 首次启动: 内置两个常用站点 (bilibili / 本仓库) */
    if (s_n == 0) {
        fav_t *a = &s_fav[0];
        a->icon = (uint8_t)ICON_BILI; a->cat = 0; a->nc = 0;
        snprintf(a->name, sizeof(a->name), "\xe5\x93\x94\xe5\x93\xa9\xe5\x93\x94\xe5\x93\xa9");           /* 哔哩哔哩 */
        snprintf(a->desc, sizeof(a->desc), "\xe5\xbc\xb9\xe5\xb9\x95\xe8\xa7\x86\xe9\xa2\x91\xe7\xbd\x91\xe7\xab\x99"); /* 弹幕视频网站 */

        fav_t *b = &s_fav[1];
        b->icon = (uint8_t)ICON_GITHUB; b->cat = 0; b->nc = 0;
        snprintf(b->name, sizeof(b->name), "GitHub");
        snprintf(b->desc, sizeof(b->desc), "\xe6\x9c\xac\xe6\x9c\xba\xe5\x9b\xba\xe4\xbb\xb6\xe4\xbb\x93\xe5\xba\x93"); /* 本机固件仓库 */

        s_n = 2;
        fav_save();
    }
}

/* ================= 备份 (TF 卡) =================
 * 文件格式: magic "FAVEXP1"(7) + cat_n(u8) + 分类名(CAT_SZ B/个) + fav_n(u8)
 *           + fav_t 数组 + pin_set(u8) + pin_len(u8) + pinblob(20B) */
#ifndef FAV_ICON_BYTES
#define FAV_ICON_BYTES 128
#endif
static bool fav_icon_get(int gi, uint8_t *out);
static void fav_icon_set(int gi, const uint8_t *bmp);
static void fav_icon_del(int gi);

/* 备份列表 (弹窗选择用) */
static char        s_bak_path[FAV_BAK_MAX][56];
static char        s_bak_label[FAV_BAK_MAX][40];
static const char *s_bak_items[FAV_BAK_MAX];

static void fav_bak_mkdir(void) { mkdir(FAV_BAK_DIR, 0777); }

/* 扫描备份目录 (含老的 /sdcard/fav.bak), 返回数量 */
static int fav_bak_scan(void)
{
    fav_bak_mkdir();
    int n = 0;
    DIR *d = opendir(FAV_BAK_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < FAV_BAK_MAX) {
            const char *nm = e->d_name;
            size_t L = strlen(nm);
            if (nm[0] == '.' || L < 5) continue;
            if (strcasecmp(nm + L - 4, ".bak") != 0) continue;
            snprintf(s_bak_path[n], sizeof(s_bak_path[0]), "%s/%s", FAV_BAK_DIR, nm);
            snprintf(s_bak_label[n], sizeof(s_bak_label[0]), "%s", nm);
            s_bak_items[n] = s_bak_label[n];
            n++;
        }
        closedir(d);
    }
    if (n < FAV_BAK_MAX) {                    /* 兼容老位置的单文件备份 */
        FILE *f = fopen(FAV_BAK_OLD, "rb");
        if (f) {
            fclose(f);
            snprintf(s_bak_path[n], sizeof(s_bak_path[0]), "%s", FAV_BAK_OLD);
            snprintf(s_bak_label[n], sizeof(s_bak_label[0]), "%s", "fav.bak (旧位置)");
            s_bak_items[n] = s_bak_label[n];
            n++;
        }
    }
    return n;
}

/* 生成下一个不重名的备份路径: fav_01.bak, fav_02.bak ... */
static void fav_bak_new_path(char *out, size_t cap)
{
    fav_bak_mkdir();
    for (int i = 1; i <= 99; i++) {
        snprintf(out, cap, "%s/fav_%02d.bak", FAV_BAK_DIR, i);
        FILE *f = fopen(out, "rb");
        if (!f) return;
        fclose(f);
    }
    snprintf(out, cap, "%s/fav_99.bak", FAV_BAK_DIR);
}

static int fav_export_file(const char *path){
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW("FAV", "导出失败: 打不开 %s (TF 卡未插?)", path); return -1; }
    int rc = -1;
    /* ★ 2026-10-20: 升级到 FAVEXP2 —— **图标一起打包**, 单文件即可整体搬到别的机器恢复。
     *   旧 FAVEXP1 仍然能导入 (没有图标)。 */
    if (fwrite("FAVEXP2", 1, 7, f) == 7) {
        uint8_t cn = (uint8_t)s_cat_n;
        if (fwrite(&cn, 1, 1, f) == 1) {
            int ok = 1;
            for (int i = 0; i < cn; i++)
                if (fwrite(s_cats[i], 1, CAT_SZ, f) != CAT_SZ) { ok = 0; break; }
            if (ok) {
                uint8_t fn = (uint8_t)(s_n > 255 ? 255 : s_n);   /* 格式里条数是 u8: 256 会写成 0 */
                if (fwrite(&fn, 1, 1, f) == 1 &&
                    (fn == 0 || fwrite(s_fav, sizeof(fav_t), fn, f) == (size_t)fn)) {
                    /* 图标段: 每条 1 字节"有/无" + (有则) 128 字节位图 */
                    uint8_t bmp[FAV_ICON_BYTES];
                    for (int i = 0; i < fn && ok; i++) {
                        uint8_t has = fav_icon_get(i, bmp) ? 1 : 0;
                        if (fwrite(&has, 1, 1, f) != 1) { ok = 0; break; }
                        if (has && fwrite(bmp, 1, FAV_ICON_BYTES, f) != FAV_ICON_BYTES) { ok = 0; break; }
                    }
                    uint8_t pb = 0, pl = 0, pinblob[20] = {0}; /* PIN 归系统 keyvault, 不随备份迁移 */
                    if (ok && fwrite(&pb, 1, 1, f) == 1 && fwrite(&pl, 1, 1, f) == 1 &&
                        fwrite(pinblob, 1, 20, f) == 20) rc = 0;
                }
            }
        }
    }
    fclose(f);
    if (rc != 0) { remove(path); ESP_LOGW("FAV", "导出失败: 写入不完整"); }
    else ESP_LOGI("FAV", "已导出 %d 条(含图标)到 %s", s_n, path);
    return rc;
}
static int fav_import_file(const char *path){
    FILE *f = fopen(path, "rb");
    if (!f) return -1;                       /* -1 = 没有备份文件 */
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz <= 0) { fclose(f); return -1; }

    char magic[8] = {0};
    uint8_t cn = 0, fn = 0, pb = 0, pl = 0, pinblob[20] = {0};
    char cats[CAT_MAX][CAT_SZ];
    memset(cats, 0, sizeof(cats));

    int rc = -1, v2 = 0;
    if (fread(magic, 1, 7, f) != 7) { fclose(f); return -3; }
    if (memcmp(magic, "FAVEXP2", 7) == 0)      v2 = 1;
    else if (memcmp(magic, "FAVEXP1", 7) != 0) { fclose(f); return -3; }

    if (fread(&cn, 1, 1, f) != 1 || cn > CAT_MAX) { fclose(f); return -3; }
    if (cn && fread(cats, 1, (size_t)cn * CAT_SZ, f) != (size_t)cn * CAT_SZ) { fclose(f); return -3; }
    if (fread(&fn, 1, 1, f) != 1) { fclose(f); return -3; }   /* u8 ⇒ 最大 255, 越不过 FAV_MAX */

    /* ★ 记录长度: v2 固定 sizeof(fav_t); v1 按"剩余字节 = fn*rec + 22"反推 (3 种历史布局) */
    long rest = fsz - ftell(f);
    size_t rec = 0;
    if (v2) rec = sizeof(fav_t);
    else {
        long payload = rest - 22;
        if (fn == 0) rec = sizeof(fav_t);
        else if (payload == (long)fn * (long)sizeof(fav_t))     rec = sizeof(fav_t);
        else if (payload == (long)fn * (long)sizeof(fav_v2_t))  rec = sizeof(fav_v2_t);
        else if (payload == (long)fn * (long)sizeof(fav_old_t)) rec = sizeof(fav_old_t);
    }
    if (!rec) { fclose(f); return -3; }

    memset(s_fav, 0, sizeof(fav_t) * FAV_MAX);
    for (int i = 0; i < (int)fn; i++) {
        uint8_t buf[sizeof(fav_t)];
        if (fread(buf, 1, rec, f) != rec) { fclose(f); return -3; }
        memset(&s_fav[i], 0, sizeof(fav_t));
        if (rec == sizeof(fav_t)) {
            memcpy(&s_fav[i], buf, sizeof(fav_t));
        } else if (rec == sizeof(fav_v2_t)) {
            fav_v2_t *o = (fav_v2_t *)buf;
            s_fav[i].icon = o->icon; s_fav[i].cat = o->cat;
            memcpy(s_fav[i].name, o->name, sizeof(o->name));
            memcpy(s_fav[i].desc, o->desc, sizeof(o->desc));
            memcpy(s_fav[i].url,  o->url,  sizeof(o->url));
            s_fav[i].nc = o->nc;
            memcpy(s_fav[i].cr, o->cr, sizeof(o->cr));
        } else {
            fav_old_t *o = (fav_old_t *)buf;
            s_fav[i].icon = o->icon; s_fav[i].cat = o->cat;
            memcpy(s_fav[i].name, o->name, sizeof(o->name));
            memcpy(s_fav[i].desc, o->desc, sizeof(o->desc));
            s_fav[i].nc = o->nc;
            memcpy(s_fav[i].cr, o->cr, sizeof(o->cr));
        }
        if (s_fav[i].icon >= MAIN_ICON_COUNT) s_fav[i].icon = 0;
    }
    s_n = fn;

    /* v2: 逐条读图标段 */
    if (v2) {
        uint8_t bmp[FAV_ICON_BYTES];
        for (int i = 0; i < (int)fn; i++) {
            uint8_t has = 0;
            if (fread(&has, 1, 1, f) != 1) { fclose(f); return -3; }
            if (has) {
                if (fread(bmp, 1, FAV_ICON_BYTES, f) != FAV_ICON_BYTES) { fclose(f); return -3; }
                fav_icon_set(i, bmp);
            } else {
                fav_icon_del(i);
            }
        }
        /* 清掉备份里没有的多余图标槽 */
        for (int i = (int)fn; i < FAV_MAX; i++) fav_icon_del(i);
    }

    if (fread(&pb, 1, 1, f) == 1 && fread(&pl, 1, 1, f) == 1 && pl <= 20 &&
        fread(pinblob, 1, 20, f) == 20) {
        memcpy(s_cats, cats, sizeof(cats)); s_cat_n = cn;
        if (s_cat_n <= 0) s_cat_n = 1;
        for (int i = 0; i < s_n; i++) if (s_fav[i].cat >= s_cat_n) s_fav[i].cat = 0;
        /* PIN 归系统 keyvault(统一配置), 备份仅恢复收藏与分类, 不恢复 PIN */
        fav_save(); cats_save();
        s_fcol_dirty = true;
        rc = 0;
    }
    fclose(f);
    return rc;
}

/* ================= 网页配置 (热点 AP / IP STA) =================
 * 1:1 复用密钥管理器/局域网手柄模式: 未连WiFi → 系统"连接WiFi"流程 → 连接成功后
 * 启动服务 → toast IP 提示. 热点模式自建 "LinTOS-FAV" 热点 (有 PIN 则作 Wi-Fi 密码). */
/* ================= 自定义图标 (32×32 1bpp, NVS linfav/imgNN) ================
 * 设备不解码图片: 浏览器把图转成 32×32 黑白位图(128B)后上传, 这里只存取。 */
#define FAV_ICON_BYTES 128

static void fav_img_key(int gi, char *k, size_t n) {
    /* "img" + 两位十进制. ⛔ 别用 snprintf("%02d"): GCC 无法证明位数 ⇒ 告警,
     * 而且历史调用方只给了 5 字节缓冲。gi 有效范围 0..FAV_MAX-1 (<100)。 */
    if (!k || n < 6) return;
    unsigned v = (gi < 0) ? 0u : ((unsigned)gi > 99u ? 99u : (unsigned)gi);
    k[0]='i'; k[1]='m'; k[2]='g';
    k[3]=(char)('0' + (v / 10u));
    k[4]=(char)('0' + (v % 10u));
    k[5]='\0';
}

static bool fav_icon_get(int gi, uint8_t *out) {
    nvs_handle_t h; char k[8]; fav_img_key(gi, k, sizeof(k));
    if (!out || svc_nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = FAV_ICON_BYTES;
    bool ok = (svc_nvs_get_blob(h, k, out, &sz) == ESP_OK && sz == FAV_ICON_BYTES);
    svc_nvs_close(h);
    return ok;
}
static void fav_icon_set(int gi, const uint8_t *bmp) {
    nvs_handle_t h; char k[8]; fav_img_key(gi, k, sizeof(k));
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_set_blob(h, k, bmp, FAV_ICON_BYTES);
    svc_nvs_commit(h); svc_nvs_close(h);
}
static void fav_icon_del(int gi) {
    nvs_handle_t h; char k[8]; fav_img_key(gi, k, sizeof(k));
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_erase_key(h, k);
    svc_nvs_commit(h); svc_nvs_close(h);
}

/* 32×32 → 任意尺寸 (最近邻), cx/cy 为中心 */
__attribute__((unused)) static void draw_icon32(st7305_handle_t *l, int cx, int cy, int size, const uint8_t *bmp) {
    int x0 = cx - size / 2, y0 = cy - size / 2;
    for (int dy = 0; dy < size; dy++) {
        int sy = dy * 32 / size;
        const uint8_t *row = bmp + sy * 4;
        for (int dx = 0; dx < size; dx++) {
            int sx = dx * 32 / size;
            if (row[sx >> 3] & (0x80 >> (sx & 7)))
                st7305_draw_pixel(l, x0 + dx, y0 + dy, ST7305_COLOR_BLACK);
        }
    }
}

#include "fav_web_page.h"   /* 内嵌网页 HTML/JS (纯字符串, 已拆出) */

static esp_err_t fav_web_send_json(httpd_req_t *req, const char *s)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, s);
    return ESP_OK;
}
static esp_err_t fav_web_closed(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"closed\"}");
}

static esp_err_t fav_web_index_handler(httpd_req_t *req)
{
    if (!s_web_allow) return fav_web_closed(req);      /* ★ 未点「局域网配置」前不对外 */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, fav_web_html(), HTTPD_RESP_USE_STRLEN);
}
/* ★ 2026-10-19: 局域网网页需要 PIN 才能读写 (token 30 分钟有效) */
static char    s_web_tok[17];
static int64_t s_web_tok_us;

static bool fav_web_authed(httpd_req_t *req)
{
    if (!s_web_tok[0]) return false;
    if (esp_timer_get_time() - s_web_tok_us > 30LL * 60 * 1000000) return false;
    /* ★★ 2026-10-20: 不能再用 httpd_query_key_value + 固定小缓冲 —— 参数一长
     *   (比如"介绍"塞满中文, URL 编码后上百字节) 查询串被截断, 末尾的 t= 就丢了
     *   ⇒ 401 ⇒ 网页跳回 PIN 输入。直接在完整 uri 里扫 t= 才稳。 */
    /* ⛔ 不能直接 strstr(uri,"t="): `cat=` 里就含子串 "t=" ⇒ 每次都匹配到 cat 的值,
     *   校验必然失败 ⇒ 加网站/改分类全变 401 跳 PIN。必须只认 **独立的** t= 键。 */
    const char *p = NULL;
    for (const char *q = req->uri; (q = strstr(q, "t=")) != NULL; q += 2) {
        if (q == req->uri || q[-1] == '?' || q[-1] == '&') { p = q; break; }
    }
    if (!p) return false;
    p += 2;
    char v[24]; int k = 0;
    while (*p && *p != '&' && k < (int)sizeof(v) - 1) v[k++] = *p++;
    v[k] = 0;
    return strcmp(v, s_web_tok) == 0;
}

static esp_err_t fav_web_deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"auth\"}");
}

/* GET /api/login?pin=xxxx → {"ok":true,"t":"<16hex>"} */
static esp_err_t fav_web_login_handler(httpd_req_t *req)
{
    char q[160], pin[24] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "pin", pin, sizeof(pin));
    os_keyvault_init();
    if (os_keyvault_verify_pin(pin) != 0) return fav_web_deny(req);
    uint64_t r = ((uint64_t)esp_random() << 32) | esp_random();
    snprintf(s_web_tok, sizeof(s_web_tok), "%016llx", (unsigned long long)r);
    s_web_tok_us = esp_timer_get_time();
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"t\":\"%s\"}", s_web_tok);
    return fav_web_send_json(req, out);
}

static esp_err_t fav_web_list_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* ★ 2026-10-19: 必须用 chunked 发送 —— 多次 httpd_resp_sendstr 只有第一次生效,
     *   后面的内容连同 Content-Length 全乱, 浏览器拿到空 body ⇒ 页面不显示任何分类/条目。 */
    httpd_resp_sendstr_chunk(req, "{\"cats\":[");
    for (int i = 0; i < s_cat_n; i++) {
        char cb[64];
        snprintf(cb, sizeof(cb), "%s\"%.*s\"", i ? "," : "", CAT_SZ, s_cats[i]);
        httpd_resp_sendstr_chunk(req, cb);
    }
    httpd_resp_sendstr_chunk(req, "],\"items\":[");
    for (int i = 0; i < s_n; i++) {
        char b[768];
        char en[80], ed[160], ew[200];
        fav_json_esc(en, sizeof(en), s_fav[i].name);
        fav_json_esc(ed, sizeof(ed), s_fav[i].desc);
        fav_json_esc(ew, sizeof(ew), s_fav[i].url);
        int off = snprintf(b, sizeof(b),
                 "%s{\"id\":%d,\"cat\":%d,\"name\":\"%s\",\"desc\":\"%s\",\"url\":\"%s\",\"creds\":[",
                 i ? "," : "", i, s_fav[i].cat, en, ed, ew);
        for (int c = 0; c < s_fav[i].nc && c < CR_MAX && off < (int)sizeof(b) - 120; c++) {
            char eu[80], ep[80];
            fav_json_esc(eu, sizeof(eu), s_fav[i].cr[c].user);
            fav_json_esc(ep, sizeof(ep), s_fav[i].cr[c].pass);
            off += snprintf(b + off, sizeof(b) - off, "%s{\"u\":\"%s\",\"p\":\"%s\"}",
                            c ? "," : "", eu, ep);
        }
        snprintf(b + off, sizeof(b) - off, "]}");
        httpd_resp_sendstr_chunk(req, b);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);   /* 结束 chunked */
    return ESP_OK;
}
/* ★ 2026-10-19: httpd_query_key_value **不做 percent 解码** —— 中文会以 %E8%AE...
 *   原样落库 (网页里显示成 Chiphell%E8%A...)。统一走这个函数取参 + 解码。 */
static void fav_qdec(const char *qs, const char *key, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    char raw[384];
    if (httpd_query_key_value(qs, key, raw, sizeof(raw)) != ESP_OK) return;
    size_t o = 0;
    for (const char *p = raw; *p && o + 1 < cap; p++) {
        if (*p == '%' && fav_hexv(p[1]) >= 0 && fav_hexv(p[2]) >= 0) {
            out[o++] = (char)((fav_hexv(p[1]) << 4) | fav_hexv(p[2]));
            p += 2;
        } else {
            out[o++] = *p;
        }
    }
    /* 截断后回退到完整 UTF-8 字符边界, 不能留半个汉字 */
    while (o > 0) {
        size_t k = o - 1;
        while (k > 0 && ((unsigned char)out[k] & 0xC0) == 0x80) k--;
        unsigned char c = (unsigned char)out[k];
        size_t need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (o - k >= need) break;
        o = k;
    }
    out[o] = 0;
}

static esp_err_t fav_web_add_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char qs[768], cat[8] = "0", n[64], d[96], w[192], u[48], p[48];
    n[0] = d[0] = w[0] = u[0] = p[0] = 0;
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) return fav_web_send_json(req, "{\"ok\":false}");
    httpd_query_key_value(qs, "cat", cat, sizeof(cat));
    fav_qdec(qs, "n", n, sizeof(n));
    fav_qdec(qs, "d", d, sizeof(d));
    fav_qdec(qs, "w", w, sizeof(w));
    fav_qdec(qs, "u", u, sizeof(u));
    fav_qdec(qs, "p", p, sizeof(p));
    int ok = 0, new_id = -1;
    if (n[0] && s_n < FAV_MAX) {
        int c = atoi(cat); if (c < 0 || c >= s_cat_n) c = 0;
        fav_t *f = &s_fav[s_n];
        memset(f, 0, sizeof(fav_t));
        f->cat = (uint8_t)c;
        fav_u8cpy(f->name, sizeof(f->name), n);
        fav_u8cpy(f->desc, sizeof(f->desc), d);
        fav_u8cpy(f->url,  sizeof(f->url),  w);
        if (u[0] || p[0]) {
            fav_u8cpy(f->cr[0].user, sizeof(f->cr[0].user), u);
            fav_u8cpy(f->cr[0].pass, sizeof(f->cr[0].pass), p);
            f->nc = 1;
        }
        s_n++;
        fav_save();
        s_fcol_dirty = true;
        ok = 1;
        new_id = s_n - 1;
    }
    char out[64];
    if (!ok) snprintf(out, sizeof(out), "{\"ok\":false,\"err\":\"%s\"}", (s_n >= FAV_MAX) ? "full" : "name");
    else     snprintf(out, sizeof(out), "{\"ok\":true,\"id\":%d}", new_id);
    return fav_web_send_json(req, out);
}
static esp_err_t fav_web_del_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char qs[64], id[8] = "0";
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK)
        httpd_query_key_value(qs, "id", id, sizeof(id));
    int i = atoi(id), ok = 0;
    if (i >= 0 && i < s_n) {
        /* ★ 自定义图标按"条目序号"存 NVS(imgNN)，删条目必须同步左移，否则图标错位 */
        uint8_t tmp[FAV_ICON_BYTES];
        for (int k = i; k < s_n - 1; k++) {
            if (fav_icon_get(k + 1, tmp)) fav_icon_set(k, tmp);
            else                          fav_icon_del(k);
        }
        fav_icon_del(s_n - 1);
        for (int k = i; k < s_n - 1; k++) s_fav[k] = s_fav[k + 1];
        s_n--;
        fav_save();
        s_fcol_dirty = true;
        ok = 1;
    }
    char out[32]; snprintf(out, sizeof(out), "{\"ok\":%s}", ok ? "true" : "false");
    return fav_web_send_json(req, out);
}
static esp_err_t fav_web_cat_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char qs[512], op[8], id[8], to[8], n[96];
    op[0] = id[0] = to[0] = n[0] = 0;
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) return fav_web_send_json(req, "{\"ok\":false}");
    httpd_query_key_value(qs, "op", op, sizeof(op));
    httpd_query_key_value(qs, "id", id, sizeof(id));
    httpd_query_key_value(qs, "to", to, sizeof(to));
    fav_qdec(qs, "n", n, sizeof(n));
    int rc = -1;
    if (strcmp(op, "add") == 0) {
        if (s_cat_n < CAT_MAX && n[0]) { fav_u8cpy(s_cats[s_cat_n], CAT_SZ, n); s_cat_n++; cats_save(); rc = 0; }
    } else if (strcmp(op, "del") == 0) {
        int c = atoi(id);
        if (c >= 0 && c < s_cat_n) {
            for (int k = c; k < s_cat_n - 1; k++) memcpy(s_cats[k], s_cats[k + 1], CAT_SZ);
            s_cat_n--;
            for (int i = 0; i < s_n; i++) {   /* 分类整体左移, 被删分类条目并入首分类 */
                if (s_fav[i].cat == (uint8_t)c) s_fav[i].cat = 0;
                else if (s_fav[i].cat > (uint8_t)c) s_fav[i].cat--;
            }
            cats_save(); fav_save();
            rc = 0;
        }
    } else if (strcmp(op, "mov") == 0) {
        /* ★ 2026-10-20: 网页里拖动分类排序 —— 数组移动 + 每条记录的 cat 索引跟着重映射 */
        int a = atoi(id), b = atoi(to);
        if (a >= 0 && a < s_cat_n && b >= 0 && b < s_cat_n && a != b) {
            char tmp[CAT_SZ];
            memcpy(tmp, s_cats[a], CAT_SZ);
            if (a < b) { for (int k = a; k < b; k++) memcpy(s_cats[k], s_cats[k + 1], CAT_SZ); }
            else       { for (int k = a; k > b; k--) memcpy(s_cats[k], s_cats[k - 1], CAT_SZ); }
            memcpy(s_cats[b], tmp, CAT_SZ);
            for (int i = 0; i < s_n; i++) {
                int c = s_fav[i].cat;
                if (c == a) c = b;
                else if (a < b && c > a && c <= b) c--;
                else if (a > b && c >= b && c < a) c++;
                s_fav[i].cat = (uint8_t)c;
            }
            cats_save(); fav_save();
            s_fcol_dirty = true;
            rc = 0;
        }
    } else if (strcmp(op, "ren") == 0) {
        int c = atoi(id);
        if (c >= 0 && c < s_cat_n && n[0]) { fav_u8cpy(s_cats[c], CAT_SZ, n); cats_save(); rc = 0; }
    }
    char out[32]; snprintf(out, sizeof(out), "{\"ok\":%s}", rc == 0 ? "true" : "false");
    return fav_web_send_json(req, out);
}

/* 热点接入: 统一 PIN 归 keyvault 且不暴露明文, 网页配置 AP 改为开放(WPA2 组网保底) */
static void fav_ap_password(char *out, size_t sz){ if (sz) out[0] = 0; }

/* ---- base64 解码 (仅用于图标 128B) ---- */
static int b64v(char c){
    if (c>='A'&&c<='Z') return c-'A';
    if (c>='a'&&c<='z') return c-'a'+26;
    if (c>='0'&&c<='9') return c-'0'+52;
    if (c=='+') return 62;
    if (c=='/') return 63;
    return -1;
}
static int b64_decode(const char *in, uint8_t *out, int cap){
    int o=0; int buf=0, bits=0;
    for (; *in; in++){
        if (*in=='=' ) break;
        int v=b64v(*in); if (v<0) continue;
        buf=(buf<<6)|v; bits+=6;
        if (bits>=8){ bits-=8; if (o<cap) out[o++]=(uint8_t)((buf>>bits)&0xFF); }
    }
    return o;
}

/* GET /api/edit?id=&n=&d=&w=(网址)&u=&p=  —— 改名称/介绍/网址/账号/密码(第 0 条) */
static esp_err_t fav_web_edit_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char q[768], id[8]={0}, n[64]={0}, d[96]={0}, w[192]={0}, u[48]={0}, p[48]={0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return fav_web_deny(req);
    httpd_query_key_value(q, "id", id, sizeof(id));
    fav_qdec(q, "n", n, sizeof(n));
    fav_qdec(q, "d", d, sizeof(d));
    fav_qdec(q, "w", w, sizeof(w));
    fav_qdec(q, "u", u, sizeof(u));
    fav_qdec(q, "p", p, sizeof(p));
    int i = atoi(id);
    bool ok = (i >= 0 && i < s_n);
    if (ok) {
        if (n[0]) fav_u8cpy(s_fav[i].name, sizeof(s_fav[i].name), n);
        if (d[0]) fav_u8cpy(s_fav[i].desc, sizeof(s_fav[i].desc), d);
        if (w[0]) fav_u8cpy(s_fav[i].url, sizeof(s_fav[i].url), w);
        if (u[0] || p[0]) {
            if (s_fav[i].nc == 0) s_fav[i].nc = 1;
            if (u[0]) fav_u8cpy(s_fav[i].cr[0].user, sizeof(s_fav[i].cr[0].user), u);
            if (p[0]) fav_u8cpy(s_fav[i].cr[0].pass, sizeof(s_fav[i].cr[0].pass), p);
        }
        fav_save();
        s_fcol_dirty = true;
    }
    return fav_web_send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* ---- 网页抓取: 设备侧直接 GET 页面, 解析 <title> 与 meta description ---- */
static void fav_json_esc(char *d, size_t cap, const char *src)
{
    size_t o = 0;
    if (!d || cap < 2) return;
    for (const unsigned char *p = (const unsigned char *)src; p && *p && o + 2 < cap; p++) {
        if (*p == '"' || *p == '\\') { d[o++] = '\\'; d[o++] = (char)*p; }
        else if (*p == '\n' || *p == '\r' || *p == '\t') { d[o++] = ' '; }
        else d[o++] = (char)*p;
    }
    d[o] = 0;
}

/* ASCII 大小写不敏感查找 (newlib 的 strcasestr 不保证可用, 自己写一个) */
static const char *fav_stristr(const char *h, const char *n)
{
    if (!h || !n || !*n) return NULL;
    size_t ln = strlen(n);
    for (const char *p = h; *p; p++) {
        size_t i = 0;
        while (i < ln && p[i] && (char)(p[i] | 0x20) == (char)(n[i] | 0x20)) i++;
        if (i == ln) return p;
    }
    return NULL;
}
/* 去掉尾部被截断的半个 UTF-8 字符 */
static void fav_u8_fix_tail(char *s)
{
    size_t o = strlen(s);
    while (o > 0) {
        size_t k = o - 1;
        while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
        unsigned char c = (unsigned char)s[k];
        size_t need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (o - k >= need) break;
        o = k;
    }
    s[o] = 0;
}
static void fav_trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int L = (int)strlen(s);
    while (L > 0 && (s[L-1] == ' ' || s[L-1] == '\t' || s[L-1] == '\r' || s[L-1] == '\n')) s[--L] = 0;
}

/* 在 HTML 里找 <title> 与 meta description */
static void fav_html_meta(const char *h, char *title, size_t tc, char *desc, size_t dc)
{
    if (title && tc) title[0] = 0;
    if (desc && dc)  desc[0] = 0;
    if (!h || !title || !desc) return;

    const char *t = fav_stristr(h, "<title");
    if (t) {
        const char *a = strchr(t, '>');
        const char *z = a ? fav_stristr(a, "</title") : NULL;
        if (a && z && z > a + 1) {
            int n = (int)(z - a - 1);
            if (n > (int)tc - 1) n = (int)tc - 1;
            memcpy(title, a + 1, n); title[n] = 0;
            fav_u8_fix_tail(title); fav_trim(title);
        }
    }
    const char *m = fav_stristr(h, "name=\"description\"");
    if (!m) m = fav_stristr(h, "name='description'");
    if (!m) m = fav_stristr(h, "property=\"og:description\"");
    if (m) {
        const char *c = fav_stristr(m, "content=");
        if (c) {
            const char *q = c + 8;
            char qc = *q;
            if (qc == '"' || qc == '\'') {
                const char *z = strchr(q + 1, qc);
                if (z && z > q + 1) {
                    int n = (int)(z - q - 1);
                    if (n > (int)dc - 1) n = (int)dc - 1;
                    memcpy(desc, q + 1, n); desc[n] = 0;
                    fav_u8_fix_tail(desc); fav_trim(desc);
                }
            }
        }
    }
}

/* GET /api/iconhex?id=N  —— 返回该条目设备里存的 32×32 1bpp 位图 (256 hex 字符).
 * 网页列表要显示"我收藏里存的那个图标", 而不是网站 favicon。 */
static esp_err_t fav_web_iconhex_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char q[64], id[8] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "id", id, sizeof(id));
    int i = atoi(id);
    uint8_t bmp[FAV_ICON_BYTES];
    if (i < 0 || i >= s_n || !fav_icon_get(i, bmp)) return fav_web_send_json(req, "{\"ok\":false}");
    char hex[FAV_ICON_BYTES * 2 + 1], out[FAV_ICON_BYTES * 2 + 40];
    for (int k = 0; k < FAV_ICON_BYTES; k++) snprintf(hex + k * 2, 3, "%02x", bmp[k]);
    snprintf(out, sizeof(out), "{\"ok\":true,\"hex\":\"%s\"}", hex);
    return fav_web_send_json(req, out);
}

/* GET /api/iconraw?url=<enc>  —— 设备去下载 favicon 原样回传.
 * 浏览器读跨域图片像素会被 canvas 污染 (SecurityError), 走设备同源就没这个问题。 */
static esp_err_t fav_web_iconraw_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char q[384], u[224] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return fav_web_send_json(req, "{\"ok\":false}");
    fav_qdec(q, "url", u, sizeof(u));
    if (!u[0]) return fav_web_send_json(req, "{\"ok\":false}");
    char url[256];
    if (strncmp(u, "http", 4) != 0) snprintf(url, sizeof(url), "https://%s", u);
    else                            snprintf(url, sizeof(url), "%s", u);

    char *buf = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return fav_web_send_json(req, "{\"ok\":false,\"err\":\"mem\"}");
    netc_req_t r = {0};
    r.url = url; r.timeout_ms = 8000;
    r.body = buf; r.body_cap = 32768;
    if (!netc_start(&r)) { free(buf); return fav_web_send_json(req, "{\"ok\":false,\"err\":\"busy\"}"); }

    int64_t t0 = esp_timer_get_time();
    while (netc_state() == NETC_RUN && esp_timer_get_time() - t0 < 9000000) vTaskDelay(pdMS_TO_TICKS(20));
    int http = netc_http(), n = netc_len();
    int ok = (netc_state() == NETC_DONE && http >= 200 && http < 300 && n > 0);
    if (ok) {
        httpd_resp_set_type(req, "image/x-icon");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, buf, n);
    } else {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "no");
    }
    netc_reset();
    free(buf);
    return ESP_OK;
}

/* GET /api/meta?url=  —— 设备侧抓标题/描述 (浏览器跨域拿不到时用这条) */
static esp_err_t fav_web_meta_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char q[384], u[224] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return fav_web_send_json(req, "{\"ok\":false}");
    fav_qdec(q, "url", u, sizeof(u));
    if (!u[0]) return fav_web_send_json(req, "{\"ok\":false}");
    char url[256];
    if (strncmp(u, "http", 4) != 0) snprintf(url, sizeof(url), "https://%s", u);
    else                            snprintf(url, sizeof(url), "%s", u);

    char *body = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return fav_web_send_json(req, "{\"ok\":false,\"err\":\"mem\"}");

    netc_req_t r = {0};
    r.url = url; r.timeout_ms = 8000;
    r.body = body; r.body_cap = 16384;
    if (!netc_start(&r)) { free(body); return fav_web_send_json(req, "{\"ok\":false,\"err\":\"busy\"}"); }

    int64_t t0 = esp_timer_get_time();
    while (netc_state() == NETC_RUN && esp_timer_get_time() - t0 < 9000000) vTaskDelay(pdMS_TO_TICKS(20));
    int http = netc_http();
    int ok = (netc_state() == NETC_DONE && http >= 200 && http < 400);

    char title[160] = {0}, desc[260] = {0};
    if (ok) fav_html_meta(body, title, sizeof(title), desc, sizeof(desc));
    netc_reset();
    free(body);

    char te[340], de[540], out[1000];
    fav_json_esc(te, sizeof(te), title);
    fav_json_esc(de, sizeof(de), desc);
    snprintf(out, sizeof(out), "{\"ok\":%s,\"title\":\"%s\",\"desc\":\"%s\"}",
             (ok && (te[0] || de[0])) ? "true" : "false", te, de);
    return fav_web_send_json(req, out);
}

/* GET /api/move?id=&cat=  —— 把网站挪到另一个分类 (网页里把图标拖到分类上) */
static esp_err_t fav_web_move_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char q[96], id[8] = {0}, cat[8] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return fav_web_send_json(req, "{\"ok\":false}");
    httpd_query_key_value(q, "id", id, sizeof(id));
    httpd_query_key_value(q, "cat", cat, sizeof(cat));
    int i = atoi(id), c = atoi(cat), ok = 0;
    if (i >= 0 && i < s_n && c >= 0 && c < s_cat_n) {
        s_fav[i].cat = (uint8_t)c;
        fav_save();
        s_fcol_dirty = true;
        ok = 1;
    }
    return fav_web_send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* GET /api/cred?op=add|upd|del&id=&c=&u=&p=  —— 账号级增改删 */
static esp_err_t fav_web_cred_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char q[512], op[8]={0}, id[8]={0}, c[8]={0}, u[48]={0}, p[48]={0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return fav_web_send_json(req, "{\"ok\":false}");
    httpd_query_key_value(q, "op", op, sizeof(op));
    httpd_query_key_value(q, "id", id, sizeof(id));
    httpd_query_key_value(q, "c",  c,  sizeof(c));
    fav_qdec(q, "u", u, sizeof(u));
    fav_qdec(q, "p", p, sizeof(p));
    int i = atoi(id), ci = atoi(c);
    int rc = -1;
    if (i >= 0 && i < s_n) {
        fav_t *f = &s_fav[i];
        if (!strcmp(op, "add")) {
            if (f->nc < CR_MAX) {
                fav_u8cpy(f->cr[f->nc].user, sizeof(f->cr[0].user), u);
                fav_u8cpy(f->cr[f->nc].pass, sizeof(f->cr[0].pass), p);
                f->nc++; rc = 0;
            }
        } else if (!strcmp(op, "upd")) {
            if (ci >= 0 && ci < f->nc) {
                fav_u8cpy(f->cr[ci].user, sizeof(f->cr[0].user), u);
                fav_u8cpy(f->cr[ci].pass, sizeof(f->cr[0].pass), p);
                rc = 0;
            }
        } else if (!strcmp(op, "del")) {
            if (ci >= 0 && ci < f->nc) {
                for (int k = ci; k < f->nc - 1; k++) f->cr[k] = f->cr[k + 1];
                memset(&f->cr[f->nc - 1], 0, sizeof(f->cr[0]));
                f->nc--; rc = 0;
            }
        }
        if (rc == 0) { fav_save(); s_fcol_dirty = true; }
    }
    char out[32]; snprintf(out, sizeof(out), "{\"ok\":%s}", rc == 0 ? "true" : "false");
    return fav_web_send_json(req, out);
}

/* POST /api/icon?id=N  body=base64(128B 32×32 1bpp)   |   GET /api/icon?id=N&del=1 清除 */
static esp_err_t fav_web_icon_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char q[128], id[8]={0}, del[4]={0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "id", id, sizeof(id));
        httpd_query_key_value(q, "del", del, sizeof(del));
    }
    int i = atoi(id);
    if (i < 0 || i >= s_n) return fav_web_send_json(req, "{\"ok\":false}");
    if (del[0] == '1') { fav_icon_del(i); return fav_web_send_json(req, "{\"ok\":true}"); }

    char body[256] = {0};
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r <= 0) return fav_web_send_json(req, "{\"ok\":false}");
    uint8_t bmp[FAV_ICON_BYTES];
    int n = b64_decode(body, bmp, sizeof(bmp));
    if (n != FAV_ICON_BYTES) return fav_web_send_json(req, "{\"ok\":false,\"err\":\"len\"}");
    fav_icon_set(i, bmp);
    return fav_web_send_json(req, "{\"ok\":true}");
}

/* 通配分发: 只注册 首页 + api 通配 两个 handler (摆脱 max_uri_handlers 限制) */
static esp_err_t fav_web_api_dispatch(httpd_req_t *req)
{
    /* ★ 通配匹配时 req->uri 可能带查询串(?pin=...), 必须先截断再比较 */
    if (!s_web_allow) return fav_web_closed(req);      /* ★ 未点「局域网配置」前不对外 */
    char path[64];
    str_trunc(path, sizeof(path), req->uri);
    char *qm = strchr(path, '?'); if (qm) *qm = 0;
    const char *u = path;
    if (!strcmp(u, "/api/login")) return fav_web_login_handler(req);
    if (!strcmp(u, "/api/list"))  return fav_web_list_handler(req);
    if (!strcmp(u, "/api/add"))   return fav_web_add_handler(req);
    if (!strcmp(u, "/api/del"))   return fav_web_del_handler(req);
    if (!strcmp(u, "/api/cat"))   return fav_web_cat_handler(req);
    if (!strcmp(u, "/api/edit"))  return fav_web_edit_handler(req);
    if (!strcmp(u, "/api/cred"))  return fav_web_cred_handler(req);
    if (!strcmp(u, "/api/move"))  return fav_web_move_handler(req);
    if (!strcmp(u, "/api/meta"))  return fav_web_meta_handler(req);
    if (!strcmp(u, "/api/iconraw")) return fav_web_iconraw_handler(req);
    if (!strcmp(u, "/api/iconhex")) return fav_web_iconhex_handler(req);
    if (!strcmp(u, "/api/icon"))  return fav_web_icon_handler(req);
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, "{\"ok\":false}");
}

static void fav_web_register_uris(httpd_handle_t h)
{
    httpd_uri_t u;
    u.user_ctx = NULL;
    u.method = HTTP_GET;  u.uri = "/";       u.handler = fav_web_index_handler;
    httpd_register_uri_handler(h, &u);
    u.method = HTTP_ANY;  u.uri = "/api/*";  u.handler = fav_web_api_dispatch;
    httpd_register_uri_handler(h, &u);
}
static void fav_web_task(void *arg)   /* 热点(AP)模式: 自建热点 + 网页服务 */
{
    (void)arg;
    esp_err_t r;
    esp_netif_init();
    r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) { s_web_phase = 3; vTaskDelete(NULL); return; }

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ic.wifi_task_core_id = 0;
    r = esp_wifi_init(&ic);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) { s_web_phase = 3; vTaskDelete(NULL); return; }

    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    memcpy(ap.ap.ssid, FAV_AP_SSID, strlen(FAV_AP_SSID));
    ap.ap.ssid_len = strlen(FAV_AP_SSID);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    char pwd[20] = "";
    fav_ap_password(pwd, sizeof(pwd));
    if (pwd[0]) {
        memcpy(ap.ap.password, pwd, strlen(pwd));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.password[0] = 0;
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap.ap.pmf_cfg.required = false;
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK ||
        esp_wifi_start() != ESP_OK) { s_web_phase = 3; vTaskDelete(NULL); return; }
    os_ap_set(true);   /* 状态栏显示 NET 图标 */

    esp_netif_ip_info_t ip = { 0 };
    IP4_ADDR(&ip.ip, 8, 8, 8, 8); IP4_ADDR(&ip.gw, 8, 8, 8, 8);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           "http://8.8.8.8/", strlen("http://8.8.8.8/"));
    esp_netif_dhcps_start(ap_netif);
    dns_server_config_t dc = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_fav_dns = start_dns_server(&dc);

    if (s_fav_httpd == NULL) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.max_uri_handlers = 16;   /* 默认 8 不够 (注册 9 个 URI) */
        cfg.max_open_sockets = 4;
        cfg.stack_size = 8192;       /* ★ 与密钥管理器一致: 默认栈太小, httpd 任务起不来 */
        cfg.lru_purge_enable = true;
        cfg.lru_purge_enable = true; cfg.stack_size = 8192;
        cfg.uri_match_fn = httpd_uri_match_wildcard;   /* 不开通配匹配时 api 通配路径会 404 */
        {
            esp_err_t hr = httpd_start(&s_fav_httpd, &cfg);
            ESP_LOGW("FAV", "httpd_start=%d port=%d handlers=%d", (int)hr, cfg.server_port, (int)cfg.max_uri_handlers);
            if (hr == ESP_OK) fav_web_register_uris(s_fav_httpd);
        }
    }
    s_web_on = true;
    s_web_phase = (s_fav_httpd != NULL) ? 2 : 3;
    ESP_LOGI("FAV", "网页配置: 热点 %s %s", FAV_AP_SSID,
             (s_fav_httpd != NULL) ? "开启 (http://8.8.8.8)" : "开启失败");
    vTaskDelete(NULL);
}
static void fav_web_task_lan(void *arg)   /* IP配置(STA)模式: 复用当前WiFi, 只启动网页服务 */
{
    (void)arg;
    if (s_fav_httpd == NULL) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.max_uri_handlers = 16;   /* 默认 8 不够 (注册 9 个 URI) */
        cfg.max_open_sockets = 4;
        cfg.stack_size = 8192;       /* ★ 与密钥管理器一致: 默认栈太小, httpd 任务起不来 */
        cfg.lru_purge_enable = true;
        cfg.lru_purge_enable = true; cfg.stack_size = 8192;
        cfg.uri_match_fn = httpd_uri_match_wildcard;   /* 不开通配匹配时 api 通配路径会 404 */
        {
            esp_err_t hr = httpd_start(&s_fav_httpd, &cfg);
            ESP_LOGW("FAV", "httpd_start=%d port=%d handlers=%d", (int)hr, cfg.server_port, (int)cfg.max_uri_handlers);
            if (hr == ESP_OK) fav_web_register_uris(s_fav_httpd);
        }
    }
    s_web_on = true;
    s_web_phase = (s_fav_httpd != NULL) ? 2 : 3;
    if (s_web_open_req) { s_web_open_req = false; s_web_allow = true; s_lan_toast_pending = true; }
    ESP_LOGI("FAV", "IP配置: 收藏服务已%s", (s_fav_httpd != NULL) ? "开启" : "开启失败");
    vTaskDelete(NULL);
}

static void fav_web_stop(ui_ctx_t *ctx);
static void fav_web_house_stop(void) { fav_web_stop(NULL); }
static void fav_web_start_ap(ui_ctx_t *ctx)
{
    (void)ctx;
    if (s_web_on || s_web_phase == 1) return;
    s_web_mode = 0;                      /* 热点配置 = AP 模式 */
    s_web_phase = 1;
    xTaskCreate(fav_web_task, "favweb", 8192, NULL, 5, NULL);
    os_housekeeper_add(&fav_web_house_stop);   /* 回主菜单/软件管家时自动关停 */
}
void fav_web_debug_start(void);   /* remcon 调试入口: 只起服务, 不开放 */
int  fav_debug_export(void)
{
    fav_load();
    uint8_t bmp[FAV_ICON_BYTES]; int nico = 0;
    for (int i = 0; i < s_n; i++) if (fav_icon_get(i, bmp)) nico++;
    int r = fav_export_file(FAV_BACKUP_PATH);
    FILE *f = fopen(FAV_BACKUP_PATH, "rb");
    long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
    printf("FAVEXP rc=%d items=%d icons=%d size=%ld\r\n", r, s_n, nico, sz);
    return r;
}
int  fav_debug_import(void)
{
    fav_load();
    FILE *f = fopen(FAV_BACKUP_PATH, "rb");
    if (!f) { printf("BAK no file\r\n"); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char mg[8] = {0}; unsigned char cn = 0, fn = 0;
    fread(mg, 1, 7, f); fread(&cn, 1, 1, f);
    char cats[CAT_MAX][CAT_SZ];
    if (cn > 0 && cn <= CAT_MAX) fread(cats, 1, (size_t)CAT_SZ * cn, f);
    fread(&fn, 1, 1, f);
    long pos = ftell(f);
    printf("BAK sz=%ld cn=%u fn=%u rest=%ld | s3=%d s2=%d s1=%d\r\n",
           sz, (unsigned)cn, (unsigned)fn, sz - pos,
           (int)sizeof(fav_t), (int)sizeof(fav_v2_t), (int)sizeof(fav_old_t));
    fclose(f);
    return fav_import_file(FAV_BACKUP_PATH);
}
void fav_web_debug_open(void);    /* remcon 调试入口: 显式对外开放 (等价点「局域网配置」) */
void fav_web_debug_open(void) { s_web_allow = true; }

static volatile bool s_dbg_redraw = false;   /* 串口调试: 请求下一帧重绘 (由 fav_poll 消费) */
static st7305_handle_t *s_dbg_lcd = NULL;   /* 串口调试: 最近一次的 lcd 句柄 */
/* ★ 编辑页内容滚动 (键盘常驻占掉底部 145px, 内容区只有 131px 必须能滚) */
static float s_ed_scroll = 0.0f;
/* 进入编辑页时的快照: 「不保存返回」用它还原 (linit 2026-10-21) */
static fav_t s_ed_snap;
static bool  s_ed_snap_ok  = false;
static bool  s_ed_snap_new = false;   /* 进入时是空白条目 ⇒ 放弃时整条删掉 */
static int   s_ed_track    = -1;      /* 上一帧看到的 s_drawer, 用于进入瞬间拍快照 */
/* 网址行单独横向滚动 (网址很长, 编辑时必须能看见光标) */
static int   s_url_scroll = 0;
static bool  s_url_drag = false;
static int   s_url_sc0 = 0;
static int   s_ed_drag_x0 = 0;
static bool  s_url_mode = false;   /* 本次手势横向拖网址 */
/* 图标选择面板 (底部 2 排, 左右拖动) */
static int   s_icon_scroll = 0;
static bool  s_icon_drag = false;
static int   s_icon_dx0 = 0, s_icon_sc0 = 0;
#define ICON_CELL_W 56
#define ICON_CELL_H 60
#define ICON_PT     (S_H - KBD_H)     /* 复用键盘位置 155 */
static bool  s_ed_drag = false;
static int   s_ed_drag_y0 = 0;
static float s_ed_scroll0 = 0.0f;
static bool  s_ed_drag_moved = false;
static bool  s_ed_anchored = false;    /* 已记录拖动起点 */
static bool  s_ed_swallow_tap = false;  /* 本次手势是滚动 ⇒ 吞掉紧随的 tap */

/* 串口调试: 清掉"账号/密码都为空"的空账号 (测试点击误加出来的) */
void fav_debug_clean_empty(void)
{
    fav_load();
    int removed = 0;
    for (int i = 0; i < s_n; i++) {
        fav_t *f = &s_fav[i];
        for (int c = f->nc - 1; c >= 0; c--) {
            if (f->cr[c].user[0] == 0 && f->cr[c].pass[0] == 0) {
                for (int k = c; k < f->nc - 1; k++) f->cr[k] = f->cr[k + 1];
                memset(&f->cr[f->nc - 1], 0, sizeof(cred_t));
                f->nc--; removed++;
            }
        }
    }
    if (removed) fav_save();
    ESP_LOGI("FAV", "favclean: 清掉 %d 个空账号", removed);
}

/* 串口调试: 把某个键盘整屏画出来, 用于比较选型 (which: 0=kbd_std 1=kbd_edit) */
void fav_debug_kbd_show(int which)
{
    st7305_handle_t *l = NULL;
    /* 借用当前页面的 lcd: 从 os 拿不到, 这里用静态缓存 (fav_enter 里存) */
    l = s_dbg_lcd;
    if (!l) { ESP_LOGW("FAV", "kbdshow: 还没进过收藏夹, 没有 lcd 句柄"); return; }
    st7305_clear(l, ST7305_COLOR_WHITE);
    if (which == 0) {
        const char *t1 = "kbd_std (5 排 / 145px)";
        draw_text(l, 8, 8, t1, false);
        kbd_draw(l, S_H - KBD_H, &kbd_std, -1);
    } else {
        const char *t2 = "kbd_edit (kbd_draw_edit)";
        draw_text(l, 8, 8, t2, false);
        kbd_draw_edit(l, S_H - 5*KBD_EDIT_ROW, &kbd_edit, -1);
    }
    st7305_set_full_window(l);
    st7305_flush_override(l);
    ESP_LOGI("FAV", "kbdshow: which=%d", which);
}

/* 串口调试专用: 直接打开"编辑页"抓图用 (跳过 PIN —— 只有物理串口能调, 不进 UI 路径) */
void fav_debug_edit(int idx)
{
    if (s_n <= 0) { ESP_LOGW("FAV", "favedit: 没有收藏可编辑"); return; }
    if (idx < 0 || idx >= s_n) idx = 0;
    fav_load();
    s_authed = true;
    s_drawer = idx;
    s_show_cats = false;
    s_edit_focus = false;
    s_field = 0;
    s_fav_import_dlg = false;
    s_dbg_redraw = true;            /* favedit 在 remcon 任务里, 拿不到 ctx ⇒ 让 poll 下一帧置脏 */
    ESP_LOGI("FAV", "favedit: 打开编辑页 idx=%d (跳过 PIN, 仅串口调试)", idx);
}
void fav_web_debug_start(void)
{
    if (s_web_on || s_web_phase == 1) { ESP_LOGI("FAV", "debug: 已在运行 on=%d phase=%d", (int)s_web_on, s_web_phase); return; }
    fav_load();                       /* ★ 调试入口: 先把数据装进内存, 否则列表为空没法测编辑 */
    s_web_mode = 1; s_web_phase = 1;
    ESP_LOGI("FAV", "debug: 启动 LAN 任务");
    xTaskCreate(fav_web_task_lan, "favweb", 8192, NULL, 5, NULL);
}
static void fav_web_start_lan_ex(ui_ctx_t *ctx, bool toast);
static void fav_web_start_lan(ui_ctx_t *ctx) { fav_web_start_lan_ex(ctx, true); }

static void fav_web_start_lan_ex(ui_ctx_t *ctx, bool toast)
{
    (void)ctx;
    if (s_web_on) return;            /* ★ 之前 phase==1 会永久卡死: 任务崩过一次就再也起不来 */
    s_web_phase = 1;
    s_web_mode = 1;                      /* IP配置 = STA 模式 */
    s_lan_pending = false;
    s_lan_toast_pending = toast;     /* ★ 预启动静默, 不弹提示 */
    s_web_phase = 1;
    xTaskCreate(fav_web_task_lan, "favweb", 8192, NULL, 5, NULL);
    /* ★ 2026-10-19: 网页服务期间关掉 WiFi 省电(PS) —— 否则每个请求要多等 ~100-300ms 唤起,
     *   页面点击明显发滞。服务停止时恢复默认省电。 */
    esp_wifi_set_ps(WIFI_PS_NONE);
    /* 不登记资源管家: IP/局域网服务在 toast IP 后回主桌面仍须运行 (1:1 局域网手柄), 仅由设置项手动关闭 */
}
static void fav_web_stop(ui_ctx_t *ctx)
{
    (void)ctx;
    if (!s_web_on && s_web_phase == 0) return;
    if (s_fav_httpd) { httpd_stop(s_fav_httpd); s_fav_httpd = NULL; }
    if (s_web_mode == 0) {   /* 热点(AP)模式额外清理: DNS + 恢复 STA + 状态栏NET图标 */
        if (s_fav_dns) { stop_dns_server(s_fav_dns); s_fav_dns = NULL; }
        os_ap_set(false);
        /* ★ 2026-10-20: 热点模式把 WiFi 设成了 WIFI_MODE_AP, 关闭后**必须切回 STA**,
         *   否则后面「连接Wi-Fi」扫描会直接报"扫描失败, 请重试"(mode 不是 STA 就没法扫)。 */
        esp_wifi_set_mode(WIFI_MODE_STA);
        if (wifi_manager_get_want_on()) {
            wifi_manager_enable();      /* 本来就想联网 → 重新拉起射频 */
            wifi_manager_autostart();   /* 有存档就自动重连 */
        } else {
            esp_wifi_stop();
        }
    }
    s_web_on = false;
    s_web_phase = 0;
    s_web_allow = false;
    s_web_open_req = false;
    s_lan_pending = false;
    s_lan_toast_pending = false;
    /* 恢复默认省电 (仅 STA 模式; AP 模式前面已 esp_wifi_stop) */
    if (s_web_mode == 1) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

/* ================= HID ================= */
static void send_char(char c){
    uint8_t mod=0,code=0;
    if(c>='a'&&c<='z'){ code=(uint8_t)(0x04+(c-'a')); }
    else if(c>='A'&&c<='Z'){ code=(uint8_t)(0x04+(c-'A')); mod=FAV_LSHIFT; }
    else if(c>='0'&&c<='9'){ code=(uint8_t)(30 + (c-'0')); }   /* HID 0x1E */
    else switch(c){
        case '.':code=0x37;break; case ',':code=0x36;break; case '/':code=0x38;break;
        case '-':code=0x2d;break; case ' ':code=0x2c;break;
        case '_':mod=FAV_LSHIFT;code=0x2d;break; case '@':mod=FAV_LSHIFT;code=0x1f;break;
        case ':':mod=FAV_LSHIFT;code=0x33;break; case '+':mod=FAV_LSHIFT;code=0x2e;break;
        default:return;
    }
    os_hid_key_tap(mod,code); vTaskDelay(3);
}
static void type_text(const char*s){ for(;s&&*s;s++){ send_char(*s); vTaskDelay(2);} }
/* ★ 2026-10-20: 只输入不回车 —— 账号后打一个 Tab 跳到密码框, 密码打完停手,
 *   由用户自己按回车提交 (以前自动回车会直接把表单提交掉). */
/* which: 0=只推账号 1=只推密码 */
static void do_input_part(ui_ctx_t*ctx,int gi,int ci,int pass){
    if(gi<0||gi>=s_n||ci<0||ci>=s_fav[gi].nc) return;
    if(!fav_hid_ensure()){ os_dialog_toast(ctx, "USB \xe9\x94\xae\xe9\xbc\xa0\xe6\x9c\xaa\xe5\xb0\xb1\xe7\xbb\xaa"); return; }
    type_text(pass ? s_fav[gi].cr[ci].pass : s_fav[gi].cr[ci].user);
}
/* 推送网址 (不回车) */
static void do_input_url(ui_ctx_t*ctx,int gi){
    if(gi<0||gi>=s_n) return;
    if(!fav_hid_ensure()){ os_dialog_toast(ctx, "USB \xe9\x94\xae\xe9\xbc\xa0\xe6\x9c\xaa\xe5\xb0\xb1\xe7\xbb\xaa"); return; }
    type_text(s_fav[gi].url);
}

/* ================= 低层绘制 ================= */
static void dot(st7305_handle_t*l,int x,int y){ if(x>=0&&x<S_W&&y>=0&&y<S_H) st7305_draw_pixel(l,x,y,ST7305_COLOR_BLACK);}

/* ★ 2026-10-21 视觉收敛: 分隔不再画"一条生直线的直线"——
 *   中段实线、两端渐隐成点阵 (参考 1bit-ui / e-ink 的 hairline + Nothing 的点阵语言),
 *   在 1bpp 上比整条实线柔和, 也更像"技术蓝图"而不是随手一条线。 */
static void ed_dotline(st7305_handle_t*l,int x0,int x1,int y){
    /* ★ 2026-10-21 linit: 下划线一律用**虚线** (整条隔点), 不要部分实线 */
    for(int x=x0;x<=x1;x++) if(!(x & 1)) dot(l,x,y);
}
/* ★ 编辑光标: 8×16 实心黑块, 400ms 一闪一闪 —— 一眼看出正在改哪一行 */
static void ed_caret(st7305_handle_t*l,int x,int y){
    if(((xTaskGetTickCount()*portTICK_PERIOD_MS)/400) & 1) return;
    fill_rect(l,x,y,x+7,y+15,ST7305_COLOR_BLACK);
}

static void text16(st7305_handle_t*l,int x,int y,const char*s,bool inv){
    while(s&&*s){ unsigned char c=(unsigned char)*s;
        if(c<0x80){ draw_ascii_sb(l,x,y,(char)c,inv); x+=8; s++; }
        else{ draw_zh_sb(l,x,y,s,inv); x+=16; s+=3; }
    }
}

/* ================= 主界面 ================= */
static void fav_gi(int fi, int *gi){
    int k=0; for(int i=0;i<s_n;i++){ if(s_fav[i].cat==s_cat){ if(fi==k){*gi=i; return;} k++; } }
    *gi=-1;
}
static int fav_max(void){ int k=0; for(int i=0;i<s_n;i++)if(s_fav[i].cat==s_cat)k++; return k; }

/* 按像素宽度裁剪 (UTF-8 安全) */
static void fav_clip(char *d, size_t cap, const char *src, int maxpx)
{
    size_t o = 0; int w = 0;
    if (!d || cap == 0) return;
    d[0] = 0;
    if (!src) return;
    for (size_t i = 0; src[i]; ) {
        unsigned char c = (unsigned char)src[i];
        size_t n = (c < 0x80) ? 1 : ((c >= 0xF0) ? 4 : ((c >= 0xE0) ? 3 : 2));
        int cw = (c < 0x80) ? 8 : 16;
        size_t j = 1; while (j < n && src[i + j]) j++;
        if (j < n) break;
        if (w + cw > maxpx || o + n >= cap) break;
        memcpy(d + o, src + i, n); o += n; w += cw; i += n;
    }
    d[o] = 0;
}

/* 「推送」按钮: 粗上箭头 (实心三角头 + 3px 竖杆), (cx,cy) 中心, 高 15 宽 13 */
static void ui_col_push(ui_col_t *c, int cx, int cy)
{
    for (int r = 0; r < 6; r++)                       /* 三角头 */
        for (int dx = -(r + 1); dx <= r + 1; dx++)
            ui_col_px(c, cx + dx, cy - 7 + r, true);
    for (int y2 = -1; y2 <= 7; y2++)                  /* 竖杆 3px */
        for (int dx = -1; dx <= 1; dx++)
            ui_col_px(c, cx + dx, cy + y2, true);
}

#define FAV_CR_H 56            /* 抽屉内每条账号: 账号/密码上下两排 + 间距 */

/* 右栏整列构建: 行高 52 = 图标 40(中心 28) + 名称 16px + 介绍 16px + 行底分割线 */
#define FC_X0 (LIST_X + 2)
#define FC_W  (S_W - 2 - FC_X0 + 1)
static void fav_col_build(void)
{
    int nF = fav_max();
    if (s_open >= s_n || s_open < -1) s_open = -1;
    int need_h = 52;                                   /* 末尾「＋ 添加收藏」 */
    for (int f = 0; f < nF; f++) {
        int gi = -1; fav_gi(f, &gi);
        need_h += 52;
        if (gi >= 0 && gi == s_open) need_h += s_fav[gi].nc * FAV_CR_H;
    }
    int view_h = S_H - HDR_H;
    if (need_h < view_h) need_h = view_h;          /* ★ 至少铺满可视区, 否则右侧残留上一屏 */
    if (!ui_col_init(&s_fcol, FC_W, need_h)) return;
    ui_col_clear(&s_fcol);

    int y = 0;
    for (int f = 0; f < nF; f++) {
        int gi = -1; fav_gi(f, &gi);
        if (gi < 0) break;
        fav_t *fv = &s_fav[gi];
        uint8_t icb[FAV_ICON_BYTES];
        if (fav_icon_get(gi, icb)) ui_col_icon32(&s_fcol, 28, y + 26, 40, icb);
        else                       ui_col_icon(&s_fcol, 28, y + 26, 40, fv->icon);
        /* 第一行: 名称 + 介绍 (介绍紧跟名称右侧) */
        char l1[80]; fav_clip(l1, sizeof(l1), fv->name, 104);
        ui_col_text16(&s_fcol, 56, y + 8, l1[0] ? l1 : " ", false);
        int nw = 0;
        for (const char *q = l1; *q; ) { if ((unsigned char)*q < 0x80) { nw += 8; q++; } else { nw += 16; q += 3; } }
        if (fv->desc[0]) {
            char d2[80]; fav_clip(d2, sizeof(d2), fv->desc, FC_W - 56 - nw - 8 - 28);
            if (d2[0]) ui_col_text16(&s_fcol, 56 + nw + 8, y + 8, d2, false);
        }
        /* 第二行: 网址 (原介绍的位置) */
        if (fv->url[0]) { char u2[96]; fav_clip(u2, sizeof(u2), fv->url, FC_W - 56 - 30); ui_col_text16(&s_fcol, 56, y + 28, u2, false); }
        /* 右侧推送按钮 = 推网址, 行内上下居中 */
        ui_col_push(&s_fcol, FC_W - 18, y + 26);
        ui_col_hline(&s_fcol, 8, FC_W - 9, y + 51, true);
        y += 52;
        /* ★ 下拉抽屉: 该网站的全部账号/密码 (密码打码) + 行尾推送箭头 */
        if (gi == s_open) {
            for (int c = 0; c < fv->nc; c++) {
                char u[64], m[24];
                fav_clip(u, sizeof(u), fv->cr[c].user, FC_W - 86 - 46);
                int pl = (int)strlen(fv->cr[c].pass);
                int ml = pl > 10 ? 10 : (pl > 0 ? pl : 0);
                for (int k = 0; k < ml; k++) m[k] = '*';
                m[ml] = 0;
                /* 账号行整体左移 10, 密码行右移 10 —— 两个按钮错开, 防误触 */
                ui_col_text16(&s_fcol, 46, y + 6,  "\xe8\xb4\xa6\xe5\x8f\xb7:", false);   /* 账号: */
                ui_col_text16(&s_fcol, 86, y + 6,  u[0] ? u : "-", false);
                ui_col_push(&s_fcol, FC_W - 47, y + 14);
                ui_col_text16(&s_fcol, 66, y + 32, "\xe5\xaf\x86\xe7\xa0\x81:", false);   /* 密码: */
                ui_col_text16(&s_fcol, 106, y + 32, m[0] ? m : "-", false);
                ui_col_push(&s_fcol, FC_W - 25, y + 40);
                y += FAV_CR_H;
            }
        }
    }
    /* 末尾「＋ 添加收藏」(纯文字, 居中) */
    {
        const char *plus = "\xef\xbc\x8b";
        const char *txt  = "\xe6\xb7\xbb\xe5\x8a\xa0\xe6\x94\xb6\xe8\x97\x8f";
        int w1 = text_width(plus), w2 = text_width(txt);     /* 24px 宽度 */
        int ax = (FC_W - (w1 + 4 + w2)) / 2;
        if (ax < 4) ax = 4;
        ui_col_text24(&s_fcol, ax, y + 14, plus, false);     /* ★ 这一行 24px */
        ui_col_text24(&s_fcol, ax + w1 + 4, y + 14, txt, false);
    }
}

static int text16w(const char *s);

static void main_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);   /* 与商店一致: 每帧整屏清 (驱动按字节写, 快) */
    /* 左栏: 一比一阅读器 os_pane 模板 (文字+水管分割线+选中框, 行高32) */
    s_pane.folder_count = s_cat_n;        /* 分类可增删, 每次同步 */
    s_pane.sel_folder   = pane_sel();
    os_pane_draw_left(l, &s_pane);
    /* 当前按住触点 (按住反黑, 松手恢复) */
    int tv=0, tx=-1, ty=-1;
    if (input_touch_now(&tx,&ty)) tv=1;
    /* 设置右栏: 各种设置项 */
    if (s_cat < 0) {
        /* ★ 与密钥管理器完全一致: 同顺序、同 32px 行高、同右栏居中 */
        /* ★ 已联网时, 最后一项直接显示当前 IP (浏览器输这个地址就能改收藏夹) */
        static char s_ipitem[40] = "IP\xe9\x85\x8d\xe7\xbd\xae";   /* IP配置 */
        {
            char ip[24] = {0};
            /* ★ 只有服务真正开启后才显示 IP; 未开启显示「网络配置」 */
            if (s_web_on && s_web_allow && wifi_manager_is_connected() && wifi_manager_get_ip(ip, sizeof(ip)) && ip[0])
                snprintf(s_ipitem, sizeof(s_ipitem), "IP %s", ip);
            else if (s_web_phase == 1)
                snprintf(s_ipitem, sizeof(s_ipitem), "\xe5\x90\xaf\xe5\x8a\xa8\xe4\xb8\xad\xe2\x80\xa6");  /* 启动中… */
            else
                snprintf(s_ipitem, sizeof(s_ipitem), "\xe5\xb1\x80\xe5\x9f\x9f\xe7\xbd\x91\xe9\x85\x8d\xe7\xbd\xae");  /* 局域网配置 */
        }
        const char *set_items[5] = {
            "\xe5\xaf\xbc\xe5\x87\xba\xe5\xa4\x87\xe4\xbb\xbd",  /* 导出备份 */
            "\xe5\xaf\xbc\xe5\x85\xa5\xe5\xa4\x87\xe4\xbb\xbd",  /* 导入备份 */
            "\xe4\xbf\xae\xe6\x94\xb9PIN",        /* 修改PIN */
            "\xe7\x83\xad\xe7\x82\xb9\xe9\x85\x8d\xe7\xbd\xae",  /* 热点配置 */
            s_ipitem,                                      /* IP配置 / IP x.x.x.x */
        };
        int y = HDR_H + 6 - s_scroll;
        for (int i = 0; i < 5; i++) {
            if (y + 32 >= HDR_H && y <= S_H - 10) {
                bool sel = (i == s_sel) || (tv && ty >= y && ty < y + 32 && tx >= LIST_X + 2 && tx <= S_W - 2);
                if (sel) fill_rect(l, LIST_X + 2, y, S_W - 2, y + 30, ST7305_COLOR_BLACK);
                draw_text_centered_in(l, LIST_X + 2, S_W - 2, y + 4, set_items[i], sel);
            }
            y += 32;
        }
        return;
    }
    /* ★ 右栏 = 整列缓冲 + 抄窗（商店同结构）: 内容变化才重建, 滚动只 blit */
    if (s_fcol_dirty) { fav_col_build(); s_fcol_dirty = false; }
    if (s_fcol.buf) ui_col_present(&s_fcol, l, LIST_X + 2, HDR_H, S_H - HDR_H, s_scroll);
}

/* 编辑抽屉 */
/* 下拉抽屉面板几何: 右栏区域 (状态栏下方展开, 不盖左栏/状态栏) */
#define DW_X0 (LIST_X + 2)   /* 114 */
#define DW_X1 (S_W - 2)
#define DW_Y0 HDR_H
#define DW_Y1 (S_H - 2)
#define DW_W  (DW_X1 - DW_X0 + 1)
static int text16w(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p < 0x80) { w += 8; p++; }
        else if ((*p & 0xF0) == 0xE0) { w += 16; p += 3; }
        else if ((*p & 0xE0) == 0xC0) { w += 16; p += 2; }
        else p++;
    }
    return w;
}
/* 下拉抽屉弹窗: 白底 + 2px 黑边框, 顶部标题居中, 图标/名称/介绍, 账号密码列表,
 * 底部按钮行贴面板底边 (添加账号/确定/取消, 棋盘分隔线). 编辑字段时下方浮动键盘. */
/* ★ 2026-10-19: **全屏编辑** (替代原底部抽屉): 名称 / 介绍 / 账号密码(最多10套) / 图标 / 完成 */
#define ED_ROW_H   34

/* ================= 全屏编辑页 v2 (2026-10-19 重做) =================
 * 统一规格: 全部 16px 文字; 行高 44; 左右边距 10; 只画框线, **不画黑底按钮**;
 * 反色只在手指按住时出现 (渲染时实时查触摸)。
 * 布局: 头部[返回 ... 完成] / 图标+名称+介绍 / 账号卡片 / 底部[＋ 添加账号]
 */
#define ED_HDR      24
#define ED_MX       10
/* ★ 2026-10-21 linit: 编辑页按"网页编辑器"重排 —— 每个字段一行 + 右侧「改」,
 *   账号一条两行(账号/密码) + 「改」「删」, 底部「＋ 添加账号」独立一行。
 *   原来只有名称/介绍、没有网址, 且密码明文、没有任何"可点"提示 ⇒ 用户不知道能编辑什么。 */
/* ★ 2026-10-21: 状态栏(26px)是独立服务画在最上层, 页面内容绝不能进 0..25 —— 原来
 *   头部画在 y=0 被状态栏整片盖住, 看着就是"字重叠"。 */
#define ED_SB       26                    /* 状态栏高度 */
#define ED_HDR_Y    ED_SB                 /* 返回/完成 行的 y */
#define ED_KBD_Y    (S_H - KBD_H)         /* 155 键盘出现时的上缘 (kbd_std 5 排全高) */
#define ED_VIEW_TOP ED_SB                 /* 26 内容可视区上界 (无头部行) */
#define ED_BTN_H    30                    /* 底部按钮行: 左「删除收藏」右「保存」 */
#define ED_BTN_Y    (S_H - ED_BTN_H)      /* 270 */
#define ED_VIEW_BOT (s_edit_focus ? ED_KBD_Y : ED_BTN_Y)   /* 有键盘 155, 无键盘 270 */
#define ED_F_H      26                    /* 行高 (左右两栏同高) */
#define ED_ICON_H   56                    /* 顶部图标行高 */
#define ED_ICON_W   48                    /* 图标显示边长 (64×64 源图四周有留白, 32 看着偏小) */
#define ED_LW       214                   /* 左栏宽: 名称/介绍/网址 */
#define ED_RX       (ED_LW + 4)           /* 右栏起点: 账号/密码 */
#define ED_CR    52                      /* 一条账号 = 账号行 26 + 密码行 26 */

/* 16px 文字按宽度截断 */
static void ed_t16(st7305_handle_t *l, int x, int y, const char *s, bool inv, int maxw){
    if(!s) return;
    char tmp[64]; tmp[0]=0; int n=0;   /* 必须先置空: 下面 text16w(tmp) 会先读它 */
    while(*s && n<(int)sizeof(tmp)-1){
        int adv = ((uint8_t)*s < 0x80) ? 1 : ((((uint8_t)*s & 0xF0)==0xE0) ? 3 : 2);
        if(text16w(tmp) + (adv==3?16:8) > maxw) break;
        for(int k=0;k<adv;k++) tmp[n++]=*s++;
        tmp[n]=0;
    }
    text16(l,x,y,tmp,inv);
}


/* 左栏三行的"显示顺序 → 字段号": 名称(0) → 介绍(2) → 网址(1) */
static const int L2F[3] = {0, 2, 1};

/* 内容总高: 左栏 3 行, 右栏 nc*52 + 添加账号; 取较大者 */
static int ed_add_cy(const fav_t *f){
    return f->nc*ED_CR + 4;      /* 紧跟右栏最后一条账号 (原来跟着左栏高度, 太靠下) */
}
static int ed_total_h(const fav_t *f){
    int lh = ED_ICON_H + 3*ED_F_H;
    int rh = ed_add_cy(f) + 24;
    return lh > rh ? lh : rh;
}
static int ed_maxs(const fav_t *f){
    int vh = ED_VIEW_BOT - ED_VIEW_TOP;
    int m = ed_total_h(f) - vh; return m > 0 ? m : 0;
}
/* 编辑页: 内容可滚动 + 头部固定 + 底部常驻 kbd_std(5 排全高) */
/* 横向平移绘制: 跳过左侧滚出可视区的字符, 剩下的从 x 开始画 (避免溢出到右栏) */
static void ed_draw_panned(st7305_handle_t*l,int x,int y,const char*s,int scroll,int maxw){
    int adv = 0; const char *p = s;
    while(*p){
        int n = ((uint8_t)*p < 0x80) ? 1 : 3;
        int w = ((uint8_t)*p < 0x80) ? 8 : 16;
        if(adv + w > scroll) break;
        adv += w; p += n;
    }
    ed_t16(l, x - (scroll - adv), y, p, false, maxw + (scroll - adv));
}

/* 网址行光标跟随: 让光标始终停在可视区右缘内 (编辑永远在末尾, 所以滚到末尾即可) */
static void ed_url_follow(void){
    int avail = (ED_LW - 24) - (ED_MX + 56);        /* 网址可视宽度 */
    int tw = text16w(s_buf);
    s_url_scroll = (tw + 10 > avail) ? (tw + 10 - avail) : 0;
}

static void drawer_render(st7305_handle_t*l){
    fav_t*f=&s_fav[s_drawer]; if(!f) return;
    st7305_clear(l, ST7305_COLOR_WHITE);

    /* 先把滚动量夹到合法范围 (内容变短时要回收) */
    const int vbot = ED_VIEW_BOT;                      /* 有键盘=155, 无键盘=300 */
    int mx = ed_maxs(f);
    if(s_ed_scroll > (float)mx) s_ed_scroll = (float)mx;
    if(s_ed_scroll < 0.0f)      s_ed_scroll = 0.0f;
    const int base = ED_VIEW_TOP - (int)s_ed_scroll;   /* 内容坐标 0 对应的屏幕 y */

    /* ---- 左栏 (名称 / 介绍 / 网址) ---- */
    static const char *LBL[3] = { "\xe5\x90\x8d\xe7\xa7\xb0\xef\xbc\x9a", "\xe4\xbb\x8b\xe7\xbb\x8d\xef\xbc\x9a",
                                  "\xe7\xbd\x91\xe5\x9d\x80\xef\xbc\x9a" };   /* 名称： / 介绍： / 网址： */
    /* 第 0 行: 当前图标 32×32 (左侧, 垂直居中), 点一下换图标 */
    {
        int y = base;
        if(y + ED_ICON_H > ED_VIEW_TOP && y < vbot){
            draw_icon_bitmap_stretched(l, (ED_MX + ED_LW - 2)/2, y+ED_ICON_H/2, ED_ICON_W, ED_ICON_W, f->icon);   /* 左栏内水平居中 */
            ed_dotline(l, ED_MX, ED_LW-2, y+ED_ICON_H-1);
        }
    }
    for(int r=0;r<3;r++){
        int y = base + ED_ICON_H + r*ED_F_H;   /* 字段在图标行之后 */
        if(y < ED_VIEW_TOP || y + ED_F_H > vbot) continue;
        int fi = L2F[r];
        bool act = s_edit_focus && s_field == fi;
        const char *v = act ? s_buf : (fi==0 ? f->name : (fi==1 ? f->url : f->desc));
        ed_t16(l, ED_MX,    y+5, LBL[r], false, 54);
        const char *vs = (v && v[0]) ? v : "-";
        int vmax = ED_LW-ED_MX-56-24;
        if(fi == 1){                                  /* 网址: 支持横向平移 */
            int sc = act ? s_url_scroll : 0;
            if(sc > 0) ed_draw_panned(l, ED_MX+56, y+5, vs, sc, vmax);
            else       ed_t16(l, ED_MX+56, y+5, vs, false, vmax);
            if(act){
                int cx = ED_MX+56 + text16w(vs) - sc + 2;
                if(cx > ED_LW-22) cx = ED_LW-22;
                if(cx < ED_MX+56) cx = ED_MX+56;
                ed_caret(l, cx, y+5);
            }
        } else {
            ed_t16(l, ED_MX+56, y+5, vs, false, vmax);
            if(act){ int cw = text16w(vs); if(cw > vmax-10) cw = vmax-10; ed_caret(l, ED_MX+56+cw+2, y+5); }
        }
        /* 不画「改」: 整行就是可点的, 点中即进入编辑 */
        ed_dotline(l, ED_MX, ED_LW-2, y+ED_F_H-1);
    }

    /* 两栏之间的竖分隔线 */
    /* 两栏之间不画竖线: 靠留白分栏 (linit 2026-10-21) */

    /* ---- 右栏 (每条账号 = 账号行 + 密码行) ---- */
    for(int c=0;c<f->nc;c++){
        int y = base + c*ED_CR;
        if(y + ED_CR <= ED_VIEW_TOP || y >= vbot) continue;
        bool a_user = (s_edit_focus && s_field == 3+2*c);
        bool a_pass = (s_edit_focus && s_field == 4+2*c);
        const char *uv = a_user ? s_buf : f->cr[c].user;
        char mask[24]; int pl=(int)strlen(f->cr[c].pass);
        int ml = pl>10?10:pl; for(int k=0;k<ml;k++) mask[k]='*'; mask[ml]=0;
        /* 账号行 */
        if(y >= ED_VIEW_TOP && y+ED_F_H <= vbot){
            ed_t16(l, ED_RX,      y+5, "\xe8\xb4\xa6\xe5\x8f\xb7\xef\xbc\x9a", false, 54);   /* 账号： */
            const char *us = (uv&&uv[0])?uv:"-";
            int umax = S_W-32-(ED_RX+56);
            ed_t16(l, ED_RX+56, y+5, us, false, umax);
            if(a_user){ int cw = text16w(us); if(cw > umax-10) cw = umax-10; ed_caret(l, ED_RX+56+cw+2, y+5); }
            ed_dotline(l, ED_RX, S_W-ED_MX, y+ED_F_H-1);
        }
        /* 密码行 */
        if(y+ED_F_H >= ED_VIEW_TOP && y+ED_CR <= vbot){
            int y2 = y + ED_F_H;
            ed_t16(l, ED_RX,    y2+5, "\xe5\xaf\x86\xe7\xa0\x81\xef\xbc\x9a", false, 54);  /* 密码： */
            const char *ps = a_pass ? s_buf : (ml?mask:"-");
            int pmax = S_W-32-(ED_RX+56);
            ed_t16(l, ED_RX+56, y2+5, ps, false, pmax);
            if(a_pass){ int cw = text16w(ps); if(cw > pmax-10) cw = pmax-10; ed_caret(l, ED_RX+56+cw+2, y2+5); }
            ed_dotline(l, ED_RX, S_W-ED_MX, y2+ED_F_H-1);
        }
    }

    /* ---- ＋ 添加账号 (右栏, 紧跟最后一条账号) ---- */
    int ay = base + ed_add_cy(f);
    if(ay >= ED_VIEW_TOP && ay + 20 <= vbot)
    {
        /* 当成一排内容: 在右栏内水平居中, 并略微上提 (linit 2026-10-21) */
        const char *addtxt = "\xef\xbc\x8b \xe6\xb7\xbb\xe5\x8a\xa0\xe8\xb4\xa6\xe5\x8f\xb7";
        int tw = text16w(addtxt);
        int colw = S_W - ED_MX - ED_RX;
        int ax = ED_RX + (colw - tw)/2; if(ax < ED_RX) ax = ED_RX;
        ed_t16(l, ax, ay - 2, addtxt, false, colw);
    }

    /* ---- 底部按钮行: 只在没有键盘时显示 (左「删除收藏」/ 右「保存」) ---- */
    if(!s_edit_focus && !s_show_cats){
        static const char *DELT = "\xe5\x88\xa0\xe9\x99\xa4\xe6\x94\xb6\xe8\x97\x8f";   /* 删除收藏 */
        static const char *SAVT = "\xe4\xbf\x9d\xe5\xad\x98";                           /* 保存 */
        ed_dotline(l, ED_MX, S_W-ED_MX, ED_BTN_Y);
        ed_t16(l, ED_MX, ED_BTN_Y+7, DELT, false, 120);
        int sw = text16w(SAVT);
        ed_t16(l, S_W-ED_MX-sw, ED_BTN_Y+7, SAVT, false, 80);
    }

    /* ---- 键盘: 只有正在编辑字段时才出现 (kbd_std 5 排全高) ---- */
    if(s_edit_focus && !s_show_cats){
        int press=-1,tx,ty;
        if(input_touch_now(&tx,&ty) && ty>=ED_KBD_Y) press=kbd_hit(&kbd_std, ED_KBD_Y, tx, ty);
        kbd_draw(l, ED_KBD_Y, &kbd_std, press);
    }
}

/* 图标选择: 底部两排 (占键盘位置), 列优先排列, 左右拖动 */
static void icon_picker_render(st7305_handle_t*l){
    int cols = (MAIN_ICON_COUNT + 1) / 2;
    int maxs = cols*ICON_CELL_W - S_W; if(maxs < 0) maxs = 0;
    if(s_icon_scroll > maxs) s_icon_scroll = maxs;
    if(s_icon_scroll < 0)    s_icon_scroll = 0;

    fill_rect(l, 0, ICON_PT, S_W-1, S_H-1, ST7305_COLOR_WHITE);   /* 只清底部面板 */
    ed_dotline(l, 0, S_W-1, ICON_PT);                             /* 面板上缘: 虚线 (不用生硬实线) */

    for(int col=0; col<cols; col++){
        for(int row=0; row<2; row++){
            int idx = col*2 + row;
            if(idx >= MAIN_ICON_COUNT) break;
            int cx = col*ICON_CELL_W + ICON_CELL_W/2 - s_icon_scroll;
            int cy = ICON_PT + 8 + row*ICON_CELL_H + 28;
            if(cx < -20 || cx > S_W+20) continue;
            draw_icon_bitmap_stretched(l, cx, cy, 40, 40, idx);
        }
    }
}

static void render(ui_ctx_t*ctx){ st7305_handle_t*l=ctx->lcd; if(!l)return;
    s_dbg_lcd = l;
    if(!s_authed){
        if(s_drawer>=0){ drawer_render(l); if(s_show_cats) icon_picker_render(l); }
        else main_render(l);                           /* 背景照常画 */
        applock_render(l);                             /* PIN 弹窗盖在上面 */
        return;
    }
    if(s_drawer>=0){                       /* 编辑页 + (可选)底部图标面板 */
        drawer_render(l);
        if(s_show_cats) icon_picker_render(l);
        return;
    }
    main_render(l);
}

/* ===== 编辑抽屉逻辑 ===== */
static void edit_load(void){
    fav_t*f=&s_fav[s_drawer]; if(!f)return;
    const char *src = "";
    if(s_field==0)      src=f->name;
    else if(s_field==1) src=f->url;      /* 网址: 原来根本没有这个字段 */
    else if(s_field==2) src=f->desc;
    else { int c=(s_field-3)/2; int is_pass=(s_field-3)%2;
           if(c<f->nc) src = is_pass ? f->cr[c].pass : f->cr[c].user; }
    snprintf(s_buf,sizeof(s_buf),"%s",src?src:"");
    s_blen=(int)strlen(s_buf);
}
static void apply_field(void){
    fav_t*f=&s_fav[s_drawer]; if(!f)return;
    /* ★ 2026-10-20: 字段早已扩到 name[24]/desc[32], 这里还写死 14/20 ⇒
     *   设备上改名只能存 4 个汉字、介绍只能 6 个, 必须跟着 sizeof 走。 */
    if(s_field==0)      fav_u8cpy(f->name, sizeof(f->name), s_buf);
    else if(s_field==1) fav_u8cpy(f->url,  sizeof(f->url),  s_buf);
    else if(s_field==2) fav_u8cpy(f->desc, sizeof(f->desc), s_buf);
    else { int c=(s_field-3)/2; int is_pass=(s_field-3)%2;
           if(c<f->nc){ if(is_pass) str_trunc(f->cr[c].pass,sizeof(f->cr[c].pass),s_buf);
                        else         str_trunc(f->cr[c].user,sizeof(f->cr[c].user),s_buf); } }
    fav_save();
}

static void add_site(ui_ctx_t*ctx){
    if(s_n>=FAV_MAX){ os_dialog_toast(ctx,"\xe5\xb7\xb2\xe6\xbb\xa1"); return; }
    fav_t*f=&s_fav[s_n]; memset(f,0,sizeof(fav_t)); f->cat=(uint8_t)s_cat; f->icon=0; f->nc=0; s_n++; fav_save();
    s_drawer=s_n-1; s_edit_focus=false; s_field=0; redraw(ctx);
}
static void add_cred(ui_ctx_t*ctx,int gi){
    if(gi<0||gi>=s_n) { return; }
    fav_t*f=&s_fav[gi];
    if(f->nc>=CR_MAX){ os_dialog_toast(ctx,"\xe5\xb7\xb2\xe6\xbb\xa1"); return; }
    f->cr[f->nc].user[0]=0; f->cr[f->nc].pass[0]=0; f->nc++; fav_save(); redraw(ctx);
}
static void del_cred_at(ui_ctx_t*ctx,int gi,int ci){
    if(gi<0||gi>=s_n) return;
    fav_t*f=&s_fav[gi];
    if(ci<0||ci>=f->nc) return;
    for(int k=ci;k<f->nc-1;k++) f->cr[k]=f->cr[k+1];
    f->nc--;
    memset(&f->cr[f->nc],0,sizeof(cred_t));
    fav_save(); redraw(ctx);
}

/* ===== 触摸 ===== */
/* 设置项动作: 0=修改PIN 1=导出备份 2=导入备份 3=热点配置 4=IP配置 */
/* 选中某个备份文件后: 直接导入 + 刷新 */
static void fav_pick_import_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    s_fav_import_dlg = false;
    if (result < 0 || result >= FAV_BAK_MAX || !s_bak_path[result][0]) return;
    int r = fav_import_file(s_bak_path[result]);
    fav_load();
    s_cat = 0; s_scroll = 0; s_open = -1; s_drawer = -1;
    s_fcol_dirty = true; s_need_clear = true;
    if (r == 0)        os_dialog_toast(ctx, "已恢复备份");
    else if (r == -1)  os_dialog_toast(ctx, "文件不存在");
    else               os_dialog_toast(ctx, "备份格式不支持");
    ctx->needs_redraw = true;
}

/* ===== 导入备份列表: 长按某项 → 确认删除 =====
 * 与存储页"长按备份项→确认删除"同一套语义 (LONG_PRESS 由 input 层在按住 2s 时投递,
 * 弹窗 on_key 拿到的是已经命中的行)。删除后置脏, 列表层自己 on_poll 里重扫刷新生效。 */
static char s_bak_pend[64];
static bool s_bak_dirty = false;

static void fav_bak_del_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    if (result != 0) return;                       /* 取消 → 不动 */
    bool ok = (remove(s_bak_pend) == 0);
    os_dialog_toast(ctx, ok ? "\xe5\xb7\xb2\xe5\x88\xa0\xe9\x99\xa4"                    /* 已删除 */
                            : "\xe5\x88\xa0\xe9\x99\xa4\xe5\xa4\xb1\xe8\xb4\xa5");       /* 删除失败 */
    if (ok) s_bak_dirty = true;
}

static bool fav_bak_list_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud)
{
    (void)ud;
    if (a != OS_ACTION_LONG_PRESS) return false;   /* 其余 (点选=导入/BACK=返回) 走默认 */
    if (!d || d->sel < 0 || d->sel >= d->count) return false;      /* 底部返回行不删 */
    if (d->sel >= FAV_BAK_MAX || !s_bak_path[d->sel][0]) return false;
    snprintf(s_bak_pend, sizeof(s_bak_pend), "%s", s_bak_path[d->sel]);
    char msg[40];
    snprintf(msg, sizeof(msg), "\xe5\x88\xa0\xe9\x99\xa4 %s?", s_bak_label[d->sel]);   /* 删除 xxx? */
    os_dialog_confirm_ex(ctx, msg, 0, -1, fav_bak_del_cb, NULL);
    return true;
}

/* 删除生效后: 本层就是那个列表, 原地重扫改写 items/count (不重开弹窗, 不弹页) */
static void fav_bak_list_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud)
{
    (void)ud;
    if (!s_bak_dirty) return;
    s_bak_dirty = false;
    int n = fav_bak_scan();
    if (n <= 0) {                                  /* 全删光 → 收掉列表 */
        s_fav_import_dlg = false;
        os_dialog_pop(ctx);
        return;
    }
    int cap = (int)(sizeof(d->items) / sizeof(d->items[0]));
    d->count = (n < cap) ? n : cap;
    for (int i = 0; i < d->count; i++)
        snprintf(d->items[i], sizeof(d->items[0]), "%.*s",
                 (int)sizeof(d->items[0]) - 1, s_bak_label[i]);
    if (d->sel >= d->count) d->sel = d->count - 1;
    ctx->needs_redraw = true;
}

static void fav_setting_run(ui_ctx_t*ctx,int idx){
    switch (idx) {
    case 2:   /* 修改PIN: 统一在密钥管理器里改 */
        os_dialog_toast(ctx, "\xe8\xaf\xb7\xe5\x9c\xa8\xe5\xaf\x86\xe9\x92\xa5\xe7\xae\xa1\xe7\x90\x86\xe5\x99\xa8\xe9\x87\x8c\xe4\xbf\xae\xe6\x94\xb9 PIN");
        break;
    case 0: { /* 导出备份: 放进 /sdcard/backup/, 文件名自动编号 fav_NN.bak */
        char path[64];
        fav_bak_new_path(path, sizeof(path));
        int r = fav_export_file(path);
        if (r == 0) {
            const char *nm = strrchr(path, '/');
            char msg[96];   /* "已备份到 " + 最长 64 字节路径 ⇒ 96 够, 不再截断 */
            snprintf(msg, sizeof(msg), "\xe5\xb7\xb2\xe5\xa4\x87\xe4\xbb\xbd\xe5\x88\xb0 %s", nm ? nm + 1 : path); /* 已备份到 xxx */
            os_dialog_toast(ctx, msg);
        } else {
            os_dialog_toast(ctx, "\xe5\xaf\xbc\xe5\x87\xba\xe5\xa4\xb1\xe8\xb4\xa5");  /* 导出失败 */
        }
        break;
    }
    case 1: { /* 导入备份: 列出 /sdcard/backup/ 里的备份让用户选 */
        if (s_fav_import_dlg) break;
        int n = fav_bak_scan();
        if (n <= 0) { os_dialog_toast(ctx, "\xe6\xb2\xa1\xe6\x9c\x89\xe5\xa4\x87\xe4\xbb\xbd\xe6\x96\x87\xe4\xbb\xb6"); break; } /* 没有备份文件 */
        s_fav_import_dlg = true;
        os_dlg_cfg_t cfg = {0};
        cfg.type  = OS_DLG_LIST;
        cfg.title = "\xe9\x80\x89\xe6\x8b\xa9\xe5\xa4\x87\xe4\xbb\xbd";   /* 选择备份 */
        cfg.items = s_bak_items;
        cfg.count = n;
        cfg.sel   = 0;
        cfg.cb    = fav_pick_import_cb;
        s_bak_dirty = false;
        os_dlg_open(ctx, &cfg);
        /* 给这层列表挂上"长按删除 + 删后刷新" (统一入口 os_dlg_open 不暴露回调, 用 top_mut 补挂) */
        os_dlg_stack_t *dl = os_dialog_top_mut(ctx);
        if (dl) { dl->on_key = fav_bak_list_key; dl->on_poll = fav_bak_list_poll; }
        break;
    }
    case 3:   /* 热点配置: toggle AP — 开/关都有 toast 提示 */
        if (s_web_on || s_web_phase == 1) {
            fav_web_stop(ctx);
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad");   /* 已关闭 */
        } else {
            fav_web_start_ap(ctx);
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\xbc\x80\xe5\x90\xaf\xe7\x83\xad\xe7\x82\xb9 LinTOS-FAV"); /* 已开启热点 LinTOS-FAV */
        }
        break;
    case 4:   /* 局域网配置: 后台已预热, 这里只负责"开放/关闭"网页访问 */
        if (s_web_allow) {                    /* 已开放 → 关闭 */
            fav_web_stop(ctx);
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad");   /* 已关闭 */
        } else if (s_web_on) {                /* 服务已在后台跑 → 立即开放 (不用等启动) */
            s_web_allow = true;
            s_lan_toast_pending = true;
            break;
        } else if (s_web_phase == 1) {        /* 正在启动 → 起来后自动开放 */
            s_web_open_req = true;
            break;
        } else {
            if (wifi_manager_is_connected()) {
                fav_web_start_lan(ctx);
                /* ★★ 2026-10-20 修"要点两次才变 IP": 原来第一次只起服务, `s_web_allow` 还是 false,
                 *   菜单项判定要 `s_web_on && s_web_allow` ⇒ 第一次点完仍显示"局域网配置",
                 *   用户必须再点一次才 allow=true。这里**点一下就立刻开放**, 服务就绪后菜单自动变 IP。 */
                s_web_allow     = true;
                s_web_open_req  = true;
                s_lan_toast_pending = true;
                ctx->needs_redraw = true;
                break;
            }
            /* 未连接: 弹系统"连接WiFi"(扫描+密码键盘); 已保存配置自动重连.
             * ★ 2026-10-02 修: 此处**不要**自己 os_hw_request(OS_HW_WIFI) ——
             *   settings_wifi_connect_open() 会按需 request 并记下"是我们开的"(私有标志),
             *   用户取消时才会 release。这里先 request 会让它以为 WiFi 早已开着(active=true)
             *   → 不记标志 → 取消后**再也没人关**, 射频常开(违背"按需启停")。 */
            s_lan_pending = true;
            s_lan_t0 = (uint32_t)(esp_timer_get_time() / 1000);
            settings_wifi_connect_open(ctx);
        }
        break;
    default: break;
    }
    redraw(ctx);
}
static void main_touch(ui_ctx_t*ctx,int x,int y){
    if(y<=HDR_H) return;   /* 状态栏区域, 不处理 */
    if(x<LIST_X){
        /* 左栏 os_pane 模板: 行高32, 滚动 folder_scroll; 0=设置 1..=分类 */
        int idx=s_pane.folder_scroll+(y-HDR_H)/32;
        if(idx<0) return;
        if(idx<s_cat_n+1){ pane_set_sel(idx); s_drawer=-1; s_open=-1; s_scroll=0; s_fcol_dirty=true; fav_hid_sync(); redraw(ctx); }
        return;
    }
    if(s_cat<0){   /* 设置右栏 */
        int idx=(y-(HDR_H+6))/32;
        if(idx>=0&&idx<5){ s_sel=idx; fav_setting_run(ctx,idx); }
        return;
    }
    int nF=fav_max(), ry=0;
    for(int f=0;f<nF;f++){
        int gi=-1; fav_gi(f,&gi); if(gi<0) break;
        if(gi==s_open && s_fav[gi].nc>0){
            /* ★ 展开态也要能点这一行收起 (之前只判了账号区, 点行头没反应) */
            int hsy=HDR_H+ry-s_scroll;
            if(y>=hsy && y<hsy+52){
                if(x>=LIST_X+2+FC_W-32){ do_input_url(ctx,gi); return; }
                s_open=-1; s_fcol_dirty=true; s_need_clear=true; redraw(ctx); return;
            }
            int cy=ry+52;
            for(int c=0;c<s_fav[gi].nc;c++){
                int sy=HDR_H+cy-s_scroll;
                if(y>=sy && y<sy+28){                              /* 账号行 */
                    if(x>=LIST_X+2+FC_W-56) do_input_part(ctx,gi,c,0);
                    return;
                }
                if(y>=sy+28 && y<sy+FAV_CR_H){                     /* 密码行 */
                    if(x>=LIST_X+2+FC_W-34) do_input_part(ctx,gi,c,1);
                    return;
                }
                cy+=FAV_CR_H;
            }
            ry=cy;
        } else {
            int sy=HDR_H+ry-s_scroll;
            if(y>=sy && y<sy+52){
                if(x>=LIST_X+2+FC_W-32){ do_input_url(ctx,gi); return; }   /* 行尾 = 推网址 */
                if(s_fav[gi].nc>0){            /* 有账号才展开抽屉 */
                    s_open=(s_open==gi)?-1:gi;
                    s_fcol_dirty=true; s_need_clear=true;
                    redraw(ctx);
                }
                return;
            }
            ry+=52;
        }
    }
    int ey=HDR_H+ry-s_scroll;
    /* ★ 列表末尾的「＋ 添加收藏」行 (有无条目都有) */
    if(y>=ey && y<ey+52){ add_site(ctx); return; }
}
/* 激活某个字段: 先保存上一个, 再载入新的 (s_buf 常驻, 编辑实时显示在该行) */
/* 把某个字段滚进可视区 (键盘展开后可视区只剩 105px, 选中行必须在里面) */
static void ed_ensure_visible(int field){
    if(s_drawer < 0) return;
    fav_t *f = &s_fav[s_drawer];
    int cy;
    if(field < 3){                                    /* 左栏: 名称/介绍/网址 的显示行 */
        int r = (field==0) ? 0 : (field==2 ? 1 : 2);
        cy = ED_ICON_H + r * ED_F_H;
    } else {                                          /* 右栏: 第 c 条的账号行/密码行 */
        int c = (field-3)/2;
        cy = c*ED_CR + (((field-3)%2) ? ED_F_H : 0);
    }
    int vh = ED_VIEW_BOT - ED_VIEW_TOP;
    int mx = ed_maxs(f);
    if(cy < (int)s_ed_scroll)              s_ed_scroll = (float)cy;
    if(cy + 24 > (int)s_ed_scroll + vh)    s_ed_scroll = (float)(cy + 24 - vh);
    if(s_ed_scroll < 0.0f)       s_ed_scroll = 0.0f;
    if(s_ed_scroll > (float)mx)  s_ed_scroll = (float)mx;
}

/* 长按 2 秒删账号: 先弹确认. 列表里不画删除按钮 (linit 2026-10-21 规范) */
static void ed_del_cred_cb(ui_ctx_t *ctx, int result, void *ud){
    if(result != 0) return;                    /* 0=确定 */
    int c = (int)(intptr_t)ud;
    del_cred_at(ctx, s_drawer, c);
}

/* ---- 「保存并返回」: 右下角按钮 / CONFIRM 键, 都先弹确认 ---- */
static void ed_save_back_cb(ui_ctx_t *ctx, int result, void *ud){
    (void)ud;
    if(result != 0) return;                    /* 0=确定 */
    if(s_drawer < 0 || s_drawer >= s_n) return;
    if(s_edit_focus){ apply_field(); s_edit_focus = false; }
    if(s_fav[s_drawer].name[0]==0 && s_fav[s_drawer].nc==0){   /* 空白条目不留 */
        for(int i=s_drawer;i<s_n-1;i++) s_fav[i]=s_fav[i+1];
        memset(&s_fav[s_n-1],0,sizeof(fav_t));
        s_n--;
    }
    s_drawer=-1; s_ed_scroll=0; s_url_scroll=0;
    s_ed_snap_ok=false; s_ed_track=-1;
    s_fcol_dirty=true; s_need_clear=true;
    fav_save(); redraw(ctx);
}

/* ---- 「不保存返回」: 用进入时的快照还原, 不落盘 ---- */
static void ed_discard_back_cb(ui_ctx_t *ctx, int result, void *ud){
    (void)ud;
    if(result != 0) return;
    if(s_drawer < 0 || s_drawer >= s_n) return;
    if(s_ed_snap_new){                          /* 原来是新建的空条目 ⇒ 整条撤掉 */
        for(int i=s_drawer;i<s_n-1;i++) s_fav[i]=s_fav[i+1];
        memset(&s_fav[s_n-1],0,sizeof(fav_t));
        s_n--;
    } else if(s_ed_snap_ok){
        s_fav[s_drawer] = s_ed_snap;             /* 内存还原, 不用写 flash */
    }
    s_edit_focus=false; s_drawer=-1; s_ed_scroll=0; s_url_scroll=0;
    s_ed_snap_ok=false; s_ed_track=-1;
    s_fcol_dirty=true; s_need_clear=true;
    redraw(ctx);
}

/* ---- 「删除这个收藏」: 左下角按钮 ---- */
static void ed_del_site_cb(ui_ctx_t *ctx, int result, void *ud){
    (void)ud;
    if(result != 0) return;
    if(s_drawer < 0 || s_drawer >= s_n) return;
    for(int i=s_drawer;i<s_n-1;i++) s_fav[i]=s_fav[i+1];
    memset(&s_fav[s_n-1],0,sizeof(fav_t));
    s_n--;
    s_drawer=-1; s_edit_focus=false; s_ed_scroll=0; s_url_scroll=0;
    s_ed_snap_ok=false; s_ed_track=-1; s_open=-1;
    int m = fav_max();
    if(s_sel >= m) s_sel = m > 0 ? m-1 : 0;
    s_fcol_dirty=true; s_need_clear=true;
    fav_save(); redraw(ctx);
}

static void ed_activate(int field){
    if(s_edit_focus && s_field != field) apply_field();
    s_field = field;
    s_edit_focus = true;                    /* 先开键盘: 可视区高度由它决定 */
    edit_load();
    if(field == 1) ed_url_follow();          /* 网址: 默认停在最右侧 */
    else           s_url_scroll = 0;
    ed_ensure_visible(field);
}

static void drawer_touch(ui_ctx_t*ctx,int x,int y){
    fav_t*f=&s_fav[s_drawer]; if(!f) return;
    if(s_ed_swallow_tap){ s_ed_swallow_tap = false; return; }   /* 刚才是滚动, 不当作点击 */

    /* 状态栏那一条不归页面管 */
    if(y < ED_SB) return;

    /* ---- 键盘 (只在编辑中显示) ---- */
    if(s_edit_focus && y >= ED_KBD_Y){
        int idx = kbd_hit(&kbd_std, ED_KBD_Y, x, y);
        if(idx < 0) return;
        kbd_t k = kbd_std;
        int act = kbd_press(&k, idx);
        if(act == KBD_ACT_CHAR){ char c=kbd_char(&k,idx); if(c && s_blen<63){ s_buf[s_blen++]=c; s_buf[s_blen]=0; } }
        else if(act == KBD_ACT_BKSP){ if(s_blen>0) s_buf[--s_blen]=0; }
        else if(act == KBD_ACT_SPACE){ if(s_blen<63){ s_buf[s_blen++]=' '; s_buf[s_blen]=0; } }
        else if(act == KBD_ACT_ENTER){ apply_field(); s_edit_focus=false; }   /* 回车 = 存并收键盘 */
        else if(act == KBD_ACT_ESC){ edit_load(); s_edit_focus=false; }       /* Esc = 撤销本次 */
        if(s_edit_focus && s_field == 1) ed_url_follow();                     /* 网址: 光标跟随 */
        redraw(ctx);
        return;
    }

    /* ---- 底部按钮行 (无键盘时): 左「删除收藏」/ 右「保存」 ---- */
    if(!s_edit_focus && y >= ED_BTN_Y){
        if(x < S_W/2){
            os_dialog_confirm_ex(ctx, "\xe5\x88\xa0\xe9\x99\xa4\xe8\xbf\x99\xe4\xb8\xaa\xe6\x94\xb6\xe8\x97\x8f?",
                                 0, -1, ed_del_site_cb, NULL);          /* 删除这个收藏? */
            redraw(ctx);
        } else {
            os_dialog_confirm_ex(ctx, "\xe4\xbf\x9d\xe5\xad\x98\xe5\xb9\xb6\xe8\xbf\x94\xe5\x9b\x9e?",
                                 0, -1, ed_save_back_cb, NULL);         /* 保存并返回? */
            redraw(ctx);
        }
        return;
    }

    /* ---- 内容区 (滚动坐标) ---- */
    if(y < ED_VIEW_TOP) return;
    int cy = (y - ED_VIEW_TOP) + (int)s_ed_scroll;

    if(x < ED_LW){                                        /* 左栏: 图标 / 名称 / 介绍 / 网址 */
        if(cy < ED_ICON_H){ s_show_cats = true; redraw(ctx); return; }   /* 点图标换图标 */
        if(cy < ED_ICON_H + 3*ED_F_H){
            int r = (cy - ED_ICON_H) / ED_F_H;
            if(r < 0) r = 0;
            if(r > 2) r = 2;
            ed_activate(L2F[r]); redraw(ctx); return;
        }
    } else {                                              /* 右栏: 账号 / 密码 */
        if(cy < f->nc*ED_CR){
            int c = cy / ED_CR;
            bool pass_line = (cy % ED_CR) >= ED_F_H;
            if(!pass_line && x >= S_W-30 && x <= S_W-2){ del_cred_at(ctx, s_drawer, c); return; }  /* 删 */
            ed_activate(3 + 2*c + (pass_line ? 1 : 0)); redraw(ctx); return;
        }
        int ay = ed_add_cy(f);
        if(cy >= ay && cy <= ay+20){ add_cred(ctx, s_drawer); redraw(ctx); return; }
    }

    /* 点了空白处: 收起键盘(并保存) */
    if(s_edit_focus){ apply_field(); s_edit_focus=false; redraw(ctx); }
}

static void icon_touch(ui_ctx_t*ctx,int x,int y){
    if(y < ICON_PT){ s_show_cats = false; redraw(ctx); return; }   /* 点面板外 = 收起 */
    int cols = (MAIN_ICON_COUNT + 1) / 2;
    int col = (x + s_icon_scroll) / ICON_CELL_W;
    int row = (y - ICON_PT - 8) / ICON_CELL_H;
    if(row < 0) row = 0;
    if(row > 1) row = 1;
    int idx = col*2 + row;
    if(col < 0 || col >= cols || idx < 0 || idx >= MAIN_ICON_COUNT) return;
    s_fav[s_drawer].icon = (uint8_t)idx;
    fav_save();
    s_show_cats = false;
    redraw(ctx);
}

static bool fav_touch(ui_ctx_t*ctx,int x,int y){
    if(!s_authed){
        applock_touch(ctx,x,y); if(!applock_active()){ s_authed=true; fav_load_enc(); }
        redraw(ctx); return true;
    }
    if(s_drawer>=0){ if(s_show_cats) icon_touch(ctx,x,y); else drawer_touch(ctx,x,y); return true; }
    main_touch(ctx,x,y); return true;
}

/* ===== 按键 ===== */
static void fav_action(ui_ctx_t*ctx,os_action_t a){
    if(!s_authed){
        /* ★ 通用应用锁: PIN 弹窗交给 applock 模块 (BACK=取消回上一级) */
        applock_action(ctx,a);
        if(!applock_active()){ s_authed=true; fav_load_enc(); }
        redraw(ctx); return;
    }
    if(s_drawer>=0){
        /* 键盘展开时: BACK/CONFIRM 只负责"收键盘" (不退出页面) */
        if(s_edit_focus){
            if(a==OS_ACTION_BACK || a==OS_ACTION_CONFIRM){
                apply_field(); s_edit_focus=false; s_url_scroll=0; redraw(ctx);
            }
            return;
        }
        /* 键盘收起后: 两个键都要弹确认 (linit 2026-10-21) */
        if(a==OS_ACTION_BACK)
            os_dialog_confirm_ex(ctx, "\xe4\xb8\x8d\xe4\xbf\x9d\xe5\xad\x98\xe8\xbf\x94\xe5\x9b\x9e?",
                                 0, -1, ed_discard_back_cb, NULL);      /* 不保存返回? */
        else if(a==OS_ACTION_CONFIRM)
            os_dialog_confirm_ex(ctx, "\xe4\xbf\x9d\xe5\xad\x98\xe5\xb9\xb6\xe8\xbf\x94\xe5\x9b\x9e?",
                                 0, -1, ed_save_back_cb, NULL);         /* 保存并返回? */
        return;
    }
    if(a==OS_ACTION_LEFT){ s_cat=(s_cat>-1)?s_cat-1:s_cat_n-1; fav_idle_hold_sync(); s_drawer=-1; s_open=-1; s_scroll=0; s_need_clear=true; s_fcol_dirty=true; fav_hid_sync(); redraw(ctx); }
    else if(a==OS_ACTION_RIGHT){ s_cat=(s_cat<s_cat_n-1)?s_cat+1:-1; fav_idle_hold_sync(); s_drawer=-1; s_open=-1; s_scroll=0; s_need_clear=true; s_fcol_dirty=true; fav_hid_sync(); redraw(ctx); }
    else if(a==OS_ACTION_DOWN&&(s_cat<0?s_sel<4:s_sel<fav_max()-1)){ s_sel++; redraw(ctx); }
    else if(a==OS_ACTION_UP&&s_sel>0){ s_sel--; redraw(ctx); }
    else if(a==OS_ACTION_CONFIRM){
        if(s_cat<0){ fav_setting_run(ctx,s_sel); }
        else { int gi=-1; fav_gi(s_sel,&gi); if(gi>=0){ s_drawer=gi; s_show_cats=false; redraw(ctx); } }
    }
    /* ★ 2026-10-25 linit: 收藏夹最外层按返回 → 先弹"退出程序?"确认, 确认才退 */
    else if(a==OS_ACTION_BACK) os_request_exit_confirm(ctx);
}

/* ===== poll 拖动 ===== */
static void fav_poll(ui_ctx_t*ctx){
    if(s_dbg_redraw){ s_dbg_redraw = false; ctx->needs_redraw = true; }

    /* ★★★ 2026-10-24 统一滚动: 右栏列表由 ui_scroll 推进（惯性×0.86 + 回弹 + 行吸附）。
     *   s_scroll 退化为渲染镜像；切分类时外部直接把 s_scroll 置 0，这里检测到差异后同步回来。 */
    {
        int vh = S_H - HDR_H - 2;
        int max = (fav_max() + 1) * 52 - vh;
        if (max < 0) max = 0;
        int content = max + vh;
        if (!s_fav_sc_rdy) { ui_scroll_config(&s_fav_sc, UI_SCROLL_LIST, 0); s_fav_sc_rdy = true; }
        if (s_fav_sc_cap != content) { ui_scroll_init(&s_fav_sc, 52, vh, content); s_fav_sc_cap = content; }
        if (!ui_scroll_busy(&s_fav_sc) && s_scroll != ui_scroll_offset(&s_fav_sc)) {
            int o = s_scroll; if (o < 0) o = 0; if (o > max) o = max;
            s_fav_sc.off = o; s_fav_sc.sub = 0; s_fav_sc.fling = 0.0f;
        }
        if (ui_scroll_poll(&s_fav_sc)) ctx->needs_redraw = true;
        int o = ui_scroll_offset(&s_fav_sc);
        if (o < 0)   o = 0;
        if (o > max) o = max;
        if (o != s_scroll) { s_scroll = o; ctx->needs_redraw = true; }
    }

    /* 进入编辑页的那一帧拍快照 —— 「不保存返回」用它还原 (不写 flash) */
    if(s_drawer >= 0 && !s_show_cats && s_drawer != s_ed_track && s_drawer < s_n){
        s_ed_snap = s_fav[s_drawer];
        s_ed_snap_ok  = true;
        s_ed_snap_new = (s_ed_snap.name[0]==0 && s_ed_snap.nc==0 && s_ed_snap.url[0]==0);
        s_ed_track = s_drawer;
    }
    if(s_drawer < 0) s_ed_track = -1;

    /* ★ 编辑页: 内容区 1:1 跟手滚动 (键盘常驻占掉底部, 内容区只有 131px 必须能滚) */
    if(s_drawer >= 0 && !s_show_cats){
        int tx, ty; bool down = input_get_touch_pos(&tx, &ty);
        if(down){
            if(!s_ed_anchored && ty > ED_VIEW_TOP && ty < ED_VIEW_BOT){
                s_ed_anchored = true;
                s_ed_drag_y0 = ty; s_ed_drag_x0 = tx;
                s_ed_scroll0 = s_ed_scroll; s_url_sc0 = s_url_scroll;
                /* 起点落在"网址"行 ⇒ 允许横向拖动这一行 */
                int cy0 = (ty - ED_VIEW_TOP) + (int)s_ed_scroll;
                s_url_mode = (cy0 >= ED_ICON_H + 2*ED_F_H && cy0 < ED_ICON_H + 3*ED_F_H);
            }
            if(s_ed_anchored){
                int dx = tx - s_ed_drag_x0;
                int dy = ty - s_ed_drag_y0;
                int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                if(!s_ed_drag && !s_ed_drag_moved && (adx > 6 || ady > 6)){
                    s_ed_drag = true; s_ed_drag_moved = true;
                    /* 横向为主且起点在网址行 ⇒ 拖网址, 不滚页面 */
                    if(s_url_mode && adx > ady){ s_url_drag = true; s_url_mode = true; }
                    else                        { s_url_drag = false; s_url_mode = false; }
                }
                if(s_ed_drag && s_url_drag){
                    int avail = (ED_LW - 24) - (ED_MX + 56);
                    int txtw  = text16w(s_fav[s_drawer].url);
                    int maxs  = txtw > avail ? txtw - avail + 4 : 0;
                    int ns = s_url_sc0 - dx;
                    if(ns < 0) ns = 0;
                    if(ns > maxs) ns = maxs;
                    if(ns != s_url_scroll){ s_url_scroll = ns; ctx->needs_redraw = true; }
                } else if(s_ed_drag){
                    float ns = s_ed_scroll0 - (float)dy;      /* 手指上推 ⇒ 内容上移 */
                    int mx = ed_maxs(&s_fav[s_drawer]);
                    if(ns < 0.0f) ns = 0.0f;
                    if(ns > (float)mx) ns = (float)mx;
                    if((int)ns != (int)s_ed_scroll){ s_ed_scroll = ns; ctx->needs_redraw = true; }
                }
            }
        } else {
            if(s_ed_drag_moved) s_ed_swallow_tap = true;   /* 这一次是滚动, 别当点击 */
            s_ed_anchored = false; s_ed_drag = false; s_ed_drag_moved = false;
            s_url_drag = false; s_url_mode = false;
        }
        if(s_ed_drag) ctx->needs_redraw = true;
        if(s_edit_focus && !s_show_cats) ctx->needs_redraw = true;   /* 光标闪烁: 每帧重绘 */
    }

    /* 图标面板: 左右拖动 (1:1) */
    if(s_drawer >= 0 && s_show_cats){
        int tx, ty; bool down = input_get_touch_pos(&tx, &ty);
        if(down && ty >= ICON_PT){
            if(!s_icon_drag){ s_icon_drag = true; s_icon_dx0 = tx; s_icon_sc0 = s_icon_scroll; }
            int cols = (MAIN_ICON_COUNT + 1) / 2;
            int maxs = cols*ICON_CELL_W - S_W; if(maxs < 0) maxs = 0;
            int ns = s_icon_sc0 - (tx - s_icon_dx0);
            if(ns < 0) ns = 0;
            if(ns > maxs) ns = maxs;
            if(ns != s_icon_scroll){ s_icon_scroll = ns; ctx->needs_redraw = true; }
        } else {
            s_icon_drag = false;
        }

        /* ★ 长按 2s 删账号 (右栏账号/密码行上), 长按即弹确认, 不需要松手 */
        static bool s_lp_fired = false;
        uint32_t hold = input_touch_hold_ms();
        if(hold == 0) s_lp_fired = false;
        else if(!s_lp_fired && hold >= 2000 && !s_ed_drag){
            int sx, sy;
            if(input_touch_start_pos(&sx, &sy) && sx >= ED_LW && sy >= ED_VIEW_TOP){
                fav_t *f = &s_fav[s_drawer];
                int cy = (sy - ED_VIEW_TOP) + (int)s_ed_scroll;
                if(cy >= 0 && cy < f->nc*ED_CR){
                    s_lp_fired = true;
                    os_dialog_confirm_ex(ctx, "\xe5\x88\xa0\xe9\x99\xa4\xe8\xbf\x99\xe4\xb8\xaa\xe8\xb4\xa6\xe5\x8f\xb7?",
                                         0, -1, ed_del_cred_cb, (void *)(intptr_t)(cy / ED_CR)); /* 删除这个账号? */
                }
            }
        }
    }
    applock_tick(ctx);   /* 门禁期间: 检测手指按下/抬起, 触发一次重绘 */

    /* ★ 长按名称 2 秒 → 进入编辑 (替代原来的「编」按钮) */
    static int  lp_gi = -1;
    static int64_t lp_t0 = 0;
    static int  lp_x0 = 0;
    if (!s_authed || s_drawer >= 0 || s_cat < 0) { lp_gi = -1; }
    else {
        int tx = -1, ty = -1;
        bool down = input_get_touch_pos(&tx, &ty);
        if (!down) { lp_gi = -1; }
        else if (lp_gi < 0) {
            /* 第一次看到按下: 判断落在哪一行的名称区 (图标右侧, 不含左栏) */
            int ey = HDR_H + 2 - s_scroll;
            int nF = fav_max();
            for (int f = 0; f < nF; f++) {
                int gi = -1; fav_gi(f, &gi);
                if (gi >= 0 && ty >= ey && ty < ey + 52 && tx >= LIST_X + 56 && tx <= S_W - 6) {
                    lp_gi = gi; lp_t0 = esp_timer_get_time(); lp_x0 = tx;
                    break;
                }
                ey += 52;
            }
        } else if (esp_timer_get_time() - lp_t0 >= 2000000LL) {
            s_drawer = lp_gi; s_field = 0; s_show_cats = false; s_edit_focus = false;
            lp_gi = -1;
            redraw(ctx);
        } else if (abs(tx - lp_x0) > 12) {
            lp_gi = -1;          /* 手指明显移动 = 拖动滚动, 不算长按 */
            if (s_r_drag) lp_gi = -1;
        }
    }
    /* IP配置(STA): 等待 WiFi 连接后自动启动收藏服务 (1:1 复用局域网手柄流程) */
    if (s_lan_pending) {
        if (wifi_manager_is_connected()) {
            s_lan_pending = false;
            fav_web_start_lan(ctx);
        } else if (!wifi_manager_is_connecting() ||
                   (uint32_t)(esp_timer_get_time() / 1000) - s_lan_t0 > 60000) {
            s_lan_pending = false;   /* 连接失败/超时 → 放弃 */
        }
        return;
    }
    /* IP 服务启动完成 → 关弹窗回主桌面 + toast IP (同局域网手柄) */
    if (s_lan_toast_pending && (s_web_phase == 2 || s_web_on)) {
        s_lan_toast_pending = false;
        char ip[16] = "";
        wifi_manager_get_ip(ip, sizeof(ip));
        /* ★ 不再 os_pop_to_main: 服务在后台跑, 页面留在收藏夹 */
        ctx->needs_redraw = true;
        if (ip[0]) {
            char msg[40];
            snprintf(msg, sizeof(msg), "IP:%s", ip);
            os_dialog_toast(ctx, msg);
        } else {
            os_dialog_toast(ctx, "\xe6\x94\xb6\xe8\x97\x8f\xe6\x9c\x8d\xe5\x8a\xa1\xe5\xb7\xb2\xe5\xbc\x80\xe5\x90\xaf"); /* 收藏服务已开启 */
        }
        return;
    }
    if(!s_authed||s_drawer>=0) return;
    int tx,ty; bool down=input_get_touch_pos(&tx,&ty);
    if(down){
        if(!s_press_on){ s_press_on=true; s_press_x=tx; s_press_y=ty; s_press_t=esp_timer_get_time()/1000; s_press=false; }
        else {
            int dy=ty-s_press_y;
            if(!s_press && (dy>8||dy<-8)){ s_press=true; s_cat_drag_last=ty; s_lpane_acc=0; }   /* 左右栏都用同一个锚点 */
            if(s_press && tx<LIST_X){
                /* ★★ 左栏滚动单位是**行**（os_pane 按 32px 分行画, 没有像素偏移）:
                 *   原来直接把像素位移加到 folder_scroll 上 ⇒ 拖 1px 跳 1 行, 而且下界用的是
                 *   `(n+1)*32-270` 这种像素值 ⇒ 分类一少就恒为 0 (左栏根本滑不动)。
                 *   这里改成像素累加, 每 24px 走一行, 行数上限 = 总行数 − 可视行数。 */
                s_lpane_acc += (s_cat_drag_last - ty);
                s_cat_drag_last = ty;
                int rows = 0;
                while (s_lpane_acc >= 24)  { s_lpane_acc -= 24; rows++; }
                while (s_lpane_acc <= -24) { s_lpane_acc += 24; rows--; }
                if (rows) {
                    int vis = (S_H - 4 - HDR_H) / 32;  if (vis < 1) vis = 1;
                    int maxr = (s_cat_n + 1) - vis;    if (maxr < 0) maxr = 0;
                    int ns = s_pane.folder_scroll + rows;
                    if (ns < 0) ns = 0;
                    if (ns > maxr) ns = maxr;
                    if (ns != s_pane.folder_scroll) { s_pane.folder_scroll = ns; redraw(ctx); }
                }
            }
            else if(s_press && tx>=LIST_X && s_cat>=0){        /* ★ 右栏: 统一跟手滚动 */
                if (ui_scroll_touch_xy(&s_fav_sc, tx, ty, true)) {
                    s_r_drag = ui_scroll_dragging(&s_fav_sc);
                    redraw(ctx);
                }
            }
        }
    } else {
        if(s_r_drag || s_fav_sc.pressed){               /* 松手: 交给 ui_scroll 惯性+回弹+吸附 */
            ui_scroll_touch_xy(&s_fav_sc, 0, 0, false);
            s_r_drag=false;
        }
        s_r_anchor=0; s_press_on=false; s_press=false;
    }
}

/* ===== 进/出 ===== */
static bool s_hid_auto = false;     /* 本次进页面由我们拉起的 USB 键鼠 (退出时关掉) */
static volatile bool s_hid_want = false;   /* 当前视图"要不要 HID": 退出/切设置管理时为 false */

static void fav_hid_task(void *arg)
{
    (void)arg;
    /* ★★★ 2026-10-20 修"退出收藏夹后串口再也不回来":
     *   原来本任务**无条件** os_hid_start(), 若用户在建任务后立刻退出页面,
     *   fav_exit 里 s_hid_auto 还是 false、os_hid_is_running() 也是 false
     *   ⇒ 什么都不停; 随后本任务才把 HID 拉起来 ⇒ 串口永久消失, 只能重启。
     *   现在: 开之前查意图, 开完再查一次, 已退出就立刻关掉。 */
    if (!s_hid_want) { vTaskDelete(NULL); return; }
    if (os_hid_start() != ESP_OK) { ESP_LOGW("FAV", "USB 键鼠启动失败"); vTaskDelete(NULL); return; }
    s_hid_auto = true;
    ESP_LOGI("FAV", "USB 键鼠已后台就绪");
    if (!s_hid_want) {                 /* 启动途中页面被关 */
        os_hid_stop();
        s_hid_auto = false;
        ESP_LOGI("FAV", "页面已退出, USB 键鼠随即关闭 (串口已还回)");
    }
    vTaskDelete(NULL);
}

/* ★ 2026-10-20 linit: 只在"分类页"启用 USB 键鼠; 切到「设置管理」就关掉,
 *   把 USB 串口控制台还回来 —— 这样开着收藏夹也能烧录/抓日志。 */
/* ★ 2026-10-20: **按需起 HID** —— 进收藏夹不再抢走 USB 串口。
 *   只有真的点"推送"时才把设备切成键盘; 平时保持串口可用(可烧录/看日志)。
 *   切到「设置管理」或退出页面时仍然会把串口还回来。 */
static bool fav_hid_ensure(void)
{
    s_hid_want = true;
    if (os_hid_is_running()) return true;
    xTaskCreate(fav_hid_task, "favhid", 4096, NULL, 4, NULL);
    for (int i = 0; i < 20 && !os_hid_is_running(); i++) vTaskDelay(pdMS_TO_TICKS(50));
    return os_hid_is_running() && os_hid_connected();
}

/* ★★ 2026-10-21 linit: **进页面不再自动起 USB 键鼠**。
 *   原来只要停在分类视图就 os_hid_start() —— USB 会从 CDC/串口切成 HID KeyMouse,
 *   串口立刻消失(连 remcon/fbdump 都断), 排查和抓图全废。
 *   现在只有用户真点「推送」才起 HID (见 fav_hid_ensure), 出来即停。
 *   ⚠ 若将来确认要恢复"进页面就绪", 把下面这个宏改回 1 即可。 */
#define FAV_HID_AUTO_START 0

static void fav_hid_sync(void)
{
    s_hid_want = (s_cat >= 0);
    if (!s_hid_want) {
        if (s_hid_auto || os_hid_is_running()) { os_hid_stop(); s_hid_auto = false; }
    } else if (FAV_HID_AUTO_START && !os_hid_is_running()) {
        xTaskCreate(fav_hid_task, "favhid", 4096, NULL, 4, NULL);
    }
}

static void fav_enter(ui_ctx_t*ctx){
    s_lan_pending = false; s_lan_toast_pending = false; s_fav_import_dlg = false;
    fav_load(); if(applock_key()) fav_load_enc();   /* 已解锁才解出密码 */
    s_drawer=-1; s_scroll=0; s_sel=0; s_field=0; s_edit_focus=false; s_show_cats=false; s_need_clear = true; s_fcol_dirty = true;
    s_cat=0;
    /* 左栏: 一比一阅读器 os_pane 模板 (内存数据源, 宽 110 容纳 4 字分类) */
    s_pane.list_y=HDR_H;
    s_pane.list_bottom=S_H-4;
    s_pane.left_w=110;
    s_pane.settings_label="\xe8\xae\xbe\xe7\xbd\xae\xe7\xae\xa1\xe7\x90\x86";   /* 设置管理 (左栏首项) */
    s_pane.mem_folders=pane_mem_folders;
    os_pane_reset(&s_pane);
    s_pane.sel_folder=1;   /* 默认第一个分类 */
    fav_idle_hold_sync();   /* 进页面按当前视图定"要不要息屏" */
    /* ★ 2026-10-18: 本地 PIN 屏已删除, 统一走 applock */
    s_authed = applock_gate(ctx, "fav", "\xe6\x94\xb6\xe8\x97\x8f\xe5\xa4\xb9");
    os_keyvault_init();               /* 预热: 免得第一次登录/加密再读 NVS */
    /* ★ 不再进页面就起 HID（会抢走 USB 串口）—— 点"推送"时才起, 见 fav_hid_ensure() */
    s_hid_want = (s_cat >= 0);
    /* ★ 2026-10-20 linit: **不再进页面就起网页服务** —— 用户点「局域网配置」才起。
     *   原来一进收藏夹就建 httpd 任务（占内部 RAM + 一个常驻任务），从来没访问过也一直挂着。 */
    /* fav_web_start_lan_ex(ctx, false); */
    redraw(ctx);
}
static void fav_exit(ui_ctx_t*ctx){
    s_pane__free();   /* LINTOS: 页面退出归还列表 pane */
    wp_idle_hold(false);   /* 退出页面必须解除占位, 否则整机再也不会息屏 */
    s_hid_want = false;    /* ★ 先断意图: 还没跑起来的 fav_hid_task 看到它就不再拉 HID */
    fav_web_stop(ctx);   /* ★ 退出收藏夹就关闭网页服务 (AP 与 IP 模式都关; 不再按手柄那样留后台) */
    /* ★ 退出一律把 USB 还给串口: 不管是不是我们拉起的, 收藏夹的规则就是"退出即还" */
    if (s_hid_auto || os_hid_is_running()) { os_hid_stop(); s_hid_auto = false; }
    if(s_n>0)fav_save();
}

static os_module_t s_mod_fav={
    .name="fav", .page_id=OS_PAGE_FAV,
    .on_enter=fav_enter, .on_exit=fav_exit,
    .render=render, .action=fav_action, .touch=fav_touch, .poll=fav_poll,
    .fullscreen=false,   /* 顶部显示正常状态栏 */
    /* ★ 2026-10-20: 空闲 5 分钟不能被弹回桌面 —— 用户是"打开着慢慢填账号"的,
     *   被 os_pop_to_main 弹走就丢现场(实测 linit 报"不动一段时间就退回桌面")。 */
    .keep_on_idle=true,
};
void os_page_fav_register(void){ os_register(&s_mod_fav); }