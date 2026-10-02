/**
 * page_diag.c — 故障诊断 页面模块.
 *
 * 移植自早期版 (ESP32_BBK_3.3) menu/diagnosis.c 的完整 UI:
 *   - 顶部 40px 步进条 (现象/症状/设备/配置/结果, 可点返回)
 *   - 中部 22px 提问 + 故障选项 (等距, 选中黑底反白)
 *   - 右侧 176px 设备概率排序栏 (每个设备 64px 图标, 可拖动滚动;
 *     图标右侧名称+百分比, 下方进度条, 点图标弹排查方案)
 *   - 排除方案弹窗 (白底黑框, 标题条+4步+返回钮)
 * 已 os 化: 使用 ui_ctx_t 签名, 返回经 os_pop.
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "ui_common.h"
#include "font_zh.h"
#include "esp_attr.h"
#include "font_zh16.h"
#include "input.h"
#include "esp_timer.h"
#include "diag_dev_icons.inc"   /* 桌面「电脑诊断」6 器件 64x64 位图 (diag_dev_icons[]) */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DW 400   /* ST7305_WIDTH */
#define DH 300   /* ST7305_HEIGHT */

/* 顶部进度条区高度 (节点 + 上方文字) */
#define DBAR_H   40
/* 内容区起始 y */
#define DCONTENT_Y (DBAR_H)
/* 进度条水平外边距 */
#define DNODE_PAD 30
/* 右侧故障元件栏宽 & 分割线 x */
#define DRIGHT_W 176
#define DIVIDER_X (DW - DRIGHT_W)
/* 右侧元件图标尺寸 / 距分割线 / 图标间距 / 展示候选数 / 进度条高 */
#define RICON_S   64
#define RICON_PAD 4
#define RICON_GAP 5
#define RSHOW     4   /* 右栏默认显示 4 个器件 (第 4 个可部分露出, 提示可拖动) */
#define RPBAR_H   6

