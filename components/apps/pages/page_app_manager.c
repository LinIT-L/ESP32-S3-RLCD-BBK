/**
 * page_app_manager.c — 应用管家 页面模块 (应用管理 + 应用商店 合并, 3.3 网格风格).
 *
 * 左侧分类栏: 引擎 / 运维 / 应用 / 商店 (4 类, 仅图标).
 * 右侧内容:
 *   - 引擎/应用/运维: 已安装应用图标网格; 应用顶部有子分类条(全部/游戏/软件/系统/运维/学习)筛选;
 *     选中应用 → 打开 / 长按 → 添加到主菜单 / 移出主菜单
 *   - 商店: 固定列表视图, 顶部子分类条, 点击安装 / 已装运行 / 长按卸载
 * 视图固定不可切换: 商店=列表, 其他分类=图标网格.
 * 主菜单显隐由 os_app 注册表 + NVS 持久化 (os_app_set_mainmenu / os_app_set_hidden).
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "am_internal.h"   /* 应用中心内部模块接口 (am_store / am_lua) */
#include "os_theme.h"
#include "ui_common.h"
#include "engine_manager.h"   /* 返回应用管家统一卸载引擎 (应用管家=主菜单级边界) */
#include "input.h"
#include "vibrator.h"         /* V1.5.x: 点击应用震动 (ctx_block 屏蔽 input 层, 打开/选中时补) */
#include "touch_panel.h"
#include "wifi_manager.h"   /* 商店联网门禁: is_connected */
#include "font_zh.h"
#include "font_zh16.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <dirent.h>
#include <strings.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define TAG "APPM"

/* ==== 布局 (复刻 3.3 应用管理页) ==== */
#define AM_CATBAR_W    75     /* 左侧分类栏宽度 (V1.5.x: 4px左缘+4px边距+64图标+2px空隙+1px分割线) */
#define AM_CAT_Y0      24     /* 分类栏起始 y = 状态栏边界 */
#define AM_CAT_X0      4      /* V1.5.x: 左缘留白, 让左角外凸圆角有显示空间 (需 ≥ 选中框 R) */
#define AM_ICON_SIZE   64     /* 网格图标尺寸 */
#define AM_ICON_GAP    4      /* V1.5.x: 图标周围/间隔 4px 空间 */
#define AM_ICON_X      (AM_CAT_X0 + AM_ICON_GAP + AM_ICON_SIZE / 2)  /* 图标中心 x = 40 (左缘 4px 留白) */
#define AM_SEL_GAP     2      /* V1.5.x: 选中指示线距图标 2px */
#define AM_SEL_W       2      /* V1.5.x: 选中指示线宽 2px */
#define AM_SEL_R       3      /* V1.5.x: 选中指示圆角半径 R3 (左侧四分类栏) */
/* 子分类当前项的水管框圆角半径。
 * ⚠ 必须比左栏的 R3 大: 水管壁厚 2px, 所以"**框内**"的圆角半径只有 R-2。
 *   R=3 时内圈弧度只剩 1px —— 填起来跟正方形没区别, 而 linit 明确要求
 *   "填充也要保持左上角/右上角的弧度"。R=6 → 内圈 4px, 肉眼清楚。 */
#define AM_SUB_R       6
#define AM_GRID_COLS   4      /* 网格列数 */
#define AM_GRID_ROWS   3      /* 网格可见行数 */
#define AM_SUB_H       26     /* 顶部子分类条高 */
#define AM_VIEW_ICON   24     /* 视图切换小图标 24×24 */

/* 左侧 4 分类 (引擎/游戏/应用/商店) */

/* 商店/应用 子分类 (横排条) */
#define AM_SUB_ALL     0
#define AM_SUB_GAME    1   /* 游戏 */
#define AM_SUB_PROG    2   /* 软件 (原 程序) */
#define AM_SUB_SYS     3   /* 系统 (新增) */
#define AM_SUB_OPS     4   /* 运维 */
#define AM_SUB_STUDY   5   /* 学习 */
#define AM_SUB_COUNT   6
static const char *const s_sub_names[AM_SUB_COUNT] = {
    "\xe5\x85\xa8\xe9\x83\xa8",  /* 全部 */
    "\xe6\xb8\xb8\xe6\x88\x8f",  /* 游戏 */
    "\xe8\xbd\xaf\xe4\xbb\xb6",  /* 软件 */
    "\xe7\xb3\xbb\xe7\xbb\x9f",  /* 系统 */
    "\xe8\xbf\x90\xe7\xbb\xb4",  /* 运维 */
    "\xe5\xad\xa6\xe4\xb9\xa0",  /* 学习 */
};

/* ==== 商店联网门禁 (2026-10-02 linit) ====
 * 需求: "每次进入应用管家后, 第一次打开商店要弹窗提示联网一次; 退出应用管家再进才会重新提示"。
 * 做法: 未联网时弹确认框 → 确定走系统"连接WiFi"(扫描+密码键盘); 取消仍可浏览本地包。
 * "只提示一次"的边界 = **本次驻留管家**: 只有真退出管家才复位, 所以
 *   "商店 → 点开某个应用 → 返回商店" 不会再提示一遍 (那份 on_exit/on_enter 是子页往返)。
 * ⚠ 不要自己 os_hw_request(OS_HW_WIFI): settings_wifi_connect_open() 内部按需 request
 *   并记账, 用户取消时才会 release。 */
static bool s_store_wifi_asked   = false;
static bool s_store_wifi_connect = false;  /* 下一帧开连网流程 */
static int  s_am_leave_child     = 0;      /* >0 = 本次离开管家是进了自己推的子页 */
/* ⚠ 连网弹窗**不能在这个回调里开**: 回调返回后 os_dialog 还要收掉当前确认框,
 *   同期再 push 会被顶掉 (实测: 点了"确定"界面停在确认框、扫描在后台空跑)。
 *   与 Lua 应用门禁 (am_lua.c wl_gate_tick) 同一处理: 挂个标志, 下一帧再开。 */
static void am_store_wifi_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ctx; (void)ud;
    if (result != 0) return;                       /* 取消: 本地包照常可看 */
    s_store_wifi_connect = true;
}
static void am_store_wifi_gate(ui_ctx_t *ctx) {
    ESP_LOGI(TAG, "商店门禁: asked=%d conn=%d", (int)s_store_wifi_asked, (int)wifi_manager_is_connected());
    if (s_store_wifi_asked) return;
    s_store_wifi_asked = true;
    if (wifi_manager_is_connected()) return;
    ESP_LOGI(TAG, "商店门禁 -> 弹连接确认框");
    os_dialog_confirm_ex(ctx,
        "\xe5\x95\x86\xe5\xba\x97\xe9\x9c\x80\xe8\xa6\x81\xe8\x81\x94\xe7\xbd\x91, \xe7\x8e\xb0\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5 Wi-Fi?", /* 商店需要联网, 现在连接 Wi-Fi? */
        0, -1, am_store_wifi_cb, NULL);
}

/* ==== 页面状态 ==== */
static int s_cat = AM_CAT_ENGINE;     /* 左栏分类 */
static int s_sub = AM_SUB_ALL;        /* 顶部子分类 */
static bool s_view_grid = true;       /* true=网格  false=单条列表 */
static int  s_focus = 0;              /* 右栏选中索引 */
static int  s_scroll = 0;             /* 上卷行数 */
static int  s_focus_bar = 0;          /* 方向键聚焦区: 0=内容网格, 1=顶部子分类横排, 2=左侧分类竖排 */
static int s_list_n = 0;             /* 当前显示条目数 */
static const os_app_t *s_list[64];    /* 已安装条目 (应用分类, 预留扩容) */
static bool s_list_vis[64];           /* 每项可见性缓存: 渲染用, 避免逐帧读 NVS(flash 慢)致卡顿 */

/* 触摸拖动滚动状态 (列表整行滚动) */
#define AM_DRAG_THRESH 12   /* 超过此位移判定为拖动 (不触发点击) */

/* ==== 网格相机 (仿主菜单: 跟手 + 平滑吸附) ====
 * s_gcam 以"行"为单位, 可小数; 拖动时跟手, 松手后平滑吸附到最近焦点行. */
static float s_gcam = 0.0f;            /* 当前相机 (行, 可小数) */
static float s_gcam_anim_from = 0.0f;
static float s_gcam_anim_to   = 0.0f;
static uint32_t s_gcam_anim_start = 0;
static uint32_t s_gcam_anim_dur   = 0;
static int   s_gcam_anim_sel = -1;     /* 动画锁定选中行 */
static int   s_gdrag_anchor_y = 0;     /* 拖动起始触摸 y */
static int   s_gdrag_last_y = -1;
static float s_gdrag_base = 0.0f;      /* 拖动起始相机 */
static bool  s_gdrag_active = false;

/* ==== 离屏整列渲染 (V1.6.x: "离屏预渲染整列 + 滚动只抄可视窗 + 硬裁剪出框")
 * 右栏内容整列在 PSRAM 预渲染成标准 1bpp 位图 (每行 bpr=(w+7)/8, MSB 在左, 1=黑, 0=白跳过);
 * 每帧只把可视窗 [content_top, 屏底) 从列缓冲抄到屏 (st7305_blit_1bit 自带屏幕硬裁剪);
 * 窗口起点按像素偏移 src_y = round(cam*row_h) → 半行/半字/半按钮自然可见, 绝不等待"完整行".
 * 内容/焦点/安装状态变化时才重渲染整列 (s_col_dirty). */
#define AM_COL_W      (UI_SCREEN_W - AM_CATBAR_W)   /* 内容区宽 = 325 */
#define AM_COL_BPR    ((AM_COL_W + 7) / 8)          /* 41 bytes/行 */
/* ---- 列表行几何 (2026-10-02 linit 定): 左边图标 + 名称/说明 整体右移 + 右侧 安装/卸载 按钮 ----
 * ⚠ 行高只能从这里改: 下面所有滚动/命中/预渲染都用 AM_ROW_H, 不再有裸 46。
 *   46px 时会"图标 + 两行 16px 文字"塞不下 (说明行紧贴行底) —— linit 实机反馈。 */
#define AM_ROW_H        54                          /* 列表行高 */
#define AM_ROW_ICON_SZ  36                          /* 行内图标边长 (缩放到行高内) */
#define AM_ROW_ICON_X   (6 + AM_ROW_ICON_SZ / 2)    /* 图标中心 x = 24 (左缘留 6px) */
#define AM_ROW_TX       50                          /* 名称/说明左边界 (图标右缘 + 9) */
#define AM_ROW_BTN_TW   32                          /* 按钮两汉字宽 (16px×2) */
#define AM_ROW_BTN_X0   (AM_COL_W - 10 - AM_ROW_BTN_TW)   /* 按钮文字左边 = 283 */
#define AM_COL_H_MAX  (64 * AM_ROW_H)               /* 列表最多 64 行的整列缓冲高 */
static uint8_t *s_col = NULL;                       /* PSRAM 整列缓冲 */
static int      s_col_h = 0;                        /* 当前列高 (px) */
static bool     s_col_dirty = false;                /* 需要重渲染整列 */

/* 长按 (首次按下即算, 无需放手指): 按住图标 3s 弹"添加/删除桌面快捷方式"确认 */
#define AM_LP_MS       3000
#define AM_LP_DRAG_MAX 12      /* 长按期间位移超过此值则取消长按意图 */
static int      s_lp_idx = -1;        /* 长按目标索引 (-1=无) */
static uint32_t s_lp_t0  = 0;         /* 按下时刻 */
static bool     s_lp_fired = false;   /* 已触发, 防重复 */
static bool     s_lp_suppress = false;/* 抑制长按松手后的那次 tap */

/* 商店卸载 (仅列表视图): 选中的已安装包 长按硬件确认键 3s 弹卸载确认; 触屏长按同样触发 */
static bool     s_uninstall_armed  = false;
static uint32_t s_uninstall_t0     = 0;
static bool     s_uninstall_fired  = false;

/* 前向声明 (定义在下方, 交叉引用) */
static void am_clamp_scroll(void);
static void am_rebuild(void);          /* 常规重建: 相机对齐到焦点行 */
void am_rebuild_keep(void);   /* 对外接口 */     /* 返回重建: 保留原滚动位置 */
static bool am_has_sub(void);          /* 是否有顶部子分类条 (应用/商店) */

