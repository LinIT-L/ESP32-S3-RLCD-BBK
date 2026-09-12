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
#include "input.h"
#include "keyboard.h"
#include "usb_hid.h"
#include "keyvault.h"
#include "wifi_manager.h"   /* is_connected/is_connecting/get_ip */
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif_ip_addr.h"
#include "lwip/ip4_addr.h"
#include "dns_server.h"
#include "esp_attr.h"
#include "esp_random.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define S_W 400
#define S_H 300
#define HDR_H 26
#define LIST_X 112          /* 左栏右缘 (右栏起点) */

#define FAV_MAX 16
#define CR_MAX  4

#define NVS_NS  "linfav"
#define NVS_DATA "blob"
#define NVS_PIN "pinsset"
#define NVS_CATS "categories"
#define NVS_PINBLOB "pinblob"

typedef struct { char user[26]; char pass[26]; } cred_t;
typedef struct {
    uint8_t icon; uint8_t cat;
    char name[14]; char desc[20];
    uint8_t nc;
    cred_t cr[CR_MAX];
} fav_t;

#define CAT_MAX 8
#define CAT_SZ  25   /* 分类名最大字节数: 容纳 8 个汉字(UTF8 3B/字)+1, 原10B只能存3汉字会截断 */
static char s_cats[CAT_MAX][CAT_SZ] = {
    "\xe8\xae\xba\xe5\x9d\x9b\xe7\xbd\x91\xe7\xab\x99",
    "\xe5\xb7\xa5\xe4\xbd\x9c\xe6\x8a\x80\xe6\x9c\xaf",
    "\xe7\x9f\xa5\xe8\xaf\x86\xe5\xad\xa6\xe4\xb9\xa0",
    "\xe4\xb8\xaa\xe4\xba\xba\xe5\xa8\xb1\xe4\xb9\x90",
};
static int s_cat_n = 4;

static EXT_RAM_BSS_ATTR fav_t s_fav[FAV_MAX];   /* 收藏数据放 PSRAM, 不占核心内存 */
static int   s_n = 0;

static int s_cat = 0;          /* -1=设置(左栏首项) 0..=分类 */
static int  s_scroll = 0;
static int  s_sel = 0;
static int  s_drawer = -1;
static bool s_edit_focus = false;
static int  s_field = 0;
static char s_buf[28]; static int s_blen = 0;
static bool s_show_cats = false;   /* 抽屉内: true=图标选择 */

/* 左栏: 一比一复用阅读器 os_pane 模板 (文字+水管分割线+选中框, 行高32) */
static EXT_RAM_BSS_ATTR os_pane_t s_pane;
static int pane_mem_folders(os_pane_t *p) {
    for (int i = 0; i < s_cat_n && i < 24; i++)
        snprintf(p->folders[i], sizeof(p->folders[i]), "%s", s_cats[i]);
    p->folder_count = s_cat_n;
    p->hide_all = true;
    return s_cat_n;
}
/* 左栏显示序号 ↔ 状态: 0=设置, 1..=分类 */
static int pane_sel(void)      { return (s_cat < 0) ? 0 : s_cat + 1; }
static void pane_set_sel(int idx) { s_cat = (idx <= 0) ? -1 : idx - 1; }

static bool  s_authed = false;
static bool  s_pin_set = false;
static char  s_pin[20]; static int s_pin_len = 0;
static char  s_pin_again[20]; static int s_again_len = 0;
static int   s_pin_step = 0;
static int   s_cr, s_cc;

/* 系统"连接WiFi"流程 (局域网手柄同款: 扫描列表+密码键盘, 复用) */
extern void settings_wifi_connect_open(ui_ctx_t *ctx);

/* ---- 备份 / 网页配置 (热点 AP / IP STA) ---- */
#define FAV_BACKUP_PATH "/sdcard/fav.bak"
#define FAV_AP_SSID     "LinTOS-FAV"
static httpd_handle_t s_fav_httpd = NULL;
static void          *s_fav_dns   = NULL;
static bool           s_web_on    = false;   /* 网页服务启动标志 */
static int            s_web_phase = 0;        /* 0=空闲 1=启动中 2=已启动 3=失败 */
static int            s_web_mode  = 0;        /* 0=热点配置(AP) 1=IP配置(STA) */
static bool           s_lan_pending = false;  /* IP配置: 等待 WiFi 连接后自动启动服务 */
static bool           s_lan_toast_pending = false; /* IP服务启动完成后弹 IP 提示 */
static uint32_t       s_lan_t0 = 0;            /* 等待 WiFi 连接的超时起点 (60s 放弃) */
static bool           s_fav_import_dlg = false; /* 防重复弹导入确认框 */

/* poll 拖动 */
static bool s_press=false; static bool s_press_on=false; static int s_press_x=0,s_press_y=0; static uint32_t s_press_t=0;
static int  s_cat_drag_last=-1;

#define FAV_LSHIFT 0x02
#define FAV_ENTER  0x28
#define FAV_PLUS_TXT "\xef\xbc\x8b"   /* ＋ 全角加号 */

static void redraw(ui_ctx_t *ctx) { ctx->needs_redraw = true; }

/* 前向声明 */
static void pin_digit(char ch);
static void pin_del(void);
static void pin_ok(ui_ctx_t*ctx);
static void pin_touch(ui_ctx_t*ctx,int x,int y);
static void add_cred(ui_ctx_t*ctx,int gi);
static void apply_field(void);
static void edit_load(void);
static void del_cred(ui_ctx_t*ctx,int gi);

