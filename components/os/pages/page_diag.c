/**
 * page_diag.c — 故障诊断 页面模块 (P4 迁移: 从 menu/diagnosis_v2.c 改签名为 ui_ctx + 私有 state).
 *
 * 浏览模式(左分类/右列表) + 故障向导模式(现象→症状→设备→配置→结果, 概率排序).
 * 状态栏由全局 svc_statusbar 自动叠加 (fullscreen=false).
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "ui_common.h"
#include "font_zh.h"
#include "esp_attr.h"
#include "font_zh16.h"
#include "input.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#define DW 400
#define DH 300

#define DBAR_H   40
#define DCONTENT_Y (DBAR_H)
#define DNODE_PAD 30
#define DLEFT_W  120
#define DIVIDER_X DLEFT_W
#define RICON_S     52
#define RICON_GAP   3
#define RSHOW       3
#define RPBAR_H     6

#define RLIN_ICON_Y0(di) ((di) * RL_ROW_H)
#define RL_ROW_H    88
#define RL_ROW_X    (DLEFT_W / 2)
#define RL_NAME_Y(di) (RLIN_ICON_Y0(di) + RICON_S + RICON_GAP)
#define RL_PROB_Y(di) (RL_NAME_Y(di) + 20)
#define RL_BAR_Y(di)  (RL_PROB_Y(di) + 14)
#define RL_MAX_OFF   (DMAXDEV - RSHOW)

/* ================= 通用绘制辅助 ================= */
static inline void dsetp(st7305_handle_t *l, int x, int y) {
    if (x < 0 || x >= DW || y < 0 || y >= DH) return;
    st7305_draw_pixel(l, x, y, ST7305_COLOR_BLACK);
}
static inline void dclrp(st7305_handle_t *l, int x, int y) {
    if (x < 0 || x >= DW || y < 0 || y >= DH) return;
    st7305_draw_pixel(l, x, y, ST7305_COLOR_WHITE);
}
static void dfill(st7305_handle_t *l, int x0, int y0, int x1, int y1, int col) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            st7305_draw_pixel(l, x, y, (st7305_color_t)col);
}
static void dhline(st7305_handle_t *l, int x0, int x1, int y) {
    if (y < 0 || y >= DH) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) if (x >= 0 && x < DW) st7305_draw_pixel(l, x, y, ST7305_COLOR_BLACK);
}
static void dvline(st7305_handle_t *l, int x, int y0, int y1) {
    if (x < 0 || x >= DW) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) if (y >= 0 && y < DH) st7305_draw_pixel(l, x, y, ST7305_COLOR_BLACK);
}
static void drect(st7305_handle_t *l, int x0, int y0, int x1, int y1, int fill) { dfill(l, x0, y0, x1, y1, fill); }
static void doutline(st7305_handle_t *l, int x0, int y0, int x1, int y1) {
    dhline(l, x0, x1, y0); dhline(l, x0, x1, y1);
    dvline(l, x0, y0, y1); dvline(l, x1, y0, y1);
}

/* ================= 中文 / ASCII 绘制 ================= */
static void dzh(st7305_handle_t *l, int x, int y, const char *str, bool inverted) {
    int idx = font_zh_find_utf8(str);
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    dfill(l, x, y, x + 11, y + 11, bg);
    if (idx < 0) return;
    const uint8_t *bmp = zh_font_data[idx];
    int bpr = (ZH_FONT_W + 7) / 8;
    for (int r = 0; r < 12; r++)
        for (int c = 0; c < 12; c++) {
            bool any = false;
            for (int dy = 0; dy < 2 && !any; dy++)
                for (int dx = 0; dx < 2 && !any; dx++) {
                    int row = r * 2 + dy, col = c * 2 + dx;
                    if (bmp[row * bpr + (col / 8)] & (1 << (7 - (col % 8)))) any = true;
                }
            st7305_draw_pixel(l, x + c, y + r, any ? fg : bg);
        }
}
static int dzhs(st7305_handle_t *l, int x, int y, const char *s, bool inv) {
    while (*s) { dzh(l, x, y, s, inv); x += 12; s += 3; }
    return x;
}
static const uint8_t dgfont[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},{0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},
    {0x08,0x08,0x3E,0x08,0x08},{0x14,0x14,0x14,0x14,0x14},{0x41,0x22,0x14,0x08,0}
};
static int dgi(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    int i = (c - 0x20);
    if (i < 0 || i > 58) return -1;
    return i;
}
static int dtxt(st7305_handle_t *l, int x, int y, const char *s) {
    while (*s) {
        int gi = (unsigned char)*s >= '0' && (unsigned char)*s <= '9'
                 ? (*s - '0') : dgi(*s);
        if (gi >= 0) {
            const uint8_t *g = dgfont[gi];
            for (int cx = 0; cx < 5; cx++)
                for (int cy = 0; cy < 7; cy++)
                    if (g[cx] & (1 << cy))
                        dsetp(l, x + cx, y + cy);
        }
        x += 6;
        s++;
    }
    return x;
}
static void dzh16(st7305_handle_t *l, int x, int y, const char *str, bool inv) {
    int idx = font_zh16_find_utf8(str);
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    dfill(l, x, y, x + 15, y + 15, bg);
    if (idx < 0) return;
    const uint8_t *bmp = zh16_font_data[idx];
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 16; col++) {
            int byte = bmp[row * 2 + (col / 8)];
            st7305_draw_pixel(l, x + col, y + row, (byte & (1 << (7 - (col % 8)))) ? fg : bg);
        }
}
static int dzh16s(st7305_handle_t *l, int x, int y, const char *s, bool inv) {
    while (*s) { dzh16(l, x, y, s, inv); x += 16; s += 3; }
    return x;
}
static void dz22(st7305_handle_t *l, int x, int y, const char *str, bool inv) {
    int idx = font_zh_find_utf8(str);
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    dfill(l, x, y, x + 23, y + 23, bg);
    if (idx < 0) return;
    const uint8_t *bmp = zh_font_data[idx];
    for (int row = 0; row < 24; row++)
        for (int col = 0; col < 24; col++) {
            int byte = bmp[row * 3 + (col / 8)];
            st7305_draw_pixel(l, x + col, y + row, (byte & (1 << (7 - (col % 8)))) ? fg : bg);
        }
}
static int dz22s(st7305_handle_t *l, int x, int y, const char *s, bool inv) {
    while (*s && *s != '\n') { dz22(l, x, y, s, inv); x += 24; s += 3; }
    return x;
}
static void dpct(st7305_handle_t *l, int x, int y) {
    static const uint8_t pat[7] = {0x18, 0x18, 0x04, 0x02, 0x01, 0x03, 0x03};
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (pat[row] & (1 << (4 - col)))
                dsetp(l, x + col, y + row);
}
static int dpct_text(st7305_handle_t *l, int x, int y, int val) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", val);
    x = dtxt(l, x, y, buf);
    dpct(l, x, y);
    return x + 6;
}
static void dline(st7305_handle_t *l, int x0, int y0, int x1, int y1);
static void dcircle(st7305_handle_t *l, int cx, int cy, int r);
static void dcircle_w(st7305_handle_t *l, int cx, int cy, int r);