/* 子分类是否匹配应用 */
static bool app_match_sub(const os_app_t *a) {
    if (!a || !a->cat) return (s_sub == AM_SUB_ALL);
    if (s_sub == AM_SUB_ALL) return true;
    const char *c = a->cat;
    if (s_sub == AM_SUB_GAME && strcmp(c, "\xe6\xb8\xb8\xe6\x88\x8f") == 0) return true;  /* 游戏 */
    if (s_sub == AM_SUB_PROG && (strcmp(c, "\xe7\xa8\x8b\xe5\xba\x8f") == 0 || strcmp(c, "\xe8\xbd\xaf\xe4\xbb\xb6") == 0)) return true; /* 程序/软件 */
    if (s_sub == AM_SUB_SYS  && (strcmp(c, "\xe7\xb3\xbb\xe7\xbb\x9f") == 0 || strcmp(c, "\xe5\xb7\xa5\xe5\x85\xb7") == 0)) return true;  /* 系统/工具 */
    if (s_sub == AM_SUB_OPS  && strcmp(c, "\xe5\xa3\x81\xe7\xba\xb8") == 0) return true;  /* 应用分类: 运维→壁纸 */
    if (s_sub == AM_SUB_STUDY&& strcmp(c, "\xe5\xad\xa6\xe4\xb9\xa0") == 0) return true;  /* 学习 */
    return false;
}
/* 左侧分类是否匹配应用 (引擎/游戏/应用) */
/* 引擎分类固定顺序 (用户指定): 步步高/文曲星/暴龙机/ArduBoy/GB/GBC/NES (+MD/SMS);
 * 末尾并入「独立游戏」(手柄 / 修改机) —— 2026-09-20 linit 定: 独立游戏归**引擎**分类,
 * 左侧第 4 个分类保留为「商店」并换用正牌商店图标(应用管家/商店.png)。 */
static const char *const s_engine_order[] = {
    "bbk", "lavax", "vpet", "arduboy", "gb", "gbc", "nes", "md", "sms",
    "gamepad", "modtool"
};
#define ENGINE_ORDER_COUNT (int)(sizeof(s_engine_order)/sizeof(s_engine_order[0]))

static bool app_match_cat(const os_app_t *a) {
    if (!a) return false;
    const char *id = a->id;
    const char *ops = "\xe8\xbf\x90\xe7\xbb\xb4";   /* 运维 */
    switch (s_cat) {
    case AM_CAT_ENGINE: {
        /* 引擎: 只认固定顺序表里的模拟器 */
        for (int i = 0; i < ENGINE_ORDER_COUNT; i++)
            if (strcmp(id, s_engine_order[i]) == 0) return true;
        return false;
    }
    case AM_CAT_OPS:
        /* 运维分类: cat=运维 的隐藏应用/工具 (不入应用分类重复) */
        if (strcmp(id, "app_mgr") == 0) return false;
        for (int i = 0; i < ENGINE_ORDER_COUNT; i++)
            if (strcmp(id, s_engine_order[i]) == 0) return false;
        if (!a->cat) return false;
        return strcmp(a->cat, ops) == 0;
    case AM_CAT_APP: {
        /* 应用: 排除引擎 + 排除运维(归运维分类) + 应用管家自身, 其余按顶部子分类条筛选
         * (游戏已并入此分类). */
        if (strcmp(id, "app_mgr") == 0) return false;
        if (a->cat && strcmp(a->cat, ops) == 0) return false;
        for (int i = 0; i < ENGINE_ORDER_COUNT; i++)
            if (strcmp(id, s_engine_order[i]) == 0) return false;
        return app_match_sub(a);
    }
    }
    return false;
}

static float am_gcam_clamp(float c);   /* 前向声明 (定义在下方) */

/* 内容区顶 (有顶部子分类条的分类下移 AM_SUB_H, 其余直接在状态栏下方) */
static int am_content_top(void) {
    return am_has_sub() ? (AM_CAT_Y0 + AM_SUB_H) : AM_CAT_Y0;
}
/* 当前视图可见行数 (网格=AM_GRID_ROWS 行; 列表=整屏可容纳的行数) */
static int am_vis_rows(void) {
    return s_view_grid ? AM_GRID_ROWS : ((UI_SCREEN_H - am_content_top()) / AM_ROW_H);
}
/* 焦点所在行 (网格=每行列数归组; 列表=一项一行) */
static int am_focus_row(void) {
    return s_view_grid ? (s_focus / AM_GRID_COLS) : s_focus;
}

/* ★ 2026-10-02 焦点可见化 (linit 反馈: 焦点移到左侧分类栏时"没有任何指示",
 *   而右侧内容首项一直反色 → 分不清焦点在哪一层)。
 *   根因: `s_focus_bar` 只驱动导航 (p_am_action); 渲染侧原先**只有顶部子分类**用了它,
 *   内容区 sel 恒按 `s_focus`、左栏框恒按 `s_cat` —— 两处标记都不代表"焦点"。
 *   现在渲染与导航同源: **只有焦点在内容区时才反色**; 焦点在别处时内容只留 1px
 *   下划线做"弱定位"(表示按 RIGHT/DOWN 会回到哪一项)。 */
static bool am_focus_on_content(void) { return s_focus_bar == 0; }
static bool am_weak_mark(int i) {
    return !input_scheme_touch() && !am_focus_on_content() && (i == s_focus);
}

/* 重建右侧列表.
 * keep_cam=true  (从应用返回): 保留当前滚动位置, 仅当焦点已滚出视野才把相机拉回焦点行.
 *   修复"触摸点开第二排的应用, 退出后第一排消失、第二排顶到第一排" —— 触摸换焦点时
 *   只改 s_focus 不改相机 (p_am_touch), 若这里无条件把相机对齐焦点行, 画面就会凭空跳一行.
 * keep_cam=false (切分类/子分类/视图): 相机对齐焦点行 (常规重置). */

/* 启动/打开: 商店=安装/打开; Lua 项=设启动路径后进 Lua 页; 否则打开应用页 */
static void am_launch(ui_ctx_t *ctx, const os_app_t *a) {
    if (!a) return;
    lua_probe_stage(1, "am_launch 入口");
    if (s_cat == AM_CAT_STORE) { am_store_toggle(ctx, a); return; }   /* 商店只装/卸, 不启动 */
    /* 标记"这次离开管家是进了子页" → 从子页返回时商店门禁不会又提示一遍。
     * (若子页被门禁拦下没真进, p_am_poll 会把标记收回。) */
    s_am_leave_child++;
    /* Lua 项统一走 am_lua_open_if_entry: 它除了设 boot 路径, 还带 `need=wifi` 门禁
     * (未联网时先弹窗连网, 连上再进) —— 原先这里自己 push, 门禁就被绕过了。 */
    if (am_lua_open_if_entry(ctx, a)) return;
    os_push(ctx, a->page);
}
/* 商店列表行触摸: 命中右侧"安装/卸载"按钮 → 走激活(确认安装/卸载); 命中名称区 →
 * 已装运行 / 未装不弹窗 (安装必须点右边按钮, 避免点名称误弹确认). */
static void am_launch_list(ui_ctx_t *ctx, const os_app_t *a, int x, int ty, int y_row) {
    if (!a || s_cat != AM_CAT_STORE) { am_launch(ctx, a); return; }
    /* 商店行: 点按钮 或 点名称区 **都一样** —— 已装→弹卸载确认 / 未装→弹安装确认。
     * **商店里永不启动应用**(要运行去「应用」分类)。
     * (原先还算了 on_btn 命中框, 但两个分支行为完全相同 → 已删, 免得后人以为有区别) */
    (void)x; (void)ty; (void)y_row;
    am_store_toggle(ctx, a);
}


/* ==== am_internal.h 接口实现 (供 am_store.c / am_lua.c 使用) ==== */
int  am_cat(void) { return s_cat; }
int  am_sub(void) { return s_sub; }
bool am_push_app(const os_app_t *a) {
    if (!a || s_list_n >= 64) return false;
    s_list[s_list_n++] = a;
    return true;
}

/* 当前子分类是否要这个 cat —— 对 Lua 文件项开放, 与 C 注册项**同一套判据**。
 * (原先 Lua 项不走 app_match_sub, 导致它们在"游戏/壁纸/学习"等每个子分类里都出现。) */
bool am_sub_want_cat(const char *cat) {
    os_app_t t;
    memset(&t, 0, sizeof(t));
    t.cat = cat;
    return app_match_sub(&t);
}

static void am_rebuild_ex(bool keep_cam) {
    /* 视图固定不可切换: 商店=列表, 其他分类=图标网格 */
    s_view_grid = (s_cat != AM_CAT_STORE);
    /* 分类 Lua 后台扫描 (不再同步阻塞首帧): 本次先只组 C 应用; 若该分类数据还没就绪,
     * 后台扫完由 poll 迁移到 s_lua 并重建填充. s_lua_ok_cat==s_cat 表示数据可用. */
    if (s_cat != AM_CAT_STORE) {
        am_lua_prefetch(s_cat);   /* 已扫过 → 标记就绪; 未扫 → 启动后台扫描 */
    }
    s_list_n = 0;
    if (s_cat == AM_CAT_STORE) {
        /* 商店: 本地应用包 (后台扫描 /store 根 与 /sdcard/store_pkg, 不联网).
         * 首次未扫完 → 空列表显示"正在扫描…", 扫描完成后由 poll 重建列表. */
        am_store_prefetch();
        if (am_store_ready())
            for (int k = 0; k < am_store_count() && s_list_n < 64; k++) {
                if (s_sub == AM_SUB_ALL || s_sub == AM_SUB_PROG)
                    { const os_app_t *sa = am_store_app(k); if (sa) s_list[s_list_n++] = sa; }
            }
    } else if (s_cat == AM_CAT_ENGINE) {
        /* 引擎: 按用户指定顺序插入 (从注册表取对应应用) */
        for (int o = 0; o < ENGINE_ORDER_COUNT && s_list_n < 64; o++) {
            int total = os_app_count();
            for (int i = 0; i < total; i++) {
                const os_app_t *a = os_app_get(i);
                if (a && strcmp(a->id, s_engine_order[o]) == 0) {
                    s_list[s_list_n++] = a;
                    break;
                }
            }
        }
        if (am_lua_ready(s_cat)) am_lua_append(false);   /* 并入 Lua 引擎文件 (后台扫完才有) */
    } else if (s_cat == AM_CAT_OPS) {
        /* 运维: cat=运维 应用 + 运维类 Lua 文件 */
        int total = os_app_count();
        for (int i = 0; i < total && s_list_n < 64; i++) {
            const os_app_t *a = os_app_get(i);
            if (app_match_cat(a)) s_list[s_list_n++] = a;
        }
        if (am_lua_ready(s_cat)) am_lua_append(true);    /* 只并入运维类 Lua */
    } else {
        /* 应用: cat 匹配 (游戏/软件/系统/学习…) + 非运维类 Lua 文件 */
        int total = os_app_count();
        for (int i = 0; i < total && s_list_n < 64; i++) {
            const os_app_t *a = os_app_get(i);
            if (app_match_cat(a)) s_list[s_list_n++] = a;
        }
        if (am_lua_ready(s_cat)) am_lua_append(false);   /* 并入 Lua 应用文件 (排除运维类) */
    }
    /* 缓存可见性: 渲染循环不再逐帧调 os_app_visible(读 NVS flash 极慢, 是管家卡顿根因) */
    for (int i = 0; i < s_list_n; i++) {
        const os_app_t *v = s_list[i];
        s_list_vis[i] = v ? os_app_visible(v) : false;
    }
    if (s_focus >= s_list_n) s_focus = 0;
    if (s_focus < 0) s_focus = 0;
    if (keep_cam) {
        /* 保留原画面: 焦点在视野内就不动相机, 只有焦点被滚出视野才拉回去 */
        int frow  = am_focus_row();
        int fvis  = (int)(s_gcam + 0.5f);
        int vlast = fvis + am_vis_rows() - 1;
        if (frow < fvis || frow > vlast) s_gcam = (float)frow;
        s_gcam   = am_gcam_clamp(s_gcam);
        s_scroll = (int)s_gcam;
    } else {
        /* 网格: 相机吸附到焦点行 */
        s_gcam   = (float)(s_focus / AM_GRID_COLS);
        s_scroll = s_focus / AM_GRID_COLS;
    }
    s_gcam_anim_start = 0;
    am_clamp_scroll();
    s_col_dirty = true;   /* 列表内容变了, 整列需重渲染 */
}
static void am_rebuild(void)      { am_rebuild_ex(false); }
void am_rebuild_keep(void) { am_rebuild_ex(true);  }   /* 对外: am_store.c 安装/卸载后重建 */
static int am_max_scroll(void) {
    if (!s_view_grid) {
        int content_top = am_content_top();
        int mv = (UI_SCREEN_H - content_top) / AM_ROW_H;
        int m = s_list_n - mv;
        return m < 0 ? 0 : m;
    }
    int rows = (s_list_n + AM_GRID_COLS - 1) / AM_GRID_COLS;
    int vis  = AM_GRID_ROWS;
    int m = rows - vis;
    return m < 0 ? 0 : m;
}
static void am_clamp_scroll(void) {
    if (s_list_n <= 0) { s_scroll = 0; return; }
    int row = s_focus / AM_GRID_COLS;
    if (row < s_scroll) s_scroll = row;
    else if (row >= s_scroll + AM_GRID_ROWS)
        s_scroll = row - (AM_GRID_ROWS - 1);
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > am_max_scroll()) s_scroll = am_max_scroll();
}