/* ================= NVS ================= */
static void cats_save(void){
    nvs_handle_t h; if(nvs_open(NVS_NS,NVS_READWRITE,&h)==ESP_OK){ nvs_set_blob(h,NVS_CATS,s_cats,sizeof(s_cats)); nvs_set_i32(h,"catn",s_cat_n); nvs_commit(h); nvs_close(h); }
}
static void cats_load(void){
    nvs_handle_t h; size_t sz=sizeof(s_cats); int32_t n=0;
    if(nvs_open(NVS_NS,NVS_READONLY,&h)==ESP_OK){
        /* 仅在新格式(blob 长度==CAT_SZ*CAT_MAX)时才采用; 旧格式(10B/项)已被截断, 忽略并回默认 */
        if(nvs_get_blob(h,NVS_CATS,s_cats,&sz)==ESP_OK && sz==sizeof(s_cats) &&
           nvs_get_i32(h,"catn",&n)==ESP_OK && n>0 && n<=CAT_MAX) s_cat_n=(int)n;
        nvs_close(h);
    }
    if(s_cat_n<=0||s_cat_n>CAT_MAX) s_cat_n=4;
}
static void fav_save(void){
    nvs_handle_t h; if(nvs_open(NVS_NS,NVS_READWRITE,&h)==ESP_OK){
        uint8_t blob[1+sizeof(fav_t)*FAV_MAX]; blob[0]=(uint8_t)s_n; fav_t*arr=(fav_t*)&blob[1];
        for(int i=0;i<s_n;i++) arr[i]=s_fav[i];
        nvs_set_blob(h,NVS_DATA,blob,1+sizeof(fav_t)*FAV_MAX); nvs_commit(h); nvs_close(h);
    }
}
static void fav_load(void){
    nvs_handle_t h; memset(s_fav,0,sizeof(s_fav)); s_n=0;
    size_t sz=1+sizeof(fav_t)*FAV_MAX; uint8_t*p=malloc(sz);
    if(p){
        if(nvs_open(NVS_NS,NVS_READONLY,&h)==ESP_OK){
            if(nvs_get_blob(h,NVS_DATA,p,&sz)==ESP_OK&&sz>=1){ int n=p[0]; if(n>FAV_MAX)n=FAV_MAX; fav_t*arr=(fav_t*)&p[1]; for(int i=0;i<n;i++) s_fav[i]=arr[i]; s_n=n; }
            nvs_close(h);
        }
        free(p);
    }
    cats_load();
    if(s_cat>=s_cat_n) s_cat=0;
}
static bool pinconf_load(void){ keyvault_init(); return keyvault_has_pin(); } /* 统一PIN: 复用系统 keyvault */
static void pinconf_save(bool v){ (void)v; }
static bool pin_verify(void){ return keyvault_verify_pin(s_pin) == 0; }        /* 统一验证 */
static void pin_store(void){ keyvault_set_pin(s_pin_again); }                  /* 首次设置统一写入 keyvault */

/* ================= 备份 (TF 卡) =================
 * 文件格式: magic "FAVEXP1"(7) + cat_n(u8) + 分类名(CAT_SZ B/个) + fav_n(u8)
 *           + fav_t 数组 + pin_set(u8) + pin_len(u8) + pinblob(20B) */
static int fav_export_file(void){
    FILE *f = fopen(FAV_BACKUP_PATH, "wb");
    if (!f) return -1;
    int rc = -1;
    if (fwrite("FAVEXP1", 1, 7, f) == 7) {
        uint8_t cn = (uint8_t)s_cat_n;
        if (fwrite(&cn, 1, 1, f) == 1) {
            int ok = 1;
            for (int i = 0; i < cn; i++)
                if (fwrite(s_cats[i], 1, CAT_SZ, f) != CAT_SZ) { ok = 0; break; }
            if (ok) {
                uint8_t fn = (uint8_t)s_n;
                if (fwrite(&fn, 1, 1, f) == 1 &&
                    (fn == 0 || fwrite(s_fav, sizeof(fav_t), fn, f) == (size_t)fn)) {
                    uint8_t pb = 0, pl = 0, pinblob[20] = {0}; /* PIN 归系统 keyvault, 不随备份迁移 */
                    if (fwrite(&pb, 1, 1, f) == 1 && fwrite(&pl, 1, 1, f) == 1 &&
                        fwrite(pinblob, 1, 20, f) == 20) rc = 0;
                }
            }
        }
    }
    fclose(f);
    if (rc != 0) remove(FAV_BACKUP_PATH);
    return rc;
}
static int fav_import_file(void){
    FILE *f = fopen(FAV_BACKUP_PATH, "rb");
    if (!f) return -1;
    int rc = -1;
    char magic[8] = {0};
    uint8_t cn = 0, fn = 0, pb = 0, pl = 0, pinblob[20] = {0};
    fav_t tmp[FAV_MAX];
    char cats[CAT_MAX][CAT_SZ];
    memset(tmp, 0, sizeof(tmp)); memset(cats, 0, sizeof(cats));
    if (fread(magic, 1, 7, f) == 7 && memcmp(magic, "FAVEXP1", 7) == 0 &&
        fread(&cn, 1, 1, f) == 1 && cn <= CAT_MAX &&
        (cn == 0 || fread(cats, 1, CAT_SZ, f) == (size_t)cn * CAT_SZ) &&
        fread(&fn, 1, 1, f) == 1 && fn <= FAV_MAX &&
        (fn == 0 || fread(tmp, sizeof(fav_t), fn, f) == (size_t)fn) &&
        fread(&pb, 1, 1, f) == 1 && fread(&pl, 1, 1, f) == 1 && pl <= 20 &&
        fread(pinblob, 1, 20, f) == 20) {
        memcpy(s_fav, tmp, sizeof(tmp)); s_n = fn;
        memcpy(s_cats, cats, sizeof(cats)); s_cat_n = cn;
        /* PIN 归系统 keyvault(统一配置), 备份仅恢复收藏与分类, 不恢复 PIN */
        fav_save(); cats_save();
        rc = 0;
    }
    fclose(f);
    return rc;
}

/* ================= 网页配置 (热点 AP / IP STA) =================
 * 1:1 复用密钥管理器/局域网手柄模式: 未连WiFi → 系统"连接WiFi"流程 → 连接成功后
 * 启动服务 → toast IP 提示. 热点模式自建 "LinTOS-FAV" 热点 (有 PIN 则作 Wi-Fi 密码). */
static const char *fav_web_html(void)
{
    return
    "<!doctype html><html><head><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{margin:0;font-family:sans-serif;background:#0f1115;color:#e6e6e6}*{box-sizing:border-box}"
    ".w{max-width:520px;margin:0 auto;padding:14px}h2{margin:.2em 0}input,select,button{padding:8px;margin:2px;font-size:15px;border-radius:6px;border:0}"
    "input,select{background:#22262e;color:#eee;flex:1;min-width:0}button{background:#2f6fdf;color:#fff;cursor:pointer}button.g{background:#444}"
    "button.r{background:#c0392b}.row{display:flex;gap:4px;margin:4px 0;align-items:center}.item{background:#1a1e26;padding:8px;border-radius:8px;margin:6px 0}"
    "b{color:#9fd0ff}small{color:#9aa}</style></head><body><div class='w'>"
    "<h2>收藏夹管理</h2><small id=st></small>"
    "<div class='row'>分类:<select id=cat></select><button class=g onclick=addcat()>+分类</button>"
    "<button class=g onclick=delcat()>-分类</button><button onclick=rencat()>改名</button></div>"
    "<div id=f><div class='row'>名称<input id=n placeholder='名称'></div>"
    "<div class='row'>介绍<input id=d placeholder='介绍'></div>"
    "<div class='row'>账号<input id=u placeholder='账号'></div>"
    "<div class='row'>密码<input id=p placeholder='密码'><button onclick=add()>添加</button></div></div>"
    "<div id=list></div></div>"
    "<script>const $=id=>document.getElementById(id),E=encodeURIComponent;"
    "async function api(u){const r=await fetch(u);return r.json()}async function load(){"
    "const d=await api('/api/list');$('cat').innerHTML=d.cats.map((c,i)=>'<option value='+i+'>'+c+'</option>').join('');"
    "$('list').innerHTML=d.items.map(x=>'<div class=item><b>'+x.name+'</b> · '+x.desc+'<br>账号: '+x.user+'<br>密码: '+x.pass"
    "+'<button class=r onclick=del('+x.id+')>删除</button></div>').join('')||'<small>空</small>';$('st').textContent='共 '+d.items.length+' 条'}"
    "async function add(){await api('/api/add?cat='+$('cat').value+'&n='+E($('n').value)+'&d='+E($('d').value)+'&u='+E($('u').value)+'&p='+E($('p').value));load()}"
    "async function del(id){await api('/api/del?id='+id);load()}"
    "async function addcat(){let n=prompt('新分类名');if(n)await api('/api/cat?op=add&n='+E(n)),load()}"
    "async function delcat(){await api('/api/cat?op=del&id='+$('cat').value);load()}"
    "async function rencat(){let n=prompt('新名', $('cat').selectedOptions[0].text);if(n)await api('/api/cat?op=ren&id='+$('cat').value+'&n='+E(n)),load()}"
    "load()</script></body></html>";
}

