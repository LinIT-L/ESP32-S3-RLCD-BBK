/**
 * gps_nmea.c — NMEA-0183 解析 + 定位状态机 (叠在 gps.c 之上).
 *
 * 只认最常用的 5 种句子 (MAX-M10S 默认输出就是这些, 多星座前缀是 GN):
 *   GGA  定位质量 / 经纬度 / 海拔 / 卫星数 / HDOP   ← 定位有效性的主依据
 *   RMC  定位状态 / 经纬度 / 地速(节) / 航向 / UTC 日期
 *   VTG  航向 / 地速 (节 与 km/h 都给)              ← 速度优先取这个 (单位现成)
 *   GSA  定位维度 2D/3D + PDOP/HDOP/VDOP
 *   GLL  经纬度 / UTC (老模块或不发 RMC 时的兜底)
 *
 * 设计取舍:
 *   - 速度/航向**优先 VTG**, 没有 VTG 才回落 RMC(节)→m/s. 两者精度不同, 别混着平均.
 *   - 空字段 (两个逗号相连) 一律当"没有", 不要把 0 写进状态 —— 否则停车时
 *     会被误判成"速度为 0 的定位", 而实际是"这句话没带速度".
 *   - 读取方与解析方跨任务 ⇒ 全部状态用临界区保护, gps_nmea_get() 做快照拷贝.
 *   - 演示模式: 不碰 UART, 用内置运动模型造一条环岛骑行轨迹 —— GPS 硬件还没接好
 *     或室内收不到星时, 上层 UI 依然能开发和验收 (这就是"先不用靠 Garmin"那一版的前提).
 */
#include "gps_nmea.h"
#include "gps.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "GPSNMEA";

#define NMEA_LINE_MAX   128     /* 单句最长 (M10S 实际 <90), 超长直接丢 */
#define FEED_CHUNK      512
#define DEMO_TICK_MS    200

#define KNOTS_TO_MPS    0.514444f

/* ==== 状态 (跨任务共享 ⇒ 临界区保护) ==== */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static gps_fix_t    s_fix;

/* 行装配缓冲 (只在 service 任务里用, 不需要锁) */
static char    s_line[NMEA_LINE_MAX];
static int     s_line_len;

static bool    s_demo;
static bool    s_task_running;
static volatile bool s_stop_req;   /* 2026-10-09: 让服务任务优雅退出 (Lua 应用退出时用) */

/* ---- 逐颗卫星 (2026-10-24 卫星信息页) ----
 * GSV 是"多句分片"的: 每个星座每轮 1~4 句, 每句 4 组 (PRN/仰角/方位/SNR)。
 * 收齐靠: msg==1 时先清掉该星座旧条目, 后面几句往里追加。
 * GSA 给"参与解算"的 PRN 列表 → used 位图 (按星座分开, GN 混合句进 s_used_any)。 */
static gps_sat_t s_sats[GPS_MAX_SATS];
static int       s_sat_n;
/* ★ 2026-10-27: 每条卫星最后一次被 GSV 提到的时刻(ms)。用来做"超时剔除",
 *   这样一轮 GSV 未走完时数量不会掉, 卫星真消失后 15s 内自动移除。 */
static uint32_t  s_sat_ms[GPS_MAX_SATS];
#define SAT_TTL_MS 15000u
static uint64_t  s_used[6];
static uint64_t  s_used_any;
static int       s_cur_sys;        /* 当前句子所属星座 (由 talker 前缀决定) */

/* ---- 时基 ---- */
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- 临界区小工具 ---- */
static inline void fix_lock(void)   { portENTER_CRITICAL(&s_mux); }
static inline void fix_unlock(void) { portEXIT_CRITICAL(&s_mux); }

void gps_nmea_reset(void)
{
    fix_lock();
    bool demo = s_fix.demo;
    memset(&s_fix, 0, sizeof(s_fix));
    s_fix.demo = demo;
    fix_unlock();
    s_sat_n = 0;
    s_used_any = 0;
    memset(s_used, 0, sizeof(s_used));
    s_line_len = 0;
}

