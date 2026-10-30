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
#include "psa/crypto.h"  /* ★ C1: 备份整包加密 (PBKDF2-HMAC-SHA256 + AES-256-GCM) */

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
static int  s_r_anchor, s_r_base __attribute__((unused));   /* 右栏拖动: 起点 y / 起始滚动量 */
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
/* ★ C4: 会话内首次推送时提示一次"按 US 布局发送"（HID Usage 是物理键位） */
static bool  s_us_hint_shown = false;
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
/* ★ C1: 本次会话的备份口令（独立于设备 PIN，不写入任何文件） */
static char s_bk_pw[33] = {0};
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

/* ★ R4 (2026-10-30) 一致性/安全标记 —— 修 P0"导入后密码被清空覆盖"与"blob/enc 版本错配":
 *   s_enc_loaded : 密码段已成功解到内存(或确认本来就是空)。为 false 时**禁止**改写 NVS_ENC,
 *                  否则会把"内存里的空密码"当成真相写回去, 密码永久丢。
 *   s_blob_v5    : 当前 NVS_DATA 是不是带 seq 的新格式
 *   s_blob_seq   : blob 里记录的版本序号, 与 enc 头部的 seq 必须一致, 否则拒绝解密(不让密码串号)
 *   s_enc_mismatch: 检测到不一致 → 界面提示"密码段与条目不同步" */
static void wr_u32le(uint8_t *p, uint32_t v);   /* fwd: 实现在备份加密段 */

static bool     s_enc_loaded   = false;
/* ★ 收尾核查: 导入过程中 s_fav 处于"清空 → 逐条填"的中间态, 此刻任何一次 fav_save()
 *   (比如并发的网页写操作)都会把这个半成品落盘。用标志把保存挡在外面, 几行代码挡住真丢数据。 */
static volatile bool s_importing = false;
static bool     s_blob_v5      = false;
static volatile bool s_enc_mismatch = false;
/* 前向: 这些编解码函数定义在文件后部, 前面先用 */
static void     wr_u32le(uint8_t *p, uint32_t v);
static uint32_t rd_u32le(const uint8_t *p);

static uint32_t s_blob_seq     = 0;

/* 把 s_fav 里的密码加密成 blob; 成功返回长度, 失败 0 */
static size_t fav_build_enc(uint8_t *out, size_t cap, uint32_t seq){
    if(!applock_key()) return 0;          /* 未解锁不写, 防止把明文换成空 */
    if (cap < 5) return 0;
    wr_u32le(out, seq);                   /* ★ R4: 前 4 字节 = 版本序号, 与 blob 里的 seq 对齐 */
    size_t o = 4;
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

/* 保存收藏。★ R4: 返回 bool, 任何关键写入失败都如实返回 false（以前 void + 忽略返回值 ⇒
 *   NVS 满了也当成功, UI 显示"已保存", 用户下次开机发现数据没了）。 */
static bool fav_save(void){
    if (s_importing) {                    /* 导入进行中: 内存是半成品, 不许落盘 */
        ESP_LOGW("FAV", "导入进行中, 本次保存跳过");
        return false;
    }
    /* ① 密码段没载入内存 ⇒ 一个字节都不写。写了就是拿空密码覆盖真密文(P0-1)。 */
    if (s_n > 0 && !s_enc_loaded) {
        ESP_LOGW("FAV", "密码未载入内存, 拒绝保存(防止用空密码覆盖旧密文)");
        return false;
    }

    uint32_t seq = s_blob_seq + 1;
    /* ② 先算密文: 超限/失败就整次放弃, 绝不出现"blob 新、enc 旧" */
    size_t el0 = 0;
    if (s_n > 0) {
        el0 = fav_build_enc(s_encbuf, sizeof(s_encbuf), seq);
        if (el0 == 0) {
            ESP_LOGE("FAV", "密码密文构建失败/超限(%u KB), 本次不保存", (unsigned)(sizeof(s_encbuf)/1024));
            return false;
        }
    } else if (applock_key()) {
        wr_u32le(s_encbuf, seq);          /* 空收藏: 也写 4 字节头, 保持格式一致 */
        el0 = 4;
    }

    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE("FAV", "NVS 打不开, 保存失败");
        return false;
    }

    /* ③ blob: V5 = magic(3) + seq(4) + n(u16) + 条目 */
    size_t need = 9;
    for(int i=0;i<s_n;i++){
        fav_t *f=&s_fav[i];
        need += 3 + 1+strlen(f->name) + 1+strlen(f->desc) + 1+strlen(f->url);
        for(int c=0;c<f->nc && c<CR_MAX;c++) need += 1+strlen(f->cr[c].user);
    }
    uint8_t *blob = heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!blob){ svc_nvs_close(h); ESP_LOGE("FAV","内部RAM不足, 保存失败"); return false; }
    size_t o=0;
    blob[o++]=0xFA; blob[o++]=0x56; blob[o++]=0x05;      /* magic "FV5" */
    wr_u32le(blob+o, seq); o += 4;
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
    esp_err_t e1 = svc_nvs_set_blob(h, NVS_DATA, blob, o);
    free(blob);
    esp_err_t e2 = (s_n == 0)
                 ? svc_nvs_set_blob(h, NVS_ENC, s_encbuf, 0)
                 : svc_nvs_set_blob(h, NVS_ENC, s_encbuf, el0);
    svc_nvs_commit(h);
    svc_nvs_close(h);

    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGE("FAV", "NVS 写入失败(blob=%s enc=%s) —— 可能是空间不足",
                 esp_err_to_name(e1), esp_err_to_name(e2));
        return false;
    }
    s_blob_v5 = true; s_blob_seq = seq; s_enc_mismatch = false;
    return true;
}

/* ★ R4: 设备端保存的统一出口 —— 失败必须让用户看到, 不能静默 */
static void fav_save_ui(ui_ctx_t *ctx) {
    if (!fav_save() && ctx) os_dialog_toast(ctx, "\xE4\xBF\x9D\xE5\xAD\x98\xE5\xA4\xB1\xE8\xB4\xA5\x28\xE7\xA9\xBA\xE9\x97\xB4\xE4\xB8\x8D\xE8\xB6\xB3\x3F\x29");
}

/* 载入后把密码解密回内存 (需要已解锁; 未解锁时留空, 解锁后再调一次) */
/* 载入后把密码解密回内存。★ R4 修两处:
 *   ① 缓冲长度原来写成 sizeof(enc)(指针=4) ⇒ 只要存过密码就 INVALID_LENGTH, 密码从没解出来过;
 *   ② 校验 enc 头部 seq 与 blob 的 seq 一致, 不一致就**拒绝解密**, 不许把密码解到别的账号上。 */
/* 载入后把密码解密回内存。
 *   ★ 收尾核查(2026-10-30): 只有"结构完整 + 每一项都解密成功"才置 s_enc_loaded = true。
 *   以前三条路径会把"部分成功"当成功:
 *     ① 长度不够(o+2>el / o+cn>el) 直接 s_enc_loaded=true 返回;
 *     ② applock_decrypt 返回 0 时静默跳过(密码留空), 最后照样置 true;
 *     ③ 密文比条目少时循环提前结束, 没人发现。
 *   一旦被当成 true, 下一次 fav_save() 就会用"空密码"覆盖真密文 —— 密码永久丢。
 *   现在任何异常都保持 false + s_enc_mismatch, 让上层拒绝保存并提示重新导入备份。 */
