/* test_engine_mgr.c — 引擎管理器状态机主机测试
 *
 * 测的是**生产源码** components/system/engine_manager/engine_manager.c。
 * 做法: 提供与生产同名的 8 组引擎 load/unload 桩 (engine_manager.c 里是 extern),
 *       原样编译生产 .c —— 状态机、幂等、失败回滚、unload_all 全是生产代码在跑,
 *       桩只负责"记录被调用次数 / 按测试要求返回成功或失败 / 模拟内部是否真的释放"。
 *
 * ⚠ 能力边界: 本测试证明**管理器自身的状态机**正确, 不能证明某个引擎在真机上
 *   真的把内存还给了堆 (那取决于引擎内部与 FreeRTOS 任务是否退出)。
 *   恰恰因为如此, E14/E15 用"引擎侧仍持有资源"的桩来暴露管理器与引擎之间的
 *   **接口语义缺口** —— 这个缺口在真机上正是"任务 100ms 未退出 ⇒ 跳过释放"。
 */
#include "engine_manager.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "st7305.h"

#include <stdio.h>
#include <string.h>

bool g_test_log_verbose = false;

/* ---------------- ESP 桩实现 ---------------- */

static size_t g_heap_int = 200 * 1024, g_heap_ps = 7 * 1024 * 1024;

void t_heap_set(size_t internal_free, size_t psram_free)
{
    g_heap_int = internal_free;
    g_heap_ps = psram_free;
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? g_heap_ps : g_heap_int;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return heap_caps_get_free_size(caps) / 2;
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:                return "ESP_OK";
    case ESP_FAIL:              return "ESP_FAIL";
    case ESP_ERR_NO_MEM:        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:   return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    default:                    return "ESP_ERR_UNKNOWN";
    }
}

/* ---------------- 引擎桩 ---------------- */

typedef struct {
    int load_calls;
    int unload_calls;
    int load_fail_times;      /* 接下来 N 次 load 返回失败 */
    int leave_live_on_fail;   /* load 失败时是否留下已分配资源 (模拟"初始化半途失败") */
    int unload_skip_release;  /* unload 内部是否提前 return (模拟"任务未退出, 跳过释放") */
    int still_live;           /* 引擎侧是否仍持有资源 (1=没释放) */
} eng_t;

static eng_t g_eng[ENGINE_COUNT];

static esp_err_t eng_load(engine_id_t id)
{
    g_eng[id].load_calls++;
    if (g_eng[id].load_fail_times > 0) {
        g_eng[id].load_fail_times--;
        if (g_eng[id].leave_live_on_fail) g_eng[id].still_live = 1;
        return ESP_FAIL;
    }
    g_eng[id].still_live = 1;
    return ESP_OK;
}

static void eng_unload(engine_id_t id)
{
    g_eng[id].unload_calls++;
    if (g_eng[id].unload_skip_release) return;   /* ← 真机 nes/gbc/md/arduboy 的行为 */
    g_eng[id].still_live = 0;
}

/* 与 engine_manager.c 的 extern 声明逐字对应 */
void gam4980_emu_background_init(st7305_handle_t *lcd) { (void)lcd; (void)eng_load(ENGINE_GAM4980); }
void gam4980_emu_unload(void)                          { eng_unload(ENGINE_GAM4980); }
esp_err_t gb_emu_background_init(void)                 { return eng_load(ENGINE_GB); }
void gb_emu_unload(void)                               { eng_unload(ENGINE_GB); }
esp_err_t nes_emu_background_init(void)                { return eng_load(ENGINE_NES); }
void nes_emu_unload(void)                              { eng_unload(ENGINE_NES); }
esp_err_t arduboy_avr_background_init(st7305_handle_t *lcd) { (void)lcd; return eng_load(ENGINE_ARDUBOY); }
void arduboy_avr_unload(void)                          { eng_unload(ENGINE_ARDUBOY); }
esp_err_t lavax_emu_background_init(st7305_handle_t *lcd)   { (void)lcd; return eng_load(ENGINE_LAVAX); }
void lavax_emu_unload(void)                            { eng_unload(ENGINE_LAVAX); }
esp_err_t vpet_emu_background_init(st7305_handle_t *lcd)    { (void)lcd; return eng_load(ENGINE_VPET); }
void vpet_emu_unload(void)                             { eng_unload(ENGINE_VPET); }
esp_err_t md_emu_background_init(void)                 { return eng_load(ENGINE_MD); }
void md_emu_unload(void)                               { eng_unload(ENGINE_MD); }
esp_err_t sms_emu_background_init(void)                { return eng_load(ENGINE_SMS); }
void sms_emu_unload(void)                              { eng_unload(ENGINE_SMS); }

