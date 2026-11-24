/**
 * @file sd_scan.c
 * @brief TF/SD 卡驱动层: 挂载 / 卸载 / 探测 / 重新挂载 / 格式化 + 挂载状态.
 *
 * 只做"卡在不在、挂没挂上"。**不做目录扫描** —— 需要列文件的页面用 os_pane 或
 * 自己的 opendir 实现, 不要把扫描逻辑再塞回驱动。
 */
#include "sd_scan.h"
#include "sd_arb.h"          /* ★ 2026-11-24: 访问仲裁状态机 (见 sd_arb.h) */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"        /* esp_get_free_heap_size (挂载/格式化日志) */

static const char *TAG = "SDSCAN";

/* ★ 2026-11-22 (第2回合) 本文件范围收敛为「SD 卡的挂载/卸载/探测/格式化」。
 *   原来这里还有一整层目录扫描 API (scan_bbk_games / scan_wqx_games / scan_path /
 *   fb 文件浏览器 / 屏保图片扫描 等, 约 1200 行) —— 全部**已被各页面自己的扫描
 *   (os_pane 或页面内 opendir) 取代, 全项目零调用。它们各自复制了一套
 *   "readdir + DT_UNKNOWN 兜底 + 去后缀 + BOM 清理" 逻辑, 并且 `open_dir_with_remount()`
 *   在**目录本身不存在**时也会整卡卸载重挂 (UI 卡顿 + 状态被重置)。已整体删除;
 *   需要扫描的页面请各自用 os_pane / opendir, 不要再往驱动里加。 */

/* 前向声明 - sd_full_unmount() 在 sd_full_remount() 定义之前调用 */
static void sd_full_unmount(void);
static int  sd_full_remount(uint8_t format_if_needed);

/* SD 卡 SDMMC 引脚 - 来自 ESP32-S3-RLCD-4.2 官方原理图 + 官方 Arduino 示例
 * 重要: SD 卡实际接到 ESP32-S3 的 SDMMC 控制器, 不是 SPI 控制器!
 * 之前误用 SPI 模式是错的, GPIO 17 不是 CS, 是 CD (卡检测) 等其他用途
 *
 * 硬件接法 (SD 卡座 SD1, 1-bit SD 模式):
 *   CLK  - **GPIO13**   (SCK)
 *   CMD  - **GPIO10**   (MOSI)
 *   D0   - **GPIO14**   (MISO)
 *   D1/D2/D3 - NC (1-bit 模式不用)
 *   CD   - 不用 (软件探测)
 *
 * ★ 2026-10-05 小智模块板换脚 (分支 board/xiaozhi-s3-core):
 *   旧三根 21/38/39 在 `ESP32-S3-AI-2E` 模块上分别是 焊盘23 `VBAT` / 模块内部 codec 的
 *   `I2C_SDA` / 焊盘32 `MIC2P` —— **一根都不能用**。新脚 13/10/14 全在 24 个可用 GPIO 内。
 *   ⚠ 尤其 **不能再用 GPIO38**: 它是模块内部 ES8311/ES7210 的 I2C 数据线,
 *     拿 SDMMC 去驱动会把 codec 的控制总线打乱。
 *   ✅ **实测定案**(2026-10-05 真机): 下面这三条是正确的 ——
 *     `CLK=13 CMD=10 D0=14` 一次认到卡, 25MHz 挂载成功, 容量 3.75GiB。
 *     (中途曾按反馈把 CMD/D0 对调成 14/10, 反而认不到; 是排列自检把它们试回来的。)
 */
/* ★★ 2026-10-13 新载板: 旧的 SDMMC 引脚(13/10/14) **与触摸 SCL(13)、LCD MOSI(10) 撞车**,
 *   一初始化就把 LCD/触摸的脚抢走 ⇒ 屏黑 + 触摸读不到 + 卡也认不到(0x107)。
 *   新板 SD 是 **SPI 接法**: MOSI=15 / SCK=16 / MISO=17(linit 提供), CS 待确认。
 *   ⇒ 先整段停用 SD, 让 LCD/触摸恢复; 确认 CS 后再按 SPI 重接。改 1 恢复(需同时改回引脚)。 */
#ifndef SD_SCAN_ENABLE
#define SD_SCAN_ENABLE 1
#endif
/* ★★ 2026-10-13 新载板(网表 SD1 座): 三个脚正好是 SDMMC 1-bit 的 CLK/CMD/D0
 *   SD座 pin5=GPIO16(SCK)→CLK · pin3=GPIO15(MOSI)→CMD · pin7=GPIO17(MISO)→D0
 *   旧值 13/10/14 **与触摸 SCL(13)、LCD MOSI(10) 撞车** ⇒ 曾导致屏黑+触摸失效。
 *   CS(DAT3,pin2) 经 R8 上拉到 3.3V、不接 MCU ⇒ 用 1-bit SD 模式无需 CS ✔ */
#define SD_CLK  GPIO_NUM_16
#define SD_CMD  GPIO_NUM_15
#define SD_D0   GPIO_NUM_17

/* ★ 2026-10-16: SD 引脚按板型表下发 (小智=CLK16/CMD15/D0 17; 微雪=CLK38/CMD21/D0 39)。
 * 驱动不反向依赖上层 ⇒ 由 app_board 调本函数传入。 */
static int s_sd_clk = 16, s_sd_cmd = 15, s_sd_d0 = 17;
/* 本次实际使用的三根脚 (排列自检可能改写) —— ★ 初值必须=默认板型, 不能用 -1:
 *   挂载路径读的是这三个, 给 -1 会直接配不出槽 ⇒ 卡认不到 (2026-10-16 踩过: SD=- 0MB) */
static int s_pin_clk = 16, s_pin_cmd = 15, s_pin_d0 = 17;
void sd_scan_apply_pins(int clk, int cmd, int d0) {
    if (clk >= 0) s_sd_clk = clk;
    if (cmd >= 0) s_sd_cmd = cmd;
    if (d0  >= 0) s_sd_d0  = d0;
    /* 实际使用脚 = 板型下发的脚 (排列自检已删, 见下方说明) */
    s_pin_clk = s_sd_clk; s_pin_cmd = s_sd_cmd; s_pin_d0 = s_sd_d0;
}

/* ★ 2026-11-21: 原来的 s_ok_perm / sd_probe_permutations() 已删除 —— 它硬编码 {15,16,17}
 *   穷举 6 种排列, 与 sd_scan_apply_pins() 下发的板型引脚脱节 (微雪板 38/21/39 时它测的是
 *   三根无关的脚), 而且每次 sd_mount_body() 都跑一遍。引脚顺序已在板型表固化并真机验证,
 *   sd_quick_probe() 内部按**本板型**的三根脚做探测即可, 不再重复穷举。 */
/* CD (卡检测) 不用: 走软件探测 (见 sd_quick_probe) */

static bool s_mounted = false;
static char s_mount_point[16] = "/sdcard";
/* 保存 sdmmc_card_t 句柄, 让 USB MSC 可以直接读写扇区 */
static sdmmc_card_t *s_card = NULL;

/* ================= 「卡已交给 USB MSC」门闸 (2026-11-22) =================
 * 问题: 挂载到电脑时本机 VFS 被注销、s_mounted=false, 但 **s_card 仍然有效**,
 *   且 s_mnt_state 还是 OK。于是 sd_mount() 的两道短路都不生效
 *   (s_mounted 为假 / sd_card_absent() 为假) ⇒ 任何后台扫描或"存储信息"
 *   都会走完整挂载流程, 在 PC 正在读写卡的时候重新 init SDMMC host 并挂 VFS;
 *   更糟的是从存储页进「存储信息 → 格式化」会在 PC 手里整卡擦除。
 * 做法: 交卡/收回卡时维护一个显式所有权标志, sd_mount()/sd_remount()/
 *   sd_get_info()/格式化全部让路 —— 卡在电脑手里时本机只认"不可用", 不去抢。
 * ⚠ 不要在 sd_unmount() 里清它: 那条路径是"本机卸载", 卡并没有交给别人。 */
static volatile bool s_usb_owned = false;

bool sd_usb_owned(void) { return s_usb_owned; }

/* ================= SDMMC host 生命周期 (2026-11-22) =================
 * 问题: 全文件有 22 处 `sdmmc_host_deinit()`, 以前都是无条件调。IDF 的实现是先取
 *   静态 host 指针, 没初始化过就是 NULL ⇒ 打两条 E 级日志:
 *     E SD_HOST: invalid argument: null pointer
 *     E sdmmc_periph: sdmmc_host_deinit(185): failed to delete controller
 *   开机第一次 sd_probe_pins() 正好是"先 deinit 再 init"的写法, 所以**每次上电必现**
 *   一条吓人的错误, 实际无害。这里按"谁 init 谁 deinit"收成一个出口。
 *   ⚠ 只有 sd_scan 会 init 这个 host (USB MSC 走 sd_get_card() 复用手柄), 所以本文件的
 *     状态就是全局真相。 */
static bool s_host_up = false;

static esp_err_t sd_host_acquire(void)
{
    esp_err_t r = sdmmc_host_init();     /* 已初始化时是空操作且返回 OK */
    if (r == ESP_OK) s_host_up = true;
    return r;
}

static void sd_host_release(void)
{
    if (!s_host_up) return;             /* 没 init 过就别去删, 也不会再刷 E 日志 */
    sdmmc_host_deinit();
    s_host_up = false;
}

/* ---- 挂载状态 / 卡名 / 失败原因 (供 UI 显示; 2026-09-29) ----
 * 背景: "换卡后认不到卡"时 UI 只显示"未挂载", 分不清是"没插好"还是"文件系统不对"。
 * 实测判据: 卡协议层正常(无 0x107 超时) + FatFs 报 13(FR_NO_FILESYSTEM)
 *           ⇒ 卡被 macOS 格式化成 exFAT, 而固件 FATFS 不支持 exFAT。 */
static volatile sd_mnt_state_t s_mnt_state = SD_MNT_UNKNOWN;
/* 最近一次挂载的结果. 已确认"没有可用卡"时 sd_mount()/sd_remount() 直接复用这个码, 不再探测 */
static volatile esp_err_t s_last_mount_err = ESP_FAIL;
static char           s_cid_name[12] = "";

const char *sd_get_cid_name(void) { return s_cid_name[0] ? s_cid_name : "-"; }

/* ★ 2026-10-18: 品牌 + 真实容量 (替代"把 CID 型号名当容量显示")。
 *   CID 的 `name[8]` 是**出厂型号名**，不是容量 —— 实测 3834MB 的卡上印的是 "SD16G"，
 *   用户看到就以为容量显示错了。品牌取 CID 的 MID (SD-3C 厂商号)。
 *   MID 表按多家实测统计 (SD-3C 不公开官方表)：Linux `mmc` 打印 / cameramemoryspeed /
 *   bahjeez 微 SD 普查 三处交叉一致。⛔ 常见误传: 0x28 不是 Kingston 而是 **Lexar(旧)**，
 *   Kingston 是 0x41；0x27 是 **Phison 代工**（Lexar/PNY/Verbatim/Delkin 等贴牌）。 */