static bool fav_load_enc(void){
    s_enc_loaded = false;
    if(!applock_key()) return false;
    nvs_handle_t h;
    if(svc_nvs_open(NVS_NS,NVS_READONLY,&h)!=ESP_OK) return false;
    uint8_t *enc = s_encbuf;
    size_t el = sizeof(s_encbuf);
    esp_err_t er = svc_nvs_get_blob(h,NVS_ENC,enc,&el);
    svc_nvs_close(h);

    /* 首次使用: 确认 NVS_ENC 确实不存在 → 允许空密码初始化 */
    if (er == ESP_ERR_NVS_NOT_FOUND || (er == ESP_OK && el == 0)) {
        s_enc_loaded = true;
        return true;
    }
    if (er != ESP_OK) {
        ESP_LOGE("FAV", "密码段读取失败: %s", esp_err_to_name(er));
        return false;
    }
    if (s_n == 0) {                       /* 没有条目 = 没有密码要保护: 直接放行,
                                           * 否则残留密文会让"删空后再新增"被永久卡住 */
        memset(enc, 0, el);
        s_enc_loaded = true;
        return true;
    }

    size_t o = 0;
    if (s_blob_v5) {
        if (el < 4) { s_enc_mismatch = true; ESP_LOGE("FAV", "密码段格式异常(太短)"); return false; }
        uint32_t es = rd_u32le(enc);
        if (es != s_blob_seq) {
            s_enc_mismatch = true;
            ESP_LOGE("FAV", "密码段与条目版本不一致(enc=%u blob=%u), 拒绝解密",
                     (unsigned)es, (unsigned)s_blob_seq);
            return false;
        }
        o = 4;
    }

    bool ok = true;
    size_t slots = 0;
    for (int i = 0; i < s_n && ok; i++) {
        for (int c = 0; c < s_fav[i].nc && c < CR_MAX && ok; c++) {
            slots++;
            if (o >= el) { ok = false; break; }           /* 条目比密文多 ⇒ 截断 */
            uint8_t present = enc[o++];
            if (!present) continue;
            if (o + 2 > el) { ok = false; break; }
            size_t cn = (size_t)enc[o] | ((size_t)enc[o+1] << 8);
            o += 2;
            if (cn == 0 || o + cn > el) { ok = false; break; }
            char tmp[64];
            size_t pl = applock_decrypt(enc + o, cn, tmp);
            o += cn;
            if (pl == 0 || pl >= sizeof(tmp)) { ok = false; break; }   /* 解不开 ⇒ 整体失败 */
            tmp[pl] = 0;
            str_trunc(s_fav[i].cr[c].pass, sizeof(s_fav[i].cr[c].pass), tmp);
        }
    }
    if (ok && o != el) ok = false;      /* 尾部多余字节 ⇒ 结构不对 */

    if (!ok) {
        s_enc_mismatch = true;
        ESP_LOGE("FAV", "密码段结构异常/解密失败(用 %u/%u B, %u 槽), 拒绝标记为已载入",
                 (unsigned)o, (unsigned)el, (unsigned)slots);
    }
    memset(enc, 0, el);                 /* 密文缓冲用完即清 */
    s_enc_loaded = ok;
    return ok;
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
    s_enc_loaded = false; s_enc_mismatch = false;   /* ★ R4: 密码已在内存里清空, 必须重新载入才算数 */
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
               p[0]==0xFA && p[1]==0x56 && (p[2]==0x04 || p[2]==0x05)){
                /* ---------- V4/V5 变长 (V5 多 4 字节 seq, 与密码段对齐) ---------- */
                int n = 0; size_t o = 5;
                if (p[2] == 0x05 && sz >= 9) {
                    s_blob_v5 = true; s_blob_seq = rd_u32le(p + 3);
                    n = p[7] | (p[8] << 8); o = 9;
                } else {
                    s_blob_v5 = false; s_blob_seq = 0;
                    if (sz >= 5) n = p[3] | (p[4]<<8);
                }
                if(n > FAV_MAX) n = FAV_MAX;
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
static bool fav_icon_set(int gi, const uint8_t *bmp);
static void fav_icon_del(int gi);
static void fav_icons_migrate(void);

/* 备份列表 (弹窗选择用) */
static char        s_bak_path[FAV_BAK_MAX][56];
static char        s_bak_label[FAV_BAK_MAX][40];
static const char *s_bak_items[FAV_BAK_MAX];

/* ==================== C1: 备份整包加密 ====================
 * 设计（2026-10-30）:
 *   文件 = "FAVENCC1"(8) | ver(1) | kdf(1) | iters(u32 LE) | salt(16) | nonce(12)
 *          | ctlen(u32 LE) | ciphertext | tag(16)
 *   - 密钥 = PBKDF2-HMAC-SHA256(口令, salt, iters=20000, 32B)
 *   - 加密 = AES-256-GCM（认证加密，tag 校验失败即拒绝，绝不返回部分数据）
 *   - 口令**独立于设备 PIN**，不写入文件；跨设备恢复只需"文件 + 口令"
 *   - 明文段内部仍是原 FAVEXP2 结构 ⇒ 旧明文备份仍可识别并导入（带警告）
 * 缓冲用 PSRAM：文件 IO 不关 cache，可以安全从 PSRAM 读；NVS 那边才必须内部 RAM。 */
#define FAVBK_MAGIC   "FAVENCC1"
#define FAVBK_VER     2        /* ★ 收尾核查: v2 起文件头参与 AAD 认证 */
#define FAVBK_VER_NOAAD 1     /* v1(第三轮产物): 头未参与 AAD —— **只读兼容, 绝不写出** */
#define FAVBK_KDF     1        /* 1 = PBKDF2-HMAC-SHA256 */
#define FAVBK_ITERS   20000u
#define FAVBK_SALT    16
#define FAVBK_NONCE   12
#define FAVBK_TAG     16
#define FAVBK_HDR     (8 + 1 + 1 + 4 + FAVBK_SALT + FAVBK_NONCE + 4)

/* 简单内存写入器（替代 open_memstream，行为可控） */
typedef struct { uint8_t *p; size_t len, cap; bool ok; } bwr_t;
static void bw(bwr_t *w, const void *d, size_t n) {
    if (!w->ok) return;
    if (w->len + n > w->cap) { w->ok = false; return; }
    if (d) memcpy(w->p + w->len, d, n);
    else   memset(w->p + w->len, 0, n);
    w->len += n;
}
static void bw_u8(bwr_t *w, uint8_t v)  { bw(w, &v, 1); }

static void wr_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 口令 → 32B 密钥。★ R4: 迭代数改用**文件头里的值**（以前写死常量 ⇒ 头里的 iters 形同虚设,
 *   将来调参数旧备份立刻解不开） */
static bool favbk_kdf(const char *pw, const uint8_t *salt, uint32_t iters, uint8_t out[32]) {
    if (!pw || !pw[0] || iters < 1000) return false;
    psa_crypto_init();
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iters);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, FAVBK_SALT);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                            (const uint8_t *)pw, strlen(pw));
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_output_bytes(&op, out, 32);
    psa_key_derivation_abort(&op);
    return st == PSA_SUCCESS;
}

/* 用派生密钥做 AES-256-GCM；返回密文+tag 长度，失败 0。
 * ★ R4: 增加 AAD 参数 —— 文件头(魔数/版本/KDF/迭代数/salt/nonce/密文长度)全部纳入认证,
 *   任何一位被改都会让 tag 校验失败。 */
static size_t favbk_gcm(int enc, const uint8_t key[32], const uint8_t *nonce,
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t n, uint8_t *out) {
    psa_key_attributes_t at = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&at, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&at, 256);
    psa_set_key_usage_flags(&at, enc ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&at, PSA_ALG_GCM);
    psa_key_id_t k = 0;
    if (psa_import_key(&at, key, 32, &k) != PSA_SUCCESS) return 0;
    size_t olen = 0;
    psa_status_t st;
    if (enc)
        st = psa_aead_encrypt(k, PSA_ALG_GCM, nonce, FAVBK_NONCE, aad, aad_len,
                              in, n, out, n + FAVBK_TAG, &olen);
    else
        st = psa_aead_decrypt(k, PSA_ALG_GCM, nonce, FAVBK_NONCE, aad, aad_len,
                              in, n, out, n, &olen);
    psa_destroy_key(k);
    return (st == PSA_SUCCESS) ? olen : 0;
}

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
/* ★ R4: 返回 false = 目录不可读, 调用方必须**拒绝导出**（以前会瞎猜一个号去覆盖） */
static bool fav_bak_new_path(char *out, size_t cap)
{
    fav_bak_mkdir();
    char p[96];
    /* ★ 修复(2026-10-30): 原来一直找到 99 号 —— 但产品设定是**最多 16 个**,
     *   于是第 17 个起就超出 fav_bak_scan 的可见范围(用户看不到也导不回来)。
     *   现在严格限制在 FAV_BAK_MAX 内。 */
    for (int i = 1; i <= (int)FAV_BAK_MAX; i++) {
        snprintf(p, sizeof(p), "%s/fav_%02d.bak", FAV_BAK_DIR, i);
        FILE *f = fopen(p, "rb");
        if (!f) { snprintf(out, cap, "%s", p); return true; }
        fclose(f);
    }
    /* 16 个都满: 覆盖**最旧**的那个（按 mtime，不按文件名猜，避免误删最新备份） */
    int oldest = -1; long long ot = 0;
    for (int i = 1; i <= (int)FAV_BAK_MAX; i++) {
        snprintf(p, sizeof(p), "%s/fav_%02d.bak", FAV_BAK_DIR, i);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (oldest < 0 || (long long)st.st_mtime < ot) { ot = (long long)st.st_mtime; oldest = i; }
    }
    if (oldest < 0) {                       /* 16 个都"存在"却全都 stat 不出来 ⇒ 目录坏了 */
        ESP_LOGE("FAV", "备份目录不可读(stat 全部失败), 拒绝导出");
        out[0] = 0;
        return false;
    }
    snprintf(out, cap, "%s/fav_%02d.bak", FAV_BAK_DIR, oldest);
    ESP_LOGW("FAV", "备份已满 %d 个, 覆盖最旧的 fav_%02d.bak", (int)FAV_BAK_MAX, oldest);
    return true;
}

/* 把当前收藏序列化成 FAVEXP2 明文到内存（成功返回 true） */
static bool fav_export_plain(uint8_t *buf, size_t cap, size_t *out_len) {
    bwr_t w = { buf, 0, cap, true };
    bw(&w, "FAVEXP2", 7);
    bw_u8(&w, (uint8_t)s_cat_n);
    for (int i = 0; i < s_cat_n && i < CAT_MAX; i++) bw(&w, s_cats[i], CAT_SZ);
    uint8_t fn = (uint8_t)(s_n > 255 ? 255 : s_n);
    bw_u8(&w, fn);
    if (fn) bw(&w, s_fav, (size_t)fn * sizeof(fav_t));
    uint8_t bmp[FAV_ICON_BYTES];
    for (int i = 0; i < (int)fn; i++) {
        uint8_t has = fav_icon_get(i, bmp) ? 1 : 0;
        bw_u8(&w, has);
        if (has) bw(&w, bmp, FAV_ICON_BYTES);
    }
    uint8_t zb[22] = {0};          /* PIN 段: 归系统 keyvault, 不随备份迁移 */
    bw(&w, zb, sizeof(zb));
    if (!w.ok) return false;
    *out_len = w.len;
    return true;
}

/* ★ R4: 导出为整包加密。头先拼好，**头本身作为 AAD** 参与认证。
 *   口令独立于设备 PIN，不落盘、不进日志。 */