/* ==== 网格相机 (仿主菜单) ==== */
static uint32_t am_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 当前相机行的边界: 目标行取与 from 最近的同余 (0..max_row) */
static float am_gcam_clamp(float c) {
    int max_row = am_max_scroll();
    if (c < 0) c = 0;
    if (c > max_row) c = (float)max_row;
    return c;
}
/* 网格吸附动画: 只移动相机 (不回写焦点, 焦点由方向键直接推进).
 * 关键修复: 原来是动画把焦点钳到 am_max_scroll() 行, 导致方向键到不了最后一排
 * (最多只剩 (max_scroll+1)*列 个); 现在相机最多滚到视图容纳处, 焦点可自由推到任意项. */
static void am_gcam_animate(int to_row, uint32_t dur_ms) {
    int total_rows = am_max_scroll();
    if (to_row > total_rows) to_row = total_rows;
    if (to_row < 0) to_row = 0;
    float from_c = am_gcam_clamp(s_gcam);
    float to_c = (float)to_row;
    if (from_c == to_c) {
        s_gcam = to_c;
        s_gcam_anim_start = 0;
        return;
    }
    s_gcam_anim_from   = from_c;
    s_gcam_anim_to     = to_c;
    s_gcam_anim_start  = am_now_ms();
    s_gcam_anim_dur    = dur_ms ? dur_ms : 1;
    s_gcam_anim_sel    = to_row;
}
/* 吸附完成: 相机锁定到最近焦点行 (只锁相机, 不动 s_focus) */
static void am_gcam_settle(void) {
    s_gcam = am_gcam_clamp(s_gcam);
    int row = (int)(s_gcam + 0.5f);
    s_gcam = (float)row;
    s_gcam_anim_start = 0;
    (void)row;
}
/* 每帧更新动画 (poll 调用), 返回是否还在动画中 */
static bool am_gcam_tick(void) {
    if (s_gcam_anim_start == 0) return false;
    uint32_t d = am_now_ms() - s_gcam_anim_start;
    float tt = d < s_gcam_anim_dur ? (float)d / (float)s_gcam_anim_dur : 1.0f;
    /* 平滑缓动 (抛物线减速), 无弹簧过冲 */
    float ease = 1.0f - (float)pow(1.0f - tt, 2.0f);
    s_gcam = s_gcam_anim_from + (s_gcam_anim_to - s_gcam_anim_from) * ease;
    if (tt >= 1.0f) {
        am_gcam_settle();
    }
    return s_gcam_anim_start != 0;
}

/* ==== 16px 文本 (名称/标签) ==== */
static int am_label_width(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) { w += ZH16_FONT_W; p += 3; }
        else { w += 8; p++; }
    }
    return w;
}
static void am_draw_label(st7305_handle_t *lcd, int cx, int y, const char *s, bool inverted) {
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    int aofs = (ZH16_FONT_H - 16) / 2;
    int x = cx - am_label_width(s) / 2;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) {
            int idx = font_zh16_find_utf8((const char *)p);
            const uint8_t *b = (idx >= 0) ? font_zh16_get_bitmap_by_index(idx) : NULL;
            if (b)
                for (int r = 0; r < ZH16_FONT_H; r++)
                    for (int c = 0; c < ZH16_FONT_W; c++)
                        if (b[r * 2 + (c >> 3)] & (0x80 >> (c & 7)))
                            st7305_draw_pixel(lcd, x + c, y + r, fg);
            x += ZH16_FONT_W; p += 3;
        } else {
            if (*p >= 0x20 && *p <= 0x7e) {
                const uint8_t *b = FONT8X12[*p - 0x20];
                for (int cy = 0; cy < 16; cy++)
                    for (int cc = 0; cc < 8; cc++)
                        if (b[cy] & (0x80u >> cc)) st7305_draw_pixel(lcd, x + cc, y + aofs + cy, fg);
            }
            x += 8; p++;
        }
    }
}

/* 16px 文本左对齐绘制 (不居中, x 为起点; 用于列表视图说明等) */
static void am_draw_text_l(st7305_handle_t *lcd, int x, int y, const char *s, bool inv) {
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    int aofs = (ZH16_FONT_H - 16) / 2;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) {
            int idx = font_zh16_find_utf8((const char *)p);
            const uint8_t *b = (idx >= 0) ? font_zh16_get_bitmap_by_index(idx) : NULL;
            if (b)
                for (int r = 0; r < ZH16_FONT_H; r++)
                    for (int c = 0; c < ZH16_FONT_W; c++)
                        if (b[r * 2 + (c >> 3)] & (0x80 >> (c & 7)))
                            st7305_draw_pixel(lcd, x + c, y + r, fg);
            x += ZH16_FONT_W; p += 3;
        } else {
            if (*p >= 0x20 && *p <= 0x7e) {
                const uint8_t *b = FONT8X12[*p - 0x20];
                for (int cy = 0; cy < 16; cy++)
                    for (int cc = 0; cc < 8; cc++)
                        if (b[cy] & (0x80u >> cc)) st7305_draw_pixel(lcd, x + cc, y + aofs + cy, fg);
            }
            x += 8; p++;
        }
    }
}

/* 实心圆 (用于"已添加到主菜单"角标, 参考 3.3 app_fill_circle) */
static void am_fill_circle(st7305_handle_t *lcd, int cx, int cy, int r, st7305_color_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                st7305_draw_pixel(lcd, cx + dx, cy + dy, c);
}
/* 已添加到桌面(主菜单)的应用: 图标右上角圆形标识.
 * 三层同心圆 (白黑白) 保证黑色背景/白色背景都能看清. */
static void am_draw_mainmenu_dot(st7305_handle_t *lcd, int cx, int cy, int size) {
    int ccx = cx + size / 2 - 4;      /* 右上角, 往里收几像素 */
    int ccy = cy - size / 2 + 4;
    am_fill_circle(lcd, ccx, ccy, 5, ST7305_COLOR_WHITE);  /* 外圈白 (黑色背景可见) */
    am_fill_circle(lcd, ccx, ccy, 3, ST7305_COLOR_BLACK);  /* 中圈黑 (白色背景可见) */
    am_fill_circle(lcd, ccx, ccy, 1, ST7305_COLOR_WHITE);  /* 内圈白 */
}

/* 应用图标 (网格视图) */
static void am_draw_app_icon(ui_ctx_t *ctx, st7305_handle_t *lcd, const os_app_t *a, int cx, int cy, int size, bool sel) {
    const os_theme_t *th = os_theme_get();
    if (a && th->draw_icon) th->draw_icon(ctx, lcd, a->icon_idx, cx, cy, size, sel);
    else draw_main_icon_stretched(lcd, cx, cy, size, size, a->icon_idx);
}

/* 子分类条绘制 (y 顶部, 先填白底盖住下方内容残留, 恒固定显示) */
/* 前置声明: 子分类条的水管弯头用它, 实现在下方 (与左栏选中框同一套) */
static void am_elbow(st7305_handle_t *lcd, int cx, int cy, int r, int kind, int w, st7305_color_t color);

/* 水管框"以内"的填充 —— **只把上两个角做圆角, 下两个角保持方角**。
 *
 * ⛔ 别用 fill_rect 代替: 那是**正方形**, 会把上两角的弧度盖平
 *    (2026-10-03 linit: "选中过后不要用正方形, 要填充它那个水管线以内的")。
 * ★ 下两角**故意不圆** (2026-10-03 linit 追加): 下边缘是与下方内容相接的那条边,
 *    圆角会让这块"浮起来"; 上两角保持弧度与框呼应。
 *
 * 半径取 R-2 = 框内圈的半径, 圆心 (x0+r, y0+r); 该行左端 x = x0 + r - √(r²-dy²)。
 * 与框内缘同心同半径 ⇒ 填充正好贴住框, 角上不会留白缝。 */
static void am_fill_top_round(st7305_handle_t *lcd, int x0, int y0, int x1, int y1, int r, st7305_color_t c) {
    if (x1 < x0 || y1 < y0) return;
    if (r < 0) r = 0;
    if (r * 2 > (y1 - y0)) r = (y1 - y0) / 2;
    if (r * 2 > (x1 - x0)) r = (x1 - x0) / 2;
    for (int y = y0; y <= y1; y++) {
        int inset = 0;
        if (y < y0 + r && r > 0) {                   /* 只有上半段收角, 下半段不收 */
            int dy = (y0 + r) - y;
            float dx = sqrtf((float)(r * r - dy * dy));
            inset = r - (int)(dx + 0.5f);
            if (inset < 0) inset = 0;
        }
        draw_hline(lcd, x0 + inset, x1 - inset, y, c);
    }
}