void gps_nmea_demo(bool on)
{
    fix_lock();
    s_fix.demo = on;
    if (on) {
        /* 演示从"已定位"起步, 免得上层还要先等 2 秒 */
        s_fix.valid       = true;
        s_fix.has_time    = true;
        s_fix.has_date    = true;
        s_fix.fix_quality = 1;
        s_fix.fix_dim     = 3;
        s_fix.sats_used   = 11;
        s_fix.hdop        = 0.8f;
    }
    fix_unlock();
    ESP_LOGD(TAG, "演示模式 %s", on ? "开 (不读 UART)" : "关");
}

bool gps_nmea_is_demo(void)
{
    fix_lock();
    bool d = s_fix.demo;
    fix_unlock();
    return d;
}

bool gps_nmea_get(gps_fix_t *out)
{
    if (!out) return false;
    fix_lock();
    *out = s_fix;
    fix_unlock();
    return true;
}

/* ================= NMEA 字段解析 ================= */

/* "ddmm.mmmm" + 半球 → 十进制度 */
static bool parse_latlon(const char *s, char hemi, double *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    double raw = strtod(s, &end);
    if (end == s || raw <= 0.0) return false;
    double deg = floor(raw / 100.0);
    double min = raw - deg * 100.0;
    double v   = deg + min / 60.0;
    if (hemi == 'S' || hemi == 's' || hemi == 'W' || hemi == 'w') v = -v;
    *out = v;
    return true;
}

/* "hhmmss.ss" → 时/分/秒 */
static bool parse_utc(const char *s, uint8_t *h, uint8_t *m, uint8_t *sec)
{
    if (!s || strlen(s) < 6) return false;
    for (int i = 0; i < 6; i++) if (s[i] < '0' || s[i] > '9') return false;
    *h   = (uint8_t)((s[0] - '0') * 10 + (s[1] - '0'));
    *m   = (uint8_t)((s[2] - '0') * 10 + (s[3] - '0'));
    *sec = (uint8_t)((s[4] - '0') * 10 + (s[5] - '0'));
    return true;
}

/* "ddmmyy" → 日/月/年(20xx) */
static bool parse_date(const char *s, uint8_t *d, uint8_t *mo, uint16_t *y)
{
    if (!s || strlen(s) < 6) return false;
    for (int i = 0; i < 6; i++) if (s[i] < '0' || s[i] > '9') return false;
    *d  = (uint8_t)((s[0] - '0') * 10 + (s[1] - '0'));
    *mo = (uint8_t)((s[2] - '0') * 10 + (s[3] - '0'));
    *y  = (uint16_t)(2000 + (s[4] - '0') * 10 + (s[5] - '0'));
    return true;
}

/* 有值才算数: 空串/解析失败 → 不动 out */
static bool parse_f(const char *s, float *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s) return false;
    *out = v;
    return true;
}

static bool parse_u(const char *s, unsigned *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) return false;
    *out = (unsigned)v;
    return true;
}

/* ---- GGA ---- */
static void on_gga(char **f, int n)
{
    if (n < 15) return;
    double lat = 0, lon = 0;
    bool ok_lat = parse_latlon(f[2], f[3] && *f[3] ? f[3][0] : 'N', &lat);
    bool ok_lon = parse_latlon(f[4], f[5] && *f[5] ? f[5][0] : 'E', &lon);

    unsigned q = 0, sats = 0;
    parse_u(f[6], &q);
    parse_u(f[7], &sats);

    float hdop = 0, alt = 0, geoid = 0;
    bool ok_hdop = parse_f(f[8], &hdop);
    bool ok_alt  = parse_f(f[9], &alt);
    bool ok_geoid = (n > 11) && parse_f(f[11], &geoid);

    uint8_t hh = 0, mm = 0, ss = 0;
    bool ok_time = parse_utc(f[1], &hh, &mm, &ss);

    fix_lock();
    if (ok_time) { s_fix.has_time = true; s_fix.hour = hh; s_fix.min = mm; s_fix.sec = ss; }
    s_fix.fix_quality = (uint8_t)q;
    s_fix.sats_used   = (uint8_t)sats;
    if (ok_hdop) s_fix.hdop = hdop;
    if (ok_alt)  s_fix.alt_m = alt;
    if (ok_geoid) s_fix.geoid_m = geoid;
    /* 质量 0 = 没定位 → 经纬度无效, 但时间/星数仍有参考价值, 照样更新 */
    if (q > 0 && ok_lat && ok_lon) {
        s_fix.lat = lat;
        s_fix.lon = lon;
        s_fix.valid = true;
        s_fix.last_fix_ms = now_ms();
    }
    fix_unlock();
}