static int fav_export_file(const char *path){
    if (!s_bk_pw[0]) { ESP_LOGW("FAV", "未设置备份口令, 拒绝导出"); return -3; }

    /* 1) 明文（PSRAM: 文件 IO 不关 cache，可安全使用） */
    size_t pcap = 7 + 1 + (size_t)s_cat_n * CAT_SZ + 1
                + (size_t)s_n * sizeof(fav_t)
                + (size_t)s_n * (1 + FAV_ICON_BYTES) + 22;
    uint8_t *pt = heap_caps_malloc(pcap, MALLOC_CAP_SPIRAM);
    if (!pt) return -1;
    size_t plen = 0;
    if (!fav_export_plain(pt, pcap, &plen)) { heap_caps_free(pt); return -1; }

    /* 2) salt / nonce / 密钥 */
    uint8_t salt[FAVBK_SALT], nonce[FAVBK_NONCE], key[32];
    for (int i = 0; i < FAVBK_SALT; i++)  salt[i]  = (uint8_t)(esp_random() & 0xFF);
    for (int i = 0; i < FAVBK_NONCE; i++) nonce[i] = (uint8_t)(esp_random() & 0xFF);
    if (!favbk_kdf(s_bk_pw, salt, FAVBK_ITERS, key)) { heap_caps_free(pt); return -1; }

    /* 3) 先拼文件头（ctlen 已知 = plen + tag） */
    uint8_t hdr[FAVBK_HDR];
    memcpy(hdr, FAVBK_MAGIC, 8);
    hdr[8] = FAVBK_VER; hdr[9] = FAVBK_KDF;
    wr_u32le(hdr + 10, FAVBK_ITERS);
    memcpy(hdr + 14, salt, FAVBK_SALT);
    memcpy(hdr + 14 + FAVBK_SALT, nonce, FAVBK_NONCE);
    wr_u32le(hdr + 14 + FAVBK_SALT + FAVBK_NONCE, (uint32_t)(plen + FAVBK_TAG));

    /* 4) 加密（AAD = 文件头） */
    uint8_t *ct = heap_caps_malloc(plen + FAVBK_TAG, MALLOC_CAP_SPIRAM);
    if (!ct) { heap_caps_free(pt); return -1; }
    size_t ctlen = favbk_gcm(1, key, nonce, hdr, FAVBK_HDR, pt, plen, ct);
    memset(pt, 0, plen); heap_caps_free(pt);     /* 明文不在堆里久留 */
    if (ctlen == 0) { heap_caps_free(ct); return -1; }

    /* 5) 原子写：tmp → 校验长度 → 换名（**不先删旧文件**） */
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { heap_caps_free(ct); return -1; }
    bool ok = (fwrite(hdr, 1, FAVBK_HDR, f) == FAVBK_HDR) &&
              (fwrite(ct, 1, ctlen, f) == ctlen);
    if (ok) ok = (fflush(f) == 0);
    int cerr = fclose(f);
    heap_caps_free(ct);
    if (ok && cerr != 0) ok = false;
    if (ok) {
        struct stat st;
        if (stat(tmp, &st) != 0 || (size_t)st.st_size != (size_t)FAVBK_HDR + ctlen) ok = false;
    }
    if (!ok) { remove(tmp); ESP_LOGW("FAV", "导出失败: 写入不完整"); return -1; }
    /* ★ R4: 不再 remove(path)。FatFs 的 f_rename 目标存在时会覆盖,
     *   原来那行 remove 是"rename 失败 ⇒ 旧备份已删"的唯一来源。 */
    if (rename(tmp, path) != 0) { remove(tmp); ESP_LOGW("FAV", "导出换名失败, 旧备份保留"); return -1; }
    ESP_LOGI("FAV", "已加密导出 %d 条到 %s (明文 %u B → 密文 %u B)",
             s_n, path, (unsigned)plen, (unsigned)ctlen);
    return 0;
}

/* ★ R4: 解密到**内存**（PSRAM），不再写 <备份>.dec.tmp 明文文件。
 *   返回 0=成功 / -2=口令错或数据被改 / -4=格式或 IO 错 */
static int favbk_decrypt_mem(const char *in_path, uint8_t **out_buf, size_t *out_len) {
    *out_buf = NULL; *out_len = 0;
    FILE *f = fopen(in_path, "rb");
    if (!f) return -4;
    uint8_t hdr[FAVBK_HDR];
    if (fread(hdr, 1, FAVBK_HDR, f) != FAVBK_HDR) { fclose(f); return -4; }
    if (memcmp(hdr, FAVBK_MAGIC, 8) != 0) { fclose(f); return -4; }
    /* ★ 兼容: 三个版本可能同时存在于用户的卡上 ——
     *     v1-a  第三轮产物: 头不参与 AAD
     *     v1-b  第四轮中间构建: 版本号仍写 1, 但**头已经参与 AAD**（这是个坑：
     *           只看版本号会把它当成 v1-a，用 AAD=NULL 去解必然失败）
     *     v2    本次起: 头参与 AAD
     *   所以 v1 一律"两种 AAD 各试一次" —— GCM 靠 tag 自己判定用哪种，
     *   试错的那次只是多算一遍，不会误解出垃圾（tag 不过就不给数据）。
     *   v2 不走回退：新格式必须严格带头认证，不接受任何降级。 */
    if ((hdr[8] != FAVBK_VER && hdr[8] != FAVBK_VER_NOAAD) || hdr[9] != FAVBK_KDF) {
        fclose(f); return -4;
    }
    uint32_t iters = rd_u32le(hdr + 10);
    if (iters < 1000 || iters > 500000) { fclose(f); return -4; }   /* 防恶意参数把设备拖死 */
    const uint8_t *salt  = hdr + 14;
    const uint8_t *nonce = hdr + 14 + FAVBK_SALT;
    uint32_t ctlen = rd_u32le(hdr + 14 + FAVBK_SALT + FAVBK_NONCE);
    if (ctlen < FAVBK_TAG || ctlen > (8u << 20)) { fclose(f); return -4; }
    uint8_t *ct = heap_caps_malloc(ctlen, MALLOC_CAP_SPIRAM);
    if (!ct) { fclose(f); return -4; }
    if (fread(ct, 1, ctlen, f) != ctlen) { heap_caps_free(ct); fclose(f); return -4; }
    fclose(f);

    uint8_t key[32];
    if (!favbk_kdf(s_bk_pw, salt, iters, key)) { heap_caps_free(ct); return -2; }
    size_t plen = (size_t)ctlen - FAVBK_TAG;
    uint8_t *pt = heap_caps_malloc(plen ? plen : 1, MALLOC_CAP_SPIRAM);
    if (!pt) { heap_caps_free(ct); return -4; }
    size_t got;
    if (hdr[8] == FAVBK_VER) {
        got = favbk_gcm(0, key, nonce, hdr, FAVBK_HDR, ct, ctlen, pt);
    } else {
        /* v1：先按"带头 AAD"（第四轮中间构建），失败再按"无 AAD"（第三轮） */
        got = favbk_gcm(0, key, nonce, hdr, FAVBK_HDR, ct, ctlen, pt);
        if (got == 0) got = favbk_gcm(0, key, nonce, NULL, 0, ct, ctlen, pt);
        if (got) ESP_LOGW("FAV", "已按 v1 兼容格式解出备份(旧版 FAVENCC1)");
    }
    heap_caps_free(ct);
    if (got == 0) { heap_caps_free(pt); return -2; }        /* 口令错 / 数据被篡改 */
    *out_buf = pt; *out_len = got;
    return 0;
}

/* ★ R4: 导入读取适配层 —— 文件 或 内存。加密备份解出的明文直接在 PSRAM 里读,
 *   所以不再需要明文临时文件(旧路径遗留的 .dec.tmp 由 fav_enter 清)。 */
typedef struct { FILE *f; const uint8_t *m; long len, pos; } fav_rd_t;
static size_t frd_read(fav_rd_t *r, void *d, size_t n) {
    if (!r) return 0;
    if (r->f) return fread(d, 1, n, r->f);
    long avail = r->len - r->pos;
    if (avail <= 0) return 0;
    if ((long)n > avail) n = (size_t)avail;
    if (n) memcpy(d, r->m + r->pos, n);
    r->pos += (long)n;
    return n;
}
static long frd_tell(const fav_rd_t *r) { return r->f ? ftell(r->f) : r->pos; }