static void am_draw_subbar(ui_ctx_t *ctx, st7305_handle_t *lcd, int x0, int y0, int x1) {
    (void)ctx;
    /* 先整条白底, 覆盖拖动/滚动时滑到顶部的图标残留, 保证子分类条固定不被遮挡 */
    fill_rect(lcd, x0, y0, x1, y0 + AM_SUB_H - 1, ST7305_COLOR_WHITE);
    int w = x1 - x0;
    int slot = w / AM_SUB_COUNT;
    int ybot = y0 + AM_SUB_H - 1;
    for (int i = 0; i < AM_SUB_COUNT; i++) {
        int sx = x0 + i * slot;
        int cx = sx + slot / 2;
        bool cur = (i == s_sub);
        const char *nm = (s_cat == AM_CAT_APP && i == AM_SUB_OPS)
                         ? "\xe5\xa3\x81\xe7\xba\xb8" : s_sub_names[i];   /* 应用分类: 运维→壁纸 */

        /* ★ 2026-10-03 linit 定案 —— 当前项的框**永远不变**, 变的只是"框内填不填":
         *   ① 焦点在顶栏还是在内容区, 画的都是**同一个闭合圆角水管框**
         *      (左竖 + 上横 + 右竖 + **下横**, 四个角都有弯头), 不再换成一整块黑方块。
         *      旧版下方是**开口**的, 底部接不上 —— 就是 linit 说的"下面的都没有连接在一起"。
         *   ② 选中时**不要用正方形填充**: 改成**填在"水管线以内"**, 且**上两角保持弧度**;
         *      **下两角要方角** —— 下边缘是与下方内容相接的那条边, 圆角会让它"浮起来"
         *      (用 am_fill_top_round; 旧的 fill_rect 会把上两角圆角盖平)。
         *   ③ 未选中项的下方分隔线**首尾相接连成一条** (旧版两端各留 4px 缺口, 是断的)。 */
        if (!cur) {
            draw_hline(lcd, sx, sx + slot - 1, ybot, ST7305_COLOR_BLACK);
            am_draw_label(lcd, cx, y0 + 5, nm, false);
            continue;
        }

        int R    = AM_SUB_R;
        int ex0  = sx + 1, ex1 = sx + slot - 2;        /* 水管左右外缘 */
        int ytop = y0;
        bool sel = (s_focus_bar == 1);                 /* 焦点在顶栏 = 选中态 */
        /* ★ 选中态的底边**上提 2px** (2026-10-03 linit: "选中的分类底部不要有黑线"):
         *   黑块底边若和下方各格的分隔线齐平, 两者会**连成一条黑线**, 看着就是"底部有黑线"。
         *   上提 2px 留出白缝后, 这一格才像"坐在栏上的一个按钮"而不是跟底线糊成一体。 */
        /* ★ 底边**恒上提 2px** (linit: "选中的那个有弧度的底部不要有黑线") ——
         *   若与下方分隔线齐平, 两者会连成一条黑线。不再分聚焦与否。 */
        int yend = ybot - 2;
        /* ★ 线宽随状态变 (linit 2026-10-03):
         *   未选中 = 2px 细框"定位用"; **选中 = 1px** —— 里面已经填满了黑, 2px 边框会显得又粗又笨。 */
        /* ★ 线宽**恒为 1px** (linit 2026-10-03: "选中的左上右以及弧度黑线只要一个像素宽")。
         *   旧写法 `sel ? 1 : 2` 是错的: 它让"没聚焦时"又变回 2px ⇒ 用户看到的一直是粗线。 */
        int  w   = 1;

        /* 顺序: 填充 → 描框 → 写字。反了的话框会被填充盖掉。 */
        if (sel) {
            /* 左/上/右各内缩 w 让开边框; **底边不内缩** —— 黑块要一直铺到 ybot,
             *   和下方各格的分隔线齐平, 看起来才是"这一格坐在栏上"。 */
            am_fill_top_round(lcd, ex0 + w, ytop + w, ex1 - w, yend,
                              R - w, ST7305_COLOR_BLACK);
        }
        /* 竖线一插到底, 左下/右下**不打弯头** ⇒ 两下角是方角。 */
        for (int k = 0; k < w; k++) {
            draw_vline(lcd, ex0 + k, ytop + R, yend, ST7305_COLOR_BLACK);   /* 左竖 */
            draw_vline(lcd, ex1 - k, ytop + R, yend, ST7305_COLOR_BLACK);   /* 右竖 */
            draw_hline(lcd, ex0 + R, ex1 - R, ytop + k, ST7305_COLOR_BLACK); /* 上横 */
        }
        /* ★ 下横**只有未选中才画**: 选中时下方开口 (linit: "选中的分类底部不要有黑线") ——
         *   黑填本身已经铺到 ybot, 再叠一条线只会让底边发胖。 */
        /* 下横: **一律不画** —— 当前项的底部不留黑线。 */
        if (0) {
            draw_hline(lcd, ex0, ex1, ybot,     ST7305_COLOR_BLACK);
            draw_hline(lcd, ex0, ex1, ybot - 1, ST7305_COLOR_BLACK);
        }
        am_elbow(lcd, ex0, ytop, R, 1, w, ST7305_COLOR_BLACK);   /* 左上 外凸向左上 */
        am_elbow(lcd, ex1, ytop, R, 3, w, ST7305_COLOR_BLACK);   /* 右上 外凸向右上 */
        am_draw_label(lcd, cx, y0 + 5, nm, sel);
    }
}

/* ==== 选中框样式 (V1.6.x) ====
 * - 选中态 = 一根 2px 水管线 (直线 + 90°外凸圆弧弯头, 像水管拐弯), 不使用曲线弧/圆端.
 * - 顶/中/底: 左竖边 + 相应横边, 弯头处直边让位 R.
 * - 常驻分割线在选中行让位, 其余分类右侧保留竖条分隔. */

/* 90°外凸圆弧弯头 (像水管拐弯): 拐点 (cx,cy), 半径 r, 带宽 2px, 圆弧向外凸出.
 * kind=1: 外凸向左上 (左上弯: 竖线在左 + 上横线在上), 圆心 (cx+r, cy+r).
 * kind=2: 外凸向左下 (左下弯: 竖线在左 + 下横线在下), 圆心 (cx+r, cy-r).
 * kind=3: 外凸向右上 (右上弯: 竖线在右 + 上横线在上), 圆心 (cx-r, cy+r).
 * kind=4: 外凸向右下 (右下弯: 竖线在右 + 下横线在下), 圆心 (cx-r, cy-r).
 * 保留到圆心距离 d∈[r-w,r] 的环带像素 (各角朝外凸出的 90° 弧).
 * [param w]: 弧带**线宽**(像素)。w=2 = 原观感(左栏/未选中用); w=1 = 细线(选中态用, 2026-10-03 linit)。 */
static void am_elbow(st7305_handle_t *lcd, int cx, int cy, int r, int kind, int w, st7305_color_t color) {
    if (w < 1) w = 1;
    int ox, oy, x0, x1, y0, y1, sx, sy;
    if (kind == 1)      { ox = cx + r; oy = cy + r; x0 = cx;    x1 = cx + r; y0 = cy;    y1 = cy + r; sx = -1; sy = -1; }
    else if (kind == 2) { ox = cx + r; oy = cy - r; x0 = cx;    x1 = cx + r; y0 = cy - r; y1 = cy;    sx = -1; sy =  1; }
    else if (kind == 3) { ox = cx - r; oy = cy + r; x0 = cx - r; x1 = cx;    y0 = cy;    y1 = cy + r; sx =  1; sy = -1; }
    else                { ox = cx - r; oy = cy - r; x0 = cx - r; x1 = cx;    y0 = cy - r; y1 = cy;    sx =  1; sy =  1; }
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= UI_SCREEN_H) continue;
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || x >= UI_SCREEN_W) continue;
            float dx = (float)x - ox;
            float dy = (float)y - oy;
            if (!((sx * dx >= 0) && (sy * dy >= 0))) continue;
            float d = sqrtf(dx * dx + dy * dy);
            if (d >= (r - w) && d <= r) st7305_draw_pixel(lcd, x, y, color);
        }
    }
}

/* ==== 左侧分类栏布局 (V1.5.x: 4 图标等距排布, 顶部距状态栏 4px,
 * 底部留 6px 供选中框下缘+外凸圆角完整显示) ==== */
static int am_cat_cy(int r) {
    int top = AM_CAT_Y0 + AM_ICON_GAP;               /* 图标0顶: 状态栏 + 4px */
    int slot = AM_ICON_SIZE + AM_ICON_GAP - 1;       /* 67: 图标间隔 3px (底部圆角让位 1px) */
    return top + r * slot + AM_ICON_SIZE / 2;
}

/* ==== 离屏整列预渲染 + 可视窗抄送 (V1.6.x) ====
 * 所有列绘制写入 s_col (标准 1bpp: 每行 bpr 字节, MSB 左, 1=黑, 0=白跳过);
 * st7305_blit_1bit 抄送时只画黑像素 (白=保持屏上白底), 自带屏幕硬裁剪. */
static void am_col_px(int x, int y, bool black) {
    if (x < 0 || x >= AM_COL_W || y < 0 || y >= s_col_h) return;
    uint8_t *b = &s_col[y * AM_COL_BPR + (x >> 3)];
    if (black) *b |=  (uint8_t)(0x80 >> (x & 7));
    else       *b &= (uint8_t)~(0x80 >> (x & 7));
}
static void am_col_fill(int x0, int y0, int x1, int y1, bool black) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= AM_COL_W) x1 = AM_COL_W - 1;
    if (y1 >= s_col_h) y1 = s_col_h - 1;
    if (x0 > x1 || y0 > y1) return;
    /* V1.7.x (2026-09-28): 逐像素 → 按字节位运算.
     * 原实现每像素一次"边界判断 + 指针换算 + PSRAM 读改写", 列表视图每行
     * (325×44≈14K 次) × 行数 = 20 万次 PSRAM 读改写, 实测首帧近 0.6 s (打开应用中心"要等一会儿").
     * 现在每行只动 (x1-x0)/8 个字节: 两端用掩码, 中间整字节直接赋值. */
    const uint8_t first_mask = (uint8_t)(0xFFu >> (x0 & 7));
    const uint8_t last_mask  = (uint8_t)(0xFFu << (7 - (x1 & 7)));
    const int b0 = x0 >> 3, b1 = x1 >> 3;
    for (int y = y0; y <= y1; y++) {
        uint8_t *row = s_col + y * AM_COL_BPR;
        if (b0 == b1) {
            const uint8_t m = (uint8_t)(first_mask & last_mask);
            if (black) row[b0] |=  m;
            else       row[b0] &= (uint8_t)~m;
        } else if (black) {
            row[b0] |= first_mask;
            for (int b = b0 + 1; b < b1; b++) row[b] = 0xFF;
            row[b1] |= last_mask;
        } else {
            row[b0] &= (uint8_t)~first_mask;
            for (int b = b0 + 1; b < b1; b++) row[b] = 0x00;
            row[b1] &= (uint8_t)~last_mask;
        }
    }
}
static void am_col_hline(int x0, int x1, int y, bool black) {
    am_col_fill(x0, y, x1, y, black);   /* 单行横线 = 单行填充 (共用字节级快路径) */
}
static void am_col_vline(int x, int y0, int y1, bool black) {
    for (int y = y0; y <= y1; y++) am_col_px(x, y, black);
}
static void am_col_rect_outline(int x0, int y0, int x1, int y1, bool black) {
    am_col_hline(x0, x1, y0, black);
    am_col_hline(x0, x1, y1, black);
    am_col_vline(x0, y0, y1, black);
    am_col_vline(x1, y0, y1, black);
}
/* 列内 16px 文本: 居中 (cx) 与左对齐 (x) 两版, 与 am_draw_label/am_draw_text_l 同字形 */
static void am_col_label(int cx, int y, const char *s, bool inv) {
    int aofs = (ZH16_FONT_H - 16) / 2;
    int x = cx - am_label_width(s) / 2;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) {
            int idx = font_zh16_find_utf8((const char *)p);
            const uint8_t *b = (idx >= 0) ? font_zh16_get_bitmap_by_index(idx) : NULL;
            if (b)
                for (int r = 0; r < ZH16_FONT_H; r++)
                    for (int c = 0; c < ZH16_FONT_W; c++)
                        if (b[r * 2 + (c >> 3)] & (0x80 >> (c & 7)))
                            am_col_px(x + c, y + r, !inv);
            x += ZH16_FONT_W; p += 3;
        } else {
            if (*p >= 0x20 && *p <= 0x7e) {
                const uint8_t *b = FONT8X12[*p - 0x20];
                for (int cy = 0; cy < 16; cy++)
                    for (int cc = 0; cc < 8; cc++)
                        if (b[cy] & (0x80u >> cc)) am_col_px(x + cc, y + aofs + cy, !inv);
            }
            x += 8; p++;
        }
    }
}
static void am_col_text_l(int x, int y, const char *s, bool inv) {
    int aofs = (ZH16_FONT_H - 16) / 2;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) {
            int idx = font_zh16_find_utf8((const char *)p);
            const uint8_t *b = (idx >= 0) ? font_zh16_get_bitmap_by_index(idx) : NULL;
            if (b)
                for (int r = 0; r < ZH16_FONT_H; r++)
                    for (int c = 0; c < ZH16_FONT_W; c++)
                        if (b[r * 2 + (c >> 3)] & (0x80 >> (c & 7)))
                            am_col_px(x + c, y + r, !inv);
            x += ZH16_FONT_W; p += 3;
        } else {
            if (*p >= 0x20 && *p <= 0x7e) {
                const uint8_t *b = FONT8X12[*p - 0x20];
                for (int cy = 0; cy < 16; cy++)
                    for (int cc = 0; cc < 8; cc++)
                        if (b[cy] & (0x80u >> cc)) am_col_px(x + cc, y + aofs + cy, !inv);
            }
            x += 8; p++;
        }
    }
}
/* 列内图标: 从 ui_common 图标位图最近邻缩放 (同 ui_blit_scaled 算法, 写 s_col).
 * inv=true → 只画"已填黑的行"上的白像素 (商店选中行整行反白, 黑图标会看不见)。 */
