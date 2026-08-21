/**
 * @file diagnosis.c
 * @brief 故障诊断应用 (自包含实现).
 *
 * 布局 (400x300):
 *  0..31        顶部 32px 步进条 (替代状态栏)
 *  32..300      内容区: 左侧/中部 = 故障选项, 右侧 = 设备概率排序栏
 *
 * 交互:
 *  - 选项 3x2 网格: 触摸点选 / 上下左右键 + 确认选中.
 *  - 顶步进条: 每往下细化一层加一段, 均分铺满; 拥挤时只显关键节点文字.
 *  - 可点顶部任一已走过的节点段返回该步修改.
 */
#include "diagnosis.h"
#include "menu_system.h"
#include "st7305.h"
#include "esp_timer.h"
#include "font_zh.h"
#include "font_zh16.h"
#include "input.h"   /* V1.0.98: 右侧栏拖动滚动用 input_get_touch_pos */
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
#define RICON_GAP 5          /* 图标之间的垂直间距 */
#define RSHOW     3          /* 右侧最多展示候选数 (64px+5px 间距约束) */
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
/* 中文: 12x12 小字 (把 24x24 字库 2x2 合 1), 同 menu_system 的 draw_zh_small. */
static void dzh(st7305_handle_t *l, int x, int y, const char *str, bool inverted) {
    int idx = font_zh_find_utf8(str);
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    dfill(l, x, y, x + 11, y + 11, bg);
    if (idx < 0) return;
    const uint8_t *bmp = zh_font_data[idx];
    int bpr = (ZH_FONT_W + 7) / 8;
    for (int r = 0; r < 12; r++) {
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
}
/* 多字中文: 逐字(每字3字节UTF-8)由 dzh 绘制, 调用方保证输入为纯汉字. 返回新 x. */
static int dzhs(st7305_handle_t *l, int x, int y, const char *s, bool inv) {
    while (*s) { dzh(l, x, y, s, inv); x += 12; s += 3; }
    return x;
}
/* 本地 5x7 数字/字符小表 (自包含, 无外部依赖) */
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
/* 紧凑英文/数字/符号: 5x7 表 1x 原样直绘, 每字符 6px 宽 (比此前 2x 更小, 仅用于右侧百分比). 返回新 x. */
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
static int dtw(const char *s) { return (int)strlen(s) * 12; }

/* ===== 16px 点阵宋体 (font_zh16): 右侧元件名 / 弹窗方案 / 单行选项 ===== */
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
/* 多字 16px, 逐字(UTF-8 每字3字节)绘制. 返回新 x. */
static int dzh16s(st7305_handle_t *l, int x, int y, const char *s, bool inv) {
    while (*s) { dzh16(l, x, y, s, inv); x += 16; s += 3; }
    return x;
}

/* ===== 22px 中文 (24x24 字库, 字形为 22px 生成, 满格绘制) =====
 * 用于顶部提问文字, 与 menu_system 的 draw_zh(scale=1) 等价. */
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
/* 多字 22px, 逐字(UTF-8 每字3字节)绘制. 返回新 x. */
static int dz22s(st7305_handle_t *l, int x, int y, const char *s, bool inv) {
    while (*s && *s != '\n') { dz22(l, x, y, s, inv); x += 24; s += 3; }
    return x;
}

/* ===== 紧凑正确 '%' 符号 (5列 x7行, 1x) =====
 * 左上小圆 + 对角斜线 + 右下小圆. 修正原 dpct 形状错误, 且不再 2x 放大. */
static void dpct(st7305_handle_t *l, int x, int y) {
    static const uint8_t pat[7] = {0x18, 0x18, 0x04, 0x02, 0x01, 0x03, 0x03};
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (pat[row] & (1 << (4 - col)))
                dsetp(l, x + col, y + row);
}
/* 右侧百分比: 数字(+%号), 返回新 x. */
static int dpct_text(st7305_handle_t *l, int x, int y, int val) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", val);
    x = dtxt(l, x, y, buf);
    dpct(l, x, y);
    return x + 6;
}

/* 前置声明 (dline/dcircle/dcircle_w 定义在后方几何辅助区) */
static void dline(st7305_handle_t *l, int x0, int y0, int x1, int y1);
static void dcircle(st7305_handle_t *l, int cx, int cy, int r);
static void dcircle_w(st7305_handle_t *l, int cx, int cy, int r);

