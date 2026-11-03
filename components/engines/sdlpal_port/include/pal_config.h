/* pal_config.h — SDLPal 的 ESP32-S3 (LinTOS) 平台配置
 *
 * 参考 3ds/pal_config.h（同为小内存 + 自绘平台）。
 * 关键取舍：
 *   · SDL 1.2 垫片（不是 SDL2）：只要 Surface 像素指针，正好接我们自己的
 *     8bpp 调色板 -> 1bpp 单色屏转换；SDL2 那条路要 Renderer/Texture/GL，我们没 GPU。
 *   · 关掉 MP3/OGG/OPUS/CD/AVI/GLSL：省 flash 也省 RAM，仙剑 DOS 版用不到。
 *   · MIDI 暂不实现（SDLPal 官方也只支持 Win/Android/iOS/macOS + Linux(timidity)）。
 *   · 屏幕用 320x200 原生分辨率，由 pal_port.c 按"百分比"放大到 400x300 上屏。
 */
#ifndef PAL_CONFIG_H
#define PAL_CONFIG_H

/* ---- 文件路径（全部落在 TF 卡）----
 * ★★★ 2026-10-25 修「打开仙剑没反应」的根因:
 *   目录早就搬到 /sdcard/apps/app/pal (商店资源包解到这里), 但这几个宏还写着
 *   旧的 /sdcard/pal/ ⇒ 引擎去找 /sdcard/pal/fbp.mkf → ENOENT → TerminateOnError
 *   → abort, 表现为"点一下震一下, 然后什么都没发生"。
 *   ⚠ 改目录只改 PAL_DATA_DIR 一处; pal_entry.c 的 s_datadir 也从这里取, 不再各写一份。 */
#define PAL_DATA_DIR          "/sdcard/apps/app/pal"
#define PAL_PREFIX            PAL_DATA_DIR "/"
#define PAL_SAVE_PREFIX       PAL_DATA_DIR "/save/"
#define PAL_CONFIG_PREFIX     PAL_DATA_DIR "/"
#define PAL_SCREENSHOT_PREFIX PAL_DATA_DIR "/"

/* ---- 两种资源装法（2026-11-01）----
 * 一套固件要同时支持"ZIP 解压目录"和"单个 .lpk 免解压"，而上面这些宏是**编译期**
 * 的、只能有一个值，所以运行时的最终路径由 pal_platform.c 的 UTIL_Platform_Init()
 * 在 PAL_LoadConfig() 之后改写 gConfig（那是引擎自己的路径真相源）：
 *
 *   ZIP 模式：pszGamePath = PAL_PREFIX（原样，历史行为不变）
 *   LPK 模式：pszGamePath = PAL_LPK_GAMEPATH（= "/lpk"，VFS 挂载点）
 *             其余（存档/配置/截图）一律落在 PAL_DATA_DIR —— **真实 FAT**。
 *
 * 存档为什么不走 /lpk：写进只读包是"假成功"，比失败更糟。即使走了，
 * lpk_mount 的可写旁路也会把它重定向到真实 FAT（见 lpk_mount.h 三条规则）。
 * 判定 LPK / ZIP 的唯一入口是 lpk_mode_decide()，不看临时文件猜。 */
#define PAL_LPK_GAMEPATH      "/lpk"
#define PAL_LPK_FILE          PAL_DATA_DIR "/pal.lpk"

/* ★ 2026-11-01：把 third_party/util.c 里那份"弱默认"的 UTIL_Platform_Init 等
 * 四个函数换成我们自己的实现（pal_platform.c）——
 * 这是**不改 third_party** 的官方口子：util.c 用 `#if !defined(...)` 挡住默认实现，
 * 我们只在自己的头里定义这个宏。改 third_party 会让上游升级变成手工合并地狱。 */
#define PAL_HAS_PLATFORM_SPECIFIC_UTILS 1

/* ---- 音频 ---- */
#define PAL_AUDIO_DEFAULT_BUFFER_SIZE  1024   /* 帧数; 我们后端再喂给 audio_player */
#define PAL_HAS_MP3            0
#define PAL_HAS_OGG            0
#define PAL_HAS_OPUS           0
#define PAL_HAS_SDLCD          0
#define PAL_HAS_NATIVEMIDI     0
/* MIDI 先关: 后端没有软音源; 后续可接 adplug(RIX) 或简化 FM */
#define PAL_HAS_TIMIDITY       0

/* ---- 输入 ---- */
#define PAL_HAS_JOYSTICKS      0
/* ⛔ 触摸先关：SDLPal 的触摸路径走 SDL2 的 SDL_FINGERDOWN/tfinger，
 *    SDL 1.2 没有这套事件；仙剑原本就是键盘/鼠标操作，先用按键玩。
 *    以后要加触摸，得在垫片里把触摸点翻译成"鼠标按下+移动"，不要开这个宏。 */
#define PAL_HAS_TOUCH          0

/* ---- 视频 ---- */
#define PAL_DEFAULT_WINDOW_WIDTH   320
#define PAL_DEFAULT_WINDOW_HEIGHT  200
#define PAL_VIDEO_INIT_FLAGS       0     /* SDL_SWSURFACE == 0, 不需要窗口标志 */
#define PAL_SDL_INIT_FLAGS         0     /* 由垫片自己决定初始化什么 */
/* 放大由 pal_port.c 做（百分比），SDLPal 自己别再缩放 */
#define PAL_SCALE_SCREEN       FALSE
#define PAL_FORCE_UPDATE_ON_PALETTE_SET

/* ---- 其它 ---- */
#define PAL_PLATFORM           "ESP32-S3 / LinTOS"
#define PAL_CREDIT             "LinTOS port"
#define PAL_PORTYEAR           "2026"
/* ★★★ 2026-10-22 关键：
 *   SDLPal 用 `PAL_LARGE` 标记"大的函数内数组"（原意是避免爆栈），在桌面平台上
 *   它就是 `static` —— 结果这些几万到几十万字节的缓冲全进了 .bss。
 *   本项目实测：ending.c 256KB + uigame.c 205KB + resampler.c 147KB + text.c 67KB
 *   + battle.c 66KB ≈ 780KB，而 ESP32-S3 的 dram0_0_seg 只有 512KB，
 *   链接直接报 "region dram0_0_seg overflowed by 712608 bytes"。
 *   ⇒ 把它定义成 EXT_RAM_BSS_ATTR static，这些缓冲自动落到 PSRAM 的 .ext_ram.bss。
 *     需要组件里的 linker.lf 把 * (extram_bss) 收进 PSRAM 段。 */
#include "esp_attr.h"
#define PAL_LARGE              EXT_RAM_BSS_ATTR static
#define PAL_FILESYSTEM_IGNORE_CASE 1         /* FAT 不区分大小写 */

#endif /* PAL_CONFIG_H */