/* ---- RMC ---- */
static void on_rmc(char **f, int n)
{
    if (n < 10) return;
    char status = f[2] && *f[2] ? f[2][0] : 'V';

    double lat = 0, lon = 0;
    bool ok_lat = parse_latlon(f[3], f[4] && *f[4] ? f[4][0] : 'N', &lat);
    bool ok_lon = parse_latlon(f[5], f[6] && *f[6] ? f[6][0] : 'E', &lon);

    float sog_knots = 0, cog = 0;
    bool ok_sog = parse_f(f[7], &sog_knots);
    bool ok_cog = parse_f(f[8], &cog);

    uint8_t hh = 0, mm = 0, ss = 0;
    bool ok_time = parse_utc(f[1], &hh, &mm, &ss);
    uint8_t dd = 0, mo = 0; uint16_t yy = 0;
    bool ok_date = parse_date(f[9], &dd, &mo, &yy);

    fix_lock();
    if (ok_time) { s_fix.has_time = true; s_fix.hour = hh; s_fix.min = mm; s_fix.sec = ss; }
    if (ok_date) { s_fix.has_date = true; s_fix.day = dd; s_fix.month = mo; s_fix.year = yy; }
    if (ok_cog)  s_fix.course_deg = fmodf(cog + 360.0f, 360.0f);
    if (status == 'A') {
        if (ok_lat && ok_lon) { s_fix.lat = lat; s_fix.lon = lon; s_fix.valid = true; s_fix.last_fix_ms = now_ms(); }
        /* VTG 没来时用 RMC 的节速兜底 */
        if (ok_sog) s_fix.speed_mps = sog_knots * KNOTS_TO_MPS;
    }
    fix_unlock();
}

/* ---- VTG ---- */
static void on_vtg(char **f, int n)
{
    if (n < 8) return;
    float course_t = 0, sog_knots = 0, sog_kmh = 0;
    bool ok_course = parse_f(f[1], &course_t);
    parse_f(f[5], &sog_knots);
    bool ok_kmh = parse_f(f[7], &sog_kmh);

    fix_lock();
    if (ok_course) s_fix.course_deg = fmodf(course_t + 360.0f, 360.0f);
    if (ok_kmh)         s_fix.speed_mps = sog_kmh / 3.6f;      /* km/h → m/s, 直接给的最准 */
    else if (sog_knots >= 0.0f) s_fix.speed_mps = sog_knots * KNOTS_TO_MPS;
    fix_unlock();
}

/* ---- GSA ---- */
static void on_gsa(char **f, int n)
{
    if (n < 18) return;
    unsigned dim = 0;
    parse_u(f[2], &dim);
    float hdop = 0;
    bool ok_hdop = parse_f(f[16], &hdop);

    /* 参与解算的 PRN 列表: 字段 3..14 */
    uint64_t mask = 0;
    for (int k = 3; k <= 14 && k < n; k++) {
        unsigned prn = 0;
        if (parse_u(f[k], &prn) && prn > 0 && prn < 64) mask |= (1ULL << prn);
    }

    fix_lock();
    if (dim >= 1 && dim <= 3) s_fix.fix_dim = (uint8_t)dim;
    if (ok_hdop && hdop > 0)  s_fix.hdop = hdop;   /* GSA 的 HDOP 比 GGA 细 */
    if (s_cur_sys >= 0 && s_cur_sys < 6) s_used[s_cur_sys] = mask;   /* 每轮覆盖, 不是累加 */
    else                                 s_used_any = mask;
    fix_unlock();
}

