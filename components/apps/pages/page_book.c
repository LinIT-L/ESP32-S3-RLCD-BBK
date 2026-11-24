/**
 * page_book.c — 电子书 页面模块.
 *
 * 书架双栏 (左分类 / 右书籍) 使用统一模板 os_pane + 阅读器模式 (book_reader 组件, 运行时全屏).
 * 私有 state 一律 static 留在本文件, 不放进 ui_ctx.
 */
#include "os.h"
#include "os_pane.h"
#include "ui_common.h"
#include "ui_scroll.h"   /* ★ 2026-10-24 统一跟手滚动 */
#include "book_reader.h"
#include "font_book.h"
#include "sd_scan.h"
#include "input.h"
#include "favorites.h"

#include "svc_nvs.h"
#include "svc_cfg.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>

#define TAG "PBOOK"

#define BOOK_ROOT "/sdcard/books"
#define BNS "os_book"

/* 书籍文件过滤: txt/epub/fb2 (损坏/0字节也列出, 打开时再提示损坏) */
static bool is_txt(const char *name) {
    size_t len = strlen(name);
    if (len < 5) return false;
    const char *e = name + len - 4;
    return (strcasecmp(e, ".txt") == 0) || (strcasecmp(e, ".epub") == 0) ||
           (strcasecmp(e, ".fb2") == 0);
}

