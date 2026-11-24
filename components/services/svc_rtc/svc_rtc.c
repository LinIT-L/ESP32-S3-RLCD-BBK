/* svc_rtc.c — DS3231MZ+ 读写 + 自动校时策略
 *
 * 硬件事实（PCB1_3 网表）:
 *   U1 DS3231MZ+（SO-8）: 2 VCC=3V3+C7 100nF · 5 GND · 6 VBAT=主电池正极(常电)
 *   7 SDA=GPIO5 · 8 SCL=GPIO13（与触摸同一条 I2C_NUM_1，4.7k 上拉到 3V3）
 *   1/3/4 悬空（32kHz / INT_SQW / RST 不用）
 *
 * 时间基准: 芯片里存 **UTC**（与系统一致），显示层靠 TZ 转北京时间。
 *   ⛔ 别存本地时间：换时区/夏令时会整套时间错位。
 *
 * 寄存器: 0x00 秒 0x01 分 0x02 时(bit6=12/24h) 0x03 周 0x04 日 0x05 月(bit7=世纪) 0x06 年, BCD
 *         0x0F 状态, bit7 = OSF（振荡器停过 = 掉过电，时间不可信）
 */
#include "svc_rtc.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "svc_time.h"
#include "hw_cfg.h"        /* hw_cfg_note_rtc / hw_cfg_enabled: 硬件管理要知道芯片在不在 */
#include "touch_panel.h"   /* touch_panel_bus_gen(): 触摸恢复会重建整条 I2C 总线 */

static const char *TAG = "RTC";

#define RTC_REG_TIME     0x00
#define RTC_REG_STATUS   0x0F
#define RTC_OSF_BIT      0x80
#define RTC_I2C_HZ       100000     /* 与触摸同总线，跟触摸保持 100kHz */
#define RTC_TIMEOUT_MS   100
#define RTC_MIN_EPOCH    1700000000LL   /* 2023-11 之前的值一律当垃圾 */

/* ---- 状态 ---- */
static i2c_master_bus_handle_t s_bus    = NULL;
static i2c_master_dev_handle_t s_dev    = NULL;
static bool s_present   = false;
static bool s_osf       = false;
static int  s_last_drift = -1;
static bool s_started   = false;
/* 挂载时触摸总线的代次。触摸恢复会删掉并重建整条 I2C 总线, 本文件的 device 句柄
 * 随之失效 —— 之后**绝不能**再拿它收发, 也不能 rm_device(那是对已释放内存操作)。 */
static uint32_t s_bus_gen_at_attach = 0;

/* ★ 2026-11-24: 开机首读是否已完成（boot_sync 与任务共用, 保证只注入一次） */
static bool     s_boot_injected = false;
/* ★ 2026-11-24: 上次写芯片时系统的 epoch, 用于合并重复回写（0=还没写过） */
static int64_t  s_last_write_sys = 0;

/* ---- BCD ---- */
static inline uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static inline uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* 公历年月日 → 1970-01-01 起的天数（Howard Hinnant 的 days_from_civil）。
 * ⛔ 不用 timegm：ESP-IDF 的 newlib 没开它（编译报 implicit declaration）。
 *   mktime 又按本地时区算，改 TZ 就会错 ⇒ 纯算术最稳，且与 TZ 无关。 */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* ==================== 挂载 / 卸载 ====================
 * ⛔ 不自建总线：遍历已存在的 I2C 端口，谁上面有 0x68 就挂到谁那儿。
 *   ESP32-S3 只有 2 个端口，全试一遍成本可忽略。 */