static int dtxt2(st7305_handle_t *l, int x, int y, const char *s) {
    while (*s) {
        int gi = (unsigned char)*s >= '0' && (unsigned char)*s <= '9'
                 ? (*s - '0') : dgi(*s);
        if (gi >= 0) {
            const uint8_t *g = dgfont[gi];
            for (int cy = 0; cy < 7; cy++)
                for (int cx = 0; cx < 5; cx++)
                    if (g[cx] & (1 << cy)) {
                        dsetp(l, x + cx * 2,     y + cy * 2);
                        dsetp(l, x + cx * 2 + 1, y + cy * 2);
                        dsetp(l, x + cx * 2,     y + cy * 2 + 1);
                        dsetp(l, x + cx * 2 + 1, y + cy * 2 + 1);
                    }
        }
        x += 12;
        s++;
    }
    return x;
}
static void dpct2(st7305_handle_t *l, int x, int y) {
    dline(l, x + 7, y - 1, x + 4, y + 13);
    dcircle(l, x + 2, y + 2, 2);
    dcircle(l, x + 9, y + 11, 2);
}

static uint32_t dmnow(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ================= 决策知识库 ================= */
#define DMAXOPT  8
#define DMAXDEV  6

typedef struct {
    const char *label;
    int         weight;
    int         target;
} diag_opt_t;
typedef struct { diag_opt_t opts[DMAXOPT]; int nopts; } diag_level_t;

static const char *DDEVNAME[DMAXDEV] = { "内存", "硬盘", "显卡", "主板", "电源", "系统" };
static int devprob[DMAXDEV];

typedef struct {
    const char  *label;
    const char *sub[DMAXOPT];
    int         subwt[DMAXOPT];
    int         subdev[DMAXOPT];
    int         nsub;
} diag_cat_t;

static const diag_cat_t DCAT[6] = {
    { "不通电", {
        "电源指示灯不亮", "按下开机键无反应", "机内有焦糊味",
        "指示灯闪烁不稳定", "能通电但瞬间关机", "不确定"
      },
      { 28, 24, 20, 22, 18, 5 },
      { 4, 3, 4, 4, 4, -1 }, 6 },
    { "卡顿闪退", {
        "开机和启动特别慢", "多开程序就卡顿", "运行游戏中闪退",
        "使用中莫名崩溃", "风扇狂转且过热", "不确定"
      },
      { 26, 26, 22, 18, 14, 5 },
      { 1, 0, 2, 0, 3, -1 }, 6 },
    { "蓝屏死机", {
        "蓝屏代码每次不同", "特定操作后蓝屏", "死机后无法关机",
        "开机引导时蓝屏", "蓝屏后重启循环", "不确定"
      },
      { 26, 20, 18, 22, 16, 5 },
      { 0, 2, 3, 1, 5, -1 }, 6 },
    { "花屏闪屏", {
        "屏幕出现彩色条纹", "画面雪花噪点", "屏幕整体闪烁跳动",
        "接外接屏显示正常", "开机花屏但能进系统", "不确定"
      },
      { 30, 26, 18, 16, 20, 5 },
      { 2, 2, 2, 3, 2, -1 }, 6 },
    { "加电无显", {
        "风扇转屏幕无信号", "主板灯亮但屏幕黑", "有开机蜂鸣声",
        "有背光但画面全黑", "完全无背光", "不确定"
      },
      { 24, 20, 22, 16, 18, 5 },
      { 2, 3, 0, 3, 4, -1 }, 6 },
    { "无法进入系统", {
        "卡在品牌标志界面", "提示修复恢复界面", "无限重启进不去",
        "黑屏停住不动", "能进但频繁卡死", "不确定"
      },
      { 26, 24, 18, 18, 20, 5 },
      { 1, 5, 4, 0, 1, -1 }, 6 },
};

static const char *DDEVTYPE[DMAXOPT] = {
    "台式机", "笔记本", "一体机", "工控机", "服务器", "不确定"
};

typedef struct {
    const char *cause;
    const char *steps[4];
    const char *tip;
} diag_kb_t;
static const diag_kb_t DKB[DMAXDEV] = {
    { "接触氧化或颗粒损坏",
      { "内存检测工具全盘测", "用好内存条替换试", "重插并清洁金手指", "替换后复测无报错" },
      "单条逐一开机定坏条" },
    { "坏道或接口接触不良",
      { "磁盘健康扫描修复", "换数据线或换盘试", "备份资料更换坏盘", "复扫无坏道即通过" },
      "留意有无异响咔哒声" },
    { "显示核心或显存故障",
      { "图形烤机稳定测试", "换一张显卡替换试", "重插显卡清洁插槽", "长时间负载无花屏" },
      "外接显示器对比排查" },
    { "供电电路或芯片异常",
      { "进设置界面查各项", "最小系统替换法测", "清设置重插各部件", "逐一替换后能稳定" },
      "查看有无鼓包焦糊味" },
    { "输出电压不稳或不足",
      { "软件监测各路电压", "用好电源替换测试", "更换电源或减负载", "满载运行不再断电" },
      "先查供电线是否插紧" },
    { "系统文件或驱动损坏",
      { "系统修复工具扫描", "用启动盘工具验证", "还原或重装系统", "连续使用不再复发" },
      "进安全模式排除软件" },
};

#define POPUP_X0 (DIVIDER_X + 16)
#define POPUP_X1 (DW - 14)
#define POPUP_Y0 (DCONTENT_Y + 56)
#define POPUP_Y1 (DH - 8)
static bool g_popup_on  = false;
static int  g_popup_dev = 0;

typedef enum {
    D_ST_CAT = 0, D_ST_SUB, D_ST_DEVTYPE, D_ST_CFG, D_ST_RESULT, D_ST_COUNT
} diag_stage_t;

static const char *DSTAGE_NAME[D_ST_COUNT] = { "现象", "症状", "设备", "配置", "结果" };
static const char *DSTAGE_QUESTION[D_ST_COUNT] = {
    "请选择故障现象", "您看到了什么症状",
    "这台是什么设备", "这台设备配置如何",
    "排查建议按概率排序"
};

static diag_stage_t g_stage = D_ST_CAT;
static int          g_cat   = 0;
static int          g_sub   = 0;
static int          g_devtype = 0;
static int          g_cfg    = 0;
static bool         g_ans_sub = false, g_ans_devtype = false, g_ans_cfg = false;
static int          g_cur_opt = 0;
static int          g_result_k = 0;
static uint32_t     g_blink_ms = 0;

static void devpplus(int target, int weight) {
    if (target < 0 || target >= DMAXDEV) {
        int each = weight / DMAXDEV;
        for (int i = 0; i < DMAXDEV; i++) devprob[i] += each;
        return;
    }
    devprob[target] += weight;
}
static void dRecompute(void) {
    for (int i = 0; i < DMAXDEV; i++) devprob[i] = 0;
    if (g_ans_sub)
        devpplus(DCAT[g_cat].subdev[g_sub], DCAT[g_cat].subwt[g_sub]);
    if (g_ans_devtype) {
        switch (g_devtype) {
            case 0: devprob[4] += 4; devprob[2] += 4; break;
            case 1: devprob[3] += 4; devprob[0] += 2; devprob[4] += 2; break;
            case 2: devprob[3] += 4; devprob[2] += 2; break;
            case 3: devprob[3] += 4; devprob[4] += 4; break;
            case 4: devprob[0] += 6; devprob[4] += 4; devprob[1] += 4; break;
            default: break;
        }
    }
    if (g_ans_cfg) {
        if (g_cfg == 0) { devprob[2] += 6; devprob[4] += 4; }
        else            { devprob[3] += 4; devprob[0] += 2; }
    }
}

static int  s_rbar_off = 0;
static bool s_rbar_drag = false;
static int  s_rbar_drag_y0 = 0;
static int  s_rbar_drag_off0 = 0;
#define RBAR_ROW_H RL_ROW_H
#define RBAR_MAX_OFF RL_MAX_OFF

static void DReset(void) {
    for (int i = 0; i < DMAXDEV; i++) devprob[i] = 0;
    g_stage = D_ST_CAT;
    g_cat = 0; g_sub = 0; g_devtype = 0; g_cfg = 0;
    g_ans_sub = g_ans_devtype = g_ans_cfg = false;
    g_cur_opt = 0; g_result_k = 0;
    g_popup_on = false; g_popup_dev = 0;
    s_rbar_off = 0; s_rbar_drag = false;
}
static int dStageOptCount(diag_stage_t st) {
    switch (st) {
        case D_ST_CAT: return 6;
        case D_ST_SUB: return DCAT[g_cat].nsub;
        case D_ST_DEVTYPE: return 6;
        case D_ST_CFG: return 2;
        case D_ST_RESULT: return DMAXDEV;
        default: return 0;
    }
}
static const char *dStageOptLabel(diag_stage_t st, int i) {
    switch (st) {
        case D_ST_CAT:   return DCAT[i].label;
        case D_ST_SUB:   return DCAT[g_cat].sub[i];
        case D_ST_DEVTYPE: return DDEVTYPE[i];
        case D_ST_CFG:   return i == 0 ? "有独立显卡" : "无独立显卡";
        case D_ST_RESULT: return DDEVNAME[i];
        default: return "";
    }
}
static int dSortedIdx[DMAXDEV];
static void dSortDev(void) {
    for (int i = 0; i < DMAXDEV; i++) dSortedIdx[i] = i;
    for (int i = 0; i < DMAXDEV - 1; i++)
        for (int j = i + 1; j < DMAXDEV; j++)
            if (devprob[dSortedIdx[j]] > devprob[dSortedIdx[i]]) {
                int t = dSortedIdx[i]; dSortedIdx[i] = dSortedIdx[j]; dSortedIdx[j] = t;
            }
}

/* ================= 渲染 ================= */
static void dDrawStageBar(st7305_handle_t *l) {
    const int n = D_ST_COUNT;
    int xs[5];
    for (int i = 0; i < n; i++)
        xs[i] = DNODE_PAD + (DW - 2 * DNODE_PAD) * i / (n - 1);
    int cur = (int)g_stage;
    int lineY = DBAR_H - 10;
    dhline(l, xs[0], xs[n - 1], lineY);
    if (cur >= 0) {
        for (int dy = 0; dy < 3; dy++)
            dhline(l, xs[0], xs[cur], lineY + dy);
    }
    for (int i = 0; i < n; i++) {
        int cx = xs[i];
        if (i <= cur) dcircle(l, cx, lineY, (i == cur) ? 6 : 5);
        else { dcircle(l, cx, lineY, 3); dcircle_w(l, cx, lineY, 2); }
        const char *nm = DSTAGE_NAME[i];
        int tw = ((int)strlen(nm) / 3) * 16;
        dzh16s(l, cx - tw / 2, 2, nm, (i == cur));
    }
}
static void dDrawOptions(st7305_handle_t *l) {
    dz22s(l, DIVIDER_X + 10, DCONTENT_Y + 4, DSTAGE_QUESTION[g_stage], false);
    int n = dStageOptCount(g_stage);
    if (n <= 0 || g_stage == D_ST_RESULT) return;
    int top = DCONTENT_Y + 36;
    int bot = DH - 8;
    int row_h = (bot - top) / n;
    int x0 = DIVIDER_X + 10, x1 = DW - 8;
    for (int i = 0; i < n; i++) {
        int y0 = top + i * row_h;
        int y1 = y0 + row_h - 1;
        bool sel = (i == g_cur_opt);
        if (sel) drect(l, x0, y0, x1, y1, ST7305_COLOR_BLACK);
        const char *nm = dStageOptLabel(g_stage, i);
        dz22s(l, x0 + 8, y0 + (row_h - 24) / 2, nm, sel);
    }
}
static void dDrawResult(st7305_handle_t *l) {
    dSortDev();
    int dev = dSortedIdx[g_result_k];
    int x0 = DIVIDER_X + 10;
    int y  = DCONTENT_Y + 38;
    int x = dz22s(l, x0, y, "判断", false);
    x = dz22s(l, x, y, DDEVNAME[dev], false);
    dz22s(l, x, y, "故障", false);
    y += 32;
    static const char *LBL[6] = { "原因", "软件测", "硬件换", "解决", "确认", "提示" };
    const char *val[6] = {
        DKB[dev].cause,
        DKB[dev].steps[0], DKB[dev].steps[1],
        DKB[dev].steps[2], DKB[dev].steps[3],
        DKB[dev].tip,
    };
    for (int i = 0; i < 6; i++) {
        int lx = dzh16s(l, x0, y, LBL[i], false);
        dzh16s(l, lx + 4, y, val[i], false);
        y += 20;
    }
    dzhs(l, x0, DH - 16, "上下键换候选点图标看方案", false);
}
static void dline(st7305_handle_t *l, int x0, int y0, int x1, int y1) {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (abs(dx) > abs(dy)) ? abs(dx) : abs(dy);
    if (steps == 0) { dsetp(l, x0, y0); return; }
    for (int i = 0; i <= steps; i++) dsetp(l, x0 + dx * i / steps, y0 + dy * i / steps);
}
static void dcircle_c(st7305_handle_t *l, int cx, int cy, int r, int col) {
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) {
                if (x >= 0 && x < DW && y >= 0 && y < DH)
                    st7305_draw_pixel(l, x, y, (st7305_color_t)col);
            }
        }
}
static void dcircle(st7305_handle_t *l, int cx, int cy, int r) { dcircle_c(l, cx, cy, r, ST7305_COLOR_BLACK); }
static void dcircle_w(st7305_handle_t *l, int cx, int cy, int r) { dcircle_c(l, cx, cy, r, ST7305_COLOR_WHITE); }

