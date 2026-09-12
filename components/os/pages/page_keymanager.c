/**
 * page_keymanager.c — 密钥管理器 页面模块 (硬件密码器).
 *
 * 主界面复用「阅读器菜单UI模板」(os_pane 双栏: 左分类/右密钥, 选中反色, 平滑滚动).
 * PIN 设置/解锁使用叠加在主列表之上的二级弹窗 (背景可见, 大九宫格数字键 1-9+0,
 * 底部三键 删除/取消/确定, 带粗边框, 支持手柄方向键黑块光标导航).
 *
 * 功能:
 *   - 首次进入: 在主列表上弹「设置Pin」二级弹窗, 两次输入确认 (提示精简)
 *   - PIN 锁 + 暴力破解退避 -> 输入 Pin 解锁后才能看/用密钥
 *   - 左栏分类(全部/网站/程序/游戏/自定义), 右栏该分类下密钥
 *   - 分类可 新增/改名/删除; 选择密钥确认 = USB HID 自动输入该密码
 */
#include "os.h"
#include "os_hw.h"      /* os_hw_is_active/request/release */
#include "os_pane.h"
#include "ui_common.h"
#include "input.h"
#include "keyvault.h"
#include "usb_hid.h"
#include "keyboard.h"
#include "bt_manager.h"
#include "wifi_manager.h"   /* is_connected/get_ip 等 */
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif_ip_addr.h"
#include "lwip/ip4_addr.h"
#include "dns_server.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define TAG "KEYM"
#define S_W   400
#define S_H   300
#define KM_KBD_Y  (S_H - KBD_H - 8)   /* 输入键盘贴底部 (KBD_H=145) */

typedef enum {
    KM_LIST,         /* 主列表 (os_pane 双栏) */
    KM_ADD_NAME,     /* 新增: 输入网站名 */
    KM_ADD_ACCOUNT,  /* 新增: 输入账号 */
    KM_ADD_SECRET,   /* 新增: 输入密码 */
    KM_DETAIL,       /* 条目详情 (显示账号/密码 + 自动输入/显示密码/删除) */
    KM_WEB,          /* 网页配置 (AP 热点 + http 数据库) */
    KM_CAT_MGR,      /* 分类管理列表 */
    KM_CAT_NAME,     /* 分类: 输入新名/改名 */
    KM_SEND          /* USB 输入中 */
} km_state_t;

static km_state_t s_state = KM_LIST;
static bool s_web_on;   /* 网页配置(AV web)启动标志, 定义见文件后部 */
static int  s_web_phase; /* 0=空闲 1=启动中 2=已启动 3=失败 */
static int  s_web_mode = 0;             /* 0=热点配置(AP) 1=IP配置(STA) */
static bool s_lan_pending = false;      /* IP配置: 等待 WiFi 连接后自动启动服务 */
static bool s_lan_toast_pending = false;/* IP服务启动完成后弹 IP 提示 */
static uint32_t s_lan_t0 = 0;           /* 等待 WiFi 连接的超时起点 (60s 放弃) */

/* 系统"连接WiFi"流程 (局域网手柄同款: 扫描列表+密码键盘, 复用) */
extern void settings_wifi_connect_open(ui_ctx_t *ctx);

/* ---- os_pane 适配 (双栏模板状态放 PSRAM, 省 ~57KB 内部 RAM, 与阅读器/游戏菜单一致) ---- */
static EXT_RAM_BSS_ATTR os_pane_t s_km_pane;
static EXT_RAM_BSS_ATTR int s_km_map[OS_PANE_ITEM_MAX];   /* pane item idx -> keyvault 全局条目序号 */

/* ---- PIN 弹窗 ---- */
static bool  s_pin_open = false;      /* 是否叠加 PIN 二级弹窗 */
static bool  s_pin_set  = false;      /* true=设置PIN false=解锁 */
static bool  s_pin_from_admin = false;/* true=设置->修改PIN 打开; 此时 BACK 关弹窗回列表(而非退出页面) */
static int   s_pin_step = 0;          /* 设置PIN: 0=第一次 1=再次确认 */
static char  s_pin[16];
static int   s_pin_len = 0;
static char  s_pin_again[16];
static int   s_again_len = 0;
static char  s_pin_plain[16];   /* 最近一次正确 PIN 明文 (供网页配置 AP 作 Wi-Fi 密码) */
static int   s_cursor_r = 0, s_cursor_c = 0; /* 九宫格光标 (无触屏时表示选中) */
static int   s_press_r = -1, s_press_c = -1;/* 当前按住的格 (触摸框: 按下瞬时黑底) */

static uint8_t s_key[KEYVAULT_KEY_LEN];   /* 派生密钥 (解锁后用) */

/* ---- 密钥数据缓存 (PSRAM, 省内部 RAM) ---- */
static EXT_RAM_BSS_ATTR char s_names[KEYVAULT_MAX_ENTRIES][64];
static EXT_RAM_BSS_ATTR int   s_cats[KEYVAULT_MAX_ENTRIES];
static int   s_total = 0;

/* ADD/CAT 输入 */
static char  s_buf[130];
static int   s_buf_len = 0;
static char  s_name_buf[60];   /* 网站名 */
static char  s_acct_buf[60];   /* 账号 */
static int   s_sel_gi = -1;    /* 详情态: 当前选中条目全局序号 */
static int   s_detail_act = 0; /* 详情底键 0=自动输入 1=删除 2=返回 */
static bool  s_detail_show = false; /* 详情密码: false=掩码* true=明文 */
static int   s_cat_manage_cursor = 0;
static int   s_cat_edit_idx = -1;

typedef struct { uint8_t kc; uint8_t mod; } kv_kc_t;
static const kv_kc_t s_asc[95] = {
#include "keymgr_hid.inc"
};

static void kv_kbd_str(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E) continue;
        const kv_kc_t *e = &s_asc[c - 0x20];
        usb_hid_key_tap(e->mod, e->kc);
        vTaskDelay(pdMS_TO_TICKS(3));
    }
}
static void redraw(ui_ctx_t *ctx) { ctx->needs_redraw = true; }
static void km_add_confirm(ui_ctx_t *ctx);
static void km_gen_secret(void);
static void km_web_start(ui_ctx_t *ctx);
static void km_web_stop(ui_ctx_t *ctx);
static void km_web_house_stop(void);
static void km_lan_start(ui_ctx_t *ctx);   /* IP配置(STA): 启动密钥服务, 见文件后部 */
static void km_detail_do(ui_ctx_t *ctx);
static void km_detail_del_cb(ui_ctx_t *ctx, int result, void *ud);
static void on_export_or_import_ex(ui_ctx_t *ctx, bool is_export);   /* 导出/导入 */
static void km_cat_manage_confirm(ui_ctx_t *ctx);   /* 分类管理确认(按键CONFIRM复用) */
static const char *km_cat_label(int idx) {
    const char *n = keyvault_cat_name(idx);
    return n ? n : "";
}

/* ---- 数据加载 ---- */
static void load_entries(void) {
    s_total = keyvault_list(s_names, s_cats, KEYVAULT_MAX_ENTRIES);
}