/* ★ LINTOS 2026-10-25: 列表 pane(约 59KB) 原来静态常驻 PSRAM, 改为进页面才分配/退出即还 */
static os_pane_t *s_bp__p;
static os_pane_t *s_bp__get(void) {
    if (!s_bp__p) {
        s_bp__p = heap_caps_calloc(1, sizeof(os_pane_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_bp__p) ESP_LOGW("PANE", "列表 pane 分配失败 (%u B)", (unsigned)sizeof(os_pane_t));
    }
    if (!s_bp__p) s_bp__p = heap_caps_calloc(1, sizeof(os_pane_t), MALLOC_CAP_8BIT);   /* 兜底: 内部 RAM */
    return s_bp__p;
}
static inline void s_bp__free(void) { if (s_bp__p) { heap_caps_free(s_bp__p); s_bp__p = NULL; } }
#define s_bp (*s_bp__get())

/* 阅读器模式 (运行时全屏) */
static bool s_reader_open = false;

/* ==== 阅读优化设置 (3.3: 字号/字体/页码/边距/行高/字距/缩进/旋转) ==== */
static int s_bs_fontsize = 1;   /* 0=16 1=24 2=32 3=48 (2026-10-31 四档) */
static int s_bs_font     = 0;   /* 0=内置 1=TF 字体（只作"有没有选 TF 字体"的标记，真正的选择看 s_bs_tfname） */
static char s_bs_tfname[64] = {0};   /* 选中的 TF 字库文件名（空=内置） */
static int s_bs_pagenum  = 1;   /* 0=关 1=开 */
static int s_bs_margin   = 1;   /* 0=窄 1=中 2=宽 */
static int s_bs_lineh    = 1;   /* 0=紧凑 1=标准 2=宽松 */
static int s_bs_gap      = 1;   /* 0=紧凑 1=标准 2=宽松 */
static int s_bs_indent   = 1;   /* 0=关 1=开 (段落首行缩进两全角空格) */
static int s_bs_rot      = 2;   /* 0=上 1=下 2=左 3=右 (阅读优化旋转方向) —— 默认「左」(linit 2026-10-24) */
/* ★ 2026-11-22 linit: 「阅读优化 → 震动」开关. 开=阅读器内点按/翻页保持现有触觉反馈,
 *   关=阅读页静音 (其它页面不受影响). NVS 键 vb, 缺省 1(开) ⇒ 老存档行为不变. */
static int s_bs_vib      = 1;   /* 0=关 1=开 */

/* 当前选中的字体文件全路径；内置返回 ""（空串）。
 * ★ 2026-10-31: 字体单一来源 = NVS 的 ftn（文件名）。阅读器内「字体选择」写的是**同一组键**，
 *   所以两边显示/生效的永远是同一个字体，不再各存一套。 */
static const char *bs_font_path(void) {
    static char p[96];
    if (s_bs_font >= 1 && s_bs_tfname[0]) {
        snprintf(p, sizeof(p), "/sdcard/fonts/%.63s", s_bs_tfname);
        return p;
    }
    return "";
}

static void bs_load(void) {
    nvs_handle_t h;
    if (svc_nvs_open(BNS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        /* 字号三档: 0=16 1=24 2=32。旧存档 0..3 = 20/24/28/32 → 20→16, 24→24, 28/32→32 */
        /* 字号 0..3 = 16/24/32/48。旧存档只有 0..2，语义不变，天然兼容。 */
        if (svc_nvs_get_i32(h, "fs", &v) == ESP_OK)
            s_bs_fontsize = (v < 0) ? 1 : (v > 3) ? 3 : (int)v;
        /* 字体: 以文件名 ftn 为准（空=内置）。ft 只作旧档兼容标记 —— 老版本只存了
         * 一个序号，序号没法反查是哪个文件，只能退回内置，不能瞎猜。 */
        {
            char nm[64] = {0};
            size_t nl = sizeof(nm);
            if (svc_nvs_get_str(h, "ftn", nm, &nl) == ESP_OK && nm[0]) {
                snprintf(s_bs_tfname, sizeof(s_bs_tfname), "%.63s", nm);
                s_bs_font = 1;
            } else {
                s_bs_tfname[0] = 0;
                s_bs_font = 0;
            }
        }
        if (svc_nvs_get_i32(h, "pn", &v) == ESP_OK && v >= 0 && v <= 1) s_bs_pagenum = (int)v;
        if (svc_nvs_get_i32(h, "mg", &v) == ESP_OK && v >= 0 && v <= 2) s_bs_margin = (int)v;
        if (svc_nvs_get_i32(h, "lh", &v) == ESP_OK && v >= 0 && v <= 2) s_bs_lineh = (int)v;
        if (svc_nvs_get_i32(h, "gp", &v) == ESP_OK && v >= 0 && v <= 2) s_bs_gap = (int)v;
        if (svc_nvs_get_i32(h, "in", &v) == ESP_OK && v >= 0 && v <= 1) s_bs_indent = (int)v;
        if (svc_nvs_get_i32(h, "rt", &v) == ESP_OK && v >= 0 && v <= 3) s_bs_rot = (int)v;
        if (svc_nvs_get_i32(h, "vb", &v) == ESP_OK && v >= 0 && v <= 1) s_bs_vib = (int)v;
        /* ★ 2026-10-24 linit: 阅读旋转方向**默认改成「左」**。
         *   老存档里可能存着别的方向(rt)，这里做一次性迁移: 没有 rot_ver 的旧档
         *   统一切成 2(左) 并写 rot_ver=1; 之后用户自己转的方向照常保存、不再覆盖。 */
        if (svc_nvs_get_i32(h, "rot_ver", &v) != ESP_OK) {
            s_bs_rot = 2;
        }
        svc_nvs_close(h);
    }
}
static void bs_save(void) {
    svc_cfg_set_i32(BNS, "fs", s_bs_fontsize);
    svc_cfg_set_i32(BNS, "ft", s_bs_font);
    /* 字体以文件名持久化：阅读器内「字体选择」写同一组键，保证两边状态一致 */
    {
        nvs_handle_t h;
        if (svc_nvs_open(BNS, NVS_READWRITE, &h) == ESP_OK) {
            svc_nvs_set_str(h, "ftn", s_bs_tfname);
            svc_nvs_commit(h);
            svc_nvs_close(h);
        }
    }
    svc_cfg_set_i32(BNS, "pn", s_bs_pagenum);
    svc_cfg_set_i32(BNS, "mg", s_bs_margin);
    svc_cfg_set_i32(BNS, "lh", s_bs_lineh);
    svc_cfg_set_i32(BNS, "gp", s_bs_gap);
    svc_cfg_set_i32(BNS, "in", s_bs_indent);
    svc_cfg_set_i32(BNS, "rt", s_bs_rot);
    svc_cfg_set_i32(BNS, "vb", s_bs_vib);
    svc_cfg_set_i32(BNS, "rot_ver", 1);   /* 迁移标记: 默认方向已定为「左」 */
}
static int  bs_size_reader_idx(void);   /* 前置声明 (定义在下方): 三档字号 → 阅读器内部档位 */
static void bs_apply(void) {
    int style = 0;                                   /* 内置字体只有一套（宋体），style 不再分仿宋/黑体 */
    const char *fp = bs_font_path();
    if (fp[0]) {
        if (!font_book_select_file(fp)) {
            /* 文件被删 / 内容损坏 → 回退内置，并把记录改回内置，避免每次开书都白试一次 */
            ESP_LOGW(TAG, "字体不可用 %s: %s → 回退内置", fp, font_book_error());
            s_bs_font = 0;
            s_bs_tfname[0] = 0;
            bs_save();
            font_book_select_embedded(style, font_book_size_px_for("", s_bs_fontsize));
        }
    } else {
        /* ★ 2026-10-31 修：原来这一段只在"选了 TF 字体"时才动字体，
         *   选回「内置字体」什么都不做 ⇒ 上一本书的 TF 字体一直生效，
         *   "内置字体"形同虚设（而且书库显示内置、正文还是像素字体）。 */
        font_book_select_embedded(style, font_book_size_px_for("", s_bs_fontsize));
    }
    book_reader_set_settings(false, 1, false, s_bs_pagenum != 0, s_bs_rot,
                             style, bs_size_reader_idx(), s_bs_margin, s_bs_lineh,
                             s_bs_gap, s_bs_indent);
    /* ★ 2026-11-22: 阅读页震动开关 (只影响阅读器内的点按/翻页反馈) */
    book_reader_set_page_vib(s_bs_vib != 0);
}

/* ==== 书架旋转 (自包含渲染, 触摸走全局 input 旋转) ====
 * 渲染: 竖屏(左2/右3) → st7305 pt 逻辑缓冲 300×400; 180°(下1) → st7305 flip;
 *       上(0) 原样.
 * 触摸: **统一由 input_set_screen_rotation(s_bs_rot) 换算** (与阅读器同一套
 *       rotate_screen_coord), 页面/弹窗/手势层共享同一个逻辑坐标系。
 * ★ 2026-11-22 为什么不再用 ctx->map_touch:
 *   map_touch 只在 os_handle_touch() 分发那一刻生效, 而**底部上滑返回手势是在
 *   input.c 的触摸状态机里就已经判定完的**, 根本拿不到这次映射。于是竖屏时:
 *   手指"视觉向上滑"= 物理横向移动 → 手势层 up = gb_sy - sy ≈ 0 → 永远不返回。
 *   改走 input 旋转后, 手势层与页面/弹窗看到的都是同一套逻辑坐标, 上滑自动正确。
 *   ⚠ 代价/约束: 本页 touch / drag / poll 收到的坐标**已经是逻辑坐标**,
 *     一律直接使用, 不能再调任何映射函数 (会二次旋转)。 */
static uint8_t *s_book_pfb  = NULL;   /* 竖屏逻辑缓冲 40×400 */
static uint8_t *s_book_flip = NULL;   /* 180° 翻转缓冲 15000 */

static void book_rotation_off(ui_ctx_t *ctx) {
    if (ctx && ctx->lcd) st7305_set_rotation(ctx->lcd, 0, NULL);
    ui_set_portrait(false);
    if (ctx) ctx->map_touch = NULL;
    /* ⚠ 这里**不碰** input 旋转: 本函数每帧都在"阅读器已接管"的分支里被调用,
     *   若在此设 0 会把阅读器的竖屏触摸掰回横屏 (退出路径由 p_book_exit 显式复位)。 */
}

static void book_rotation_ensure(ui_ctx_t *ctx) {
    int rot = s_bs_rot & 3;
    bool portrait = (rot == 2 || rot == 3);
    ui_set_rotation_mode(rot);   /* 宽高判据统一由 ui_common 推导, 本页不重复写 */
    if (portrait && !s_book_pfb)
        s_book_pfb = heap_caps_malloc(40 * 400, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rot == 1 && !s_book_flip)
        s_book_flip = heap_caps_malloc(15000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx && ctx->lcd)
        st7305_set_rotation(ctx->lcd, rot, portrait ? s_book_pfb : s_book_flip);
    s_bp.list_y = portrait ? (24 + 4) : 30;   /* 竖屏: 状态栏(24)下方; 横屏: 状态栏(24)+4 */
    /* ★ 2026-11-22: 触摸坐标换算交 input 层 (rotate_screen_coord 与本页旧
     *   book_touch_map 的 rot1/2/3 三态公式逐字一致 ⇒ 命中与弹窗坐标完全等价),
     *   这样**手势层也拿到同一套逻辑坐标**, 竖屏上滑返回才成立 (详见上文注释)。
     *   这里每帧都刷新一次, 覆盖"设置里改旋转方向"的即时生效路径。 */
    input_set_screen_rotation(rot);
    if (ctx) ctx->map_touch = NULL;
}

/* 字号三档 (linit 2026-10-14 定):
 *   0 = 16px  → 原生 font_zh16 (宋体), 最清晰
 *   1 = 24px  → 原生 font_zh  (粗宋), 最清晰
 *   2 = 32px  → font_zh16 的 **2 倍整数放大** (笔画干净; 不用 24→32 的 1.33 倍, 那会断笔)
 * ⛔ 20px / 28px 已去掉 —— 它们是"既非原生、又非整数倍"的两档, 缩放最糊。 */
static const char *bs_fs_name(void) {
    /* ★ 2026-10-31: 不再写死 16/24/32/48，而是问**当前选中的字体**真实支持的字号。
     *   内置字体 = 16/24/32/48；TF 卡 LBF = 原生 N 与 2N（8px 字体只有 8/16）。
     *   注意这里用的是 font_book_size_px_for(path) —— 按**字体文件头**算，
     *   而不是按"当前已加载的字体"算：站在书库时 LBF 往往还没加载，
     *   用 font_book_size_px_at() 会永远返回内置四档。
     *   统一显示成 "16px" 形式，避免"菜单写 24、实际画 8"的错位。 */
    static char b[10];
    snprintf(b, sizeof(b), "%dpx", font_book_size_px_for(bs_font_path(), s_bs_fontsize));
    return b;
}
/* → 阅读器档位。两边现在是**同一套** 0..3 = 16/24/32/48，直接透传。
 * ⚠ 旧实现查的是 {0,2,4}，而阅读器把档位夹在 0..3 ⇒ 32px 档(4) 被夹成 3(24px)，
 *   菜单写 32、实际画 24。现在档位一一对应，不再有夹值。 */
static int bs_size_reader_idx(void) {
    /* 档位 = 当前字体字号表里的下标；越界由阅读器按"最大合法档"收敛
     * （font_book_size_px_at），不会再出现"菜单 32、实际 24"。 */
    return (s_bs_fontsize < 0) ? 0 : s_bs_fontsize;
}
static const char *bs_font_name(void) {
    /* 2026-10-31: 也走中文短名, 与滚轮/阅读器菜单保持一致 */
    static char s_disp[24];
    if (s_bs_tfname[0]) return font_book_short_name(s_bs_tfname, s_disp, sizeof(s_disp));
    return "\xe5\x86\x85\xe7\xbd\xae\xe5\xad\x97\xe4\xbd\x93";   /* 内置字体 */
}
static const char *bs_on_off(int v) { return v ? "\xe5\xbc\x80" : "\xe5\x85\xb3"; } /* 开/关 */
static const char *bs_mg_name(void) {
    switch (s_bs_margin) { case 0: return "\xe6\x94\xb6\xe7\xaa\x84"; /*收窄*/ case 2: return "\xe5\x8a\xa0\xe5\xae\xbd"; /*加宽*/ default: return "\xe4\xb8\xad\xe7\xad\x89"; /*中等*/ }
}
static const char *bs_lh_name(void) {
    switch (s_bs_lineh) { case 0: return "\xe7\xb4\xa7\xe5\x87\x91"; /*紧凑*/ case 2: return "\xe5\xae\xbd\xe6\x9d\xbe"; /*宽松*/ default: return "\xe6\xa0\x87\xe5\x87\x86"; /*标准*/ }
}
static const char *bs_gap_name(void) {
    switch (s_bs_gap) { case 0: return "\xe7\xb4\xa7\xe5\x87\x91"; /*紧凑*/ case 2: return "\xe5\xae\xbd\xe6\x9d\xbe"; /*宽松*/ default: return "\xe6\xa0\x87\xe5\x87\x86"; /*标准*/ }
}

static const char *bs_rot_name(void) {
    switch (s_bs_rot) { case 1: return "\xe4\xb8\x8b"; /*下*/ case 2: return "\xe5\xb7\xa6"; /*左*/ case 3: return "\xe5\x8f\xb3"; /*右*/ default: return "\xe4\xb8\x8a"; /*上*/ }
}

/* 旋转方向: 上(0)->左(2)->下(1)->右(3) (逆时针 90°) */
static int rot_next_cw(int rot) {
    static const int seq[4] = { 0, 2, 1, 3 };   /* 上->左->下->右 (逆时针 90°) */
    int i = 0;
    while (i < 4 && seq[i] != (rot & 3)) i++;
    return seq[(i + 1) & 3];
}
/* 阅读优化右栏: **名称贴左 / 数值贴右** 两列排版 (选中整行反色)。
 * ★ 2026-11-22 linit「右侧太空」根因:
 *   旧实现先用全角空格把名称补成 4 字, 拼成 "名称: 值" 一整串, 再按**可见行里最长的
 *   那一行**把整块水平居中 ⇒ 右栏有 288px, 而最长行只有 ~192px, 两侧各空 48px;
 *   而且每行数值长短不同(「旋转方向: 左」比「字号大小: 24px」短两个字), 居中后右侧
 *   还各挂一段空白 ⇒ 看上去右半边整片是空的。竖屏(300 宽)时右栏只有 188px, 反而
 *   会溢出屏幕。现在改为两列对齐: 名称贴 rx0+8, 数值右对齐到 rx1-8, 整栏宽度用满,
 *   行内空白全部留在"名称与数值之间"。
 * ⚠ 不要再改回"补全角空格 + 整块居中" —— 那必然重新出现右侧大片空白。 */
static const char *bs_item_name(int idx) {
    switch (idx) {
    case 0: return "\xe5\xad\x97\xe4\xbd\x93\xe5\xa4\xa7\xe5\xb0\x8f";             /* 字号大小 */
    case 1: return "\xe5\xad\x97\xe4\xbd\x93";                                      /* 字体 */
    case 2: return "\xe6\x98\xbe\xe7\xa4\xba\xe8\xbf\x9b\xe5\xba\xa6";             /* 显示进度 */
    case 3: return "\xe8\xbe\xb9\xe8\xb7\x9d";                                      /* 边距 */
    case 4: return "\xe8\xa1\x8c\xe9\xab\x98";                                      /* 行高 */
    case 5: return "\xe5\xad\x97\xe8\xb7\x9d";                                      /* 字距 */
    case 6: return "\xe6\xae\xb5\xe8\x90\xbd\xe7\xbc\xa9\xe8\xbf\x9b";             /* 段落缩进 */
    case 7: return "\xe9\x9c\x87\xe5\x8a\xa8";                                      /* 震动 */
    default: return "\xe6\x97\x8b\xe8\xbd\xac\xe6\x96\xb9\xe5\x90\x91";            /* 旋转方向 */
    }
}
static const char *bs_item_value(int idx) {
    switch (idx) {
    case 0: return bs_fs_name();
    case 1: return bs_font_name();
    case 2: return bs_on_off(s_bs_pagenum);
    case 3: return bs_mg_name();
    case 4: return bs_lh_name();
    case 5: return bs_gap_name();
    case 6: return bs_on_off(s_bs_indent);
    case 7: return bs_on_off(s_bs_vib);       /* 震动开关 (2026-11-22) */
    default: return bs_rot_name();
    }
}

/* 右栏: 名称贴左 + 数值贴右 (选中整行反色)。行高 32 / 反色块 30 与原实现一致 */
static void p_book_settings_render(ui_ctx_t *ctx, os_pane_t *p, int rx0, int rx1,
                                   int list_y, int list_bottom, int top_row, int off_mod) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    const int pad    = 8;                 /* 与栏边留 8px, 不贴死分割线 */
    const int x_name = rx0 + pad;
    const int x_right = rx1 - pad;
    for (int i = 0; i < p->settings_count; i++) {
        int idx = top_row + i;
        if (idx >= p->settings_count) break;
        int row_y = list_y + i * 32 - off_mod;   /* 平滑跟手滚动 (游戏菜单同款) */
        if (row_y + 32 < list_y || row_y >= list_bottom) continue;
        bool sel = (p->focus == 1 && idx == p->sel_item);
        if (sel) fill_rect(lcd, rx0, row_y, rx1, row_y + 30, ST7305_COLOR_BLACK);
        draw_text(lcd, x_name, row_y + 4, bs_item_name(idx), sel);
        const char *val = bs_item_value(idx);
        int vx = x_right - text_width(val);
        /* 名称最宽 4 字(96px): 数值至少离名称右沿 8px, 极窄栏位下宁可右侧截止也不叠字 */
        int vx_min = x_name + 4 * 24 + 8;
        if (vx < vx_min) vx = vx_min;
        draw_text(lcd, vx, row_y + 4, val, sel);
    }
}

/* TF 卡字体文件夹扫描: 返回字体文件数 (存 s_tf_fonts)
 * ★ 2026-10-31: 结果缓存，一次进页面只扫一次（原来每次打开字体滚轮都 readdir 一遍）。
 *   字体装/卸/拷贝后由 p_book_enter() 置脏，下次进入书库重新扫。 */
EXT_RAM_BSS_ATTR static char s_tf_fonts[16][64];
static int  s_tf_font_n = 0;
static bool s_tf_font_dirty = true;
static void tf_font_scan(void) {
    if (!s_tf_font_dirty) return;
    s_tf_font_dirty = false;
    s_tf_font_n = 0;
    DIR *d = opendir("/sdcard/fonts");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_tf_font_n < 16) {
        if (e->d_name[0] == '.') continue;
        const char *ext = strrchr(e->d_name, '.');
        if (ext && strcasecmp(ext, ".lbf") == 0)
            snprintf(s_tf_fonts[s_tf_font_n++], 64, "%.63s", e->d_name);
    }
    closedir(d);
}