static esp_err_t fav_web_send_json(httpd_req_t *req, const char *s)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, s);
    return ESP_OK;
}
static esp_err_t fav_web_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, fav_web_html(), HTTPD_RESP_USE_STRLEN);
}
static esp_err_t fav_web_list_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"cats\":[");
    for (int i = 0; i < s_cat_n; i++) {
        if (i) httpd_resp_sendstr(req, ",");
        char cb[48]; snprintf(cb, sizeof(cb), "\"%s\"", s_cats[i]);
        httpd_resp_sendstr(req, cb);
    }
    httpd_resp_sendstr(req, "],\"items\":[");
    for (int i = 0; i < s_n; i++) {
        if (i) httpd_resp_sendstr(req, ",");
        char b[512];
        snprintf(b, sizeof(b), "{\"id\":%d,\"cat\":%d,\"name\":\"%s\",\"desc\":\"%s\",\"user\":\"%s\",\"pass\":\"%s\"}",
                 i, s_fav[i].cat, s_fav[i].name, s_fav[i].desc,
                 s_fav[i].nc > 0 ? s_fav[i].cr[0].user : "",
                 s_fav[i].nc > 0 ? s_fav[i].cr[0].pass : "");
        httpd_resp_sendstr(req, b);
    }
    httpd_resp_sendstr(req, "]}");
    return ESP_OK;
}
static esp_err_t fav_web_add_handler(httpd_req_t *req)
{
    char qs[512], cat[8] = "0", n[32], d[32], u[32], p[32];
    n[0] = d[0] = u[0] = p[0] = 0;
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) return fav_web_send_json(req, "{\"ok\":false}");
    httpd_query_key_value(qs, "cat", cat, sizeof(cat));
    httpd_query_key_value(qs, "n", n, sizeof(n));
    httpd_query_key_value(qs, "d", d, sizeof(d));
    httpd_query_key_value(qs, "u", u, sizeof(u));
    httpd_query_key_value(qs, "p", p, sizeof(p));
    int ok = 0;
    if (n[0] && s_n < FAV_MAX) {
        int c = atoi(cat); if (c < 0 || c >= s_cat_n) c = 0;
        fav_t *f = &s_fav[s_n];
        memset(f, 0, sizeof(fav_t));
        f->cat = (uint8_t)c;
        snprintf(f->name, sizeof(f->name), "%s", n);
        snprintf(f->desc, sizeof(f->desc), "%s", d);
        if (u[0] || p[0]) {
            snprintf(f->cr[0].user, sizeof(f->cr[0].user), "%s", u);
            snprintf(f->cr[0].pass, sizeof(f->cr[0].pass), "%s", p);
            f->nc = 1;
        }
        s_n++;
        fav_save();
        ok = 1;
    }
    char out[32]; snprintf(out, sizeof(out), "{\"ok\":%s}", ok ? "true" : "false");
    return fav_web_send_json(req, out);
}
static esp_err_t fav_web_del_handler(httpd_req_t *req)
{
    char qs[64], id[8] = "0";
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK)
        httpd_query_key_value(qs, "id", id, sizeof(id));
    int i = atoi(id), ok = 0;
    if (i >= 0 && i < s_n) {
        for (int k = i; k < s_n - 1; k++) s_fav[k] = s_fav[k + 1];
        s_n--;
        fav_save();
        ok = 1;
    }
    char out[32]; snprintf(out, sizeof(out), "{\"ok\":%s}", ok ? "true" : "false");
    return fav_web_send_json(req, out);
}
static esp_err_t fav_web_cat_handler(httpd_req_t *req)
{
    char qs[128], op[8], id[8], n[40];
    op[0] = id[0] = n[0] = 0;
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) return fav_web_send_json(req, "{\"ok\":false}");
    httpd_query_key_value(qs, "op", op, sizeof(op));
    httpd_query_key_value(qs, "id", id, sizeof(id));
    httpd_query_key_value(qs, "n", n, sizeof(n));
    int rc = -1;
    if (strcmp(op, "add") == 0) {
        if (s_cat_n < CAT_MAX && n[0]) { snprintf(s_cats[s_cat_n], CAT_SZ, "%s", n); s_cat_n++; cats_save(); rc = 0; }
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
    } else if (strcmp(op, "ren") == 0) {
        int c = atoi(id);
        if (c >= 0 && c < s_cat_n && n[0]) { snprintf(s_cats[c], CAT_SZ, "%s", n); cats_save(); rc = 0; }
    }
    char out[32]; snprintf(out, sizeof(out), "{\"ok\":%s}", rc == 0 ? "true" : "false");
    return fav_web_send_json(req, out);
}

/* 热点接入: 统一 PIN 归 keyvault 且不暴露明文, 网页配置 AP 改为开放(WPA2 组网保底) */
static void fav_ap_password(char *out, size_t sz){ if (sz) out[0] = 0; }