/* ===== 放大的 2x 数字 (5x7 表 -> 10x14, 每字符 12px 宽): 侧栏百分比 ===== */
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
/* ===== 放大的细笔画 '%' (细斜线 + 两个小圆, 高约 14px, 宽约 12px) ===== */
static void dpct2(st7305_handle_t *l, int x, int y) {
    dline(l, x + 7, y - 1, x + 4, y + 13);   /* 细斜线 */
    dcircle(l, x + 2, y + 2, 2);               /* 左上圆 */
    dcircle(l, x + 9, y + 11, 2);              /* 右下圆 */
}

static uint32_t dmnow(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ================= 决策知识库 (const, 内置 Flash) =================
 * 每个诊断选项都会对若干"疑似设备/部件"的概率做增减. 概率 = 0..100.
 * 模型:
 *  故障大类(6) → 细分(2级) → 设备类型咨询 → 配置咨询 → 结果展示.
 * 概率随用户选择方向累加, 最终按降序显示右侧.
 */
#define DMAXOPT  8    /* 每层最多选项数 */
#define DMAXDEV  6    /* 右侧设备栏最多候选 */

typedef struct {
    const char *label;      /* 选项中文名 (<=4 字建议) */
    int         weight;     /* 选择该选项对主因的倾向 (示意, 影响设备概率) */
    int         target;     /* 倾向的设备索引 (0..DMAXDEV-1), -1=均分 */
} diag_opt_t;

typedef struct {
    diag_opt_t opts[DMAXOPT];
    int        nopts;
} diag_level_t;

/* 设备候选名 (右侧概率栏) */
static const char *DDEVNAME[DMAXDEV] = {
    "内存", "硬盘", "显卡", "主板", "电源", "系统"
};

/* 一组设备的基础概率 (随选择增减, 每层都被追加影响) */
static int devprob[DMAXDEV];

/* ============ 6 大故障类 ============ */
/* 每个大类: 若干细分(第2层), 每细分再给出咨询选项(第3层). 为控制复杂度,
 * 这里用 2 层: 大类 (3x2) → 细分 (最多 DMAXOPT). 细分选项各自带 probability
 * 增量, 顶层每大类给一个基础倾向表. */
typedef struct {
    const char  *label;                 /* 大类中文名 */
    const char *sub[DMAXOPT];           /* 细分: 表现症状选项名 */
    int         subwt[DMAXOPT];         /* 每症状对应的设备倾向权重 */
    int         subdev[DMAXOPT];        /* 每症状倾向的设备索引 (0内存1硬盘2显卡3主板4电源5系统, -1=均分) */
    int         nsub;
} diag_cat_t;

/* 大类排序: 字数大的放下面; 字数相同按严重程度, 越严重越放下面.
 * 不通电(3字) → 卡顿闪退/蓝屏死机/花屏闪屏/加电无显(4字, 轻→重) → 无法进入系统(6字). */
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

/* 设备类型咨询选项 */
static const char *DDEVTYPE[DMAXOPT] = {
    "台式机", "笔记本", "一体机", "工控机", "服务器", "不确定"
};

/* ===== 排查知识库 =====
 * DDEVNAME 顺序: 0内存 1硬盘 2显卡 3主板 4电源 5系统.
 * cause   = 可能原因 (结果页展示)
 * steps[4]= 软件测试 / 硬件交换测试 / 解决办法 / 验证确认 (弹窗与结果页共用)
 * tip     = 辅助提示 (结果页展示). 行内纯汉字, 由 dzhs/dzh16s 逐字绘制. */
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

/* ===== 排除方案弹窗 (中间内容区覆盖层) ===== */
#define POPUP_X0 16
#define POPUP_X1 (DIVIDER_X - 14)
#define POPUP_Y0 (DCONTENT_Y + 56)
#define POPUP_Y1 (DH - 8)
static bool g_popup_on  = false;   /* 弹窗是否打开 */
static int  g_popup_dev = 0;       /* 当前弹窗对应的故障元件索引 */

/* ================= 诊断状态 ================= */
typedef enum {
    D_ST_CAT = 0,   /* 选故障大类 (6, 3x2) */
    D_ST_SUB,       /* 选细分选项 */
    D_ST_DEVTYPE,   /* 咨询设备类型 */
    D_ST_CFG,       /* 咨询配置 (有无独显/内存是否板载) */
    D_ST_RESULT,    /* 结果显示 */
    D_ST_COUNT
} diag_stage_t;

/* 关键节点步进条显示名 (顶部节点上方文字, 固定 5 段) */
static const char *DSTAGE_NAME[D_ST_COUNT] = {
    "现象", "症状", "设备", "配置", "结果"
};
/* 每步的提问文字 (22px, 中间内容区顶部) */
static const char *DSTAGE_QUESTION[D_ST_COUNT] = {
    "请选择故障现象", "您看到了什么症状",
    "这台是什么设备", "这台设备配置如何",
    "排查建议按概率排序"
};

static diag_stage_t g_stage = D_ST_CAT;
static int          g_cat   = 0;   /* 选中的大类索引 */
static int          g_sub   = 0;   /* 选中的症状索引 */
static int          g_devtype = 0; /* 设备类型索引 */
static int          g_cfg    = 0;  /* 配置: 0=有独显 1=无独显 */
/* 各阶段是否已作答 (用于"重算式"概率, 返回上一步后自动失效, 避免重复累加) */
static bool         g_ans_sub = false, g_ans_devtype = false, g_ans_cfg = false;
static int          g_cur_opt = 0; /* 当前选项游标 (按键导航) */
static int          g_result_k = 0;/* 结果页当前查看的候选名次 (0=概率最高) */
static uint32_t     g_blink_ms = 0;

/* 概率定向累加: target<0 表示均分到所有设备. */
static void devpplus(int target, int weight) {
    if (target < 0 || target >= DMAXDEV) { /* 均分 */
        int each = weight / DMAXDEV;
        for (int i = 0; i < DMAXDEV; i++) devprob[i] += each;
        return;
    }
    devprob[target] += weight;
}

/* ===== 重算式概率 =====
 * 每次作答/返回都从零重算, 只累计"已作答"阶段的贡献. 相比旧的增量累加,
 * 来回切换/返回修改不会重复叠加, 结果稳定且成功率更高. */
static void dRecompute(void) {
    for (int i = 0; i < DMAXDEV; i++) devprob[i] = 0;
    /* 1) 症状定向 (权重最大的主信号) */
    if (g_ans_sub)
        devpplus(DCAT[g_cat].subdev[g_sub], DCAT[g_cat].subwt[g_sub]);
    /* 2) 设备类型修正 (不同机型故障分布不同) */
    if (g_ans_devtype) {
        switch (g_devtype) {
            case 0: devprob[4] += 4; devprob[2] += 4; break; /* 台式: 电源/独显 */
            case 1: devprob[3] += 4; devprob[0] += 2; devprob[4] += 2; break; /* 笔记本 */
            case 2: devprob[3] += 4; devprob[2] += 2; break; /* 一体机 */
            case 3: devprob[3] += 4; devprob[4] += 4; break; /* 工控机 */
            case 4: devprob[0] += 6; devprob[4] += 4; devprob[1] += 4; break; /* 服务器 */
            default: break; /* 不确定: 不修正 */
        }
    }
    /* 3) 配置修正 (有无独显影响显卡/主板倾向) */
    if (g_ans_cfg) {
        if (g_cfg == 0) { devprob[2] += 6; devprob[4] += 4; }   /* 有独显 */
        else            { devprob[3] += 4; devprob[0] += 2; }   /* 无独显(核显) */
    }
}

/* V1.0.98: 右侧栏滚动状态. 栏内靠顶显示 RSHOW 行, 全部 DMAXDEV 个配件
 * 通过在栏内上下拖动换行查看 (行级滚动, 不做像素裁剪). */
static int  s_rbar_off = 0;        /* 首行显示的排序索引 (0..DMAXDEV-RSHOW) */
static bool s_rbar_drag = false;   /* 拖动进行中 */
static int  s_rbar_drag_y0 = 0;    /* 拖动起点 y */
static int  s_rbar_drag_off0 = 0;  /* 拖动起点时的 off */
#define RBAR_ROW_H (RICON_S + RICON_GAP)
#define RBAR_MAX_OFF (DMAXDEV - RSHOW)

/* 重置. */
static void DReset(void) {
    for (int i = 0; i < DMAXDEV; i++) devprob[i] = 0;  /* 初始各设备故障概率为 0 */
    g_stage = D_ST_CAT;
    g_cat = 0; g_sub = 0; g_devtype = 0; g_cfg = 0;
    g_ans_sub = g_ans_devtype = g_ans_cfg = false;
    g_cur_opt = 0; g_result_k = 0;
    g_popup_on = false; g_popup_dev = 0;
    s_rbar_off = 0; s_rbar_drag = false;   /* V1.0.98: 右侧栏滚动复位 */
}

/* 当前层的选项总数 (用于按键导航边界). */
static int dStageOptCount(diag_stage_t st) {
    switch (st) {
        case D_ST_CAT: return 6;
        case D_ST_SUB: return DCAT[g_cat].nsub;
        case D_ST_DEVTYPE: return 6;
        case D_ST_CFG: return 2;   /* 有独显 / 无独显 */
        case D_ST_RESULT: return DMAXDEV;
        default: return 0;
    }
}

/* 选项名称. */
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

/* ================= 右侧设备概率排序 ================= */
/* 返回按概率降序排列的设备索引表 (0..count-1). */
static int dSortedIdx[DMAXDEV];
static void dSortDev(void) {
    for (int i = 0; i < DMAXDEV; i++) dSortedIdx[i] = i;
    for (int i = 0; i < DMAXDEV - 1; i++)
        for (int j = i + 1; j < DMAXDEV; j++)
            if (devprob[dSortedIdx[j]] > devprob[dSortedIdx[i]]) {
                int t = dSortedIdx[i]; dSortedIdx[i] = dSortedIdx[j]; dSortedIdx[j] = t;
            }
}

/* ================= 顶部进度条绘制 =================
 * 一条连续线 + 固定 5 个节点(现象/细分/设备/配置/结果), 字在节点上方.
 * 已走过的节点实心、可点击返回; 当前节点高亮(空心大圆); 未走到的浅色空心、不可点. */
static void dDrawStageBar(st7305_handle_t *l) {
    const int n = D_ST_COUNT;
    int xs[5];
    for (int i = 0; i < n; i++)
        xs[i] = DNODE_PAD + (DW - 2 * DNODE_PAD) * i / (n - 1);
    int cur = (int)g_stage;
    int lineY = DBAR_H - 10;                 /* 线在底部, 文字在线上面 */
    /* 先整条浅色基线 */
    dhline(l, xs[0], xs[n - 1], lineY);
    /* 已走过段: 从首节点到当前节点画粗实线 */
    if (cur >= 0) {
        for (int dy = 0; dy < 3; dy++)
            dhline(l, xs[0], xs[cur], lineY + dy);
    }
    /* 节点 + 上方文字 */
    for (int i = 0; i < n; i++) {
        int cx = xs[i];
        /* 已走过/当前: 独立实心黑圆 (画在粗线上方, 明显可辨); 未走到: 浅色空心圆 */
        if (i <= cur) {
            dcircle(l, cx, lineY, (i == cur) ? 6 : 5);
        } else {
            dcircle(l, cx, lineY, 3);
            dcircle_w(l, cx, lineY, 2);
        }
        /* 文字(16px)在线之上, 距顶部 2px, 水平居中 */
        const char *nm = DSTAGE_NAME[i];
        int tw = ((int)strlen(nm) / 3) * 16;    /* 16px 每字 */
        dzh16s(l, cx - tw / 2, 2, nm, (i == cur));
    }
}

/* ================= 中部内容区 =================
 * 顶部 22px 提问文字, 下方选项一行一个 (等距铺满, 选中黑底反白, 选项 22px). */
static void dDrawOptions(st7305_handle_t *l) {
    /* 提问 (22px), 位于内容区顶部 */
    dz22s(l, 8, DCONTENT_Y + 4, DSTAGE_QUESTION[g_stage], false);
    int n = dStageOptCount(g_stage);
    if (n <= 0 || g_stage == D_ST_RESULT) return;   /* 结果阶段走报告绘制 */
    int top = DCONTENT_Y + 36;
    int bot = DH - 8;
    int row_h = (bot - top) / n;                    /* 等距铺满 */
    int x0 = 8, x1 = DIVIDER_X - 8;
    for (int i = 0; i < n; i++) {
        int y0 = top + i * row_h;
        int y1 = y0 + row_h - 1;
        bool sel = (i == g_cur_opt);
        /* 选中: 黑底反白; 未选中: 白底黑字, 不加边框 (简洁原生) */
        if (sel) drect(l, x0, y0, x1, y1, ST7305_COLOR_BLACK);
        const char *nm = dStageOptLabel(g_stage, i);
        dz22s(l, x0 + 8, y0 + (row_h - 24) / 2, nm, sel);   /* 选项 22px */
    }
}

/* ================= 结果页报告 =================
 * 针对概率最高(或上下键切换)的候选给出完整报告:
 * 判断 → 可能原因 → 软件测试 → 硬件交换测试 → 解决办法 → 验证确认 → 辅助提示. */
static void dDrawResult(st7305_handle_t *l) {
    dSortDev();
    int dev = dSortedIdx[g_result_k];
    int x0 = 10;
    int y  = DCONTENT_Y + 38;
    /* 首行 22px: 判断 + 设备名 + 故障 */
    int x = dz22s(l, x0, y, "判断", false);
    x = dz22s(l, x, y, DDEVNAME[dev], false);
    dz22s(l, x, y, "故障", false);
    y += 32;
    /* 明细行 16px: 标签 + 内容 */
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
    /* 底部操作提示 (12px 小字) */
    dzhs(l, x0, DH - 16, "上下键换候选点图标看方案", false);
}

/* ================= 右侧故障元件排序栏 ================= */
/* -- 简笔几何辅助 (自包含) -- */
static void dline(st7305_handle_t *l, int x0, int y0, int x1, int y1) {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (abs(dx) > abs(dy)) ? abs(dx) : abs(dy);
    if (steps == 0) { dsetp(l, x0, y0); return; }
    for (int i = 0; i <= steps; i++) {
        dsetp(l, x0 + dx * i / steps, y0 + dy * i / steps);
    }
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

/* -- 右侧各故障元件的简笔示意图标 (每类一个, 居中于 64x64 外框内) -- */
static void dDrawDevIcon(st7305_handle_t *l, int dev, int cx, int cy) {
    switch (dev) {
    case 0: {  /* 内存: 竖条内存 + 颗粒 + 底部金手指 */
        doutline(l, cx - 16, cy - 16, cx + 16, cy + 8);
        for (int i = -10; i <= 10; i += 5)
            dvline(l, cx + i, cy - 11, cy - 4);
        dvline(l, cx - 12, cy + 8, cy + 16);
        dvline(l, cx - 4,  cy + 8, cy + 16);
        dvline(l, cx + 4,  cy + 8, cy + 16);
        dvline(l, cx + 12, cy + 8, cy + 16);
        break;
    }
    case 1: {  /* 硬盘: 盘片 (外圆 + 内环 + 中心轴) */
        dcircle(l, cx, cy, 19);
        dcircle_w(l, cx, cy, 16);
        dcircle(l, cx, cy, 4);
        break;
    }
    case 2: {  /* 显卡: 板卡 + 中置散热风扇 */
        doutline(l, cx - 19, cy - 13, cx + 19, cy + 13);
        dcircle(l, cx, cy, 6);                 /* 风扇轮毂 */
        dhline(l, cx - 12, cx + 3, cy - 10);
        dvline(l, cx + 10, cy - 5, cy + 5);
        dhline(l, cx - 12, cx + 3, cy + 10);
        dvline(l, cx - 10, cy - 5, cy + 5);
        break;
    }
    case 3: {  /* 主板: 电路板 + 中央芯片 + 走线 */
        doutline(l, cx - 19, cy - 18, cx + 19, cy + 18);
        drect(l, cx - 9, cy - 9, cx + 9, cy + 6, 0);
        doutline(l, cx - 10, cy - 10, cx + 10, cy + 7);
        dhline(l, cx - 19, cx - 10, cy - 14);  dhline(l, cx + 10, cx + 19, cy - 14);
        dhline(l, cx - 19, cx - 10, cy + 14);  dhline(l, cx + 10, cx + 19, cy + 14);
        break;
    }
    case 4: {  /* 电源: 闪电符号 */
        dline(l, cx + 3, cy - 18, cx - 6, cy + 1);
        dline(l, cx - 6, cy + 1, cx + 6, cy + 1);
        dline(l, cx + 6, cy + 1, cx - 3, cy + 18);
        break;
    }
    default: { /* 系统: 桌面窗口 (标题栏 + 内容条) */
        doutline(l, cx - 18, cy - 16, cx + 18, cy + 16);
        drect(l, cx - 18, cy - 16, cx + 18, cy - 10, 0);
        drect(l, cx - 12, cy - 6, cx + 12, cy - 1, 0);
        drect(l, cx - 12, cy + 3, cx + 12, cy + 12, 0);
        break;
    }
    }
}

/* 概率排序栏: 靠顶显示 RSHOW 行, 每行 = 64x64 图标 + 右侧名称(16px)+百分比,
 * 图标间固定 5px 垂直间距; 名称后紧跟百分比, 进度条在其下方. 概率降序.
 * V1.0.98: 全部 DMAXDEV 个配件可经栏内拖动滚动查看 (s_rbar_off 行偏移). */
static void dDrawRightBar(st7305_handle_t *l) {
    dSortDev();
    /* 分割线 */
    dvline(l, DIVIDER_X, DCONTENT_Y, DH - 1);
    int vyoff = DCONTENT_Y + 4;   /* 靠顶显示 */
    int icon_x = DIVIDER_X + RICON_PAD;
    for (int k = 0; k < RSHOW; k++) {
        int si = s_rbar_off + k;
        if (si >= DMAXDEV) break;
        int di = dSortedIdx[si];
        int ic_y = vyoff + k * RBAR_ROW_H;
        /* 64x64 图标外框 (白底黑框), 简笔元件图标居中 */
        drect(l, icon_x, ic_y, icon_x + RICON_S - 1, ic_y + RICON_S - 1, 0);
        doutline(l, icon_x, ic_y, icon_x + RICON_S - 1, ic_y + RICON_S - 1);
        dDrawDevIcon(l, di, icon_x + RICON_S / 2, ic_y + RICON_S / 2);
        /* 名称(16px)在图标右侧, 百分比紧跟名称末 (贴近), 同一行 */
        int name_x = icon_x + RICON_S + 4;
        int name_y = ic_y + 4;
        dzh16s(l, name_x, name_y, DDEVNAME[di], false);
        int di_nchar = (int)(strlen(DDEVNAME[di]) / 3);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", devprob[di]);
        int pct_x = name_x + di_nchar * 16 + 4;   /* 紧贴名称后面 */
        dtxt2(l, pct_x, name_y, buf);
        dpct2(l, pct_x + (int)strlen(buf) * 12, name_y);
        /* 进度条: 白底黑框, 在文字正下方 4px */
        int bar_y = name_y + 16 + 4;
        int bx0 = name_x, bx1 = DIVIDER_X + DRIGHT_W - 6;
        drect(l, bx0, bar_y, bx1, bar_y + RPBAR_H - 1, 1);
        doutline(l, bx0, bar_y, bx1, bar_y + RPBAR_H - 1);
        int fill_w = (int)((bx1 - bx0 + 1) * devprob[di]) / 100;
        if (fill_w > bx1 - bx0 + 1) fill_w = bx1 - bx0 + 1;
        if (fill_w > 2) drect(l, bx0 + 1, bar_y + 1, bx0 + fill_w - 1, bar_y + RPBAR_H - 2, 0);
    }
}

/* ===== 排除方案弹窗绘制 (覆盖在中间内容区) =====
 * 居中弹出: 白底黑框, 顶标题(黑底反白), 中部 4 行 16px 排查步骤, 底部"返回"钮.
 * 注意: 本屏 0=黑 1=白, 弹窗主体必须白底(1)黑字, 此前误用 0 导致整块黑底. */
static void dDrawPopup(st7305_handle_t *l) {
    int px0 = POPUP_X0, px1 = POPUP_X1, py0 = POPUP_Y0, py1 = POPUP_Y1;
    drect(l, px0, py0, px1, py1, ST7305_COLOR_WHITE);   /* 白底 */
    doutline(l, px0, py0, px1, py1);
    /* 标题 (黑底反白, 16px): "元件名 排查方案" */
    char title[24];
    snprintf(title, sizeof(title), "%s排查方案", DDEVNAME[g_popup_dev]);
    size_t tl = strlen(title) / 3;
    int tw = (int)tl * 16;
    int tx = px0 + (px1 - px0 - tw) / 2;
    drect(l, px0, py0, px1, py0 + 18, ST7305_COLOR_BLACK);   /* 标题条黑底 */
    dzh16s(l, tx, py0 + 1, title, true);
    /* 4 行 16px 排查步骤 (白底黑字) */
    int ly = py0 + 26;
    for (int i = 0; i < 4; i++) {
        const char *ln = DKB[g_popup_dev].steps[i];
        size_t llen = strlen(ln) / 3;
        int lw = (int)llen * 16;
        dzh16s(l, px0 + (px1 - px0 - lw) / 2, ly, ln, false);
        ly += 19;
    }
    /* 底部"返回"钮 (黑底白字) */
    int bx0 = px0 + (px1 - px0) / 2 - 34, bx1 = bx0 + 68;
    int by0 = py1 - 26, by1 = py1 - 4;
    drect(l, bx0, by0, bx1, by1, ST7305_COLOR_BLACK);
    dzh16s(l, bx0 + (68 - 32) / 2, by0 + 3, "返回", true);
}

/* ================= 主渲染 ================= */
void diag_render(menu_state_t *state) {
    st7305_handle_t *l = state->lcd;
    st7305_clear(l, ST7305_COLOR_WHITE);
    dDrawStageBar(l);
    if (g_stage == D_ST_RESULT) dDrawResult(l);   /* 结果页: 完整报告 */
    else                        dDrawOptions(l);  /* 问答页: 提问+选项 */
    dDrawRightBar(l);
    if (g_popup_on) dDrawPopup(l);
}

/* ================= 进度推进 / 返回 ================= */
/* 选中当前选项 → 推进到下一阶段 (概率按"已作答"重算). */
static void dConfirmSelect(void) {
    switch (g_stage) {
    case D_ST_CAT:
        g_cat = g_cur_opt;
        g_cur_opt = 0;
        /* 选大类本身不累计概率: 等选中具体症状再定向累加, 避免初始就集体涨% */
        g_stage = D_ST_SUB;
        break;
    case D_ST_SUB:
        g_sub = g_cur_opt;
        g_cur_opt = 0;
        g_ans_sub = true;
        dRecompute();   /* 症状定向 + 已答阶段修正, 重算防重复累加 */
        g_stage = D_ST_DEVTYPE;
        break;
    case D_ST_DEVTYPE:
        g_devtype = g_cur_opt;
        g_cur_opt = 0;
        g_ans_devtype = true;
        dRecompute();
        g_stage = D_ST_CFG;
        break;
    case D_ST_CFG:
        g_cfg = g_cur_opt;
        g_cur_opt = 0;
        g_ans_cfg = true;
        dRecompute();
        dSortDev();
        g_result_k = 0;   /* 结果页默认展示概率最高者 */
        g_stage = D_ST_RESULT;
        break;
    case D_ST_RESULT:
        /* 结果页确认: 选中项即最终判定 (此处仅保留, 无下一级) */
        break;
    default:
        break;
    }
}

/* 失效从某阶段起的所有"已作答"标记 (顶部进度条跳回时用), 并重算概率. */
static void dInvalidateFrom(diag_stage_t st) {
    if (st <= D_ST_SUB)     g_ans_sub = false;
    if (st <= D_ST_DEVTYPE) g_ans_devtype = false;
    if (st <= D_ST_CFG)     g_ans_cfg = false;
    dRecompute();
}

/* 返回上一阶段 (进度条可点返回). 返回后该阶段答案失效并重算概率. */
static void dStepBack(void) {
    if (g_stage <= D_ST_CAT) { g_stage = D_ST_CAT; g_cur_opt = 0; return; }
    /* 离开某阶段即清除其"已作答"标记, 重新作答才计入 */
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

/* ================= 触摸 / 按键 / 轮询 ================= */
/* 返回命中选项索引 (单行等距), -1 未命中. */
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

/* 右侧概率栏中命中的故障元件索引 (靠顶 RSHOW 行 + 滚动偏移), -1 未命中. */
static int dHitRightDev(int x, int y) {
    if (x < DIVIDER_X - 2) return -1;
    int vyoff = DCONTENT_Y + 4;
    if (y < vyoff) return -1;
    int k = (y - vyoff) / RBAR_ROW_H;
    if (k < 0 || k >= RSHOW) return -1;
    /* 只认图标方框内 (含右侧名称区), 间隙不命中 */
    if (y > vyoff + k * RBAR_ROW_H + RICON_S) return -1;
    int si = s_rbar_off + k;
    if (si >= DMAXDEV) return -1;
    dSortDev();
    return dSortedIdx[si];
}

bool diag_touch(menu_state_t *state, int x, int y) {
    /* 触摸点击本函数内部"完整消费"(选中/推进/返回/弹窗), 返回 false 避免框架
     * 再触发 menu_handle_action(CONFIRM) 造成二次推进. 因此这里必须主动置位
     * needs_redraw, 否则 menu_render 被 needs_redraw 门控, 点击后不重绘 (物理键
     * 路径已在 menu_handle_action 里置位, 触摸路径此前漏设导致"点了没反应/卡"). */
    state->needs_redraw = true;
    /* 弹窗打开时: 点"返回"钮关闭; 点在弹窗外关闭; 点在弹窗内容内忽略. */
    /* 说明: 本函数内部已"完整消费"点击 (选中/推进/返回/弹窗),
     * 因此一律返回 false, 避免框架在 menu_handle_action(CONFIRM) 里再触发
     * 一次 dConfirmSelect 导致"点一下跳两级" (与电子书/USB HID 页同策略). */
    if (g_popup_on) {
        int bx0 = POPUP_X0 + (POPUP_X1 - POPUP_X0) / 2 - 34, bx1 = bx0 + 68;
        int by0 = POPUP_Y1 - 26, by1 = POPUP_Y1 - 4;
        if (x >= bx0 && x <= bx1 && y >= by0 && y <= by1) { g_popup_on = false; return false; }
        if (x >= POPUP_X0 && x <= POPUP_X1 && y >= POPUP_Y0 && y <= POPUP_Y1) return false;
        g_popup_on = false;   /* 点弹窗周围 */
        return false;
    }
    /* 顶部进度条: 只有已走过的节点可点击返回 (未走到不可点) */
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
            dInvalidateFrom((diag_stage_t)best);   /* 跳回即失效该阶段及之后答案 */
        }
        return false;
    }
    /* 右侧故障元件 → 弹窗显示排除方案 */
    int dev = dHitRightDev(x, y);
    if (dev >= 0 && g_stage != D_ST_CAT) {
        g_popup_dev = dev;
        g_popup_on  = true;
        return false;
    }
    int hit = dHitOption(x, y);
    if (hit >= 0 && hit < dStageOptCount(g_stage)) {
        g_cur_opt = hit;
        dConfirmSelect();
    }
    return false;
}

bool diag_poll(menu_state_t *state) {
    /* V1.0.98: 右侧概率栏拖动滚动 (行级). 手指在栏内按住上下拖动,
     * 每移过一行高度换一行显示, 松手结束. 弹窗打开时不滚动. */
    if (g_popup_on) { s_rbar_drag = false; return false; }
    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    if (!down) { s_rbar_drag = false; return false; }
    if (tx < DIVIDER_X - 2) return false;   /* 不在右侧栏内 */
    if (!s_rbar_drag) {
        s_rbar_drag = true;
        s_rbar_drag_y0 = ty;
        s_rbar_drag_off0 = s_rbar_off;
        return false;
    }
    int dy = s_rbar_drag_y0 - ty;           /* 上划 dy>0 → 看更靠后的行 */
    int off = s_rbar_drag_off0 + dy / RBAR_ROW_H;
    if (off < 0) off = 0;
    if (off > RBAR_MAX_OFF) off = RBAR_MAX_OFF;
    if (off != s_rbar_off) {
        s_rbar_off = off;
        state->needs_redraw = true;
        return true;
    }
    return false;
}

void diag_action(menu_state_t *state, menu_action_t action) {
    (void)state;
    /* 结果页: 上下键切换查看的候选名次 (前3), 其余按键同返回 */
    if (g_stage == D_ST_RESULT) {
        switch (action) {
        case MENU_ACTION_UP:
        case MENU_ACTION_LEFT:
            if (g_result_k > 0) g_result_k--;
            break;
        case MENU_ACTION_DOWN:
        case MENU_ACTION_RIGHT:
            if (g_result_k < RSHOW - 1) g_result_k++;
            break;
        case MENU_ACTION_BACK:
            dStepBack();
            break;
        default:
            break;
        }
        return;
    }
    int n = dStageOptCount(g_stage);
    switch (action) {
    case MENU_ACTION_UP:
    case MENU_ACTION_LEFT:
        if (g_cur_opt > 0) g_cur_opt--;
        break;
    case MENU_ACTION_DOWN:
    case MENU_ACTION_RIGHT:
        if (g_cur_opt < n - 1) g_cur_opt++;
        break;
    case MENU_ACTION_CONFIRM:
        dConfirmSelect();
        break;
    case MENU_ACTION_BACK:
        dStepBack();
        break;
    default:
        break;
    }
}

void diag_reset(menu_state_t *state) {
    (void)state;
    DReset();
}