/* ================= 通用绘制辅助 (自包含) ================= */
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
static void drect(st7305_handle_t *l, int x0, int y0, int x1, int y1, int fill) {
    dfill(l, x0, y0, x1, y1, fill);
}
static void doutline(st7305_handle_t *l, int x0, int y0, int x1, int y1) {
    dhline(l, x0, x1, y0);
    dhline(l, x0, x1, y1);
    dvline(l, x0, y0, y1);
    dvline(l, x1, y0, y1);
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
static void dpct(st7305_handle_t *l, int x, int y) {
    static const uint8_t pat[7] = {0x18, 0x18, 0x04, 0x02, 0x01, 0x03, 0x03};
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (pat[row] & (1 << (4 - col)))
                dsetp(l, x + col, y + row);
}
/* 1.5x 放大数字 (介于小 dtxt 与大 dtxt2 之间): 每字约 8px 宽 */
static int dtxt15(st7305_handle_t *l, int x, int y, const char *s) {
    while (*s) {
        int gi = (unsigned char)*s >= '0' && (unsigned char)*s <= '9'
                 ? (*s - '0') : dgi(*s);
        if (gi >= 0) {
            const uint8_t *g = dgfont[gi];
            for (int cy = 0; cy < 7; cy++)
                for (int cx = 0; cx < 5; cx++)
                    if (g[cx] & (1 << cy)) {
                        int tx0 = cx * 3 / 2, tx1 = tx0 + ((cx & 1) ? 2 : 1);
                        int ty0 = cy * 3 / 2, ty1 = ty0 + ((cy & 1) ? 2 : 1);
                        for (int yy = ty0; yy < ty1; yy++)
                            for (int xx = tx0; xx < tx1; xx++) dsetp(l, x + xx, y + yy);
                    }
        }
        x += 8;
        s++;
    }
    return x;
}
/* 1.5x 放大 '%' 符号 */
static void dpct15(st7305_handle_t *l, int x, int y) {
    static const uint8_t pat[7] = {0x18, 0x18, 0x04, 0x02, 0x01, 0x03, 0x03};
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (pat[row] & (1 << (4 - col))) {
                int tx0 = col * 3 / 2, tx1 = tx0 + ((col & 1) ? 2 : 1);
                int ty0 = row * 3 / 2, ty1 = ty0 + ((row & 1) ? 2 : 1);
                for (int yy = ty0; yy < ty1; yy++)
                    for (int xx = tx0; xx < tx1; xx++) dsetp(l, x + xx, y + yy);
            }
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
static const char *DDEVNAME[DMAXDEV] = { "内存", "硬盘", "显卡", "主板", "电源", "系统" };
static int devprob[DMAXDEV];
/* 器件图标来自桌面「电脑诊断」位图 (diag_dev_icons[], 见 diag_dev_icons.inc):
 * 0内存(M.2) 1硬盘 2显卡 3主板 4电源 5系统(电脑) */

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
      }, { 28, 24, 20, 22, 18, 5 }, { 4, 3, 4, 4, 4, -1 }, 6 },
    { "卡顿闪退", {
        "开机和启动特别慢", "多开程序就卡顿", "运行游戏中闪退",
        "使用中莫名崩溃", "风扇狂转且过热", "不确定"
      }, { 26, 26, 22, 18, 14, 5 }, { 1, 0, 2, 0, 3, -1 }, 6 },
    { "蓝屏死机", {
        "蓝屏代码每次不同", "特定操作后蓝屏", "死机后无法关机",
        "开机引导时蓝屏", "蓝屏后重启循环", "不确定"
      }, { 26, 20, 18, 22, 16, 5 }, { 0, 2, 3, 1, 5, -1 }, 6 },
    { "花屏闪屏", {
        "屏幕出现彩色条纹", "画面雪花噪点", "屏幕整体闪烁跳动",
        "接外接屏显示正常", "开机花屏但能进系统", "不确定"
      }, { 30, 26, 18, 16, 20, 5 }, { 2, 2, 2, 3, 2, -1 }, 6 },
    { "加电无显", {
        "风扇转屏幕无信号", "主板灯亮但屏幕黑", "有开机蜂鸣声",
        "有背光但画面全黑", "完全无背光", "不确定"
      }, { 24, 20, 22, 16, 18, 5 }, { 2, 3, 0, 3, 4, -1 }, 6 },
    { "无法进入系统", {
        "卡在品牌标志界面", "提示修复恢复界面", "无限重启进不去",
        "黑屏停住不动", "能进但频繁卡死", "不确定"
      }, { 26, 24, 18, 18, 20, 5 }, { 1, 5, 4, 0, 1, -1 }, 6 },
};
static const char *DDEVTYPE[DMAXOPT] = {
    "台式机", "笔记本", "一体机", "工控机", "服务器", "不确定"
};
typedef struct {
    const char *cause;       /* 故障原因: 可能怎么导致 */
    const char *phen;        /* 典型现象: 该配件坏了会出现的表现 */
    const char *steps[4];    /* 排除方案 */
} diag_kb_t;
static const diag_kb_t DKB[DMAXDEV] = {
    { "内存颗粒虚焊或接触氧化，长期高温下电子元件隐性老化；多为进液、磕碰或出厂不良诱发。",
      "开机无显示并报警、蓝屏代码不固定、随机重启、程序莫名崩溃。",
      { "先用内存检测工具全盘扫描", "换好条与坏机交叉替换测", "重插并清洁金手指", "单根逐一开机定位坏条" } },
    { "硬盘坏道或盘面损伤，多因突然断电、搬运磕碰、长期高负载导致。",
      "开机卡Logo、读取奇慢、蓝屏0x7B、硬盘异响咔哒。",
      { "磁盘健康工具扫描修复", "换数据线或换盘交叉验证", "及时备份并隔离坏区", "反复复扫无坏道才算过" } },
    { "显存虚焊、核心过热或供电电路老化，多为积灰、超频或散热失效引起。",
      "花屏、雪花噪点、画面撕裂、游戏中突然黑屏。",
      { "图形烤机做长时间稳定性测试", "替换显卡交叉验证", "清灰并重涂散热硅脂", "长期满载无花屏则通过" } },
    { "南桥或供电芯片损坏、电容鼓包、液体腐蚀，多为供电异常或进水所致。",
      "不开机、进系统反复重启、USB与内存接口时好时坏。",
      { "用最小系统逐一替换法测", "目检电容有无鼓包焦糊味", "清灰并重插各部件", "逐一替换后能稳定为准" } },
    { "输出功率不足、电容老化、负载超限，多为长期老化或带载过高。",
      "重载时黑屏重启、电压不稳而花屏、开机无响应。",
      { "进设置监测各路输出电压", "换额定功率电源替换测试", "降低负载并检查供电插线", "满载运行不再断电为过" } },
    { "系统文件损坏、驱动冲突或引导异常，多为非正常关机或软件层故障。",
      "开机蓝屏循环、桌面卡死、驱动报错、无法进入系统。",
      { "用系统修复工具扫描检错", "用启动盘修复引导记录", "进安全模式排查驱动冲突", "还原或重装系统验证" } },
};
#define POPUP_X0 16
#define POPUP_X1 (DIVIDER_X - 14)
#define POPUP_Y0 (DCONTENT_Y + 56)
#define POPUP_Y1 (DH - 8)
static bool g_popup_on  = false;
static int  g_popup_dev = 0;

