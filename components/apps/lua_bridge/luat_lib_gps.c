/* luat_lib_gps.c — LinTOS Lua 桥 · gps 模块
 *
 * 暴露 gps_nmea.c 的定位快照给 Lua 应用 (自行车码表最先用它)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       子表自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "gps")。
 *
 * ★ 数值一律走**整数定标**, 不走浮点 —— 本项目 Lua 编的是 LUA_32BITS=1,
 *   lua_Number 是 **float**, 只 ~7 位有效数字。直接推 double 经纬度
 *   (119.119000 就 9 位) 会被截断, 位置会漂。所以:
 *     经纬度 → 微度 (×1e6)    速度 → 0.1km/h (×10) 与 cm/s (×100)
 *     航向   → 0.1° (×10)     海拔 → 米 与 分米       HDOP → ×100
 *   显示时整数除法自己拼小数点即可, 全程不丢精度。
 */
#include "lua_bridge.h"
#include "gps.h"
#include "gps_nmea.h"
#include "hw_cfg.h"        /* ★ 2026-10-26: 启动成功时补记"已识别" */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ==================== 异步启动 (2026-11-23) ====================
 * 现场: 打开矢量地图要等 4.5s, 其中 **3.74s 全在 gps_init 的自动波特率扫描**
 * (6 档 × 600ms; 本机没有 GPS 模块, 必然全落空)。日志证据:
 *   LUAPROF script on_enter +3744636 us
 *   W (19772) GPS: 未检测到 GPS 模块(6 档波特率均无 UBX/NMEA 应答)   ← 同一毫秒
 * 这跟地图数据、Lua 编译都无关, 是"打开应用被 GPS 探测堵住"。
 *
 * 做法: 新增 app.gps.start_async() —— 立即返回, 探测+唤醒+起任务全丢后台 (core 1),
 * 界面先出来; 脚本照旧轮询 app.gps.fix(), 定位到了自然有数据。
 * app.gps.start() **语义不变**(同步), 码表这类"失败就走模拟"的脚本不受影响。
 * 另外驱动侧记住"本次开机扫不到模块"(gps_module_absent), 之后任何应用都不再白等。 */
static volatile bool s_start_busy   = false;
static volatile bool s_start_cancel = false;

/* ★ 2026-11-23: 三态判定全部走 NVS 持久化记录（hw_cfg 的 gps_probed + det 位）:
 *   ① !probed                = 待识别（新固件第一次 / 手动"重新识别"）
 *   ②  probed && det         = 已安装 —— 可直接唤醒使用，不必再扫波特率找"有没有"
 *   ③  probed && !det        = 未安装 —— 任何应用调 start 都立刻 false, 一个字节都不发
 *   原来靠 gps_module_absent()（只在本次开机有效）⇒ 每次重启后的第一次打开仍要白扫
 *   3.7 秒。改成持久化后，没焊的板子从第二次开机起彻底零开销。 */
static bool gps_mod_absent(void)
{
    return hw_cfg_gps_probed() && !hw_cfg_detected(HW_GPS);
}

static void gps_start_task(void *arg)
{
    (void)arg;
    esp_err_t e = ESP_OK;
    if (!gps_is_ready()) {
        e = gps_init(NULL);
        if (!hw_cfg_gps_probed()) hw_cfg_set_gps_present(e == ESP_OK);  /* 首次探测落盘 */
    }
    if (e == ESP_OK) e = gps_wakeup();
    if (e == ESP_OK) e = gps_service_start();
    /* 与同步路径同样的补记 (启动成功 ⇒ 已识别/已启用), 只补"未识别"的情况 */
    if (e == ESP_OK && !hw_cfg_detected(HW_GPS)) {
        hw_cfg_set_gps_present(true);
        ESP_LOGW("GPS", "异步启动成功 ⇒ 补记已识别/已启用");
    }
    if (s_start_cancel) {          /* 启动期间应用已退出: 收尾把模块送回待机 */
        gps_service_stop();
        gps_release();
        s_start_cancel = false;
    }
    s_start_busy = false;
    vTaskDelete(NULL);
}

/* app.gps.start() → bool: 初始化 UART、**唤醒模块**、起后台解析任务 (幂等)
 * ★ 2026-10-09 修: 原来漏了 gps_wakeup() —— 开机时 app_board 已经把 GPS 送进
 *   "软件待机(46µA)", 不唤醒的话它是睡着的, 服务起来的 1.2s 线路探测一个字节都
 *   收不到 ⇒ 应用永远显示"无 GPS 数据"。启动顺序必须是 唤醒 → 再起服务。 */