/* ---- GSV: 一次给出 4 组卫星 (PRN, 仰角, 方位, SNR) ---- */
static void on_gsv(char **f, int n)
{
    /* 调试: 打一条原始 GSV 看字段 (只打前 2 条, 之后静默) */
    static int dbg = 0;
    if (dbg < 2) {
        dbg++;
        char raw[160]; raw[0] = 0;
        for (int k = 0; k < n && k < 20; k++) {
            strncat(raw, f[k] ? f[k] : "(nil)", sizeof(raw) - strlen(raw) - 2);
            strncat(raw, "|", sizeof(raw) - strlen(raw) - 2);
        }
        ESP_LOGI("GPSGSV", "n=%d fields: %s", n, raw);
    }
    unsigned cur = 0;
    parse_u(f[2], &cur);                     /* f[1]=总句数, f[2]=当前第几句 */
    fix_lock();
    /* ★★★ 2026-10-27 修「卫星数量忽多忽少」(linit):
     *   原来 `cur == 1` 就把**本星座旧条目整批删掉**再从头追加 ⇒ 一轮 GSV 里
     *   s_sat_n 从 4 颗一路长到 12 颗, 任何时刻去读都可能读到中间态;
     *   多星座(GP/GL/GA/GB)轮流刷新又各自清空 ⇒ 界面上数量每秒乱跳。
     *   现在: 按 (sys,prn) **就地更新**, 不清空; 每条记录带"最后出现时刻"(s_sat_ms),
     *   超过 SAT_TTL_MS 没再被任何一条 GSV 提到才剔除 —— 数量只会在卫星真的
     *   看不见时减少, 不会因为帧边界而抖。 */
    {
        uint32_t now = (uint32_t)now_ms();
        uint8_t  sys = (uint8_t)s_cur_sys;
        for (int k = 4; k + 3 < n; k += 4) {
            unsigned prn = 0, el = 0, az = 0, sn = 0;
            if (!parse_u(f[k], &prn) || prn == 0) continue;
            bool ok_el = parse_u(f[k + 1], &el);
            bool ok_az = parse_u(f[k + 2], &az);
            bool ok_sn = parse_u(f[k + 3], &sn);
            int idx = -1;
            for (int i = 0; i < s_sat_n; i++)
                if (s_sats[i].sys == sys && s_sats[i].prn == (uint8_t)prn) { idx = i; break; }
            if (idx < 0) {
                if (s_sat_n < GPS_MAX_SATS) {
                    idx = s_sat_n++;
                } else {
                    /* 表满: 顶掉最久没更新的那条, 别把新卫星丢掉 */
                    int oldest = 0; uint32_t om = 0xFFFFFFFFu;
                    for (int i = 0; i < s_sat_n; i++)
                        if (s_sat_ms[i] < om) { om = s_sat_ms[i]; oldest = i; }
                    idx = oldest;
                }
            }
            gps_sat_t *s = &s_sats[idx];
            s->sys  = sys;
            s->prn  = (uint8_t)prn;
            s->elev = ok_el ? (int8_t)el : -1;
            s->azim = ok_az ? (int16_t)az : -1;
            s->snr  = ok_sn ? (int8_t)sn  : -1;
            s->used = 0;
            s_sat_ms[idx] = now;
        }
    }
    fix_unlock();
}

/* 取卫星列表 (加锁拷贝) */
int gps_nmea_sats(gps_sat_t *out, int max)
{
    if (!out || max <= 0) return 0;
    fix_lock();
    /* ★ 2026-10-27: 先做"超时剔除" —— 15s 没再被 GSV 提到才算真的看不见了。
     *   这一步同时把表压紧(原地搬移), 后面的拷贝就不用管空洞。 */
    {
        uint32_t now = (uint32_t)now_ms();
        int w = 0;
        for (int i = 0; i < s_sat_n; i++) {
            if ((uint32_t)(now - s_sat_ms[i]) > SAT_TTL_MS) continue;
            if (w != i) { s_sats[w] = s_sats[i]; s_sat_ms[w] = s_sat_ms[i]; }
            w++;
        }
        s_sat_n = w;
    }
    int n = (s_sat_n < max) ? s_sat_n : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_sats[i];
        uint64_t m = s_used_any;
        if (s_sats[i].sys < 6) m |= s_used[s_sats[i].sys];
        out[i].used = (uint8_t)(((m >> (s_sats[i].prn & 63)) & 1ULL) ? 1 : 0);
    }
    fix_unlock();
    return n;
}