/* ---- os_pane 内存数据源回调 ---- */
static int km_pane_folders(os_pane_t *p) {
    p->hide_all = true;   /* "全部"作为 folders[0], 不用 os_pane 自动"全部" */
    int ncat = keyvault_cat_count();
    snprintf(p->folders[0], sizeof(p->folders[0]), "%s", "\xe5\x85\xa8\xe9\x83\xa8\xe8\xb4\xa6\xe5\x8f\xb7"); /* 全部账号 */
    for (int i = 0; i < ncat && (i + 1) < 24; i++) {
        const char *cn = keyvault_cat_name(i);
        snprintf(p->folders[i + 1], sizeof(p->folders[i + 1]), "%s", cn ? cn : "");
    }
    int total = 1 + ncat;
    if (total > 24) total = 24;
    p->folder_count = total;
    return total;
}
/* 按 UTF-8 整字边界把 s 拷到 out(≤maxb-1 字节), 避免截断中文切半成乱码 */
static size_t km_clip_utf8(const char *s, char *out, size_t maxb) {
    if (!s || !out || maxb == 0) return 0;
    size_t o = 0;
    while (s[o] && o + 1 < maxb) {
        uint8_t c = (uint8_t)s[o];
        size_t len = (c < 0x80) ? 1 :
                     ((c & 0xE0) == 0xC0) ? 2 :
                     ((c & 0xF0) == 0xE0) ? 3 :
                     ((c & 0xF8) == 0xF0) ? 4 : 1;
        if (o + len >= maxb) break;   /* 放不下整个字就停(不切半) */
        for (size_t i = 0; i < len; i++) out[o + i] = s[o + i];
        o += len;
    }
    out[o] = 0;
    return o;
}
static int km_pane_items(os_pane_t *p) {
    /* 左栏"设置"(sel_folder==0): 右栏走自定义设置列表, 而非内存条目数据源 */
    if (p->sel_folder == 0 && p->settings_label) {
        p->settings_mode = true;
        p->item_count = 0;
        return 0;
    }
    p->settings_mode = false;
    int cat = p->sel_folder - 2;   /* sel_folder: 0=设置, 1=全部, 2..=分类索引 */
    int n = 0;
    for (int i = 0; i < s_total && n < OS_PANE_ITEM_MAX; i++) {
        if (cat < 0 || s_cats[i] == cat) {
            char ac[20];
            /* UTF-8 整字边界裁剪: 避免 %.30s/%.12s 按字节截断把中文切半成乱码 */
            char nm[31], acb[13];
            km_clip_utf8(s_names[i], nm, sizeof(nm));
            if (keyvault_get_account(s_key, i, ac, sizeof(ac)) == 0 && ac[0]) {
                km_clip_utf8(ac, acb, sizeof(acb));
                snprintf(p->items[n], sizeof(p->items[n]), "%s\xc2\xb7%s", nm, acb); /* 网站名·账号 */
            } else {
                km_clip_utf8(s_names[i], nm, sizeof(nm));
                snprintf(p->items[n], sizeof(p->items[n]), "%s", nm);
            }
            s_km_map[n] = i;
            n++;
        }
    }
    p->item_count = n;
    return n;
}
static void km_pane_select(ui_ctx_t *ctx, os_pane_t *p,
                           const char *item_path, const char *item_name) {
    (void)item_path; (void)item_name;
    if (p->sel_item < 0 || p->sel_item >= p->item_count) return;
    s_sel_gi = s_km_map[p->sel_item];
    s_detail_act = 0; s_detail_show = false;
    s_state = KM_DETAIL; redraw(ctx);   /* 点条目 → 进详情 (显示密码/自动输入/删除) */
}
/* 左栏"设置"右栏渲染: 设置功能列表 */
/* V1.6.x: 蓝牙HID键盘↔手柄 二选一模式 (0=手柄/正常, 1=蓝牙键盘) */
static int  s_hid_ble = 0;
static bool s_bt_prev = false;   /* 切到蓝牙键盘前, 蓝牙是否原本启用 (切回时恢复) */
static void km_hidmode_load(void) {
    nvs_handle_t h;
    s_hid_ble = 0;
    if (nvs_open("hidmode", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "mode", (int32_t *)&s_hid_ble);
        nvs_close(h);
    }
}
static void km_hidmode_save(void) {
    nvs_handle_t h;
    if (nvs_open("hidmode", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "mode", s_hid_ble);
        nvs_commit(h);
        nvs_close(h);
    }
}
/* 设置"设置"左栏首项: 设置管理 */
static void km_settings_render(ui_ctx_t *ctx, os_pane_t *p, int rx0, int rx1,
                               int list_y, int list_bottom, int top_row, int off_mod) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    static const char *labels[5] = {
        "\xe9\x85\x8d\xe7\xbd\xae\xe5\xaf\xbc\xe5\x87\xba",  /* 配置导出 */
        "\xe9\x85\x8d\xe7\xbd\xae\xe5\xaf\xbc\xe5\x85\xa5",  /* 配置导入 */
        "\xe4\xbf\xae\xe6\x94\xb9\xe5\xaf\x86\xe7\xa0\x81",  /* 修改密码 */
        "\xe7\x83\xad\xe7\x82\xb9\xe9\x85\x8d\xe7\xbd\xae",  /* 热点配置 */
        "IP\xe9\x85\x8d\xe7\xbd\xae",                        /* IP配置 */
    };
    for (int i = 0; i < (int)(sizeof(labels) / sizeof(labels[0])); i++) {
        int idx = top_row + i;
        int ry = list_y + idx * 32 - off_mod;
        if (ry + 32 < list_y || ry >= list_bottom) continue;
        bool sel = (p->focus == 1 && idx == p->sel_item);
        if (sel) fill_rect(lcd, rx0, ry, rx1, ry + 30, ST7305_COLOR_BLACK);
        draw_text_centered(lcd, ry + 4, labels[idx], sel);
    }
}
static void km_settings_select(ui_ctx_t *ctx, os_pane_t *p, int item) {
    (void)p;
    switch (item) {
    case 0: on_export_or_import_ex(ctx, true); break;                                     /* 配置导出 */
    case 1: on_export_or_import_ex(ctx, false); break;                                    /* 配置导入 */
    case 2: /* 修改密码 */
        s_pin_from_admin = true;
        s_pin_open = true; s_pin_set = true;
        s_pin_step = 0; s_pin[0] = 0; s_pin_len = 0;
        s_pin_again[0] = 0; s_again_len = 0;
        s_cursor_r = 0; s_cursor_c = 0;
        redraw(ctx); break;
    case 3: /* 热点配置: toggle AP — 开/关都有 toast 提示, 不跳新页面 */
        if (s_web_on || s_web_phase == 1) {
            km_web_stop(ctx);
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad");   /* 已关闭 */
        } else {
            km_web_start(ctx);
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\xbc\x80\xe5\x90\xaf\xe7\x83\xad\xe7\x82\xb9 LinTOS-KV"); /* 已开启热点 LinTOS-KV */
        }
        redraw(ctx); break;
    case 4: /* IP配置: 1:1 复用局域网手柄流程 — 未连WiFi先要求联网输密码, 连接后启动服务+toast IP */
        if (s_web_on || s_web_phase == 1) {   /* 服务在跑(任意模式) → 关闭 */
            km_web_stop(ctx);
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad");   /* 已关闭 */
        } else {
            if (wifi_manager_is_connected()) { km_lan_start(ctx); break; }
            /* 未连接: 弹系统"连接WiFi"(扫描+密码键盘); 已保存配置自动重连 */
            if (!os_hw_is_active(OS_HW_WIFI)) os_hw_request(OS_HW_WIFI);
            s_lan_pending = true;
            s_lan_t0 = (uint32_t)(esp_timer_get_time() / 1000);
            settings_wifi_connect_open(ctx);
        }
        redraw(ctx); break;
    default: break;
    }
}

/* ---- PIN 弹窗: 九宫格几何 (相对弹窗窗口) ----
 * 布局: 标题 + 单一 PIN 输入框 + 九宫格 1-9(3行) + 底行 [删除][0][确定]
 * 删除/确定 放 0 左右, 与数字同尺寸, 宽度再收窄 1/3. */
#define KP_WIN_W   304
#define KP_WIN_X   ((S_W - KP_WIN_W) / 2)   /* 48 */
#define KP_WIN_Y   24                      /* 弹窗顶边贴状态栏底部(24), 状态栏下方 */
#define KP_TITLE_H 26
#define KP_PIN_Y   (KP_WIN_Y + KP_TITLE_H + 8)
#define KP_PIN_H   46
#define KP_TOP     (KP_PIN_Y + KP_PIN_H + 12)
#define KP_COL_W   88
#define KP_COL_X0  (KP_WIN_X + 20)
#define KP_COL_GAP 8
#define KP_ROW_H   36
#define KP_ROW_GAP 4
#define KP_WIN_H   (KP_TOP + 4 * (KP_ROW_H + KP_ROW_GAP) - KP_ROW_GAP + 8 - KP_WIN_Y) /* 数字3行+底行 */

static void kp_rect(int r, int c, int *x, int *y, int *w, int *h) {
    *x = KP_COL_X0 + c * (KP_COL_W + KP_COL_GAP);
    *y = KP_TOP + r * (KP_ROW_H + KP_ROW_GAP);
    *w = KP_COL_W; *h = KP_ROW_H;
}
/* 格子统一绘制: 上下居中按字体高 (ascii 数字 16 / 汉字 24) —— 根治选中黑底/文字错位 */
static void km_draw_cell(st7305_handle_t *lcd, int cx, int cy, int w, int h,
                         const char *text, bool cur, bool pressed, int fh) {
    /* 按钮交互统一规则: 有触屏→按下格子瞬时黑底/松手恢复(不做常驻选中);
     * 无触屏→黑底代表当前选中(cur). 框线恒显示. */
    bool touchdev = input_has_touch();
    bool active = touchdev ? pressed : cur;
    draw_rect_outline(lcd, cx, cy, cx + w - 1, cy + h - 1, ST7305_COLOR_BLACK);
    if (active) fill_rect(lcd, cx + 1, cy + 1, cx + w - 2, cy + h - 2, ST7305_COLOR_BLACK);
    draw_label_centered_at(lcd, cx + w / 2, cy + (h - fh) / 2, text, active);
}
static char kp_cell_char(int r, int c) {
    if (r < 0 || r >= 3) return 0;
    int d = r * 3 + c + 1;
    return (d >= 1 && d <= 9) ? (char)('0' + d) : 0;
}
static void kp_clamp_cursor(void) {
    if (s_cursor_r < 0) s_cursor_r = 0;
    if (s_cursor_r > 3) s_cursor_r = 3;
    if (s_cursor_c < 0) s_cursor_c = 0;
    if (s_cursor_c > 2) s_cursor_c = 2;
}