static void fav_web_register_uris(httpd_handle_t h)
{
    httpd_uri_t u;
    u.user_ctx = NULL;
    u.method = HTTP_GET; u.uri = "/"; u.handler = fav_web_index_handler;
    httpd_register_uri_handler(h, &u);
    u.uri = "/api/list"; u.handler = fav_web_list_handler; httpd_register_uri_handler(h, &u);
    u.uri = "/api/add";  u.handler = fav_web_add_handler;  httpd_register_uri_handler(h, &u);
    u.uri = "/api/del";  u.handler = fav_web_del_handler;  httpd_register_uri_handler(h, &u);
    u.uri = "/api/cat";  u.handler = fav_web_cat_handler;  httpd_register_uri_handler(h, &u);
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
        cfg.lru_purge_enable = true; cfg.stack_size = 8192;
        if (httpd_start(&s_fav_httpd, &cfg) == ESP_OK) fav_web_register_uris(s_fav_httpd);
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
        cfg.lru_purge_enable = true; cfg.stack_size = 8192;
        if (httpd_start(&s_fav_httpd, &cfg) == ESP_OK) fav_web_register_uris(s_fav_httpd);
    }
    s_web_on = true;
    s_web_phase = (s_fav_httpd != NULL) ? 2 : 3;
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
static void fav_web_start_lan(ui_ctx_t *ctx)
{
    (void)ctx;
    if (s_web_on || s_web_phase == 1) return;
    s_web_mode = 1;                      /* IP配置 = STA 模式 */
    s_lan_pending = false;
    s_lan_toast_pending = true;
    s_web_phase = 1;
    xTaskCreate(fav_web_task_lan, "favweb", 4096, NULL, 5, NULL);
    /* 不登记资源管家: IP/局域网服务在 toast IP 后回主桌面仍须运行 (1:1 局域网手柄), 仅由设置项手动关闭 */
}
static void fav_web_stop(ui_ctx_t *ctx)
{
    (void)ctx;
    if (!s_web_on && s_web_phase == 0) return;
    if (s_fav_httpd) { httpd_stop(s_fav_httpd); s_fav_httpd = NULL; }
    if (s_web_mode == 0) {   /* 热点(AP)模式额外清理: DNS + 停WiFi(AP) + 状态栏NET图标 */
        if (s_fav_dns) { stop_dns_server(s_fav_dns); s_fav_dns = NULL; }
        esp_wifi_stop();
        os_ap_set(false);
    }
    s_web_on = false;
    s_web_phase = 0;
    s_lan_pending = false;
    s_lan_toast_pending = false;
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
    usb_hid_key_tap(mod,code); vTaskDelay(3);
}
static void type_text(const char*s){ for(;s&&*s;s++){ send_char(*s); vTaskDelay(2);} }
static void do_input(ui_ctx_t*ctx,int gi,int ci){
    if(gi<0||gi>=s_n||ci<0||ci>=s_fav[gi].nc) return;
    if(!usb_hid_connected()){ os_dialog_toast(ctx, "\xe8\xaf\xb7\xe5\x85\x88\xe5\xbc\x80\xe5\x90\xaf USB \xe9\x94\xae\xe9\xbc\xa0"); return; }
    type_text(s_fav[gi].cr[ci].user); usb_hid_key_tap(0,FAV_ENTER);
    type_text(s_fav[gi].cr[ci].pass); usb_hid_key_tap(0,FAV_ENTER);
    os_dialog_toast(ctx, "\xe5\xb7\xb2\xe8\xbe\x93\xe5\x85\xa5");
}

/* ================= 低层绘制 ================= */
static void dot(st7305_handle_t*l,int x,int y){ if(x>=0&&x<S_W&&y>=0&&y<S_H) st7305_draw_pixel(l,x,y,ST7305_COLOR_BLACK);}
static void hline(st7305_handle_t*l,int x0,int x1,int y){ for(int x=x0;x<=x1;x++) dot(l,x,y);}
static void vline(st7305_handle_t*l,int x,int y0,int y1){ for(int y=y0;y<=y1;y++) dot(l,x,y);}
static void rect_out(st7305_handle_t*l,int x0,int y0,int x1,int y1){ hline(l,x0,x1,y0);hline(l,x0,x1,y1);vline(l,x0,y0,y1);vline(l,x1,y0,y1);}
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