/* 真正的解析主体: 从 r 里读, total = 总长度(用于回推旧格式的记录长度) */
static int fav_import_stream_inner(fav_rd_t *r, long total) {

    char magic[8] = {0};
    uint8_t cn = 0, fn = 0, pb = 0, pl = 0, pinblob[20] = {0};
    char cats[CAT_MAX][CAT_SZ];
    memset(cats, 0, sizeof(cats));

    int rc = -1, v2 = 0;
    if (frd_read(r, magic, 7) != 7) { return -3; }
    if (memcmp(magic, "FAVEXP2", 7) == 0)      v2 = 1;
    else if (memcmp(magic, "FAVEXP1", 7) != 0) { return -3; }

    if (frd_read(r, &cn, 1) != 1 || cn > CAT_MAX) { return -3; }
    if (cn && frd_read(r, cats, (size_t)cn * CAT_SZ) != (size_t)cn * CAT_SZ) { return -3; }
    if (frd_read(r, &fn, 1) != 1) { return -3; }   /* u8 ⇒ 最大 255, 越不过 FAV_MAX */

    /* ★ 记录长度: v2 固定 sizeof(fav_t); v1 按"剩余字节 = fn*rec + 22"反推 (3 种历史布局) */
    long rest = total - frd_tell(r);
    size_t rec = 0;
    if (v2) rec = sizeof(fav_t);
    else {
        long payload = rest - 22;
        if (fn == 0) rec = sizeof(fav_t);
        else if (payload == (long)fn * (long)sizeof(fav_t))     rec = sizeof(fav_t);
        else if (payload == (long)fn * (long)sizeof(fav_v2_t))  rec = sizeof(fav_v2_t);
        else if (payload == (long)fn * (long)sizeof(fav_old_t)) rec = sizeof(fav_old_t);
    }
    if (!rec) { return -3; }

    memset(s_fav, 0, sizeof(fav_t) * FAV_MAX);
    for (int i = 0; i < (int)fn; i++) {
        uint8_t buf[sizeof(fav_t)];
        if (frd_read(r, buf, rec) != rec) { return -3; }
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
    int icon_fail = 0;
    if (v2) {
        uint8_t bmp[FAV_ICON_BYTES];
        for (int i = 0; i < (int)fn; i++) {
            uint8_t has = 0;
            if (frd_read(r, &has, 1) != 1) { return -3; }
            if (has) {
                if (frd_read(r, bmp, FAV_ICON_BYTES) != FAV_ICON_BYTES) { return -3; }
                if (!fav_icon_set(i, bmp)) icon_fail++;
            } else {
                fav_icon_del(i);
            }
        }
        /* 清掉备份里没有的多余图标槽 */
        for (int i = (int)fn; i < FAV_MAX; i++) fav_icon_del(i);
    }

    if (frd_read(r, &pb, 1) == 1 && frd_read(r, &pl, 1) == 1 && pl <= 20 &&
        frd_read(r, pinblob, 20) == 20) {
        memcpy(s_cats, cats, sizeof(cats)); s_cat_n = cn;
        if (s_cat_n <= 0) s_cat_n = 1;
        for (int i = 0; i < s_n; i++) if (s_fav[i].cat >= s_cat_n) s_fav[i].cat = 0;
        /* PIN 归系统 keyvault(统一配置), 备份仅恢复收藏与分类, 不恢复 PIN */
        /* ★ 收尾核查: 导入是**整体替换**, 密码全部来自备份文件本身 ⇒ 此刻内存里的密码
         *   就是用户要的那一份。若旧密文已损坏(s_enc_loaded=false), 不松开这一位的话
         *   fav_save() 会一直拒绝, 用户就永远修不回来了。 */
        s_enc_loaded = true;
        s_importing = false;         /* ★ 数据已完整, 这一次保存必须放行(否则导入永远失败) */
        if (!fav_save()) {
            rc = -7;                 /* ★ R4: NVS 写入失败绝不能报"导入成功" */
        } else {
            cats_save();
            s_fcol_dirty = true;
            rc = 0;
            if (icon_fail) { ESP_LOGW("FAV", "%d 个图标未能写入 TF", icon_fail); rc = -6; }
        }
    }
    return rc;
}

/* ★ 导入期间置 s_importing, 结束后无论成败都复位 */
static int fav_import_stream(fav_rd_t *r, long total) {
    s_importing = true;
    int rc = fav_import_stream_inner(r, total);
    s_importing = false;
    return rc;
}

/* 导入入口: 文件 → 判魔数(加密?) → 内存解密 或 直接读文件 */
static int fav_import_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;                       /* -1 = 没有备份文件 */
    char mg[8] = {0};
    size_t g = fread(mg, 1, sizeof(mg), f);
    if (g == 8 && memcmp(mg, FAVBK_MAGIC, 8) == 0) {     /* 加密备份 */
        fclose(f);
        if (!s_bk_pw[0]) { ESP_LOGW("FAV", "加密备份需要口令"); return -3; }
        uint8_t *pt = NULL; size_t plen = 0;
        int dr = favbk_decrypt_mem(path, &pt, &plen);
        if (dr != 0) { ESP_LOGW("FAV", "解密失败 rc=%d", dr); return dr; }
        fav_rd_t rd = { NULL, pt, (long)plen, 0 };
        int rc = fav_import_stream(&rd, (long)plen);
        memset(pt, 0, plen); heap_caps_free(pt);   /* 明文不在堆里久留 */
        return rc;
    }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz <= 0) { fclose(f); return -1; }
    fav_rd_t rd = { f, NULL, fsz, 0 };
    int rc = fav_import_stream(&rd, fsz);
    fclose(f);
    return rc;
}

/* ================= 网页配置 (热点 AP / IP STA) =================
 * 1:1 复用密钥管理器/局域网手柄模式: 未连WiFi → 系统"连接WiFi"流程 → 连接成功后
 * 启动服务 → toast IP 提示. 热点模式自建 "LinTOS-FAV" 热点 (有 PIN 则作 Wi-Fi 密码). */
/* ================= 自定义图标 (32×32 1bpp, NVS linfav/imgNN) ================
 * 设备不解码图片: 浏览器把图转成 32×32 黑白位图(128B)后上传, 这里只存取。 */
#define FAV_ICON_BYTES 128

/* ================= R4: 收藏夹图标改存 TF 卡 =================
 * 文件: /sdcard/fav_icons/iNNN.bin  (128 B, 32×32 1bpp) —— 索引 000~255 一一对应。
 * 旧 NVS("imgNNN" / "imgNN") 只在新目录**还没迁移**时读一次；迁移完成后永不回读，
 * 因此"删条目后旧图标复活到新条目"的问题从根上消失。
 * NVS 里的旧图标本轮**不删除**，留作兼容备份（迁移失败还能回退）。 */
#define FAV_ICON_DIR   "/sdcard/fav_icons"
#define FAV_ICON_MARK  FAV_ICON_DIR "/.migrated"

static int s_ic_tf  = -1;      /* -1 未探测 / 0 不可用 / 1 可用 */
static int s_ic_mig = -1;      /* -1 未探测 / 0 未迁移 / 1 已迁移 */

static bool fav_icon_path(int gi, char *out, size_t cap) {
    if (gi < 0 || gi >= FAV_MAX) return false;
    snprintf(out, cap, FAV_ICON_DIR "/i%03d.bin", gi);
    return true;
}
/* TF 是否可用（每次进页面探测一次，避免"没卡还反复 fopen"） */
static bool fav_tf_ok(void) {
    if (s_ic_tf >= 0) return s_ic_tf == 1;
    struct stat st;
    s_ic_tf = 0;
    if (stat("/sdcard", &st) == 0) {
        mkdir(FAV_ICON_DIR, 0777);                    /* 已存在返回 EEXIST, 忽略 */
        if (stat(FAV_ICON_DIR, &st) == 0) s_ic_tf = 1; /* 目录建不出来(只读/无卡) → 判不可用 */
    }
    return s_ic_tf == 1;
}
/* 迁移完成标记: 文件标记 + NVS 标记双保险。
 *   ⚠ 只认文件标记是不够的 —— 卡拔掉后 stat 失败会被判成"没迁移过", 于是又回读旧 NVS,
 *   被删掉的旧图标就"复活"了。所以完成时同时写一个 NVS 标记, 之后永远只认 TF。 */
static bool fav_ic_migrated(void) {
    if (s_ic_mig < 0) {
        struct stat st;
        bool by_file = (fav_tf_ok() && stat(FAV_ICON_MARK, &st) == 0);
        bool by_nvs  = false;
        nvs_handle_t h; uint8_t v = 0;
        if (svc_nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            if (svc_nvs_get_u8(h, "imgmig", &v) == ESP_OK && v == 1) by_nvs = true;
            svc_nvs_close(h);
        }
        s_ic_mig = (by_file || by_nvs) ? 1 : 0;
    }
    return s_ic_mig == 1;
}

/* ---- TF 读写：写后校验长度，失败不留半截文件 ---- */
static bool fav_icon_tf_get(int gi, uint8_t *out) {
    char p[48];
    if (!out || !fav_icon_path(gi, p, sizeof(p))) return false;
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, FAV_ICON_BYTES, f);
    fclose(f);
    return n == FAV_ICON_BYTES;
}
static bool fav_icon_tf_set(int gi, const uint8_t *bmp) {
    char p[48];
    if (!bmp || !fav_icon_path(gi, p, sizeof(p))) return false;
    FILE *f = fopen(p, "wb");
    if (!f) return false;
    size_t n = fwrite(bmp, 1, FAV_ICON_BYTES, f);
    if (n == FAV_ICON_BYTES) fflush(f);
    fclose(f);
    struct stat st;
    if (n != FAV_ICON_BYTES || stat(p, &st) != 0 || (size_t)st.st_size != FAV_ICON_BYTES) {
        remove(p);
        ESP_LOGW("FAV", "图标写入失败: %s", p);
        return false;
    }
    return true;
}
static void fav_icon_tf_del(int gi) {
    char p[48];
    if (fav_icon_path(gi, p, sizeof(p))) remove(p);
}

