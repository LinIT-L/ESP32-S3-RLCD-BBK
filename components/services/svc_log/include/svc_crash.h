#ifndef LIN_TOS_SVC_CRASH_H
#define LIN_TOS_SVC_CRASH_H

/* svc_crash — 崩溃现场自留证
 *
 * 目的: 用户报"突然重启"时, 不必依赖当时是否插着串口。panic 发生时把现场
 * （异常类型 / 出错地址 / 任务名 / backtrace / 栈余量 / 堆 / SD 与网络服务状态）
 * 写进 **RTC 保留内存**, 该区域在 panic 软复位、看门狗复位后都不会被清零；
 * 下一次开机读出来打印, 并在 NVS 就绪后转存一份（真掉电也能留）。
 *
 * ★ 2026-11-25 改造原因（"重启后拿不到任何现场"）:
 *   ① 原来的 svc_crash_init() 在 nvs_flash_init() **之前**调用(main.c 1136 vs 1192),
 *      内部 crash_save_nvs/crash_load_nvs 全部 nvs_open 失败后静默返回 ⇒
 *      NVS 转存从未成功过一次, "RTC 丢失回退 NVS"这条分支永远读不到东西。
 *      现在拆成两段: svc_crash_init() 只读 RTC + 打印(早期), svc_crash_persist()
 *      在 NVS 就绪后才落盘。
 *   ② 原来只有 panic 才产生记录; 看门狗/掉压(brownout)/软件重启**完全不落盘**,
 *      串口没接 = 永久无痕。而 TF 卡大电流操作最容易触发 brownout。
 *      现在每次开机都往 NVS 追加一条"复位历史"(16 条环形), 覆盖所有复位类型。
 *
 * ⚠ panic 上下文里只允许纯内存写: 不 malloc、不 printf、不碰 flash。
 *   所有格式化/打印/落盘都推迟到重启后的 svc_crash_init() / svc_crash_persist()。
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ⚠ 只要 svc_crash_rec_t 的**字段布局**有任何变化，这个版本号必须 +1。
 *   否则旧固件留在 RTC/NVS 里的 blob 会被按新布局解读 —— 字段全部错位，
 *   打印出来的"复位原因/任务名"都是别的字段的字节（本轮踩过：ver 没升，
 *   reason 显示成 "nstruction"、reset_reason 显示成一个时间戳）。
 *   读侧(init / get / persist)一律校验 ver == SVC_CRASH_VER，不匹配就当作无记录。 */
#define SVC_CRASH_VER       4
#define SVC_CRASH_FRAMES    12
#define SVC_CRASH_MAGIC     0x4C435243u   /* 'LCRC' 新现场, 尚未打印 */
#define SVC_CRASH_MAGIC_DONE 0x4C435244u  /* 'LCRD' 已打印过, 内容仍可读 */

/* NVS 里保留的复位历史条数（够看清"是不是在循环重启"以及循环在什么类型上） */
#define SVC_CRASH_HIST_N    16

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t nframes;
    uint32_t crc;            /* 本结构(除 crc 字段本身)的 CRC32; 0=未计算 */
    uint32_t reset_reason;   /* 本次开机看到的复位原因 esp_reset_reason_t */
    uint32_t exception;      /* panic_exception_t */
    uint32_t core;
    uint32_t addr;           /* 触发异常的指令地址 */
    uint32_t uptime_ms;      /* 崩溃时已开机多久（区分"开机即崩"和"跑了几小时才崩"） */
    uint32_t boot_seq;       /* RTC 累计开机次数（掉电归零，用来判断现场新旧） */
    uint32_t stack_free;     /* 崩溃时当前任务栈余量(字节), 0=取不到 */
    uint32_t heap_free;      /* 内部堆空闲 */
    uint32_t heap_min;       /* 内部堆历史最低 */
    uint32_t psram_free;     /* PSRAM 空闲 */
    uint8_t  sd_state;       /* sd_arb_state_t: 0=OFF 1=LOCAL 2=HANDOFF 3=USB 4=RESTORE */
    uint8_t  sd_inflight;    /* 交卡时刻的在途访问数 */
    uint8_t  msc_owned;      /* USB MSC 是否正持有该卡 */
    uint8_t  web_running;    /* 网页文件服务是否在跑 */
    /* 关键寄存器（Xtensa 异常帧；非异常类 panic 可能为 0）。
     * exccause/excvaddr 对 LoadProhibited/StoreProhibited 尤其关键：
     *   exccause 区分"读非法"还是"写非法"，excvaddr 就是那个非法地址。 */
    uint32_t reg_pc;
    uint32_t reg_ps;
    uint32_t reg_exccause;
    uint32_t reg_excvaddr;
    uint32_t reg_a0;
    uint32_t reg_a1;
    uint32_t reg_a2;
    uint32_t reg_a3;
    uint32_t reg_a4;
    uint32_t reg_a5;
    char     reason[32];
    char     desc[64];
    char     task[16];
    uint32_t frames[SVC_CRASH_FRAMES];
} svc_crash_rec_t;

/* NVS 复位历史: 每次开机追加一条, 与 RTC 是否保留无关。
 * 用途: 判断"重启是在什么条件下反复发生"—— brownout / 看门狗 / panic 一目了然。 */
typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t head;           /* 下一条写入位置 */
    uint32_t total;          /* 累计条数（只增，可用于判断是否循环） */
    struct {
        uint32_t rst;        /* 本次开机的复位原因 */
        uint32_t uptime;     /* 上次运行了多久(ms), 0=未知 */
        uint32_t flags;      /* bit0=有 panic 现场 */
    } e[SVC_CRASH_HIST_N];
} svc_crash_hist_t;

#define SVC_CRASH_HIST_MAGIC 0x4C434853u  /* 'LCHS' */

/* 开机早期调用一次: 只读 RTC 并打印, **不碰 NVS**（此时 NVS 常常还没初始化）。
 * 无崩溃时静默（异常复位时会 dump 日志环）。 */
void svc_crash_init(void);

/* nvs_flash_init() 成功之后调用一次:
 *   · 把 RTC 里的 panic 现场转存 NVS（真掉电也能留）
 *   · 把本次开机的复位原因追加进复位历史
 * 重复调用安全（以 boot_seq 去重，同一次开机不会写两条历史）。 */
void svc_crash_persist(void);

/* 取上一次崩溃现场（RTC 优先，退回 NVS）。返回 false = 没有记录。 */
bool svc_crash_get(svc_crash_rec_t *out);

/* 取复位历史（仅 NVS）。返回 false = 没有历史。 */
bool svc_crash_hist_get(svc_crash_hist_t *out);

/* 复位原因 → 可读文字（crash 命令与日志共用一份措辞）。 */
const char *svc_crash_rst_name(uint32_t rst);

#ifdef __cplusplus
}
#endif

#endif /* LIN_TOS_SVC_CRASH_H */