static void km_pin_digit(ui_ctx_t *ctx, char ch) {
    if (s_pin_len < 15) { s_pin[s_pin_len++] = ch; s_pin[s_pin_len] = 0; }   /* 不限制设置数量 */
    redraw(ctx);
}
static void km_pin_del(ui_ctx_t *ctx) {
    if (s_pin_len > 0) { s_pin[--s_pin_len] = 0; redraw(ctx); }
}
static void km_pin_submit(ui_ctx_t *ctx) {
    if (s_pin_set) {
        if (s_pin_len < 8) { os_dialog_toast(ctx, "最少要八位"); return; }   /* 至少 8 位 */
        if (s_pin_step == 0) {
            memcpy(s_pin_again, s_pin, sizeof(s_pin_again) - 1);
            s_pin_again[sizeof(s_pin_again) - 1] = '\0';
            s_again_len = s_pin_len; s_pin[0] = 0; s_pin_len = 0;
            s_pin_step = 1;
            redraw(ctx); return;   /* 标题显示"再次输入", 不弹 toast (避免压 dialog 页吞按键) */
        }
        if (strcmp(s_pin, s_pin_again) != 0) {
            os_dialog_toast(ctx, "两次不一致");
            s_pin[0] = 0; s_pin_len = 0; s_pin_step = 0;
            s_pin_again[0] = 0; s_again_len = 0;
            redraw(ctx); return;
        }
        keyvault_set_pin(s_pin_again);
        keyvault_unlock_secret(s_pin_again, s_key);
        strncpy(s_pin_plain, s_pin_again, sizeof(s_pin_plain) - 1); s_pin_plain[sizeof(s_pin_plain) - 1] = 0; /* 存明文供 AP 密码 */
    } else {
        if (s_pin_len == 0) return;
        int r = keyvault_verify_pin(s_pin);
        if (r == 0) {
            keyvault_unlock_secret(s_pin, s_key);
            strncpy(s_pin_plain, s_pin, sizeof(s_pin_plain) - 1); s_pin_plain[sizeof(s_pin_plain) - 1] = 0; /* 存明文供 AP 密码 */
        } else if (r == 1) { os_dialog_toast(ctx, "PIN 错误"); s_pin[0]=0; s_pin_len=0; redraw(ctx); return; }
        else if (r == 2) { os_dialog_toast(ctx, "已锁定, 请稍后"); s_pin[0]=0; s_pin_len=0; redraw(ctx); return; }
    }
    s_pin[0] = 0; s_pin_len = 0; s_pin_again[0] = 0; s_again_len = 0;
    s_pin_open = false; s_pin_step = 0;
    load_entries();
    redraw(ctx);
}

/* ---- PIN 二级弹窗渲染 (叠加在主列表之上, 背景可见) ---- */
static void km_render_pin_popup(st7305_handle_t *lcd) {
    int wx0 = KP_WIN_X, wy0 = KP_WIN_Y, wx1 = KP_WIN_X + KP_WIN_W - 1, wy1 = KP_WIN_Y + KP_WIN_H - 1;
    /* 弹窗外背景保留; 窗口白底 + 上下左右 2px 线框 */
    fill_rect(lcd, wx0, wy0, wx1, wy1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, wx0 + k, wx1 - k, wy0 + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, wx0 + k, wx1 - k, wy1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, wx0 + k, wy0 + k, wy1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, wx1 - k, wy0 + k, wy1 - k, ST7305_COLOR_BLACK);
    }
    /* 标题 (白底黑字, 不再有黑色标题条) */
    const char *t = s_pin_set
        ? (s_pin_step == 1 ? "\xe5\x86\x8d\xe6\xac\xa1\xe8\xbe\x93\xe5\x85\xa5" : "\xe8\xae\xbe\xe7\xbd\xaePin") /* 再次输入 / 设置Pin */
        : "\xe8\xbe\x93\xe5\x85\xa5Pin"; /* 输入Pin */
    int tw = text_width(t);
    draw_text(lcd, KP_WIN_X + (KP_WIN_W - tw) / 2, KP_WIN_Y + 5, t, false);
    /* PIN 单一输入框: 增大, 输入一个加一个 * (不限制数量) */
    {
        int px0 = KP_WIN_X + 12, px1 = KP_WIN_X + KP_WIN_W - 12;
        int py0 = KP_PIN_Y, py1 = KP_PIN_Y + KP_PIN_H - 1;
        draw_rect_outline(lcd, px0, py0, px1, py1, ST7305_COLOR_BLACK);
        char dots[20];
        int n = s_pin_len > 18 ? 18 : s_pin_len;
        for (int i = 0; i < n; i++) dots[i] = '*';
        dots[n] = 0;
        if (n > 0) draw_text_centered(lcd, py0 + (KP_PIN_H - 26) / 2, dots, false);
    }
    /* 九宫格数字 1-9 (ascii 数字字高 16, 上下居中) */
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int x, y, w, h; kp_rect(r, c, &x, &y, &w, &h);
            char buf[2] = { kp_cell_char(r, c), 0 };
            km_draw_cell(lcd, x, y, w, h, buf,
                        (s_cursor_r == r && s_cursor_c == c),
                        (s_press_r == r && s_press_c == c), 16);
        }
    }
    /* 底行: [删除][0][确定], 框统一为数字框尺寸 (KP_COL_W), 上下居中按字体高 */
    {
        static const char *sdel = "\xe5\x88\xa0\xe9\x99\xa4"; /* 删除 */
        static const char *sok  = "\xe7\xa1\xae\xe5\xae\x9a"; /* 确定 */
        int x, y, w, h;
        kp_rect(3, 0, &x, &y, &w, &h);
        km_draw_cell(lcd, x, y, w, h, sdel, (s_cursor_r == 3 && s_cursor_c == 0),
                     (s_press_r == 3 && s_press_c == 0), 24);
        kp_rect(3, 1, &x, &y, &w, &h);
        km_draw_cell(lcd, x, y, w, h, "0",  (s_cursor_r == 3 && s_cursor_c == 1),
                     (s_press_r == 3 && s_press_c == 1), 16);
        kp_rect(3, 2, &x, &y, &w, &h);
        km_draw_cell(lcd, x, y, w, h, sok, (s_cursor_r == 3 && s_cursor_c == 2),
                     (s_press_r == 3 && s_press_c == 2), 24);
    }
}

/* ---- 主列表底部操作行 ---- */
static void km_render_bottom(st7305_handle_t *lcd) {
    int by = 260;
    draw_hline(lcd, 0, S_W - 1, by - 2, ST7305_COLOR_BLACK);
    draw_label_centered_at(lcd, 43, by + 6, "\xe5\x88\x86\xe7\xb1\xbb", false);    /* 分类 */
    draw_label_centered_at(lcd, 116, by + 6, "\xe6\x96\xb0\xe5\xa2\x9e", false);   /* 新增 */
    draw_label_centered_at(lcd, 185, by + 6, "\xe5\xaf\xbc\xe5\x87\xba", false);   /* 导出 */
    draw_label_centered_at(lcd, 255, by + 6, "\xe5\xaf\xbc\xe5\x85\xa5", false);   /* 导入 */
    draw_label_centered_at(lcd, 330, by + 6, "\xe5\x88\xa0\xe9\x99\xa4", false);   /* 删除 */
    draw_label_centered_at(lcd, 376, by + 6, "\xe9\x80\x80\xe5\x87\xba", false);   /* 退出 */
}

/* 导出/导入到 TF 卡 (手动备份) */
static void on_export_or_import_ex(ui_ctx_t *ctx, bool is_export) {
    if (is_export) {
        int r = keyvault_export("/sdcard/keyvault.bak");
        os_dialog_toast(ctx, r == 0
            ? "\xe5\xb7\xb2\xe5\xaf\xbc\xe5\x87\xba\xe5\x88\xb0TF\xe5\x8d\xa1"   /* 已导出到TF卡 */
            : "\xe5\xaf\xbc\xe5\x87\xba\xe5\xa4\xb1\xe8\xb4\xa5");             /* 导出失败 */
    } else {
        int r = keyvault_import("/sdcard/keyvault.bak");
        os_dialog_toast(ctx, r == 0
            ? "\xe5\xb7\xb2\xe5\xaf\xbc\xe5\x85\xa5"                            /* 已导入 */
            : "\xe5\xaf\xbc\xe5\x85\xa5\xe5\xa4\xb1\xe8\xb4\xa5/\xe6\x97\xa0\xe5\xa4\x87\xe4\xbb\xbd"); /* 导入失败/无备份 */
        if (r == 0) {
            load_entries();
            os_pane_reset(&s_km_pane);
            os_pane_build_right(&s_km_pane);
            redraw(ctx);
        }
    }
}

