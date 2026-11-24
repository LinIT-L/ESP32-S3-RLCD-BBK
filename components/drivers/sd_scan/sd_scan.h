#ifndef SD_SCAN_H
#define SD_SCAN_H

/* SD 卡驱动层接口 (挂载/卸载/探测/格式化/状态)。
 * ⚠ 本模块**不提供目录扫描** —— 2026-11-22 删除了那层已无人调用的 scan_* API
 *   (各页面用自己的 os_pane / opendir)。需要列文件请在上层实现。 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include "sdmmc_cmd.h"   /* for sdmmc_card_t typedef */

#ifdef __cplusplus
extern "C" {
#endif

/* 挂载 TF/SD 卡到 /sdcard */
esp_err_t sd_mount(void);

/* 卸载 TF/SD 卡 */
void sd_unmount(void);

/* ================= 挂载状态 / 卡名 / 失败原因 (2026-09-29, 供 UI 显示) =================
 * 背景: "换卡后认不到卡"时 UI 只显示"未挂载", 分不清"没插好"与"文件系统不对"。
 * 判据: 卡协议层正常(无 0x107 超时) + FatFs 报 13(FR_NO_FILESYSTEM) ⇒ 卡是 exFAT (固件不支持)。 */
typedef enum {
    SD_MNT_UNKNOWN = 0,
    SD_MNT_OK,        /* 已挂载 */
    SD_MNT_NO_CARD,   /* 卡无响应: 未插好 / 接触不良 / CMD-CLK-D0 接线 */
    SD_MNT_EXFAT,     /* 卡是 exFAT, 固件 FATFS 不支持 → 请格式化为 FAT32 */
    SD_MNT_NO_FS,     /* 卡就绪但没有可挂载的 FAT 文件系统 (未格式化/损坏/其它格式) */
} sd_mnt_state_t;

const char *sd_get_cid_name(void);        /* 卡的 CID 卡名 (出厂固化), 无卡返回 "-" */
/* ★ 2026-10-18: CID 的 name 是**出厂型号名**(很多 4G 卡就写 "SD16G"), 不是容量 —— UI 别拿它当容量。
 *   要显示"品牌 + 真实容量"用下面两个。 */
const char *sd_get_vendor_str(void);      /* 按 CID MID 推断品牌 (SanDisk/Samsung/...), 未知返回 "通用卡" */
uint64_t    sd_get_size_bytes(void);      /* 卡原始容量 (字节), 无卡返回 0 */
sd_mnt_state_t sd_get_mnt_state(void);
const char *sd_get_mnt_state_str(void);   /* 中文短句 (已挂载/卡无响应/exFAT 不支持/无文件系统) */

/* 重新挂载 TF 卡: 换卡/插卡后不必重启 (SD Watcher 的自动补挂是禁用状态) */
esp_err_t sd_remount(void);

/* 挂载门闸: 开机挂载是异步任务, 任何"要读 /sdcard"的页面在首次 opendir 前先过这道闸 ——
 * 已挂载立刻返回; 正在挂载则最多等 wait_ms; retry=true 时还没挂上会补挂一次。
 * ★ 2026-11-21: 补挂改为**后台任务 + 最多等 wait_ms**; 且"已确认无可用卡"后
 *   **立即返回 false, 不再重复探测** (没卡时进页面不再白等 1~2 秒)。
 * 返回是否已挂载。⚠ 有阻塞 (上限 wait_ms), 只在页面入口/首次扫描用, 别放渲染循环里。 */
bool sd_wait_ready(uint32_t wait_ms, bool retry);

/* === 卡可用性 (2026-11-21 合并为单一真相源) ===
 * 状态全部由 sd_get_mnt_state() 一个变量派生, 不再有第二个"无卡锁存"变量:
 *   UNKNOWN               → 还没试过 / 刚卸载 ⇒ 允许探测
 *   OK                    → 已挂载
 *   NO_CARD/EXFAT/NO_FS   → 已确认"当前没有可用卡"
 * sd_card_absent(): 是否已确认没有可用卡。为 true 时下列函数都**不做任何探测**, 直接返回:
 *   sd_wait_ready() → false · sd_mount()/sd_remount() → 上次的错误码 · sd_get_info() → -1
 *   ⇒ 没插卡时进页面/扫目录/看存储信息都是零等待。
 * sd_probe_invalidate(): 重置为 UNKNOWN —— 只在"卡状态可能变了"时调
 *   (进存储页 / 换卡 / 格式化 / USB MSC 退出)。
 * sd_probe_async(): 后台低优先级补探一次 (已挂载/已在探测/格式化中返回 false)。 */
bool sd_card_absent(void);
void sd_probe_invalidate(void);
bool sd_probe_async(void);

/* 格式化 TF 卡 + 创建标准目录 (与挂载路径共用同一张目录表)
 * 返回 0 成功; <0 失败:
 *   -2 SDMMC host / slot 初始化失败   -3 认不到卡
 *   -4 擦除失败   -5 mkfs 后挂载失败
 *   -7 卡已挂载到电脑 (USB MSC), 拒绝执行 */
int sd_format_and_create_dirs(void);
/* 异步格式化 + 进度 (2026-10-01): 全卡擦除要数十秒, 同步调用会让 UI 卡死。
 * 用法: sd_format_start_async() → 弹窗 on_render 里读 sd_format_pct()/sd_format_stage()
 *       画进度条 → on_poll 里 sd_format_is_running() 变 false 后读 sd_format_result()。 */
bool sd_format_start_async(void);      /* false = 已在跑 / 任务创建失败 */
bool sd_format_is_running(void);
int  sd_format_pct(void);              /* 0..100 */
const char *sd_format_stage_str(void); /* 阶段文字 (UTF-8; ⚠ 别叫 sd_format_stage —— 存储页已有同名静态函数) */
int  sd_format_result(void);           /* 完成后: 0=成功, <0 = sd_format_and_create_dirs 的返回码 */

/* 卸载 VFS 但保留 card 句柄 (切换 USB MSC 模式用)
 * 返回 0=卡已交给 USB MSC; <0=失败, 调用方必须**中止挂载**并保持本地挂载不变:
 *   -1 格式化进行中 / 没有可交出的 card   -2 注销本地 VFS 失败 (本地仍挂载) */
int sd_unmount_vfs_keep_card(void);

/* 从已有 card 句柄重新挂载 VFS/FAT (退出 USB MSC 模式用)
 * 返回 0=成功; -1 无 card; -2 注册 VFS 失败; -3 f_mount 失败
 * ⚠ 失败后卡状态被置为 UNKNOWN ⇒ 下一次 sd_mount() 会重新认卡。 */
int sd_remount_vfs_from_card(void);

/* 卡当前是否已交给 USB MSC (挂载到电脑)。
 * 为 true 时 sd_mount()/sd_remount()/sd_get_info()/格式化全部让路 ——
 * 本机不与 PC 抢同一张卡 (PC 读写期间重新 init SDMMC 或整卡擦除会丢数据)。 */
bool sd_usb_owned(void);

/* 取得 sdmmc_card_t 句柄, 让 USB MSC 直接访问扇区 (已挂载时非 NULL) */
sdmmc_card_t *sd_get_card(void);

/* 是否已挂载 (供 UI 状态显示用) */
bool sd_is_mounted(void);

/* 启动 SD 卡监控任务 (空闲时慢速补挂)
 * ⚠ 当前**编译期关闭** (SD_WATCHER_ENABLE=0, 见 .c): 浅睡禁 flash cache 期间补挂
 *   会触发 "Cache disabled ... accessed" 重启。调用它是安全的空操作。 */
void sd_watcher_start(void);
/* 暂停/恢复 SD 监控 (游戏运行、USB MSC "挂载到电脑" 期间必须暂停) */
void sd_watcher_set_paused(bool paused);

/* 获取 TF 卡容量信息 (总字节, 剩余字节)
 * 返回 0 成功, 负值失败 */
int sd_get_info(uint64_t *total_bytes, uint64_t *free_bytes);

/* ★ 2026-10-16: app_board 按板型表下发 SD(SDMMC 1-bit) 引脚
 * ⚠ 2026-11-22: 原来这一行在 `#endif` **之后** (include guard 外面), 被多次包含时会
 *   重复声明; 挪进来。 */
void sd_scan_apply_pins(int clk, int cmd, int d0);

#ifdef __cplusplus
}
#endif

#endif /* SD_SCAN_H */
