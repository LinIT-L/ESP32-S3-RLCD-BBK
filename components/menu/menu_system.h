#ifndef MENU_SYSTEM_H
#define MENU_SYSTEM_H

#include "st7305.h"
#include "bt_manager.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 菜单页面 */
typedef enum {
    MENU_PAGE_MAIN = 0,          /* 主菜单 */
    MENU_PAGE_SELECT_GAME,       /* 选择文曲星游戏 */
    MENU_PAGE_SETTINGS,          /* 设置 (容器页面) */
    MENU_PAGE_GAMEPAD,           /* 手柄配置 */
    /* === 设置子页面 === */
    MENU_PAGE_SETTINGS_DISPLAY,  /* 显示设置 */
    MENU_PAGE_SETTINGS_TIME,     /* 时间设置 */
    MENU_PAGE_SETTINGS_BT,       /* 蓝牙设备 */
    MENU_PAGE_SETTINGS_SD,       /* TF卡管理 */
    MENU_PAGE_SETTINGS_INFO,     /* 系统信息 */
    /* === 其他页面 === */
    MENU_PAGE_KEY_CONFIG,        /* 按键设置 */
    MENU_PAGE_VOLUME,            /* 音量设置 */
    MENU_PAGE_RETURN_GAME,       /* 返回游戏 */
    MENU_PAGE_FILE_BROWSER,      /* 文件浏览器 */
    MENU_PAGE_MP3_PLAYER,        /* MP3 播放器 */
    MENU_PAGE_GAME_SETTINGS,     /* 游戏设置 (电子词典的子页面) */
    MENU_PAGE_GB_GAME,           /* GB 游戏 (复用 SELECT_GAME 分栏布局) */
    MENU_PAGE_BOOK,              /* 电子书 */
    MENU_PAGE_WALLPAPER,         /* 壁纸设置 */
    MENU_PAGE_POMODORO,          /* 番茄钟 */
    MENU_PAGE_COUNT
} menu_page_t;

/* 按键动作 (3 键导航: 左/右/确认 + 退出) */
typedef enum {
    MENU_ACTION_NONE,
    MENU_ACTION_UP,
    MENU_ACTION_DOWN,
    MENU_ACTION_LEFT,
    MENU_ACTION_RIGHT,
    MENU_ACTION_CONFIRM,
    MENU_ACTION_BACK,
    MENU_ACTION_HOME,    /* 退出到菜单页面 (手柄映射第 7 键) */
    MENU_ACTION_LONG_LEFT,  /* LEFT 长按 500ms (V1.0.39: 手柄配置弹窗用此快捷键直接扫描) */
    MENU_ACTION_LONG_PRESS, /* V1.0.68: 触摸长按 (游戏列表收藏等) */
    MENU_ACTION_POWER_LOCK,    /* V1.0.68: 软关机键点按 = 锁屏休眠 */
    MENU_ACTION_POWER_HINT,    /* V1.0.68: 软关机键 0.5s = 弹"返回菜单"提示 */
    MENU_ACTION_POWER_RELEASE, /* V1.0.68: 软关机键 0.5s 后松手 = 返回主菜单 */
} menu_action_t;

/* 游戏退出确认弹窗结果 */
typedef enum {
    GAME_EXIT_CANCEL = 0,   /* 取消, 恢复游戏继续 */
    GAME_EXIT_CONFIRMED,    /* 确认退出 (返回游戏二级菜单) */
    GAME_EXIT_TIMEOUT,      /* 无操作超时退出 (返回桌面) */
} game_exit_result_t;

/* 屏保类型 */
typedef enum {
    SCREENSAVER_STARS = 0,      /* 星空动画 */
    SCREENSAVER_COUNT
} screensaver_type_t;

/* 蓝牙连接设备类型 */
typedef enum {
    BT_DEVICE_NONE,         /* 未连接 */
    BT_DEVICE_GAMEPAD,      /* 手柄 */
    BT_DEVICE_HEADPHONE,    /* 耳机/音箱 */
    BT_DEVICE_OTHER         /* 其他设备 */
} bt_device_type_t;

