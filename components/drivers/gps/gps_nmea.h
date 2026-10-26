/**
 * @file gps_nmea.h
 * @brief NMEA-0183 解析 + 定位状态机, 叠在 gps.c 的原始字节流之上.
 *
 * 分层: gps.c      = 电源 + UART + 通用 UBX 收发 (传输层)
 *       gps_nmea.c = 解析 GGA/RMC/VTG/GSA/GLL → gps_fix_t (业务层, 本文件)
 *
 * 用法:
 *   gps_init(NULL);              // 传输层 (已存在)
 *   gps_service_start();         // 起后台任务: 收字节 → 解析 → 更新 fix
 *   gps_nmea_get(&fix);          // 任意任务随时取最新定位快照 (加锁拷贝, 不阻塞)
 *
 * 无硬件时用 gps_nmea_demo(true) 打开内置运动模型, 上层代码一行不用改 ——
 * 目的: GPS 硬件未接好/未定位时, UI 仍能开发与验证.
 */
#ifndef GPS_NMEA_H
#define GPS_NMEA_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>          /* size_t (gps_nmea_feed 的 len) */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一次定位快照. 所有字段在"从未定位"时清零, valid=false. */
typedef struct {
    bool     valid;          /* 收到过定位质量 > 0 的 GGA/RMC */
    bool     has_time;       /* UTC 时间字段有效 */
    bool     has_date;       /* 日期字段有效 */

    double   lat;            /* 十进制度, 北纬为正 (GGA/RMC) */
    double   lon;            /* 十进制度, 东经为正 */

    float    alt_m;          /* 海拔 (GGA 字段 9) */
    float    geoid_m;        /* 大地水准面高差 (GGA 字段 11) */
    float    speed_mps;      /* 地速 m/s (优先 VTG, 回落 RMC) */
    float    course_deg;     /* 航向 真北 0~360 (优先 VTG, 回落 RMC) */
    float    hdop;           /* 水平精度因子 (GGA 字段 8) */

    uint8_t  fix_quality;    /* GGA 字段 6: 0=无效 1=GPS 2=DGPS 4=RTK 5=浮点RTK */
    uint8_t  sats_used;      /* GGA 字段 7: 参与解算的卫星数 */
    uint8_t  fix_dim;        /* GSA 字段 2: 1=未定位 2=2D 3=3D */

    uint8_t  hour, min, sec; /* UTC 时:分:秒 */
    uint8_t  day, month;     /* UTC 日/月 */
    uint16_t year;           /* UTC 年 (4 位) */

    uint32_t last_fix_ms;    /* 最近一次有效定位的毫秒时基 (gps_service 内部时钟) */
    uint32_t sentences;      /* 累计校验通过的 NMEA 句子数 (判断"线到底通不通") */
    uint32_t bad_checksum;   /* 累计校验失败数 (波特率/接线问题的指纹) */
    uint32_t rx_bytes;       /* 累计从 UART 收到的字节数 (0 = 线没通/模块没供电) */
    uint32_t last_rx_ms;     /* 最近一次收到字节的时基 (0 = 从未收到) */
    uint32_t ubx_frames;     /* 见到的 UBX 同步头数 (区分模块在说 NMEA 还是 UBX) */
    bool     demo;           /* 当前是否为演示数据 */
} gps_fix_t;

/* 清零状态 (含统计计数). 不清 demo 开关. */
void gps_nmea_reset(void);

/* 喂一段原始字节 (内部按行切分 + 校验和). 由 gps_service 调用, 也可自行调用做单测. */
void gps_nmea_feed(const uint8_t *buf, size_t len);

/* 取最新快照 (加锁拷贝). 返回 false 表示 out 为空/未初始化. */
bool gps_nmea_get(gps_fix_t *out);

/* ---- 逐颗卫星 (GSV + GSA) : 卫星信息页画地球用 (2026-10-24) ----
 * 星座 sys: 0=GPS 1=GLONASS 2=Galileo 3=BeiDou 4=QZSS 5=NavIC 6=混合/未知
 * snr = -1 无信噪比, elev/azim = -1 未知。 */
#define GPS_MAX_SATS 40
typedef struct {
    uint8_t sys;
    uint8_t prn;
    int8_t  elev;    /* 仰角 0..90 */
    int16_t azim;    /* 方位 0..359 (真北=0, 顺时针) */
    int8_t  snr;
    uint8_t used;    /* 1 = 参与本次解算 (GSA 点过名) */
} gps_sat_t;
/* 取当前卫星列表 (加锁拷贝), 返回条数 (≤ max)。无数据返回 0。 */
int gps_nmea_sats(gps_sat_t *out, int max);

/* 演示模式: true = 后台任务不读 UART, 改用内置运动模型生成数据. */
void gps_nmea_demo(bool on);
bool gps_nmea_is_demo(void);

/* 起后台任务: 循环 gps_read() → gps_nmea_feed(). 幂等, 重复调用不会起两个.
 * 返回 ESP_ERR_INVALID_STATE 表示 gps_init() 还没做. */
esp_err_t gps_service_start(void);
/* ★ 2026-10-09: 停掉后台解析任务 (任务自己退出)。退出地图等应用时调用, 别让任务常驻。 */
void      gps_service_stop(void);
/* 解析任务是否还在跑 (管家看门狗用: 没人在用就把模块送回待机) */
bool      gps_service_is_running(void);

#ifdef __cplusplus
}
#endif
#endif /* GPS_NMEA_H */
