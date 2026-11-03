/* pal_entry.c — 终端调用的 SDLPal 入口（把游戏塞进 LinTOS 的一个任务里）
 *
 * 为什么不直接调 main()：
 *   SDLPal 的 main() 是"跑完整个游戏才返回"的阻塞函数（内部 setjmp/longjmp 管退出），
 *   直接在终端页面的按键回调里调会把 UI 卡死。
 *   所以这里起一个独立任务跑它，终端页面在 poll 里调 pal_tick() 上屏。
 *
 * 给终端用的接口：
 *   pal_launch()    —— 敲 `pal` 时调用：起任务
 *   pal_tick(lcd)   —— 终端 poll 每帧调用：把最新画面转成 1bpp 上屏
 *   pal_is_running()/pal_stop() —— 判断状态 / 请求退出
 */
#include "st7305.h"
#include "pal_entry.h"
#include "input.h"      /* ★ 手柄/虚拟按键 joypad 状态 */
#include "pal_config.h"   /* ★ 数据目录单一真相源 (PAL_DATA_DIR) */
#include "pal_port.h"
#include "esp_timer.h"
#include "virtual_keys.h"  /* 系统虚拟按键 */
#include "SDL.h"          /* pal_stop 要往事件队列塞 SDL_QUIT */

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* ★ vTaskDeleteWithCaps: 与 xTaskCreateWithCaps 配对 */

#include "lpk_mode.h"     /* 资源模式判定（唯一入口 lpk_mode_decide） */
#include "lpk_vfs.h"      /* /lpk 的 VFS 挂载（真机入口） */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "PALENTRY";

/* SDLPal 的 main（已用编译宏改名） */
extern int pal_main(int argc, char *argv[]);

static volatile bool s_running = false;
static volatile bool s_stop_req = false;
static int64_t s_launch_us = 0;        /* 本次启动时刻: 载入阶段耗时以它为 0 点 */
static TaskHandle_t  s_task = NULL;

/* ---------------- 资源模式（一次性判定，启动时定死） ----------------
 * ★ 2026-11-01: 仙剑支持两种装法 —— 单文件 .lpk（免解压）/ 原 ZIP 解压目录。
 *   判定在 lpk_mode.c（安装记录 + 自检 + 无记录时目录优先），这里只负责：
 *     ① 判定 → ② LPK 模式挂 /lpk → ③ 把结果给 pal_platform.c 改写 gConfig。
 *   ⛔ 不允许"包里没找到就回退到盘上"：那是资源混用，见 lpk_mount.h 三条规则。 */
static lpk_res_mode_t s_res = LPK_RES_NONE;
static bool           s_res_mounted = false;

/* 可写旁路：这三个路径无论引擎怎么拼，都只落真实 FAT。
 * 现在引擎的存档/配置路径直接就是 PAL_DATA_DIR（不经过 /lpk），
 * 这张表是**兜底**：万一将来某条代码拼出 /lpk/save/...，也不会写进只读包。 */
static const lpk_vfs_wr_t PAL_WRITABLE[] = {
    { "save/",       true  },
    { "screenshot/", true  },
    { "sdlpal.cfg",  false },
};

static void make_lpk_path(char *out, size_t cap) {
    snprintf(out, cap, "%s/pal.lpk", pal_entry_datadir());
}

pal_res_mode_t pal_res_mode(void) { return (pal_res_mode_t)s_res; }
const char *pal_res_gamepath(void) {
    return (s_res == LPK_RES_LPK) ? LPK_VFS_MNT : PAL_DATA_DIR;
}
bool pal_res_mounted(void) { return s_res_mounted; }

const char *pal_res_mode_str(pal_res_mode_t m) { return lpk_mode_str((lpk_res_mode_t)m); }

/* 只判定不挂载。索引表按头里的 count 分配（PSRAM 优先），判完即放。 */
pal_res_mode_t pal_res_probe(char *why, size_t cap) {
    char local[128];
    if (!why) { why = local; cap = sizeof(local); }
    if (cap) why[0] = 0;

    const char *dir = pal_entry_datadir();
    char lpkp[200];
    make_lpk_path(lpkp, sizeof(lpkp));

    lpk_entry_t *tab = NULL;
    int max = 0;
    lpk_t h;
    if (lpk_open(&h, lpkp, NULL, 0) == LPK_OK) {      /* 只读 64 B 头，拿条数 */
        max = (int)h.count;
        lpk_close(&h);
        if (max > 0) {
            size_t bytes = (size_t)max * sizeof(lpk_entry_t);
            tab = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!tab) tab = malloc(bytes);
            if (!tab) max = 0;                        /* 分配失败就让判定层自己再试一次 */
        }
    }
    lpk_res_mode_t m = lpk_mode_decide(dir, tab, max, why, cap);
    if (tab) free(tab);
    return (pal_res_mode_t)m;
}