static bool rtc_attach(void)
{
    if (s_dev) return true;

    /* ★ 2026-11-21: **先试 I2C_NUM_1 再试 0**。顺序很关键 ——
     *   i2c_master_get_bus_handle() 对未初始化的 port 会 ESP_LOGE("this port has not
     *   been initialized")。小智/微雪/标准的触摸都建在 port 1 上，先试 0 等于每次开机
     *   白刷一条错误日志; 先试 1 命中就 return，根本不会碰 0。 */
    static const int k_order[2] = { 1, 0 };
    for (int i = 0; i < 2; i++) {
        const int p = k_order[i];
        i2c_master_bus_handle_t bus = NULL;
        if (i2c_master_get_bus_handle((i2c_port_num_t)p, &bus) != ESP_OK || !bus) continue;
        if (i2c_master_probe(bus, SVC_RTC_I2C_ADDR, 50) != ESP_OK) continue;

        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = SVC_RTC_I2C_ADDR,
            .scl_speed_hz    = RTC_I2C_HZ,
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) continue;

        s_bus  = bus;
        s_dev  = dev;
        s_bus_gen_at_attach = touch_panel_bus_gen();
        s_present = true;
        ESP_LOGW(TAG, "DS3231 挂载成功: port=%d addr=0x%02X", p, SVC_RTC_I2C_ADDR);
        hw_cfg_note_rtc(true);      /* ★ 回填 hw_cfg 检测位 ⇒ 硬件管理显示"时钟:检测到" */
        return true;
    }
    return false;
}

/* 触摸重建总线后旧句柄会失效 ⇒ 丢掉，下次 attach 重来 */
static void rtc_detach(void)
{
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    s_bus = NULL;
}

/* 访问前的统一入口: 保证 s_dev 指向**当前**总线上的有效器件。
 * 触摸恢复/禁用会 i2c_del_master_bus() 整条总线, 此时旧 device 句柄已随总线释放,
 * 继续用就是 use-after-free(表现为随机重启或读到乱码时间)。发现代次变了就直接丢弃,
 * 不能 rm_device(那是对已释放内存再操作一次)。 */
static bool rtc_ready(void)
{
    if (s_dev) {
        if (s_bus_gen_at_attach == touch_panel_bus_gen()) return true;
        ESP_LOGD(TAG, "I2C 总线已被触摸恢复重建 ⇒ 丢弃旧句柄重挂");
        s_dev = NULL;
        s_bus = NULL;
        s_present = false;
    }
    return rtc_attach();
}

/* ==================== 读状态（OSF） ==================== */
static bool rtc_read_status(uint8_t *st)
{
    if (!rtc_ready()) return false;
    uint8_t reg = RTC_REG_STATUS, v = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &v, 1, RTC_TIMEOUT_MS) != ESP_OK) return false;
    *st = v;
    return true;
}

/* ==================== 读时间 ==================== */
bool svc_rtc_read(int64_t *epoch)
{
    if (!rtc_ready()) return false;

    uint8_t reg = RTC_REG_TIME;
    uint8_t b[7] = {0};
    esp_err_t e = i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof(b), RTC_TIMEOUT_MS);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "读失败(%s) ⇒ 丢句柄重挂", esp_err_to_name(e));
        rtc_detach();
        return false;
    }

    int sec  = bcd2bin((uint8_t)(b[0] & 0x7F));
    int min  = bcd2bin((uint8_t)(b[1] & 0x7F));
    int hour = (b[2] & 0x40) ? (bcd2bin((uint8_t)(b[2] & 0x1F)) + ((b[2] & 0x20) ? 12 : 0))
                             : bcd2bin((uint8_t)(b[2] & 0x3F));
    int mday = bcd2bin((uint8_t)(b[4] & 0x3F));
    int mon  = bcd2bin((uint8_t)(b[5] & 0x1F));
    int year = bcd2bin(b[6]) + 2000 + ((b[5] & 0x80) ? 100 : 0);

    /* 合法性: BCD 拆错/总线噪声会给出 0xFF 这类值，宁可判失败也别注入错误时间 */
    if (sec > 59 || min > 59 || hour > 23 || mday < 1 || mday > 31 || mon < 1 || mon > 12 ||
        year < 2000 || year > 2199) {
        ESP_LOGW(TAG, "数据非法: %02X %02X %02X %02X %02X %02X %02X",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
        return false;
    }

    /* 芯片里是 UTC（见文件头）⇒ 直接按 UTC 算 epoch，不经过本地时区 */
    int64_t ts = days_from_civil(year, (unsigned)mon, (unsigned)mday) * 86400
               + hour * 3600 + min * 60 + sec;
    if (ts <= 0) return false;

    uint8_t st = 0;
    if (rtc_read_status(&st)) s_osf = (st & RTC_OSF_BIT) != 0;

    if (epoch) *epoch = (int64_t)ts;
    return true;
}