static void main_render(st7305_handle_t *l){
    fill_rect(l,0,0,S_W-1,S_H-1,ST7305_COLOR_WHITE);
    /* 顶部: 状态栏由系统叠加显示 (此处仅白底, 无标题/无下划线/无设置) */
    fill_rect(l,0,0,S_W-1,HDR_H-1,ST7305_COLOR_WHITE);
    /* 左栏: 一比一阅读器 os_pane 模板 (文字+水管分割线+选中框, 行高32) */
    s_pane.folder_count = s_cat_n;        /* 分类可增删, 每次同步 */
    s_pane.sel_folder   = pane_sel();
    os_pane_draw_left(l, &s_pane);
    /* 当前按住触点 (按住反黑, 松手恢复) */
    int tv=0, tx=-1, ty=-1;
    if (input_touch_now(&tx,&ty)) tv=1;
    /* 设置右栏: 各种设置项 */
    if (s_cat < 0) {
        const char *set_items[5] = {
            "\xe4\xbf\xae\xe6\x94\xb9 PIN",        /* 修改PIN */
            "\xe5\xaf\xbc\xe5\x87\xba\xe5\xa4\x87\xe4\xbb\xbd",  /* 导出备份 */
            "\xe5\xaf\xbc\xe5\x85\xa5\xe5\xa4\x87\xe4\xbb\xbd",  /* 导入备份 */
            "\xe7\x83\xad\xe7\x82\xb9\xe9\x85\x8d\xe7\xbd\xae",  /* 热点配置 */
            "IP\xe9\x85\x8d\xe7\xbd\xae",                        /* IP配置 */
        };
        int y = HDR_H + 4 - s_scroll;
        for (int i = 0; i < 5; i++) {
            if (y + 40 >= HDR_H && y <= S_H - 10) {
                bool sel = (i == s_sel) || (tv && ty >= y && ty < y + 40 && tx >= LIST_X + 2 && tx <= S_W - 2);
                fill_rect(l, LIST_X + 2, y, S_W - 2, y + 40 - 1, sel ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
                draw_text(l, LIST_X + 14, y + 8, set_items[i], sel);
            }
            y += 40;
        }
        return;
    }
    /* 右栏: 收藏行高 52, 按住反黑 */
    int nF=fav_max(), y=HDR_H+2-s_scroll;
    for(int f=0; f<nF; f++){
        int gi=-1; fav_gi(f,&gi); if(gi<0) break;
        fav_t*fv=&s_fav[gi];
        if(y+52>=HDR_H && y<=S_H-10){
            bool hit = tv && ty>=y && ty<y+52 && tx>=LIST_X+2 && tx<=S_W-2;   /* 按住整行反黑 */
            bool hit_edit = tv && ty>=y+10 && ty<y+42 && tx>=S_W-45 && tx<=S_W-13; /* 按住"编"反黑 */
            fill_rect(l,LIST_X+2,y,S_W-2,y+52-1, hit ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
            if (!hit) draw_icon_bitmap_stretched(l,LIST_X+28,y+2,48,48,fv->icon);
            draw_text(l,LIST_X+58,y+2, fv->name[0]?fv->name:" ", hit);
            text16(l,LIST_X+58,y+26, fv->desc[0]?fv->desc:" ", hit);
            if (hit_edit) {
                fill_rect(l,S_W-45,y+10,S_W-13,y+42,ST7305_COLOR_BLACK);
                draw_text(l,S_W-41,y+15,"\xe7\xbc\x96",true);
            } else {
                fill_rect(l,S_W-45,y+10,S_W-13,y+42,ST7305_COLOR_WHITE);
                rect_out(l,S_W-45,y+10,S_W-13,y+42);
                draw_text(l,S_W-41,y+15,"\xe7\xbc\x96",false);
            }
        }
        y+=52;
    }
    /* 空分类: 「＋ 添加收藏」无边框, 点击弹下拉抽屉 (行高 52, 按住反黑) */
    if(nF==0){
        int ey=HDR_H+4-s_scroll;
        bool hit = tv && ty>=ey && ty<ey+52 && tx>=LIST_X+2 && tx<=S_W-2;
        fill_rect(l,LIST_X+2,ey,S_W-2,ey+51, hit ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
        draw_text(l,LIST_X+18,ey+12,FAV_PLUS_TXT,hit);
        draw_text(l,LIST_X+34,ey+12,"\xe6\xb7\xbb\xe5\x8a\xa0\xe6\x94\xb6\xe8\x97\x8f",hit);
    }
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
static void drawer_render(st7305_handle_t*l){
    fav_t*f=&s_fav[s_drawer];
    int dy1 = s_edit_focus ? (S_H - 5 * KBD_EDIT_ROW - 2) : DW_Y1;   /* 编辑时面板收窄让位键盘 */
    fill_rect(l,DW_X0,DW_Y0,DW_X1,dy1,ST7305_COLOR_WHITE);
    for(int k=0;k<2;k++){
        draw_hline(l,DW_X0+k,DW_X1-k,DW_Y0+k,ST7305_COLOR_BLACK);
        draw_hline(l,DW_X0+k,DW_X1-k,dy1-k,ST7305_COLOR_BLACK);
        draw_vline(l,DW_X0+k,DW_Y0+k,dy1-k,ST7305_COLOR_BLACK);
        draw_vline(l,DW_X1-k,DW_Y0+k,dy1-k,ST7305_COLOR_BLACK);
    }
    const char *title = f->name[0] ? "\xe7\xbc\x96\xe8\xbe\x91\xe6\x94\xb6\xe8\x97\x8f" : "\xe6\xb7\xbb\xe5\x8a\xa0\xe6\x94\xb6\xe8\x97\x8f"; /* 编辑收藏/添加收藏 */
    draw_text(l, DW_X0 + (DW_W - text_width(title)) / 2, DW_Y0 + 4, title, false);
    int iy = DW_Y0 + 34;
    draw_icon_bitmap_stretched(l, DW_X0 + 14, iy, 48, 48, f->icon);
    int lx = DW_X0 + 76;
    draw_text(l, lx, iy - 2, "\xe5\x90\x8d\xe7\xa7\xb0:", false);   /* 名称: */
    draw_text(l, lx + text_width("\xe5\x90\x8d\xe7\xa7\xb0:"), iy - 2,
              f->name[0] ? f->name : "\xe5\x8d\x95\xe5\x87\xbb\xe7\xbc\x96\xe8\xbe\x91",
              (s_edit_focus && s_field == 0));
    draw_text(l, lx, iy + 26, "\xe4\xbb\x8b\xe7\xbb\x8d:", false);  /* 介绍: */
    text16(l, lx + text16w("\xe4\xbb\x8b\xe7\xbb\x8d:"), iy + 28,
           f->desc[0] ? f->desc : "\xe5\x8d\x95\xe5\x87\xbb\xe7\xbc\x96\xe8\xbe\x91",
           (s_edit_focus && s_field == 1));
    draw_hline(l, DW_X0 + 4, DW_X1 - 4, iy + 64, ST7305_COLOR_BLACK);
    if (s_edit_focus) {
        int kb_y = (int)S_H - 5 * KBD_EDIT_ROW;
        /* 按压反馈: 按住键盘时对应键反黑 (松手恢复) */
        int press = -1, tx, ty;
        if (input_touch_now(&tx, &ty)) press = kbd_hit_edit(&kbd_edit, kb_y, tx, ty);
        kbd_draw_edit(l, kb_y, &kbd_edit, press);
        return;
    }
    /* 账号密码列表 */
    int yy = iy + 70;
    for (int c = 0; c < f->nc && yy + 44 <= dy1 - 42; c++) {
        char ua[36]; snprintf(ua,36,"\xe8\xb4\xa6\xe5\x8f\xb7:%s", f->cr[c].user); text16(l, DW_X0 + 10, yy, ua, false);      /* 账号: */
        char pa[36]; snprintf(pa,36,"\xe5\xaf\x86\xe7\xa0\x81:%s", f->cr[c].pass); text16(l, DW_X0 + 10, yy + 20, pa, false); /* 密码: */
        fill_rect(l, DW_X1 - 60, yy, DW_X1 - 6, yy + 18, ST7305_COLOR_BLACK); draw_text(l, DW_X1 - 56, yy + 1, "\xe8\xbe\x93\xe5\x87\xba", true);  /* 输出 */
        rect_out(l, DW_X1 - 60, yy + 20, DW_X1 - 6, yy + 38);       draw_text(l, DW_X1 - 56, yy + 21, "\xe5\x88\xa0", false);                   /* 删除 */
        yy += 46;
    }
    /* 底部按钮行 (贴面板底边, 左中右棋盘分隔) */
    int by = dy1 - 36;
    draw_hline(l, DW_X0 + 2, DW_X1 - 2, by - 2, ST7305_COLOR_BLACK);
    fill_rect(l, DW_X0 + 2, by, DW_X0 + 94, dy1 - 2, ST7305_COLOR_BLACK); draw_text(l, DW_X0 + 8, by + 5, "\xe6\xb7\xbb\xe5\x8a\xa0\xe8\xb4\xa6\xe5\x8f\xb7", true); /* 添加账号 */
    fill_rect(l, DW_X0 + 96, by, DW_X0 + 196, dy1 - 2, ST7305_COLOR_BLACK); draw_text(l, DW_X0 + 116, by + 5, "\xe7\xa1\xae\xe5\xae\x9a", true); /* 确定 */
    fill_rect(l, DW_X0 + 198, by, DW_X1 - 2, dy1 - 2, ST7305_COLOR_WHITE); rect_out(l, DW_X0 + 198, by, DW_X1 - 2, dy1 - 2); draw_text(l, DW_X0 + 218, by + 5, "\xe5\x8f\x96\xe6\xb6\x88", false); /* 取消 */
    draw_vline(l, DW_X0 + 95, by, dy1 - 2, ST7305_COLOR_BLACK);
    draw_vline(l, DW_X0 + 197, by, dy1 - 2, ST7305_COLOR_BLACK);
}

static void icon_picker_render(st7305_handle_t*l){
    fill_rect(l,0,0,S_W-1,S_H-1,ST7305_COLOR_WHITE);
    draw_text(l,6,2,"\xe9\x80\x89\xe6\x8b\xa9\xe5\x9b\xbe\xe6\xa0\x87",false);
    hline(l,0,S_W-1,HDR_H-1);
    int idx=0, y0=HDR_H+8;
    for(int r=0;r<4;r++) for(int c=0;c<6;c++){
        int cx=22+c*62, cy=y0+r*60;
        draw_icon_bitmap_stretched(l,cx+28,cy+24,48,48,idx);
        idx++;
    }
    fill_rect(l,140,S_H-34,260,S_H-4,ST7305_COLOR_BLACK); draw_text(l,168,S_H-30,"\xe7\xa1\xae\xe5\xae\x9a",true);
}

/* ================= PIN ================= */
static void pin_render(st7305_handle_t*l){
    fill_rect(l,0,0,S_W-1,S_H-1,ST7305_COLOR_WHITE);
    const char*ph = s_pin_set ? (s_pin_step==1?"\xe5\x86\x8d\xe6\xac\xa1\xe8\xbe\x93\xe5\x85\xa5":"\xe8\xae\xbe\xe7\xbd\xae PIN") : "\xe8\xaf\xb7\xe8\xbe\x93\xe5\x85\xa5 PIN";
    draw_text(l,6,40,ph,false);
    char dots[20]; int n=s_pin_len>18?18:s_pin_len; for(int i=0;i<n;i++)dots[i]='*'; dots[n]=0;
    if(n>0) draw_text_centered(l,74,dots,false);
    const int cell=86,gap=8,x0=36,y0=104,th=44;
    for(int r=0;r<3;r++)for(int c=0;c<3;c++){
        int x=x0+c*(cell+gap), y=y0+r*(th+gap);
        bool cur=(s_cr==r&&s_cc==c);
        rect_out(l,x,y,x+cell-1,y+th-1);
        if(cur) fill_rect(l,x+1,y+1,x+cell-2,y+th-2,ST7305_COLOR_BLACK);
        char buf[2]={0}; int d=r*3+c+1; if(d<=9)buf[0]=(char)('0'+d);
        draw_ascii(l,x+cell/2-4,y+th/2-4,(unsigned char)*buf,cur);
    }
    int bw=(S_W-2*x0-2*gap)/3;
    for(int c=0;c<3;c++){
        int x=x0+c*(bw+gap), y=y0+3*(th+gap), w=(c==1)?bw:(bw*2/3);
        bool cur=(s_cr==3&&s_cc==c);
        rect_out(l,x,y,x+w-1,y+th-1);
        if(cur) fill_rect(l,x+1,y+1,x+w-2,y+th-2,ST7305_COLOR_BLACK);
        const char*t=(c==0)?"\xe5\x88\xa0\xe9\x99\xa4":(c==2)?"\xe7\xa1\xae\xe5\xae\x9a":"0";
        draw_text_centered(l,y+(th-16)/2,t,cur);
    }
}

static void render(ui_ctx_t*ctx){ st7305_handle_t*l=ctx->lcd; if(!l)return;
    if(!s_authed){ pin_render(l); return; }
    if(s_drawer>=0){ if(s_show_cats) icon_picker_render(l); else drawer_render(l); return; }
    main_render(l);
}

/* ===== PIN 逻辑 ===== */
static void pin_digit(char ch){ if(s_pin_len<19){ s_pin[s_pin_len++]=ch; s_pin[s_pin_len]=0; } }
static void pin_del(void){ if(s_pin_len>0) s_pin[--s_pin_len]=0; }
static void pin_ok(ui_ctx_t*ctx){
    if(s_pin_set){
        if(s_pin_len<4){ os_dialog_toast(ctx,"\xe6\x9c\x80\xe5\xb0\x91\xe5\x9b\x9b\xe4\xbd\x8d"); return; }
        if(s_pin_step==0){ memcpy(s_pin_again,s_pin,sizeof(s_pin_again)); s_again_len=s_pin_len; s_pin[0]=0; s_pin_len=0; s_pin_step=1; redraw(ctx); return; }
        if(strcmp(s_pin,s_pin_again)!=0){ os_dialog_toast(ctx,"\xe4\xb8\xa4\xe6\xac\xa1\xe4\xb8\x8d\xe4\xb8\x80\xe8\x87\xb4"); s_pin[0]=0;s_pin_len=0;s_pin_step=0;s_pin_again[0]=0; redraw(ctx); return; }
        pin_store(); s_pin_set=false; s_authed=true; redraw(ctx); return;
    }
    if(pin_verify()){ s_authed=true; redraw(ctx); }
    else { os_dialog_toast(ctx,"PIN \xe9\x94\x99\xe8\xaf\xaf"); s_pin[0]=0;s_pin_len=0; redraw(ctx); }
}
static void pin_touch(ui_ctx_t*ctx,int x,int y){
    const int cell=86,gap=8,x0=36,y0=104,th=44,bw=(S_W-2*x0-2*gap)/3;
    for(int r=0;r<3;r++)for(int c=0;c<3;c++){ int cx=x0+c*(cell+gap), cy=y0+r*(th+gap);
        if(x>=cx&&x<=cx+cell-1&&y>=cy&&y<=cy+th-1){ int d=r*3+c+1; if(d<=9)pin_digit((char)('0'+d)); redraw(ctx); return; } }
    for(int c=0;c<3;c++){ int cx=x0+c*(bw+gap), cy=y0+3*(th+gap), cn=cx, w=(c==1)?bw:(bw*2/3);
        if(x>=cn&&x<=cn+w-1&&y>=cy&&y<=cy+th-1){ if(c==0)pin_del(); else if(c==2)pin_ok(ctx); redraw(ctx); return; } }
}

/* ===== 编辑抽屉逻辑 ===== */
static void edit_load(void){ fav_t*f=&s_fav[s_drawer]; if(!f)return; const char*src=(s_field==0)?f->name:f->desc; if(!src)src=""; snprintf(s_buf,sizeof(s_buf),"%s",src); s_blen=(int)strlen(s_buf); }
static void apply_field(void){ fav_t*f=&s_fav[s_drawer]; if(!f)return; if(s_field==0) snprintf(f->name,14,"%s",s_buf); else snprintf(f->desc,20,"%s",s_buf); fav_save(); }

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
static void del_cred(ui_ctx_t*ctx,int gi){
    if(gi<0||gi>=s_n) { return; }
    fav_t*f=&s_fav[gi];
    if(f->nc>0){ f->nc--; f->cr[f->nc].user[0]=0; f->cr[f->nc].pass[0]=0; fav_save(); redraw(ctx); }
}

/* ===== 触摸 ===== */
/* 设置项动作: 0=修改PIN 1=导出备份 2=导入备份 3=热点配置 4=IP配置 */
static void fav_import_cb(ui_ctx_t *ctx, int result, void *ud);   /* 前向 */
static void fav_setting_run(ui_ctx_t*ctx,int idx){
    switch (idx) {
    case 0:   /* 修改PIN: 进入 PIN 设置流程 (完成后回主界面) */
        s_pin_set=true; s_pin_step=0; s_pin_len=0; s_pin_again[0]=0; s_again_len=0; s_cr=0; s_cc=0;
        s_authed=false;
        break;
    case 1: { /* 导出备份 */
        int r = fav_export_file();
        os_dialog_toast(ctx, r == 0 ? "\xe5\xb7\xb2\xe5\xaf\xbc\xe5\x87\xba\xe5\x88\xb0TF\xe5\x8d\xa1" /* 已导出到TF卡 */
                                    : "\xe5\xaf\xbc\xe5\x87\xba\xe5\xa4\xb1\xe8\xb4\xa5");           /* 导出失败 */
        break;
    }
    case 2:   /* 导入备份: 覆盖当前数据, 需确认 */
        if (s_fav_import_dlg) break;
        s_fav_import_dlg = true;
        os_dialog_confirm_ex(ctx, "\xe5\xaf\xbc\xe5\x85\xa5\xe5\xa4\x87\xe4\xbb\xbd\xe5\xb0\x86\xe8\xa6\x86\xe7\x9b\x96\xe5\xbd\x93\xe5\x89\x8d\xe6\x94\xb6\xe8\x97\x8f\xe4\xb8\x8e PIN?", /* 导入备份将覆盖当前收藏与PIN? */
                             3000, 0, fav_import_cb, NULL);
        break;
    case 3:   /* 热点配置: toggle AP — 开/关都有 toast 提示 */
        if (s_web_on || s_web_phase == 1) {
            fav_web_stop(ctx);
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad");   /* 已关闭 */
        } else {
            fav_web_start_ap(ctx);
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\xbc\x80\xe5\x90\xaf\xe7\x83\xad\xe7\x82\xb9 LinTOS-FAV"); /* 已开启热点 LinTOS-FAV */
        }
        break;
    case 4:   /* IP配置: 1:1 复用局域网手柄流程 — 未连WiFi先要求联网输密码, 连接后启动服务+toast IP */
        if (s_web_on || s_web_phase == 1) {   /* 服务在跑(任意模式) → 关闭 */
            fav_web_stop(ctx);
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad");   /* 已关闭 */
        } else {
            if (wifi_manager_is_connected()) { fav_web_start_lan(ctx); break; }
            /* 未连接: 弹系统"连接WiFi"(扫描+密码键盘); 已保存配置自动重连 */
            if (!os_hw_is_active(OS_HW_WIFI)) os_hw_request(OS_HW_WIFI);
            s_lan_pending = true;
            s_lan_t0 = (uint32_t)(esp_timer_get_time() / 1000);
            settings_wifi_connect_open(ctx);
        }
        break;
    default: break;
    }
    redraw(ctx);
}
static void fav_import_cb(ui_ctx_t *ctx, int result, void *ud){
    (void)ud;
    s_fav_import_dlg = false;
    if (result != 0) return;   /* 取消 */
    int r = fav_import_file();
    os_dialog_toast(ctx, r == 0 ? "\xe5\xb7\xb2\xe5\xaf\xbc\xe5\x85\xa5" /* 已导入 */
                                : "\xe5\xaf\xbc\xe5\x85\xa5\xe5\xa4\xb1\xe8\xb4\xa5/\xe6\x97\xa0\xe5\xa4\x87\xe4\xbb\xbd"); /* 导入失败/无备份 */
    if (r == 0) {
        s_cat = 0; s_scroll = 0; s_sel = 0; s_drawer = -1;
        redraw(ctx);
    }
}
static void main_touch(ui_ctx_t*ctx,int x,int y){
    if(y<=HDR_H) return;   /* 状态栏区域, 不处理 */
    if(x<LIST_X){
        /* 左栏 os_pane 模板: 行高32, 滚动 folder_scroll; 0=设置 1..=分类 */
        int idx=s_pane.folder_scroll+(y-HDR_H)/32;
        if(idx<0) return;
        if(idx<s_cat_n+1){ pane_set_sel(idx); s_drawer=-1; s_scroll=0; redraw(ctx); }
        return;
    }
    if(s_cat<0){   /* 设置右栏 */
        int idx=(y-(HDR_H+4))/40;
        if(idx>=0&&idx<5){ s_sel=idx; fav_setting_run(ctx,idx); }
        return;
    }
    int nF=fav_max(), ey=HDR_H+2-s_scroll;
    if(nF==0 && y>=HDR_H+4-s_scroll && y<=HDR_H+54-s_scroll){ add_site(ctx); return; }   /* 「＋ 添加收藏」无边框行 */
    for(int f=0;f<nF;f++){ int gi=-1; fav_gi(f,&gi); if(gi<0)break;
        if(y>=ey&&y<ey+52){
            if(x>=S_W-45&&x<=S_W-13){ s_drawer=gi; s_field=0; s_show_cats=false; s_edit_focus=false; redraw(ctx); return; }
            return;
        }
        ey+=52;
    }
}
static void drawer_touch(ui_ctx_t*ctx,int x,int y){
    fav_t*f=&s_fav[s_drawer]; if(!f) return;
    if(s_edit_focus){
        int kb_y=(int)S_H-5*KBD_EDIT_ROW;
        if(y>=kb_y){
            int idx=kbd_hit_edit(&kbd_edit,kb_y,x,y);
            if(idx>=0){ kbd_t k = kbd_edit; int act=kbd_press(&k,idx);
                if(act==KBD_ACT_CHAR){ char c=kbd_char(&k,idx); if(c&&s_blen<27) s_buf[s_blen++]=c; s_buf[s_blen]=0; }
                else if(act==KBD_ACT_BKSP){ if(s_blen>0)s_buf[--s_blen]=0; }
                else if(act==KBD_ACT_SPACE){ if(s_blen<27)s_buf[s_blen++]=' '; s_buf[s_blen]=0; }
                else if(act==KBD_ACT_ENTER){ apply_field(); s_edit_focus=false; fav_save(); }
                redraw(ctx); return;
            }
            return;
        }
        return;
    }
    /* 图标区 → 选图标 */
    if(x>=DW_X0+14&&x<=DW_X0+62&&y>=DW_Y0+34&&y<=DW_Y0+82){ s_show_cats=true; redraw(ctx); return; }
    /* 名称/介绍行 → 编辑字段 */
    if(x>=DW_X0+76&&y>=DW_Y0+30&&y<=DW_Y0+88){
        s_field=(y<DW_Y0+58)?0:1; s_edit_focus=true; edit_load(); redraw(ctx); return;
    }
    /* 账号密码行: 输出/删除 */
    int yy=DW_Y0+104;
    for(int c=0;c<f->nc;c++){
        if(y>=yy&&y<yy+44){
            if(x>=DW_X1-60&&x<=DW_X1-6){ if(y>=yy+20) del_cred(ctx,s_drawer); else do_input(ctx,s_drawer,c); }
            return;
        }
        yy+=46;
    }
    /* 底部按钮行: 添加账号/确定/取消 */
    int by=DW_Y1-36;
    if(y>=by&&y<=DW_Y1-2){
        if(x>=DW_X0+2&&x<=DW_X0+94){ add_cred(ctx,s_drawer); return; }
        if(x>=DW_X0+96&&x<=DW_X0+196){ s_drawer=-1; fav_save(); redraw(ctx); return; }   /* 确定 */
        if(x>=DW_X0+198&&x<=DW_X1-2){  /* 取消: 刚添加未填内容则删除 */
            fav_t*g=&s_fav[s_drawer];
            if(g->name[0]==0&&g->nc==0){ for(int i=s_drawer;i<s_n-1;i++) s_fav[i]=s_fav[i+1]; s_n--; }
            fav_save(); s_drawer=-1; redraw(ctx); return;
        }
        return;
    }
}
static void icon_touch(ui_ctx_t*ctx,int x,int y){
    if(y>=S_H-34&&y<=S_H-4&&x>=140&&x<=260){ s_show_cats=false; redraw(ctx); return; }
    int y0=HDR_H+8, idx=0;
    for(int r=0;r<4;r++) for(int c=0;c<6;c++){
        int cx=22+c*62, cy=y0+r*60;
        if(x>=cx&&x<=cx+56&&y>=cy&&y<=cy+56){ s_fav[s_drawer].icon=(uint8_t)idx; fav_save(); s_show_cats=false; redraw(ctx); return; }
        idx++;
    }
}
static bool fav_touch(ui_ctx_t*ctx,int x,int y){
    if(!s_authed){ pin_touch(ctx,x,y); return true; }
    if(s_drawer>=0){ if(s_show_cats) icon_touch(ctx,x,y); else drawer_touch(ctx,x,y); return true; }
    main_touch(ctx,x,y); return true;
}

/* ===== 按键 ===== */
static void fav_action(ui_ctx_t*ctx,os_action_t a){
    if(!s_authed){
        if(a==OS_ACTION_UP&&s_cr>0)s_cr--; else if(a==OS_ACTION_DOWN&&s_cr<3)s_cr++;
        else if(a==OS_ACTION_LEFT&&s_cc>0)s_cc--; else if(a==OS_ACTION_RIGHT&&s_cc<2)s_cc++;
        else if(a==OS_ACTION_CONFIRM){ if(s_cr<3){int d=s_cr*3+s_cc+1; if(d<=9)pin_digit((char)('0'+d));} else { if(s_cc==0)pin_del(); else if(s_cc==2)pin_ok(ctx);} }
        else if(a==OS_ACTION_BACK){ if(s_pin_set){ s_authed=true; s_pin_set=false; redraw(ctx); } else os_pop(ctx); }
        redraw(ctx); return;
    }
    if(s_drawer>=0){ if(a==OS_ACTION_BACK){ s_drawer=-1; redraw(ctx); } return; }
    if(a==OS_ACTION_LEFT){ s_cat=(s_cat>-1)?s_cat-1:s_cat_n-1; s_drawer=-1; s_scroll=0; redraw(ctx); }
    else if(a==OS_ACTION_RIGHT){ s_cat=(s_cat<s_cat_n-1)?s_cat+1:-1; s_drawer=-1; s_scroll=0; redraw(ctx); }
    else if(a==OS_ACTION_DOWN&&(s_cat<0?s_sel<4:s_sel<fav_max()-1)){ s_sel++; redraw(ctx); }
    else if(a==OS_ACTION_UP&&s_sel>0){ s_sel--; redraw(ctx); }
    else if(a==OS_ACTION_CONFIRM){
        if(s_cat<0){ fav_setting_run(ctx,s_sel); }
        else { int gi=-1; fav_gi(s_sel,&gi); if(gi>=0){ s_drawer=gi; s_show_cats=false; redraw(ctx); } }
    }
    else if(a==OS_ACTION_BACK) os_pop(ctx);
}

/* ===== poll 拖动 ===== */
static void fav_poll(ui_ctx_t*ctx){
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
    if (s_lan_toast_pending && s_web_phase == 2) {
        s_lan_toast_pending = false;
        char ip[16] = "";
        wifi_manager_get_ip(ip, sizeof(ip));
        os_dialog_clear_all(ctx);
        os_pop_to_main(ctx);
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
            if(!s_press && (dy>6||dy<-6)){ s_press=true; if(tx<LIST_X){s_cat_drag_last=ty;} }
            if(s_press && tx<LIST_X){ int d=s_cat_drag_last-ty; s_cat_drag_last=ty; s_pane.folder_scroll+=d;
                if(s_pane.folder_scroll<0)s_pane.folder_scroll=0;
                int max=(s_cat_n+1)*32-(S_H-4-HDR_H); if(max<0)max=0;   /* 可视约8行 */
                if(s_pane.folder_scroll>max)s_pane.folder_scroll=max;
                redraw(ctx); }
        }
    } else { s_press_on=false; s_press=false; }
}

/* ===== 进/出 ===== */
static void fav_enter(ui_ctx_t*ctx){
    s_lan_pending = false; s_lan_toast_pending = false; s_fav_import_dlg = false;
    fav_load(); s_drawer=-1; s_scroll=0; s_sel=0; s_field=0; s_edit_focus=false; s_show_cats=false;
    s_cat=0;
    /* 左栏: 一比一阅读器 os_pane 模板 (内存数据源, 宽 110 容纳 4 字分类) */
    s_pane.list_y=HDR_H;
    s_pane.list_bottom=S_H-4;
    s_pane.left_w=110;
    s_pane.settings_label="\xe8\xae\xbe\xe7\xbd\xae";   /* 设置 (左栏首项) */
    s_pane.mem_folders=pane_mem_folders;
    os_pane_reset(&s_pane);
    s_pane.sel_folder=1;   /* 默认第一个分类 */
    bool pc=pinconf_load(); s_pin_set=(!pc); s_authed=false; s_pin_step=0; s_pin[0]=0; s_pin_len=0; s_cr=0; s_cc=0;
    redraw(ctx);
}
static void fav_exit(ui_ctx_t*ctx){
    if (s_web_mode == 0) fav_web_stop(ctx);   /* 退出应用自动关闭 热点(AP); IP/局域网服务保留 (1:1 手柄) */
    if(s_n>0)fav_save();
}

static os_module_t s_mod_fav={
    .name="fav", .page_id=OS_PAGE_FAV,
    .on_enter=fav_enter, .on_exit=fav_exit,
    .render=render, .action=fav_action, .touch=fav_touch, .poll=fav_poll,
    .fullscreen=false,   /* 顶部显示正常状态栏 */
};
void os_page_fav_register(void){ os_register(&s_mod_fav); }