static void dDrawDevIcon(st7305_handle_t *l, int dev, int cx, int cy) {
    switch (dev) {
    case 0: {
        doutline(l, cx - 16, cy - 16, cx + 16, cy + 8);
        for (int i = -10; i <= 10; i += 5) dvline(l, cx + i, cy - 11, cy - 4);
        dvline(l, cx - 12, cy + 8, cy + 16); dvline(l, cx - 4, cy + 8, cy + 16);
        dvline(l, cx + 4, cy + 8, cy + 16);  dvline(l, cx + 12, cy + 8, cy + 16);
        break;
    }
    case 1: { dcircle(l, cx, cy, 19); dcircle_w(l, cx, cy, 16); dcircle(l, cx, cy, 4); break; }
    case 2: {
        doutline(l, cx - 19, cy - 13, cx + 19, cy + 13);
        dcircle(l, cx, cy, 6);
        dhline(l, cx - 12, cx + 3, cy - 10); dvline(l, cx + 10, cy - 5, cy + 5);
        dhline(l, cx - 12, cx + 3, cy + 10); dvline(l, cx - 10, cy - 5, cy + 5);
        break;
    }
    case 3: {
        doutline(l, cx - 19, cy - 18, cx + 19, cy + 18);
        drect(l, cx - 9, cy - 9, cx + 9, cy + 6, 0);
        doutline(l, cx - 10, cy - 10, cx + 10, cy + 7);
        dhline(l, cx - 19, cx - 10, cy - 14); dhline(l, cx + 10, cx + 19, cy - 14);
        dhline(l, cx - 19, cx - 10, cy + 14); dhline(l, cx + 10, cx + 19, cy + 14);
        break;
    }
    case 4: {
        dline(l, cx + 3, cy - 18, cx - 6, cy + 1);
        dline(l, cx - 6, cy + 1, cx + 6, cy + 1);
        dline(l, cx + 6, cy + 1, cx - 3, cy + 18);
        break;
    }
    default: {
        doutline(l, cx - 18, cy - 16, cx + 18, cy + 16);
        drect(l, cx - 18, cy - 16, cx + 18, cy - 10, 0);
        drect(l, cx - 12, cy - 6, cx + 12, cy - 1, 0);
        drect(l, cx - 12, cy + 3, cx + 12, cy + 12, 0);
        break;
    }
    }
}
static void dDrawLeftBar(st7305_handle_t *l) {
    dSortDev();
    dvline(l, DIVIDER_X, DCONTENT_Y, DH - 1);
    int vyoff = DCONTENT_Y + 4;
    int icon_x = (DLEFT_W - RICON_S) / 2;
    for (int k = 0; k < RSHOW; k++) {
        int si = s_rbar_off + k;
        if (si >= DMAXDEV) break;
        int di = dSortedIdx[si];
        int ic_y = vyoff + k * RL_ROW_H;
        drect(l, icon_x, ic_y, icon_x + RICON_S - 1, ic_y + RICON_S - 1, 0);
        doutline(l, icon_x, ic_y, icon_x + RICON_S - 1, ic_y + RICON_S - 1);
        dDrawDevIcon(l, di, icon_x + RICON_S / 2, ic_y + RICON_S / 2);
        int name_y = ic_y + RICON_S + RICON_GAP;
        const char *nm = DDEVNAME[di];
        int nw = ((int)(strlen(nm) / 3)) * 16;
        dzh16s(l, (DLEFT_W - nw) / 2, name_y, nm, false);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", devprob[di]);
        int prob_x = (DLEFT_W - ((int)strlen(buf) * 12 + 12)) / 2;
        int prob_y = name_y + 20;
        dtxt2(l, prob_x, prob_y, buf);
        dpct2(l, prob_x + (int)strlen(buf) * 12, prob_y);
        int bar_y = prob_y + 14;
        int bx0 = 4, bx1 = DLEFT_W - 5;
        drect(l, bx0, bar_y, bx1, bar_y + RPBAR_H - 1, 1);
        doutline(l, bx0, bar_y, bx1, bar_y + RPBAR_H - 1);
        int fill_w = (int)((bx1 - bx0 + 1) * devprob[di]) / 100;
        if (fill_w > bx1 - bx0 + 1) fill_w = bx1 - bx0 + 1;
        if (fill_w > 2) drect(l, bx0 + 1, bar_y + 1, bx0 + fill_w - 1, bar_y + RPBAR_H - 2, 0);
    }
}
static void dDrawPopup(st7305_handle_t *l) {
    int px0 = POPUP_X0, px1 = POPUP_X1, py0 = POPUP_Y0, py1 = POPUP_Y1;
    drect(l, px0, py0, px1, py1, ST7305_COLOR_WHITE);
    doutline(l, px0, py0, px1, py1);
    char title[24];
    snprintf(title, sizeof(title), "%s排查方案", DDEVNAME[g_popup_dev]);
    size_t tl = strlen(title) / 3;
    int tw = (int)tl * 16;
    int tx = px0 + (px1 - px0 - tw) / 2;
    drect(l, px0, py0, px1, py0 + 18, ST7305_COLOR_BLACK);
    dzh16s(l, tx, py0 + 1, title, true);
    int ly = py0 + 26;
    for (int i = 0; i < 4; i++) {
        const char *ln = DKB[g_popup_dev].steps[i];
        size_t llen = strlen(ln) / 3;
        int lw = (int)llen * 16;
        dzh16s(l, px0 + (px1 - px0 - lw) / 2, ly, ln, false);
        ly += 19;
    }
    int bx0 = px0 + (px1 - px0) / 2 - 34, bx1 = bx0 + 68;
    int by0 = py1 - 26, by1 = py1 - 4;
    drect(l, bx0, by0, bx1, by1, ST7305_COLOR_BLACK);
    dzh16s(l, bx0 + (68 - 32) / 2, by0 + 3, "返回", true);
}