/* 显示设置状态 */
typedef struct {
    bool    low_power;      /* 低功耗模式 */
    uint8_t volume;         /* 0-100, 音量 */
    bool    mute;           /* 静音 */
    bool    audio_disable;  /* V1.0.68: 禁用音频 (强制关掉音乐+游戏声音) */
    uint8_t audio_scheme;   /* V1.0.68: 音频方案 0=解码输出 1=方波直驱(PWM) 2=禁用音频 */
    bool    touch_disable;  /* V1.0.68: 禁用触摸屏 (释放触摸内存) */
    uint8_t tone_effect_sel;/* V1.0.68: 试听音效选择 (0-5) */
    /* 状态栏显示状态 */
    bool    bt_connected;   /* 蓝牙连接状态 */
    bool    bt_enabled;     /* 蓝牙开关 */
    bt_device_type_t bt_device_type;  /* 连接的设备类型 */
    bool    pad_connected;  /* 手柄连接状态 */
    bool    wifi_enabled;   /* WiFi 开关 */
    uint8_t battery;        /* 电量 0-100 */
    /* 时间设置 */
    uint8_t hour;           /* 小时 0-23 */
    uint8_t minute;         /* 分钟 0-59 */
    uint8_t second;         /* 秒 0-59 */
    /* 屏保: 内部固定使用星空动画, 3 分钟超时, 默认开启 (无 UI 选项) */
    screensaver_type_t screensaver_type;  /* 保留供内部使用 (仅星空类型) */
    /* 游戏状态栏: 游戏界面是否显示状态栏 (true=显示, false=隐藏) */
    bool    game_status_bar;
} menu_settings_t;

/* 前置声明: 保证下方函数指针参数与 menu_state_t 内的回调指针是同一 struct 类型
 * (GCC14/IDF5.5 下, 原型作用域内隐式声明的 struct 会被视为不同类型而报错) */
struct menu_state_s;

/* 列表弹窗嵌套栈: 保存父弹窗状态, 子弹窗关闭后恢复 (含原选中位置) */
#define LIST_DIALOG_STACK_DEPTH   4
typedef struct {
    char            items[100][64];   /* 20260812: 与 list_dialog_items 同步扩容 */
    int             count;
    int             selected;
    int             scroll;
    menu_page_t     return_page;
    void          (*on_select)(struct menu_state_s *state, int idx);
    void          (*on_key)(struct menu_state_s *state, int idx, menu_action_t action);
    void          (*on_close)(struct menu_state_s *state);
    void          (*on_render)(struct menu_state_s *state, st7305_handle_t *lcd,
                               int content_x, int content_y, int content_w, int content_h);
    int             prev_selected;
    bool            prev_active;
    bool            content_dirty;
} list_dialog_saved_t;