static void am_col_icon_inv(int cx, int cy, int size, int icon_idx, bool inv) {
    int sw = 0, sh = 0;
    const uint8_t *src = ui_main_icon_bitmap(icon_idx, &sw, &sh);
    if (!src || sw <= 0 || sh <= 0) return;
    const int bpr = (sw + 7) >> 3;
    int x0 = cx - size / 2, y0 = cy - size / 2;
    for (int dy = 0; dy < size; dy++) {
        int sy = (dy * sh) / size;
        const uint8_t *row = src + sy * bpr;
        for (int dx = 0; dx < size; dx++) {
            int sx = (dx * sw) / size;
            if (row[sx >> 3] & (0x80 >> (sx & 7)))
                am_col_px(x0 + dx, y0 + dy, !inv);
        }
    }
}
static void am_col_icon(int cx, int cy, int size, int icon_idx) {
    am_col_icon_inv(cx, cy, size, icon_idx, false);
}
/* 按像素宽度截断 UTF-8 串 (超宽补 ".."); 16px 汉字 / 8px ASCII, 与 am_col_text_l 同口径。
 * 返回**静态缓冲** —— 只供渲染当场使用, 不要留存指针。 */
static const char *am_col_ellipsis(const char *s, int max_w) {
    static char buf[160];
    int i = 0, w = 0;
    const int lim = max_w - 16;                 /* 先给尾部 ".." 预留 16px */
    if (!s) s = "";
    while (s[i] && i < (int)sizeof(buf) - 4) {
        unsigned char c = (unsigned char)s[i];
        int wide = ((c & 0xe0) == 0xe0);
        int adv = wide ? 16 : 8;
        int len = wide ? 3 : 1;
        if (w + adv > lim) break;
        for (int k = 0; k < len; k++) buf[i + k] = s[i + k];
        i += len;
        w += adv;
    }
    if (s[i]) { buf[i++] = '.'; buf[i++] = '.'; }   /* 后面还有内容 → 省略号 */
    buf[i] = 0;
    return buf;
}
/* 列内实心圆 / 主菜单角标 (白底上: 黑中圈 + 白内圈) */
static void am_col_circle(int cx, int cy, int r, bool black) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                am_col_px(cx + dx, cy + dy, black);
}
static void am_col_dot(int cx, int cy, int size) {
    int ccx = cx + size / 2 - 4, ccy = cy - size / 2 + 4;
    am_col_circle(ccx, ccy, 3, true);
    am_col_circle(ccx, ccy, 1, false);
}
/* 整列预渲染: 网格/列表各自把全部行画进 s_col (列坐标系, 原点=内容区左上) */
static void am_col_render_grid(void) {
    int content_top = am_content_top();
    int cell_w = (UI_SCREEN_W - AM_CATBAR_W - 8) / AM_GRID_COLS;
    int step_y = (UI_SCREEN_H - content_top - 8) / AM_GRID_ROWS;
    if (step_y < AM_ICON_SIZE + 18) step_y = AM_ICON_SIZE + 18;
    int rows = (s_list_n + AM_GRID_COLS - 1) / AM_GRID_COLS;
    s_col_h = rows * step_y;
    int gx0 = 4;
    for (int row = 0; row < rows; row++) {
        int cy = row * step_y + 2 + AM_ICON_SIZE / 2;
        for (int col = 0; col < AM_GRID_COLS; col++) {
            int i = row * AM_GRID_COLS + col;
            if (i >= s_list_n) break;
            int cx = gx0 + col * cell_w + cell_w / 2;
            bool sel = !input_scheme_touch() && am_focus_on_content() && (i == s_focus);
            const os_app_t *a = s_list[i];
            am_col_icon(cx, cy, AM_ICON_SIZE, a ? a->icon_idx : 0);
            if (s_list_vis[i]) am_col_dot(cx, cy, AM_ICON_SIZE);
            const char *nm = a ? a->label : "?";
            int lw = am_label_width(nm);
            int ly = cy + AM_ICON_SIZE / 2 + 2;
            if (sel) am_col_fill(cx - lw / 2, ly, cx + lw / 2 - 1, ly + ZH16_FONT_H - 1, true);
            am_col_label(cx, ly, nm, sel);
            if (am_weak_mark(i))   /* 焦点不在内容区 → 1px 下划线弱定位 (不反色) */
                am_col_hline(cx - lw / 2, cx + lw / 2 - 1, ly + ZH16_FONT_H, true);
        }
    }
}
static void am_col_render_list(void) {
    const int row_h = AM_ROW_H;
    s_col_h = s_list_n * row_h;
    for (int i = 0; i < s_list_n; i++) {
        int y = i * row_h;
        bool sel = !input_scheme_touch() && am_focus_on_content() && (i == s_focus);
        if (sel) am_col_fill(2, y, AM_COL_W - 2, y + row_h - 2, true);
        else     am_col_fill(2, y, AM_COL_W - 2, y + row_h - 2, false);
        if (i > 0) am_col_fill(2, y, AM_COL_W - 2, y, true);   /* 行间分隔线 */
        if (am_weak_mark(i))   /* 弱定位: 行底 1px 下划线 */
            am_col_hline(3, AM_COL_W - 4, y + row_h - 4, true);
        const os_app_t *a = s_list[i];
        const char *nm = a ? a->label : "?";
        const char *id = a ? a->id : "";
        /* 行内统一: 左图标 + 右侧名称/说明 (图标在反白行上用反色画, 否则黑图标看不见) */
        am_col_icon_inv(AM_ROW_ICON_X, y + row_h / 2, AM_ROW_ICON_SZ,
                        a ? a->icon_idx : 0, sel);
        am_col_text_l(AM_ROW_TX, y + 6, nm, sel);
        if (s_cat == AM_CAT_STORE) {
            /* 第二行显示**包简介** (LAPP desc=), 不再显示文件路径/文件名 ——
             * 用户原话: "模拟器下方不要显示路径名称, 直接显示对应说明"。 */
            const char *ds = am_store_desc_of(a);
            if (ds && ds[0])
                am_col_text_l(AM_ROW_TX, y + 27,
                              am_col_ellipsis(ds, AM_ROW_BTN_X0 - 4 - AM_ROW_TX), sel);
            bool inst = am_store_installed_of(a);      /* 读缓存, 不逐帧 fopen */
            const char *act = inst ? "\xe5\x8d\xb8\xe8\xbd\xbd" : "\xe5\xae\x89\xe8\xa3\x85"; /* 卸载/安装 */
            const int bt = y + (AM_ROW_H - 16) / 2;    /* 按钮垂直居中 */
            am_col_text_l(AM_ROW_BTN_X0, bt, act, sel);
            /* 框必须**完整包住**两字: 文字实占 [bt, bt+15] → 框 [bt-2, bt+17] (上下各留 2px).
             * (旧代码框底到 y+26 < 文字底 y+30 → 两字下半截露在框外, 就是"框没框住"。) */
            am_col_rect_outline(AM_ROW_BTN_X0 - 3, bt - 2,
                                AM_ROW_BTN_X0 + AM_ROW_BTN_TW + 2, bt + 17, !sel);
        } else {
            am_col_text_l(AM_ROW_TX, y + 27, id, sel);
            bool mm = (i >= 0 && i < s_list_n) && s_list_vis[i];
            const char *st = mm ? "\xe5\x9c\xa8\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95" : "\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95"; /* 在主菜单/主菜单 */
            am_col_text_l(UI_SCREEN_W - 58 - AM_CATBAR_W, y + (AM_ROW_H - 16) / 2, st, sel);
        }
    }
}
static void am_col_render(void) {
    if (!s_col) return;
    memset(s_col, 0, AM_COL_H_MAX * AM_COL_BPR);   /* 整列清白 (0=白) */
    s_col_h = 0;
    if (s_list_n <= 0) return;
    if (s_view_grid) am_col_render_grid();
    else             am_col_render_list();
}
/* 滚动只抄可视窗: 窗口起点按像素偏移 src_y=round(cam*row_h), 只把 [content_top, 屏底)
 * 从列缓冲抄到屏. st7305_blit_1bit 自带屏幕边界硬裁剪; 顶部从 content_top 起 →
 * 永不穿透子分类条/状态栏; 半行/半字/半按钮在窗口上下沿自然可见. */
static void am_col_present(st7305_handle_t *lcd, int grid_x0, int content_top, int row_h) {
    if (!s_col || s_col_h <= 0) return;
    int src_y = (int)lroundf(s_gcam * (float)row_h);
    int win_h = UI_SCREEN_H - content_top;
    if (src_y >= s_col_h) return;
    if (src_y + win_h > s_col_h) win_h = s_col_h - src_y;
    if (win_h <= 0) return;
    st7305_blit_1bit(lcd, grid_x0, content_top, AM_COL_W, win_h,
                     s_col + src_y * AM_COL_BPR);
}