/* ---- GLL ---- */
static void on_gll(char **f, int n)
{
    if (n < 6) return;
    char status = f[6] && *f[6] ? f[6][0] : 'V';
    double lat = 0, lon = 0;
    bool ok_lat = parse_latlon(f[1], f[2] && *f[2] ? f[2][0] : 'N', &lat);
    bool ok_lon = parse_latlon(f[3], f[4] && *f[4] ? f[4][0] : 'E', &lon);
    if (status != 'A' || !ok_lat || !ok_lon) return;

    uint8_t hh = 0, mm = 0, ss = 0;
    bool ok_time = (n > 5) && parse_utc(f[5], &hh, &mm, &ss);

    fix_lock();
    s_fix.lat = lat; s_fix.lon = lon;
    s_fix.valid = true; s_fix.last_fix_ms = now_ms();
    if (ok_time) { s_fix.has_time = true; s_fix.hour = hh; s_fix.min = mm; s_fix.sec = ss; }
    fix_unlock();
}

/* ---- 一句完整 NMEA (以 '$' 开头, 无 CR/LF) ---- */
static void parse_sentence(char *line)
{
    char *star = strchr(line, '*');
    if (!star || star == line) return;

    /* 校验和: '$' 与 '*' 之间所有字符的 XOR */
    uint8_t cs = 0;
    for (char *p = line + 1; p < star; p++) cs ^= (uint8_t)*p;
    unsigned given = 0;
    if (sscanf(star + 1, "%2x", &given) != 1 || (uint8_t)given != cs) {
        fix_lock(); s_fix.bad_checksum++; fix_unlock();
        return;
    }
    *star = '\0';

    /* 切字段: f[0] = "$GNGGA" 整头, f[1..] = 各字段 */
    char *f[24];
    int n = 0;
    f[n++] = line + 1;
    for (char *p = line + 1; *p && n < 24; p++) {
        if (*p == ',') { *p = '\0'; f[n++] = p + 1; }
    }
    if (strlen(f[0]) < 5) return;

    fix_lock(); s_fix.sentences++; fix_unlock();

    /* talker 前缀 → 星座 (GSV/GSA 用来区分"哪一套星") */
    s_cur_sys = 6;
    if      (f[0][0] == 'G' && f[0][1] == 'P') s_cur_sys = 0;
    else if (f[0][0] == 'G' && f[0][1] == 'L') s_cur_sys = 1;
    else if (f[0][0] == 'G' && f[0][1] == 'A') s_cur_sys = 2;
    else if (f[0][0] == 'B' && (f[0][1] == 'D' || f[0][1] == 'D')) s_cur_sys = 3;
    else if (f[0][0] == 'G' && f[0][1] == 'Q') s_cur_sys = 4;
    else if (f[0][0] == 'G' && f[0][1] == 'I') s_cur_sys = 5;

    const char *type = f[0] + 2;          /* 跳过 2 字符 talker (GP/GN/GL/BD…) */
    if      (!strcmp(type, "GGA")) on_gga(f, n);
    else if (!strcmp(type, "RMC")) on_rmc(f, n);
    else if (!strcmp(type, "VTG")) on_vtg(f, n);
    else if (!strcmp(type, "GSA")) on_gsa(f, n);
    else if (!strcmp(type, "GSV")) on_gsv(f, n);
    else if (!strcmp(type, "GLL")) on_gll(f, n);
}