/* ==== 自适应滚轮弹窗 (字号/字体): 尺寸随内容伸缩, 单列纵向循环滚动 ==== */
#define BS_WHEEL_SIDE  2
#define BS_WHEEL_STEP  30
static const char *s_bs_wheel_opts[24];   /* 选项文字 (UTF-8) */
static int   s_bs_wheel_n = 0;
static int   s_bs_wheel_idx = 0;
static void (*s_bs_wheel_commit)(void);   /* 确认提交回调 */

/* 自适应几何: 宽=最长选项+边距, 高=可见行+按钮区, 均夹在屏幕内 */
static void bs_wheel_geom(int *bx, int *by, int *W, int *H) {
    int mw = 44;
    for (int i = 0; i < s_bs_wheel_n; i++) {
        int tw = text_width(s_bs_wheel_opts[i]);
        if (tw > mw) mw = tw;
    }
    int w = mw + 48;
    if (w < 140) w = 140;
    if (w > ui_screen_w() - 12) w = ui_screen_w() - 12;
    int h = (2 * BS_WHEEL_SIDE + 1) * BS_WHEEL_STEP + 50;
    if (h < 150) h = 150;
    if (h > ui_screen_h() - 12) h = ui_screen_h() - 12;
    *W = w; *H = h;
    *bx = (ui_screen_w() - w) / 2;
    *by = (ui_screen_h() - h) / 2;
}