/* ==== 渲染 ==== */
static void p_am_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    const os_theme_t *th = os_theme_get();
    st7305_clear(lcd, th->bg_color);

    int grid_x0 = AM_CATBAR_W;
    /* 常驻分割线已删除: 分类栏与内容区之间不再画贯穿竖线, 选中态由一根 2px 水管线承担 */

    /* 内容起点 (应用/商店 分类带顶部子分类条, 内容下移 AM_SUB_H) */
    int content_top = am_content_top();
    if (am_has_sub()) am_draw_subbar(ctx, lcd, grid_x0, AM_CAT_Y0, UI_SCREEN_W - 1);

    if (s_list_n > 0 && !s_col) {   /* 首次: 分配 PSRAM 整列缓冲 (120KB) */
        s_col = heap_caps_malloc(AM_COL_H_MAX * AM_COL_BPR, MALLOC_CAP_SPIRAM);
        if (!s_col)
            s_col = heap_caps_malloc(AM_COL_H_MAX * AM_COL_BPR, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_col) s_col_dirty = true;
    }

    if (s_list_n > 0 && s_col) {
        /* === 离屏整列预渲染 + 滚动只抄可视窗 + 硬裁剪出框 (V1.6.x) ===
         * 整列一次性预渲染到 s_col; 拖动/滚动只按像素偏移抄可视窗 [content_top, 屏底),
         * st7305_blit_1bit 自带屏幕硬裁剪 → 半行/半字/半按钮在窗口上下沿自然可见,
         * 顶部从 content_top 起 → 永不穿透子分类条/状态栏. */
        if (s_col_dirty) {
            uint64_t t_col0 = (uint64_t)esp_timer_get_time();
            am_col_render();
            s_col_dirty = false;
            uint64_t d_col = (uint64_t)esp_timer_get_time() - t_col0;
            if (d_col >= 10000)
                ESP_LOGW(TAG, "整列预渲染 %llu us (条目=%d 网格=%d 列高=%d)",
                         d_col, s_list_n, (int)s_view_grid, s_col_h);
        }
        int row_h = AM_ROW_H;
        if (s_view_grid) {
            row_h = (UI_SCREEN_H - content_top - 8) / AM_GRID_ROWS;
            if (row_h < AM_ICON_SIZE + 18) row_h = AM_ICON_SIZE + 18;
        }
        am_col_present(lcd, grid_x0, content_top, row_h);
    } else if (s_list_n > 0) {
        /* PSRAM 分配失败兜底: 退回旧逐帧渲染 */
        if (s_view_grid) {
            int cell_w = (UI_SCREEN_W - grid_x0 - 8) / AM_GRID_COLS;
            int step_y = (UI_SCREEN_H - content_top - 8) / AM_GRID_ROWS;
            if (step_y < AM_ICON_SIZE + 18) step_y = AM_ICON_SIZE + 18;
            float cam = s_gcam;
            int grid_start_x = grid_x0 + 4;
            int first = (int)floorf(cam);
            int last = first + AM_GRID_ROWS;
            for (int row = first; row <= last; row++) {
                int y_center = content_top + 2 + (int)(((float)row - cam) * step_y) + AM_ICON_SIZE / 2;
                if (y_center - AM_ICON_SIZE / 2 > UI_SCREEN_H) break;
                if (y_center - AM_ICON_SIZE / 2 < content_top) continue;
                for (int col = 0; col < AM_GRID_COLS; col++) {
                    int i = row * AM_GRID_COLS + col;
                    if (i < 0 || i >= s_list_n) continue;
                    int cx = grid_start_x + col * cell_w + cell_w / 2;
                    bool sel = !input_scheme_touch() && am_focus_on_content() && (i == s_focus);
                    const os_app_t *a = s_list[i];
                    am_draw_app_icon(ctx, lcd, a, cx, y_center, AM_ICON_SIZE, sel);
                    if (s_list_vis[i]) am_draw_mainmenu_dot(lcd, cx, y_center, AM_ICON_SIZE);
                    const char *nm = a ? a->label : "?";
                    int lw = am_label_width(nm);
                    int ly = y_center + AM_ICON_SIZE / 2 + 2;
                    if (sel) fill_rect(lcd, cx - lw / 2, ly, cx + lw / 2 - 1, ly + ZH16_FONT_H - 1, ST7305_COLOR_BLACK);
                    am_draw_label(lcd, cx, ly, nm, sel);
                    if (am_weak_mark(i))
                        draw_hline(lcd, cx - lw / 2, cx + lw / 2 - 1, ly + ZH16_FONT_H, ST7305_COLOR_BLACK);
                }
            }
        } else {
            int row_h = AM_ROW_H;
            float cam = s_gcam;
            int max_vis = (UI_SCREEN_H - content_top) / row_h;
            int list_max = s_list_n - max_vis;
            if (list_max < 0) list_max = 0;
            if (cam > list_max) cam = (float)list_max;
            if (cam < 0) cam = 0;
            int first = (int)floorf(cam);
            for (int i = first; i < s_list_n; i++) {
                int y = content_top + (int)(((float)i - cam) * row_h);
                if (y + row_h > UI_SCREEN_H) break;
                if (y < content_top) continue;
                bool sel = !input_scheme_touch() && am_focus_on_content() && (i == s_focus);
                if (sel) fill_rect(lcd, grid_x0 + 2, y, UI_SCREEN_W - 2, y + row_h - 2, ST7305_COLOR_BLACK);
                else     fill_rect(lcd, grid_x0 + 2, y, UI_SCREEN_W - 2, y + row_h - 2, ST7305_COLOR_WHITE);
                if (i > first) fill_rect(lcd, grid_x0 + 2, y, UI_SCREEN_W - 2, y, ST7305_COLOR_BLACK);
                if (am_weak_mark(i))
                    draw_hline(lcd, grid_x0 + 3, UI_SCREEN_W - 4, y + row_h - 4, ST7305_COLOR_BLACK);
                const os_app_t *a = s_list[i];
                const char *nm = a ? a->label : "?";
                const char *id = a ? a->id : "";
                am_draw_text_l(lcd, grid_x0 + 12, y + 6, nm, sel);
                if (s_cat == AM_CAT_STORE) {
                    const char *pf = am_store_fname_of(a);
                    am_draw_text_l(lcd, grid_x0 + 12, y + 27, pf ? pf : id, sel);
                    bool inst = am_store_installed_of(a);
                    /* 按钮 = 紧贴两字加边框框 (V1.6.x: 去掉 ➕/空框, 框宽正好容纳两字) */
                    const char *act = inst ? "\xe5\x8d\xb8\xe8\xbd\xbd" : "\xe5\xae\x89\xe8\xa3\x85";
                    int txt_w = 32;   /* 两汉字 16px×2 */
                    int bx0 = UI_SCREEN_W - 8 - txt_w;   /* 右缘留 8px 边距 */
                    am_draw_text_l(lcd, bx0, y + 14, act, sel);
                    draw_rect_outline(lcd, bx0 - 3, y + 12, bx0 + txt_w + 2, y + 26,
                                      sel ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
                } else {
                    am_draw_text_l(lcd, grid_x0 + 12, y + 27, id, sel);
                    bool mm = (i >= 0 && i < s_list_n) && s_list_vis[i];
                    const char *st = mm ? "\xe5\x9c\xa8\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95" : "\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95";
                    am_draw_text_l(lcd, UI_SCREEN_W - 58, y + 14, st, sel);
                }
            }
        }
    } else if (s_cat == AM_CAT_STORE) {
        /* 商店空态: 已扫完但一个包都没有 → 显示"待添加"
         * (用户规范 2026-09-28: 内置应用改用 C 版后商店不再列内置应用, 空商店要给明确文案,
         *  不能留空白; 待用户自己放 .lua 包到 /store 或 TF 卡 store_pkg 后再列出).
         * 未扫完仍保持空白 —— 避免"正在扫描…"这种半成品中间态一闪而过. */
        if (am_store_ready()) {
            int w = text_width("\xe5\xbe\x85\xe6\xb7\xbb\xe5\x8a\xa0");   /* 待添加 */
            draw_text(lcd, grid_x0 + (UI_SCREEN_W - grid_x0 - w) / 2, content_top + 60,
                      "\xe5\xbe\x85\xe6\xb7\xbb\xe5\x8a\xa0", false);
        }
    } else {
        draw_text(lcd, grid_x0 + 40, content_top + 60,
                  "\xe8\xaf\xa5\xe5\x88\x86\xe7\xb1\xbb\xe6\x9a\x82\xe6\x97\xa0\xe5\xba\x94\xe7\x94\xa8", false);
        /* 该分类暂无应用 */
    }

    /* 视图切换图标已移除: 改为 长按硬件确认键 3s 切换 (见 p_am_poll/OS_ACTION_BT_SEARCH). */

    /* 左侧分类栏 (引擎/游戏/应用/运维: 4px 等距排布, 仅图标).
     * V1.6.x 选中态 = 从头到尾仅一根 2px 水管线:
     *   右侧一条竖线从上贯穿到底 (形成"完善分割"), 选中行这一段让位改走包围框.
     *   选中行: 右侧进来 → 往左(顶部) → 往下(左边) → 往右(底部) → 回到右侧 → 继续直下到底.
     *   其余未选中行: 右侧就是直的 2px 竖条.
     *   路径依选中行位置而变 (顶部/中间/底部), 拐角先按直线直角处理. */
    int sx  = AM_CATBAR_W;                        /* 右侧分割线列 (2px: sx, sx+1 = 右缘, 右移 2px) */
    int rx  = AM_CATBAR_W + 1;                   /* 右侧分割线右列 (右移 2px) */
    int sy0 = AM_CAT_Y0 + AM_ICON_GAP;                /* 最上图标顶 (状态栏下方) */
    int sy1 = am_cat_cy(AM_CAT_COUNT - 1) + AM_ICON_SIZE / 2; /* 最下图标底 */

    /* 选中行包围框几何 (上/左/下 三边, 开口朝右, 让位 2px 空隙) */
    int cy  = am_cat_cy(s_cat);
    int it  = cy - AM_ICON_SIZE / 2;                  /* 选中图标顶 */
    int ib  = cy + AM_ICON_SIZE / 2;                  /* 选中图标底 */
    int lx  = AM_ICON_X - AM_ICON_SIZE / 2 - AM_SEL_GAP - AM_SEL_W; /* 左竖 x0 */
    int ty  = it - AM_SEL_GAP - AM_SEL_W;             /* 上横 y0 */
    int by  = ib + AM_SEL_GAP;                        /* 下横 y0 */
    int w1  = AM_SEL_W - 1;

    /* 分类图标先铺底 (引擎/运维/应用/商店).
     * cat_icons[] 固定顺序为 引擎/游戏/工具/其他, 这里按四分类语义映射:
     * 引擎→引擎(0), 运维→其他(3), 应用→工具(2), 商店→原样(1).
     * ⚠ 索引 = 行序 = AM_CAT_* 枚举值; 2026-09-28 应用与运维换位, 映射同步换过. */
    static const int s_cat_icon_map[AM_CAT_COUNT] = { 0, 3, 2, 1 };
    for (int r = 0; r < AM_CAT_COUNT; r++)
        draw_cat_icon_stretched(lcd, AM_ICON_X, am_cat_cy(r), AM_ICON_SIZE, AM_ICON_SIZE, s_cat_icon_map[r]);

    /* ==== 一根 2px 水管线 ==== */
    int R = AM_SEL_R;                          /* 拐角外凸圆弧 R3 */
    bool is_top = (s_cat == 0);
    bool is_bot = (s_cat == AM_CAT_COUNT - 1);

    /* 右缘分隔线: 选中行让位 (砍掉 [ty, by+w1] 这段), 上方段 + 下方段贯穿.
     * 上下两段在拐点 (rx,ty)/(rx,by+w1) 处让位 R, 由右上/右下外凸弧连接.
     * 顶部项上方无 (直接对接状态栏), 底部项下方无 (直接到底). */
    if (ty - R > sy0) { draw_vline(lcd, sx,  sy0,   ty - R - 1, ST7305_COLOR_BLACK);
                        draw_vline(lcd, sx+1, sy0,  ty - R - 1, ST7305_COLOR_BLACK); }
    if (by + w1 + R < sy1) { draw_vline(lcd, sx,   by + w1 + R + 1, sy1, ST7305_COLOR_BLACK);
                             draw_vline(lcd, sx+1, by + w1 + R + 1, sy1, ST7305_COLOR_BLACK); }

    /* 选中行包围框 (左竖 + 上横 + 下横, 均 2px), 四角让位 R, 由外凸弧连接.
     * 顶部项不画上横+上两角 (右缘直碰状态栏); 底部项不画下横+下两角 (右缘直到底). */
    int lty0 = is_top ? ty        : ty + R;     /* 左竖上端: 顶部项平头, 否则让 R 给左上角 */
    int lby1 = is_bot ? sy1       : by + w1 - R; /* 左竖下端: 底部项延伸到屏幕底部, 否则让 R 给左下角 */
    draw_vline(lcd, lx, lty0, lby1,     ST7305_COLOR_BLACK);   /* 左竖 */
    draw_vline(lcd, lx + 1, lty0, lby1, ST7305_COLOR_BLACK);
    if (!is_top) { /* 上横 (顶部项无) */
        draw_hline(lcd, lx + w1 + R, rx - R, ty,    ST7305_COLOR_BLACK);
        draw_hline(lcd, lx + w1 + R, rx - R, ty + 1, ST7305_COLOR_BLACK);
    }
    if (!is_bot) { /* 下横 (底部项无) */
        draw_hline(lcd, lx + w1 + R, rx - R, by,    ST7305_COLOR_BLACK);
        draw_hline(lcd, lx + w1 + R, rx - R, by + 1, ST7305_COLOR_BLACK);
    }

    /* ★ 2026-10-02 焦点在**左侧分类栏**时, 把包围框向外加粗 2px (2px 水管 → 4px):
     *   与"内容区取消反色"配对 —— 任何时刻只有一处是强指示, 一眼看出焦点在哪层。
     *   纯附加绘制 (只往更外侧多画两行/两列), 不动原有管线的几何与拐角。 */
    if (s_focus_bar == 2) {
        draw_vline(lcd, lx - 1, lty0, lby1, ST7305_COLOR_BLACK);
        draw_vline(lcd, lx - 2, lty0, lby1, ST7305_COLOR_BLACK);
        if (!is_top) {
            draw_hline(lcd, lx + w1 + R - 2, rx - R, ty - 1, ST7305_COLOR_BLACK);
            draw_hline(lcd, lx + w1 + R - 2, rx - R, ty - 2, ST7305_COLOR_BLACK);
        }
        if (!is_bot) {
            draw_hline(lcd, lx + w1 + R - 2, rx - R, by + w1 + 1, ST7305_COLOR_BLACK);
            draw_hline(lcd, lx + w1 + R - 2, rx - R, by + w1 + 2, ST7305_COLOR_BLACK);
        }
    }

    /* 四角 R3 外凸圆弧弯头 (接触分割线的是右上/右下, 用 kind3/kind4) */
    if (!is_top) { /* 上两角: 仅非顶部项 */
        am_elbow(lcd, lx, ty, R, 1, 2, ST7305_COLOR_BLACK);   /* 左上: 外凸向左上 */
        am_elbow(lcd, rx, ty, R, 4, 2, ST7305_COLOR_BLACK);   /* 右上: 外凸向右下 */
    }
    if (!is_bot) { /* 下两角: 仅非底部项 */
        am_elbow(lcd, lx, by + w1, R, 2, 2, ST7305_COLOR_BLACK);  /* 左下: 外凸向左下 */
        am_elbow(lcd, rx, by + w1, R, 3, 2, ST7305_COLOR_BLACK);  /* 右下: 外凸向右上 */
    }
}