/* 启动前准备：判定 + 需要时挂载。返回 false = 不能启动。 */
static bool res_prepare(void) {
    if (s_res_mounted) {                              /* 上一次没收拾干净 */
        lpk_vfs_unmount();
        s_res_mounted = false;
    }

    const char *dir = pal_entry_datadir();
    struct stat st;
    if (stat(dir, &st) != 0) {                        /* 目录是两种模式的共同根，必须先有 */
        ESP_LOGE(TAG, "数据目录不存在: %s", dir);
        return false;
    }

    char why[128] = {0};
    s_res = (lpk_res_mode_t)pal_res_probe(why, sizeof(why));
    ESP_LOGW(TAG, "资源判定: %s —— %s", lpk_mode_str(s_res), why);
    if (s_res == LPK_RES_NONE) return false;

    if (s_res == LPK_RES_LPK) {
        char lpkp[200];
        make_lpk_path(lpkp, sizeof(lpkp));
        esp_err_t e = lpk_vfs_mount(lpkp, dir, PAL_WRITABLE,
                                    (int)(sizeof(PAL_WRITABLE) / sizeof(PAL_WRITABLE[0])));
        if (e == ESP_OK) {
            s_res_mounted = true;
            ESP_LOGW(TAG, "LPK 模式: %s → %s（%d 条资源，免解压）",
                     lpkp, LPK_VFS_MNT, lpk_vfs_file_count());
        } else {
            /* 挂载失败绝不能"假装成功"继续跑 —— 引擎会一路 ENOENT 然后 abort。
             * 如果目录模式还完整，就老实回落到目录模式（存档目录本来就在那儿）。 */
            ESP_LOGE(TAG, "挂载 %s 失败: %s", lpkp, esp_err_to_name(e));
            char w2[128] = {0};
            if (lpk_mode_dir_ready(dir, w2, sizeof(w2))) {
                ESP_LOGW(TAG, "回落到目录模式（%s）", dir);
                s_res = LPK_RES_DIR;
            } else {
                ESP_LOGE(TAG, "目录模式也不可用: %s", w2);
                return false;
            }
        }
    }
    ESP_LOGW(TAG, "本次 GamePath=%s SavePath=%s",
             (s_res == LPK_RES_LPK) ? LPK_VFS_MNT : PAL_DATA_DIR, PAL_DATA_DIR);
    return true;
}