static int l_gps_start(lua_State *L) {
    /* ★ 2026-11-23: 已确认没装（NVS 持久化）⇒ 直接失败, 不再白扫 3.7s。
     *   语义不变 (仍然返回 false, 码表照旧走模拟)。 */
    if (!gps_is_ready() && gps_mod_absent()) {
        lua_pushboolean(L, false);
        return 1;
    }
    esp_err_t e = ESP_OK;
    if (!gps_is_ready()) {
        e = gps_init(NULL);
        /* 首次探测（开机路径没跑过就到这，例如用户刚点了"重新识别"）结果落盘 */
        if (!hw_cfg_gps_probed()) hw_cfg_set_gps_present(e == ESP_OK);
    }
    if (e == ESP_OK) e = gps_wakeup();
    if (e == ESP_OK) e = gps_service_start();

    /* ★★ 2026-10-26 linit 兜底: 启动成功就把"已识别/已启用"补记回去。
     *   背景: 开机一次性识别时模块冷启动慢 / 波特率乱 ⇒ 被记成"未识别",
     *   hw_cfg 把 GPS 位清掉并标记 probed ⇒ 之后每次开机“什么都不做”
     *   (连待机都不送, 模块可能 10mA 空转), 设置页也一直显示未启用。
     *   现在: 只要这次从 GPS 应用里真起来了(init+wakeup+service 全部成功),
     *   就把"已识别 + 已启用"写回 ⇒ 设置页正确, 下次开机恢复 boot_park 送待机。
     *   ⚠ 只在"未识别"(detected=0) 时补记; 用户手动关掉的(detected=1/enabled=0) 绝不覆盖。 */
    if (e == ESP_OK && !hw_cfg_detected(HW_GPS)) {
        hw_cfg_set_gps_present(true);
        ESP_LOGW("GPS", "启动成功 ⇒ 补记已识别/已启用 (下次开机会自动送待机)");
    }

    lua_pushboolean(L, e == ESP_OK);
    return 1;
}

/* app.gps.start_async() → bool: 立即返回, 不阻塞 UI。
 *   · 已就绪 / 后台已在启动 → true (视为"已受理")
 *   · 本次开机已确认无模块 → false (调用方应直接按"无 GPS"处理)
 *   · 首次探测 → 起后台任务 (core 1) 后立刻返回 true
 * 真值含义变了(原来是"模块可用"), 所以**没有**改 start() —— 只有明确理解这一点的
 * 脚本才用它; 需要"不可用就走模拟"的仍用 start()。 */
static int l_gps_start_async(lua_State *L) {
    if (gps_is_ready()) {
        gps_wakeup();
        gps_service_start();
        lua_pushboolean(L, true);
        return 1;
    }
    if (gps_mod_absent()) {
        ESP_LOGI("GPS", "已确认本板未安装 GPS ⇒ start_async 立即返回 false (不碰 UART)");
        lua_pushboolean(L, false);
        return 1;
    }
    if (s_start_busy) { lua_pushboolean(L, true); return 1; }
    s_start_cancel = false;
    s_start_busy   = true;
    if (xTaskCreatePinnedToCore(gps_start_task, "gps_start", 6144, NULL, 3, NULL, 1) != pdPASS) {
        s_start_busy = false;
        ESP_LOGE("GPS", "gps_start 任务创建失败(内部 RAM 不足?)");
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, true);
    return 1;
}

/* app.gps.stop() → bool: 停后台解析任务 + **把模块送回待机** (2026-10-09)
 * 应用退出时务必调用: 否则解析任务常驻, 且 GPS 会保持 ~10mA 连续跟踪
 * (待机只需 46µA) —— 直接吃电池。 */
static int l_gps_stop(lua_State *L) {
    /* ★ 2026-11-23: 后台启动还没结束就被要求停 ⇒ 让任务收尾时自己释放,
     *   否则会出现"应用已退出、GPS 探测成功后在后台常驻耗电"。 */
    if (s_start_busy) s_start_cancel = true;
    gps_service_stop();
    gps_release();      /* ★ 2026-10-24: 送待机 + 卸 UART 驱动 + 还原引脚 (内部 RAM 全归还) */
    lua_pushboolean(L, true);
    return 1;
}

/* app.gps.fix() → table | nil
 * 全是整数定标值 (见文件头), 括号里是还原公式。 */