/* ---- 渲染主入口 ---- */
static void km_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    if (s_state == KM_LIST) {
        /* 阅读器菜单UI模板: os_pane 双栏 (左: 设置/全部/分类; 右: 密钥或设置列表).
         * 无顶部标题黑条、无底部操作行 —— 设置入口在左栏首项"设置". */
        os_pane_render(ctx, &s_km_pane);
        /* 右栏为空且非设置态: 显示"点击添加"(点右栏进入新增) */
        if (!s_km_pane.settings_mode && s_km_pane.item_count == 0 && !s_pin_open) {
            /* 右栏内水平+垂直居中 (点击添加) */
            const char *tip = "\xe7\x82\xb9\xe5\x87\xbb\xe6\xb7\xbb\xe5\x8a\xa0"; /* 点击添加 */
            int twd = text_width(tip);
            int lw = s_km_pane.left_w > 0 ? s_km_pane.left_w : 100;
            int rx0 = lw + 11;              /* 与 os_pane 右栏起点对齐 (模板 sh=3 → left_w+11) */
            int re  = ui_screen_w() - 4;
            int tx = rx0 + (re - rx0 - twd) / 2;
            if (tx < rx0) tx = rx0;
            int hy = s_km_pane.list_y + (292 - s_km_pane.list_y) / 2 - 12;
            draw_text(lcd, tx, hy, tip, false);
        }
        if (s_pin_open) km_render_pin_popup(lcd);
    }
    else if (s_state == KM_ADD_NAME || s_state == KM_ADD_ACCOUNT || s_state == KM_ADD_SECRET) {
        fill_rect(lcd, 0, 26, S_W - 1, 48, ST7305_COLOR_BLACK);
        const char *t = (s_state == KM_ADD_NAME) ? "\xe8\xbe\x93\xe5\x85\xa5\xe7\xbd\x91\xe7\xab\x99\xe5\x90\x8d" /* 输入网站名 */
                     : (s_state == KM_ADD_ACCOUNT) ? "\xe8\xbe\x93\xe5\x85\xa5\xe8\xb4\xa6\xe5\x8f\xb7"          /* 输入账号 */
                     : "\xe8\xbe\x93\xe5\x85\xa5\xe5\xaf\x86\xe7\xa0\x81";                                        /* 输入密码 */
        int l2 = text_width(t);
        draw_text(lcd, (S_W - l2) / 2, 29, t, true);
        draw_text_centered(lcd, 56, s_buf, false);
        if (s_state == KM_ADD_SECRET) {   /* 生成随机密钥按钮 (右侧) */
            const int gx = S_W - 168, gy = 92, gw = 148, gh = 30;
            draw_rect_outline(lcd, gx, gy, gx + gw - 1, gy + gh - 1, ST7305_COLOR_BLACK);
            const char *gt = "\xe7\x94\x9f\xe6\x88\x90\xe9\x9a\x8f\xe6\x9c\xba\xe5\xaf\x86\xe9\x92\xa5"; /* 生成随机密钥 */
            int gtw = text_width(gt);
            draw_text(lcd, gx + (gw - gtw) / 2, gy + 4, gt, false);
        }
        /* 按压反馈: 按住键盘时对应键反黑 (松手恢复) */
        int press = -1, tx, ty;
        if (input_touch_now(&tx, &ty)) press = kbd_hit(&kbd_std, KM_KBD_Y, tx, ty);
        kbd_draw(lcd, KM_KBD_Y, &kbd_std, press);
    }
    else if (s_state == KM_DETAIL) {
        /* 条目详情: 展示账号/密码(可显示/隐藏), 底部 [自动输入][删除][返回] */
        fill_rect(lcd, 0, 26, S_W - 1, 48, ST7305_COLOR_BLACK);
        int l2 = text_width(s_names[s_sel_gi]);
        if (l2 > S_W - 20) l2 = S_W - 20;
        draw_text(lcd, (S_W - l2) / 2, 29, s_names[s_sel_gi], true);
        char ac[60], sec[KEYVAULT_MAX_SECRET + 1], scr[KEYVAULT_MAX_SECRET + 1];
        keyvault_get_account(s_key, s_sel_gi, ac, sizeof(ac));
        keyvault_get_secret(s_key, s_sel_gi, sec, sizeof(sec));
        int ly = 56;
        draw_label_centered_at(lcd, 20, ly, "\xe8\xb4\xa6\xe5\x8f\xb7", false);   /* 账号 */
        draw_text_centered(lcd, ly, ac[0] ? ac : "(none)", false);
        ly += 32;
        bool show = s_detail_show;
        if (show) snprintf(scr, sizeof(scr), "%s", sec);
        else { for (size_t i = 0; sec[i]; i++) scr[i] = '*'; size_t n = strlen(sec); scr[n] = 0; }
        draw_label_centered_at(lcd, 20, ly, "\xe5\xaf\x86\xe7\xa0\x81", false);    /* 密码 */
        draw_text_centered(lcd, ly, show ? (sec[0] ? sec : "(none)") : scr, false);
        draw_label_centered_at(lcd, 20, ly + 30, show ? "\xe9\x9a\x90\xe8\x97\x8f\xe5\xaf\x86\xe7\xa0\x81" : "\xe6\x98\xbe\xe7\xa4\xba\xe5\xaf\x86\xe7\xa0\x81", false); /* 隐藏密码/显示密码 */
        int by = S_H - 48;
        draw_hline(lcd, 0, S_W, by - 2, ST7305_COLOR_BLACK);
        draw_label_centered_at(lcd, (S_W / 3) / 2, by + 4, "\xe8\x87\xaa\xe5\x8a\xa8\xe8\xbe\x93\xe5\x85\xa5", s_detail_act == 0); /* 自动输入 */
        draw_label_centered_at(lcd, (S_W / 3) * 1.5f, by + 4, "\xe5\x88\xa0\xe9\x99\xa4", s_detail_act == 1);                  /* 删除 */
        draw_label_centered_at(lcd, (S_W / 3) * 2.5f, by + 4, "\xe8\xbf\x94\xe5\x9b\x9e", s_detail_act == 2);                  /* 返回 */
    }
    else if (s_state == KM_WEB) {
        /* 网页配置: 显示 AP 连接信息 + 关闭键 */
        fill_rect(lcd, 0, 26, S_W - 1, 48, ST7305_COLOR_BLACK);
        int l2 = text_width("\xe7\xbd\x91\xe9\xa1\xb5\xe9\x85\x8d\xe7\xbd\xae"); /* 网页配置 */
        draw_text(lcd, (S_W - l2) / 2, 29, "\xe7\xbd\x91\xe9\xa1\xb5\xe9\x85\x8d\xe7\xbd\xae", true);
        draw_text_centered(lcd, 52, "\xe7\x83\xad\xe7\x82\xb9: LinTOS-KV (\xe6\x97\xa0\xe5\xaf\x86\xe7\xa0\x81)", false); /* 热点: LinTOS-KV (无密码) */
        draw_text_centered(lcd, 84, "\xe6\x89\x8b\xe6\x9c\xba\xe8\xbf\x9e\xe5\x90\x8e\xe8\xae\xbf\xe9\x97\xae http://8.8.8.8", false); /* 手机连后访问 http://8.8.8.8 */
        draw_text_centered(lcd, 116, s_web_phase == 2 ? "\xe7\x8a\xb6\xe6\x80\x81: \xe5\xb7\xb2\xe5\x90\xaf\xe5\x8a\xa8"      /* 状态: 已启动 */
                                  : s_web_phase == 1 ? "\xe7\x8a\xb6\xe6\x80\x81: \xe5\x90\xaf\xe5\x8a\xa8\xe4\xb8\xad..."       /* 状态: 启动中... */
                                  : "\xe7\x8a\xb6\xe6\x80\x81: \xe6\x9c\xaa\xe5\x90\xaf\xe5\x8a\xa8/\xe5\xa4\xb1\xe8\xb4\xa5", false); /* 状态: 未启动/失败 */
        int by = S_H - 48;
        draw_hline(lcd, 0, S_W, by - 2, ST7305_COLOR_BLACK);
        draw_label_centered_at(lcd, S_W / 2, by + 4, "\xe5\x85\xb3\xe9\x97\xad", false); /* 关闭 */
    }
    else if (s_state == KM_CAT_MGR) {
        fill_rect(lcd, 0, 26, S_W - 1, 48, ST7305_COLOR_BLACK);
        int l2 = text_width("\xe5\x88\x86\xe7\xb1\xbb\xe7\xae\xa1\xe7\x90\x86"); /* 分类管理 */
        draw_text(lcd, (S_W - l2) / 2, 29, "\xe5\x88\x86\xe7\xb1\xbb\xe7\xae\xa1\xe7\x90\x86", true);
        int n_cat = keyvault_cat_count();
        int y = 52, maxr = (S_H - 44 - 40) / 30;
        int scroll = 0;
        if (s_cat_manage_cursor >= maxr) scroll = s_cat_manage_cursor - maxr + 1;
        for (int i = 0; i < maxr && (scroll + i) < n_cat; i++) {
            int idx = scroll + i;
            bool sel = (idx == s_cat_manage_cursor);
            if (sel) fill_rect(lcd, 30, y, S_W - 30, y + 26, ST7305_COLOR_BLACK);
            int tw2 = text_width(km_cat_label(idx));
            draw_text(lcd, (S_W - tw2) / 2, y + 4, km_cat_label(idx), sel);
            y += 30;
        }
        int by = S_H - 40;
        draw_hline(lcd, 0, S_W, by - 2, ST7305_COLOR_BLACK);
        draw_label_centered_at(lcd, (S_W / 3) / 2, by - 1, "\xe6\x96\xb0\xe5\xa2\x9e", false);        /* 新增 */
        draw_label_centered_at(lcd, (S_W / 3) * 1.5f, by - 1, "\xe6\x94\xb9\xe5\x90\x8d", false);      /* 改名 */
        draw_label_centered_at(lcd, (S_W / 3) * 2.5f, by - 1, "\xe5\x88\xa0\xe9\x99\xa4", false);     /* 删除 */
    }
    else if (s_state == KM_CAT_NAME) {
        fill_rect(lcd, 0, 26, S_W - 1, 48, ST7305_COLOR_BLACK);
        int l2 = text_width("\xe5\x88\x86\xe7\xb1\xbb\xe5\x90\x8d\xe7\xa7\xb0"); /* 分类名称 */
        draw_text(lcd, (S_W - l2) / 2, 29, "\xe5\x88\x86\xe7\xb1\xbb\xe5\x90\x8d\xe7\xa7\xb0", true);
        draw_text_centered(lcd, 56, s_buf, false);
        /* 按压反馈: 按住键盘时对应键反黑 (松手恢复) */
        int press = -1, tx, ty;
        if (input_touch_now(&tx, &ty)) press = kbd_hit(&kbd_std, KM_KBD_Y, tx, ty);
        kbd_draw(lcd, KM_KBD_Y, &kbd_std, press);
    }
    else if (s_state == KM_SEND) {
        draw_text_centered(lcd, 140, "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbe\x93\xe5\x85\xa5...", false); /* 正在输入... */
    }
}