/* ================= 浏览模式 (左分类 / 右列表) ================= */
#define DI_CATBAR_W  64
#define DICAT_N      4
static const char *DI_NAME[DICAT_N] = { "采集", "当前信息", "信息处理", "历史" };
static const char *DI_LIST[DICAT_N][12] = {
  { "获取硬件配置","获取错误日志","获取蓝屏信息","获取IP和MAC","获取系统信息","获取磁盘内存" },
  { "查看硬件配置","查看错误日志","查看蓝屏信息","查看网络信息","查看系统信息" },
  { "硬件概率分析","系统问题分析","蓝屏原因分析" },
  {},
};
static int DI_COUNT[DICAT_N] = { 6, 5, 3, 0 };

#define DI_HIST_DIR  "/sdcard/diag"
#define DI_HIST_MAX  16
EXT_RAM_BSS_ATTR static char di_hist[DI_HIST_MAX][48];
static int  di_hist_n = -1;

static void di_scan_history(void) {
    di_hist_n = 0;
    DIR *d = opendir(DI_HIST_DIR);
    if (!d) { di_hist_n = 0; return; }
    struct dirent *e;
    while (di_hist_n < DI_HIST_MAX && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(di_hist[di_hist_n], sizeof(di_hist[0]), "%.47s", e->d_name);
        di_hist_n++;
    }
    closedir(d);
    for (int i = 0; i < di_hist_n - 1; i++)
        for (int j = i + 1; j < di_hist_n; j++)
            if (strcmp(di_hist[j], di_hist[i]) > 0) {
                char t[48]; strncpy(t, di_hist[j], 47); t[47]=0;
                strncpy(di_hist[j], di_hist[i], 47); di_hist[j][47]=0;
                strncpy(di_hist[i], t, 47); di_hist[i][47]=0;
            }
}
static void di_hist_label(const char *name, char *out, int n) {
    out[0] = 0;
    if (strlen(name) >= 9 && name[0] >= '0' && name[0] <= '9') {
        snprintf(out, n, "%.4s-%.2s-%.2s %s", name, name + 4, name + 6, name + 9);
    } else {
        snprintf(out, n, "%s", name);
    }
}
#define DI_MODE_BROWSE 0
#define DI_MODE_FLOW   1
static int g_mode = DI_MODE_BROWSE;
static int g_bcat = 0;
static int g_lst  = 0;
static int g_lsc  = 0;
#define DI_LROW_H  40
#define DI_CAT_Y0    24
#define DI_CAT_ROW_H 64
#define DI_LTOP    (DI_CAT_Y0 + 40)
#define DI_LBOT    (DH - 14)
#define DI_LVIS    ((DI_LBOT - DI_LTOP) / DI_LROW_H)
static int di_count(void) {
    if (g_bcat == 3) { if (di_hist_n < 0) di_scan_history(); return di_hist_n; }
    return DI_COUNT[g_bcat];
}
static const char *di_label(int idx) {
    static char buf[48];
    if (g_bcat == 3) { di_hist_label(di_hist[idx], buf, sizeof(buf)); return buf; }
    return DI_LIST[g_bcat][idx];
}
static void di_enter_flow(void) { DReset(); g_mode = DI_MODE_FLOW; }
static void di_act_item(int idx) {
    switch (g_bcat) {
    case 0: return;
    case 2:
        if (idx == 0) { di_enter_flow(); }
        return;
    default: return;
    }
}