/* 按像素宽截断 UTF-8 文本 (至少保留 1 个完整字符) */
static void bs_wheel_fit(const char *s, int maxw, char *out, size_t n) {
    size_t w = 0;
    int i = 0;
    while (s[i] && i + 1 < (int)n) {
        unsigned char c = (unsigned char)s[i];
        int cl = (c < 0x80) ? 1 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xE0) == 0xC0) ? 2 : 4;
        int cw = (c < 0x80) ? font_book_ascii_w() : font_book_cell_w();
        if (w > 0 && w + (size_t)cw > (size_t)maxw) break;
        for (int k = 0; k < cl && s[i + k] && (int)w + k + 1 < (int)n; k++) out[w++] = s[i + k];
        i += cl;
    }
    out[w] = 0;
}

static bool bs_wheel_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    int bx, by, W, H; bs_wheel_geom(&bx, &by, &W, &H);
    fill_rect(lcd, bx, by, bx + W - 1, by + H - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + W - 1 - k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
    }
    const int bbt = by + H - 2 - 44;   /* 底部按钮区顶 */
    const int cy = by + 2 + (bbt - (by + 2)) / 2;
    const int y0 = cy - 13;
    int ccx = bx + W / 2;
    int n = s_bs_wheel_n; if (n < 1) n = 1;
    char tmp[40];
    for (int k = -BS_WHEEL_SIDE; k <= BS_WHEEL_SIDE; k++) {
        if (k == 0) continue;
        int idx = (s_bs_wheel_idx + k + n * 4) % n;
        bs_wheel_fit(s_bs_wheel_opts[idx], W - 24, tmp, sizeof(tmp));
        draw_text(lcd, ccx - text_width(tmp) / 2, y0 + k * BS_WHEEL_STEP, tmp, false);
    }
    /* 当前值黑框反色 */
    bs_wheel_fit(s_bs_wheel_opts[s_bs_wheel_idx], W - 24, tmp, sizeof(tmp));
    int vw = text_width(tmp);
    fill_rect(lcd, ccx - vw / 2 - 4, y0 - 3, ccx + vw / 2 + 3, y0 + 25, ST7305_COLOR_BLACK);
    draw_text(lcd, ccx - vw / 2, y0, tmp, true);
    /* 底部 确认/取消 */
    draw_hline(lcd, bx + 2, bx + W - 1 - 2, bbt, ST7305_COLOR_BLACK);
    int mid = bx + W / 2;
    draw_vline(lcd, mid, bbt, by + H - 1 - 2, ST7305_COLOR_BLACK);
    int ty = bbt + 10;
    draw_text(lcd, bx + W / 4 - text_width("\xe7\xa1\xae\xe8\xae\xa4") / 2, ty, "\xe7\xa1\xae\xe8\xae\xa4", false);   /* 确认 */
    draw_text(lcd, bx + W * 3 / 4 - text_width("\xe5\x8f\x96\xe6\xb6\x88") / 2, ty, "\xe5\x8f\x96\xe6\xb6\x88", false);  /* 取消 */
    return true;
}

static bool bs_wheel_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)d; (void)ud;
    int n = s_bs_wheel_n; if (n < 1) n = 1;
    switch (a) {
    case OS_ACTION_UP:   s_bs_wheel_idx = (s_bs_wheel_idx + 1) % n; ctx->needs_redraw = true; return true;
    case OS_ACTION_DOWN: s_bs_wheel_idx = (s_bs_wheel_idx + n - 1) % n; ctx->needs_redraw = true; return true;
    case OS_ACTION_CONFIRM: if (s_bs_wheel_commit) s_bs_wheel_commit(); os_dialog_pop(ctx); return true;
    default: return false;   /* BACK → 默认取消 */
    }
}

static void bs_wheel_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    int tx, ty;
    static int s_w_last = -1, s_w_acc = 0;
    if (input_get_touch_pos(&tx, &ty)) {
        /* ★ 2026-11-22: input_get_touch_pos() 返回的就是**已按 s_screen_rot 旋转的
         *   逻辑坐标** (input_poll_touch / input_touch_resample 两个写入者都调了
         *   rotate_screen_coord)。书库现在把旋转交给 input, 所以这里直接使用,
         *   不能再映射一次 —— 否则竖屏滚轮会朝反方向跳。
         *   (旧实现书库 s_screen_rot 恒为 0, 才必须在这里手动物理→逻辑。) */
        if (s_w_last < 0) { s_w_last = ty; s_w_acc = 0; }
        else {
            s_w_acc += ty - s_w_last;
            s_w_last = ty;
            int n = s_bs_wheel_n; if (n < 1) n = 1;
            bool changed = false;
            while (s_w_acc <= -BS_WHEEL_STEP) { s_bs_wheel_idx = (s_bs_wheel_idx + 1) % n; s_w_acc += BS_WHEEL_STEP; changed = true; }
            while (s_w_acc >= +BS_WHEEL_STEP) { s_bs_wheel_idx = (s_bs_wheel_idx + n - 1) % n; s_w_acc -= BS_WHEEL_STEP; changed = true; }
            if (changed) ctx->needs_redraw = true;
        }
    } else {
        s_w_last = -1; s_w_acc = 0;
    }
}

static bool bs_wheel_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)d; (void)ud;
    int bx, by, W, H; bs_wheel_geom(&bx, &by, &W, &H);
    if (x < bx || x > bx + W - 1 || y < by || y > by + H - 1) return false;
    const int bbt = by + H - 2 - 44;
    if (y >= bbt) {
        int mid = bx + W / 2;
        if (x < mid && s_bs_wheel_commit) s_bs_wheel_commit();
        os_dialog_pop(ctx);
        return true;
    }
    return true;   /* 弹窗内其他区域吞掉 (拖动调值由 poll 处理) */
}

static ui_ctx_t *s_wheel_ctx;   /* 滚轮弹窗的 ctx：commit 回调没有 ctx 参数，靠这个拿 */

static void bs_wheel_push(ui_ctx_t *ctx) {
    s_wheel_ctx = ctx;
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.no_footer = true;
    dlg.on_render = bs_wheel_render;
    dlg.on_key    = bs_wheel_key;
    dlg.on_poll   = bs_wheel_poll;
    dlg.on_touch  = bs_wheel_touch;
    os_dialog_push(ctx, &dlg);
}

/* 字号：**按当前选中字体动态生成**（内置 16/24/32/48；8px 点阵字体只有 8/16）。
 * 用 font_book_size_options_for(路径) —— 站在书库时 LBF 还没加载也能拿到真实规格。
 * 静态缓冲按条目保存 —— 滚轮长期持有 opts 指针，不能指向临时栈。 */