/* ---- PIN 弹窗触摸 ---- */
static bool km_pin_touch(ui_ctx_t *ctx, int x, int y) {
    /* 仅拦截弹窗窗口内; 窗口外点击取消弹窗 (或忽略) */
    if (x < KP_WIN_X || x >= KP_WIN_X + KP_WIN_W || y < KP_WIN_Y || y >= KP_WIN_Y + KP_WIN_H) return true;
    /* 九宫格 */
    for (int r = 0; r < 3; r++) {
        int ry = KP_TOP + r * (KP_ROW_H + KP_ROW_GAP);
        if (y >= ry && y < ry + KP_ROW_H) {
            int c = (x - KP_COL_X0) / (KP_COL_W + KP_COL_GAP);
            if (c >= 0 && c <= 2 && kp_cell_char(r, c)) {
                s_press_r = r; s_press_c = c;
                s_cursor_r = r; s_cursor_c = c; km_pin_digit(ctx, kp_cell_char(r, c));
            }
            return true;
        }
    }
    /* 底行 (r3): [删除][0][确定] 全宽等框, 按列命中 */
    if (y >= KP_TOP + 3 * (KP_ROW_H + KP_ROW_GAP) && y < KP_TOP + 3 * (KP_ROW_H + KP_ROW_GAP) + KP_ROW_H) {
        int c = (x - KP_COL_X0) / (KP_COL_W + KP_COL_GAP);
        if (c < 0) c = 0;
        if (c > 2) c = 2;
        s_press_r = 3; s_press_c = c;
        s_cursor_r = 3; s_cursor_c = c;
        if (c == 0) km_pin_del(ctx);
        else if (c == 1) km_pin_digit(ctx, '0');
        else km_pin_submit(ctx);
        return true;
    }
    return true;
}

/* ---- 按键 ---- */
static void km_action(ui_ctx_t *ctx, os_action_t a) {
    /* PIN 弹窗优先 */
    if (s_pin_open) {
        if (a == OS_ACTION_UP)   { if (s_cursor_r > 0) { s_cursor_r--; kp_clamp_cursor(); redraw(ctx);} return; }
        if (a == OS_ACTION_DOWN) { if (s_cursor_r < 3) { s_cursor_r++; kp_clamp_cursor(); redraw(ctx);} return; }
        if (a == OS_ACTION_LEFT) { if (s_cursor_c > 0) s_cursor_c--; redraw(ctx); return; }
        if (a == OS_ACTION_RIGHT){ if (s_cursor_c < 2) s_cursor_c++; redraw(ctx); return; }
        if (a == OS_ACTION_CONFIRM) {
            if (s_cursor_r < 3) { char ch = kp_cell_char(s_cursor_r, s_cursor_c); if (ch) km_pin_digit(ctx, ch); }
            else { /* 底行: c0=删除 c1=0 c2=确定 */
                if (s_cursor_c == 0) km_pin_del(ctx);
                else if (s_cursor_c == 1) km_pin_digit(ctx, '0');
                else km_pin_submit(ctx);
            }
            return;
        }
        if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) {
            /* 设置->修改PIN: BACK 关弹窗回列表 */
            if (s_pin_from_admin) {
                s_pin_open = false; s_pin[0]=0; s_pin_len=0; s_pin_step=0;
                s_pin_from_admin = false; redraw(ctx); return;
            }
            /* 未输入密码按返回 → 直接退出本页 (不进主列表) */
            os_pop(ctx); return;
        }
        return;
    }

    if (s_state == KM_LIST) {
        if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) { os_pop(ctx); return; }
        if (os_pane_action(ctx, &s_km_pane, a)) return;
        return;
    }
    if (s_state == KM_ADD_NAME || s_state == KM_ADD_ACCOUNT || s_state == KM_ADD_SECRET) {
        if (a == OS_ACTION_BACK) {
            s_buf_len = 0; s_buf[0] = 0;
            if (s_state == KM_ADD_SECRET) s_state = KM_ADD_ACCOUNT;
            else if (s_state == KM_ADD_ACCOUNT) s_state = KM_ADD_NAME;
            else s_state = KM_LIST;
            redraw(ctx);
        }
        return;
    }
    if (s_state == KM_DETAIL) {
        if (a == OS_ACTION_LEFT)  { s_detail_act = (s_detail_act + 2) % 3; redraw(ctx); return; }
        if (a == OS_ACTION_RIGHT) { s_detail_act = (s_detail_act + 1) % 3; redraw(ctx); return; }
        if (a == OS_ACTION_UP || a == OS_ACTION_DOWN) { s_detail_show = !s_detail_show; redraw(ctx); return; } /* 显示/隐藏密码 */
        if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) { s_state = KM_LIST; s_sel_gi = -1; redraw(ctx); return; }
        if (a == OS_ACTION_CONFIRM) { km_detail_do(ctx); return; }
        return;
    }
    if (s_state == KM_WEB) {
        if (a == OS_ACTION_BACK || a == OS_ACTION_HOME || a == OS_ACTION_CONFIRM) {
            km_web_stop(ctx); s_state = KM_LIST; redraw(ctx);
        }
        return;
    }
    if (s_state == KM_CAT_MGR) {
        int n_cat = keyvault_cat_count();
        if (a == OS_ACTION_UP) { if (s_cat_manage_cursor > 0) s_cat_manage_cursor--; redraw(ctx); return; }
        if (a == OS_ACTION_DOWN) { if (s_cat_manage_cursor < n_cat - 1) s_cat_manage_cursor++; redraw(ctx); return; }
        if (a == OS_ACTION_BACK) { s_state = KM_LIST; redraw(ctx); return; }
        if (a == OS_ACTION_CONFIRM) { km_cat_manage_confirm(ctx); return; }
        return;
    }
    if (s_state == KM_CAT_NAME) {
        if (a == OS_ACTION_BACK) { s_buf_len = 0; s_buf[0] = 0; s_state = KM_CAT_MGR; redraw(ctx); }
        return;
    }
    if (s_state == KM_SEND) {
        if (a == OS_ACTION_BACK) { s_state = KM_LIST; redraw(ctx); }
    }
}

/* 分类管理 确认 (逻辑原在触摸, 按键 CONFIRM 复用) */
static void km_cat_manage_confirm(ui_ctx_t *ctx) {
    if (s_cat_manage_cursor >= 0 && s_cat_manage_cursor < keyvault_cat_count()) {
        os_dialog_toast(ctx, "分类已选");
    }
}