/* 菜单状态 */
typedef struct menu_state_s {
    menu_page_t      current_page;
    int              selected_index;   /* 主菜单: 横向索引; 子页: 列表索引 */
    int              scroll_offset;
    bool             needs_redraw;
    st7305_handle_t *lcd;
    menu_settings_t  settings;
    /* 子页内编辑模式: 选中项进入编辑后, 左右键改值, 再按确认退出编辑 */
    int              editing_index;    /* -1 = 未编辑 */
    /* 临时提示 (e.g. "USB MSC 开发中"), 0 = 无提示 */
    char             hint_text[64];
    uint32_t         hint_until_ms;    /* 过期时间戳 */
    /* 确认弹窗: confirm_active=true 时, 再次 KEY 确认, 长按 BOOT 取消 */
    bool             confirm_active;
    bool             confirm_notice;   /* true = 通知弹窗 (任意 KEY/BACK 关闭), false = 确认弹窗 */
    bool             confirm_no_hint;  /* true = 不显示底部按键提示 (如 USB 挂载中) */
    bool             confirm_executing; /* 弹窗已 KEY 确认, 正在调用 on_confirm 执行 */
    char             confirm_title[32];
    char             confirm_msg[64];
    int              confirm_idx;      /* 弹窗要执行的原 idx (供用户回退用) */
    uint32_t         confirm_until_ms; /* 弹窗自动关闭超时 (0=无超时, 用于蓝牙连接等待) */
    /* 连接小弹窗 (用户需求: 居中对齐的小弹窗显示"正在连接"/"连接成功") */
    bool             connecting_popup_active;   /* true = 小弹窗可见 */
    bool             connecting_popup_success;  /* false="正在连接", true="连接成功" */
    uint32_t         connecting_popup_until_ms; /* 自动关闭时间戳 (0=手动/回调关闭) */
    uint32_t         connecting_popup_started_at_ms; /* V1.0.24: 弹窗开始显示时刻, 看门狗用 */
    /* 列表选择弹窗 */
    bool             list_dialog_active;
    char             list_dialog_title[32];
    char             list_dialog_items[100][64];   /* 20260812: 支持游戏壁纸全量列表滚动 (原16不够) */
    int              list_dialog_count;
    int              list_dialog_selected;
    int              list_dialog_scroll;
    menu_page_t      list_dialog_return_page;
    void             (*list_dialog_on_select)(struct menu_state_s *state, int idx);
    /* 弹窗局部刷新: 仅 list_dialog_active 期间, 选项变化时只重绘新旧两行
     * (避免每帧 st7305_clear + 整页重绘造成的闪烁) */
    int              list_dialog_prev_selected; /* 上一次选中的 idx, -1=未初始化 */
    int              list_dialog_prev_scroll;   /* 上一次滚动偏移 (用于检测滚动, 滚动变化强制全量重绘刷新滚动条) */
    bool             list_dialog_prev_active;   /* 上一次弹窗是否激活 (用于检测开/关) */
    bool             list_dialog_local_update;  /* 本帧仅弹窗内选项变化, 跳过底页重绘 */
    /* 主菜单切换动画 (滑动 + 缩放) */
    int              prev_selected;    /* 动画开始时的索引 */
    int              anim_direction;   /* -1=向左滑, +1=向右滑, 0=无动画 */
    uint32_t         anim_start_ms;    /* 动画开始时间戳 */
    /* V1.0.66: 主菜单跟手拖动 (手指拖动整排图标平移) */
    int              main_drag_offset;  /* 当前拖动偏移(像素), 渲染时叠加到图标 x */
    bool             main_drag_active;  /* 是否正在拖动 */
    int              main_drag_start_x; /* 拖动起始触摸屏幕 x */
    /* V1.0.68: 游戏二级菜单右栏跟手拖动 (上下滑动列表跟随手指) */
    int              select_game_drag_offset;  /* 右栏垂直拖动偏移(像素) */
    bool             select_game_drag_active;  /* 是否正在拖动右栏 */
    int              select_game_drag_start_y; /* 拖动起始触摸屏幕 y */
    bool             select_game_drag_fix;     /* V1.0.68: 松手后固定内容页位置 (渲染跳过选中钳制) */
    /* V1.0.68: 列表弹窗 (番茄钟时间列表等) 跟手拖动 */
    int              list_dialog_drag_offset;  /* 内容区拖动偏移(px), 渲染时叠加到行 y */
    bool             list_dialog_drag_fix;     /* 松手后固定内容位置 (跳过选中钳制) */
    /* 进入二级菜单前保存的主菜单位置 */
    int              main_selected_index;
    /* V1.0.41: 二级菜单(sub_pages)进入子项前的选中位置, 返回时恢复 */
    int              sub_selected_index;
    /* 蓝牙扫描设备列表 */
    bt_device_t      bt_devices[20];
    int              bt_device_count;
    bool             bt_scan_active;   /* 弹窗正在扫描中 */
    /* 主动连接: 扫描过程中匹配历史记录并自动连接.
     * bt_auto_connect_active=true 时, 扫描结果中如发现与历史记录 MAC 匹配的设备,
     * 自动调用 bt_manager_connect_device 主动连接 (选 RSSI 强的一个). */
    bool             bt_auto_connect_active;     /* 是否处于"搜索主动连接"模式 */
    int              bt_auto_connect_found;      /* 当前已发现的历史设备数 */
    char             bt_auto_connect_target[32]; /* 当前尝试连接的目标设备名 */
    bool             bt_connect_awaiting;         /* 正在等待连接任务结果(手动/主动), 用于区分"连接失败"与正常断开 */
    /* 后台轮询标记: 蓝牙开启后, 等 HID Host 就绪自动启动一次主动连接扫描.
     * 用户场景: 设备不重启, 手柄断电后十几分钟再开, 设备原配置不主动扫,
     *           需手动点"搜索主动连接设备"才能连上. 设为 true 后, 每帧检查
     *           HID Host 就绪+已配对+未连接, 满足则触发主动连接扫描一次. */
    bool             bt_auto_connect_on_enable;  /* 开蓝牙后自动启动主动连接的待触发标志 */
    /* 首次连接的新设备: 连接成功 0.5s 后自动跳转按键映射界面 (无需弹窗确认).
     * >0 = 目标时间戳 (ms), 到点后自动 start_key_mapping; 0 = 无待跳转 */
    uint32_t         bt_map_jump_at_ms;
    /* "已连接 X" 提示是否为手柄映射引导提示: 是则手动按确认键也直接进映射
     * (避免"手动点击确认却只是关掉提示/闪退回菜单"的问题) */
    bool             bt_map_notice_active;
    /* 主菜单页面未连接设备时, 每 5 秒自动尝试连接一次历史记录设备 (静默).
     * bt_retry_next_ms: 下次尝试时间戳 (0=未排期); bt_retry_hist_idx: 轮询游标 */
    uint32_t         bt_retry_next_ms;
    int              bt_retry_hist_idx;
    bool             bt_retry_scan_active;    /* 静默扫描重连中 (无 UI) */
    bool             bt_retry_direct_failed;  /* 上次直连失败, 下轮走扫描 */
    uint32_t         bt_retry_scan_until_ms;  /* 静默扫描截止时间 */
    bool             bt_retry_connect_pending; /* 本次连接由后台静默重连发起 */
    /* 按键映射小弹窗 (draw_notice_popup 样式):
     * key_mapping_idx >= 0 时激活, 8 键顺序映射, 每键检测到物理输入后显示结果 0.5s.
     * 流程: gamepad_act_keymap 启动 → menu_poll_gamepad_mapping 每帧轮询 → 8 键完成保存退出.
     * BACK 键可随时取消 (不保存当前进度). */
    int              key_mapping_idx;       /* 0..7=当前映射功能, -1=未激活 */
    bool             key_mapping_phase;     /* false=等待按键, true=显示已映射结果 */
    uint32_t         key_mapping_until_ms;  /* 显示已映射结果的到期时间戳 */
    /* 补充按键映射小弹窗 (draw_notice_popup 样式):
     * sup_map_idx >= 0 时激活, 4 个功能(F/G/Shift/空格 → 功能1-4)顺序映射.
     * 流程: gamepad_act_sup_keymap 启动 → menu_poll_sup_mapping 每帧轮询:
     *   捕获阶段: 消费一个物理边沿 (已占用=8键映射或其它补充键 的按键无效);
     *   确认阶段: 按 F_CONFIRM(确定) 写入并进入下一功能, 按 F_BACK(返回) 跳过当前;
     *   4 个完成后自动保存退出. 与 8 键映射独立 (取消方式不同: 8 键 BACK 取消, 4 键 BACK 跳过). */
    int              sup_map_idx;         /* 0..3=当前功能, -1=未激活 */
    bool             sup_map_captured;    /* 当前功能是否已捕获物理按键 */
    phys_t           sup_map_pending;     /* 已捕获待确认的物理按键 (PHYS_MAX=无) */
    bool             sup_confirm_prev;    /* F_CONFIRM 边沿检测 */
    bool             sup_back_prev;       /* F_BACK 边沿检测 */
    /* GB 辅助按键映射小弹窗 (draw_notice_popup 样式):
     * gb_aux_prompt=true 时显示"映射辅助键"提示, 按 F_CONFIRM 进入映射, F_BACK 取消;
     * gb_map_idx >= 0 时激活, 2 个功能(SELECT/START)顺序映射, 流程同补充按键
     * (捕获 → 确定(F_CONFIRM)/跳过(F_BACK)), 完成后自动保存退出.
     * 只用于 GB 游戏二级菜单及游戏 (input.c 的 GB joypad 投递). */
    bool             gb_aux_prompt;       /* true=显示"映射辅助键"提示, 等待确认 */
    int              gb_map_idx;          /* 0=SELECT, 1=START, -1=未激活 */
    bool             gb_map_captured;     /* 当前功能是否已捕获物理按键 */
    phys_t           gb_map_pending;      /* 已捕获待确认的物理按键 (PHYS_MAX=无) */
    bool             gb_confirm_prev;     /* F_CONFIRM 边沿检测 */
    bool             gb_back_prev;        /* F_BACK 边沿检测 */
    /* CD 动画: 上一帧角度 (暂停时保持) */
    int              last_cd_angle;
    /* === 电子词典游戏: 左右分栏布局 ===
     * 左边 100px 宽, 显示文件夹列表 (不含"全部", 直接是子文件夹); 右边 280px, 显示选中文件夹下的游戏
     * 方向键左右切换分栏焦点, 上下在当前分栏内选择 */
    int              select_focus;          /* 0=左文件夹, 1=右游戏 */
    int              select_folder_idx;     /* 左边选中的索引: 0=游戏设置, 1=收藏(默认), 2..N+1=真实子文件夹 (idx-2) */
    int              select_folder_scroll;  /* 左边滚动偏移 */
    int              select_game_idx;       /* 右边选中的游戏索引 (-1=无游戏) */
    int              select_game_scroll;    /* 右边滚动偏移 */
    bool             select_loaded;         /* 一次会话内是否已扫描过文件夹 (避免每帧重扫) */
    /* === 游戏页面模式 (0=文曲星 BBK, 1=GB) ===
     * GB 复用 SELECT_GAME 的分栏布局, 通过此字段区分扫描目录和启动模拟器. */
    int              select_mode;           /* 0=BBK, 1=GB */
    /* V1.0.47: GB 游戏页内的引擎 (GB/GBC/NES/arduboy 共用 MENU_PAGE_GB_GAME 分栏页).
     * 决定扫描 .gb/.gbc 扩展名、启动对应模拟器、以及游戏设置里的音量引擎. */
    int              select_engine;         /* 0=GB, 1=GBC */
    /* === 游戏设置子项 (持久化到 NVS) === */
    uint8_t          game_display_mode;     /* 游戏显示模式: 0=点对点, 1=全屏, 2=强制拉伸全屏 */
    bool             game_show_statusbar;   /* 状态栏显示: 在游戏界面是否显示顶部状态栏 */
    bool             game_key_sound;        /* BBK 按键音效: true=开, false=关 */
    bool             game_virtual_keys;     /* V1.0.68: 游戏内屏幕虚拟按键 (开/关) */
    /* V1.0.46: 画面优化选项 (选项已从菜单移除, 字段保留固定为 0=关) */
    int              game_gray_mode;        /* 模拟灰度模式: 0=纯黑白, 1=4级点聚, 2=5级点聚 */
    int              game_pic_opt;          /* 画面优化: 0=关, 1=标准圆角, 2=深度灰度模拟 (默认关) */
    /* === 多功能键 (短按) 收藏检测 ===
     * 用户需求: 游戏菜单选中游戏 -> 按下多功能键 -> 添加到收藏栏, 弹 0.5s 提示.
     * 通过 上升沿 (cur_held && !prev_held) 触发, 避免长按/按住重复触发.
     * 物理 KEY 按钮 (GPIO18) 复用为多功能键的兜底, 也走上升沿. */
    bool             space_prev_held;       /* 多功能键上一帧是否被按住 */
    uint32_t         space_cooldown_until_ms; /* 触发后的冷却到期时间戳, 0=无冷却 */
    /* === 电子书页面 (复用 select_* 分栏状态, 独立缓存开关) ===
     * 左栏: 0=设置, 1=收藏书架, 2..N=分类目录, 最后=临时目录(根目录书籍)
     * 设置: 敲击翻页 / 敲击灵敏度 / 夜间模式 / 显示页码 */
    bool             book_loaded;           /* 书单是否已扫描 (一次会话内) */
    bool             book_knock;            /* 敲击翻页开关 */
    uint8_t          book_sens;             /* 敲击灵敏度: 0=低, 1=中, 2=高 */
    bool             book_night;            /* 夜间模式 (反色) */
    bool             book_pagenum;          /* 显示页码 */
    uint8_t          book_rot;              /* 旋转方向: 0=上, 1=下, 2=左, 3=右 */
    uint8_t          book_fontsize;         /* 0=20 1=24 2=28 3=32 */
    uint8_t          book_font_family;      /* 0=黑体 1=宋体 (阅读字体) */
    uint8_t          book_margin;           /* 0=窄 1=中 2=宽 */
    uint8_t          book_lineh;            /* 0=紧凑 1=标准 2=宽松 */
    uint8_t          book_gap;              /* 0=标准 1=宽松 */
    /* === V1.0.64: 壁纸设置 === */
    uint8_t          wallpaper_mode;        /* 0=内置星空 1=TF动态图 2=游戏壁纸 */
    uint8_t          wallpaper_program;     /* 内置壁纸程序 0..10 (V1.0.64) */
    uint8_t          wallpaper_timeout_min; /* 休眠时间: 1..30 分钟, 默认 3 */
    uint8_t          wallpaper_bmp_fps;     /* TF动态图速度: 0=慢 1=标准 2=快 */
    uint8_t          wallpaper_game_rot;    /* 游戏壁纸轮换序号 (运行时) */
    /* === V1.0.64: 番茄钟 === */
    uint8_t          pomo_work_min;         /* 工作分钟 1..120 */
    uint8_t          pomo_rest_min;         /* 休息分钟 1..60 */
    bool             pomo_reminder;         /* 完成提醒声音 开/关 */
    /* === 连接记录 (蓝牙设备历史列表) ===
     * 用户需求: 手柄弹窗 -> 连接记录, 列表展示已连接设备, 选中后弹出删除确认.
     * 与 select_game 不同: 不分栏, 是一页全屏列表 + 底部状态栏; 选中按确认进入删除. */
    bool             bt_history_active;     /* 连接记录列表是否激活 */
    int              bt_history_idx;        /* 当前选中的设备 idx (0..count-1) */
    int              bt_history_scroll;     /* 滚动偏移 */
    /* V1.0.46: Wi-Fi 虚拟键盘 (输入 SSID/密码) */
    bool             wifi_kb_active;        /* 虚拟键盘激活 */
    bool             sponsor_active;      /* V1.0.46: 显示赞助作者图片 (全屏 1:1) */
    /* === 隐藏游戏菜单彩蛋 (持久化 NVS key="show_hidden") ===
     * 在"请作者喝杯水"赞助图界面连续按确认键 5 次, 切换隐藏游戏模拟器菜单的显示.
     * 默认隐藏 (false), 解锁后主菜单显示额外游戏模拟器并提示"测试功能, 可能闪退". */
    int              sponsor_confirm_count; /* 赞助图界面连续确认键计数 */
    bool             sponsor_notice_active; /* 赞助图彩蛋提示为模态弹窗, 需确认键退出 */
    bool             show_hidden_menus;     /* true=主菜单显示隐藏游戏模拟器 */
    int              wifi_kb_field;         /* 0=SSID, 1=密码 */
    int              wifi_kb_cur;           /* 字符表当前索引 */
    bool             wifi_kb_shift;         /* V1.0.67: 大写锁定 (Shift 切换) */
    bool             wifi_kb_sym;           /* V1.0.67: 符号键盘层 (Sym 切换) */
    char             wifi_kb_ssid[33];
    char             wifi_kb_pass[65];
    char             wifi_kb_msg[64];       /* 状态/提示消息 */
    /* 列表弹窗: 方向键自定义回调 (默认 NULL=走通用上下选择 wrap).
     * 用户需求: 时间弹窗里 UP/DOWN 用于调值, LEFT/RIGHT 用于切换字段.
     * 实现: list_dialog 收到 UP/DOWN/LEFT/RIGHT 时, 如果 on_key!=NULL, 调它;
     *       回调中可以修改 selected 或对应字段, 自己控制 UI 更新. */
    void             (*list_dialog_on_key)(struct menu_state_s *state, int idx, menu_action_t action);
    /* 列表弹窗: 关闭后回调 (用户 BACK 或选"返回"后调, 用于打开下一层弹窗) */
    void             (*list_dialog_on_close)(struct menu_state_s *state);
    /* 列表弹窗: 自定义渲染回调 (默认 NULL=走通用行渲染).
     * 用户需求: 时间设置弹窗需要在单行内横向显示 "年 月 日 时:分:秒",
     * 通用行渲染不支持单行多字段, 故引入此回调.
     * 回调负责: 渲染弹窗内容 (不画外框/不画背景, 由 draw_list_dialog 处理).
     * 设置 list_dialog_count=1 配合单行高亮, 实现整行只显示一行内容. */
    void             (*list_dialog_on_render)(struct menu_state_s *state, st7305_handle_t *lcd,
                                             int content_x, int content_y, int content_w, int content_h);
    /* 列表弹窗: 内容脏标志 (true 时强制全量重绘, 用于"同一选中但内容变化"场景, 如时间调值) */
    bool             list_dialog_content_dirty;
    /* 列表弹窗嵌套栈: 打开子弹窗(时间/音量/系统信息)时压入父弹窗状态,
     * 子弹窗关闭后恢复父弹窗(含原选中位置), 实现"弹窗返回上级保持原位置". */
    list_dialog_saved_t  list_dialog_stack[LIST_DIALOG_STACK_DEPTH];
    int                  list_dialog_stack_top;  /* 0=无父弹窗 */
} menu_state_t;