const char *sd_get_vendor_str(void)
{
    if (!s_card) return "-";
    switch (s_card->cid.mfg_id) {
        case 0x01: return "Panasonic";
        case 0x02: return "Toshiba";     /* 现 Kioxia */
        case 0x03: return "SanDisk";
        case 0x09: return "ATP";
        case 0x1B: return "Samsung";
        case 0x1D: return "ADATA";
        case 0x27: return "Phison";      /* 代工厂: Lexar/PNY/Verbatim/Delkin… */
        case 0x28: return "Lexar";
        case 0x31: return "SiliconPower";
        case 0x41: return "Kingston";
        case 0x6F: return "Netac";       /* 也供 HP/Kodak/Hiksemi 等 */
        case 0x74: return "Transcend";
        case 0xAD: return "Longsys";     /* 新 Lexar / Amazon Basics / RPi */
        case 0xFE: return "无名";        /* 白牌/贴牌常见 */
        default:   return "未知";
    }
}

uint64_t sd_get_size_bytes(void)
{
    if (!s_card) return 0;
    return (uint64_t)s_card->csd.capacity * 512ULL;   /* capacity 单位 = 512B 扇区 */
}
sd_mnt_state_t sd_get_mnt_state(void) { return s_mnt_state; }
const char *sd_get_mnt_state_str(void)
{
    switch (s_mnt_state) {
    case SD_MNT_OK:      return "\xe5\xb7\xb2\xe6\x8c\x82\xe8\xbd\xbd";                 /* 已挂载 */
    case SD_MNT_NO_CARD: return "\xe5\x8d\xa1\xe6\x97\xa0\xe5\x93\x8d\xe5\xba\x94";      /* 卡无响应 */
    case SD_MNT_EXFAT:   return "exFAT \xe4\xb8\x8d\xe6\x94\xaf\xe6\x8c\x81";            /* exFAT 不支持 */
    case SD_MNT_NO_FS:   return "\xe6\x97\xa0\xe6\x96\x87\xe4\xbb\xb6\xe7\xb3\xbb\xe7\xbb\x9f"; /* 无文件系统 */
    default:             return "\xe6\x9c\xaa\xe6\x8c\x82\xe8\xbd\xbd";                  /* 未挂载 */
    }
}

/* 探测"卡在但挂不上"的原因: 直接按 SD 协议初始化卡 + 读 LBA0 判文件系统.
 * 只在 sd_mount 全部尝试失败后调用一次; **只读**, 不会动卡上的数据。 */

static void sd_probe_fail_reason(void)
{
    s_cid_name[0] = '\0';
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 5000;             /* 低速探测, 成功率高 */
    host.flags = SDMMC_HOST_FLAG_1BIT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    /* 用**本次实际使用**的三根脚 (sd_scan_apply_pins 下发的板型值), 与挂载路径保持同一来源 */
    slot.clk = s_pin_clk; slot.cmd = s_pin_cmd; slot.d0 = s_pin_d0;
    slot.d1 = slot.d2 = slot.d3 = -1;
    slot.cd = -1; slot.wp = -1; slot.flags = 0;

    if (sd_host_acquire() != ESP_OK || sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot) != ESP_OK) {
        s_mnt_state = SD_MNT_NO_CARD;
        ESP_LOGD(TAG, "[探测] SDMMC host/slot 初始化失败 → 判定: 卡无响应");
        sd_host_release();
        return;
    }
    sdmmc_card_t *card = heap_caps_malloc(sizeof(sdmmc_card_t), MALLOC_CAP_INTERNAL);
    if (!card) {
        s_mnt_state = SD_MNT_NO_CARD;
        ESP_LOGD(TAG, "[探测] 卡结构体分配失败");
        sd_host_release();
        return;
    }
    if (sdmmc_card_init(&host, card) != ESP_OK) {
        s_mnt_state = SD_MNT_NO_CARD;
        ESP_LOGD(TAG, "[探测] 卡不响应 → 判定: 未插好/接触不良/CMD-CLK-D0 接线");
    } else {
        snprintf(s_cid_name, sizeof(s_cid_name), "%s", card->cid.name);
        uint8_t *buf = heap_caps_malloc(512, MALLOC_CAP_DMA);
        if (buf && sdmmc_read_sectors(card, buf, 0, 1) == ESP_OK) {
            if (memcmp(buf + 3, "EXFAT   ", 8) == 0) {
                s_mnt_state = SD_MNT_EXFAT;
                ESP_LOGD(TAG, "[探测] 卡=%s 是 exFAT (固件 FATFS 不支持) → 请格式化为 FAT32",
                         s_cid_name);
            } else if (buf[510] == 0x55 && buf[511] == 0xAA) {
                s_mnt_state = SD_MNT_NO_FS;
                ESP_LOGD(TAG, "[探测] 卡=%s 有分区表但挂不上 (非 FAT / 损坏)", s_cid_name);
            } else {
                s_mnt_state = SD_MNT_NO_FS;
                ESP_LOGD(TAG, "[探测] 卡=%s 无 FAT 引导记录 → 未格式化或不兼容", s_cid_name);
            }
        } else {
            s_mnt_state = SD_MNT_NO_CARD;
            ESP_LOGD(TAG, "[探测] 卡=%s 就绪但扇区读失败", s_cid_name);
        }
        if (buf) free(buf);
    }
    sd_host_release();
}

/* 重新挂载 (换卡/插卡后不用重启: SD Watcher 的自动补挂是禁用状态) */
esp_err_t sd_remount(void)
{
    /* ★ 2026-10-28: 格式化期间绝不能碰 SDMMC host —— 下面的 sdmmc_host_deinit() 会把
     *   格式化任务正在用的 host 拆掉(整卡擦除中途被拆 ⇒ 卡写坏/认不到)。
     *   sd_mount() 里也有一道同样的拦, 但这里的 deinit 在它之前, 必须单独挡。 */
    if (sd_format_is_running()) {
        ESP_LOGW(TAG, "格式化进行中 ⇒ 拒绝重新挂载");
        return ESP_ERR_INVALID_STATE;
    }
    /* ★ 2026-11-22: 卡在电脑手里 (USB MSC) ⇒ 不重挂, 否则会和 PC 抢 SDMMC */
    if (s_usb_owned) {
        ESP_LOGW(TAG, "卡已挂载到电脑 ⇒ 拒绝重新挂载");
        return ESP_ERR_INVALID_STATE;
    }
    /* ★ 2026-11-21: 已确认没有可用卡 ⇒ 直接返回上次结果。
     *   重挂没有意义 (卡没插), 而这里会先 deinit host + 空等 80ms 再进完整探测。 */
    if (!s_mounted && sd_card_absent()) return s_last_mount_err;
    if (s_mounted) sd_unmount();
    sd_host_release();
    vTaskDelay(pdMS_TO_TICKS(80));
    return sd_mount();
}

/* ================= 挂载门闸 (2026-10-24) =================
 * 问题: 开机挂载是**异步任务**(app_board 里 xTaskCreate), 页面/引擎/MP3 可能在挂载完成前
 *   就去 opendir("/sdcard") → 拿到空列表并记住"没卡", 之后再也不会重扫
 *   (linit: "存储信息文件全空"、"引擎MP3读不到")。
 * 做法: sd_mount 包一层, 记录"正在挂载/已尝试"; 任何消费者在首次读卡前调 sd_wait_ready(),
 *   它会等这次挂载收尾, 还没挂上就**补挂一次**。这样"顺序"不再依赖谁先起来。 */
static SemaphoreHandle_t s_mnt_mutex = NULL;
static volatile bool     s_mounting   = false;   /* sd_mount 正在执行 */
static volatile bool     s_mount_tried = false;  /* 至少尝试过一次 */

/* ================= 「没卡就别反复探测」门闸 (linit 2026-11-21) =================
 * 问题: 每次进游戏/音乐/阅读器页都调 sd_wait_ready(1200,true) → 没挂上就补挂一次。
 *   没插卡时这一次补挂 = sd_remount(卸载 200ms + host deinit) + 卡识别 400kHz 重试
 *   + 3 轮强制空闲重探 + 引脚体检 ⇒ **每次进页面白等 1~2 秒**, 还刷一堆
 *   `sdmmc_host_deinit` 错误日志。板上没有卡检测脚 (CD 未接), 所以只能靠探测,
 *   但"探测结果"应该记住, 不能每次重问一遍。
 * 做法 (参考 retro-go / ESP-IDF sdcard 示例: 挂载是一次性状态, 不做轮询):
 *   · "当前有没有可用卡"**只有一个真相源: s_mnt_state** ——
 *       UNKNOWN   = 还没试过 / 刚卸载 ⇒ 允许探测
 *       OK        = 已挂载
 *       其余三种  = 已确认"当前没有可用卡" ⇒ sd_wait_ready 立即返回 false, 不再探测
 *   · 只有"卡状态可能变了"的事件才把它重置为 UNKNOWN: 卸载/格式化/USB MSC 退出/进存储页,
 *     重置后由 sd_probe_async() 在**后台低优先级任务**里补探一次, 不阻塞 UI。
 *   ★ 2026-11-21 合并: 原来这里还有一个并行的 s_sd_absent_latched, 要 5 处手动同步,
 *     与 s_mnt_state 随时可能不一致。现在删掉, 由 s_mnt_state 单点派生。
 * ============================================================================ */
static volatile bool s_probe_running     = false;   /* 后台探测任务在跑 */

bool sd_card_absent(void) {
    sd_mnt_state_t st = s_mnt_state;
    return (st != SD_MNT_UNKNOWN && st != SD_MNT_OK);
}
void sd_probe_invalidate(void) { s_mnt_state = SD_MNT_UNKNOWN; }

/* 后台探测任务: 只用 sd_mount() (内部有挂载锁), 结束后自己删掉 */
static void sd_probe_task(void *arg)
{
    (void)arg;
    if (!sd_is_mounted() && !sd_format_is_running())
        sd_mount();                     /* 结果 (含"无卡") 由 sd_mount 落到锁存 */
    s_probe_running = false;
    vTaskDelete(NULL);
}

/* 后台补探一次 (低优先级一次性任务; 已挂载/已在探测/格式化中 ⇒ 直接返回 false) */
bool sd_probe_async(void)
{
    /* ★ 2026-11-24: 卡在电脑手里 / 正在交接时**不许**补探 —— 补探会跑去 sd_mount(),
     *   而挂载要重新 init SDMMC 主机; 即使 sd_mount 内部有闸, 也没必要白建任务。
     *   (这条原来漏了 s_usb_owned, 是审计发现的绕过入口之一。) */
    if (s_usb_owned) return false;
    {
        sd_arb_state_t as = sd_arb_state();
        if (as == SD_ARB_HANDOFF || as == SD_ARB_USB || as == SD_ARB_RESTORE) return false;
    }
    if (sd_is_mounted() || s_probe_running || sd_format_is_running()) return false;
    s_probe_running = true;
    if (xTaskCreate(sd_probe_task, "sd_probe", 4096, NULL, 1, NULL) != pdPASS) {
        s_probe_running = false;
        return false;
    }
    return true;
}

static inline void mnt_lazy_init(void)
{
    if (!s_mnt_mutex) s_mnt_mutex = xSemaphoreCreateMutex();
}
/* ★ 2026-10-28: 挂载/卸载统一互斥。原来只有 sd_mount() 持 s_mnt_mutex,
 *   而 sd_unmount()/sd_full_unmount()/sd_full_remount() 都直接改 s_card/s_mounted
 *   并操作 SDMMC host —— 与开机异步挂载(或重挂)并发时会出现
 *   "host already initialized"/VFS 重复注册/在 mount 过程中把 host deinit 掉。 */