/* ---- 触摸 ---- */
static bool km_touch(ui_ctx_t *ctx, int x, int y) {
    if (x < 0 || y < 0) return false;
    if (s_pin_open) return km_pin_touch(ctx, x, y);

    if (s_state == KM_LIST) {
        /* 纯 os_pane 双栏交互; BACK 退出由 km_action 处理 */
        if (os_pane_touch(ctx, &s_km_pane, x, y)) return true;
        /* 右栏为空时点击 → 进入新增 */
        int lw = s_km_pane.left_w > 0 ? s_km_pane.left_w : 100;
        if (!s_km_pane.settings_mode && s_km_pane.item_count == 0 && x >= lw &&
            y >= s_km_pane.list_y && y < 292) {
            s_buf[0] = 0; s_buf_len = 0;
            s_state = KM_ADD_NAME; redraw(ctx);
            return true;
        }
        return false;
    }
    if (s_state == KM_DETAIL) {
        int by = S_H - 48;
        if (y >= by) {   /* 底键区 */
            int c = x / (S_W / 3);
            if (c < 0) c = 0;
            if (c > 2) c = 2;
            s_detail_act = c; km_detail_do(ctx);
        } else if (y >= 24 && y < by - 2) {   /* 触摸账号/密码区 → 显示/隐藏密码 */
            s_detail_show = !s_detail_show; redraw(ctx);
        }
        return true;
    }
    if (s_state == KM_WEB) {
        int by = S_H - 48;
        if (y >= by) { km_web_stop(ctx); s_state = KM_LIST; redraw(ctx); }
        return true;
    }
    if (s_state == KM_ADD_NAME || s_state == KM_ADD_ACCOUNT || s_state == KM_ADD_SECRET) {
        /* 生成随机密钥按钮 (仅密码步, 右侧) */
        if (s_state == KM_ADD_SECRET) {
            const int gx = S_W - 168, gy = 92, gw = 148, gh = 30;
            if (x >= gx && x < gx + gw && y >= gy && y < gy + gh) {
                km_gen_secret();
                redraw(ctx);
                return true;
            }
        }
        int ki = kbd_hit(&kbd_std, KM_KBD_Y, x, y);
        if (ki >= 0) {
            int act = kbd_press(&kbd_std, ki);
            if (act == KBD_ACT_CHAR) {
                char ch = kbd_char(&kbd_std, ki);
                if (ch && s_buf_len < (int)sizeof(s_buf) - 1) { s_buf[s_buf_len++] = ch; s_buf[s_buf_len] = 0; }
            } else if (act == KBD_ACT_BKSP) { if (s_buf_len > 0) s_buf[--s_buf_len] = 0; }
            else if (act == KBD_ACT_ENTER) { km_add_confirm(ctx); }
            else if (act == KBD_ACT_SPACE) { if (s_buf_len < (int)sizeof(s_buf) - 1) { s_buf[s_buf_len++] = ' '; s_buf[s_buf_len] = 0; } }
            redraw(ctx);
        }
        return true;
    }
    if (s_state == KM_CAT_MGR) {
        if (y >= 44 && y < S_H - 44) {
            int i = (y - 44) / 30 + ((s_cat_manage_cursor >= (S_H - 44 - 40) / 30) ? s_cat_manage_cursor - ((S_H - 44 - 40) / 30) + 1 : 0);
            if (i >= 0 && i < keyvault_cat_count()) { s_cat_manage_cursor = i; redraw(ctx); }
            return true;
        }
        if (y >= S_H - 40) {
            if (x < S_W / 3) {
                s_cat_edit_idx = -1; s_buf[0] = 0; s_buf_len = 0; s_state = KM_CAT_NAME; redraw(ctx);
            }
            else if (x < 2 * S_W / 3) {
                if (s_cat_manage_cursor >= 0 && s_cat_manage_cursor < keyvault_cat_count()) {
                    s_cat_edit_idx = s_cat_manage_cursor;
                    strncpy(s_buf, km_cat_label(s_cat_manage_cursor), sizeof(s_buf) - 1);
                    s_buf_len = (int)strlen(s_buf);
                    s_state = KM_CAT_NAME; redraw(ctx);
                }
            }
            else {
                if (s_cat_manage_cursor >= 0 && s_cat_manage_cursor < keyvault_cat_count()) {
                    if (keyvault_cat_remove(s_cat_manage_cursor) != 0)
                        os_dialog_toast(ctx, "无法删除");
                    else {
                        load_entries();
                        if (s_cat_manage_cursor >= keyvault_cat_count()) s_cat_manage_cursor--;
                        redraw(ctx);
                    }
                }
            }
            return true;
        }
        return true;
    }
    if (s_state == KM_CAT_NAME) {
        int ki = kbd_hit(&kbd_std, KM_KBD_Y, x, y);
        if (ki >= 0) {
            int act = kbd_press(&kbd_std, ki);
            if (act == KBD_ACT_CHAR) {
                char ch = kbd_char(&kbd_std, ki);
                if (ch && s_buf_len < 30) { s_buf[s_buf_len++] = ch; s_buf[s_buf_len] = 0; }
            } else if (act == KBD_ACT_BKSP) { if (s_buf_len > 0) s_buf[--s_buf_len] = 0; }
            else if (act == KBD_ACT_ENTER) {
                if (s_buf_len == 0) os_dialog_toast(ctx, "名称不能为空");
                else if (s_cat_edit_idx < 0) { if (keyvault_cat_add(s_buf) != 0) os_dialog_toast(ctx, "无法添加"); }
                else { if (keyvault_cat_rename(s_cat_edit_idx, s_buf) != 0) os_dialog_toast(ctx, "重名"); }
                s_buf[0] = 0; s_buf_len = 0; s_cat_edit_idx = -1;
                load_entries();
                s_state = KM_CAT_MGR; redraw(ctx);
            }
            else if (act == KBD_ACT_SPACE) { if (s_buf_len < 30) { s_buf[s_buf_len++] = ' '; s_buf[s_buf_len] = 0; } }
            redraw(ctx);
        }
        return true;
    }
    return false;
}

/* ================= 网页配置 (AP 热点 + HTTP 数据库) ================= */
static const char KV_AP_SSID[] = "LinTOS-KV";
static httpd_handle_t s_kv_httpd = NULL;
static void          *s_kv_dns   = NULL;
static bool           s_web_on   = false;
static uint8_t        s_web_key[KEYVAULT_KEY_LEN];

/* 精简前端: 查看/新增/删除 账号, 分类管理 */
static const char *km_web_html(void)
{
    return
    "<!doctype html><html><head><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{margin:0;font-family:sans-serif;background:#0f1115;color:#e6e6e6}*{box-sizing:border-box}"
    ".w{max-width:520px;margin:0 auto;padding:14px}h2{margin:.2em 0}input,select,button{padding:8px;margin:2px;font-size:15px;border-radius:6px;border:0}"
    "input,select{background:#22262e;color:#eee;flex:1;min-width:0}button{background:#2f6fdf;color:#fff;cursor:pointer}button.g{background:#444}"
    "button.r{background:#c0392b}.row{display:flex;gap:4px;margin:4px 0;align-items:center}.item{background:#1a1e26;padding:8px;border-radius:8px;margin:6px 0}"
    "b{color:#9fd0ff}small{color:#9aa}td{padding:2px 6px}</style></head><body><div class='w'>"
    "<h2>密钥管理</h2><small id=st></small>"
    "<div class='row'>分类:<select id=cat></select><button class=g onclick=addcat()>+分类</button>"
    "<button class=g onclick=delcat()>-分类</button><button onclick=rencat()>改名</button></div>"
    "<div id=f><div class='row'>网站<input id=g placeholder='网站名'></div>"
    "<div class='row'>账号<input id=a placeholder='账号'></div>"
    "<div class='row'>密码<input id=p placeholder='密码'><button onclick=add()>添加</button></div></div>"
    "<div id=list></div></div>"
    "<script>const $=id=>document.getElementById(id),E=encodeURIComponent;"
    "async function api(u){const r=await fetch(u);return r.json()}async function load(){"
    "const d=await api('/api/list');$('cat').innerHTML=d.cats.map((c,i)=>'<option value='+i+'>'+c+'</option>').join('');"
    "$('list').innerHTML=d.items.map(x=>'<div class=item><b>'+x.name+'</b> · '+x.account+'<br>密码: '+x.secret"
    "+'<button class=r onclick=del('+x.id+')>删除</button></div>').join('')||'<small>空</small>';$('st').textContent='共 '+d.items.length+' 条'}"
    "async function add(){await api('/api/add?cat='+$('cat').value+'&g='+E($('g').value)+'&a='+E($('a').value)+'&p='+E($('p').value));load()}"
    "async function del(id){await api('/api/del?id='+id);load()}"
    "async function addcat(){let n=prompt('新分类名');if(n)await api('/api/cat?op=add&n='+E(n)),load()}"
    "async function delcat(){await api('/api/cat?op=del&id='+$('cat').value);load()}"
    "async function rencat(){let n=prompt('新名', $('cat').selectedOptions[0].text);if(n)await api('/api/cat?op=ren&id='+$('cat').value+'&n='+E(n)),load()}"
    "load()</script></body></html>";
}

static esp_err_t km_web_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, km_web_html(), HTTPD_RESP_USE_STRLEN);
}

static void km_web_send_json(httpd_req_t *req, const char *s)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, s);
}

static esp_err_t km_web_list_handler(httpd_req_t *req)
{
    /* 构造 JSON: {"cats":[...],"items":[{"id","cat","name","account","secret"}]} */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"cats\":[");
    int nc = keyvault_cat_count();
    for (int i = 0; i < nc; i++) {
        if (i) httpd_resp_sendstr(req, ",");
        char cb[96]; snprintf(cb, sizeof(cb), "\"%s\"", keyvault_cat_name(i));
        httpd_resp_sendstr(req, cb);
    }
    httpd_resp_sendstr(req, "],\"items\":[");
    char names[KEYVAULT_MAX_ENTRIES][64]; int cats[KEYVAULT_MAX_ENTRIES];
    int total = keyvault_list(names, cats, KEYVAULT_MAX_ENTRIES);
    for (int i = 0; i < total; i++) {
        char ac[64], sc[KEYVAULT_MAX_SECRET + 1];
        keyvault_get_account(s_web_key, i, ac, sizeof(ac));
        keyvault_get_secret(s_web_key, i, sc, sizeof(sc));
        if (i) httpd_resp_sendstr(req, ",");
        char b[512];
        snprintf(b, sizeof(b), "{\"id\":%d,\"cat\":%d,\"name\":\"%s\",\"account\":\"%s\",\"secret\":\"%s\"}",
                 i, cats[i], names[i], ac, sc);
        httpd_resp_sendstr(req, b);
    }
    httpd_resp_sendstr(req, "]}");
    return ESP_OK;
}

static esp_err_t km_web_add_handler(httpd_req_t *req)
{
    char qs[512], cat[8] = "0", g[64], a[64], p[128];
    g[0] = a[0] = p[0] = 0;
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) { km_web_send_json(req, "{\"ok\":false}"); return ESP_OK; }
    httpd_query_key_value(qs, "cat", cat, sizeof(cat));
    httpd_query_key_value(qs, "g", g, sizeof(g));
    httpd_query_key_value(qs, "a", a, sizeof(a));
    httpd_query_key_value(qs, "p", p, sizeof(p));
    int rc = keyvault_add(s_web_key, atoi(cat), g, a, p);
    char out[32]; snprintf(out, sizeof(out), "{\"ok\":%s}", rc == 0 ? "true" : "false");
    km_web_send_json(req, out);
    return ESP_OK;
}