void menu_init(menu_state_t *state, st7305_handle_t *lcd);
void menu_handle_action(menu_state_t *state, menu_action_t action);
/* V1.0.65: 触摸点击 hit-test (点哪进哪). 仅在主菜单/列表弹窗两种干净状态下介入:
 * 命中目标后移动选中并返回 true (main.c 随后调用 menu_handle_action(CONFIRM) 复用进入逻辑);
 * 未命中/不适用返回 false (main.c 回退为普通 CONFIRM). */
bool menu_handle_touch(menu_state_t *state, int x, int y);

/* V1.0.66: 主菜单拖动松手吸附. steps 为选中项变化量(正=右移选中, 负=左移选中),
 * 带循环 wrap; 触发 cover-flow 动画. 拖动残留 main_drag_offset 由 render 动画期间衰减. */
void menu_drag_settle(menu_state_t *state, int steps);
/* V1.0.68: 游戏二级菜单右栏拖动松手吸附 (steps 正=选中下移, 负=上移) */
void menu_select_game_settle(menu_state_t *state, int steps);
/* V1.0.68: 游戏列表拖动/按下实时选中: 选中手指所在行的游戏 (ty=手指物理y, off=拖动偏移) */
void menu_select_game_touch_track(menu_state_t *state, int ty, int off);
/* V1.0.68: 触摸长按 (游戏列表右栏) → 选中该行并加入/取消收藏 */
void menu_touch_long_press(menu_state_t *state, int x, int y);
/* V1.0.68: 列表弹窗跟手拖动: 选中手指所在行 (番茄钟时间列表等) */
void menu_list_dialog_touch_track(menu_state_t *state, int ty);
/* V1.0.68: 列表弹窗松手: 固定内容位置 (scroll 补偿偏移, 不回弹) */
void menu_list_dialog_release(menu_state_t *state);
/* V1.0.68: 是否有模态弹窗/全屏覆盖激活 (弹窗期间禁止背景拖动等) */
bool menu_modal_active(const menu_state_t *state);
/* V1.0.68: 手动锁屏 (点按软关机键): 下一次 check_and_render 立即进入屏保 */
void menu_screensaver_activate(void);
void menu_check_dialog_timeout(void);
/* 后台轮询: 蓝牙开启后, 等 HID Host 就绪自动启动"主动连接"扫描 */
void menu_poll_bt_auto_connect(menu_state_t *state);
/* 把 g_menu.settings.volume 同步到 audio_player.
 * 必须在 audio_player_init 完成后调用 (例如 main.c 初始化末尾). */