static char s_bs_size_disp[8][8];
static void bs_wheel_commit_size(void) { s_bs_fontsize = s_bs_wheel_idx; bs_save(); bs_apply(); }
static void bs_size_wheel_open(ui_ctx_t *ctx) {
    int px[8];
    int n = font_book_size_options_for(bs_font_path(), px, 8);
    if (n <= 0) { px[0] = 24; n = 1; }
    for (int i = 0; i < n; i++) {
        snprintf(s_bs_size_disp[i], sizeof(s_bs_size_disp[i]), "%dpx", px[i]);
        s_bs_wheel_opts[i] = s_bs_size_disp[i];
    }
    s_bs_wheel_n = n;
    s_bs_wheel_idx = (s_bs_fontsize < 0) ? 0 : (s_bs_fontsize >= n ? n - 1 : s_bs_fontsize);
    s_bs_wheel_commit = bs_wheel_commit_size;
    bs_wheel_push(ctx);
}

/* ===== 字体后台加载（pin 到 core 0，UI 在 core 1 不受影响）=====
 * 起因：lbf_open 一次性整块读 TF 卡（0.3~0.7MB，1~2 秒），在 UI 任务里同步跑
 * ⇒ 界面完全卡死、连"正在加载"都画不出来。
 * 现在：丢给独立任务在另一个核读，UI 立刻弹提示窗；读完后由 poll 收尾并应用设置。 */
static volatile int s_font_load_st;        /* 0=空闲 1=加载中 2=成功 3=失败 */
static volatile int s_font_load_gen;       /* ★ 代次: 页面退出时自增, 让在飞任务的结果作废 */
static char         s_font_load_path[128];
static char         s_font_load_name[64];

static void font_load_task(void *arg)
{
    int gen = (int)(intptr_t)arg;
    bool ok = font_book_select_file(s_font_load_path);
    /* ★ 2026-11-22: 页面已经退出(代次变了) ⇒ 只把状态归零, 不回报成功/失败。
     *   否则下次进阅读器时 poll 会把上一次的结果当成"刚加载好"再应用一遍,
     *   字体设置会莫名其妙被改掉。s_font_load_st 已在 exit 里清 0, 这里不能再写 2/3。 */
    s_font_load_st = (gen == s_font_load_gen) ? (ok ? 2 : 3) : 0;
    vTaskDelete(NULL);
}

/* ★ 2026-11-22: 字体载入进度窗（用户要求"载入字体中 + 进度条"）
 *   进度来自 lbf_open() 的 32KB 分块读（font_book_load_percent()）。
 *   只用内置 16px 字绘制，绝不依赖正在加载的那套字库（否则会取字形死等）。 */
static bool font_wait_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud)
{
    (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    const int w = 264, h = 116;
    int x = (UI_SCREEN_W - w) / 2, y = (UI_SCREEN_H - h) / 2;

    fill_rect(lcd, x, y, x + w - 1, y + h - 1, ST7305_COLOR_WHITE);
    draw_dialog_frame(lcd, x, y, w, h, 2);
    ui_text16c(lcd, x + w / 2, y + 10, d->title, false);

    int pct = font_book_load_percent();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    int bx = x + 24, by = y + 40, bw = w - 48, bh = 18;
    draw_rect_outline(lcd, bx, by, bx + bw - 1, by + bh - 1, ST7305_COLOR_BLACK);
    int inner = bw - 4;
    int fillw = inner * pct / 100;
    if (fillw > 0)
        fill_rect(lcd, bx + 2, by + 2, bx + 2 + fillw - 1, by + bh - 3, ST7305_COLOR_BLACK);

    char buf[32];
    snprintf(buf, sizeof(buf), "载入字体 %d%%", pct);
    ui_text16c(lcd, x + w / 2, by + bh + 6, buf, false);
    return true;      /* 已消费：不走默认列表渲染 */
}

static bool font_wait_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud)
{
    (void)ctx; (void)d; (void)a; (void)ud;
    return true;      /* 加载中吞掉按键：中途退出会留下半加载状态 */
}

/* 加载完成收尾：应用字体设置 + 关提示窗（在 UI 任务里执行，安全） */
static void font_wait_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud)
{
    (void)d; (void)ud;
    if (s_font_load_st == 1) {                     /* 还在读卡 */
        ctx->needs_redraw = true;                  /* ★ 进度在动 → 持续重绘 */
        return;
    }
    if (s_font_load_st == 2) {
        s_bs_font = 1;
        strncpy(s_bs_tfname, s_font_load_name, sizeof(s_bs_tfname) - 1);
        s_bs_tfname[sizeof(s_bs_tfname) - 1] = 0;
        int cnt = font_book_size_count();
        if (cnt > 0 && s_bs_fontsize >= cnt) s_bs_fontsize = cnt - 1;
        bs_save(); bs_apply();
    } else if (s_font_load_st == 3) {
        ESP_LOGW(TAG, "字体加载失败: %s", font_book_error());
    }
    s_font_load_st = 0;
    os_dialog_pop(ctx);
    ctx->needs_redraw = true;
}

static void font_wait_push(ui_ctx_t *ctx)
{
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.title, sizeof(dlg.title), "字体");
    dlg.count = 1;
    snprintf(dlg.items[0], sizeof(dlg.items[0]), "正在载入字体…");
    dlg.sel = -1;
    dlg.no_footer = true;
    dlg.on_render = font_wait_render;   /* ★ 自绘: 进度条 + 百分比 */
    dlg.on_key    = font_wait_key;      /* ★ 加载中吞按键 */
    dlg.on_poll = font_wait_poll;
    os_dialog_push(ctx, &dlg);
}

/* 字体: 内置 + TF 字库（与阅读器内「字体选择」是同一份状态：都认 NVS 的 ftn） */
static void bs_wheel_commit_font(void) {
    int idx = s_bs_wheel_idx;
    if (idx < 1) {
        s_bs_font = 0;     /* 0=内置字体 */
        s_bs_tfname[0] = 0;
        bs_save(); bs_apply();
        return;
    }
    int fi = idx - 1;
    if (fi < 0 || fi >= s_tf_font_n) return;

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/fonts/%s", s_tf_fonts[fi]);

    /* 诊断：确认到底走哪条路（用户反馈"没弹窗直接卡一下切换"） */
    ESP_LOGW(TAG, "字体提交: idx=%d name=%s cached=%d st=%d ctx=%p",
             idx, s_tf_fonts[fi], (int)font_book_is_cached(path),
             s_font_load_st, (void *)s_wheel_ctx);

    /* 已缓存（最近用过 3 个之内）→ 同步切换，零 IO、无提示 */
    if (font_book_is_cached(path)) {
        if (font_book_select_file(path)) {
            s_bs_font = 1;
            strncpy(s_bs_tfname, s_tf_fonts[fi], sizeof(s_bs_tfname) - 1);
            s_bs_tfname[sizeof(s_bs_tfname) - 1] = 0;
            int cnt = font_book_size_count();
            if (cnt > 0 && s_bs_fontsize >= cnt) s_bs_fontsize = cnt - 1;
            bs_save(); bs_apply();
        }
        return;
    }

    /* 未缓存 → 后台核加载 + 提示窗 */
    if (s_font_load_st == 1) return;              /* 已有一次在跑，忽略连点 */
    snprintf(s_font_load_path, sizeof(s_font_load_path), "%s", path);
    snprintf(s_font_load_name, sizeof(s_font_load_name), "%s", s_tf_fonts[fi]);
    s_font_load_st = 1;

    ui_ctx_t *ctx = s_wheel_ctx;
    ESP_LOGW(TAG, "字体: 走异步加载, ctx=%p", (void *)ctx);
    if (!ctx) {                                   /* 没有 ctx 就别冒风险，直接同步 */
        s_font_load_st = 0;
        ESP_LOGE(TAG, "字体: 无 ctx, 同步加载");
        if (font_book_select_file(path)) {
            s_bs_font = 1;
            strncpy(s_bs_tfname, s_tf_fonts[fi], sizeof(s_bs_tfname) - 1);
            s_bs_tfname[sizeof(s_bs_tfname) - 1] = 0;
            bs_save(); bs_apply();
        }
        return;
    }
    font_wait_push(ctx);                          /* 先把提示画出来 */
    /* 把当前代次带进任务: 页面在加载中途退出时代次会被 +1, 任务据此丢弃结果 */
    if (xTaskCreatePinnedToCore(font_load_task, "fontload", 4096,
                                (void *)(intptr_t)s_font_load_gen, 2, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "字体加载任务创建失败, 回退同步");
        s_font_load_st = 0;
        os_dialog_pop(ctx);
        if (font_book_select_file(path)) {
            s_bs_font = 1;
            strncpy(s_bs_tfname, s_tf_fonts[fi], sizeof(s_bs_tfname) - 1);
            s_bs_tfname[sizeof(s_bs_tfname) - 1] = 0;
            bs_save(); bs_apply();
        }
    }
}
/* 2026-10-31: 滚轮显示**中文短名**（原来直接列 "fusion8.lbf" 这种文件名）。
 *   映射与阅读器菜单共用 font_book_short_name(), 两处名字永远一致。
 *   显示缓冲按条目静态保存 —— 滚轮长期持有 opts 指针, 不能返回临时栈缓冲。 */