/* ---- 旧 NVS 键（仅迁移/未迁移时读） ---- */
static void fav_img_key(int gi, char *k, size_t n) {
    if (!k || n < 8) return;
    unsigned v = (gi < 0) ? 0u : ((unsigned)gi > (FAV_MAX - 1) ? (FAV_MAX - 1) : (unsigned)gi);
    k[0]='i'; k[1]='m'; k[2]='g';
    k[3]=(char)('0' + (v / 100u));
    k[4]=(char)('0' + ((v / 10u) % 10u));
    k[5]=(char)('0' + (v % 10u));
    k[6]='\0';
}
static void fav_img_key_legacy(int gi, char *k, size_t n) {   /* 老版本 img00~img99 */
    if (!k || n < 6) return;
    unsigned v = (gi < 0) ? 0u : ((unsigned)gi > 99u ? 99u : (unsigned)gi);
    k[0]='i'; k[1]='m'; k[2]='g';
    k[3]=(char)('0' + (v / 10u));
    k[4]=(char)('0' + (v % 10u));
    k[5]='\0';
}
static bool fav_icon_nvs_get(int gi, uint8_t *out) {
    nvs_handle_t h; char k[10]; fav_img_key(gi, k, sizeof(k));
    if (!out || svc_nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = FAV_ICON_BYTES;
    bool ok = (svc_nvs_get_blob(h, k, out, &sz) == ESP_OK && sz == FAV_ICON_BYTES);
    svc_nvs_close(h);
    if (ok) return true;
    char lk[6]; fav_img_key_legacy(gi, lk, sizeof(lk));
    nvs_handle_t h2;
    if (svc_nvs_open(NVS_NS, NVS_READONLY, &h2) != ESP_OK) return false;
    sz = FAV_ICON_BYTES;
    ok = (svc_nvs_get_blob(h2, lk, out, &sz) == ESP_OK && sz == FAV_ICON_BYTES);
    svc_nvs_close(h2);
    return ok;
}
static void fav_icon_nvs_erase(int gi) {
    char k[10], lk[6];
    fav_img_key(gi, k, sizeof(k));
    fav_img_key_legacy(gi, lk, sizeof(lk));
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_erase_key(h, k);
    svc_nvs_erase_key(h, lk);
    svc_nvs_commit(h); svc_nvs_close(h);
}

/* ---- 对外接口 ---- */
static bool fav_icon_get(int gi, uint8_t *out) {
    if (!out) return false;
    if (fav_tf_ok() && fav_icon_tf_get(gi, out)) return true;
    if (fav_ic_migrated()) return false;      /* 迁移过 ⇒ 只认 TF, 绝不回读 NVS */
    return fav_icon_nvs_get(gi, out);         /* 还没迁移过 ⇒ 用旧 NVS */
}
static bool fav_icon_set(int gi, const uint8_t *bmp) {
    if (!bmp) return false;
    if (!fav_tf_ok()) {                       /* 无卡: 明确失败, 不静默当成功 */
        ESP_LOGW("FAV", "TF 卡不可用, 图标未保存 (item %d)", gi);
        return false;
    }
    return fav_icon_tf_set(gi, bmp);
}
static void fav_icon_del(int gi) {
    fav_icon_tf_del(gi);
    if (!fav_ic_migrated()) fav_icon_nvs_erase(gi);   /* 迁移前顺手清旧键, 免得之后读回来 */
}

/* ---- 一次性迁移: NVS → TF。全部成功才写标记；失败保留旧数据下次重试 ---- */
static void fav_icons_migrate(void) {
    if (!fav_tf_ok() || fav_ic_migrated()) return;
    uint8_t b[FAV_ICON_BYTES];
    int moved = 0, fail = 0;
    for (int i = 0; i < FAV_MAX; i++) {
        if (!fav_icon_nvs_get(i, b)) continue;
        if (fav_icon_tf_set(i, b)) moved++;
        else if (++fail >= 4) break;          /* 明显写不进去就停手, 别把 256 次全试一遍 */
    }
    if (fail) { ESP_LOGW("FAV", "图标迁移中断(已迁 %d, 失败 %d), 下次重试", moved, fail); return; }

    /* ★ 收尾核查: 两个标记都写, 且**先写 NVS 那个** —— 卡拔掉后只有 NVS 标记还认得,
     *   文件标记在没卡时 stat 不到。任一写失败都记日志：
     *   真正危险的是"NVS 标记没写成功", 那下次无卡启动会回退读旧 NVS 图标(复活)。
     *   写失败不影响本次会话(s_ic_mig=1), 但下次开机可能退化 —— 所以必须留痕。 */
    esp_err_t me = ESP_FAIL;
    {
        nvs_handle_t h; uint8_t v = 1;
        if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            me = svc_nvs_set_u8(h, "imgmig", v);
            svc_nvs_commit(h); svc_nvs_close(h);
        }
    }
    bool fok = false;
    FILE *m = fopen(FAV_ICON_MARK, "wb");
    if (m) { fok = (fputs("1", m) >= 0); if (fclose(m) != 0) fok = false; }
    if (me != ESP_OK || !fok)
        ESP_LOGW("FAV", "图标迁移标记写入不全(NVS=%s 文件=%d)：拔卡后可能回退读旧图标",
                 esp_err_to_name(me), (int)fok);

    s_ic_mig = 1;
    if (moved) ESP_LOGI("FAV", "图标已迁移到 TF: %d 个", moved);
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

/* ★ R4: 只接受 `Authorization: Bearer <token>`。
 *   URL query 里的 t= 兼容分支已**整段删除** —— token 放 URL 会进浏览器历史/Referer/代理日志。
 *   前端(fav_web_page.h)已全部走请求头, 删除后没有遗留依赖。 */
static bool fav_web_authed(httpd_req_t *req)
{
    if (!s_web_tok[0]) return false;
    if (esp_timer_get_time() - s_web_tok_us > 30LL * 60 * 1000000) return false;
    char hv[64];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hv, sizeof(hv)) != ESP_OK) return false;
    const char *b = hv;
    if (strncmp(b, "Bearer ", 7) == 0) b += 7;
    else if (strncmp(b, "bearer ", 7) == 0) b += 7;      /* 头名大小写不敏感, 值这里宽容一点 */
    return strcmp(b, s_web_tok) == 0;
}

/* 严格整数解析: 只接受可选负号 + 纯数字, 其余一律失败。
 * ★ R4: 以前到处 atoi(), 传 "abc" 会静默变 0 → 命中第 0 条, 删错/改错东西。 */
static bool fav_atoi(const char *s0, int *out) {
    if (!s0 || !*s0) return false;
    const char *p = s0;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    if (*p < '0' || *p > '9') return false;
    long v = 0; int n = 0;
    while (*p >= '0' && *p <= '9') {
        if (++n > 6) return false;                       /* 6 位足够(索引/分类号), 超了当非法 */
        v = v * 10 + (*p - '0');
        p++;
    }
    if (*p) return false;
    *out = (int)(sign * v);
    return true;
}
/* 拿不到就给默认值(调用方传 -1 当"非法", 现有的 i>=0 判断自然拒掉) */
static int fav_atoi_or(const char *s, int def) { int v; return fav_atoi(s, &v) ? v : def; }