void menu_apply_volume_setting(void);
void menu_render(menu_state_t *state);
/* 每帧轮询: 检测 KEY 长按 -> 收藏/取消收藏. 必须在 menu_handle_action 后调用. */
void menu_poll_long_press(menu_state_t *state);
/* 按键映射模式下每帧调用: 捕获手柄按下的键并推进映射进度 (需在 menu_render 前调用) */
void menu_poll_gamepad_mapping(menu_state_t *state);
/* 补充按键映射模式下每帧调用: 捕获物理键 + 处理确定(下一键)/返回(跳过) (需在 menu_render 前调用) */
void menu_poll_sup_mapping(menu_state_t *state);
/* GB 辅助按键映射模式下每帧调用: 处理"映射辅助键"提示确认 + 捕获/确定/跳过 (需在 menu_render 前调用) */
void menu_poll_gb_auxmap(menu_state_t *state);

/* 绘制状态栏 (可复用: 菜单和游戏全屏模式共用)
 * center_text: 状态栏中间显示的文本 (NULL=不显示) */
void menu_draw_status_bar(st7305_handle_t *lcd, const menu_settings_t *settings,
                          const char *center_text);

/* 获取当前时间字符串 "HH:MM" (基于 RTC) */
void menu_get_time_str(char *buf, int bufsize);