static void pal_task(void *arg)
{
    (void)arg;
    /* SDLPal 的 main 会 memset/strncpy gExecutablePath，需要 argv[0] 可读 */
    static char a0[] = "pal";
    static char a1[160];
    snprintf(a1, sizeof(a1), "%s", pal_entry_datadir());
    char *argv[2] = { a0, a1 };

    ESP_LOGW(TAG, "SDLPal 启动: datadir=%s", a1);
    int rc = pal_main(2, argv);
    ESP_LOGW(TAG, "SDLPal 退出 rc=%d", rc);

    s_running = false;
    s_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

bool pal_is_running(void) { return s_running; }
int64_t pal_launch_us(void) { return s_launch_us ? s_launch_us : esp_timer_get_time(); }

/* 请求退出：SDLPal 的主循环只认 SDL_QUIT 事件（它自己用 setjmp 收尾），
 * 光置标志位没用，必须往事件队列里塞一个 QUIT。 */
void pal_stop(void)
{
    s_stop_req = true;
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
}

/* ★★★ 2026-10-25 linit「仙剑退出了 好像后台还在运行 请彻底退出」:
 *   pal_stop() 只是往 SDL 事件队列塞一个 QUIT —— 游戏任务要自己跑到检查点才退出。
 *   如果它卡在 SDL 的事件等待/加载里, 任务会一直在后台跑(占 ~3.7MB PSRAM + CPU),
 *   而页面已经把 s_running 置 false ⇒ 表面上"退出了", 实际没退。
 *   这里: 先温和请求退出, 等最多 1.5s; 还活着就 vTaskDelete 强杀, 保证不留后台任务。 */
/* ------------------------------------------------------------------
 * 退出收尾: 必须同时停掉 **游戏任务** 和 **音频任务**
 *   仙剑的音乐是 RIX 音序器, 由 sdl_aud 每 23ms 驱动一次 OPL 合成,
 *   它和 sdlpal 任务是**互相独立**的。只杀游戏任务 ⇒ 画面回桌面了,
 *   但音乐还会一直播(linit 实测: 回桌面后音乐正常播放、游戏也像还在跑)。
 *   所以这里按"停源 → 清缓冲 → 兜底按名字强杀"的顺序做干净。
 * ------------------------------------------------------------------ */
void pal_shutdown(void)
{
    extern void pal_audio_stop(void);
    extern void audio_player_flush_pcm(void);

    pal_audio_stop();                 /* ① 停音序器驱动源(先停它, 否则会继续产出) */
    audio_player_flush_pcm();         /* ② 丢掉环形缓冲里残留的音频块 */

    bool killed = false;
    if (s_task) {                     /* ③ 请游戏任务自己退 */
        pal_stop();
        /* ★★★ 2026-10-25 「退出后整机卡死」根因:
         *   原来只等 250ms 就强杀。如果此刻游戏任务正在 f_open/f_read/f_close 中间,
         *   它持着 FatFs 的卷锁被删掉 ⇒ 锁永远不释放 ⇒ 之后**每一次** SD 访问都要
         *   等满超时才返回。实测症状: /sdcard 列目录 4s 无响应、回桌面时
         *   APPM rebuild 10 条花 9,999,487us(正常 7.8ms)、界面像死机。
         *   SD 单次读最长是 2MB 字库(约 1~2s), 所以等 2s 基本不会走到强杀。 */
        for (int w = 0; w < 2000 && s_task; w += 10) vTaskDelay(pdMS_TO_TICKS(10));
        if (s_task) {
            ESP_LOGW(TAG, "游戏任务 2s 未退 ⇒ 强制结束 (可能正卡在 SD 读)");
            vTaskDeleteWithCaps(s_task);
            s_task = NULL;
            st7305_lock_recover();    /* 锁可能被它带走, 必须复原 */
            killed = true;
        } else {
            ESP_LOGI(TAG, "游戏任务已自行退出");
        }
    }

    /* ④ 兜底: 按名字查残留(音频任务不归 s_task 管, 必须单独查) */
    TaskHandle_t h = xTaskGetHandle("sdl_aud");
    if (h) { ESP_LOGW(TAG, "残留 sdl_aud ⇒ 强制结束"); vTaskDeleteWithCaps(h); killed = true; }
    h = xTaskGetHandle("sdlpal");
    if (h) { ESP_LOGW(TAG, "残留 sdlpal ⇒ 强制结束"); vTaskDeleteWithCaps(h); st7305_lock_recover(); killed = true; }


    /* ④a2 ★★★ 2026-10-25: 强杀过就必须重建文件系统。
     *   被强杀的任务可能正持有 FatFs 卷锁, 之后每次开/读/关文件都会等满超时,
     *   表现就是"退出后整机卡死"(rebuild 10s、列目录无响应)。
     *   必须在 AUDIO_CloseDevice(它会 fclose) **之前**做, 否则那一步就会卡住。
     *   sd_remount 内部 = unmount + sdmmc_host_deinit + mount, 耗时约 200ms。 */
    if (killed) {
        extern esp_err_t sd_remount(void);
        ESP_LOGW(TAG, "强杀过任务 ⇒ 重建 SD 文件系统 (防 FatFs 卷锁泄漏)");
        esp_err_t e = sd_remount();
        ESP_LOGW(TAG, "sd_remount 结果: %s", esp_err_to_name(e));
    }

    /* ③b ★ 2026-11-01 实测修正: 卸载 /lpk **必须放在 sd_remount 之后**。
     *   原来放在强杀之后、remount 之前 —— 结果 unmount 里的 fclose 去拿 FatFs 卷锁，
     *   而锁被刚被 vTaskDelete 的任务带走 ⇒ 卡了整整 **10 秒**才返回（真机日志实测）。
     *   现在先 remount（把卷和锁重置掉），再卸载；此时 fclose 立刻失败返回，
     *   既不会挂住，也不会把"退出后整机卡死"的老病引回来。
     *   ⚠ 不删包、不碰存档：这里只注销挂载点、释放条目表与缓存。 */
    if (s_res_mounted) {
        lpk_vfs_unmount();
        s_res_mounted = false;
        ESP_LOGI(TAG, "已卸载 %s", LPK_VFS_MNT);
    }

    /* ④b ★★★ 2026-10-25 「第二次进游戏没声音」的根因:
     *   SDLPal 的 AUDIO_OpenDevice() 第一句是
     *       if (gAudioDevice.fOpened) return -1;      // 已打开就不再打开
     *   而 gAudioDevice.fOpened 只有 **AUDIO_CloseDevice() 结尾**(audio.c:438) 才置 FALSE。
     *   我们的退出是"强杀任务" —— SDLPal 自己的 PAL_Shutdown→AUDIO_CloseDevice
     *   根本不会跑 ⇒ fOpened 永远是 TRUE ⇒ **第二次进游戏 OpenDevice 直接返回 -1**,
     *   整局没有音频(画面正常、日志也不报错)。
     *   这里显式补一次 AUDIO_CloseDevice():
     *     · 置 fOpened = FALSE（下一次能正常打开）
     *     · Shutdown 掉 pMusPlayer/pSoundPlayer（顺带归还 RIX 从 mus.mkf 解出来的 PSRAM）
     *   ⚠ 必须在任务确认死亡之后调用: 此时没人再碰这些播放器。 */
    {
        extern void AUDIO_CloseDevice(void);
        AUDIO_CloseDevice();
        ESP_LOGI(TAG, "AUDIO_CloseDevice: fOpened 已复位 (下次进游戏音频可重开)");
    }

    /* ⑤ ★★★ 2026-10-25 归还 PSRAM（实测一次进出仙剑永久少 2.59MB 的根因）:
     *   字模表 2,097,152 B + 字宽 64KB (font.c) + 视频缓冲 300KB (sdl_shim.c)。
     *   这三块原来只分配、从不释放 ⇒ 桌面上、其他应用里都白占 2.46MB，
     *   而它们**只有玩游戏时才需要**。这里在任务确认死亡后统一归还。
     *   ⚠ 强杀的分支要等一小会儿：被 vTaskDelete 的任务可能在另一个核上
     *     还跑着最后几条指令，立刻 free 会让它踩空（cache 已失效的内存）。
     *   ⚠ 释放后 PAL_EnsureFontTable()/SDL_SetVideoMode() 会重新分配，
     *     所以下次进游戏照常，只是多一次 SD 读取（约 0.1~0.2s，载入页可见）。 */
    {
        extern void PAL_FreeFontTable(void);
        extern void pal_shim_free_video(void);
        extern void pal_audio_free_buf(void);
        /* 先等 idle 回收被 vTaskDeleteWithCaps 标记的任务栈(16KB+32KB),
         * 再取基线, 这样日志里的差值就是"真正归还给堆"的量。 */
        vTaskDelay(pdMS_TO_TICKS(80));
        uint32_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        PAL_FreeFontTable();
        pal_shim_free_video();
        pal_audio_free_buf();
        ESP_LOGI(TAG, "PSRAM 归还: %u → %u (+%d B)",
                 (unsigned)psram_before,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - psram_before));
    }

    s_running = false;
    ESP_LOGI(TAG, "退出完成: sdlpal/sdl_aud 均已清理");
}