/* 取 query 里的整数参数, 缺省/非法都返回 false(不动 *out) */
static bool fav_qint(const char *qs, const char *key, int *out) {
    char v[12];
    if (!qs || httpd_query_key_value(qs, key, v, sizeof(v)) != ESP_OK) return false;
    return fav_atoi(v, out);
}
/* 读 POST body 到 buf（返回实际长度, 失败 -1） */
static int fav_read_body(httpd_req_t *req, char *buf, size_t cap) {
    if (!req || !buf || cap < 2) return -1;
    if (req->content_len <= 0 || req->content_len >= (int)cap) return -1;
    int got = httpd_req_recv(req, buf, cap - 1);
    if (got <= 0) return -1;
    buf[got] = 0;
    return got;
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
    char pin[24] = {0};
    /* ★ R4: PIN **只**从 POST body 读。URL query 分支已删（PIN 进 URL = 进历史/日志）。 */
    if (fav_read_body(req, pin, sizeof(pin)) <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return fav_web_send_json(req, "{\"ok\":false,\"err\":\"body\"}");
    }
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
        char ce[80], cb[96];
        fav_json_esc(ce, sizeof(ce), s_cats[i]);     /* ★ R4: 分类名必须转义(以前直出 ⇒ 带 " 就把 JSON 打断) */
        snprintf(cb, sizeof(cb), "%s\"%s\"", i ? "," : "", ce);
        if (httpd_resp_sendstr_chunk(req, cb) != ESP_OK) return ESP_FAIL;
    }
    httpd_resp_sendstr_chunk(req, "],\"items\":[");
    for (int i = 0; i < s_n; i++) {
        /* ★ C12 (2026-10-30): 原来把整条(含 10 组账号)拼进 768B 缓冲,
         *   10 组时约 1400B ⇒ 被 snprintf 截断 ⇒ 前端收到半截 JSON 解析失败。
         *   现在**逐段 chunked 发送**, 每段都很小, 结构上不可能截断。 */
        char b[512];
        char en[80], ed[160], ew[200];
        fav_json_esc(en, sizeof(en), s_fav[i].name);
        fav_json_esc(ed, sizeof(ed), s_fav[i].desc);
        fav_json_esc(ew, sizeof(ew), s_fav[i].url);
        int off = snprintf(b, sizeof(b),
                 "%s{\"id\":%d,\"cat\":%d,\"name\":\"%s\",\"desc\":\"%s\",\"url\":\"%s\",\"creds\":[",
                 i ? "," : "", i, s_fav[i].cat, en, ed, ew);
        (void)off;
        if (httpd_resp_sendstr_chunk(req, b) != ESP_OK) return ESP_FAIL;
        for (int c = 0; c < s_fav[i].nc && c < CR_MAX; c++) {
            char eu[96], one[160];
            fav_json_esc(eu, sizeof(eu), s_fav[i].cr[c].user);
            /* ★ C11: **不再下发密码**，只给"密码是否非空"位；真密码走 /api/credget */
            snprintf(one, sizeof(one), "%s{\"u\":\"%s\",\"hp\":%d}",
                     c ? "," : "", eu, (s_fav[i].cr[c].pass[0] ? 1 : 0));
            if (httpd_resp_sendstr_chunk(req, one) != ESP_OK) return ESP_FAIL;
        }
        if (httpd_resp_sendstr_chunk(req, "]}") != ESP_OK) return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(req, "]}") != ESP_OK) return ESP_FAIL;
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
        int c = fav_atoi_or(cat, 0); if (c < 0 || c >= s_cat_n) c = 0;
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
        if (!fav_save()) { ESP_LOGE("FAV", "保存失败, 本次改动未落盘"); }
        else { s_fcol_dirty = true; ok = 1; }
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
    int i = fav_atoi_or(id, -1), ok = 0;
    if (i >= 0 && i < s_n) {
        /* ★ 自定义图标按"条目序号"存 NVS(imgNN)，删条目必须同步左移，否则图标错位 */
        uint8_t tmp[FAV_ICON_BYTES];
        for (int k = i; k < s_n - 1; k++) {
            if (fav_icon_get(k + 1, tmp)) { if (!fav_icon_set(k, tmp)) ESP_LOGW("FAV","图标搬移失败 %d", k); }
            else                          fav_icon_del(k);
        }
        fav_icon_del(s_n - 1);
        for (int k = i; k < s_n - 1; k++) s_fav[k] = s_fav[k + 1];
        s_n--;
        if (!fav_save()) { ESP_LOGE("FAV", "保存失败, 本次改动未落盘"); }
        else { s_fcol_dirty = true; ok = 1; }
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
        int c = fav_atoi_or(id, -1);
        if (c >= 0 && c < s_cat_n) {
            for (int k = c; k < s_cat_n - 1; k++) memcpy(s_cats[k], s_cats[k + 1], CAT_SZ);
            s_cat_n--;
            for (int i = 0; i < s_n; i++) {   /* 分类整体左移, 被删分类条目并入首分类 */
                if (s_fav[i].cat == (uint8_t)c) s_fav[i].cat = 0;
                else if (s_fav[i].cat > (uint8_t)c) s_fav[i].cat--;
            }
            cats_save();
            if (!fav_save()) ESP_LOGE("FAV", "分类变更保存失败");
            rc = 0;
        }
    } else if (strcmp(op, "mov") == 0) {
        /* ★ 2026-10-20: 网页里拖动分类排序 —— 数组移动 + 每条记录的 cat 索引跟着重映射 */
        int a = fav_atoi_or(id, -1), b = fav_atoi_or(to, -1);
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
            cats_save();
            if (!fav_save()) ESP_LOGE("FAV", "分类变更保存失败");
            s_fcol_dirty = true;
            rc = 0;
        }
    } else if (strcmp(op, "ren") == 0) {
        int c = fav_atoi_or(id, -1);
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
    int i = fav_atoi_or(id, -1);
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
    int i = fav_atoi_or(id, -1);
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
    int i = fav_atoi_or(id, -1), c = fav_atoi_or(cat, 0), ok = 0;
    if (i >= 0 && i < s_n && c >= 0 && c < s_cat_n) {
        s_fav[i].cat = (uint8_t)c;
        if (!fav_save()) { ESP_LOGE("FAV", "保存失败, 本次改动未落盘"); }
        else { s_fcol_dirty = true; ok = 1; }
    }
    return fav_web_send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* GET /api/cred?op=add|upd|del&id=&c=&u=&p=  —— 账号级增改删 */
static esp_err_t fav_web_cred_handler(httpd_req_t *req)
{
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char q[512], op[8]={0}, u[48]={0}, p[48]={0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return fav_web_send_json(req, "{\"ok\":false}");
    httpd_query_key_value(q, "op", op, sizeof(op));
    int i = -1, ci = -1;
    fav_qint(q, "id", &i);
    fav_qint(q, "c",  &ci);
    /* ★ R4: 账号/密码**只**从 POST body 取（body 形如 u=..&p=..，URL 编码）。
     *   以前把密码拼在 query 里 ⇒ 明文密码进浏览器历史/代理日志。 */
    char body[320];
    int bl = fav_read_body(req, body, sizeof(body));
    if (bl > 0) {
        fav_qdec(body, "u", u, sizeof(u));
        fav_qdec(body, "p", p, sizeof(p));
    }
    if ((!strcmp(op, "add") || !strcmp(op, "upd")) && bl <= 0)
        return fav_web_send_json(req, "{\"ok\":false,\"err\":\"body\"}");
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
        if (rc == 0) { if (fav_save()) s_fcol_dirty = true; else { ESP_LOGE("FAV", "账号保存失败"); rc = -1; } }
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
    int i = fav_atoi_or(id, -1);
    if (i < 0 || i >= s_n) return fav_web_send_json(req, "{\"ok\":false}");
    if (del[0] == '1') { fav_icon_del(i); return fav_web_send_json(req, "{\"ok\":true}"); }

    char body[256] = {0};
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r <= 0) return fav_web_send_json(req, "{\"ok\":false}");
    uint8_t bmp[FAV_ICON_BYTES];
    int n = b64_decode(body, bmp, sizeof(bmp));
    if (n != FAV_ICON_BYTES) return fav_web_send_json(req, "{\"ok\":false,\"err\":\"len\"}");
    if (!fav_icon_set(i, bmp)) return fav_web_send_json(req, "{\"ok\":false,\"err\":\"sd\"}");
    return fav_web_send_json(req, "{\"ok\":true}");
}

/* 通配分发: 只注册 首页 + api 通配 两个 handler (摆脱 max_uri_handlers 限制) */
/* ★ C11 (2026-10-30): 单条密码按需读取（必须已登录）。
 *   /api/credget?i=<item>&c=<cred>  → {"ok":true,"p":"..."}
 *   前端只有在用户点"显示/编辑"时才调它，列表接口永远不批量下发密码。 */
static esp_err_t fav_web_credget_handler(httpd_req_t *req) {
    if (!fav_web_authed(req)) return fav_web_deny(req);
    char q[96];
    int i = -1, c = -1;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        fav_qint(q, "i", &i);
        fav_qint(q, "c", &c);
    }
    if (i < 0 || i >= s_n || c < 0 || c >= CR_MAX || c >= (int)s_fav[i].nc)
        return fav_web_send_json(req, "{\"ok\":false}");
    char eb[160], out[224];
    fav_json_esc(eb, sizeof(eb), s_fav[i].cr[c].pass);
    snprintf(out, sizeof(out), "{\"ok\":true,\"p\":\"%s\"}", eb);
    return fav_web_send_json(req, out);
}

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
    if (!strcmp(u, "/api/credget")) return fav_web_credget_handler(req);
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
    /* ★ 2026-10-28: 补返回值检查。建任务失败必须回滚 s_web_phase,
     *   否则它会永远停在 1 —— 该状态在 fav_web_start_* 里是"已在启动中"的判据,
     *   于是这个功能再也起不来(本文件 1549 行已踩过同类坑: phase==1 永久卡死)。 */
    if (xTaskCreate(fav_web_task, "favweb", 8192, NULL, 5, NULL) != pdPASS) {
        s_web_phase = 0;
        ESP_LOGE("FAV", "favweb 任务创建失败(AP 模式) ⇒ 收藏夹热点未启动");
        return;
    }
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
    /* ★ 2026-10-28: 同上, 失败回滚 phase, 避免"已在启动中"永久卡死。 */
    if (xTaskCreate(fav_web_task_lan, "favweb", 8192, NULL, 5, NULL) != pdPASS) {
        s_web_phase = 0;
        ESP_LOGE("FAV", "favweb 任务创建失败(debug/LAN) ⇒ 收藏夹服务未启动");
    }
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
    /* ★ 2026-10-28: 补返回值检查 + 完整回滚。
     *   失败时不但要清 phase(否则"已在启动中"永久卡死), 还要清 s_lan_toast_pending
     *   —— 那个标志由任务内部消费, 任务没起来就永远 pending。并且**提前 return**,
     *   不执行下面的 esp_wifi_set_ps(WIFI_PS_NONE): 服务没起就没必要把省电关掉。 */
    if (xTaskCreate(fav_web_task_lan, "favweb", 8192, NULL, 5, NULL) != pdPASS) {
        s_web_phase = 0;
        s_lan_toast_pending = false;
        ESP_LOGE("FAV", "favweb 任务创建失败(LAN) ⇒ 收藏夹服务未启动");
        return;
    }
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
    /* ★ C11: 服务停下即销毁会话，重新开放后旧 token 一律失效 */
    s_web_tok[0] = 0;
    s_web_tok_us = 0;
    s_web_open_req = false;
    s_lan_pending = false;
    s_lan_toast_pending = false;
    /* 恢复默认省电 (仅 STA 模式; AP 模式前面已 esp_wifi_stop) */
    if (s_web_mode == 1) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

/* ================= HID ================= */
/* ★★★ C4 (2026-10-30): 完整的 US 布局 ASCII → HID Usage 映射。
 *
 * 同时修掉一个**比"缺字符"更严重的老 bug**：
 *   原实现 `c>='0'&&c<='9'` 用 `30 + (c-'0')`，而 HID 里 '1' 才是 0x1E。
 *   ⇒ '0' 打成 '1'、'1' 打成 '2' … '9' 打成 '0'，**所有数字整体错一位**。
 *   密码里只要含数字就必然输错，且用户完全看不出来。
 *
 * 布局假设：**US 布局**（物理键位 → 字符的对应关系固定，主机侧若为 UK/DE/FR
 *   等布局，Shift 组合会打出别的字符）。这一点在界面上会提示用户。
 *
 * 返回 false = 该字符无法用键盘发送（调用方必须**先整串检查**，不能打到一半才发现）。 */
static bool hid_char_map(char c, uint8_t *mod, uint8_t *code) {
    uint8_t m = 0, k = 0;
    if (c >= 'a' && c <= 'z')      { k = (uint8_t)(0x04 + (c - 'a')); }
    else if (c >= 'A' && c <= 'Z') { k = (uint8_t)(0x04 + (c - 'A')); m = FAV_LSHIFT; }
    else if (c >= '1' && c <= '9') { k = (uint8_t)(0x1E + (c - '1')); }   /* 0x1E..0x26 */
    else if (c == '0')             { k = 0x27; }                          /* 0x27 = '0' */
    else switch (c) {
        case ' ':  k = 0x2C; break;
        case '\t': k = 0x2B; break;
        case '\n': k = 0x28; break;
        case '.':  k = 0x37; break;
        case ',':  k = 0x36; break;
        case '/':  k = 0x38; break;
        case '-':  k = 0x2D; break;
        case '=':  k = 0x2E; break;
        case ';':  k = 0x33; break;
        case '\'': k = 0x34; break;
        case '[':  k = 0x2F; break;
        case ']':  k = 0x30; break;
        case '\\': k = 0x31; break;
        case '`':  k = 0x35; break;
        /* ---- 以下为 Shift 组合（US 布局）---- */
        case '_':  k = 0x2D; m = FAV_LSHIFT; break;
        case '+':  k = 0x2E; m = FAV_LSHIFT; break;
        case '{':  k = 0x2F; m = FAV_LSHIFT; break;
        case '}':  k = 0x30; m = FAV_LSHIFT; break;
        case '|':  k = 0x31; m = FAV_LSHIFT; break;
        case ':':  k = 0x33; m = FAV_LSHIFT; break;
        case '"':  k = 0x34; m = FAV_LSHIFT; break;
        case '<':  k = 0x36; m = FAV_LSHIFT; break;
        case '>':  k = 0x37; m = FAV_LSHIFT; break;
        case '?':  k = 0x38; m = FAV_LSHIFT; break;
        case '!':  k = 0x1E; m = FAV_LSHIFT; break;
        case '@':  k = 0x1F; m = FAV_LSHIFT; break;
        case '#':  k = 0x20; m = FAV_LSHIFT; break;
        case '$':  k = 0x21; m = FAV_LSHIFT; break;
        case '%':  k = 0x22; m = FAV_LSHIFT; break;
        case '^':  k = 0x23; m = FAV_LSHIFT; break;
        case '&':  k = 0x24; m = FAV_LSHIFT; break;
        case '*':  k = 0x25; m = FAV_LSHIFT; break;
        case '(':  k = 0x26; m = FAV_LSHIFT; break;
        case ')':  k = 0x27; m = FAV_LSHIFT; break;
        case '~':  k = 0x35; m = FAV_LSHIFT; break;
        default: return false;
    }
    *mod = m; *code = k;
    return true;
}
/* 整串可输入性检查：有一个字符发不出去就返回 false（绝不"打到一半才发现"） */
static bool hid_can_type(const char *s) {
    if (!s) return false;
    for (const char *p = s; *p; p++) { uint8_t m, k; if (!hid_char_map(*p, &m, &k)) return false; }
    return true;
}
static void send_char(char c){
    uint8_t mod = 0, code = 0;
    if (!hid_char_map(c, &mod, &code)) return;    /* 调用方已整串检查过, 这里只是兜底 */
    os_hid_key_tap(mod, code); vTaskDelay(3);
}
static void type_text(const char*s){ for(;s&&*s;s++){ send_char(*s); vTaskDelay(2);} }
/* ★ 2026-10-20 / 修正 2026-10-30: **只输入文本, 不发送 Tab、不发送 Enter**。
 *   旧注释写的"账号后打一个 Tab 跳密码框"从来没有实现过, 已删除, 以免误导。
 *   用户点"推账号"→ 自己 Tab 到密码框 → 再点"推密码"。 */