static char s_bs_font_disp[24][24];
static void bs_font_wheel_open(ui_ctx_t *ctx) {
    tf_font_scan();
    int n = 0;
    int sel = 0;
    s_bs_wheel_opts[n++] = "\xe5\x86\x85\xe7\xbd\xae\xe5\xad\x97\xe4\xbd\x93";  /* 内置字体 */
    for (int i = 0; i < s_tf_font_n && n < 24; i++) {
        font_book_short_name(s_tf_fonts[i], s_bs_font_disp[i], sizeof(s_bs_font_disp[i]));
        s_bs_wheel_opts[n++] = s_bs_font_disp[i];
        /* 选中项按**文件名**定位，不按序号 —— 序号会随目录增删错位 */
        if (s_bs_tfname[0] && strcmp(s_bs_tfname, s_tf_fonts[i]) == 0) sel = i + 1;
    }
    s_bs_wheel_n = n;
    s_bs_wheel_idx = sel;
    s_bs_wheel_commit = bs_wheel_commit_font;
    bs_wheel_push(ctx);
}

static void p_book_settings_select(ui_ctx_t *ctx, os_pane_t *p, int item) {
    (void)p;
    switch (item) {
    case 0: bs_size_wheel_open(ctx); return;    /* 字号(16/24/32) → 滚轮 */
    case 1: bs_font_wheel_open(ctx); return;    /* 字体(仿宋/黑体/TF 字库) → 滚轮 */
    case 2: s_bs_pagenum ^= 1; break;           /* 显示进度 点击切换 */
    case 3: s_bs_margin = (s_bs_margin + 1) % 3; break;   /* 边距 点击循环 */
    case 4: s_bs_lineh = (s_bs_lineh + 1) % 3; break;     /* 行高 点击循环 */
    case 5: s_bs_gap = (s_bs_gap + 1) % 3; break;       /* 字距 紧凑/标准/宽松 */
    case 6: s_bs_indent ^= 1; break;            /* 段落缩进 点击切换 */
    case 7: s_bs_vib ^= 1; break;               /* 震动 开↔关 (阅读页点按/翻页反馈, 2026-11-22) */
    default: s_bs_rot = rot_next_cw(s_bs_rot); break;   /* 旋转方向 顺时针+90° */
    }
    bs_save(); bs_apply();
    ctx->needs_redraw = true;
}

/* ==== 书架自包含渲染/触摸/拖动 (不再复用 os_pane 模板的渲染与触摸) ====
 * 保留 os_pane 仅做目录扫描与数据填充 (os_pane_reset/build_right), 
 * 渲染走 st7305 逻辑坐标 (横屏 400×300 / 竖屏 300×400, 与阅读器同源),
 * 触摸与拖动由 input 层按旋转映射为逻辑坐标后直接命中 (与渲染坐标系一致),
 * 底部上滑返回 zone 也随 input_set_screen_rotation 自动跟随显示方向. */
#define SL_ROW_H  32
#define SL_LEFT_W 100
#define SL_GAP    6

/* 左栏总项数 (同 os_pane.total) */
static int book_left_total(void) {
    int n = s_bp.folder_count;
    if (s_bp.settings_label) n++;
    if (s_bp.recent_label) n++;
    if (s_bp.fav_enabled) n++;
    if (!s_bp.hide_all) n++;
    return n;
}

/* UTF-8 截断到最多 max_chars 个字符 (防超长目录名溢出) */

/* 左栏显示名 */

/* 右栏平滑滚动最大偏移 */
static int __attribute__((unused)) book_max_off(void) {
    int list_y = s_bp.list_y > 0 ? s_bp.list_y : 30;
    int total = s_bp.settings_mode ? s_bp.settings_count : s_bp.item_count;
    int mo = total * SL_ROW_H - (ui_screen_h() - 4 - list_y);
    return mo < 0 ? 0 : mo;
}

/* 渲染双栏 (逻辑坐标; st7305 旋转在 flush 时按当前 target 完成) */
static void book_shelf_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    const int list_y = s_bp.list_y > 0 ? s_bp.list_y : 30;
    const int list_bot = ui_screen_h() - 4;
    const int sw = ui_screen_w();
    int lx0 = 2, lx1 = lx0 + SL_LEFT_W - 4;
    int rx0 = lx1 + SL_GAP + 4;
    const int right_edge = sw - 4;

    /* 左栏: 统一 os_pane 模板 (文字 + 水管分割线 + 选中框), 与游戏/密码机一致 */
    os_pane_draw_left(lcd, &s_bp);

    /* 右栏: 设置模式走自定义渲染 */
    if (s_bp.settings_mode) {
        int off2 = s_bp.item_off; if (off2 < 0) off2 = 0;
        int top2 = off2 / SL_ROW_H, mod2 = off2 % SL_ROW_H;
        if (s_bp.settings_render)
            s_bp.settings_render(ctx, &s_bp, rx0, right_edge, list_y, list_bot, top2, mod2);
        return;
    }
    /* 右栏: 内容, 选中整行反色, 平滑滚动 */
    int view_h = list_bot - list_y;
    int max_vis = view_h / SL_ROW_H;
    if (max_vis < 1) max_vis = 1;
    int off = s_bp.item_off; if (off < 0) off = 0;
    int top_row = off / SL_ROW_H, off_mod = off % SL_ROW_H;
    int n_vis = (s_bp.item_count - top_row) > (max_vis + 1) ? (max_vis + 1) : (s_bp.item_count - top_row);
    for (int i = 0; i < n_vis; i++) {
        int idx = top_row + i;
        bool sel = (s_bp.focus == 1 && idx == s_bp.sel_item);
        int py = list_y + i * SL_ROW_H - off_mod;
        if (sel && py + SL_ROW_H - 2 >= list_y && py < list_bot) {
            int fy = py < list_y ? list_y : py;
            int ty = py + SL_ROW_H - 2; if (ty > list_bot - 1) ty = list_bot - 1;
            fill_rect(lcd, rx0, fy, right_edge, ty, ST7305_COLOR_BLACK);
        }
        if (py + SL_ROW_H < list_y || py >= list_bot) continue;
        int tw = text_width(s_bp.items[idx]);
        int tx = rx0 + ((right_edge - rx0) - tw) / 2;
        if (tx < rx0) tx = rx0;
        draw_text(lcd, tx, py + 4, s_bp.items[idx], sel);
    }
    /* 收藏栏/最近阅读为空提示: 右栏内居中 */
    if (s_bp.fav_mode && s_bp.item_count == 0) {
        const char *tip = s_bp.fav_empty_tip ? s_bp.fav_empty_tip :
            "\xe9\x95\xbf\xe6\x8c\x89\xe7\xa1\xae\xe8\xae\xa4\xe6\xb8\xb8\xe6\x88\x8f\xe5\x8a\xa0\xe5\x85\xa5\xe6\x94\xb6\xe8\x97\x8f";
        int tyy = list_y + view_h / 2 - 12;
        int tw = text_width(tip);
        int tx = rx0 + ((right_edge - rx0) - tw) / 2; if (tx < rx0) tx = rx0;
        draw_text(lcd, tx, tyy, tip, false);
    }
    if (s_bp.recent_mode && s_bp.item_count == 0) {
        const char *tip = "\xe6\x9c\x80\xe8\xbf\x91\xe6\x97\xa0\xe9\x98\x85\xe8\xaf\xbb\xe8\xae\xb0\xe5\xbd\x95";
        int tyy = list_y + view_h / 2 - 12;
        int tw = text_width(tip);
        int tx = rx0 + ((right_edge - rx0) - tw) / 2; if (tx < rx0) tx = rx0;
        draw_text(lcd, tx, tyy, tip, false);
    }
}