static esp_err_t km_web_del_handler(httpd_req_t *req)
{
    char qs[64], id[8] = "0";
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK)
        httpd_query_key_value(qs, "id", id, sizeof(id));
    int rc = keyvault_remove(s_web_key, atoi(id));
    char out[32]; snprintf(out, sizeof(out), "{\"ok\":%s}", rc == 0 ? "true" : "false");
    km_web_send_json(req, out);
    return ESP_OK;
}

static esp_err_t km_web_cat_handler(httpd_req_t *req)
{
    char qs[128], op[8], id[8], n[40];
    op[0] = id[0] = n[0] = 0;
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) { km_web_send_json(req, "{\"ok\":false}"); return ESP_OK; }
    httpd_query_key_value(qs, "op", op, sizeof(op));
    httpd_query_key_value(qs, "id", id, sizeof(id));
    httpd_query_key_value(qs, "n", n, sizeof(n));
    int rc = -1;
    if (strcmp(op, "add") == 0) rc = keyvault_cat_add(n);
    else if (strcmp(op, "del") == 0) rc = keyvault_cat_remove(atoi(id));
    else if (strcmp(op, "ren") == 0) rc = keyvault_cat_rename(atoi(id), n);
    char out[32]; snprintf(out, sizeof(out), "{\"ok\":%s}", rc == 0 ? "true" : "false");
    km_web_send_json(req, out);
    return ESP_OK;
}

static int s_web_phase = 0;   /* 0=空闲 1=启动中 2=已启动 3=失败 */
static void km_web_task(void *arg)
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
    memcpy(ap.ap.ssid, KV_AP_SSID, strlen(KV_AP_SSID));
    ap.ap.ssid_len = strlen(KV_AP_SSID);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    if (s_pin_plain[0] && strlen(s_pin_plain) >= 8) {   /* 有 PIN(≥8位) → Wi-Fi 密码 = PIN (WPA2) */
        memcpy(ap.ap.password, s_pin_plain, strlen(s_pin_plain));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.password[0] = 0;        /* 无/过短 PIN → 开放 (WPA2 需≥8位) */
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap.ap.pmf_cfg.required = false;
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK ||
        esp_wifi_start() != ESP_OK) { s_web_phase = 3; vTaskDelete(NULL); return; }
    os_ap_set(true);   /* 状态栏显示 NET 图标 */
    keyvault_init();   /* 确保分类已加载, 网页下拉能列出分类 */

    esp_netif_ip_info_t ip = { 0 };
    IP4_ADDR(&ip.ip, 8, 8, 8, 8); IP4_ADDR(&ip.gw, 8, 8, 8, 8);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           "http://8.8.8.8/", strlen("http://8.8.8.8/"));
    esp_netif_dhcps_start(ap_netif);
    dns_server_config_t dc = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_kv_dns = start_dns_server(&dc);

    if (s_kv_httpd == NULL) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.lru_purge_enable = true; cfg.stack_size = 8192;
        if (httpd_start(&s_kv_httpd, &cfg) == ESP_OK) {
            httpd_uri_t u;
            u.user_ctx = NULL;
            u.method = HTTP_GET; u.uri = "/"; u.handler = km_web_index_handler;
            httpd_register_uri_handler(s_kv_httpd, &u);
            u.uri = "/api/list"; u.handler = km_web_list_handler; httpd_register_uri_handler(s_kv_httpd, &u);
            u.uri = "/api/add";  u.handler = km_web_add_handler;  httpd_register_uri_handler(s_kv_httpd, &u);
            u.uri = "/api/del";  u.handler = km_web_del_handler;  httpd_register_uri_handler(s_kv_httpd, &u);
            u.uri = "/api/cat";  u.handler = km_web_cat_handler;  httpd_register_uri_handler(s_kv_httpd, &u);
        }
    }
    s_web_on = true;
    s_web_phase = (s_kv_httpd != NULL) ? 2 : 3;
    ESP_LOGI(TAG, "网页配置: 热点 %s 已%s", KV_AP_SSID,
             (s_kv_httpd != NULL) ? "开启 (http://8.8.8.8)" : "开启失败");
    vTaskDelete(NULL);
}

void km_web_start(ui_ctx_t *ctx)
{
    (void)ctx;
    if (s_web_on || s_web_phase == 1) return;
    s_web_mode = 0;                      /* 热点配置 = AP 模式 */
    memcpy(s_web_key, s_key, sizeof(s_web_key));
    s_web_phase = 1;   /* 后台任务启动, 避免在 UI 按键栈上执行网络栈导致溢出重启 */
    xTaskCreate(km_web_task, "kvweb", 8192, NULL, 5, NULL);
    /* 资源管家: 登记瞬态清理 — 退回主菜单/软件管家时自动关停 AP 网页服务器 */
    os_housekeeper_add(&km_web_house_stop);
}

/* 管家无参清理: 关停 AP 网页服务器 (忽略 UI 上下文) */
static void km_web_house_stop(void) { km_web_stop(NULL); }

void km_web_stop(ui_ctx_t *ctx)
{
    (void)ctx;
    if (!s_web_on && s_web_phase == 0) return;
    if (s_kv_httpd) { httpd_stop(s_kv_httpd); s_kv_httpd = NULL; }
    if (s_web_mode == 0) {   /* 热点(AP)模式额外清理: DNS + 停WiFi(AP) + 状态栏NET图标 */
        if (s_kv_dns) { stop_dns_server(s_kv_dns); s_kv_dns = NULL; }
        esp_wifi_stop();
        os_ap_set(false);
    }
    s_web_on = false;
    s_web_phase = 0;
    s_lan_pending = false;
    s_lan_toast_pending = false;
}

/* ---- IP配置 (STA/局域网): 不起AP, 复用当前 WiFi(STA), 只启动 http 数据库, 1:1 复用局域网手柄流程 ----
 * 流程: 未连WiFi → 系统"连接WiFi"(扫描+密码) → 连接成功(poll检测) → 启动服务 → toast IP 提示. */
static void km_web_task_lan(void *arg)
{
    (void)arg;
    keyvault_init();   /* 确保分类已加载 */
    if (s_kv_httpd == NULL) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.lru_purge_enable = true; cfg.stack_size = 8192;
        if (httpd_start(&s_kv_httpd, &cfg) == ESP_OK) {
            httpd_uri_t u;
            u.user_ctx = NULL;
            u.method = HTTP_GET; u.uri = "/"; u.handler = km_web_index_handler;
            httpd_register_uri_handler(s_kv_httpd, &u);
            u.uri = "/api/list"; u.handler = km_web_list_handler; httpd_register_uri_handler(s_kv_httpd, &u);
            u.uri = "/api/add";  u.handler = km_web_add_handler;  httpd_register_uri_handler(s_kv_httpd, &u);
            u.uri = "/api/del";  u.handler = km_web_del_handler;  httpd_register_uri_handler(s_kv_httpd, &u);
            u.uri = "/api/cat";  u.handler = km_web_cat_handler;  httpd_register_uri_handler(s_kv_httpd, &u);
        }
    }
    s_web_on = true;
    s_web_phase = (s_kv_httpd != NULL) ? 2 : 3;
    ESP_LOGI(TAG, "IP配置: 密钥服务已%s", (s_kv_httpd != NULL) ? "开启" : "开启失败");
    vTaskDelete(NULL);
}

/* 启动 IP(STA) 密钥服务; 启动结果由 poll 检测后 toast IP 提示 */
static void km_lan_start(ui_ctx_t *ctx)
{
    (void)ctx;
    if (s_web_on || s_web_phase == 1) return;
    s_web_mode = 1;                      /* IP配置 = STA 模式 */
    s_lan_pending = false;
    s_lan_toast_pending = true;
    memcpy(s_web_key, s_key, sizeof(s_web_key));
    s_web_phase = 1;
    xTaskCreate(km_web_task_lan, "kvlweb", 4096, NULL, 5, NULL);
    /* 不登记资源管家: IP/局域网服务在 toast IP 后回主桌面仍须运行 (1:1 局域网手柄), 仅由设置项手动关闭 */
}