/* which: 0=只推账号 1=只推密码 */
static void do_input_part(ui_ctx_t*ctx,int gi,int ci,int pass){
    if(gi<0||gi>=s_n||ci<0||ci>=s_fav[gi].nc) return;
    const char *txt = pass ? s_fav[gi].cr[ci].pass : s_fav[gi].cr[ci].user;
    if (!hid_can_type(txt)) {                    /* ★ C4: 先整串检查, 不静默丢掉字符 */
        os_dialog_toast(ctx, "\xE5\x90\xAB\xE6\x97\xA0\xE6\xB3\x95\xE8\xBE\x93\xE5\x85\xA5\xE7\x9A\x84\xE5\xAD\x97\xE7\xAC\xA6\x28\xE4\xBB\x85\xE6\x94\xAF\xE6\x8C\x81\x41\x53\x43\x49\x49\x29");              /* 含无法输入的字符 */
        return;
    }
    if(!fav_hid_ensure()){ os_dialog_toast(ctx, "USB \xe9\x94\xae\xe9\xbc\xa0\xe6\x9c\xaa\xe5\xb0\xb1\xe7\xbb\xaa"); return; }
    if (!s_us_hint_shown) { s_us_hint_shown = true; os_dialog_toast(ctx, "\xE9\x94\xAE\xE7\x9B\x98\xE6\x8C\x89\x55\x53\xE5\xB8\x83\xE5\xB1\x80\xE8\xBE\x93\xE5\x85\xA5"); }
    type_text(txt);
}
/* 推送网址 (不回车) */
static void do_input_url(ui_ctx_t*ctx,int gi){
    if(gi<0||gi>=s_n) return;
    if (!hid_can_type(s_fav[gi].url)) {
        os_dialog_toast(ctx, "\xE5\x90\xAB\xE6\x97\xA0\xE6\xB3\x95\xE8\xBE\x93\xE5\x85\xA5\xE7\x9A\x84\xE5\xAD\x97\xE7\xAC\xA6\x28\xE4\xBB\x85\xE6\x94\xAF\xE6\x8C\x81\x41\x53\x43\x49\x49\x29");
        return;
    }
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

/* ==================== C1: 备份口令输入模态 ====================
 * 复用系统 52 键虚拟键盘（kbd_std）。口令与设备 PIN 完全独立，只在本会话内存里，
 * 进页面/退出都会清零，绝不落盘。 */
static int   s_pw_mode = 0;        /* 0=关 1=导出 2=导入 */
static char  s_pw_tmp[33];
static int   s_pw_len = 0;
static char  s_pw_pend[96];        /* 导入时要处理的文件 */
static int   s_pw_press = -1;

static void pw_open(int mode, const char *pend) {
    s_pw_mode = mode; s_pw_len = 0; s_pw_tmp[0] = 0; s_pw_press = -1;
    if (pend) snprintf(s_pw_pend, sizeof(s_pw_pend), "%s", pend);
    else      s_pw_pend[0] = 0;
}

static void pw_render(st7305_handle_t *l) {
    st7305_clear(l, ST7305_COLOR_WHITE);
    int base = S_H - KBD_H;
    draw_text(l, 8, 6,  (s_pw_mode == 1) ? "\xE8\xAE\xBE\xE7\xBD\xAE\xE5\xA4\x87\xE4\xBB\xBD\xE5\x8F\xA3\xE4\xBB\xA4\x28\xE5\xAF\xBC\xE5\x87\xBA\x29" : "\xE8\xBE\x93\xE5\x85\xA5\xE5\xA4\x87\xE4\xBB\xBD\xE5\x8F\xA3\xE4\xBB\xA4\x28\xE5\xAF\xBC\xE5\x85\xA5\x29", false);
    char mask[40];
    int n = s_pw_len > 32 ? 32 : s_pw_len;
    for (int i = 0; i < n; i++) mask[i] = '*';
    mask[n] = 0;
    if (n == 0) snprintf(mask, sizeof(mask), "%s", "\xE8\x87\xB3\xE5\xB0\x91\x34\xE4\xBD\x8D\xEF\xBC\x8C\xE4\xB8\x8E\xE8\xAE\xBE\xE5\xA4\x87\x50\x49\x4E\xE6\x97\xA0\xE5\x85\xB3");
    draw_text(l, 8, 40, mask, false);
    draw_text(l, 8, 72, "\xE5\x9B\x9E\xE8\xBD\xA6\xE7\xA1\xAE\xE8\xAE\xA4\x20\x2F\x20\x45\x53\x43\xE5\x8F\x96\xE6\xB6\x88", false);
    kbd_draw(l, base, &kbd_std, s_pw_press);
}

static bool pw_touch(ui_ctx_t *ctx, int x, int y) {
    int base = S_H - KBD_H;
    int idx = kbd_hit(&kbd_std, base, x, y);
    if (idx < 0) return true;
    s_pw_press = idx;
    int act = kbd_press(&kbd_std, idx);
    if (act == KBD_ACT_CHAR) {
        char c = kbd_char(&kbd_std, idx);
        if (c && s_pw_len < 32) { s_pw_tmp[s_pw_len++] = c; s_pw_tmp[s_pw_len] = 0; }
    } else if (act == KBD_ACT_BKSP) {
        if (s_pw_len > 0) s_pw_tmp[--s_pw_len] = 0;
    } else if (act == KBD_ACT_ESC) {
        s_pw_mode = 0; s_pw_len = 0; s_pw_tmp[0] = 0;
        redraw(ctx); return true;
    } else if (act == KBD_ACT_ENTER) {
        if (s_pw_len < 4) { os_dialog_toast(ctx, "\xE5\x8F\xA3\xE4\xBB\xA4\xE8\x87\xB3\xE5\xB0\x91\x34\xE4\xBD\x8D"); redraw(ctx); return true; }
        snprintf(s_bk_pw, sizeof(s_bk_pw), "%s", s_pw_tmp);
        int mode = s_pw_mode;
        s_pw_mode = 0; s_pw_len = 0; s_pw_tmp[0] = 0;
        if (mode == 1) {
            char path[96];
            if (!fav_bak_new_path(path, sizeof(path))) {
                os_dialog_toast(ctx, "\xE5\xA4\x87\xE4\xBB\xBD\xE7\x9B\xAE\xE5\xBD\x95\xE4\xB8\x8D\xE5\x8F\xAF\xE8\xAF\xBB");
                redraw(ctx); return true;
            }
            int r = fav_export_file(path);
            if (r == 0) {
                const char *nm = strrchr(path, '/');
                char msg[160];
                snprintf(msg, sizeof(msg), "%s %s", "\xE5\xB7\xB2\xE5\x8A\xA0\xE5\xAF\x86\xE5\xA4\x87\xE4\xBB\xBD\xE5\x88\xB0", nm ? nm + 1 : path);
                os_dialog_toast(ctx, msg);
            } else {
                os_dialog_toast(ctx, "\xE5\xAF\xBC\xE5\x87\xBA\xE5\xA4\xB1\xE8\xB4\xA5");
            }
        } else if (mode == 2) {
            int r = fav_import_file(s_pw_pend);
            fav_load();
            fav_load_enc();          /* ★ R4 P0-1: 必须立刻把密码也载回来, 否则下一次保存就是空密码覆盖 */
            if (s_enc_mismatch) os_dialog_toast(ctx, "\xE5\xAF\x86\xE7\xA0\x81\xE6\xAE\xB5\xE5\xBC\x82\xE5\xB8\xB8\x2C\xE5\xB7\xB2\xE6\x81\xA2\xE5\xA4\x8D\xE6\x9D\xA1\xE7\x9B\xAE\xE4\xBD\x86\xE5\xAF\x86\xE7\xA0\x81\xE9\x9C\x80\xE9\x87\x8D\xE6\x96\xB0\xE5\xAF\xBC\xE5\x85\xA5");
            s_cat = 0; s_scroll = 0; s_open = -1; s_drawer = -1;
            s_fcol_dirty = true; s_need_clear = true;
            if (r == 0)                   os_dialog_toast(ctx, "\xE5\xB7\xB2\xE6\x81\xA2\xE5\xA4\x8D\x28\xE5\x8A\xA0\xE5\xAF\x86\xE5\xA4\x87\xE4\xBB\xBD\x29");
            else if (r == -2 || r == -5)  os_dialog_toast(ctx, "\xE5\x8F\xA3\xE4\xBB\xA4\xE9\x94\x99\xE8\xAF\xAF\xE6\x88\x96\xE6\x96\x87\xE4\xBB\xB6\xE6\x8D\x9F\xE5\x9D\x8F");
            else if (r == -7)             os_dialog_toast(ctx, "\xE4\xBF\x9D\xE5\xAD\x98\xE5\xA4\xB1\xE8\xB4\xA5\x28NVS\xE7\xA9\xBA\xE9\x97\xB4\x3F\x29");
            else if (r == -6)             os_dialog_toast(ctx, "\xE5\xB7\xB2\xE6\x81\xA2\xE5\xA4\x8D\x28\xE9\x83\xA8\xE5\x88\x86\xE5\x9B\xBE\xE6\xA0\x87\xE6\x9C\xAA\xE5\x86\x99\xE5\x85\xA5\x29");
            else                          os_dialog_toast(ctx, "\xE5\xAF\xBC\xE5\x85\xA5\xE5\xA4\xB1\xE8\xB4\xA5");
        }
        redraw(ctx);
        return true;
    }
    redraw(ctx);
    return true;
}

static void render(ui_ctx_t*ctx){ st7305_handle_t*l=ctx->lcd; if(!l)return;
    if (s_pw_mode) { pw_render(l); return; }
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
    fav_load_enc();              /* ★ R4 P0-1: 同上 */
    if (s_enc_mismatch) os_dialog_toast(ctx, "\xE5\xAF\x86\xE7\xA0\x81\xE6\xAE\xB5\xE5\xBC\x82\xE5\xB8\xB8\x2C\xE5\xB7\xB2\xE6\x81\xA2\xE5\xA4\x8D\xE6\x9D\xA1\xE7\x9B\xAE\xE4\xBD\x86\xE5\xAF\x86\xE7\xA0\x81\xE9\x9C\x80\xE9\x87\x8D\xE6\x96\xB0\xE5\xAF\xBC\xE5\x85\xA5");
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
    case 0:   /* ★ C1: 导出前先设**独立备份口令**（不复用设备 PIN） */
        pw_open(1, NULL);
        break;
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
    fav_save_ui(ctx); redraw(ctx);
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
    /* ★ C2 (2026-10-30): 删条目必须同步图标 —— 原来只移数据不移图标,
     *   被删位置之后的条目图标集体错位一格（Web 端删除早已这么做了, 这里补齐）。 */
    {
        uint8_t tbmp[FAV_ICON_BYTES];
        for (int i = s_drawer; i < s_n - 1; i++) {
            if (fav_icon_get(i + 1, tbmp)) { if (!fav_icon_set(i, tbmp)) ESP_LOGW("FAV","图标搬移失败 %d", i); }
            else                           fav_icon_del(i);
        }
        fav_icon_del(s_n - 1);      /* 最后一个槽清掉 */
    }
    for(int i=s_drawer;i<s_n-1;i++) s_fav[i]=s_fav[i+1];
    memset(&s_fav[s_n-1],0,sizeof(fav_t));
    s_n--;
    s_drawer=-1; s_edit_focus=false; s_ed_scroll=0; s_url_scroll=0;
    s_ed_snap_ok=false; s_ed_track=-1; s_open=-1;
    int m = fav_max();
    if(s_sel >= m) s_sel = m > 0 ? m-1 : 0;
    s_fcol_dirty=true; s_need_clear=true;
    fav_save_ui(ctx); redraw(ctx);
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
    fav_save_ui(ctx);
    s_show_cats = false;
    redraw(ctx);
}

static bool fav_touch(ui_ctx_t*ctx,int x,int y){
    if (s_pw_mode) return pw_touch(ctx, x, y);
    if(!s_authed){
        applock_touch(ctx,x,y); if(!applock_active()){ s_authed=true; fav_load_enc();
            if (s_enc_mismatch) os_dialog_toast(ctx, "\xE5\xAF\x86\xE7\xA0\x81\xE6\xAE\xB5\xE4\xB8\x8E\xE6\x9D\xA1\xE7\x9B\xAE\xE4\xB8\x8D\xE5\x90\x8C\xE6\xAD\xA5\x2C\xE8\xAF\xB7\xE9\x87\x8D\xE6\x96\xB0\xE5\xAF\xBC\xE5\x85\xA5\xE5\xA4\x87\xE4\xBB\xBD"); }
        redraw(ctx); return true;
    }
    if(s_drawer>=0){ if(s_show_cats) icon_touch(ctx,x,y); else drawer_touch(ctx,x,y); return true; }
    main_touch(ctx,x,y); return true;
}

/* ===== 按键 ===== */
static void fav_action(ui_ctx_t*ctx,os_action_t a){
    if (s_pw_mode) {                       /* ★ C1: 口令模态独占按键 */
        if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) {
            s_pw_mode = 0; s_pw_len = 0; s_pw_tmp[0] = 0; redraw(ctx);
        }
        return;
    }
    if(!s_authed){
        /* ★ 通用应用锁: PIN 弹窗交给 applock 模块 (BACK=取消回上一级) */
        applock_action(ctx,a);
        if(!applock_active()){ s_authed=true; fav_load_enc();
            if (s_enc_mismatch) os_dialog_toast(ctx, "\xE5\xAF\x86\xE7\xA0\x81\xE6\xAE\xB5\xE4\xB8\x8E\xE6\x9D\xA1\xE7\x9B\xAE\xE4\xB8\x8D\xE5\x90\x8C\xE6\xAD\xA5\x2C\xE8\xAF\xB7\xE9\x87\x8D\xE6\x96\xB0\xE5\xAF\xBC\xE5\x85\xA5\xE5\xA4\x87\xE4\xBB\xBD"); }
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
    if (s_pw_mode) return;                 /* ★ C1: 口令模态不跑列表拖动 */
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
static bool s_hid_auto = false;
     /* 本次进页面由我们拉起的 USB 键鼠 (退出时关掉) */
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
    /* ★ 2026-10-28: 补返回值检查。本函数**本来就返回 bool**, 调用方靠它判断能否推送;
     *   创建失败就让 s_hid_want 回 false(fav_hid_task 首行也会检查这个标志),
     *   直接返回 false, 不再白等 1 秒轮询。 */
    if (xTaskCreate(fav_hid_task, "favhid", 4096, NULL, 4, NULL) != pdPASS) {
        s_hid_want = false;
        ESP_LOGE("FAV", "favhid 任务创建失败 ⇒ USB 键鼠推送不可用");
        return false;
    }
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
        /* ★ 2026-10-28: 补返回值检查。本分支当前由 FAV_HID_AUTO_START=0 关闭(死代码),
         *   但保留正确的失败处理: 失败清 s_hid_want, 让 fav_hid_ensure 下次还能再试。 */
        if (xTaskCreate(fav_hid_task, "favhid", 4096, NULL, 4, NULL) != pdPASS) {
            s_hid_want = false;
            ESP_LOGW("FAV", "favhid 任务创建失败(自动启动分支) ⇒ 本次未起 USB 键鼠");
        }
    }
}