static inline void mnt_lock(void)
{
    mnt_lazy_init();
    if (s_mnt_mutex) xSemaphoreTake(s_mnt_mutex, portMAX_DELAY);
}
static inline void mnt_unlock(void)
{
    if (s_mnt_mutex) xSemaphoreGive(s_mnt_mutex);
}

static esp_err_t sd_mount_body(void);

esp_err_t sd_mount(void)
{
    /* ★★★ 2026-10-28 【格式化期间禁止挂载】★★★
     *   sd_format_and_create_dirs() 会先 sd_full_unmount() 再整卡擦除(数十秒), 擦完才重新
     *   mkfs+挂载。这段窗口里 s_mounted 是 false, 于是:
     *     · 用户从格式化页退出 → 进音乐/游戏页 → sd_mount() 直接开始挂载正在被擦除的卡;
     *     · sd_get_info() → sd_full_unmount + sd_full_remount 同理。
     *   结果: 擦除被打断/文件系统写上垃圾/卡彻底认不到。这里直接拒绝, 让调用方按
     *   "未就绪" 处理; 格式化结束后 sd_full_remount() 自己会把卡挂回来。 */
    if (sd_format_is_running()) {
        ESP_LOGW(TAG, "格式化进行中 ⇒ 本次挂载请求被拒绝");
        return ESP_ERR_INVALID_STATE;
    }
    /* ★ 2026-11-22: 卡已交给电脑 (USB MSC) ⇒ 本机绝不挂 VFS。
     *   原来这里没有这道闸: 交卡后 s_mounted=false 但 s_mnt_state 仍是 OK,
     *   两道短路都不生效 ⇒ 扫描/存储信息会在 PC 读写期间重新 init host + 挂 VFS。 */
    if (s_usb_owned) {
        ESP_LOGD(TAG, "卡已挂载到电脑 (USB MSC) ⇒ 本机不挂载 VFS");
        return ESP_ERR_INVALID_STATE;
    }
    /* ★ 2026-11-24: 交卡/收卡过渡窗口同样不许重挂 —— 这段时间 VFS 正在被注销/重建,
     *   任何挂载动作都会与 sd_unmount_vfs_keep_card()/sd_remount_vfs_from_card()
     *   抢同一套 SDMMC 主机与 VFS 注册项。 */
    {
        sd_arb_state_t as = sd_arb_state();
        if (as == SD_ARB_HANDOFF || as == SD_ARB_RESTORE) {
            ESP_LOGD(TAG, "卡正在交接 (%s) ⇒ 本次挂载请求被拒绝", sd_arb_state_str());
            return ESP_ERR_INVALID_STATE;
        }
    }
    /* ★★ 2026-11-21 linit「没卡就当作无卡, 别再每次去读」★★
     *   这里原来**没有任何短路**: 只要 s_mounted==false 就重跑整套探测
     *   (host init/deinit ×N + 400kHz 卡识别 + 3 轮 80-clock 冷静期)。
     *   而全项目有十几个调用点 (页面进入/引擎/MP3/web_files …) 都是
     *   `if (sd_mount() != ESP_OK) return;` ⇒ **没插卡时每进一次页面就白等 1~2 秒**。
     *   探测结论本来就落在 s_mnt_state 上, 直接复用:
     *     已挂载                → OK
     *     已确认无可用卡        → 直接返回上次的错误码 (零 IO)
     *     只有 UNKNOWN(刚卸载/存储页主动重试) 才真正去认卡一次。
     */
    if (s_mounted) return ESP_OK;
    if (sd_card_absent()) return s_last_mount_err;

    mnt_lock();
    esp_err_t r = sd_mount_body();
    s_mounting = false;
    s_mount_tried = true;
    s_last_mount_err = r;
    /* 结果已在 sd_mount/sd_probe_fail_reason 内部落到 s_mnt_state (唯一状态源),
     * 不再单独维护"无卡锁存"变量。 */
    mnt_unlock();
    return r;
}

/* 等挂载就绪; retry=true 时若还没挂上会补挂一次 (页面入口用)。
 * ⚠ 有阻塞: wait_ms 是**上限**, 正常路径(已挂载)立即返回。 */
bool sd_wait_ready(uint32_t wait_ms, bool retry)
{
    mnt_lazy_init();
    if (sd_is_mounted()) return true;

    uint32_t t0 = xTaskGetTickCount();
    while (s_mounting) {
        if ((uint32_t)(xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(wait_ms)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
        if (sd_is_mounted()) return true;
    }
    if (sd_is_mounted()) return true;
    if (!retry || s_mounting) return sd_is_mounted();

    /* ★ 2026-11-21 linit「没卡就别探测」: 已确认无可用卡 ⇒ 立即返回 false,
     *   不再做 sd_remount (那次补挂: 卸载 200ms + 卡识别重试 ≈ 1~2 秒, 还刷错误日志)。
     *   要重新认卡: 进「存储」页 (会 sd_probe_invalidate + 后台补探) 或拔插后重启。 */
    if (sd_card_absent()) return false;

    /* 还没确认过 (开机挂载未跑到这里等): 后台补探一次, 最多等 wait_ms。
     * 有卡一般 200~400ms 内挂上; 无卡则探测任务自己把锁存置上, 下次直接返回。 */
    ESP_LOGI(TAG, "[门闸] 需要读卡但未挂载 (tried=%d) → 后台补挂一次", (int)s_mount_tried);
    sd_probe_async();
    uint32_t t1 = xTaskGetTickCount();
    while ((uint32_t)(xTaskGetTickCount() - t1) < pdMS_TO_TICKS(wait_ms)) {
        if (sd_is_mounted()) return true;
        /* ★ 2026-11-21: 探测已经收尾却没挂上 ⇒ 结论就是"没卡", 立刻返回。
         *   原来这里只盯 s_mounted, 无卡时要一直耗满 wait_ms (600~1200ms) 才返回。 */
        if (!s_probe_running) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return sd_is_mounted();
}

/* 挂载 SD 卡 (SDMMC 1-bit 模式, ESP32-S3 专用 SDMMC 控制器)
 *
 * 重要发现: Waveshare 官方 Arduino 示例代码用的是 SDMMC 模式!
 *   CustomSDPort(const char *SdName, int clk = 38, int cmd = 21, int d0 = 39, int width = 1);
 *
 * SDMMC 是 ESP32-S3 内置的 SD 卡专用硬件控制器, 比 SPI 快得多 (最高 40MHz)
 * 之前一直用 SPI 模式, 引脚虽然是 21/38/39 但走 SPI 协议卡根本不应答
 */
/* ★ 2026-10-05: "到底有没有插卡"的**快速探测** + **引脚排列自检**。
 *
 * 动机 (linit 反馈):
 *   ① 板上没接 TF 卡时, 原来 25M→20M→5M→400k 四档重试 + 失败探测会让启动日志刷一整屏
 *      `ESP_ERR_TIMEOUT` 加"可能原因 / 建议格式化"—— **没插卡不是错误**。
 *   ② 但卡确实插了却认不到, 最难查的是"**三根线哪根接哪个脚**"—— 板子刚焊/飞线时最容易错,
 *      而反复"改固件→烧录→看日志"几轮很痛苦。
 *
 * 做法:
 *   1. 先用 **400 kHz**(SD 规范"识别阶段"频率, 只要卡和线是通的就必然应答)
 *      按固件里 `s_sd_clk/s_sd_cmd/s_sd_d0` 的顺序试一次。
 *   2. 不行 ⇒ 把**这三根脚的所有排列(6 种)**都试一遍, 谁通就把正确顺序打进日志,
 *      **并且本次就直接用那一组继续挂载** —— 一次上电就把"接线顺序"问题解决掉, 不用来回烧。
 *   3. 6 种全不通 ⇒ 只打一行 INFO + 引脚体检; **不再走四档重试、不再刷错误**(无卡时零噪音)。
 *
 * ⚠ 只读: 卡识别 (CMD0/CMD8/ACMD41 + 读 CSD/CID) 不含写操作, 不会动卡上数据。
 * ⚠ 与 sd_probe_fail_reason() 的区别: 那个是"挂载失败后查原因"(5000kHz + 读 LBA0 判 exFAT),
 *   这个是"挂载之前先确认有没有卡", 更轻, 所以分开写。
 *
 * ★★ 2026-11-21 linit「没卡就要秒判」: 排列扫描默认**关闭**。
 *   引脚顺序已由板型表固化 (小智 16/15/17, 微雪 38/21/39) 并真机验证, 不需要每次上电都穷举。
 *   打开时 6 种排列全试 ≈ 0.9 s —— 没插卡时这是纯白等。真接错线要排查时把这里改回 1。 */
#define SD_PIN_PERM_SCAN 0

/* 本次实际使用的三根脚 (排列扫描可能改写它们) 的声明已提前到 s_sd_* 旁边 */

/* 按指定三根脚做一次 400kHz 卡识别 */
static esp_err_t sd_probe_pins(int clk, int cmd, int d0) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 400;
    host.flags = SDMMC_HOST_FLAG_1BIT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = clk; slot.cmd = cmd; slot.d0 = d0;
    slot.d1 = slot.d2 = slot.d3 = -1;
    slot.cd = -1; slot.wp = -1; slot.flags = 0;

    esp_err_t r = ESP_FAIL;
    /* ★ 2026-10-18: 进函数先 deinit。sdmmc_host_init() 在"已初始化"时是**空操作**,
     *   会拿上一组脚的 slot 配置继续用（排列自检失败后就是这种状态）⇒ 必然 0x107。 */
    sd_host_release();
    if (sd_host_acquire() != ESP_OK) return ESP_FAIL;
    if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot) == ESP_OK) {
        sdmmc_card_t *card = heap_caps_malloc(sizeof(sdmmc_card_t), MALLOC_CAP_INTERNAL);
        if (card) {
            r = sdmmc_card_init(&host, card);
            heap_caps_free(card);
        }
    }
    sd_host_release();   /* 必须放掉 host, 否则接下来正常挂载会 "already initialized" */
    return r;
}