typedef enum {
    D_ST_DEV = 0, D_ST_CAT, D_ST_SUB, D_ST_CFG, D_ST_RESULT, D_ST_COUNT
} diag_stage_t;
static const char *DSTAGE_NAME[D_ST_COUNT] = { "设备", "现象", "症状", "配置", "结果" };
static const char *DSTAGE_QUESTION[D_ST_COUNT] = {
    "这台是什么设备", "请选择故障现象", "您看到了什么症状",
    "这台设备配置如何", "排查建议按概率排序"
};
static diag_stage_t g_stage = D_ST_DEV;
static int          g_cat   = 0;
static int          g_devtype = 0;
static int          g_cfg    = 0;
static bool         g_ans_sub = false, g_ans_devtype = false, g_ans_cfg = false;
static int          g_cur_opt = 0;
static int          g_result_k = 0;
/* 症状多轮: 在所选大类下逐条追问其多个症状, 每条选 符合/不符合/不确定, 可提前"完成" */
#define DSUB_MAX  DMAXOPT
static int  g_sub_idx = 0;        /* 当前问到的大类内症状索引 */
static bool g_sub_ans[DSUB_MAX];  /* 各条是否已作答 */
static int  g_sub_wt[DSUB_MAX];   /* 各条累计权重 (符合=原权, 不确定≈1/3, 不符=0) */

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
    if (g_ans_sub) {
        int nsub = DCAT[g_cat].nsub;
        for (int i = 0; i < nsub; i++)
            if (g_sub_ans[i] && g_sub_wt[i] > 0)
                devpplus(DCAT[g_cat].subdev[i], g_sub_wt[i]);
    }
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
static int  s_pix_off = 0;            /* 右栏整体像素偏移 (0..RBAR_MAX_PX), 跟手滚动 */
static int  s_drag_y0 = 0;
static int  s_drag_pix0 = 0;
static bool s_drag = false;
#define RBAR_ROW_H (RICON_S + RICON_GAP)
/* 最大滚动: 让最后一行 (索引 DMAXDEV-1) 的图标底能贴到屏底 (完全显示) */
#define RBAR_MAX_PX ((DMAXDEV - 1) * RBAR_ROW_H + (DCONTENT_Y + 4) + RICON_S - DH)

