/* pal_platform.c — 平台特定的 UTIL_* 实现（替代 third_party/util.c 里的弱默认版）
 *
 * 为什么要抢这四个函数
 * --------------------
 * third_party/util.c 末尾有一段
 *     #if !defined(PAL_HAS_PLATFORM_SPECIFIC_UTILS)
 *         UTIL_GetScreenSize / UTIL_IsAbsolutePath / UTIL_Platform_Init / UTIL_Platform_Quit
 *     #endif
 * 也就是说上游**本来就留了口子**给我们实现，不必去改 third_party（改了以后升级就要手工合并）。
 * 我们在 pal_config.h 里定义 PAL_HAS_PLATFORM_SPECIFIC_UTILS=1，于是默认实现被编译掉，
 * 这四个符号由本文件提供。
 *
 * 时机为什么关键
 * --------------
 * main.c 的顺序是：
 *     SDL_Init → PAL_LoadConfig(TRUE)  →  UTIL_Platform_Init(argc, argv)  →  PAL_Init()
 * PAL_LoadConfig 会把 sdlpal.cfg 里的 GamePath/SavePath 读进 gConfig。
 * 所以**唯一能改路径且改完还算数**的位置就是 UTIL_Platform_Init ——
 * 它之后 PAL_Init 才真正去开 ABC.MKF 等资源。
 *
 * 改写规则（与 pal_config.h 的说明一致）
 * --------------------------------------
 *   LPK 模式：pszGamePath = "/lpk"；pszSavePath = PAL_DATA_DIR（真实 FAT）
 *   ZIP 模式：pszGamePath 保持配置值；但若是 "/lpk"（上一次 LPK 模式写进 cfg 的残留）
 *             则改回 PAL_DATA_DIR —— 否则切成 ZIP 后会去开一个不存在的挂载点。
 * 存档路径里出现 "/lpk" 一律改回 PAL_DATA_DIR，理由同上。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"        /* BOOL / INT / VOID / DWORD / LPCSTR + pal_config.h */
#include "palcfg.h"        /* gConfig */
#include "pal_config.h"
#include "pal_entry.h"     /* pal_res_mode() / pal_res_gamepath() */

#include "esp_log.h"

static const char *TAG = "PALPLAT";

/* 屏幕尺寸：交给我们自己的 pal_port.c 按 320x200 处理，这里沿用默认（返回 FALSE =
 * 让 palcfg 用 PAL_DEFAULT_WINDOW_WIDTH/HEIGHT）。 */
BOOL UTIL_GetScreenSize(DWORD *pdwScreenWidth, DWORD *pdwScreenHeight)
{
    (void)pdwScreenWidth;
    (void)pdwScreenHeight;
    return FALSE;
}

/* 绝对路径判断：**保持上游默认行为（恒 FALSE）**。
 * 上游 UTIL_IsFileExist() 用它决定"拿哪个 basepath 去拼"：
 *     if (UTIL_IsAbsolutePath(path)) 用 ""    else 用 gConfig.pszGamePath
 * 一旦改成"以 / 开头即绝对"，凡是被拼成绝对路径的调用就会绕开 gamePath ——
 * 那属于顺手改行为，本轮不碰。 */
BOOL UTIL_IsAbsolutePath(LPCSTR lpszFileName)
{
    (void)lpszFileName;
    return FALSE;
}

static void set_path(char **slot, const char *v)
{
    if (!slot || !v) return;
    if (*slot && strcmp(*slot, v) == 0) return;      /* 没变就别动，省一次 malloc */
    if (*slot) free(*slot);
    *slot = strdup(v);
}

INT UTIL_Platform_Init(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    gConfig.fLaunchSetting = FALSE;

    const pal_res_mode_t mode = pal_res_mode();
    const char *game = pal_res_gamepath();          /* LPK: "/lpk" ; DIR: PAL_DATA_DIR */

    /* ① 游戏数据路径 */
    if (mode == PAL_RES_LPK) {
        set_path(&gConfig.pszGamePath, game);
        /* 存档必须落真实 FAT：写进只读包是"假成功"。
         * 与 ZIP 模式同一个目录 ⇒ 老存档照旧读得到。 */
        set_path(&gConfig.pszSavePath, PAL_DATA_DIR);
    } else {
        const char *cur = gConfig.pszGamePath;
        if (cur && strncmp(cur, PAL_LPK_GAMEPATH, sizeof(PAL_LPK_GAMEPATH) - 1) == 0) {
            ESP_LOGW(TAG, "配置里的 GamePath=%s 是 LPK 模式的残留 ⇒ 改回 %s", cur, PAL_DATA_DIR);
            set_path(&gConfig.pszGamePath, PAL_DATA_DIR);
        }
        if (gConfig.pszSavePath &&
            strncmp(gConfig.pszSavePath, PAL_LPK_GAMEPATH, sizeof(PAL_LPK_GAMEPATH) - 1) == 0) {
            ESP_LOGW(TAG, "配置里的 SavePath 指向 %s ⇒ 改回 %s", gConfig.pszSavePath, PAL_DATA_DIR);
            set_path(&gConfig.pszSavePath, PAL_DATA_DIR);
        }
    }

    ESP_LOGW(TAG, "资源模式=%s  GamePath=%s  SavePath=%s  Config=%s",
             pal_res_mode_str(mode),
             gConfig.pszGamePath ? gConfig.pszGamePath : "(null)",
             gConfig.pszSavePath ? gConfig.pszSavePath : "(null)",
             PAL_CONFIG_PREFIX);
    return 0;
}

VOID UTIL_Platform_Quit(VOID)
{
}