static void fav_enter(ui_ctx_t*ctx){
    s_lan_pending = false; s_lan_toast_pending = false; s_fav_import_dlg = false;
    s_bk_pw[0] = 0;                        /* 每次进页面清空备份口令（不跨会话留存） */
    s_pw_mode = 0; s_pw_tmp[0] = 0; s_pw_len = 0; s_pw_press = -1;   /* ★ R4: 口令模态状态不跨会话 */
    {   /* ★ R4: 清理历史遗留的明文临时文件(<备份>.dec.tmp)。
         *   新版已改成内存解密, 不再产生; 这里只扫旧残留。
         *   两个目录都扫: 备份目录 + /sdcard 根(老位置 fav.bak 的临时文件在根下)。 */
        const char *dirs[2] = { FAV_BAK_DIR, "/sdcard" };
        for (int di = 0; di < 2; di++) {
            DIR *d = opendir(dirs[di]);
            if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                size_t l = strlen(e->d_name);
                if (l > 8 && strcmp(e->d_name + l - 8, ".dec.tmp") == 0) {
                    char p[96]; snprintf(p, sizeof(p), "%s/%s", dirs[di], e->d_name);
                    if (remove(p) == 0) ESP_LOGW("FAV", "已清理遗留明文临时文件 %s", p);
                }
            }
            closedir(d);
        }
    }
    s_ic_tf = -1; s_ic_mig = -1;      /* ★ R4: 每次进页面重新探测 TF/迁移状态 */
    fav_load(); if(applock_key()) fav_load_enc();   /* 已解锁才解出密码 */
    fav_icons_migrate();              /* ★ R4: 旧 NVS 图标 → TF（已迁移则直接返回） */
    if (s_enc_mismatch) os_dialog_toast(ctx, "\xE5\xAF\x86\xE7\xA0\x81\xE6\xAE\xB5\xE4\xB8\x8E\xE6\x9D\xA1\xE7\x9B\xAE\xE4\xB8\x8D\xE5\x90\x8C\xE6\xAD\xA5\x2C\xE8\xAF\xB7\xE9\x87\x8D\xE6\x96\xB0\xE5\xAF\xBC\xE5\x85\xA5\xE5\xA4\x87\xE4\xBB\xBD");
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
    if(s_n>0 && !fav_save()) ESP_LOGE("FAV", "退出时保存失败");
    /* ★ R4: 退出即清口令相关状态(含半截输入), 不留给下次进页面 */
    s_bk_pw[0] = 0; s_pw_tmp[0] = 0; s_pw_len = 0; s_pw_pend[0] = 0;
    s_pw_mode = 0; s_pw_press = -1;
    memset(s_encbuf, 0, sizeof(s_encbuf));
    s_enc_loaded = false;
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