/* 触摸命中 (坐标**已由 input 层按 s_bs_rot 旋转成逻辑坐标**, 与渲染同系) */
static bool book_shelf_touch(ui_ctx_t *ctx, int x, int y) {
    const int list_y = s_bp.list_y > 0 ? s_bp.list_y : 30;
    if (y < list_y || y >= ui_screen_h() - 4) return false;
    if (x < SL_LEFT_W) {
        int idx = s_bp.folder_scroll + (y - list_y) / SL_ROW_H;
        if (idx < book_left_total()) {
            if (s_bp.sel_folder != idx) {
                s_bp.sel_folder = idx;
                s_bp.focus = 0;
                os_pane_build_right(&s_bp);
            }
            ctx->needs_redraw = true;
            return true;
        }
        return false;
    }
    if (s_bp.settings_mode) {
        int idx = (s_bp.item_off + (y - list_y)) / SL_ROW_H;
        if (idx >= s_bp.settings_count) return false;
        s_bp.sel_item = idx;
        s_bp.focus = 1;
        ctx->needs_redraw = true;
        if (s_bp.on_settings_select) s_bp.on_settings_select(ctx, &s_bp, idx);
        return true;
    }
    int idx = (s_bp.item_off + (y - list_y)) / SL_ROW_H;
    if (idx >= s_bp.item_count) return false;
    s_bp.sel_item = idx;
    s_bp.focus = 1;
    ctx->needs_redraw = true;
    if (s_bp.on_select) s_bp.on_select(ctx, &s_bp, s_bp.item_paths[idx], s_bp.items[idx]);
    return true;
}

/* 每帧: 右栏触摸拖动滚动 —— ★★★ 2026-10-24 统一交给 ui_scroll
 *   (1:1 跟手 + 惯性×0.86 + 越界回弹 + 松手吸附整行)。
 *   原来自己维护 s_last_y/s_start_off 做 1:1，但没有惯性也没有回弹。 */
static void book_shelf_drag(ui_ctx_t *ctx) {
    static ui_scroll_t sc;
    static bool ready = false, started_right = false;
    const int list_y = s_bp.list_y > 0 ? s_bp.list_y : 30;
    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    /* 2026-11-22: 坐标已是 input 层旋转后的逻辑坐标, 不再页内映射 (见 book_rotation_ensure) */

    const int vh = ui_screen_h() - list_y - 4;
    /* ★ 2026-11-22: 设置模式的行数也要参与滚动 —— 阅读优化加到 9 项后
     *   横屏(可用 266px)放不下 288px, 原来 content 恒按 item_count 算 ⇒ 末项
     *   (旋转方向) 永远滚不到. */
    const int list_n = s_bp.settings_mode ? s_bp.settings_count : s_bp.item_count;
    const int content = list_n * SL_ROW_H;
    if (!ready) { ui_scroll_config(&sc, UI_SCROLL_LIST, 0); ready = true; }
    /* ★ 2026-11-22: 左右栏切换 (目录 ⇄ 阅读优化) 必须把滚动归零.
     *   ui_scroll_init 只夹取不清零 ⇒ 在目录里滚过的偏移会被带进设置页,
     *   top2/off_mod 按它算 ⇒ 阅读优化第一行被顶出上看不见. */
    static bool last_settings = false;
    if (s_bp.settings_mode != last_settings) {
        last_settings = s_bp.settings_mode;
        ui_scroll_to_top(&sc);
        s_bp.item_off = 0;
    }
    /* ★ 2026-11-23: 统一 ui_scroll_update —— 目录/设置项数量变化、视口变化都走它:
     *   内容变了只软更新(保留拖动/惯性/位置), 几何变了才重配。 */
    ui_scroll_update(&sc, SL_ROW_H, vh, content);

    if (down && !sc.pressed) {
        started_right = (tx >= SL_LEFT_W &&
                         ty >= list_y && ty < ui_screen_h() - 4);
        if (started_right) {
            /* 设置模式不做"按下即选中" (否则拖动第一行就被当成点选), 选中交给
             * book_shelf_touch 的点击判定 —— 那里本来就按行高换算 idx. */
            int hidx = (s_bp.item_off + (ty - list_y)) / SL_ROW_H;
            if (!s_bp.settings_mode && hidx >= 0 && hidx < s_bp.item_count) {
                if (s_bp.sel_item != hidx || s_bp.focus != 1) {
                    s_bp.sel_item = hidx;
                    s_bp.focus = 1;
                    if (ctx) ctx->needs_redraw = true;
                }
            }
        } else {
            /* 右栏外按下: 只登记 pressed, 不参与滚动 */
            ui_scroll_touch_xy(&sc, tx, ty, true);
        }
    }
    if (down && started_right) {
        if (ui_scroll_touch_xy(&sc, tx, ty, true) && ctx) ctx->needs_redraw = true;
    } else if (!down) {
        ui_scroll_touch_xy(&sc, 0, 0, false);
        started_right = false;
    }
    if (ui_scroll_poll(&sc) && ctx) ctx->needs_redraw = true;

    /* 写回渲染读的偏移量（外力改过 item_off 时也在这里对齐回来）*/
    int off = ui_scroll_offset(&sc);
    if (off < 0) off = 0;
    int mx = ui_scroll_max(&sc);
    if (off > mx) off = mx;
    if (s_bp.item_off != off) {
        s_bp.item_off = off;
        if (ctx) ctx->needs_redraw = true;
    }
}

/* 右栏确认: 打开书籍 */
static void on_book_select(ui_ctx_t *ctx, os_pane_t *p,
                           const char *path, const char *name) {
    (void)p; (void)name;
    bs_apply();
    if (book_reader_open(path)) {
        s_reader_open = true;
        ctx->fullscreen = true;
        ESP_LOGI(TAG, "打开: %s", path);
    } else {
        os_dialog_toast(ctx, "\xe6\x96\x87\xe4\xbb\xb6\xe6\x8d\x9f\xe5\x9d\x8f");   /* 文件损坏 */
    }
}

/* 最近阅读入口: 从 NVS 读最近打开的书 (TF 卡损坏/换卡后仍可回到最近一本) */
static void book_recent_load(void) {
    s_bp.recent_path[0] = 0;
    nvs_handle_t h;
    if (svc_nvs_open(BNS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_bp.recent_path);
        if (svc_nvs_get_str(h, "last_path", s_bp.recent_path, &sz) != ESP_OK)
            s_bp.recent_path[0] = 0;
        svc_nvs_close(h);
    }
}