/* ==================== 写时间 ==================== */
bool svc_rtc_write(int64_t epoch)
{
    if (epoch < RTC_MIN_EPOCH) return false;
    if (!rtc_ready()) return false;

    time_t ts = (time_t)epoch;
    struct tm t;
    gmtime_r(&ts, &t);                   /* UTC 存盘，同读侧 */

    uint8_t buf[8];
    buf[0] = RTC_REG_TIME;
    buf[1] = bin2bcd((uint8_t)t.tm_sec);
    buf[2] = bin2bcd((uint8_t)t.tm_min);
    buf[3] = bin2bcd((uint8_t)t.tm_hour);      /* bit6=0 ⇒ 24 小时模式 */
    buf[4] = (uint8_t)(t.tm_wday + 1);         /* DS3231 周寄存器 1-7，仅展示用 */
    buf[5] = bin2bcd((uint8_t)t.tm_mday);
    buf[6] = bin2bcd((uint8_t)(t.tm_mon + 1));
    buf[7] = bin2bcd((uint8_t)(t.tm_year % 100));

    esp_err_t e = i2c_master_transmit(s_dev, buf, sizeof(buf), RTC_TIMEOUT_MS);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "写失败(%s) ⇒ 丢句柄重挂", esp_err_to_name(e));
        rtc_detach();
        return false;
    }

    /* 写完必须清 OSF，否则下次开机仍被判"掉过电、时间不可信" */
    uint8_t st = 0;
    if (rtc_read_status(&st) && (st & RTC_OSF_BIT)) {
        uint8_t w[2] = { RTC_REG_STATUS, (uint8_t)(st & ~RTC_OSF_BIT) };
        if (i2c_master_transmit(s_dev, w, 2, RTC_TIMEOUT_MS) == ESP_OK) {
            s_osf = false;
            ESP_LOGI(TAG, "OSF 已清除");
        }
    }
    s_last_write_sys = epoch;      /* ★ 记写入点, 供回写节流/合并判断 */
    return true;
}

/* 写芯片 + 节流：距上次写入不足 SVC_RTC_WRITE_MIN_GAP_S 秒就跳过。
 * 用途: main 主循环的"校准后立刻回写"与周期任务的回写会撞在同一秒,
 *   这里统一挡掉第二次, 避免重复 I2C 事务。OSF 未清时例外(必须立刻写)。
 * ⚠ 只用于"把系统时间刷进芯片"的场景; 手动设时/关机前保存走 svc_rtc_save_now。 */
static bool rtc_write_throttled(int64_t epoch)
{
    if (s_last_write_sys > 0 && (epoch - s_last_write_sys) < SVC_RTC_WRITE_MIN_GAP_S && !s_osf)
        return false;
    return svc_rtc_write(epoch);
}

/* ==================== 对外小接口 ==================== */
/* ★ 2026-11-21 诊断用: 原样读回 7 字节时间 + 状态, 不做任何解释/修正 */
bool svc_rtc_dump(uint8_t regs[7], uint8_t *status)
{
    if (!rtc_ready()) return false;
    uint8_t reg = RTC_REG_TIME;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, regs, 7, RTC_TIMEOUT_MS) != ESP_OK) return false;
    if (status && !rtc_read_status(status)) return false;
    return true;
}

bool svc_rtc_present(void)     { return s_present && s_dev != NULL; }
bool svc_rtc_lost_power(void)  { return s_osf; }
int  svc_rtc_last_drift(void)  { return s_last_drift; }

void svc_rtc_save_now(void)
{
    if (!hw_cfg_enabled(HW_RTC)) return;      /* 被禁用 ⇒ 关机也不写芯片 */
    if (!rtc_ready()) return;
    int64_t now = (int64_t)time(NULL);
    if (now < RTC_MIN_EPOCH) return;          /* 系统时间无效就别污染芯片 */
    if (svc_rtc_write(now)) ESP_LOGI(TAG, "关机前已写回芯片 epoch=%lld", (long long)now);
}