static void DReset(void) {
    for (int i = 0; i < DMAXDEV; i++) devprob[i] = 0;
    g_stage = D_ST_DEV;
    g_cat = 0; g_devtype = 0; g_cfg = 0;
    g_ans_sub = g_ans_devtype = g_ans_cfg = false;
    g_cur_opt = 0; g_result_k = 0; g_sub_idx = 0;
    for (int i = 0; i < DSUB_MAX; i++) { g_sub_ans[i] = false; g_sub_wt[i] = 0; }
    g_popup_on = false; g_popup_dev = 0;
    s_pix_off = 0; s_drag = false;
}
static int dStageOptCount(diag_stage_t st) {
    switch (st) {
        case D_ST_DEV: return 6;               /* 设备类型 */
        case D_ST_CAT: return 6;               /* 故障大类 */
        case D_ST_SUB: return 3;               /* 符合/不符合/结束 */
        case D_ST_CFG: return 2;
        case D_ST_RESULT: return DMAXDEV;
        default: return 0;
    }
}
static const char *dStageOptLabel(diag_stage_t st, int i) {
    switch (st) {
        case D_ST_DEV:   return DDEVTYPE[i];
        case D_ST_CAT:   return DCAT[i].label;
        case D_ST_SUB: { /* 逐条追问当前症状: 符合/不符合/结束排查 (无"不确定") */
            static const char *S[3] = { "符合", "不符合", "结束排查" };
            return S[i];
        }
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
    /* 症状阶段: 顶部问题 = 当前逐条追问的那条症状 */
    const char *q = (g_stage == D_ST_SUB) ? DCAT[g_cat].sub[g_sub_idx] : DSTAGE_QUESTION[g_stage];
    dz22s(l, 8, DCONTENT_Y + 4, q, false);
    int n = dStageOptCount(g_stage);
    if (n <= 0 || g_stage == D_ST_RESULT) return;
    int top = DCONTENT_Y + 36;
    int bot = DH - 8;
    int row_h = (bot - top) / n;
    int x0 = 8, x1 = DIVIDER_X - 8;
    /* 把若干行选项文本作为一个整体块水平居中 (以最长行为块宽, 非逐行居中):
     * 块左 = 区间中心对齐, 块内每行仍左对齐 块左. */
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int lw = ((int)(strlen(dStageOptLabel(g_stage, i)) / 3)) * 24;
        if (lw > maxw) maxw = lw;
    }
    int block_x = x0 + ((x1 - x0 + 1) - maxw) / 2;
    if (block_x < x0) block_x = x0;
    for (int i = 0; i < n; i++) {
        int y0 = top + i * row_h;
        int y1 = y0 + row_h - 1;
        bool sel = (i == g_cur_opt);
        if (sel) drect(l, x0, y0, x1, y1, ST7305_COLOR_BLACK);
        const char *nm = dStageOptLabel(g_stage, i);
        dz22s(l, block_x, y0 + (row_h - 24) / 2, nm, sel);
    }
}
/* 结果区 16px 自动换行 (每行约 maxc 字), 返回排版后的 y */
static int dwrap16(st7305_handle_t *l, int x0, int x1, int y0, const char *s, int lh) {
    int maxc = (x1 - x0 + 1) / 16; if (maxc < 1) maxc = 1;
    int n = 0, y = y0;
    for (const char *p = s; *p; p += 3) {
        if (y + 16 > DH) break;                 /* 越界不画 */
        dzh16(l, x0 + n * 16, y, p, false);
        if (++n >= maxc) { n = 0; y += lh; }
    }
    return y + lh;
}
/* 结果页: 售后工程师风格 - 故障配件(带概率)/故障原因/典型现象/排除方案 */
static void dDrawResult(st7305_handle_t *l) {
    dSortDev();
    int dev = dSortedIdx[g_result_k];
    int x0 = 10, x1 = DIVIDER_X - 8;
    int y = DCONTENT_Y + 34;
    /* 头: 故障配件 + 名称 + 概率 */
    int x = dz22s(l, x0, y, "故障配件", false);
    x = dz22s(l, x, y, DDEVNAME[dev], false);
    char buf[8]; snprintf(buf, sizeof(buf), "%d", devprob[dev]);
    dtxt15(l, x + 6, y + 6, buf);
    dpct15(l, x + 6 + (int)strlen(buf) * 8, y + 6);
    y += 30;
    /* 故障原因 */
    dzh16s(l, x0, y, "故障原因", true); y += 20;
    y = dwrap16(l, x0 + 4, x1, y, DKB[dev].cause, 20);
    y += 6;
    /* 典型现象 */
    dzh16s(l, x0, y, "典型现象", true); y += 20;
    y = dwrap16(l, x0 + 4, x1, y, DKB[dev].phen, 20);
    y += 6;
    /* 排除方案 (序号) */
    dzh16s(l, x0, y, "排除方案", true); y += 20;
    for (int i = 0; i < 4; i++) {
        char t[8]; snprintf(t, sizeof(t), "%d.", i + 1);
        dtxt15(l, x0 + 4, y, t);
        y = dwrap16(l, x0 + 4 + (int)strlen(t) * 8, x1, y, DKB[dev].steps[i], 20);
        y += 2;
    }
}
static void dDrawRightBar(st7305_handle_t *l) {
    dSortDev();
    /* 无分割线. 全部器件按像素偏移 s_pix_off 平移绘制 (跟手滚动). */
    int vyoff = DCONTENT_Y + 4;
    for (int i = 0; i < DMAXDEV; i++) {
        int iy = vyoff + i * RBAR_ROW_H - s_pix_off;   /* 跟手像素平移 */
        if (iy > DH || iy + RICON_S < 0) continue;   /* 整行全在屏外才跳; 顶部被进度条遮挡的部分仍绘制, 由其下露出的部分显示 */
        int di = dSortedIdx[i];
        int CY = iy + RICON_S / 2;     /* 图标垂直中心 */
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", devprob[di]);
        int dlen = (int)strlen(buf);
        int nm_w = (int)((strlen(DDEVNAME[di]) / 3)) * 16;
        /* 进度条: 贴最右, 高12, 相对图标垂直中心下移 10px */
        int bar_right = DW - 4;
        int bar_x0 = bar_right - 64 + 1;
        int bar_y = CY - 6 + 10;   /* 名称+进度条整体下移 10px (图标不动) */
        drect(l, bar_x0, bar_y, bar_right, bar_y + 11, ST7305_COLOR_WHITE);
        doutline(l, bar_x0, bar_y, bar_right, bar_y + 11);
        int fill_w = (int)((int64_t)(64) * devprob[di]) / 100;
        if (fill_w > 64) fill_w = 64;
        if (fill_w > 2) drect(l, bar_x0 + 1, bar_y + 1, bar_x0 + fill_w - 1, bar_y + 10, 0);
        /* 图标: 桌面「电脑诊断」对应器件位图, 紧靠进度条左边, 无框, 垂直居中 */
        int icon_left = bar_x0 - 8 - RICON_S;
        draw_1bpp_stretched(l, icon_left + RICON_S / 2, CY, RICON_S, RICON_S, 64, 64, diag_dev_icons[di]);
        /* 名称: 进度条正上方, 左对齐进度条 (随进度条下移10px) */
        int top_y = bar_y - 4 - 16;
        dzh16s(l, bar_x0, top_y, DDEVNAME[di], false);
        /* 百分比: 稍大(1.5x), 贴最右, 在进度条正上方 */
        int w15 = dlen * 8 + 8 + 2;   /* 数字(1.5x 每字8) + '%'(8) + 间隙 */
        int pct_x = (DW - 4) - w15;
        dtxt15(l, pct_x, top_y + 2, buf);
        dpct15(l, pct_x + dlen * 8, top_y + 2);
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

/* ================= 主渲染 / 推进 ================= */
static void p_diag_render(ui_ctx_t *ctx) {
    st7305_handle_t *l = ctx->lcd;
    st7305_clear(l, ST7305_COLOR_WHITE);
    /* 右栏先画 (被进度条遮挡的部分仍绘制, 便于露出的下半显示) */
    dDrawRightBar(l);
    /* 顶部步进条区整体清白, 盖掉右栏越顶残留, 保证进度条固定纯净 */
    dfill(l, 0, 0, DW - 1, DCONTENT_Y - 1, ST7305_COLOR_WHITE);
    if (g_stage == D_ST_RESULT) dDrawResult(l);
    else                        dDrawOptions(l);
    dDrawStageBar(l);   /* 进度条恒在顶层显示 */
    if (g_popup_on) dDrawPopup(l);
}
static void dConfirmSelect(void) {
    switch (g_stage) {
    case D_ST_DEV:
        g_devtype = g_cur_opt; g_cur_opt = 0; g_ans_devtype = true; dRecompute(); g_stage = D_ST_CAT;
        break;
    case D_ST_CAT: g_cat = g_cur_opt; g_cur_opt = 0; g_stage = D_ST_SUB; g_sub_idx = 0; g_ans_sub = false;
                   for (int zi = 0; zi < DSUB_MAX; zi++) { g_sub_ans[zi] = false; g_sub_wt[zi] = 0; }
                   break;
    case D_ST_SUB: {
        int i = g_sub_idx, nsub = DCAT[g_cat].nsub;
        /* 符合=原权, 不符合=0 */
        if (g_cur_opt == 0)      { g_sub_ans[i] = true; g_sub_wt[i] = DCAT[g_cat].subwt[i]; }
        else if (g_cur_opt == 1) { g_sub_ans[i] = true; g_sub_wt[i] = 0; }
        /* 选了"结束排查" 或 已问完最后一条 → 进入配置 */
        if (g_cur_opt == 2 || i >= nsub - 1) {
            g_ans_sub = true; dRecompute(); g_cur_opt = 0; g_sub_idx = 0; g_stage = D_ST_CFG;
        } else {
            g_sub_idx++; g_cur_opt = 0;   /* 继续追问下一条 */
        }
        break;
    }
    case D_ST_CFG:
        g_cfg = g_cur_opt; g_cur_opt = 0; g_ans_cfg = true; dRecompute();
        dSortDev(); g_result_k = 0; g_stage = D_ST_RESULT;
        break;
    case D_ST_RESULT: break;
    default: break;
    }
}
static void dInvalidateFrom(diag_stage_t st) {
    if (st <= D_ST_CAT)     g_ans_devtype = false;   /* 跳回 现象/设备 → 设备答案失效 */
    if (st <= D_ST_SUB) {
        g_ans_sub = false; g_sub_idx = 0;
        for (int zi = 0; zi < DSUB_MAX; zi++) { g_sub_ans[zi] = false; g_sub_wt[zi] = 0; }
    }
    if (st <= D_ST_CFG)     g_ans_cfg = false;
    dRecompute();
}
static void dStepBack(void) {
    if (g_stage <= D_ST_DEV) { g_stage = D_ST_DEV; g_cur_opt = 0; return; }
    switch (g_stage) {
        case D_ST_CAT:     g_ans_devtype = false; break;   /* 离开 现象 → 回设备重选 */
        case D_ST_SUB:     g_ans_sub = false; g_sub_idx = 0;
                   for (int zi = 0; zi < DSUB_MAX; zi++) { g_sub_ans[zi] = false; g_sub_wt[zi] = 0; }
                   break;
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
    if (x >= DIVIDER_X - 8 || x < 8) return -1;
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
static int dHitRightDev(int x, int y) {
    if (x < DIVIDER_X - 2) return -1;
    int vyoff = DCONTENT_Y + 4;
    if (y < vyoff) return -1;
    int rel = y - vyoff + s_pix_off;      /* 相对内容顶 (像素) */
    int i = rel / RBAR_ROW_H;
    if (i < 0 || i >= DMAXDEV) return -1;
    if (rel - i * RBAR_ROW_H > RICON_S) return -1;   /* 仅图标高度内命中 */
    dSortDev();
    return dSortedIdx[i];
}
static bool p_diag_touch(ui_ctx_t *ctx, int x, int y) {
    ctx->needs_redraw = true;
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
            dInvalidateFrom((diag_stage_t)best);
        }
        return false;
    }
    int dev = dHitRightDev(x, y);
    if (dev >= 0 && g_stage != D_ST_DEV) { g_popup_dev = dev; g_popup_on = true; return false; }
    int hit = dHitOption(x, y);
    if (hit >= 0 && hit < dStageOptCount(g_stage)) { g_cur_opt = hit; dConfirmSelect(); }
    return false;
}
static void p_diag_poll(ui_ctx_t *ctx) {
    if (g_popup_on) { s_drag = false; return; }
    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    if (!down) {
        if (s_drag) {              /* 松手: 吸附到整行 */
            s_drag = false;
            int snap = (s_pix_off + RBAR_ROW_H / 2) / RBAR_ROW_H;
            int p = snap * RBAR_ROW_H;
            if (p < 0) p = 0;
            if (p > RBAR_MAX_PX) p = RBAR_MAX_PX;   /* 贴底: 确保最后一行能完全显示 */
            if (p != s_pix_off) { s_pix_off = p; ctx->needs_redraw = true; }
        }
        return;
    }
    if (tx < DIVIDER_X - 2) return;      /* 仅在右侧栏内拖动 */
    if (!s_drag) {
        s_drag = true; s_drag_y0 = ty; s_drag_pix0 = s_pix_off;
        return;
    }
    int off = s_drag_pix0 + (s_drag_y0 - ty);    /* 手指上滑 → 内容上移(看更靠后的行) */
    if (off < 0) off = 0;
    if (off > RBAR_MAX_PX) off = RBAR_MAX_PX;
    if (off != s_pix_off) { s_pix_off = off; ctx->needs_redraw = true; }
}
static void p_diag_action(ui_ctx_t *ctx, os_action_t action) {
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
        if (g_stage == D_ST_DEV) { os_pop(ctx); return; }   /* 最顶层: 返回上一页 */
        dStepBack();
        break;
    default: break;
    }
    ctx->needs_redraw = true;
}
static void p_diag_enter(ui_ctx_t *ctx) {
    (void)ctx;
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
    .fullscreen = true,   /* 早期版: 无 os 状态栏, 顶部用诊断自己的步进条(进度条)替代 */
};

void os_page_diag_register(void) { os_register(&s_mod_diag); }