/* ---- 生成随机密钥 (填入当前密码输入框 s_buf) ---- */
static void km_gen_secret(void) {
    static const char cs[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!@#$%^&*_-";
    const int n = (int)strlen(cs);
    const int L = 16;
    int len = 0;
    for (int i = 0; i < L && len + 1 < (int)sizeof(s_buf); i++) {
        s_buf[len++] = cs[(int)(esp_random() % (uint32_t)n)];
    }
    s_buf[len] = 0;
    s_buf_len = len;
}

/* ---- 新增确认 (网站名→账号→密码 三步) ---- */
static void km_add_confirm(ui_ctx_t *ctx) {
    if (s_state == KM_ADD_NAME) {
        if (s_buf_len == 0) { os_dialog_toast(ctx, "网站名不能为空"); return; }
        strncpy(s_name_buf, s_buf, sizeof(s_name_buf) - 1); s_name_buf[sizeof(s_name_buf) - 1] = 0;
        s_buf[0] = 0; s_buf_len = 0;
        s_state = KM_ADD_ACCOUNT;
        redraw(ctx);
    } else if (s_state == KM_ADD_ACCOUNT) {
        strncpy(s_acct_buf, s_buf, sizeof(s_acct_buf) - 1); s_acct_buf[sizeof(s_acct_buf) - 1] = 0;
        s_buf[0] = 0; s_buf_len = 0;
        s_state = KM_ADD_SECRET;
        redraw(ctx);
    } else if (s_state == KM_ADD_SECRET) {
        if (s_buf_len == 0) { os_dialog_toast(ctx, "密码不能为空"); return; }
        int cat = (s_km_pane.sel_folder > 1) ? (s_km_pane.sel_folder - 2) : 0; /* 选中分类(非全部)归入 */
        if (keyvault_add(s_key, cat, s_name_buf, s_acct_buf, s_buf) == 0) os_dialog_toast(ctx, "已添加");
        else os_dialog_toast(ctx, "添加失败");
        s_buf[0] = 0; s_buf_len = 0;
        s_acct_buf[0] = 0; s_name_buf[0] = 0;
        load_entries();
        os_pane_build_right(&s_km_pane);
        s_state = KM_LIST;
        redraw(ctx);
    }
}

/* 删除确认回调 (长按右栏项 1 秒触发) */
static bool s_km_del_dlg = false;   /* 防重复弹确认框 */
static void km_del_cb(ui_ctx_t *ctx, int result, void *ud) {
    s_km_del_dlg = false;
    if (result == 0) {
        int gi = (int)(intptr_t)ud;
        keyvault_remove(s_key, gi);
        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x88\xa0\xe9\x99\xa4"); /* 已删除 */
        load_entries();
        os_pane_build_right(&s_km_pane);
        redraw(ctx);
    }
}
/* 详情态删除确认 */
static void km_detail_del_cb(ui_ctx_t *ctx, int result, void *ud) {
    if (result == 0) {
        int gi = (int)(intptr_t)ud;
        keyvault_remove(s_key, gi);
        load_entries();
        os_pane_build_right(&s_km_pane);
    }
    s_state = KM_LIST; s_sel_gi = -1; s_detail_show = false;
    redraw(ctx);
}
/* 详情态底键执行: 0=自动输入 1=删除 2=返回 */
static void km_detail_do(ui_ctx_t *ctx) {
    if (s_sel_gi < 0 || s_sel_gi >= s_total) return;
    if (s_detail_act == 0) {
        char sec[KEYVAULT_MAX_SECRET + 1];
        if (keyvault_get_secret(s_key, s_sel_gi, sec, sizeof(sec)) == 0 && sec[0]) {
            s_state = KM_SEND; redraw(ctx);
            input_set_gamepad_nav_enabled(false);
            vTaskDelay(pdMS_TO_TICKS(150));
            kv_kbd_str(sec);
            input_set_gamepad_nav_enabled(true);
        }
        s_state = KM_LIST; s_sel_gi = -1; s_detail_show = false; redraw(ctx);
    } else if (s_detail_act == 1) {
        os_dialog_confirm_ex(ctx, "\xe5\x88\xa0\xe9\x99\xa4\xe6\xad\xa4\xe9\xa1\xb9?", 3000, 0,
                             km_detail_del_cb, (void *)(intptr_t)s_sel_gi); /* 删除此项? */
    } else {
        s_state = KM_LIST; s_sel_gi = -1; redraw(ctx);
    }
}

/* ---- 每帧: os_pane 平滑滚动 + 长按删除 ---- */
/* 触摸松手 → 复位按下状态 (恢复原色) 并重绘 */
static void km_press_release(ui_ctx_t *ctx) {
    static bool prev_down = false;    /* 记录上一帧是否有手指 */
    bool down = input_get_touch_pos(NULL, NULL);
    if (!down && prev_down) {
        if (s_press_r >= 0 || s_press_c >= 0) { s_press_r = s_press_c = -1; redraw(ctx); }
    }
    prev_down = down;
}

static void km_poll(ui_ctx_t *ctx) {
    km_press_release(ctx);           /* 弹窗/列表共用: 松手恢复按键原色 */
    /* IP配置 (STA): 等待 WiFi 连接后自动启动密钥服务 (1:1 复用局域网手柄流程) */
    if (s_lan_pending) {
        if (wifi_manager_is_connected()) {
            s_lan_pending = false;
            km_lan_start(ctx);
        } else if (!wifi_manager_is_connecting()) {
            s_lan_pending = false;   /* 连接失败/被取消 → 放弃 */
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
            os_dialog_toast(ctx, "\xe5\xaf\x86\xe9\x92\xa5\xe6\x9c\x8d\xe5\x8a\xa1\xe5\xb7\xb2\xe5\xbc\x80\xe5\x90\xaf"); /* 密钥服务已开启 */
        }
        return;
    }
    if (s_pin_open) return;          /* 弹窗期间不滚动底层 */
    if (s_state != KM_LIST) return;
    os_pane_poll(ctx, &s_km_pane);
    /* 长按右栏选项 1 秒 → 弹「是否删除」确认 */
    if (s_km_del_dlg || s_km_pane.settings_mode || s_km_pane.item_count == 0) return;
    static int  lp_idx = -1;
    static uint32_t lp_t0 = 0;
    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    int lw = s_km_pane.left_w > 0 ? s_km_pane.left_w : 100;
    int list_y = s_km_pane.list_y;
    int sel = s_km_pane.sel_item;
    if (down && tx >= lw && ty >= list_y && ty < 292 &&
        sel >= 0 && sel < s_km_pane.item_count) {
        if (lp_idx != sel) { lp_idx = sel; lp_t0 = (uint32_t)(esp_timer_get_time() / 1000); }
        else if ((uint32_t)(esp_timer_get_time() / 1000) - lp_t0 >= 1000) {
            lp_idx = -1;
            s_km_del_dlg = true;
            int gi = s_km_map[sel];
            os_dialog_confirm_ex(ctx, "\xe5\x88\xa0\xe9\x99\xa4\xe6\xad\xa4\xe6\x9d\xa1\xe8\xae\xb0\xe5\xbd\x95?", /* 删除此条记录? */
                                 3000, 0, km_del_cb, (void *)(intptr_t)gi);
        }
    } else {
        lp_idx = -1;
    }
}

/* ---- 进入/退出 ---- */
static void km_enter(ui_ctx_t *ctx) {
    keyvault_init();
    km_hidmode_load();   /* 载入 蓝牙HID键盘↔手柄 模式 */
    memset(&s_km_pane, 0, sizeof(s_km_pane));
    s_km_pane.root = "/";            /* 内存数据源不扫目录, 仅供 os_pane_reset 日志非空 */
    s_km_pane.list_y = 30;           /* 状态栏(24)下方, 与其他页面一致 */
    s_km_pane.list_bottom = 292;     /* 无底部操作行, 到底 */
                                     /* 左栏用模板参考宽度 (默认 100, 不再收缩) */
    s_km_pane.right_gap = 3;   /* 右缘水管右移3px(仅本页), 不影响其它 os_pane 页面 */
    s_km_pane.settings_label = "\xe8\xae\xbe\xe7\xbd\xae\xe7\xae\xa1\xe7\x90\x86";  /* 设置管理 (左栏首项) */
    s_km_pane.settings_count = 5;
    s_km_pane.settings_render = km_settings_render;
    s_km_pane.on_settings_select = km_settings_select;
    s_km_pane.mem_folders = km_pane_folders;
    s_km_pane.mem_items   = km_pane_items;
    s_km_pane.on_select   = km_pane_select;
    load_entries();
    os_pane_reset(&s_km_pane);
    os_pane_build_right(&s_km_pane);
    /* PIN 引导: 直接进入主列表, 再弹二级弹窗设置/解锁 */
    s_state = KM_LIST;
    s_pin_from_admin = false;
    s_pin_open = true;
    s_pin_set  = !keyvault_has_pin();   /* 无 PIN → 引导设置; 有 PIN → 解锁 */
    s_pin_step = 0;
    s_pin[0] = 0; s_pin_len = 0;
    s_pin_again[0] = 0; s_again_len = 0;
    s_cursor_r = 0; s_cursor_c = 0;
    ctx->needs_redraw = true;
}
static void km_exit(ui_ctx_t *ctx) {
    (void)ctx;
    if (s_web_mode == 0) km_web_stop(ctx);   /* 退出应用自动关闭 热点(AP); IP/局域网服务保留 (1:1 手柄) */
    s_state = KM_LIST;
    s_pin_open = false;
    s_pin_from_admin = false;
    memset(s_key, 0, sizeof(s_key));
}

static const os_module_t s_mod_keymgr = {
    .name      = "keymgr",
    .page_id   = OS_PAGE_PLACEHOLDER_SECURE,
    .on_enter  = km_enter,
    .on_exit   = km_exit,
    .render    = km_render,
    .action    = km_action,
    .touch     = km_touch,
    .poll      = km_poll,
    .fullscreen = false,
};

void os_page_keymanager_register(void) { os_register(&s_mod_keymgr); }