static esp_err_t sd_quick_probe(void) {
#if !SD_SCAN_ENABLE
    ESP_LOGD("SDSCAN", "新载板: SD 暂未启用(旧 SDMMC 脚 13/10/14 与触摸 SCL、LCD MOSI 撞车) ⇒ 已跳过");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    /* 1. 按板型下发的三根脚试一次 (400 kHz = SD 规范识别阶段频率) */
    esp_err_t r0 = sd_probe_pins(s_sd_clk, s_sd_cmd, s_sd_d0);
    if (r0 == ESP_OK) {
        s_pin_clk = s_sd_clk; s_pin_cmd = s_sd_cmd; s_pin_d0 = s_sd_d0;
        return ESP_OK;
    }

#if SD_PIN_PERM_SCAN
    /* 2. 穷举这三根脚的 6 种排列 (仅在 SD_PIN_PERM_SCAN=1 时; 默认关) */
    const int  cand[3] = { s_sd_clk, s_sd_cmd, s_sd_d0 };
    const char *nm[3]  = { "CLK",   "CMD",   "D0"   };
    ESP_LOGD(TAG, "按固件定义认不到卡 ⇒ 开始穷举三根脚 %d/%d/%d 的所有排列...",
             s_sd_clk, s_sd_cmd, s_sd_d0);
    for (int a = 0; a < 3; a++) {
        for (int b = 0; b < 3; b++) {
            if (b == a) continue;
            int c = 3 - a - b;
            if (a == 0 && b == 1) continue;        /* 这一组上面已经试过 */
            esp_err_t rp = sd_probe_pins(cand[a], cand[b], cand[c]);
            ESP_LOGD(TAG, "  试 %s=IO%-2d  %s=IO%-2d  %s=IO%-2d   -> %s",
                     nm[0], cand[a], nm[1], cand[b], nm[2], cand[c],
                     rp == ESP_OK ? "★★ 认到卡了!" : "认不到");
            if (rp == ESP_OK) {
                s_pin_clk = cand[a]; s_pin_cmd = cand[b]; s_pin_d0 = cand[c];
                ESP_LOGD(TAG, "  ⇒⇒ 正确接法: CLK=IO%d  CMD=IO%d  D0=IO%d"
                              "  (本次已按这组继续挂载)",
                         s_pin_clk, s_pin_cmd, s_pin_d0);
                return ESP_OK;
            }
        }
    }
    ESP_LOGD(TAG, "  6 种排列全认不到 ⇒ 不是【三根脚接错顺序】的问题");
#endif
    /* 保留 sdmmc_card_init 的原始错误码: 上层据此区分 "没卡(0x107 超时)" 与
     * "卡有应答但状态异常(0x108)" —— 只有后者值得做 80-clock 冷静期重试。 */
    return r0;
}

/* ★ 2026-10-05: 探测失败时的**引脚体检** —— 直接回答"是不是我接的有问题"。
 *
 * 做法: 把三根线先当**输入 + 内部上拉**读一次电平。判据:
 *   电平 = 1  → 该脚悬空或外部有上拉/被卡座内部上拉拉起 ⇒ 接线"通"、上拉"有"
 *   电平 = 0  → 该脚被**拉到地**: 接错脚 / 焊短路 / 或该脚被别的外设驱动为低
 *
 * ⚠ 只能判"被拉死/短路", 判不出"虚焊/断线"(断了会浮空读到 1)。
 *   真正的 SD 规范要求: **CMD 与 D0 各一个 10k 上拉到 3V3**; CLK 不需要上拉。
 *   没有外部上拉时卡常见症状就是"400kHz 都认不到"。 */
static void sd_pin_health_check(void) {
    const int  pins[3] = { s_pin_clk, s_pin_cmd, s_pin_d0 };
    const char *nm[3]  = { "CLK",   "CMD",   "D0 "  };
    for (int i = 0; i < 3; i++) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << pins[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        vTaskDelay(pdMS_TO_TICKS(3));
        int lv = gpio_get_level(pins[i]);
        ESP_LOGD(TAG, "  引脚体检 %s = IO%-2d  电平=%d  %s",
                 nm[i], pins[i], lv,
                 lv ? "(悬空/有上拉 ✓)" : "(被拉到地 ✗ 接错脚/短路/被别的外设占用)");
    }
    ESP_LOGD(TAG, "  ⚠ 体检判不出【虚焊 / 断线】(断了会浮空读到 1) —— 重点查 CMD 与 D0 是否各有一个 10k 上拉到 3V3");
}

/* ================= 卡上的标准目录 (挂载与格式化**共用一张表**) =================
 * ★ 2026-11-22 (第2回合) 修两处真实缺陷:
 *   ① 原来挂载路径与格式化路径各写一份目录清单, 且**内容不一致**: 格式化只建 9 个,
 *      挂载建 18 个 ⇒ 格式化完 → /sdcard/media/mp3、/sdcard/roms/{md,sms,lava,vpet}、
 *      /sdcard/apps/data、/sdcard/data/log 全部缺失, 直到下次重启挂载才补齐
 *      (期间录音/音乐/文曲星/应用存档写不进去, 且没有任何报错)。
 *   ② 原来挂载路径对每个目录直接 mkdir 一层 —— FatFs 的 mkdir **不建父目录**,
 *      于是全新卡第一次上电时 mkdir("/sdcard/roms/gam") 因 /sdcard/roms 不存在而失败
 *      (只打一行 LOGD, 看起来"正常")。现在统一逐级创建。 */
static const char *const k_sd_dirs[] = {
    "/sdcard/roms/gam",                    /* 文曲星 .gam */
    "/sdcard/roms/gb",                     /* GB */
    "/sdcard/roms/gbc",                    /* GBC */
    "/sdcard/roms/nes",                    /* NES/FC */
    "/sdcard/roms/md",                     /* MD/Genesis (Gwenesis) */
    "/sdcard/roms/sms",                    /* SMS/GG (SMSPlus) */
    "/sdcard/roms/ab",                     /* Arduboy */
    "/sdcard/roms/vpet",                   /* 暴龙机 */
    "/sdcard/roms/lava",                   /* 文曲星 .lav */
    "/sdcard/roms/lava/LavaData",          /* 遗留只读共享存档 (V1.6 起写各游戏自身 LavaData) */
    "/sdcard/media/mp3",                   /* 音乐 */
    "/sdcard/media/screensaver",           /* 屏保图片 */
    "/sdcard/data/saves",                  /* 电子词典/应用存档 */
    "/sdcard/data/log",                    /* 系统日志 (main.c log_drain 写 bbk.log) */
    "/sdcard/books",                       /* 电子书 */
    "/sdcard/apps/data",                   /* 应用数据根 (原 /sdcard/App) */
    "/sdcard/apps/data/\xe5\x9b\xbe\xe7\x89\x87",  /* apps/data/图片 (画布) */
    NULL
};

/* 逐级创建 (mkdir -p)。返回最终那一级是否就绪 (已存在也算就绪)。 */
static bool sd_mkdir_p(const char *path)
{
    char tmp[64];
    size_t n = strlen(path);
    if (n < 2 || n >= sizeof(tmp)) return false;      /* 太短(如 "/")或太长都不碰 */
    memcpy(tmp, path, n + 1);
    bool ok = false;
    for (char *p = tmp + 1; ; p++) {
        if (*p != '/' && *p != '\0') continue;
        char c = *p;
        *p = '\0';
        ok = (mkdir(tmp, 0755) == 0) || (errno == EEXIST);
        *p = c;
        if (c == '\0') break;
    }
    return ok;
}

/* 检查并补齐标准目录; 返回本次新建的个数 (0 = 全都已存在) */
static int sd_ensure_dirs(void)
{
    int made = 0;
    for (int i = 0; k_sd_dirs[i]; i++) {
        char probe[72];
        struct stat st;
        snprintf(probe, sizeof(probe), "%s", k_sd_dirs[i]);
        if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) continue;   /* 已存在: 不重复 mkdir */
        if (sd_mkdir_p(k_sd_dirs[i])) {
            made++;
            ESP_LOGD(TAG, "  创建目录: %s", k_sd_dirs[i]);
        } else {
            ESP_LOGW(TAG, "  创建目录失败: %s (errno=%d)", k_sd_dirs[i], errno);
        }
    }
    if (made) ESP_LOGI(TAG, "  SD 目录就绪 (新建 %d 个)", made);
    return made;
}