bool pal_stop_and_wait(int timeout_ms)
{
    (void)timeout_ms;          /* 统一走 pal_shutdown 的固定预算(≈400ms 上限) */
    const bool self = (s_task != NULL);
    pal_shutdown();
    return self;
}

/* ★ 2026-10-27 诊断: 仙剑跑一段后堆元数据被写坏(StoreProhibited in TLSF)。
 *   每 2 秒校验一次两个堆区，能在"刚被写坏"的窗口内报出来，
 *   配合堆 canary(CONFIG_HEAP_POISONING_COMPREHENSIVE)定位越界写的来源。 */
static void pal_heap_guard_task(void *arg)
{
    (void)arg;
    int n = 0;
    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_running) break;
        n++;
        ESP_LOGD(TAG, "[HEAP-GUARD] #%d 检查内部堆…", n);
        bool ok_int  = heap_caps_check_integrity(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, true);
        ESP_LOGD(TAG, "[HEAP-GUARD] #%d 检查 PSRAM…", n);
        bool ok_psr  = heap_caps_check_integrity(MALLOC_CAP_SPIRAM, true);
        if (!ok_int || !ok_psr) {
            ESP_LOGE(TAG, "[HEAP-GUARD] 第%d次检查失败: 内部=%s PSRAM=%s  (内部=%u PSRAM=%u)",
                     n, ok_int ? "OK" : "损坏", ok_psr ? "OK" : "损坏",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
    vTaskDelete(NULL);
}

bool pal_launch(void)
{
    /* ★★★ 2026-10-25 修「第二次进游戏没有加载进度条 / 也没声音」:
     *   原来这里只看 s_running。一旦上一轮 pal_shutdown 没走完（或中途被复位打断），
     *   s_running 残留 true 就**直接 return true** —— 页面以为已经在跑:
     *     · 不显示载入进度条（走了"重入"分支）
     *     · 实际没有 sdlpal 任务 ⇒ 没有新一局、也没有音频
     *   现在用任务句柄交叉验证: 任务查不到 ⇒ 状态是脏的 ⇒ 复位后重新启动。 */
    if (s_running) {
        if (xTaskGetHandle("sdlpal")) return true;      /* 真的在跑才算重入 */
        ESP_LOGW(TAG, "s_running 残留但 sdlpal 任务不存在 ⇒ 复位后重新启动");
        s_running = false;
        s_task    = NULL;
    }

    /* 资源准备：判定 LPK / 目录模式；LPK 模式在这里把 /lpk 挂上。
     * ★ 2026-11-01: 原来只 stat 数据目录。现在两种装法的"可用"标准不同，
     *   统一交给 res_prepare()（内部走 lpk_mode_decide，唯一判定点）。
     *   两种都不可用时**不启动** —— 否则引擎 TerminateOnError → abort，
     *   表现是"点一下震一下，然后什么都没发生"，比明确不启动更难查。 */
    if (!res_prepare()) {
        ESP_LOGE(TAG, "资源不可用（LPK 与目录都不完整），放弃启动");
        return false;
    }

    s_stop_req = false;
    s_running  = true;
    s_launch_us = esp_timer_get_time();

    /* 栈必须内部 RAM？不：SDLPal 不碰 flash 写/cache 关断路径，放 PSRAM 省内部 RAM。
     * 但栈上有大量局部变量（PAL_LARGE=static 已经把大数组挪走了），16KB 够。 */
    /* ★ 2026-10-27: 加 DMA 可用的内部内存 —— SD 读卡必须从这里分配 DMA 缓冲,
     *   分配失败会直接让 SDLPal 读资源失败退出(实测 rc=255 闪退)。 */
    ESP_LOGI(TAG, "启动前 内部RAM free=%u largest=%u | DMA可用 free=%u largest=%u | PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* 2026-10-27: 堆校验任务暂时停用 —— 它在堆已损坏时会崩在遍历内部并重启, 冲掉现场。 */
    (void)pal_heap_guard_task;

    if (xTaskCreateWithCaps(pal_task, "sdlpal", 16 * 1024, NULL, 5, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_running = false;
        s_task = NULL;
        ESP_LOGE(TAG, "任务创建失败");
        if (s_res_mounted) { lpk_vfs_unmount(); s_res_mounted = false; }
        return false;
    }
    /* ★ 内存体检: 进游戏 3 秒后再打一次, 差值就是 SDLPal 吃掉的量。
     *   (linit 报"内存 RAM 不够" —— 有这两个数才能判断改配置有没有效果) */
    return true;
}

void pal_tick(void *lcd)
{
    if (!s_running) return;
    pal_port_present(lcd);
}

/* 仙剑单独限帧 (linit 要求: 只限仙剑, 别处不动) */
void pal_set_max_fps(int fps) { pal_port_set_max_fps(fps); }
int  pal_get_max_fps(void)    { return pal_port_get_max_fps(); }

/* 固定数据目录（终端里也可用 set 改，后续接 NVS） */
/* ★ 2026-10-25: 数据目录统一放到 TF 卡的 apps 目录下 (linit 要求: 不要散在卡根)。
 *   /sdcard/apps/app/pal —— 由商店的"资源包"解压过来 (apps/app = 应用分类)。 */
static char s_datadir[96] = PAL_DATA_DIR;   /* 与 pal_config.h 的 PAL_PREFIX 同源, 不会再漂移 */
const char *pal_entry_datadir(void) { return s_datadir; }
void pal_entry_set_datadir(const char *d)
{
    if (d && d[0]) {
        /* 换目录 = 换资源来源，缓存下来的模式立刻作废；
         * 若还挂着旧的 /lpk 必须先卸（同一个挂载点不能挂两次）。 */
        if (s_res_mounted) { lpk_vfs_unmount(); s_res_mounted = false; }
        s_res = LPK_RES_NONE;
        snprintf(s_datadir, sizeof(s_datadir), "%s", d);
    }
}


/* ---------------- 输入：物理按键 / 虚拟手柄 -> SDL 事件 ---------------- */

static const int s_keymap[PAL_KEY_NONE] = {
    SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
    SDLK_RETURN, SDLK_ESCAPE, SDLK_PAGEUP, SDLK_PAGEDOWN,
};

void pal_input(pal_key_t k, int down)
{
    if (!s_running || k < 0 || k >= PAL_KEY_NONE) return;
    SDL_PushKeyEvent(s_keymap[k], down);
}

void pal_keys_enable(bool on)
{
    virtual_keys_set_layout(VK_LAYOUT_STANDARD);
    virtual_keys_set_hide_selsta(false);   /* 仙剑要 Select(翻页) / Start(确认) */
    virtual_keys_set_enabled(on);
}

/* ★★★ 2026-10-25 linit: 仙剑没有虚拟手柄、手柄也按不动。
 *   原因：虚拟按键只往 joypad 位里写字（input_get_held_gb_joypad），
 *   而仙剑只吃 pal_input()。中间没有人做桥接 ⇒ 画出来也按不动。
 *   这里每帧取一次 joypad（低有效），把变化的位转成按下/松开两个事件。
 *   bit: 0=A 1=B 2=Select 3=Start 4=右 5=左 6=上 7=下 */
void pal_poll_keys(void)
{
    if (!s_running) return;

    /* ★ 2026-10-25 linit「游戏运行一会儿虚拟手柄就不见了」的自愈:
     *   不管是别的模块把 virtual_keys 关掉, 还是 fb 被别处整体覆盖,
     *   这里每秒兜底一次 —— 重新拉起虚拟键 + 强制重画一帧。
     *   (只在真的丢了才动手, 不打扰正常渲染) */
    static uint32_t s_last_heal = 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (!virtual_keys_is_enabled()) {
        if (s_last_heal == 0 || (uint32_t)(now_ms - s_last_heal) > 5000) {
            s_last_heal = now_ms;
            ESP_LOGW("PAL", "虚拟手柄被关闭 → 自动重新拉起");
        }
        pal_keys_enable(true);
        pal_port_mark_dirty();
    } else if (s_last_heal && (uint32_t)(now_ms - s_last_heal) > 1000) {
        s_last_heal = 0;
    }
    /* 每秒强制重画: 万一画面被弹窗/切页擦掉, 能自己回来, 不用退出重进 */
    static uint32_t s_last_force = 0;
    if (s_last_force == 0 || (uint32_t)(now_ms - s_last_force) > 1000) {
        s_last_force = now_ms;
        pal_port_mark_dirty();
    }

    static uint8_t prev = 0xFF;
    uint8_t j = input_get_held_gb_joypad();
    uint8_t chg = (uint8_t)(prev ^ j);
    if (!chg) return;

    static const struct { uint8_t bit; pal_key_t k; } M[] = {
        { 1u << 0, PAL_KEY_OK     },   /* A     → 回车/对话 */
        { 1u << 1, PAL_KEY_MENU   },   /* B     → ESC/菜单 */
        { 1u << 2, PAL_KEY_MENU   },   /* Select→ 菜单 (仙剑用得到) */
        { 1u << 3, PAL_KEY_OK     },   /* Start → 确认 */
        { 1u << 4, PAL_KEY_RIGHT  },
        { 1u << 5, PAL_KEY_LEFT   },
        { 1u << 6, PAL_KEY_UP     },
        { 1u << 7, PAL_KEY_DOWN   },
    };
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++)
        if (chg & M[i].bit) pal_input(M[i].k, (j & M[i].bit) == 0);   /* 低有效 */
    prev = j;
    /* ★ 游戏画面静止时(对话/菜单)也要把虚拟键的按下态画出来, 否则"按了没反馈" */
    pal_port_mark_dirty();
}