static void p_book_enter(ui_ctx_t *ctx) {
    int64_t t_enter0 = esp_timer_get_time();   /* 2026-10-31: 「进入书库耗时」实测打点 */
    s_tf_font_dirty = true;    /* 字体可能刚装/卸/拷过 → 下次列字体时重扫目录 */
    if (!s_bp.loaded) {
        s_bp.root = BOOK_ROOT;
        s_bp.is_item = is_txt;
        s_bp.on_select = on_book_select;
        s_bp.all_label = NULL;
        /* 侧栏首项「阅读优化」(3.3): 选中后右栏内联显示阅读设置 */
        s_bp.settings_label = "\xe9\x98\x85\xe8\xaf\xbb\xe4\xbc\x98\xe5\x8c\x96"; /* 阅读优化 */
        s_bp.settings_count = 9;   /* 字号/字体/显示进度/边距/行高/字距/段落缩进/震动/旋转方向 */
        s_bp.settings_render = p_book_settings_render;
        s_bp.on_settings_select = p_book_settings_select;
        /* 侧栏「最近阅读」: 设备级兜底, 右栏显示最近打开的书 */
        s_bp.recent_label = "\xe6\x9c\x80\xe8\xbf\x91\xe9\x98\x85\xe8\xaf\xbb"; /* 最近阅读 */
        /* 侧栏「收藏栏」: 长按书加入收藏, 与游戏收藏同机制 (TF 卡持久化) */
        s_bp.fav_enabled = true;
        s_bp.fav_engine = FAV_ENGINE_BOOK;
        s_bp.fav_empty_tip = "\xe9\x95\xbf\xe6\x8c\x89\xe7\xa1\xae\xe8\xae\xa4\xe4\xb9\xa6\xe5\x8a\xa0\xe5\x85\xa5\xe6\x94\xb6\xe8\x97\x8f"; /* 长按确认书加入收藏 */
        s_bp.list_y = 30;   /* 状态栏(24)下方 */
        bs_load();
        os_pane_reset(&s_bp);
    }
    book_recent_load();   /* 每次进入刷新最近阅读 (NVS 可能已被阅读器更新) */
    /* 2026-11-22: 触摸旋转由 book_rotation_ensure() 统一设成 s_bs_rot ——
     *   不再先复位到 0 再页内自映射, 否则手势层(底部上滑返回)会按横屏判方向。 */
    book_rotation_ensure(ctx);   /* 进入书架即按旋转方向启用竖屏 + 触摸旋转 */
    os_pane_build_right(&s_bp);
    ESP_LOGI(TAG, "进入书库耗时 %lld ms (%d 本)",
             (long long)((esp_timer_get_time() - t_enter0) / 1000), s_bp.item_count);
}

static void p_book_exit(ui_ctx_t *ctx) {
    s_bp__free();   /* LINTOS: 页面退出归还列表 pane */
    /* 从阅读器直接 HOME/退出离开: 先收尾阅读器 (释放内存+复位), 再复位触摸旋转 */
    if (s_reader_open) {
        book_reader_close();
        s_reader_open = false;
        ctx->fullscreen = false;
    }
    book_rotation_off(ctx);   /* 卸载书架旋转 (含 map_touch), 阅读器自管全局旋转 */
    input_set_screen_rotation(0);

    /* ★ 2026-10-31: 字体整块映像（PSRAM，最大 1.23MB × 2 槽）在这里统一释放。
     *   关书不再释放 —— 同一会话里换书要复用，否则每本都要重新预加载 ~1.1s。
     *   离开书架页说明"不看了"，这时才把 PSRAM 还给游戏引擎等模块。 */
    /* ★ 2026-11-22: 退出页面时把"加载中"状态收掉。提示窗被框架强清(clear_all)时
     *   font_wait_poll 不会跑, 该标志会永远停在 1 ⇒ 下次进书库点"加载字体"什么都不发生。
     *   ★ 代次自增: 若 font_load_task 还在读卡, 它完成时看到代次已变就只归零、
     *   不回报成功/失败 —— 否则下次进页面 poll 会把上一轮结果当"刚加载好"再应用一次。
     *   ⚠ 真正防止 use-after-free 的是 font_book 内部的锁(font_book_select_file 与
     *   font_book_release_all 串行), 代次只负责状态语义。 */
    s_font_load_gen++;
    if (s_font_load_st == 1) s_font_load_st = 0;
    font_book_release_all();
}

static void p_book_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    if (s_reader_open && book_reader_is_open()) {
        /* 阅读器自己管理旋转 (自带 pfb/portrait_rotate), 书架竖屏模式让位 */
        book_rotation_off(ctx);   /* 阅读器接管: 卸载书架 st7305 旋转与映射 */
        book_reader_render(lcd);
        ctx->fullscreen = true;
        return;
    }
    if (s_reader_open) {
        /* 阅读器已通过内部菜单关闭 (如"返回书库") → 收尾并刷新最近阅读 */
        s_reader_open = false;
        ctx->fullscreen = false;
        bs_load();   /* 同步阅读器内旋转方向 (book_rotate_next 已写 NVS) */
        book_recent_load();
        s_bp.built_folder = -1;
        os_pane_build_right(&s_bp);
    }
    book_rotation_ensure(ctx);   /* 书架按旋转方向渲染 (竖屏画进 pfb, flush 时旋转) */
    ctx->fullscreen = false;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    book_shelf_render(ctx);   /* 自包含渲染 (逻辑坐标, 与阅读器同一坐标系) */
}

static void p_book_action(ui_ctx_t *ctx, os_action_t a) {
    if (s_reader_open) {
        if (a == OS_ACTION_BACK) {
            book_reader_close();
            s_reader_open = false;
            ctx->fullscreen = false;
            /* 刚读完的书 → 刷新「最近阅读」右栏 + 同步阅读器内旋转方向 */
            bs_load();
            book_recent_load();
            s_bp.built_folder = -1;   /* 强制重建 (recent_path 可能已变) */
            os_pane_build_right(&s_bp);
            ctx->needs_redraw = true;
            return;
        }
        if (book_reader_handle_action((int)a)) ctx->needs_redraw = true;
        return;
    }
    if (os_pane_action(ctx, &s_bp, a)) return;
    if (a == OS_ACTION_BACK) os_request_exit_confirm(ctx);   /* 书架最外层: 确认后才退 */
}

static bool p_book_touch(ui_ctx_t *ctx, int x, int y) {
    if (s_reader_open) {
        if (book_reader_handle_touch(x, y)) ctx->needs_redraw = true;
        return true;   /* 阅读器内全部消费 */
    }
    return book_shelf_touch(ctx, x, y);   /* 自包含书架触摸 (页内映射逻辑坐标) */
}

static void p_book_poll(ui_ctx_t *ctx) {
    if (s_reader_open) {
        if (book_reader_poll()) ctx->needs_redraw = true;   /* 阅读器内: 目录/书签菜单拖动滚动 */
        return;
    }
    if (!s_reader_open) book_shelf_drag(ctx);   /* 自包含右栏拖动滚动 (页内映射) */
}

static const os_module_t s_mod_book = {
    .name       = "book",
    .page_id    = OS_PAGE_BOOK,
    .on_enter   = p_book_enter,
    .on_exit    = p_book_exit,
    .render     = p_book_render,
    .action     = p_book_action,
    .touch      = p_book_touch,
    .poll       = p_book_poll,
    .fullscreen = false,
};

void os_page_book_register(void) { os_register(&s_mod_book); }