static esp_err_t sd_mount_body(void) {
    s_mounting = true;
#if !SD_SCAN_ENABLE
    ESP_LOGD("SDSCAN", "本板型 SD 未启用(引脚与 LCD/触摸撞车), 见 sd_scan.c");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (s_mounted) {
        ESP_LOGI(TAG, "已挂载, 跳过");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "=== sd_mount 开始 (SDMMC 1-bit 模式) ===");
    ESP_LOGI(TAG, "  SD 引脚: CLK=%d CMD=%d D0=%d", s_pin_clk, s_pin_cmd, s_pin_d0);
    ESP_LOGI(TAG, "  堆剩余: %u 字节, DMA内部: %u 字节 (最大块 %u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    /* 0. 先确认有没有卡 —— 没卡就安静跳过, 不刷错误 (2026-10-05) */
    /* ★★ 2026-11-21 linit「没卡就当作无卡」: 冷静期只对**卡有应答但状态异常**才有意义。
     *   判据直接用 sdmmc_card_init 的错误码:
     *     ESP_ERR_TIMEOUT (0x107)        = 卡完全没有应答 ⇒ 就是没插卡/线断, 立刻判定, 不重试
     *     ESP_ERR_INVALID_RESPONSE(0x108)= 卡有应答但回得不对 (刷机后停在 1-bit 传输态) ⇒ 救一次
     *   原来不分原因固定重试 3 轮, 没插卡时白白多跑 3 套 400kHz 识别 (≈0.5~1.5 s)。
     *   80 个 CLK 是幂等的标准 idle 序列, 做一次就够 —— 重复 3 次没有任何额外语义。 */
    esp_err_t pr = sd_quick_probe();
    if (pr == ESP_ERR_INVALID_RESPONSE) {
        ESP_LOGW(TAG, "卡有应答但状态异常(0x108) ⇒ 补 80 个 CLK 冷静期后重探一次");
        sd_host_release();
        /* ★★★ 关键: 按 SD 规范打 **80 个 CLK 时钟**(CMD/D0 保持高) —— 这才是"让卡回到
         *   idle 态"的官方序列。只把 CLK 拉低不动是没用的: 卡没时钟就什么都不做。
         *   刷机多次后卡停在 1-bit 传输态, ESP32 复位不等于卡掉电 ⇒ 必须补这段时钟。 */
        if (s_pin_clk >= 0) {
            gpio_config_t io = { 0 };
            io.pin_bit_mask = (1ULL << s_pin_clk);
            io.mode = GPIO_MODE_OUTPUT;
            gpio_config(&io);
            if (s_pin_cmd >= 0) { gpio_set_direction((gpio_num_t)s_pin_cmd, GPIO_MODE_INPUT);
                                  gpio_pullup_en((gpio_num_t)s_pin_cmd); }
            if (s_pin_d0  >= 0) { gpio_set_direction((gpio_num_t)s_pin_d0,  GPIO_MODE_INPUT);
                                  gpio_pullup_en((gpio_num_t)s_pin_d0); }
            for (int c = 0; c < 80; c++) {
                gpio_set_level((gpio_num_t)s_pin_clk, 0); esp_rom_delay_us(2);
                gpio_set_level((gpio_num_t)s_pin_clk, 1); esp_rom_delay_us(2);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        pr = sd_quick_probe();
    }
    if (pr != ESP_OK) {
        s_mnt_state = SD_MNT_NO_CARD;
        s_cid_name[0] = '\0';
        s_card = NULL;
        ESP_LOGI(TAG, "未检测到 TF 卡 (无卡座 / 未插卡) —— 跳过 SD 初始化");
        sd_pin_health_check();      /* 顺手做一次引脚体检, 便于判"是接线问题还是真没卡" */
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "检测到 TF 卡, 开始按速度梯度挂载...");

    /* 1. 快速挂载: 从高往低试, 第一个挂上的就是本次会话的实际速度。
     *    ★ 2026-10-30 提速: 首位加 **40MHz**(SD High Speed 档, 1-bit 同样支持)。
     *      它不是"强行超频" —— max_freq_khz 只是上限, SDMMC 驱动会在 CMD6 协商,
     *      卡不支持会自己退回默认档, 不会因此挂不上。
     *    2026-09-30: 末尾那档 **400kHz** 是 SD 规范规定的"识别阶段"频率, 只要卡和线
     *      是通的就一定能挂上。**若只有 400kHz 能挂上 = 信号完整性/走线/电源问题**
     *      (日志会体现), 而不是卡的问题 → 修板子, 别在这上面继续降速。
     *    2026-10-20: 实测降到 10MHz 也救不回"认不到"(连 400k 都不应答, 是卡锁死态),
     *      所以保留高速梯度。真正救法见 sd_quick_probe() 里的多次强制空闲重试。 */
    const uint32_t speeds_khz[] = { 40000, 25000, 20000, 5000, 400 };
    const uint32_t delays_ms[]  = {  50,   50,   100,   150, 200 };
    const int retry_count = sizeof(speeds_khz) / sizeof(speeds_khz[0]);

    for (int attempt = 0; attempt < retry_count; attempt++) {
        ESP_LOGI(TAG, "  [尝试 %d/%d] 速度 %lu kHz, 延时 %lu ms...",
                 attempt + 1, retry_count,
                 (unsigned long)speeds_khz[attempt],
                 (unsigned long)delays_ms[attempt]);

        vTaskDelay(pdMS_TO_TICKS(delays_ms[attempt]));

        /* 重试前彻底释放 SDMMC host, 避免 "already initialized" 跳过初始化
         * 导致 DMA buffer 无法重新分配 (内部 RAM 碎片化时报 not enough mem) */
        if (attempt > 0) {
            sd_host_release();
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
            .format_if_mount_failed = false,
            .max_files = 24,
            /* 2026-09-30: 16KB → 0(=按扇区)。已核实 IDF 源码 (fatfs/src/ff.c:1679 +
             * vfs_fat_sdmmc.c:220): allocation_unit_size 只影响**格式化时的簇大小**
             * 与报告给 diskio 的分配单元, FatFs 的读窗口缓冲仍是 SS(fs)=512B。
             * 所以它不是崩溃原因, 但改小只有好处: 万一走到 format 分支, 不必一次要 16KB 连续内存。 */
            .allocation_unit_size = 0,
        };

        /* SDMMC 主机 - ESP32-S3 内置 SD 卡专用控制器 */
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = speeds_khz[attempt];
        host.flags = SDMMC_HOST_FLAG_1BIT;  /* 1-bit SD 模式 (官方示例 width=1) */

        sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_cfg.width = 1;        /* 1-bit 模式 */
        slot_cfg.clk = s_pin_clk;  /* 见顶部 s_sd_clk (排列自检可能改写) */
        slot_cfg.cmd = s_pin_cmd;
        slot_cfg.d0  = s_pin_d0;
        slot_cfg.d1 = -1;          /* 1-bit 模式不用 */
        slot_cfg.d2 = -1;
        slot_cfg.d3 = -1;
        slot_cfg.cd = -1;          /* 不用卡检测 */
        slot_cfg.wp = -1;          /* 不用写保护 */
        slot_cfg.flags = 0;

        esp_err_t ret = esp_vfs_fat_sdmmc_mount(s_mount_point, &host, &slot_cfg, &mount_cfg, &s_card);
        if (ret == ESP_OK) {
            s_mounted = true;
            s_mnt_state = SD_MNT_OK;
            /* ⚠ 仲裁状态等确认 card 非空、目录补齐之后再置 LOCAL (见下面 return 前) */
            /* 2026-09-30: 先判空再解引用 —— IDF 在 ESP_OK 时保证回填 card, 但崩一次代价太大,
             * 这里补一道防线 (s_cid_name / 容量 / print_info 三处都要用 s_card)。 */
            if (!s_card) {
                ESP_LOGD(TAG, "  挂载返回 OK 但 card 句柄为空 → 按失败处理");
                esp_vfs_fat_sdcard_unmount(s_mount_point, NULL);
                sd_host_release();
                s_mounted = false;
                s_mnt_state = SD_MNT_NO_CARD;
                continue;
            }
            snprintf(s_cid_name, sizeof(s_cid_name), "%s", s_card->cid.name);
            ESP_LOGI(TAG, "  [尝试 %d] 挂载成功! card=%p 速度=%lu kHz 容量 %llu 字节",
                     attempt + 1, s_card, (unsigned long)speeds_khz[attempt],
                     (unsigned long long)((uint64_t)s_card->csd.capacity * 512));
            /* 诊断提示: 高档位能挂上 = 电源/走线 OK; 只有低档位能挂上 = 信号完整性问题 */
            if (attempt > 0) {
                ESP_LOGD(TAG, "  ⚠ 只在 %lu kHz 才挂上 —— 说明 25MHz 下信号/电源不足,"
                              " 优先补: 卡座 VDD 就近 10µF+0.1µF / 3V3 主轨储能 / CLK 串 22Ω",
                         (unsigned long)speeds_khz[attempt]);
            }
            sdmmc_card_print_info(stdout, s_card);
            ESP_LOGI(TAG, "=== sd_mount 成功 ===");

            /* 补齐标准目录 (与格式化共用 k_sd_dirs[], 见 sd_ensure_dirs) */
            sd_ensure_dirs();

            /* ★ 2026-11-24: 卡确实可用、目录也补齐了, 这时才对本机开放访问。
             *   (放在 !s_card 的失败分支之后 —— 否则"挂载返回 OK 但 card 为空"
             *    那条路上会把闸门开在一个没有卡的卷上。) */
            sd_arb_set_state(SD_ARB_LOCAL);
            return ESP_OK;
        }

        ESP_LOGD(TAG, "  [尝试 %d] 挂载失败: %s (0x%x)",
                 attempt + 1, esp_err_to_name(ret), ret);
        s_card = NULL;
        /* 失败后释放 host, 下次重试能重新初始化 */
        sd_host_release();
    }

    /* 走到这里说明**卡是被识别到的**, 只是挂载失败 —— 这才是真该报的错误。
     * (没插卡的情况已经在 sd_quick_probe() 那一步安静返回了。)
     * ★ 2026-10-18: 这几行原来是 LOGD(默认看不见) ⇒ 用户只看到"未检测到 TF 卡",
     *   分不清"没卡"和"卡在但文件系统不认"。改 LOGW, 并返回**独立错误码**。 */
    ESP_LOGW(TAG, "=== sd_mount 全部 %d 次尝试都失败 (卡已被识别, 不是没插卡) ===", retry_count);
    /* 2026-09-29: 探测失败原因 (无卡 / exFAT / 无文件系统) 供 UI 显示 */
    sd_probe_fail_reason();
    ESP_LOGW(TAG, "  判定: %s", sd_get_mnt_state_str());
    ESP_LOGW(TAG, "  处理: 菜单 → 存储 → 格式化TF卡 (固件只支持 FAT32, 不支持 exFAT)");
    return ESP_ERR_INVALID_STATE;   /* ≠ ESP_FAIL: 让上层区分"卡在但挂不上" */
}

/* 卸载 VFS + 释放 host。★ 2026-11-22 (第2回合): 卸载失败不再"假装卸载成功"。
 *   esp_vfs_fat_sdcard_unmount() 失败时 VFS 注册项还在, 而调用方会以为已经卸干净;
 *   之后每一次 esp_vfs_fat_sdmmc_mount() 都因 "路径已注册" 返回 ESP_ERR_INVALID_STATE
 *   ⇒ 卡从此认不到, 只能重启。这里补一步强制注销 (esp_vfs_fat_unregister_path) 兜底,
 *   s_mounted/s_card 仍旧清零 —— 状态必须与"我们要的结果"一致, 不能停在半挂半卸。 */
static void sd_vfs_unmount_force(void)
{
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    if (ret == ESP_OK) return;
    ESP_LOGW(TAG, "[卸载] esp_vfs_fat_sdcard_unmount 失败: %s (0x%x) ⇒ 强制注销 VFS",
             esp_err_to_name(ret), ret);
    esp_err_t r2 = esp_vfs_fat_unregister_path(s_mount_point);
    if (r2 != ESP_OK)
        ESP_LOGW(TAG, "[卸载] 强制注销也失败: %s (0x%x) —— 下次挂载可能报 already registered",
                 esp_err_to_name(r2), r2);
}

/* 卸载 SD 卡 (SDMMC 模式)
 * 重要: esp_vfs_fat_sdcard_unmount 只卸载 VFS+diskio, 不释放 SDMMC host.
 *       必须额外调用 sdmmc_host_deinit() 彻底释放 host 和 DMA buffer,
 *       否则重挂时 "host already initialized" 跳过初始化, DMA buffer 无法重新分配. */
void sd_unmount(void) {
    if (!s_mounted) { return; }
    /* ★ 2026-10-28: 与 sd_mount() 共用挂载锁 —— 否则可能在挂载进行到一半时
     *   把 SDMMC host deinit 掉(manifest: host already initialized / 卡认不到)。 */
    mnt_lock();
    if (!s_mounted) { mnt_unlock(); return; }   /* 等锁期间已被别人卸掉 */
    sd_vfs_unmount_force();
    sd_host_release();  /* 彻底释放 SDMMC host + DMA buffer */
    s_mounted = false;
    s_card = NULL;
    s_mnt_state = SD_MNT_UNKNOWN;   /* 卸载后卡状态未知 ⇒ 允许下次重新探测 */
    sd_arb_set_state(SD_ARB_OFF);   /* ★ 本机已无卡可用: 仲裁闸门同步关掉 */
    mnt_unlock();
    ESP_LOGI(TAG, "SD 卡已卸载 (SDMMC host 已释放)");
}

/* 卸载 VFS 但保留 card 句柄 (用于切换到 USB MSC 模式)
 *
 * ★★ 2026-11-22 三处修正 (这三点任意一条失效都可能丢卡上数据):
 *   ① 注销 VFS 失败必须**中止交卡**。原来只打一行 LOGD 就继续返回 0, 于是本地 VFS
 *      还挂着, 却把卡同时交给电脑 ⇒ PC 与固件一起写同一张卡, 目录项/簇链互相踩。
 *   ② 与 sd_mount()/sd_unmount() 共用挂载锁。原来这里不加锁, 与后台补挂或
 *      sd_get_info() 的重挂并发时会重复注销/重复注册 VFS。
 *   ③ ff_sdmmc_set_disk_status_check() **内部没有边界检查**, 而
 *      ff_diskio_get_pdrv_card() 找不到卡时返回 0xFF ⇒ 原来会写
 *      `s_disk_status_check_en[255]`, 越界踩 fatfs 组件里的相邻静态变量。 */
int sd_unmount_vfs_keep_card(void) {
    /* 格式化进行中: 卡正在被整卡擦除, 绝不能交给电脑 */
    if (sd_format_is_running()) {
        ESP_LOGW(TAG, "[卸载VFS] 格式化进行中 ⇒ 拒绝交出卡");
        return -1;
    }

    mnt_lock();
    if (!s_mounted || !s_card) {
        /* 本来就没有本地 VFS: 有 card ⇒ 可以直接交给电脑; 没 card ⇒ 失败 */
        s_usb_owned = (s_card != NULL);
        if (s_card) sd_arb_set_state(SD_ARB_USB);
        mnt_unlock();
        return s_card ? 0 : -1;
    }

    ESP_LOGI(TAG, "[卸载VFS] 开始 (保留 SDMMC 主机和 card 句柄)");

    /* ★★ 2026-11-24 交卡四步 —— 顺序不能换, 每一步都在关掉一个真实的崩溃窗口:
     *   ① 先切 HANDOFF: 此后本机新访问被 sd_arb_enter() 一律拒绝 (封新入口);
     *   ② 立刻卸卷 f_mount(NULL): 已经进到 FatFs 里的调用在下一次访卷时拿到
     *      FR_NOT_ENABLED 快速失败, **不再触碰磁盘**; 手上还开着的 FILE* 也立刻
     *      变成"无效对象"而不是悬垂指针;
     *   ③ 等 in-flight 归零: 这是**唯一**能安全销毁 VFS 上下文的时刻;
     *   ④ 最后才注销 VFS (它内部 _lock_close + free(fat_ctx))。
     *
     *   原来的顺序是"先注销、后卸卷", ②③ 根本不存在 ⇒ 只要此刻别的任务正在
     *   f_open/f_read/f_write (持有卷互斥量), fat_ctx 被 free、互斥量被删除,
     *   下一次 xSemaphoreTake 就踩已释放内存 → 直接重启。
     *   这就是"USB MSC 一访问就重启"的根因, 不是 FatFs 重入锁能解决的问题。
     */
    sd_arb_set_state(SD_ARB_HANDOFF);

    FRESULT fr = f_mount(NULL, "", 0);
    if (fr != FR_OK)
        ESP_LOGD(TAG, "[卸载VFS] f_mount(NULL) 返回 %d (无卷可卸, 继续)", fr);

    if (!sd_arb_wait_idle(3000)) {
        /* 有模块把访问括起来却没退出(或还没接入仲裁)。记下来仍继续:
         * 继续的代价 = 那一路拿到 I/O 错误; 中止的代价 = 卡交不出去、只能重启。 */
        ESP_LOGW(TAG, "[卸载VFS] 仍有 %d 个在途访问未退出 ⇒ 强制继续 (它们会收到 I/O 错误)",
                 sd_arb_inflight());
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    if (pdrv < FF_VOLUMES) {
        ff_sdmmc_set_disk_status_check(pdrv, false);
    } else {
        ESP_LOGD(TAG, "[卸载VFS] card 未注册 diskio (pdrv=0x%x) ⇒ 跳过状态检查开关", pdrv);
    }

    esp_err_t ret = esp_vfs_fat_unregister_path(s_mount_point);
    if (ret != ESP_OK) {
        /* 注册项还在, 但卷已被 ② 卸掉 ⇒ 不能假装"本地仍挂载"(那样上层会以为卡可用,
         * 实际每次访问都失败)。状态归 OFF, 所有权放回本机, 由上层重新 sd_mount()。 */
        ESP_LOGW(TAG, "[卸载VFS] 注销 VFS 失败: %s (0x%x) ⇒ 中止交卡, 卡转为未挂载",
                 esp_err_to_name(ret), ret);
        s_mounted = false;
        s_usb_owned = false;
        s_mnt_state = SD_MNT_UNKNOWN;
        sd_arb_set_state(SD_ARB_OFF);
        mnt_unlock();
        return -2;
    }

    s_mounted = false;
    s_usb_owned = true;      /* ★ 卡从此归电脑: 本机所有挂载/格式化都必须让路 */
    sd_arb_set_state(SD_ARB_USB);
    mnt_unlock();

    ESP_LOGI(TAG, "[卸载VFS] 完成, card=%p 仍然有效, SDMMC 主机保持运行", s_card);
    return 0;
}

int sd_remount_vfs_from_card(void) {
    mnt_lock();
    if (s_mounted) {
        s_usb_owned = false;
        sd_arb_set_state(SD_ARB_LOCAL);
        mnt_unlock();
        return 0;
    }
    if (!s_card) {
        s_usb_owned = false;
        sd_arb_set_state(SD_ARB_OFF);
        mnt_unlock();
        return -1;
    }

    /* ★ 2026-11-24: 收卡期间保持 RESTORE —— 本机访问继续被拒, 直到卷重新挂上。
     *   否则"USB 已释放、VFS 还没挂好"这段窗口里, 任何任务 fopen 都会拿到
     *   ENOENT 并被上层当成"卡没了", 状态被错误锁存。 */
    sd_arb_set_state(SD_ARB_RESTORE);

    ESP_LOGI(TAG, "[重挂VFS] 从已有 card=%p 重新挂载 FAT/VFS", s_card);

    BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    if (pdrv == 0xFF) {
        ff_diskio_register_sdmmc(0, s_card);
        pdrv = 0;
        ESP_LOGI(TAG, "[重挂VFS] 重新注册 diskio, pdrv=%u", pdrv);
    }

    char drive_str[8];
    snprintf(drive_str, sizeof(drive_str), "%u:", pdrv);

    FATFS *fs = NULL;
    esp_err_t ret = esp_vfs_fat_register_cfg(
        &(esp_vfs_fat_conf_t){
            .base_path = s_mount_point,
            .fat_drive = drive_str,
            .max_files = 24,
        },
        &fs
    );
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "[重挂VFS] esp_vfs_fat_register_cfg 失败: %s (0x%x)",
                 esp_err_to_name(ret), ret);
        /* ★ 交卡状态到此结束: 卡已不在 PC 手里, 但 VFS 也没挂上 ⇒ 必须把状态
         *   置为 UNKNOWN 并放开所有权, 否则本机既读不到卡、也不再允许重新探测
         *   (s_mnt_state 还停在 OK ⇒ sd_mount() 的两道短路都不生效却又不锁存"无卡")。 */
        s_usb_owned = false;
        s_mnt_state = SD_MNT_UNKNOWN;
        sd_arb_set_state(SD_ARB_OFF);
        mnt_unlock();
        return -2;
    }

    FRESULT fres = f_mount(fs, drive_str, 1);
    if (fres != FR_OK) {
        ESP_LOGD(TAG, "[重挂VFS] f_mount 失败: %d", fres);
        esp_vfs_fat_unregister_path(s_mount_point);
        s_usb_owned = false;
        s_mnt_state = SD_MNT_UNKNOWN;
        sd_arb_set_state(SD_ARB_OFF);
        mnt_unlock();
        return -3;
    }

    s_mounted = true;
    s_mnt_state = SD_MNT_OK;      /* 状态单一真相源: 挂上就必须同步, 否则 UI 显示"未知" */
    s_usb_owned = false;
    sd_arb_set_state(SD_ARB_LOCAL);   /* ★ 卷已就绪, 这才放开本机访问 */
    mnt_unlock();
    ESP_LOGI(TAG, "[重挂VFS] 成功 @ %s", s_mount_point);
    return 0;
}

/* 取得 sdmmc_card_t 句柄 (用于 USB MSC 直接访问) */
sdmmc_card_t *sd_get_card(void) { return s_card; }

/* 是否已挂载 (供 UI 状态显示用) */
bool sd_is_mounted(void) { return s_mounted && s_card != NULL; }


/* ===== SD 卡监控 (轻量 watcher) =====
 * 参考 esp-box-emu / Retro-Go: 开机挂载一次, 不做高频重挂.
 * 本 watcher 只负责: 卡被拔出/挂载失败后, 在空闲时慢速补挂一次
 * (15s 间隔), 不再做动态调速 (高低速都是 20MHz, 纯死代码) 和
 * 周期性健康检查 (无 CD 检测脚时高频探活反而容易抢 SDMMC 总线).
 *
 * 关键: 游戏运行 (game_run_loop) 和 USB MSC "挂载到电脑" 期间必须
 * 暂停. 否则 watcher 发现 s_mounted==false 就去 sdmmc_host_deinit()+
 * 重新挂载, 会拆掉正在被 PC 读写的 SDMMC host, 导致设备挂死
 * (PC 端 "无法访问磁盘") 或游戏读 ROM 时看门狗复位.
 */
/* ★ 2026-11-22 (第2回合): 原实现是"函数体第一行 return;" —— 后面 30 行是永远执行
 *   不到的**死代码**(编译器不报, 读代码的人以为它在跑)。改成编译期开关:
 *   现在=0, 行为与之前完全一致 (不建任务); 要恢复空闲补挂时改 1 即可。 */
#define SD_WATCHER_ENABLE 0
#define SD_WATCHER_PERIOD_MS   2000   /* 检查周期: 2s (纯变量判断, 零 IO) */
#define SD_REMOUNT_INTERVAL_MS 15000  /* 补挂间隔: 15s (掉卡后慢速恢复) */

static volatile bool s_watcher_paused = false;   /* 游戏/MSC 期间暂停 */
#if SD_WATCHER_ENABLE
static TaskHandle_t s_sd_watcher_h = NULL;       /* 监控任务句柄 */
#endif

/* 监控任务: 仅在空闲且未挂载时, 慢速补挂(无卡时逐级退避, 减少无效 IO/日志/耗电) */
#if SD_WATCHER_ENABLE
static void sd_watcher_task(void *arg) {
    uint32_t last_remount_attempt = 0;
    uint32_t interval = SD_REMOUNT_INTERVAL_MS;   /* 实际退避间隔, 随连续失败递增 */
    ESP_LOGI(TAG, "[SD Watcher] 启动, 基础补挂间隔 %d ms", SD_REMOUNT_INTERVAL_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SD_WATCHER_PERIOD_MS));
        /* 游戏运行 / USB MSC 期间: 完全不碰 SD (V1.0.60+) */
        if (s_watcher_paused) continue;
        /* 已挂载: 什么都不做 */
        if (s_mounted && s_card) continue;

        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_remount_attempt < interval) continue;
        last_remount_attempt = now;

        ESP_LOGD(TAG, "[SD Watcher] 未挂载, 尝试补挂(%ums)...", interval);
        /* 重挂前确保 host 已彻底释放 (sd_mount 内部会重新初始化) */
        sd_host_release();
        vTaskDelay(pdMS_TO_TICKS(100));
        if (sd_mount() == ESP_OK) {
            ESP_LOGI(TAG, "[SD Watcher] 补挂成功!");
            interval = SD_REMOUNT_INTERVAL_MS;   /* 成功: 复位退避 */
        } else {
            /* 无卡/失败: 逐级退避 15s→30s→60s→120s, 封顶, 减少反复无效挂载 */
            if (interval < 120000) interval *= 2;
            ESP_LOGD(TAG, "[SD Watcher] 补挂失败, 退避至 %ums 后再试", interval);
        }
    }
    vTaskDelete(NULL);
}
#endif /* SD_WATCHER_ENABLE */