static int l_gps_fix(lua_State *L) {
    gps_fix_t f;
    if (!gps_nmea_get(&f)) { lua_pushnil(L); return 1; }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    lua_newtable(L);
#define PUT_INT(k, v) do { lua_pushinteger(L, (lua_Integer)(v)); lua_setfield(L, -2, (k)); } while (0)
#define PUT_BOOL(k, v) do { lua_pushboolean(L, (v)); lua_setfield(L, -2, (k)); } while (0)

    PUT_BOOL("valid",  f.valid);
    PUT_BOOL("demo",   f.demo);
    PUT_BOOL("has_time", f.has_time);
    PUT_BOOL("has_date", f.has_date);

    PUT_INT("lat_e6", (int32_t)(f.lat * 1000000.0));        /* 微度 */
    PUT_INT("lon_e6", (int32_t)(f.lon * 1000000.0));

    PUT_INT("kmh10",   (int)(f.speed_mps * 3.6f * 10.0f + 0.5f));  /* 0.1 km/h */
    PUT_INT("mps100",  (int)(f.speed_mps * 100.0f + 0.5f));        /* cm/s */
    PUT_INT("crs10",   (int)(f.course_deg * 10.0f + 0.5f));        /* 0.1 度 */
    PUT_INT("alt_m",   (int)(f.alt_m + 0.5f));
    PUT_INT("alt_dm",  (int)(f.alt_m * 10.0f + 0.5f));             /* 分米 */
    PUT_INT("hdop100", (int)(f.hdop * 100.0f + 0.5f));

    PUT_INT("sats",    f.sats_used);
    PUT_INT("quality", f.fix_quality);
    PUT_INT("dim",     f.fix_dim);

    PUT_INT("hh", f.hour);  PUT_INT("mm", f.min);  PUT_INT("ss", f.sec);
    PUT_INT("yy", f.year);  PUT_INT("mo", f.month); PUT_INT("dd", f.day);

    /* 距上次有效定位多久 (ms); 从未定位 = -1 */
    PUT_INT("age_ms", f.last_fix_ms ? (int32_t)(now - f.last_fix_ms) : -1);
    PUT_INT("sentences", (int32_t)f.sentences);   /* 收了多少句 → 判断线通不通 */
    PUT_INT("badcs",     (int32_t)f.bad_checksum); /* 校验失败数 → 波特率/接线的指纹 */
    PUT_INT("rx",        (int32_t)f.rx_bytes);     /* UART 收到的总字节数 (0 = 线没通) */
    PUT_INT("ubx",       (int32_t)f.ubx_frames);   /* UBX 同步头数 */
    PUT_INT("age_rx",    f.last_rx_ms ? (int32_t)(now - f.last_rx_ms) : -1);
    PUT_INT("baud",      (int32_t)gps_baud());     /* 实际生效波特率 */
    PUT_BOOL("link",     f.last_rx_ms && (now - f.last_rx_ms) < 5000);  /* 5s 内有字节 */
    lua_pushstring(L, f.sentences ? "NMEA" : (f.ubx_frames ? "UBX" : "-"));
    lua_setfield(L, -2, "proto");

#undef PUT_INT
#undef PUT_BOOL
    return 1;
}

/* app.gps.demo([on]) → bool: 开/关演示数据 (不读 UART); 不传参 = 查询 */
static int l_gps_demo(lua_State *L) {
    if (lua_gettop(L) >= 1) gps_nmea_demo(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, gps_nmea_is_demo());
    return 1;
}

/* app.gps.reset(): 清定位状态与统计 (不关演示开关) */
static int l_gps_reset(lua_State *L) {
    (void)L;
    gps_nmea_reset();
    return 0;
}

/* app.gps.sats() → 数组, 每项 {sys,prn,el,az,snr,used}
 * 卫星信息页画地球用。sys: 0=GPS 1=GLONASS 2=Galileo 3=BeiDou 4=QZSS 5=NavIC 6=混合
 * el=仰角 0..90, az=方位 0..359 (真北=0 顺时针), snr=-1 表示无, used=1 参与解算。 */
static int l_gps_sats(lua_State *L) {
    static gps_sat_t v[GPS_MAX_SATS];
    int n = gps_nmea_sats(v, GPS_MAX_SATS);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, v[i].sys);  lua_setfield(L, -2, "sys");
        lua_pushinteger(L, v[i].prn);  lua_setfield(L, -2, "prn");
        lua_pushinteger(L, v[i].elev); lua_setfield(L, -2, "el");
        lua_pushinteger(L, v[i].azim); lua_setfield(L, -2, "az");
        lua_pushinteger(L, v[i].snr);  lua_setfield(L, -2, "snr");
        lua_pushboolean(L, v[i].used != 0); lua_setfield(L, -2, "used");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* app.gps.start(): 同步版本, 语义不变 (返回 true 才算模块可用)。
 * app.gps.start_async(): 立即返回, 探测在后台 —— 给"打开就要出画面"的应用用。 */

void lua_mod_gps_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_gps_start);   lua_setfield(L, -2, "start");
    lua_pushcfunction(L, l_gps_start_async); lua_setfield(L, -2, "start_async");
    lua_pushcfunction(L, l_gps_stop);    lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, l_gps_fix);     lua_setfield(L, -2, "fix");
    lua_pushcfunction(L, l_gps_sats);    lua_setfield(L, -2, "sats");
    lua_pushcfunction(L, l_gps_demo);    lua_setfield(L, -2, "demo");
    lua_pushcfunction(L, l_gps_reset);   lua_setfield(L, -2, "reset");
    lua_setfield(L, -2, "gps");
}