/* ==================== 开机首读 + 注入（幂等, 两条路径共用） ====================
 * ★ 2026-11-24: 从 rtc_task 里抽出来, 让两条路径共用:
 *   ① main 在 app_board_init() 返回后调 svc_rtc_boot_sync() —— **同步**执行,
 *      一次 I2C 读 <5ms, 时间在 Logo/桌面之前就位(原来要等到 1597ms 才注入);
 *   ② rtc_task 兜底(boot_sync 因故没跑成功时)。
 * ⚠ 这里**不判 hw_cfg_enabled**: 首次认到芯片时 en 位刚由 hw_cfg_note_rtc() 置上,
 *   再判会把首读挡掉; 该不该跑由调用方把关(用户在硬件管理里关掉时钟 ⇒ 两边都不进来)。 */
static void rtc_boot_inject(void)
{
    if (s_boot_injected) return;

    int64_t rt = 0;
    if (!svc_rtc_read(&rt)) return;        /* 读不到: 不置位, 留给周期任务重试 */
    s_boot_injected = true;

    /* 系统时间无效 ⇒ 采用芯片时间
     * ★ 2026-11-21 linit 定规: **每次开机都按芯片时间来，哪怕它不准也照用**。
     *   原来 OSF=1 就拒绝注入 ⇒ 断过电的板子永远停在 1970，而 OSF 只有"联网校时成功"
     *   才会被清 ⇒ 没网就是死锁。现改为无条件采纳，OSF 只作提示。
     *   唯一保留的拦截: 值必须 ≥ 2023-11(TIME_MIN_VALID)，防把 0x00/0xFF 这类
     *   垃圾 BCD(→1970/2099) 当时间用。 */
    const int64_t fw = svc_time_fw_epoch();
    if (fw > 0 && rt < fw) {
        /* ★★★ 2026-11-21 linit 定规 ★★★
         *   **RTC 时间少于固件更新时间 ⇒ 判为未校准**(掉电归零 / 出厂 2000-01-01)。
         *   这时不能拿它当时间源(界面会显示 2000-01-01)，改为把**当前系统时间**
         *   (= 固件更新时间，或刚被网络/GPS 校准过的时间) 写回芯片。 */
        int64_t sys = (int64_t)time(NULL);
        ESP_LOGW(TAG, "芯片时间 %lld 早于固件更新时间 %lld ⇒ 判为未校准, 不采纳",
                 (long long)rt, (long long)fw);
        if (sys >= fw) {
            if (svc_rtc_write(sys))
                ESP_LOGW(TAG, "已把系统时间 %lld 写回芯片 (RTC 校准完成)", (long long)sys);
            else
                ESP_LOGW(TAG, "写芯片失败 ⇒ %ds 周期会再试", SVC_RTC_PERIOD_S);
        }
    } else {
        /* ★ 2026-11-24 真机实测修正（原判 `!svc_time_is_valid()` 就跳过注入）:
         *   冷启动时 svc_time 会用 NVS 记录**回放上次关机时刻**并判成 RTC_VALID
         *   ⇒ "有效"为真, 芯片时间被永久忽略。实测: 芯片=07:38(真实), 系统=23:28
         *   (上次关机时刻), 差 8h10m ⇒ 桌面时间错, 还会被下面的周期逻辑反写芯片。
         *   改为: 只有**本次开机**已被网络/GPS 校准才信系统; 其余(INVALID, 或 NVS
         *   回放出来的 RTC_VALID)一律采纳芯片 —— 关机期间过了多久只有 DS3231 知道。
         *   芯片若是垃圾(掉电归零 / 2000-01-01) 已被上面的 fw 分支先拦掉。 */
        const time_state_t st = svc_time_state();
        if (st == TIME_NETWORK_VALID || st == TIME_GPS_VALID) {
            ESP_LOGI(TAG, "开机: 系统时间已由%s校准, 芯片=%lld 系统=%lld",
                     (st == TIME_GPS_VALID) ? "GPS" : "网络",
                     (long long)rt, (long long)time(NULL));
        } else {
            if (s_osf) {
                ESP_LOGW(TAG, "芯片 OSF=1(掉过电) 但按定规仍采用其时间: %lld", (long long)rt);
            } else {
                ESP_LOGW(TAG, "开机: 用芯片 %lld 覆盖%s回放的 %lld",
                         (long long)rt,
                         (st == TIME_RTC_VALID) ? "NVS" : "本地",
                         (long long)time(NULL));
            }
            svc_time_set_from_rtc(rt);    /* 由 svc_time 决定采纳 + settimeofday */
        }
    }
}