static void di_draw_browse(ui_ctx_t *ctx) {
    st7305_handle_t *l = ctx->lcd;
    st7305_clear(l, ST7305_COLOR_WHITE);
    dvline(l, DI_CATBAR_W, DI_CAT_Y0, DH - 1);
    for (int c = 0; c < DICAT_N; c++) {
        int y0 = DI_CAT_Y0 + c * DI_CAT_ROW_H;
        draw_main_icon_stretched(l, DI_CATBAR_W / 2, y0 + DI_CAT_ROW_H / 2, 64, 64, c);
        if (c == g_bcat) {
            drect(l, DI_CATBAR_W / 2 - 32, y0 + DI_CAT_ROW_H - 2,
                  DI_CATBAR_W / 2 + 31, y0 + DI_CAT_ROW_H - 1, ST7305_COLOR_BLACK);
        }
    }
    dz22s(l, DI_CATBAR_W + 10, DI_CAT_Y0 + 4, DI_NAME[g_bcat], false);
    int n = di_count(), vis = DI_LVIS;
    if (vis < 1) vis = 1;
    if (g_lsc > n - vis) g_lsc = n - vis;
    if (g_lsc < 0) g_lsc = 0;
    for (int i = 0; i < vis; i++) {
        int idx = g_lsc + i; if (idx >= n) break;
        int y0 = DI_LTOP + i * DI_LROW_H, y1 = y0 + DI_LROW_H - 1, sel = (idx == g_lst);
        if (sel) drect(l, DI_CATBAR_W + 10, y0, DW - 8, y1, ST7305_COLOR_BLACK);
        dz22s(l, DI_CATBAR_W + 14, y0 + (DI_LROW_H - 24) / 2, di_label(idx), sel);
    }
    if (n == 0) {
        dz22s(l, DI_CATBAR_W + 10, 120, g_bcat == 3 ? "\xe6\x9a\x82\xe6\x97\xa0\xe5\x8e\x86\xe5\x8f\xb2\xe8\xae\xb0\xe5\xbd\x95" : "\xe6\x9a\x82\xe6\x97\xa0\xe5\x86\x85\xe5\xae\xb9", false);
    } else if (n > vis) {
        dzhs(l, DI_CATBAR_W + 10, DH - 10, "上下滑动查看更多", false);
    }
}