/* 暂停/恢复 SD 监控:
 *  - 游戏运行期间由 game_run_loop 暂停 (避免与读 ROM/存档冲突)
 *  - USB MSC "挂载到电脑" 期间由菜单暂停 (VFS 已卸载但 PC 在读写扇区) */
void sd_watcher_set_paused(bool paused) {
    s_watcher_paused = paused;
}

void sd_watcher_start(void) {
#if !SD_WATCHER_ENABLE
    ESP_LOGD(TAG, "[SD Watcher] 已关闭(不自动补挂 TF), 见 SD_WATCHER_ENABLE");
    return;
#else
    if (s_sd_watcher_h) {
        ESP_LOGD(TAG, "[SD Watcher] 已存在, 跳过");
        return;
    }
    /* 优先级: 1 (与 UI 相同, 时间片轮转; V1.0.41: 从 5 降到 1, 不抢占 UI) */
    BaseType_t ret = xTaskCreate(
        sd_watcher_task, "sd_watcher",
        3072, NULL, 1, &s_sd_watcher_h);
    if (ret != pdPASS) {
        ESP_LOGD(TAG, "[SD Watcher] 创建失败: %d", ret);
        s_sd_watcher_h = NULL;
    } else {
        ESP_LOGI(TAG, "[SD Watcher] 创建成功");
    }
#endif
}