/* 同步注入入口（main 在 app_board_init 之后调用, 早于 UI 首帧） */
bool svc_rtc_boot_sync(void)
{
    /* 用户在硬件管理里**关掉**了时钟(det=1 且 en=0) ⇒ 一个 I2C 事务都不做;
     * 从未检测到(det=0) ⇒ 允许试一次, 探到后 hw_cfg_note_rtc() 回填 det/en,
     *   这样"首次探测失败"不会再变成"永久用不上时钟"。 */
    if (!hw_cfg_enabled(HW_RTC) && hw_cfg_detected(HW_RTC)) return false;
    if (!rtc_ready()) return false;
    rtc_boot_inject();
    return s_present;
}

/* ==================== 周期任务 ==================== */
static void rtc_task(void *arg)
{
    (void)arg;

    /* ① 等"别人先把芯片挂上"。
     * ★ 2026-11-24: 不再固定睡 600ms（实测空转 335ms: 总线 1130ms 就绪, 却等到 1465ms 才试）。
     *   正常时序: main 在 app_board_init() 返回后(实测 T+1130ms)同步调 svc_rtc_boot_sync(),
     *   它 attach 完 s_dev 就非空; 本任务 T+865ms 起轮询 s_dev(50ms 一档, 最多 1s),
     *   一挂好立刻往下走 —— 不固定长睡, 也不盲目 probe
     *   (i2c_master_get_bus_handle 对未初始化 port 会刷 ESP_LOGE 噪声)。
     *   1s 内没人挂上 ⇒ 自己按 200ms 间隔重试(总线异常 / 没芯片的兜底路径)。 */
    for (int i = 0; i < 20 && !s_dev; i++) vTaskDelay(pdMS_TO_TICKS(50));
    int tries = 0;
    while (!rtc_attach() && ++tries <= 20) vTaskDelay(pdMS_TO_TICKS(200));
    if (!s_dev) {
        ESP_LOGW(TAG, "未找到 DS3231(0x68) ⇒ 自动校时退出");
        s_present = false;
        s_started = false;         /* ★ 清掉"已启动"标记, 下次开机/重试还能再认一次 */
        vTaskDelete(NULL);
        return;
    }

    /* ② 开机首读（幂等: boot_sync 已注入过就直接返回, 不会重复 settimeofday） */
    rtc_boot_inject();

    /* ③ 周期比对（SVC_RTC_PERIOD_S 秒） */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SVC_RTC_PERIOD_S * 1000));

        /* ★ 硬件管理里可禁用时钟: 关掉就一个 I2C 事务都不做 (不读不写不校时) */
        if (!hw_cfg_enabled(HW_RTC)) continue;

        if (!rtc_ready()) continue;

        int64_t r = 0;
        if (!svc_rtc_read(&r)) {               /* 总线异常：等一拍重挂 */
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        int64_t sys = (int64_t)time(NULL);
        int64_t signed_d = sys - r;            /* >0 = 芯片落后于系统 */
        int64_t d = (signed_d < 0) ? -signed_d : signed_d;
        s_last_drift = (int)d;

        /* ★ 2026-11-21: 芯片"比固件还旧"= 从来没被校准过(2000-01-01 / 掉电归零)。
         *   不等联网也自愈: 只要系统时间立起来了(≥ 固件更新时间)就写进去。 */
        {
            const int64_t fw = svc_time_fw_epoch();
            if (fw > 0 && r < fw && sys >= fw) {
                if (svc_rtc_write(sys))
                    ESP_LOGW(TAG, "芯片仍落后于固件更新时间(%lld < %lld) ⇒ 已用系统时间 %lld 校正",
                             (long long)r, (long long)fw, (long long)sys);
                continue;
            }
        }

        const time_state_t st = svc_time_state();

        if (st == TIME_NETWORK_VALID || st == TIME_GPS_VALID) {
            /* 系统时间刚被网络/GPS 校准 = 最准 ⇒ 把芯片写成它
             * ★ 2026-11-21: GPS 是第一优先级，同样要把芯片校正过来，
             *   这样下一次冷启动(没 GPS 没网)直接 io 出 GPS 那一次的准确时间。
             * ★ 2026-11-24: 走节流写 —— main 主循环校准后也会立刻回写一次,
             *   两条路径撞在同一秒时这里让开, 不重复开 I2C 事务。 */
            if (s_osf || d >= 1) {
                if (rtc_write_throttled(sys))
                    ESP_LOGI(TAG, "芯片已同步到系统时间（原偏差 %llds）", (long long)d);
            }
        } else if (st == TIME_RTC_VALID && signed_d > SVC_RTC_BACKWARD_MAX_S) {
            /* ★ 2026-11-24 倒退/异常保护: 系统时间已可信(离线走时连贯), 而**芯片比系统早**
             *   10 分钟以上 ⇒ 芯片掉过电/被写错/读回旧值。这时**不能**拿芯片去修
             *   系统时间(会把时间拉回), 反向把系统时间写进芯片纠正它。
             * ⚠ 判据用**带符号**差值: 原用绝对值 |d|, 芯片比系统**新**时(冷启动系统
             *   时间是 NVS 回放的旧值)也会命中, 反而把正确的芯片时间写成旧值 —— 实测
             *   踩到过(芯片 07:38 / 系统 23:28, |d|=29395)。 */
            if (rtc_write_throttled(sys))
                ESP_LOGW(TAG, "芯片比系统早 %llds(超 %ds) ⇒ 判为芯片异常, 已用系统时间校正",
                         (long long)signed_d, SVC_RTC_BACKWARD_MAX_S);
        } else if (d >= SVC_RTC_CORRECT_MIN_S) {
            /* 离线久了：芯片(±5ppm)比内部 RC 准 ⇒ 反过来修系统时间。
             * ★ 2026-11-24: 差值 < 5s 不动(避免频繁跳秒); 芯片早 10min 以上见上面的保护分支。
             * ★ 2026-11-21: 去掉 !s_osf 限制 —— 掉过电也照用芯片时间，
             *   否则 OSF=1 的板子每一轮都跳过校正，系统时间就自己漂。 */
            svc_time_set_from_rtc(r);
        }
    }
}

bool svc_rtc_init(void)
{
    if (s_started) return false;
    s_started = true;

    /* ⛔ 栈必须放**内部 RAM**，不能放 PSRAM。
     *   本任务会读 NVS(rec_load/rec_save) —— NVS 走 flash，要临时关 cache；
     *   cache 一关 PSRAM 就不可访问，若任务栈在 PSRAM 会当场
     *   assert: esp_task_stack_is_sane_cache_disabled()（2026-11-21 实测崩溃）。
     *   3KB 内部 RAM 换这条通路的安全，值。 */
    static StackType_t s_stk[3584 / sizeof(StackType_t)];
    static StaticTask_t s_tcb;

    TaskHandle_t h = xTaskCreateStatic(rtc_task, "svc_rtc",
                                       3584 / sizeof(StackType_t), NULL, 2, s_stk, &s_tcb);
    if (!h) {
        ESP_LOGE(TAG, "任务创建失败");
        s_started = false;
        return false;
    }
    ESP_LOGI(TAG, "自动校时已启动（周期 %ds）", SVC_RTC_PERIOD_S);
    return true;
}