/* ================= 主渲染 / 推进 ================= */
static void p_diag_render(ui_ctx_t *ctx) {
    if (g_mode == DI_MODE_BROWSE) { di_draw_browse(ctx); return; }
    st7305_handle_t *l = ctx->lcd;
    st7305_clear(l, ST7305_COLOR_WHITE);
    dDrawStageBar(l);
    if (g_stage == D_ST_RESULT) dDrawResult(l);
    else                        dDrawOptions(l);
    dDrawLeftBar(l);
    if (g_popup_on) dDrawPopup(l);
}

static void dConfirmSelect(void) {
    switch (g_stage) {
    case D_ST_CAT: g_cat = g_cur_opt; g_cur_opt = 0; g_stage = D_ST_SUB; break;
    case D_ST_SUB:
        g_sub = g_cur_opt; g_cur_opt = 0; g_ans_sub = true; dRecompute(); g_stage = D_ST_DEVTYPE;
        break;
    case D_ST_DEVTYPE:
        g_devtype = g_cur_opt; g_cur_opt = 0; g_ans_devtype = true; dRecompute(); g_stage = D_ST_CFG;
        break;
    case D_ST_CFG:
        g_cfg = g_cur_opt; g_cur_opt = 0; g_ans_cfg = true; dRecompute();
        dSortDev(); g_result_k = 0; g_stage = D_ST_RESULT;
        break;
    case D_ST_RESULT: break;
    default: break;
    }
}
static void dInvalidateFrom(diag_stage_t st) {
    if (st <= D_ST_SUB)     g_ans_sub = false;
    if (st <= D_ST_DEVTYPE) g_ans_devtype = false;
    if (st <= D_ST_CFG)     g_ans_cfg = false;
    dRecompute();
}
static void dStepBack(void) {
    if (g_stage <= D_ST_CAT) { g_stage = D_ST_CAT; g_cur_opt = 0; return; }
    switch (g_stage) {
        case D_ST_SUB:     g_ans_sub = false; break;
        case D_ST_DEVTYPE: g_ans_devtype = false; break;
        case D_ST_CFG:     g_ans_cfg = false; break;
        case D_ST_RESULT:  g_ans_cfg = false; break;
        default: break;
    }
    g_stage = (diag_stage_t)((int)g_stage - 1);
    g_cur_opt = 0;
    dRecompute();
}