/* 设置 RTC 时间 */
void menu_set_time(int hour, int minute, int second);

/* 屏保: 检查并渲染 (返回 true=屏保激活中) */
bool screensaver_check_and_render(st7305_handle_t *lcd, bool has_input);

/* 壁纸类型 (与 screensaver/wallpaper 共用, 供外部模块引用) */
#define WALLPAPER_MODE_STARS 0
#define WALLPAPER_MODE_BMP   1
#define WALLPAPER_MODE_GAME  2

/* 立即进入壁纸 (测试/自测用, 与"壁纸设置->测试壁纸"等效) */
void menu_screensaver_enter_test(void);

/* 当前壁纸是否激活 (自测用) */
bool menu_screensaver_is_active(void);
void screensaver_reset(void);
bool screensaver_is_active(void);

/* 判断菜单是否停在"主菜单桌面"页面 (MENU_PAGE_MAIN).
 * 用于: 蓝牙自动重连、屏保等只希望在桌面后台运行的逻辑. */
bool menu_is_current_page_main(void);

/* === 通用 list_dialog 弹窗接口 (供其它组件调用, 内部封装 static list_dialog_open) ===
 * 用途: file_browser.c 等需要从外部打开列表弹窗的模块.
 * 参数: state - 菜单状态; title - 弹窗标题; count - 项数; on_select - 选中回调 (idx=count-1 时不会调用, 走通用关闭).
 * 注意: 内部会自动重置 prev 状态机, 防止外框/标题被局部刷新吞掉. */
void menu_open_list_dialog(menu_state_t *state, const char *title,
                           int count, void (*on_select)(menu_state_t *, int));

/* 重开 SD 管理 list_dialog 弹窗 (浏览文件/挂载/格式化 关闭后通过 on_close 调用,
 * 回到上一步的 SD 管理弹窗, 符合用户"按返回键停留到上一步"需求). */
void menu_open_sd_dialog(menu_state_t *state);

/* 绘制紧凑提示小弹窗 (与"收藏提示"同款: 3px 边框, 24px 字体, 屏幕居中).
 * 供其它组件(如 gam4980 电子词典退出确认)复用, 保证与收藏提示外观完全一致. */
void menu_draw_notice_popup(st7305_handle_t *lcd, const char *text);

/* V1.0.68: 软关机流程 (显示"正在关机" → 清屏 → deep sleep, 不返回) */
void menu_soft_power_off(st7305_handle_t *lcd);

#ifdef __cplusplus
}
#endif

#endif
