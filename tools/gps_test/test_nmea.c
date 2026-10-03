/**
 * test_nmea.c — gps_nmea.c 的本机单元测试 (不依赖 ESP-IDF / 不依赖硬件).
 *
 * 编译运行:
 *   cc -std=gnu11 -Wall -Wextra -I tools/gps_test/shim -I components/drivers/gps \
 *      tools/gps_test/test_nmea.c components/drivers/gps/gps_nmea.c -lm -o /tmp/t_nmea && /tmp/t_nmea
 *
 * 报文校验和是用独立实现算出来的, 且其中一条是维基公开的经典样例 (*47) ——
 * 用来交叉验证"校验和算法本身"有没有写错.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "gps_nmea.h"
#include "gps.h"

/* ---- 被测文件依赖的传输层符号: host 上用桩顶掉 ---- */
int  gps_read(uint8_t *buf, size_t max_len, uint32_t timeout_ms) { (void)buf; (void)max_len; (void)timeout_ms; return 0; }
bool gps_is_ready(void) { return true; }

static int g_pass, g_fail;

static void ok(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  ok   %s\n", what); }
    else      { g_fail++; printf("  FAIL %s\n", what); }
}

static void okf(int cond, const char *what, double got, double want)
{
    if (cond) { g_pass++; printf("  ok   %s (%.6f)\n", what, got); }
    else      { g_fail++; printf("  FAIL %s: got %.6f, want %.6f\n", what, got, want); }
}

/* 逐行喂 (服务任务就是这么喂的: 按 \r\n 切) */
static void feed_line(const char *s)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%s\r\n", s);
    gps_nmea_feed((const uint8_t *)buf, strlen(buf));
}

#define TOL 1e-4