static int dHitOption(int x, int y) {
    if (y < DCONTENT_Y) return -1;
    if (x < DIVIDER_X + 10 || x >= DW - 8) return -1;
    if (g_stage == D_ST_RESULT) return -1;
    int n = dStageOptCount(g_stage);
    if (n <= 0) return -1;
    int top = DCONTENT_Y + 36, bot = DH - 8;
    int row_h = (bot - top) / n;
    int i = (y - top) / row_h;
    if (i < 0 || i >= n) return -1;
    if (y < top) return -1;
    return i;
}
static int dHitLeftDev(int x, int y) {
    if (x >= DIVIDER_X - 2) return -1;
    int vyoff = DCONTENT_Y + 4;
    if (y < vyoff) return -1;
    int k = (y - vyoff) / RBAR_ROW_H;
    if (k < 0 || k >= RSHOW) return -1;
    if (y > vyoff + k * RBAR_ROW_H + RL_ROW_H - 2) return -1;
    int si = s_rbar_off + k;
    if (si >= DMAXDEV) return -1;
    dSortDev();
    return dSortedIdx[si];
}

static bool p_diag_touch(ui_ctx_t *ctx, int x, int y) {
    ctx->needs_redraw = true;
    if (g_mode == DI_MODE_BROWSE) {
        if (x < DI_CATBAR_W && y >= DI_CAT_Y0) {
            int c = (y - DI_CAT_Y0) / DI_CAT_ROW_H;
            if (c >= 0 && c < DICAT_N) { g_bcat = c; g_lst = 0; g_lsc = 0; }
            return false;
        }
        if (x >= DI_CATBAR_W + 10 && x < DW - 8 && y >= DI_LTOP) {
            int i = (y - DI_LTOP) / DI_LROW_H + g_lsc;
            if (i < di_count()) { g_lst = i; di_act_item(i); }
        }
        return false;
    }
    if (g_popup_on) {
        int bx0 = POPUP_X0 + (POPUP_X1 - POPUP_X0) / 2 - 34, bx1 = bx0 + 68;
        int by0 = POPUP_Y1 - 26, by1 = POPUP_Y1 - 4;
        if (x >= bx0 && x <= bx1 && y >= by0 && y <= by1) { g_popup_on = false; return false; }
        if (x >= POPUP_X0 && x <= POPUP_X1 && y >= POPUP_Y0 && y <= POPUP_Y1) return false;
        g_popup_on = false;
        return false;
    }
    if (y < DBAR_H) {
        const int n = D_ST_COUNT;
        int best = -1, bd = 1000;
        for (int i = 0; i < n; i++) {
            int xi = DNODE_PAD + (DW - 2 * DNODE_PAD) * i / (n - 1);
            int d = abs(x - xi);
            if (d < bd) { bd = d; best = i; }
        }
        if (best >= 0 && best <= (int)g_stage) {
            g_stage = (diag_stage_t)best;
            g_cur_opt = 0;
            if (best <= D_ST_SUB) g_sub = 0;
            dInvalidateFrom((diag_stage_t)best);
        }
        return false;
    }
    int dev = dHitLeftDev(x, y);
    if (dev >= 0 && g_stage != D_ST_CAT) { g_popup_dev = dev; g_popup_on = true; return false; }
    int hit = dHitOption(x, y);
    if (hit >= 0 && hit < dStageOptCount(g_stage)) { g_cur_opt = hit; dConfirmSelect(); }
    return false;
}