/* 是否有顶部子分类横排 (应用/商店 两分类) */
static bool am_has_sub(void) { return (s_cat == AM_CAT_APP) || (s_cat == AM_CAT_STORE); }

/* ==== 交互 ==== */

static void p_am_action(ui_ctx_t *ctx, os_action_t a) {
    /* 左侧分类栏聚焦 (UP/DOWN 切分类; RIGHT/确认 选定回内容; LEFT/BACK 回内容) */
    if (s_focus_bar == 2) {
        switch (a) {
        /* ⚠ 切左侧分类必须把顶部子分类复位: 否则残留的 s_sub 会跟到别的分类去
         * (触摸路径本来就有 s_sub = AM_SUB_ALL, 键盘这条原先漏了 → 在「应用→游戏」
         *  后按 UP/DOWN 切到「引擎」, 残留"游戏"会让按子分类过滤的逻辑把内容滤空)。 */
        case OS_ACTION_UP:
            s_cat = (s_cat + AM_CAT_COUNT - 1) % AM_CAT_COUNT;
            s_sub = AM_SUB_ALL; am_rebuild();
            if (s_cat == AM_CAT_STORE) am_store_wifi_gate(ctx);
            break;
        case OS_ACTION_DOWN:
            s_cat = (s_cat + 1) % AM_CAT_COUNT;
            s_sub = AM_SUB_ALL; am_rebuild();
            if (s_cat == AM_CAT_STORE) am_store_wifi_gate(ctx);
            break;
        case OS_ACTION_RIGHT:
        case OS_ACTION_CONFIRM:
        case OS_ACTION_BACK:
        case OS_ACTION_LEFT:
        case OS_ACTION_HOME:
            s_focus_bar = 0;
            s_col_dirty = true;   /* ★ 回内容区 → 恢复反色, 须重渲染整列 */
            break;
        default: break;
        }
        ctx->needs_redraw = true;
        return;
    }
    /* 顶部子分类横排聚焦 (LEFT/RIGHT 切子分类; DOWN/确认 选定回内容; UP 不继续向上=停在此层) */
    if (s_focus_bar == 1) {
        switch (a) {
        case OS_ACTION_LEFT:  s_sub = (s_sub + AM_SUB_COUNT - 1) % AM_SUB_COUNT; am_rebuild(); break;
        case OS_ACTION_RIGHT: s_sub = (s_sub + 1) % AM_SUB_COUNT; am_rebuild(); break;
        case OS_ACTION_DOWN:
        case OS_ACTION_CONFIRM:
            s_focus_bar = 0; break;                 /* 选定子分类 → 回内容 */
        case OS_ACTION_UP:
            break;                                  /* 子分类已到顶: 不再继续向上 */
        case OS_ACTION_BACK:
            s_focus_bar = 0; break;
        default: break;
        }
        ctx->needs_redraw = true;
        return;
    }
    switch (a) {
    case OS_ACTION_UP:
        /* ★ 2026-10-02: 到顶**不再停在内容**。原来这里的注释写"已无顶部子分类条",
         *   但实现相反 —— 顶部子分类条(应用/商店)确实画出来了 (am_draw_subbar, 见 760 行)。
         *   而且 `s_focus_bar` 全文件从未被赋 1 → 手柄/键盘**永远够不到**子分类层,
         *   492 行那处"子分类聚焦 U 形括号"因此是死代码。现在补上这一跳(三层贯通)。 */
        if (s_view_grid) {
            if (s_focus >= AM_GRID_COLS) {
                s_focus -= AM_GRID_COLS;
                am_gcam_animate(s_focus / AM_GRID_COLS, 300);
                s_col_dirty = true;   /* 选中行变 → 重渲染整列 */
            } else {
                s_focus_bar = am_has_sub() ? 1 : 0;   /* 有子分类条就上去, 否则停住 */
                s_col_dirty = true;                   /* 内容改画"弱定位" */
            }
        } else {
            if (s_focus > 0) { s_focus--; am_gcam_animate(s_focus, 260); s_col_dirty = true; }
            else { s_focus_bar = am_has_sub() ? 1 : 0; s_col_dirty = true; }
        }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_DOWN:
        if (s_view_grid) {
            if (s_focus + AM_GRID_COLS < s_list_n) {
                s_focus += AM_GRID_COLS;
                am_gcam_animate(s_focus / AM_GRID_COLS, 300);
                s_col_dirty = true;
            }
        } else {
            if (s_focus + 1 < s_list_n) { s_focus++; am_gcam_animate(s_focus, 260); s_col_dirty = true; }
        }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_LEFT:
        /* 内容已到最左列 → 跳到左侧分类栏 */
        if (s_view_grid ? (s_focus % AM_GRID_COLS == 0) : (s_focus == 0)) {
            s_focus_bar = 2;
            s_col_dirty = true;   /* ★ 焦点离开内容区 → 内容改画"弱定位", 须重渲染整列 */
            ctx->needs_redraw = true;
            break;
        }
        s_focus--;
        if (s_view_grid) am_gcam_animate(s_focus / AM_GRID_COLS, 260);
        else am_gcam_animate(s_focus, 260);
        s_col_dirty = true;
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_RIGHT:
        if (s_focus + 1 < s_list_n) {
            s_focus++;
            if (s_view_grid) am_gcam_animate(s_focus / AM_GRID_COLS, 260);
            else am_gcam_animate(s_focus, 260);
            s_col_dirty = true;
        }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_CONFIRM:
        if (s_list_n <= 0) break;
        {
            const os_app_t *a = s_list[s_focus];
            if (!a) break;
            am_launch(ctx, a);     /* 打开/安装/Lua 启动 统一入口 */
        }
        break;
    case OS_ACTION_BT_SEARCH:
        if (s_cat == AM_CAT_STORE) {
            /* 商店 (列表视图): 确认键长按 3s → 准备卸载当前选中的已安装包 */
            s_uninstall_armed = true;
            s_uninstall_t0    = esp_timer_get_time() / 1000;
            s_uninstall_fired = false;
            ctx->needs_redraw = true;
            break;
        }
        /* 非商店分类: 视图固定不可切换 (图标/列表模式由分类固定), 忽略长按 */
        break;
    case OS_ACTION_BACK:
    case OS_ACTION_HOME:
        os_pop(ctx);
        break;
    default:
        break;
    }
}

static bool p_am_touch(ui_ctx_t *ctx, int x, int y) {
    /* 拖动滚动中: 忽略点击 (松手后由 poll 复位 s_gdrag_active) */
    if (s_gdrag_active) return true;
    /* 长按已触发: 吞掉松手后的那次 tap (避免重复弹操作窗) */
    if (s_lp_suppress) { s_lp_suppress = false; return true; }
    /* 左侧分类栏 (触摸按图标中心判定, 触摸区上下延伸: 取最近的分类中心) */
    if (x < AM_CATBAR_W) {
        int best = -1, best_d = 1 << 30;
        for (int r = 0; r < AM_CAT_COUNT; r++) {
            int d = y - am_cat_cy(r);
            if (d < 0) d = -d;
            if (d < best_d) { best_d = d; best = r; }
        }
        if (best >= 0 && best != s_cat) {
            s_cat = best;
            s_sub = AM_SUB_ALL;
            s_focus = 0; s_scroll = 0;
            s_focus_bar = 0;   /* 触摸点分类后回到内容聚焦 */
            am_rebuild();
            if (s_cat == AM_CAT_STORE) am_store_wifi_gate(ctx);
            ctx->needs_redraw = true;
        }
        return true;
    }
    /* 应用/商店 顶部子分类条可点击: 按 x 分槽选中对应子分类 */
    if (y >= AM_CAT_Y0 && y < AM_CAT_Y0 + AM_SUB_H && (s_cat == AM_CAT_APP || s_cat == AM_CAT_STORE)) {
        if (x >= AM_CATBAR_W) {
            int slot = (UI_SCREEN_W - AM_CATBAR_W) / AM_SUB_COUNT;
            int idx = (x - AM_CATBAR_W) / (slot > 0 ? slot : 1);
            if (idx < 0) idx = 0;
            if (idx >= AM_SUB_COUNT) idx = AM_SUB_COUNT - 1;
            if (idx != s_sub) {
                s_sub = idx;
                s_focus = 0; s_scroll = 0; s_gcam = 0; s_gcam_anim_start = 0;
                am_rebuild();
                ctx->needs_redraw = true;
            }
        }
        return true;
    }
    /* 右栏内容 (y < content_top: 顶部状态栏/子分类条区域 → 不触发下方图标) */
    int content_top = am_content_top();
    if (y < content_top) return false;
    /* 商店后台扫描未完成: 吞掉内容点击, 避免对半成品列表误操作 (状态栏/分类栏仍可用) */
    if (s_cat == AM_CAT_STORE && !am_store_ready()) return true;
    if (s_view_grid) {
        int cell_w = (UI_SCREEN_W - AM_CATBAR_W - 8) / AM_GRID_COLS;
        int step_y = (UI_SCREEN_H - content_top - 8) / AM_GRID_ROWS;
        if (step_y < AM_ICON_SIZE + 18) step_y = AM_ICON_SIZE + 18;
        int col = (x - (AM_CATBAR_W + 4)) / cell_w;
        int row_vis = (y - (content_top + 2)) / step_y;
        if (col < 0 || col >= AM_GRID_COLS || row_vis < 0) return false;
        int row = (int)floorf(s_gcam) + row_vis;
        int idx = row * AM_GRID_COLS + col;
        if (idx >= 0 && idx < s_list_n) {
            vibrator_click();
            const os_app_t *a = s_list[idx]; if (a) am_launch(ctx, a);  /* 触屏: 点一次直接打开 */
            ctx->needs_redraw = true;
            return true;
        }
    } else {
        int row_h = AM_ROW_H;
        int idx = (int)floorf(s_gcam) + (y - content_top) / row_h;
        if (idx >= 0 && idx < s_list_n) {
            vibrator_click();
            const os_app_t *a = s_list[idx];
            int y_row = content_top + (int)(((float)idx - s_gcam) * row_h);
            if (a) am_launch_list(ctx, a, x, y, y_row);
            ctx->needs_redraw = true;
            return true;
        }
    }
    return false;
}

/* ==== 长按桌面快捷方式确认 ==== */
static void am_lp_confirm_cb(ui_ctx_t *ctx, int result, void *ud) {
    const os_app_t *a = (const os_app_t *)ud;
    if (!a) return;
    if (result != 0) return;   /* 取消: 弹窗由框架自动关 */
    bool vis = os_app_visible(a);
    if (vis) {
        /* 移出主菜单: 显式加入隐藏列表, 覆盖 reveal/隐藏开关 的"强制显示",
         * 否则引擎等 reveal 应用删图标后仍留在主菜单/圆圈不消失 (冲突根因). */
        os_app_set_mainmenu(a->id, false);
        os_app_set_hidden(a->id, true);
        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x88\xa0\xe9\x99\xa4\xe6\xa1\x8c\xe9\x9d\xa2\xe5\x9b\xbe\xe6\xa0\x87"); /* 已删除桌面图标 */
    } else {
        os_app_set_mainmenu(a->id, true);
        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe6\xb7\xbb\xe5\x8a\xa0\xe6\xa1\x8c\xe9\x9d\xa2\xe5\x9b\xbe\xe6\xa0\x87"); /* 已添加桌面图标 */
    }
    /* 确定后必须立即关闭确认弹窗: os_dialog_toast 会置 s_cb_handled 阻止 dlg_finish_top
     * 自动关, 这里显式弹掉 (否则确认框一直卡着). 之后由 os_enter 重建可见列表. */
    os_dialog_pop(ctx);
    ctx->needs_redraw = true;
    /* 刷新可见性缓存 (长按增删桌面图标后不能让渲染用旧值) */
    for (int i = 0; i < s_list_n; i++)
        s_list_vis[i] = (s_list[i]) ? os_app_visible(s_list[i]) : false;
}
/* 按住图标 3s → 弹确认 (已添加到桌面则问删除, 否则问添加). 确认后自动关闭并弹 0.5s 提示 */
static void am_long_press(ui_ctx_t *ctx, int idx) {
    if (idx < 0 || idx >= s_list_n) return;
    if (!s_list[idx]) return;   /* 仅已安装应用图标 */
    if (s_cat == AM_CAT_STORE) {          /* 商店长按 → 卸载已安装包 */
        am_store_uninstall_ask(ctx, s_list[idx]);
        return;
    }
    /* 2026-09-22: Lua 文件项**也要能加/删桌面图标**。原先这里直接 return → 长按完全没反应
     * (用户想"把白板放主菜单"时点了没动静)。启动侧已由 am_lua_open_if_entry 兜住。 */
    const os_app_t *a = s_list[idx];
    bool mm = os_app_visible(a);
    const char *msg = mm ? "\xe5\x88\xa0\xe9\x99\xa4\xe6\xa1\x8c\xe9\x9d\xa2\xe5\x9b\xbe\xe6\xa0\x87?"     /* 删除桌面图标? */
                         : "\xe6\xb7\xbb\xe5\x8a\xa0\xe6\xa1\x8c\xe9\x9d\xa2\xe5\x9b\xbe\xe6\xa0\x87?";   /* 添加桌面图标? */
    os_dialog_confirm_ex(ctx, msg, 0, 0, am_lp_confirm_cb, (void *)a);
}