int main(void)
{
    gps_fix_t f;

    /* ================= 1. 校验和闸门 ================= */
    printf("[1] 校验和\n");
    gps_nmea_reset();
    feed_line("$GNGGA,081234.00,2502.7600,N,11907.1400,E,1,10,0.9,32.5,M,0.0,M,,*00"); /* 故意错 */
    gps_nmea_get(&f);
    ok(f.bad_checksum == 1, "错误校验和被计数");
    ok(f.sentences == 0, "错误句子不计入 sentences");
    ok(!f.valid, "错误句子不产生定位");

    /* 分片喂 (一行拆成多段, 模拟 UART 任意切分) */
    gps_nmea_reset();
    const char *g  = "$GNGGA,081234.00,2502.7600,N,11907.1400,E,1,10,0.9,32.5,M,0.0,M,,*7D\r\n";
    gps_nmea_feed((const uint8_t *)g, 10);
    gps_nmea_feed((const uint8_t *)g + 10, strlen(g) - 10);
    gps_nmea_get(&f);
    ok(f.sentences == 1, "跨两次 feed 的一行仍能解析 (行装配正确)");

    /* ================= 2. GGA ================= */
    printf("[2] GGA\n");
    gps_nmea_reset();
    feed_line("$GNGGA,081234.00,2502.7600,N,11907.1400,E,1,10,0.9,32.5,M,0.0,M,,*7D");
    gps_nmea_get(&f);
    ok(f.valid, "定位有效");
    okf(fabs(f.lat - 25.046) < TOL, "纬度 (2502.7600N = 25 + 2.76/60)", f.lat, 25.046);
    okf(fabs(f.lon - 119.119) < TOL,    "经度 (11907.1400E = 119 + 7.14/60)", f.lon, 119.119);
    okf(fabs(f.alt_m - 32.5) < TOL,     "海拔", f.alt_m, 32.5);
    ok(f.sats_used == 10,  "卫星数 = 10");
    ok(f.fix_quality == 1, "定位质量 = 1");
    okf(fabs(f.hdop - 0.9) < TOL, "HDOP", f.hdop, 0.9);
    ok(f.has_time && f.hour == 8 && f.min == 12 && f.sec == 34, "UTC 时间 08:12:34");

    /* ================= 3. RMC (含日期 + 节速回落) ================= */
    printf("[3] RMC\n");
    feed_line("$GNRMC,081234.00,A,2502.7600,N,11907.1400,E,12.5,84.4,031026,,,A*4B");
    gps_nmea_get(&f);
    okf(fabs(f.speed_mps - 12.5 * 0.514444f) < 1e-3, "RMC 节速 → m/s (VTG 未到时回落)", f.speed_mps, 6.43056);
    okf(fabs(f.course_deg - 84.4) < TOL, "航向", f.course_deg, 84.4);
    ok(f.has_date && f.day == 3 && f.month == 10 && f.year == 2026, "UTC 日期 2026-10-03");

    /* ================= 4. VTG 覆盖速度 (km/h 优先) ================= */
    printf("[4] VTG\n");
    feed_line("$GNVTG,84.4,T,85.0,M,12.5,N,23.2,K,A*3D");
    gps_nmea_get(&f);
    okf(fabs(f.speed_mps - 23.2 / 3.6) < 1e-3, "VTG km/h 优先于 RMC 节速", f.speed_mps, 6.444444);

    /* ================= 5. GSA ================= */
    printf("[5] GSA\n");
    feed_line("$GNGSA,A,3,01,02,03,04,05,06,07,08,09,10,,,1.5,0.9,1.2,1*3F");
    gps_nmea_get(&f);
    ok(f.fix_dim == 3, "定位维度 = 3D");

    /* ================= 6. 质量 0 不得产生定位 ================= */
    printf("[6] 无定位 (quality=0)\n");
    {
        /* quality=0 + 空经纬度; 校验和由本机独立算 */
        const char *s = "$GNGGA,081234.00,,,,,0,03,9.9,,M,,M,,";
        unsigned cs = 0; for (const char *p = s + 1; *p; p++) cs ^= (unsigned char)*p;
        char line[160]; snprintf(line, sizeof(line), "%s*%02X", s, cs);
        gps_nmea_reset();
        feed_line(line);
    }
    gps_nmea_get(&f);
    ok(!f.valid, "quality=0 不置 valid");
    ok(f.sats_used == 3, "quality=0 时仍更新卫星数 (供上层显示 等定位)");

    /* ================= 7. GLL 兜底 ================= */
    printf("[7] GLL\n");
    gps_nmea_reset();
    feed_line("$GNGLL,2502.7600,N,11907.1400,E,081234.00,A,A*74");
    gps_nmea_get(&f);
    ok(f.valid, "GLL 可独立产生定位");
    okf(fabs(f.lat - 25.046) < TOL, "GLL 纬度", f.lat, 25.046);

    /* ================= 8. 经典样例交叉验证 ================= */
    printf("[8] 交叉验证 (维基样例)\n");
    gps_nmea_reset();
    feed_line("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    gps_nmea_get(&f);
    ok(f.sentences == 1, "校验和 *47 通过 (算法与公开实现一致)");
    okf(fabs(f.lat - 48.1173) < 1e-3, "样例纬度 4807.038N", f.lat, 48.1173);
    okf(fabs(f.lon - 11.516667) < 1e-3, "样例经度 01131.000E", f.lon, 11.516667);
    okf(fabs(f.alt_m - 545.4) < TOL, "样例海拔", f.alt_m, 545.4);

    /* ================= 9. 半球符号 ================= */
    printf("[9] 南纬 / 西经\n");
    gps_nmea_reset();
    feed_line("$GNGLL,2502.7600,S,11907.1400,W,081234.00,A,A*7B");
    gps_nmea_get(&f);
    ok(f.lat < 0 && f.lon < 0, "S/W 取负");

    /* ================= 10. 演示模式开关 ================= */
    printf("[10] 演示模式\n");
    gps_nmea_demo(true);
    ok(gps_nmea_is_demo(), "演示模式已开");
    gps_nmea_reset();
    gps_nmea_get(&f);
    ok(f.demo, "reset 不清 demo 标志");
    ok(!f.valid, "reset 清定位状态");

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