/* 完整的卸载 - 释放 SD 资源 (SDMMC 模式)
 * 包含 sdmmc_host_deinit() 彻底释放 host 和 DMA buffer,
 * 否则重挂时 host 已初始化被跳过, DMA buffer 无法重新分配. */
static void sd_full_unmount(void) {
    ESP_LOGI(TAG, "[卸载] 开始, s_mounted=%d, s_card=%p", s_mounted, s_card);
    mnt_lock();   /* ★ 2026-10-28: 与 sd_mount() 互斥, 防止并发改 s_card / deinit host */

    /* ★ 2026-11-24: 销毁 VFS 上下文 (sd_vfs_unmount_force 内部会 free fat_ctx) 之前,
     *   同样要"封新 + 等在途" —— 和交卡走同一条规矩, 否则本机另一个任务正在
     *   f_read/f_write 时被 free 掉卷, 一样是悬垂指针崩溃。 */
    sd_arb_set_state(SD_ARB_HANDOFF);
    (void)sd_arb_wait_idle(300);        /* 短超时: 本函数也可能被"自己持锁"的调用方间接触发 */

    /* 1. 卸载 VFS (如果还挂着) */
    if (s_mounted && s_card) {
        sd_vfs_unmount_force();     /* 失败时强制注销, 避免"路径已注册"卡死后续挂载 */
    }
    s_mounted = false;
    s_card = NULL;
    s_mnt_state = SD_MNT_UNKNOWN;   /* 卸载后卡状态未知 ⇒ 允许下次重新探测 */
    sd_arb_set_state(SD_ARB_OFF);   /* 本机无卡可用 */

    /* 2. 彻底释放 SDMMC host + DMA buffer (关键!) */
    sd_host_release();

    /* 3. 等待卡稳定 */
    vTaskDelay(pdMS_TO_TICKS(200));
    mnt_unlock();

    ESP_LOGI(TAG, "[卸载] 完成 (host 已释放)");
}

/* 完全重新挂载 (SDMMC 模式)
 * ESP-IDF 的 esp_vfs_fat_sdmmc_mount() 内部会处理 SDMMC 总线初始化 */
static int sd_full_remount(uint8_t format_if_needed) {
#if !SD_SCAN_ENABLE
    ESP_LOGD("SDSCAN", "新载板: SD 暂未启用(旧 SDMMC 脚 13/10/14 与触摸 SCL、LCD MOSI 撞车) ⇒ 已跳过");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    mnt_lock();   /* ★ 2026-10-28: 与 sd_mount()/卸载互斥 */
    /* ★ 已挂载就别再挂: esp_vfs_fat_sdmmc_mount 对同一个 base_path 会返回
     *   ESP_ERR_INVALID_STATE, 而下面失败分支会把 s_card 置 NULL —— 那样 VFS 还注册着、
     *   我们却认为"没卡", 后续所有读卡都失败(必须重启才能恢复)。 */
    if (s_mounted && s_card) {
        sd_arb_set_state(SD_ARB_LOCAL);   /* 已挂载: 保证闸门是开的 */
        mnt_unlock();
        return 0;
    }
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = format_if_needed,
        .max_files = 24,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* ★ 2026-10-30: 原来这里固定 20000 —— 只要发生过一次重挂载(拔插/格式化后),
     *   速度就从主路径的 25MHz 掉到 20MHz, 而且再也不会回去。
     *   现在与主挂载路径对齐(40MHz), 格式化仍用 5MHz(慢一点但更稳)。 */
    host.max_freq_khz = format_if_needed ? 5000 : 40000;
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;
    /* ★ 2026-11-22: 用**本次实际使用**的三根脚 (s_pin_*), 与主挂载路径同一来源。
     *   原来这里读板型原始值 s_sd_*: 排列自检一旦开启并改写顺序, 重挂/格式化就会
     *   按**错的**顺序重新配槽 ⇒ 卡认不到 (主路径能挂、这里挂不上)。 */
    slot_cfg.clk = s_pin_clk;
    slot_cfg.cmd = s_pin_cmd;
    slot_cfg.d0  = s_pin_d0;
    slot_cfg.d1 = -1;
    slot_cfg.d2 = -1;
    slot_cfg.d3 = -1;
    slot_cfg.cd = -1;
    slot_cfg.wp = -1;
    slot_cfg.flags = 0;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(s_mount_point, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "挂载失败: %s (0x%x)", esp_err_to_name(ret), ret);
        s_card = NULL;
        s_mnt_state = SD_MNT_UNKNOWN;
        sd_arb_set_state(SD_ARB_OFF);     /* 挂不上就不许本机访问 */
        mnt_unlock();
        return -2;
    }
    s_mounted = true;
    s_mnt_state = SD_MNT_OK;      /* 状态单一真相源: 挂上就必须同步 */
    sd_arb_set_state(SD_ARB_LOCAL);   /* ★ 2026-11-24: 重挂成功 ⇒ 放开本机访问 */
    mnt_unlock();
    ESP_LOGI(TAG, "挂载成功 @ %s, card=%p (SDMMC 1-bit, 格式化=%d)",
             s_mount_point, s_card, format_if_needed);
    return 0;
}

/* 格式化 TF 卡 + 创建 BBK/Gam, GB/Gam 目录 (SDMMC 模式)
 * 流程:
 *   1. 完全卸载
 *   2. 探测卡 (低速, 5MHz)
 *   3. 物理擦除
 *   4. 用 format_if_mount_failed=true 重新挂载, 触发自动 mkfs
 *   5. 创建目录
 * 返回:
 *    0  成功
 *   -1  SDMMC 主机初始化失败
 *   -2  SDMMC 设备 (slot) 初始化失败
 *   -3  SD 卡探测失败
 *   -4  物理擦除失败
 *   -5  格式化后挂载失败
 *   -6  创建目录失败 (警告, 不返回失败)
 *   -7  卡已挂载到电脑 (USB MSC) ⇒ 拒绝执行, 必须先卸载 (见 s_usb_owned)
 */
/* ==================== 异步格式化 + 进度 (2026-10-01) ====================
 * 背景: 之前存储页在 UI 回调里**同步**调 sd_format_and_create_dirs(),
 *       16GB 卡全卡擦除要数十秒 → 整个界面卡死, 而且没有任何进度反馈
 *       (用户实测: "点击格式化过后可以格式化, 但期间是卡死的, 没有进度条")。
 * 现在: 起后台任务 (内部 RAM 栈, 优先级低于 UI), 全程更新 s_fmt_pct/s_fmt_stage,
 *       由存储页用弹窗的 on_render 钩子画进度条; 完成后 on_poll 收尾。
 * ⚠ 栈必须在内部 RAM: 任务里会读卡/写 FAT, 不能落在 PSRAM 栈上。 */
static volatile bool s_fmt_running = false;
static volatile int  s_fmt_pct     = 0;
static volatile int  s_fmt_result  = 0;
/* ⚠ volatile 要加在**指针本身**上: 是"别的任务会换掉这个指针", 不是"内容可变" */
static const char * volatile s_fmt_stage = "";

bool sd_format_is_running(void) { return s_fmt_running; }
int  sd_format_pct(void)        { return s_fmt_pct; }
const char *sd_format_stage_str(void) { return s_fmt_stage; }
int  sd_format_result(void)     { return s_fmt_result; }

static void sd_format_task(void *arg)
{
    (void)arg;
    s_fmt_result = sd_format_and_create_dirs();   /* 内部逐步更新进度 */
    s_fmt_running = false;                        /* 先置 false 再删任务 */
    vTaskDelete(NULL);
}