/* ---------------- 断言框架 ---------------- */

static int g_pass = 0, g_fail = 0;

#define SEC(name) do { printf("\n== %s ==\n", (name)); } while (0)
#define CK(cond) do { \
        if (cond) g_pass++; \
        else { g_fail++; printf("  FAIL %s:%d  %s\n", __func__, __LINE__, #cond); } \
    } while (0)
#define CK_EQ(a, b) do { \
        long _a = (long)(a), _b = (long)(b); \
        if (_a == _b) g_pass++; \
        else { g_fail++; printf("  FAIL %s:%d  %s(=%ld) != %s(=%ld)\n", \
                                __func__, __LINE__, #a, _a, #b, _b); } \
    } while (0)

static void reset_all(void)
{
    memset(g_eng, 0, sizeof g_eng);
    /* 让管理器回到"全部未加载": 逐个 unload (已加载的才动) */
    engine_manager_unload_all(false);
    memset(g_eng, 0, sizeof g_eng);
    t_heap_set(200 * 1024, 7 * 1024 * 1024);
}

int main(void)
{
    printf("engine_manager 状态机测试 (生产源码 engine_manager.c)\n");

    SEC("E1 初始状态: 全部未加载");
    reset_all();
    {
        int all_false = 1;
        for (int i = 0; i < ENGINE_COUNT; i++)
            if (engine_manager_is_loaded((engine_id_t)i)) all_false = 0;
        CK(all_false);
        CK(!engine_manager_is_loaded((engine_id_t)-1));
        CK(!engine_manager_is_loaded((engine_id_t)ENGINE_COUNT));
    }

    SEC("E2 加载成功 (NES)");
    reset_all();
    CK_EQ(engine_manager_load(ENGINE_NES, NULL), ESP_OK);
    CK(engine_manager_is_loaded(ENGINE_NES));
    CK_EQ(g_eng[ENGINE_NES].load_calls, 1);

    SEC("E3 重复加载 → 幂等 (不重复调 load)");
    CK_EQ(engine_manager_load(ENGINE_NES, NULL), ESP_OK);
    CK_EQ(g_eng[ENGINE_NES].load_calls, 1);
    CK(engine_manager_is_loaded(ENGINE_NES));

    SEC("E4 加载失败 → 标记回滚, 可重试 (第4回合修的点)");
    reset_all();
    g_eng[ENGINE_GB].load_fail_times = 1;
    CK_EQ(engine_manager_load(ENGINE_GB, NULL), ESP_FAIL);
    CK(!engine_manager_is_loaded(ENGINE_GB));      /* ★ 不得留下 s_loaded=true */
    CK_EQ(g_eng[ENGINE_GB].load_calls, 1);

    SEC("E5 失败后再次加载 → 必须能成功 (不能永久坏死)");
    CK_EQ(engine_manager_load(ENGINE_GB, NULL), ESP_OK);
    CK(engine_manager_is_loaded(ENGINE_GB));
    CK_EQ(g_eng[ENGINE_GB].load_calls, 2);

    SEC("E6 未加载就 unload → 一个字都不动");
    reset_all();
    engine_manager_unload(ENGINE_SMS);
    CK_EQ(g_eng[ENGINE_SMS].unload_calls, 0);      /* 引擎的 unload 对未 init 实例不安全 */

    SEC("E7 正常卸载");
    reset_all();
    CK_EQ(engine_manager_load(ENGINE_SMS, NULL), ESP_OK);
    engine_manager_unload(ENGINE_SMS);
    CK(!engine_manager_is_loaded(ENGINE_SMS));
    CK_EQ(g_eng[ENGINE_SMS].unload_calls, 1);
    CK_EQ(g_eng[ENGINE_SMS].still_live, 0);

    SEC("E8 重复卸载 → 幂等 (第二次不调引擎 unload)");
    engine_manager_unload(ENGINE_SMS);
    CK_EQ(g_eng[ENGINE_SMS].unload_calls, 1);

    SEC("E9 连续加载/卸载 10 轮");
    reset_all();
    {
        int ok = 1;
        for (int i = 0; i < 10; i++) {
            if (engine_manager_load(ENGINE_NES, NULL) != ESP_OK) { ok = 0; break; }
            if (!engine_manager_is_loaded(ENGINE_NES)) { ok = 0; break; }
            engine_manager_unload(ENGINE_NES);
            if (engine_manager_is_loaded(ENGINE_NES)) { ok = 0; break; }
        }
        CK(ok);
        CK_EQ(g_eng[ENGINE_NES].load_calls, 10);
        CK_EQ(g_eng[ENGINE_NES].unload_calls, 10);
    }

    SEC("E10 unload_all: 只卸载已加载的");
    reset_all();
    CK_EQ(engine_manager_load(ENGINE_NES, NULL), ESP_OK);
    CK_EQ(engine_manager_load(ENGINE_GB, NULL), ESP_OK);
    engine_manager_unload_all(false);
    CK_EQ(g_eng[ENGINE_NES].unload_calls, 1);
    CK_EQ(g_eng[ENGINE_GB].unload_calls, 1);
    CK_EQ(g_eng[ENGINE_MD].unload_calls, 0);       /* 没加载过的不许碰 */
    CK_EQ(g_eng[ENGINE_SMS].unload_calls, 0);

    SEC("E11 引擎切换: 加载 A+B, 只卸载 A → B 仍在");
    reset_all();
    CK_EQ(engine_manager_load(ENGINE_NES, NULL), ESP_OK);
    CK_EQ(engine_manager_load(ENGINE_GB, NULL), ESP_OK);
    engine_manager_unload(ENGINE_NES);
    CK(!engine_manager_is_loaded(ENGINE_NES));
    CK(engine_manager_is_loaded(ENGINE_GB));
    CK_EQ(g_eng[ENGINE_GB].unload_calls, 0);

    SEC("E12 越界 id / 未注册引擎");
    reset_all();
    CK_EQ(engine_manager_load((engine_id_t)-1, NULL), ESP_ERR_INVALID_ARG);
    CK_EQ(engine_manager_load((engine_id_t)ENGINE_COUNT, NULL), ESP_ERR_INVALID_ARG);
    CK_EQ(engine_manager_load(ENGINE_MESHTASIC, NULL), ESP_ERR_INVALID_ARG);  /* 表项未注册 */
    engine_manager_unload((engine_id_t)-1);        /* 不得崩 */
    engine_manager_unload((engine_id_t)ENGINE_COUNT);
    CK(!engine_manager_is_loaded(ENGINE_MESHTASIC));

    SEC("E13 初始化半途失败 → 管理器回滚标记 (引擎内部残留由引擎自负)");
    reset_all();
    g_eng[ENGINE_LAVAX].load_fail_times = 1;
    g_eng[ENGINE_LAVAX].leave_live_on_fail = 1;
    CK_EQ(engine_manager_load(ENGINE_LAVAX, NULL), ESP_FAIL);
    CK(!engine_manager_is_loaded(ENGINE_LAVAX));
    /* ★ 记录: 引擎侧 still_live=1 说明它自己已分配了资源。管理器无法回收 ——
     *   这是"引擎 load 失败时必须自己清干净"的契约, 管理器不做二次释放 (否则重复释放)。
     *   真机上需靠各引擎 load 内的错误分支自查。 */
    CK_EQ(g_eng[ENGINE_LAVAX].still_live, 1);

    /* ================= 核心: 管理器与引擎的接口语义缺口 ================= */

    SEC("E14 ★ unload 内部跳过释放 (任务 100ms 未退出) → 管理器仍视为已卸载");
    reset_all();
    g_eng[ENGINE_NES].unload_skip_release = 1;     /* 真机 nes/gbc/md/arduboy 的确这么做 */
    CK_EQ(engine_manager_load(ENGINE_NES, NULL), ESP_OK);
    engine_manager_unload(ENGINE_NES);
    CK_EQ(g_eng[ENGINE_NES].unload_calls, 1);
    CK(!engine_manager_is_loaded(ENGINE_NES));     /* 管理器视角: 已卸载 */
    CK_EQ(g_eng[ENGINE_NES].still_live, 1);        /* 引擎视角: 资源还在 */
    /* ★★ 两者的不一致就是缺陷本体: unload() 返回 void, 管理器拿不到失败信号,
     *    只能无条件 s_loaded[id] = false。而 engine_manager.c:148-155 的注释
     *    写的是"没成功的留给下一次 unload_all()/退出页面再试一次" —— 与实现不符。 */

    SEC("E15 ★ 后续 unload_all 不再给它第二次机会 (注释承诺未实现)");
    engine_manager_unload_all(false);
    CK_EQ(g_eng[ENGINE_NES].unload_calls, 1);      /* 仍是 1: 完全没重试 */
    CK_EQ(g_eng[ENGINE_NES].still_live, 1);        /* 资源残留到整机复位 */
    /* ★★ 这是可复现的证据: 第4回合把"先置 false"改成"后置 false"确实修正了
     *    "unload 执行期间标记已为 false"的时序问题, 但**没有**实现注释所说的重试。
     *    要实现重试, unload 必须能回传成功/失败 (改签名 = ABI 变更, 需单独立项)。 */

    SEC("E16 unload_all(true) 走泄漏复核分支不崩");
    reset_all();
    CK_EQ(engine_manager_load(ENGINE_GB, NULL), ESP_OK);
    t_heap_set(100 * 1024, 6 * 1024 * 1024);       /* 空闲骤降 → 触发 [LEAK] 日志路径 */
    g_test_log_verbose = false;
    engine_manager_unload_all(true);
    CK(!engine_manager_is_loaded(ENGINE_GB));
    /* 第二次调用: 基线已建立, 走对比分支, 同样不得崩 */
    engine_manager_unload_all(true);
    CK(1);

    SEC("E17 gam4980 异步加载: load 返回 void ⇒ 管理器无法感知失败");
    reset_all();
    CK_EQ(engine_manager_load(ENGINE_GAM4980, NULL), ESP_OK);   /* 恒 OK, 见 engine_manager.c:77 */
    CK(engine_manager_is_loaded(ENGINE_GAM4980));
    /* ★ 已知设计 (注释已说明: 返回时 retro_init 还在后台任务里跑)。记录: 若后台
     *   init 失败, 管理器与页面都不知道, 用户看到的是"进去黑屏"。属既有设计,
     *   本回合不判缺陷, 列入真机观察项。 */

    printf("\n---------------------------------------------\n");
    printf("PASS=%d  FAIL=%d  合计=%d\n", g_pass, g_fail, g_pass + g_fail);
    printf("%s\n", g_fail ? "结果: FAILED" : "结果: ALL PASS");
    return g_fail ? 1 : 0;
}
