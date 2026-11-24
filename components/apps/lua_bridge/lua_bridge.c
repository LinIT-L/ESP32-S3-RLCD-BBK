/* lua_bridge.c — 桥总调度: 只有一张模块表, 加模块 = 加一行。 */
#include "lua_bridge.h"

/* ⚠ const 要放在 * 右边 (指向函数的指针数组本身只读);
 *   写成 `const void (*k_mods[])()` 时 const 修饰的是返回值 → -Wignored-qualifiers。 */
static void (*const k_mods[])(lua_State *) = {
    lua_mod_consts_register,
    lua_mod_gfx_register,
    lua_mod_input_register,
    lua_mod_sys_register,
    lua_mod_fs_register,
    lua_mod_store_register,
    lua_mod_net_register,
    lua_mod_json_register,
    lua_mod_wifi_register,
    lua_mod_hid_register,
    lua_mod_kbd_register,
    lua_mod_img_register,
    lua_mod_pkg_register,
    lua_mod_gps_register,
    lua_mod_espnow_register,
    lua_mod_wiki_register,
    lua_mod_util_register,
    lua_mod_rec_register,
    lua_mod_haptic_register,   /* 2026-11-24: app.haptic (触觉效果库) */
};

/* ==================== 应用退出时的跨应用状态回收 ====================
 * ★ 2026-11-22 (第2回合) 新增。原状况: 桥里几个模块把自己当成"进程内单例", 持有的
 *   **C 侧状态不跟 lua_State 一起死** —— VM 关了, 这些还在:
 *     · luat_lib_img : s_img[] 里最多 10 张已解码位图 (地图单张 ~200KB, 全在 PSRAM)。
 *                      脚本不调 app.img.freeall() 就永远不还; 下一个应用 load 时只在
 *                      "槽全满"时顶掉 0 号槽 → 别的槽一直占着。这是本回合发现的
 *                      **最大一笔跨应用 PSRAM 泄漏**。
 *     · luat_lib_sys : app.dlg.result() 的静态结果。应用 A 弹窗后退出, 应用 B 第一次
 *                      result() 会读到 A 的旧结果 (看起来像"莫名自己点了确定")。
 *     · luat_lib_input: app.key_repeat() 的按下时刻表。同上, 新应用第一次按键会被当成
 *                      "早已按住" → 首帧不出边沿。
 *     · luat_lib_util : after/every 的 registry 引用。虽然是 luaL_ref, VM 销毁后即失效,
 *                      但槽位标志必须清掉, 否则新 VM 里会对垃圾 ref 做 pcall。
 *   规矩: 模块若持有**跨应用**状态, 就在这里加一行; 只持有当前 VM 状态的不用加。
 *   page_lua.c 在每次 lua_close() 前统一调本函数 (见 lua_vm_close)。 */
static void (*const k_app_resets[])(void) = {
    lua_img_reset_all,      /* luat_lib_img.c   : 释放全部已加载位图 */
    lua_pkg_reset_all,      /* luat_lib_pkg.c   : 关闭单文件包句柄 */
    lua_util_reset,         /* luat_lib_util.c  : 清定时器槽 */
    lua_sys_reset_app,      /* luat_lib_sys.c   : 清 app.dlg 结果 */
    lua_input_reset_app,    /* luat_lib_input.c : 清按键重复状态 */
    lua_net_reset_app,      /* luat_lib_net.c   : 释放 128KB 非流式 fetch 缓冲 */
    lua_gfx_reset_rotation, /* luat_lib_gfx.c   : ★ 2026-11-23 回横屏 + 释放竖屏中转缓冲 */
    lua_haptic_reset_app,   /* luat_lib_haptic.c: 停播触觉效果 + 清冷却表 */
};

void lua_bridge_reset_app_state(void)
{
    for (unsigned i = 0; i < sizeof(k_app_resets) / sizeof(k_app_resets[0]); i++)
        k_app_resets[i]();
}

void lua_bridge_setup(lua_State *L) {
    lua_newtable(L);                       /* app */
    for (unsigned i = 0; i < sizeof(k_mods) / sizeof(k_mods[0]); i++) k_mods[i](L);
    lua_setglobal(L, "app");
}