static void p_diag_poll(ui_ctx_t *ctx) {
    if (g_mode == DI_MODE_BROWSE) return;
    if (g_popup_on) { s_rbar_drag = false; return; }
    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    if (!down) { s_rbar_drag = false; return; }
    if (tx >= DIVIDER_X - 2) return;
    if (!s_rbar_drag) {
        s_rbar_drag = true;
        s_rbar_drag_y0 = ty;
        s_rbar_drag_off0 = s_rbar_off;
        return;
    }
    int dy = s_rbar_drag_y0 - ty;
    int off = s_rbar_drag_off0 + dy / RBAR_ROW_H;
    if (off < 0) off = 0;
    if (off > RBAR_MAX_OFF) off = RBAR_MAX_OFF;
    if (off != s_rbar_off) { s_rbar_off = off; ctx->needs_redraw = true; }
}

static void p_diag_action(ui_ctx_t *ctx, os_action_t action) {
    if (g_mode == DI_MODE_BROWSE) {
        switch (action) {
        case OS_ACTION_LEFT:
            g_bcat = (g_bcat + DICAT_N - 1) % DICAT_N; g_lst = 0; g_lsc = 0; break;
        case OS_ACTION_RIGHT:
            g_bcat = (g_bcat + 1) % DICAT_N; g_lst = 0; g_lsc = 0; break;
        case OS_ACTION_UP:
            if (g_lst > 0) { g_lst--; if (g_lst < g_lsc) g_lsc = g_lst; } break;
        case OS_ACTION_DOWN:
            if (g_lst < di_count() - 1) { g_lst++; if (g_lst >= g_lsc + DI_LVIS) g_lsc++; } break;
        case OS_ACTION_CONFIRM:
            di_act_item(g_lst); break;
        case OS_ACTION_BACK:
        case OS_ACTION_HOME:
            os_pop(ctx);
            return;
        default: break;
        }
        ctx->needs_redraw = true;
        return;
    }
    if (g_stage == D_ST_RESULT) {
        switch (action) {
        case OS_ACTION_UP:
        case OS_ACTION_LEFT:
            if (g_result_k > 0) g_result_k--;
            break;
        case OS_ACTION_DOWN:
        case OS_ACTION_RIGHT:
            if (g_result_k < RSHOW - 1) g_result_k++;
            break;
        case OS_ACTION_BACK:
            dStepBack();
            break;
        default: break;
        }
        ctx->needs_redraw = true;
        return;
    }
    int n = dStageOptCount(g_stage);
    switch (action) {
    case OS_ACTION_UP:
    case OS_ACTION_LEFT:
        if (g_cur_opt > 0) g_cur_opt--;
        break;
    case OS_ACTION_DOWN:
    case OS_ACTION_RIGHT:
        if (g_cur_opt < n - 1) g_cur_opt++;
        break;
    case OS_ACTION_CONFIRM:
        dConfirmSelect();
        break;
    case OS_ACTION_BACK:
        if (g_stage == D_ST_CAT) { g_mode = DI_MODE_BROWSE; g_bcat = 2; g_lst = 0; g_lsc = 0; }
        else dStepBack();
        break;
    default: break;
    }
    ctx->needs_redraw = true;
}

static void p_diag_enter(ui_ctx_t *ctx) {
    (void)ctx;
    g_mode = DI_MODE_BROWSE; g_bcat = 0; g_lst = 0; g_lsc = 0;
    DReset();
}

static const os_module_t s_mod_diag = {
    .name       = "diag",
    .page_id    = OS_PAGE_DIAGNOSIS,
    .on_enter   = p_diag_enter,
    .render     = p_diag_render,
    .action     = p_diag_action,
    .touch      = p_diag_touch,
    .poll       = p_diag_poll,
    .fullscreen = false,
};

void os_page_diag_register(void) { os_register(&s_mod_diag); }