/* 触摸点 → 当前视图条目索引 (网格/列表) */
static int am_idx_at(int tx, int ty, int content_top) {
    if (tx < AM_CATBAR_W || ty < content_top) return -1;
    if (s_view_grid) {
        int cell_w = (UI_SCREEN_W - AM_CATBAR_W - 8) / AM_GRID_COLS;
        int step_y = (UI_SCREEN_H - content_top - 8) / AM_GRID_ROWS;
        if (step_y < AM_ICON_SIZE + 18) step_y = AM_ICON_SIZE + 18;
        int col = (tx - (AM_CATBAR_W + 4)) / cell_w;
        int row_vis = (ty - (content_top + 2)) / step_y;
        if (col < 0 || col >= AM_GRID_COLS || row_vis < 0) return -1;
        int row = (int)floorf(s_gcam) + row_vis;
        int idx = row * AM_GRID_COLS + col;
        return (idx >= 0 && idx < s_list_n) ? idx : -1;
    } else {
        int idx = (int)floorf(s_gcam) + (ty - content_top) / AM_ROW_H;
        return (idx >= 0 && idx < s_list_n) ? idx : -1;
    }
}

/* ==== 每帧轮询: 触摸上下拖动 (相机跟手 + 惯性甩动 + 网格吸附, 仿主菜单) ==== */
static void p_am_poll(ui_ctx_t *ctx) {
    /* 商店联网门禁: 用户点了"确定" → 这一帧(确认框已收)再开连网流程 */
    if (s_store_wifi_connect) {
        s_store_wifi_connect = false;
        settings_wifi_connect_open(ctx);           /* ★ 唯一入口: 内部按需 os_hw_request */
    }
    /* 子页没真推入(如 Wi-Fi 门禁拦下)时把"离开管家"标记收回, 否则会累积 */
    if (s_am_leave_child > 0 && ctx->current_page == OS_PAGE_APP_MANAGER) s_am_leave_child = 0;
    /* 商店卸载: 确认键按满 3s (BT_SEARCH 起点再加 1s) 且未松手 → 弹卸载确认 */
    if (s_cat == AM_CAT_STORE && s_uninstall_armed && !s_uninstall_fired) {
        if (input_is_held(1)) {
            if ((esp_timer_get_time() / 1000 - s_uninstall_t0) >= 1000) {
                s_uninstall_armed  = false;
                s_uninstall_fired  = true;
                am_store_uninstall_ask(ctx, s_list[s_focus]);
            }
        } else {
            s_uninstall_armed = false;
        }
    }

    /* 相机动画进行中: 每帧请求重绘 */
    if (s_gcam_anim_start > 0) {
        am_gcam_tick();
        ctx->needs_redraw = true;
    }

    /* 分类 Lua 后台扫描完成: 影子缓冲搬到 s_lua, 标记数据就绪, 重建列表 (UI 线程取货).
     * 迁移须在 UI 线程一次性完成 (后台只写 s_lua_bg, 避免与渲染读 s_lua 竞态). */
    if (am_lua_tick()) {
        if (s_cat != AM_CAT_STORE) { am_rebuild_keep(); ctx->needs_redraw = true; }
    }

    /* 商店包后台扫描完成: 一次性清理 SD 独立副本 + 重建列表 (UI 线程取货) */
    if (am_store_tick()) {
        if (s_cat == AM_CAT_STORE) {
            am_rebuild_keep();
            ctx->needs_redraw = true;
        }
    }
    if (s_list_n <= 0) return;

    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    int content_top = am_content_top();
    int row_h = s_view_grid
        ? ((UI_SCREEN_H - content_top - 8) / AM_GRID_ROWS) : AM_ROW_H;
    if (s_view_grid && row_h < AM_ICON_SIZE + 18) row_h = AM_ICON_SIZE + 18;
    int max_row = s_view_grid ? am_max_scroll() : 0;
    if (!s_view_grid) {
        int mv = (UI_SCREEN_H - content_top) / AM_ROW_H;
        max_row = s_list_n - mv;
        if (max_row < 0) max_row = 0;
    }

    if (!down) {
        /* 松手: 平滑吸附到最近焦点行 (无惯性甩手/弹簧回弹) */
        if (s_gdrag_active) {
            s_gdrag_active = false;
            am_gcam_animate(lroundf(s_gcam), 260);
            ctx->needs_redraw = true;
        }
        s_gdrag_last_y = -1;
        s_lp_idx = -1;
        s_lp_fired = false;
        return;
    }

    /* 只在右栏内容区开始拖动 */
    if (tx < AM_CATBAR_W || ty < content_top || ty >= UI_SCREEN_H) {
        s_gdrag_last_y = -1;
        s_lp_idx = -1;
        return;
    }

    if (s_gdrag_last_y < 0) {
        s_gdrag_last_y  = ty;
        s_gdrag_anchor_y = ty;
        s_gdrag_base     = s_gcam;
        s_gdrag_active   = false;
        s_gcam_anim_start = 0;   /* 新拖动取消旧动画 */
        s_lp_idx   = am_idx_at(tx, ty, content_top);
        s_lp_t0    = am_now_ms();
        s_lp_fired = false;
        return;
    }
    s_gdrag_last_y = ty;

    /* 位移过大 → 判为拖动, 取消长按意图 */
    int moved = ty - s_gdrag_anchor_y;
    if (moved < 0) moved = -moved;
    if (moved >= AM_DRAG_THRESH) { s_gdrag_active = true; }
    if (moved >= AM_LP_DRAG_MAX) { s_lp_idx = -1; s_lp_fired = false; }
    else if (s_lp_idx >= 0 && !s_lp_fired && !s_gdrag_active) {
        /* 按住 3s 未放手即触发 */
        if (am_now_ms() - s_lp_t0 >= AM_LP_MS) {
            s_lp_fired = true;
            s_lp_suppress = true;
            am_long_press(ctx, s_lp_idx);
            return;
        }
    }

    /* 跟手: 相机 = 基点 - 位移/行高 */
    s_gcam = s_gdrag_base - (float)(ty - s_gdrag_anchor_y) / (float)row_h;
    /* 边界 */
    if (s_gcam < 0) s_gcam = 0;
    if (s_gcam > max_row) s_gcam = (float)max_row;
    if (s_gdrag_active) ctx->needs_redraw = true;
}

/* ==== 模块 ==== */
static void p_am_enter(ui_ctx_t *ctx) {
    /* V1.7.x: 进页面耗时分解 (打开应用中心"要等一会儿"的定位探针) */
    uint64_t t_all = (uint64_t)esp_timer_get_time();
    uint64_t t_prev = t_all;
    /* 触摸重检: 开机时 GT911 未就绪会误判"无触摸"(返回键降级成方向键), 进管家时
     * 重新探测并刷新物理键语义, 恢复 BOOT=返回. 已有触摸则为零开销. */
    /* 只重探**一次**: 本机没有触摸芯片时, 每次进管家都探会白等一遍 I2C 超时 */
    static bool s_reprobed;
    if (!s_reprobed && !touch_panel_is_present()) {
        s_reprobed = true;
        touch_panel_reprobe();
        input_recheck_touch();
    }
    uint64_t t_touch = (uint64_t)esp_timer_get_time();
    /* 应用管家 = 主菜单级引擎生命周期边界: 从任意引擎/软件页 os_pop 回应用管家时,
     * 统一卸载所有已加载引擎的后台 (PSRAM/字体缓冲), 防止引擎内存泄漏到应用管家.
     * - unload_all 幂等 (各引擎 unload 均带 guard/null 保护), 即使 gb/vpet 在 select
     *   内已直接 unload 也不会双重释放; 未加载引擎时为 no-op.
     * - do_leak_check=false: 应用管家自身常驻 UI 缓冲使空闲低于开机基线, 此处比对
     *   会误报 [LEAK]; 真正的泄漏复核由主菜单返回路径 (os_mgr) 的 unload_all(true) 兜底. */
    engine_manager_unload_all(false);
    uint64_t t_unload = (uint64_t)esp_timer_get_time();
    /* 不再无条件重置分类/子分类/焦点: 从应用返回时 os_pop 会再次触发 on_enter,
     * 保留 static 状态即可回到"原分类原选项" (与主菜单返回保持位置的既定行为一致).
     * 仅当状态非法时由 am_rebuild 内部兜底复位.
     * 用 am_rebuild_keep: 连滚动位置一起保留 —— 触摸点开第二排的应用再退出时,
     * 画面停在打开前那一屏 (第一排仍在), 不会把第二排顶到第一行. */
    /* 进应用管家即后台预扫描商店包 (幂等: 已扫描/扫描中会早退), 不必等首次切到商店;
     * 这样首次进商店时列表/安装态已就绪, 不再闪现"正在扫描…". */
    am_store_prefetch();
    if (s_cat == AM_CAT_STORE) am_store_wifi_gate(ctx);   /* 进来就停在商店 → 照样提示一次 */
    uint64_t t_store = (uint64_t)esp_timer_get_time();
    am_rebuild_keep();
    uint64_t t_end = (uint64_t)esp_timer_get_time();
    ESP_LOGI(TAG, "enter 分解: touch=%llu unload=%llu store=%llu rebuild=%llu 合计=%llu us (分类=%d 条目=%d)",
             t_touch - t_prev, t_unload - t_touch, t_store - t_unload, t_end - t_store,
             t_end - t_all, s_cat, s_list_n);
    /* 无触摸屏时触摸类应用(白板/触屏测试等)在表里就被过滤掉了, **不再弹提示** ——
     * linit 2026-10-01: 本机(微雪)就是没有触摸芯片的硬件, 每次进管家都提一句是噪音。 */
}

/* 离开管家: 进子页只收回标记; **真退出**才复位商店联网提示 (下次重进重新提示一次) */
static void p_am_exit(ui_ctx_t *ctx) {
    (void)ctx;
    if (s_am_leave_child > 0) s_am_leave_child--;
    else                      s_store_wifi_asked = false;
    ESP_LOGI(TAG, "exit: child=%d asked=%d", s_am_leave_child, (int)s_store_wifi_asked);
}
static const os_module_t s_mod_app_manager = {
    .name       = "app_butler",
    .page_id    = OS_PAGE_APP_MANAGER,
    .on_enter   = p_am_enter,
    .on_exit    = p_am_exit,
    .render     = p_am_render,
    .action     = p_am_action,
    .touch      = p_am_touch,
    .poll       = p_am_poll,
    .fullscreen = false,   /* 显示状态栏 */
};

void os_page_app_manager_register(void) {
    os_register(&s_mod_app_manager);
    am_lua_gate_register();           /* need=wifi 应用的启动门禁 (全局 service, 每帧轮询) */
}