void gps_nmea_feed(const uint8_t *buf, size_t len)
{
    if (!buf) return;
    /* 线路活性统计: rx_bytes=0 说明模块根本没说话(没焊/没电/TX-RX 没交叉);
     * age_rx 一直变大说明中途掉线。同时在输入里数 UBX 同步头。 */
    {
        uint32_t ubx = 0;
        for (size_t i = 0; i + 1 < len; i++)
            if (buf[i] == 0xB5 && buf[i + 1] == 0x62) ubx++;
        fix_lock();
        s_fix.rx_bytes += (uint32_t)len;
        s_fix.last_rx_ms = now_ms();
        s_fix.ubx_frames += ubx;
        fix_unlock();
    }
    for (size_t i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (c == '\n' || c == '\r') {
            if (s_line_len > 0) {
                s_line[s_line_len] = '\0';
                if (s_line[0] == '$') parse_sentence(s_line);
                s_line_len = 0;
            }
            continue;
        }
        if (s_line_len >= NMEA_LINE_MAX - 1) { s_line_len = 0; continue; }  /* 超长 → 丢, 重新对齐 */
        s_line[s_line_len++] = c;
    }
}

/* ================= 演示模式: 内置运动模型 =================
 * 示例轨迹: 起点 39.9042N 116.4074E, 航向匀速右转画圈, 速度带加减速波动.
 * 目的只有一个 —— 让上层 UI 在没星/没接线时也能跑出真实感的数据流. */
#define DEMO_LAT0   25.046000
#define DEMO_LON0   119.119000
#define DEMO_SPEED  6.0f          /* 巡航 m/s ≈ 21.6 km/h */
#define DEMO_TURN   4.0f          /* 度/秒 转向率 → 一圈 90s */
#define DEMO_ALT    32.0f

static void demo_step(void)
{
    static double lat = DEMO_LAT0, lon = DEMO_LON0;
    static float  course = 0.0f, t = 0.0f;

    t += DEMO_TICK_MS / 1000.0f;
    float speed = DEMO_SPEED + 1.6f * sinf(t * 0.45f);   /* 加减速波动 */
    course = fmodf(course + DEMO_TURN * (DEMO_TICK_MS / 1000.0f), 360.0f);

    /* 按航向推进 (米 → 度; 纬度 1°≈111320m, 经度按 cos(lat) 收缩) */
    double dist = (double)speed * (DEMO_TICK_MS / 1000.0);
    double rad  = course * (float)M_PI / 180.0;
    lat += (dist * cos(rad)) / 111320.0;
    lon += (dist * sin(rad)) / (111320.0 * cos(lat * M_PI / 180.0));

    fix_lock();
    s_fix.demo        = true;
    s_fix.valid       = true;
    s_fix.lat         = lat;
    s_fix.lon         = lon;
    s_fix.alt_m       = DEMO_ALT + 1.5f * sinf(t * 0.12f);
    s_fix.speed_mps   = speed;
    s_fix.course_deg  = course;
    s_fix.fix_quality = 1;
    s_fix.fix_dim     = 3;
    s_fix.sats_used   = 10 + (uint8_t)(2 + 2 * sinf(t * 0.07f));
    s_fix.hdop        = 0.7f + 0.25f * sinf(t * 0.3f);
    s_fix.last_fix_ms = now_ms();
    /* UTC 时间跟着跑 (演示就够用, 不追求与真实日期对齐) */
    if (!s_fix.has_time) { s_fix.has_time = true; s_fix.hour = 8; s_fix.min = 0; s_fix.sec = 0; }
    else {
        if (++s_fix.sec >= 60) { s_fix.sec = 0; if (++s_fix.min >= 60) { s_fix.min = 0; s_fix.hour = (uint8_t)((s_fix.hour + 1) % 24); } }
    }
    s_fix.sentences++;

    /* 演示星座: 18 颗, 分属 GPS/GLONASS/BeiDou/Galileo, 方位缓慢转动 → 卫星页能看出"在动" */
    {
        static const uint8_t dsys[] = { 0,0,0,0,0,0,0, 1,1,1,1, 3,3,3,3,3, 2,2 };
        static const uint8_t dprn[] = { 3,6,11,17,19,22,28, 65,72,78,81, 7,19,24,30,38, 5,11 };
        int n = (int)(sizeof(dsys) / sizeof(dsys[0]));
        if (n > GPS_MAX_SATS) n = GPS_MAX_SATS;
        s_sat_n = n;
        for (int i = 0; i < n; i++) {
            float ph = t * 0.22f + (float)i * 0.83f;
            s_sats[i].sys  = dsys[i];
            s_sats[i].prn  = dprn[i];
            s_sats[i].elev = (int8_t)(12 + (int)(68.0f * (0.5f + 0.5f * sinf(ph))));
            s_sats[i].azim = (int16_t)fmodf((float)i * 41.0f + t * 7.0f, 360.0f);
            s_sats[i].snr  = (int8_t)(i < 5 ? 0 : (int)(22.0f + 20.0f * (0.5f + 0.5f * sinf(ph * 1.6f))));
            s_sats[i].used = (uint8_t)((i < 9) ? 1 : 0);
        }
        s_used_any = 0;
        memset(s_used, 0, sizeof(s_used));
    }
    fix_unlock();
}