bool sd_format_start_async(void)
{
    if (s_fmt_running) return false;
    /* ★ 2026-11-22: 卡已挂载到电脑 (USB MSC) 时禁止格式化 ——
     *   否则会在 PC 正在读写这张卡的时候抢占 SDMMC 并整卡擦除。 */
    if (s_usb_owned) {
        ESP_LOGW(TAG, "卡已挂载到电脑 ⇒ 拒绝格式化 (请先卸载)");
        return false;
    }
    /* ★ 2026-11-24: 交卡/收卡窗口同样拒绝 —— 整卡擦除与 VFS 注销/重建不能交错。 */
    {
        sd_arb_state_t as = sd_arb_state();
        if (as == SD_ARB_HANDOFF || as == SD_ARB_RESTORE) {
            ESP_LOGW(TAG, "卡正在交接 (%s) ⇒ 拒绝格式化", sd_arb_state_str());
            return false;
        }
    }
    s_fmt_running = true;
    s_fmt_pct = 0;
    s_fmt_result = 0;
    s_fmt_stage = "\xe5\x87\x86\xe5\xa4\x87";   /* 准备 */
    if (xTaskCreatePinnedToCore(sd_format_task, "sd_fmt", 8192, NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGD(TAG, "格式化任务创建失败 (内存不足?)");
        s_fmt_running = false;
        return false;
    }
    return true;
}

int sd_format_and_create_dirs(void) {
#if !SD_SCAN_ENABLE
    ESP_LOGD("SDSCAN", "新载板: SD 暂未启用(旧 SDMMC 脚 13/10/14 与触摸 SCL、LCD MOSI 撞车) ⇒ 已跳过");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    ESP_LOGD(TAG, "========== 开始格式化 TF 卡 (SDMMC 模式) ==========");
    /* ★ 2026-11-22: 同步入口同样要挡 (sd_format_and_create_dirs 也可能被直接调用)。
     *   返回独立错误码 -7 = "卡在电脑手里", 与探测失败 (-3/-4) 区分开。 */
    if (s_usb_owned) {
        ESP_LOGW(TAG, "格式化为空: 卡已挂载到电脑 (USB MSC) ⇒ 拒绝执行");
        return -7;
    }
    {
        sd_arb_state_t as = sd_arb_state();
        if (as == SD_ARB_HANDOFF || as == SD_ARB_RESTORE) {
            ESP_LOGW(TAG, "卡正在交接 (%s) ⇒ 拒绝格式化", sd_arb_state_str());
            return -7;
        }
    }
    ESP_LOGD(TAG, "[进度 ⑧] 完全卸载旧挂载...");
    s_fmt_pct = 3; s_fmt_stage = "\xe5\x8d\xb8\xe8\xbd\xbd\xe4\xb8\xad";  /* 卸载中 */

    /* 1. 完全卸载 */
    sd_full_unmount();

    /* 2. 探测卡 (低速) */
    ESP_LOGD(TAG, "[进度 ⑨] 探测卡 (5MHz 低速, SDMMC)...");
    s_fmt_pct = 6; s_fmt_stage = "\xe6\x8e\xa2\xe6\xb5\x8b\xe5\x8d\xa1";  /* 探测卡 */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 5000;  /* 5MHz 低速更稳 */
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;
    /* ★ 2026-11-22: 用**本次实际使用**的三根脚 (s_pin_*), 与主挂载路径同一来源。
     *   原来这里读板型原始值 s_sd_*: 排列自检一旦开启并改写顺序, 重挂/格式化就会
     *   按**错的**顺序重新配槽 ⇒ 卡认不到 (主路径能挂、这里挂不上)。 */
    slot_cfg.clk = s_pin_clk;
    slot_cfg.cmd = s_pin_cmd;
    slot_cfg.d0  = s_pin_d0;
    slot_cfg.d1 = -1;
    slot_cfg.d2 = -1;
    slot_cfg.d3 = -1;
    slot_cfg.cd = -1;
    slot_cfg.wp = -1;
    slot_cfg.flags = 0;

    ESP_LOGD(TAG, "[进度 ⑩] 初始化 SDMMC 主机...");
    esp_err_t ret = sd_host_acquire();
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "[进度 ⑩] SDMMC 主机初始化失败: %s (0x%x)", esp_err_to_name(ret), ret);
        return -2;
    }

    ESP_LOGD(TAG, "[进度 ⑪] 初始化 SDMMC slot (GPIO 38/21/39)...");
    ret = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot_cfg);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "[进度 ⑪] SDMMC slot 失败: %s (0x%x)", esp_err_to_name(ret), ret);
        sd_host_release();
        return -2;
    }

    ESP_LOGD(TAG, "[进度 ⑫] 探测 SD 卡...");
    /* V1.0.68: 修复格式化失效: sdmmc_card_init 需要调用方提供 card 结构体地址,
     * 之前传 NULL 导致写入空指针/探测必然失败, "格式化"实际从未执行. */
    sdmmc_card_t card_struct;
    memset(&card_struct, 0, sizeof(card_struct));
    sdmmc_card_t *card = &card_struct;
    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "[进度 ⑫] 探测 SD 卡失败: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGD(TAG, "         可能原因: TF 卡没插 / 接触不良 / 卡损坏");
        sd_host_release();
        return -3;
    }
    ESP_LOGI(TAG, "[进度 ⑫] 探测到卡: 容量 %lu 扇区, 名称 %s",
             (unsigned long)card->csd.capacity, card->cid.name);
    sdmmc_card_print_info(stdout, card);

    /* 3. 物理擦除 (彻底清除所有数据) */
    ESP_LOGD(TAG, "[进度 ⑬] 物理擦除卡 (这可能需要数十秒)...");
    uint32_t sector_count = card->csd.capacity;
    ESP_LOGI(TAG, "         总扇区: %lu", (unsigned long)sector_count);
    /* 2026-10-01: 改成**分块擦除** —— 单次全卡擦除有两个问题:
     *   ① 无法报告进度 (用户实测"格式化期间卡死、没有进度条");
     *   ② 16GB 整卡一个 ERASE 命令容易超时。
     *   分块后每块更新一次进度 (10%→70%), UI 能画真进度条。 */
    {
        const uint32_t CHUNKS = 64;
        uint32_t chunk = sector_count / CHUNKS;
        if (chunk == 0) chunk = 1;
        s_fmt_stage = "\xe6\x93\xa6\xe9\x99\xa4\xe4\xb8\xad";   /* 擦除中 */
        s_fmt_pct = 10;
        for (uint32_t sec = 0; sec < sector_count; sec += chunk) {
            uint32_t n = (sec + chunk > sector_count) ? (sector_count - sec) : chunk;
            ret = sdmmc_erase_sectors(card, (size_t)sec, (size_t)n, SDMMC_ERASE_ARG);
            if (ret != ESP_OK) {
                ret = sdmmc_erase_sectors(card, (size_t)sec, (size_t)n, SDMMC_DISCARD_ARG);
            }
            if (ret != ESP_OK) {
                ESP_LOGD(TAG, "[进度 ⑬] 擦除失败 @扇区 %lu: %s (0x%x)",
                         (unsigned long)sec, esp_err_to_name(ret), ret);
                sd_host_release();
                return -4;
            }
            s_fmt_pct = 10 + (int)((int64_t)(sec + n) * 60 / (int64_t)sector_count);
        }
    }
    ESP_LOGI(TAG, "[进度 ⑬] 擦除完成 (分块, 进度 10%%→70%%)");

    /* 4. 释放 host, 准备重新挂载 */
    ESP_LOGD(TAG, "[进度 ⑭] 释放 host, 等待卡稳定...");
    s_fmt_pct = 72; s_fmt_stage = "\xe7\xad\x89\xe5\x8d\xa1\xe7\xa8\xb3\xe5\xae\x9a"; /* 等卡稳定 */
    sd_host_release();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 5. 重新挂载 (format_if_mount_failed=true 触发 mkfs) */
    ESP_LOGD(TAG, "[进度 ⑮] 重新挂载 (format_if_mount_failed=true 触发 mkfs)...");
    s_fmt_pct = 80; s_fmt_stage = "\xe5\x86\x99\xe6\x96\x87\xe4\xbb\xb6\xe7\xb3\xbb\xe7\xbb\x9f"; /* 写文件系统 */
    int rc = sd_full_remount(1);
    if (rc != 0) {
        ESP_LOGD(TAG, "[进度 ⑮] 挂载失败 rc=%d", rc);
        ESP_LOGD(TAG, "         可能原因: mkfs 失败 / 卡读写出错");
        return -5;
    }
    ESP_LOGI(TAG, "[进度 ⑮] mkfs + 挂载成功");

    /* 6. 补齐标准目录 (与挂载路径共用 k_sd_dirs[] —— 原来这里是一份更短的清单,
     *    格式化完会缺 media/mp3、roms/{md,sms,lava,vpet}、apps/data、data/log) */
    ESP_LOGD(TAG, "[进度 ⑯] 创建标准目录...");
    s_fmt_pct = 92; s_fmt_stage = "\xe5\xbb\xba\xe7\x9b\xae\xe5\xbd\x95"; /* 建目录 */
    sd_ensure_dirs();     /* 失败只告警, 不判格式化失败 (下次挂载会再补) */

    s_fmt_pct = 100; s_fmt_stage = "\xe5\xae\x8c\xe6\x88\x90"; /* 完成 */
    ESP_LOGD(TAG, "========== 格式化完成 ==========");
    return 0;
}

/* 获取 TF 卡容量信息
 * 返回: 0 成功, -1 挂载失败 (卡未插或未格式化), -2 VFS info 失败
 *
 * 优化: 如果已经挂载, 直接用 esp_vfs_fat_info, 不要 unmount+remount
 * 原因: 之前的逻辑每次都卸载+重挂载, 但 SPI 设备复用 LCD 的 SPI2 时, 重新初始化
 *       容易因为时钟频率/状态残留导致 remount 失败, 误报"请格式化"
 */
int sd_get_info(uint64_t *total_bytes, uint64_t *free_bytes) {
    ESP_LOGD(TAG, "[卡信息] 进入, s_mounted=%d, s_card=%p, heap_free=%u",
             s_mounted, s_card, (unsigned)esp_get_free_heap_size());

    /* ★ 2026-10-28: 格式化进行中不要去探测/重挂 —— 卡正在被整卡擦除,
     *   这里的 sd_full_unmount + sd_full_remount 会把擦除打断并让文件系统写坏。
     *   直接返回"未就绪", UI 显示容量为空, 格式化结束后自然恢复。 */
    if (sd_format_is_running()) {
        ESP_LOGW(TAG, "[卡信息] 格式化进行中 ⇒ 跳过容量查询");
        return -1;
    }

    /* ★ 2026-11-22: 卡在电脑手里时**不许**走下面的 sd_full_unmount + sd_full_remount:
     *   那会在 PC 读写扇区的过程中重新 init SDMMC host 并抢回卡。容量未知就返回 -1,
     *   UI 显示"状态"即可 (网络访问存储页在 MSC 期间本来也打不开)。 */
    if (s_usb_owned) {
        ESP_LOGD(TAG, "[卡信息] 卡已挂载到电脑 (USB MSC) ⇒ 不查询容量");
        return -1;
    }
    /* ★ 2026-11-24: 交卡/收卡窗口也不许进来 —— 本函数后半段会
     *   sd_full_unmount() + sd_full_remount(), 那会在 VFS 注销/重建的中途
     *   重新 init SDMMC 主机并抢卡 (审计发现的绕过入口)。 */
    {
        sd_arb_state_t as = sd_arb_state();
        if (as == SD_ARB_HANDOFF || as == SD_ARB_RESTORE) {
            ESP_LOGD(TAG, "[卡信息] 卡正在交接 (%s) ⇒ 不查询容量", sd_arb_state_str());
            return -1;
        }
    }

    /* ★ 2026-11-21 linit「没卡就别再读」: 已确认没有可用卡 ⇒ 不做重挂尝试。
     *   原来无卡时点开"存储信息"每次都会 sd_full_unmount + sd_full_remount
     *   (整套 40MHz 认卡, 认不到要等超时) ⇒ 弹窗卡好几秒。
     *   具体原因由 sd_get_mnt_state_str() 给 UI 显示, 不需要靠重挂来"再确认一次"。 */
    if (!s_mounted && sd_card_absent()) return -1;

    /* 1. 优先直接读 VFS info (如果已挂载且 card 有效) */
    if (s_mounted && s_card != NULL) {
        ESP_LOGI(TAG, "[卡信息] 已挂载, 直接读 VFS info");
        uint64_t total = 0, free = 0;
        int ret = esp_vfs_fat_info(s_mount_point, &total, &free);
        if (ret == ESP_OK) {
            if (total_bytes) *total_bytes = total;
            if (free_bytes) *free_bytes = free;
            ESP_LOGI(TAG, "[卡信息] 总 %llu 字节, 剩 %llu 字节",
                     (unsigned long long)total, (unsigned long long)free);
            return 0;
        }
        ESP_LOGD(TAG, "[卡信息] VFS info 失败: %s (0x%x), 尝试 remount",
                 esp_err_to_name(ret), ret);
        /* VFS info 失败, 落入下面的 remount 路径 */
    }

    /* 2. 未挂载 或 VFS info 失败: 重新挂载 */
    /* ★ 2026-11-24: 有在途访问时**不要**走"卸载 + 重挂"这条重路径 ——
     *   它会销毁并重建 VFS 上下文, 而调用方自己可能正握着句柄
     *   (典型: 网页列目录里调本函数), 那等于自己把脚下的东西拆掉。
     *   容量未知就返回 -1, UI 显示"--"即可。 */
    if (sd_arb_inflight() > 0) {
        ESP_LOGD(TAG, "[卡信息] 有 %d 个在途访问 ⇒ 不重挂, 返回未就绪", sd_arb_inflight());
        return -1;
    }
    ESP_LOGD(TAG, "[卡信息] 重新挂载中...");
    sd_full_unmount();
    int rc = sd_full_remount(0);  /* format_if_needed=0, 不自动 mkfs */
    ESP_LOGD(TAG, "[卡信息] remount rc=%d, heap_free=%u", rc, (unsigned)esp_get_free_heap_size());
    if (rc != 0) {
        ESP_LOGD(TAG, "[卡信息] 挂载失败 rc=%d", rc);
        s_mounted = false;
        s_card = NULL;
        return -1;
    }
    ESP_LOGI(TAG, "[卡信息] 挂载成功, 读 VFS info");
    uint64_t total = 0, free = 0;
    int ret = esp_vfs_fat_info(s_mount_point, &total, &free);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "[卡信息] VFS info 失败: %s (0x%x)", esp_err_to_name(ret), ret);
        return -2;
    }
    if (total_bytes) *total_bytes = total;
    if (free_bytes) *free_bytes = free;
    ESP_LOGI(TAG, "[卡信息] 总 %llu 字节, 剩 %llu 字节",
             (unsigned long long)total, (unsigned long long)free);
    return 0;
}