/* ================= 后台任务 ================= */
static void gps_service_task(void *arg)
{
    (void)arg;
    uint8_t buf[FEED_CHUNK];
    ESP_LOGI(TAG, "服务任务启动 (%s)", s_demo ? "演示模式" : "读 UART");

    while (!s_stop_req) {              /* ★ 2026-10-09: 可被 gps_service_stop() 叫停 */
        if (s_demo) {
            demo_step();
            vTaskDelay(pdMS_TO_TICKS(DEMO_TICK_MS));
            continue;
        }
        int n = gps_read(buf, sizeof(buf), 200);   /* 无数据时 200ms 超时返回 */
        if (n > 0) {
            gps_nmea_feed(buf, (size_t)n);
        } else if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));        /* 驱动未就绪 → 退避, 别忙等 */
        }
    }
    s_task_running = false;
    s_stop_req = false;
    ESP_LOGI(TAG, "服务任务已停止");
    vTaskDelete(NULL);
}

/* ★ 2026-10-09: 停服务 (供 Lua 应用退出时调用)。最多等 ~600ms 让任务自己退出。 */
bool gps_service_is_running(void) { return s_task_running; }

void gps_service_stop(void)
{
    if (!s_task_running) return;
    s_stop_req = true;
    for (int i = 0; i < 60 && s_task_running; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t gps_service_start(void)
{
    if (!gps_is_ready()) {
        ESP_LOGD(TAG, "gps_init() 尚未成功, 不能起服务");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task_running) return ESP_OK;

    if (gps_nmea_is_demo()) {
        s_task_running = true;
        xTaskCreate(gps_service_task, "gps_svc", 3072, NULL, 4, NULL);
        return ESP_OK;
    }

    /* 先探一下线到底通不通: 收到任意 NMEA 字节就说明波特率/接线对。
     * ★ 2026-10-24: 窗口 1200ms → 600ms。gps_wakeup() 已经叫醒模块, 1Hz NMEA
     *   在 600ms 内必然吐 1 句以上; 这 600ms 是**同步**的(卡应用打开),
     *   原来 1.2s 让码表/地图开门明显顿一下。收到 64 字节即提前收工。 */
    uint8_t probe[256];
    int got = 0;
    uint32_t t0 = now_ms();
    while (now_ms() - t0 < 600 && got < 64) {
        int n = gps_read(probe, sizeof(probe), 150);
        if (n > 0) {
            got += n;
            gps_nmea_feed(probe, (size_t)n);
        }
    }
    if (got == 0) {
        ESP_LOGD(TAG, "1.2s 内没收到任何字节 —— 检查 V_IO(pin7) 是否接 3V3 / TX-RX 是否交叉 / 波特率");
    } else {
        ESP_LOGI(TAG, "线路探测 OK: 收到 %d 字节", got);
    }

    s_task_running = true;
    xTaskCreate(gps_service_task, "gps_svc", 3072, NULL, 4, NULL);
    return ESP_OK;
}
