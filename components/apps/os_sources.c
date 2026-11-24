/* os_sources.c — 分发包的**镜像源清单** (固件内唯一真相源, 2026-10-05)
 *
 * 消费者:
 *   · 应用商店   am_store_net.c  →  os_sources_get(0..n-1) .. "/index.json" + "/" + path
 *   · Lua 地图   map.lua         →  app.net.mirrors() 拿同一份清单, 再拼 "/maps.json"
 *
 * 设备端**按顺序回退**: 前一个连不上/超时/拿到 HTML 就换下一个, 全试完才报失败。
 * 服务器上的 index.json / maps.json 里也各带一份 mirrors[] —— 那是**运行期**的源列表,
 * 拿到后会覆盖/扩展这份编译期的引导列表 (所以"加源"通常连固件都不用刷)。
 *
 * ⚠ 这里的顺序就是尝试顺序: 国内源在前 (**Gitee raw 为主**, 国内直连最快), 境外源在后。
 * ⚠ 加 GitHub 之前先确认它**真的能下到**: 一个死链每次都会白等一次超时,
 *   拖慢整个回退过程。等真把 store/ 推上去了再取消注释。
 */
#include "os_sources.h"

static const char *const S[] = {
    /* ★★★ 2026-10-18: **自有服务器已恢复上线** ⇒ 主源换成 bbk.linit.cn(国内直连最快) ✓
     *   实测: index.json 200/2621B ✓ · pkgs/英雄坛说.lua 200/63947B(=本地大小) ✓ · 无 WordPress 301 ✓ */
    "https://bbk.linit.cn/bbk",
    /* 备源: jsDelivr CDN(GitHub 镜像) */
    "https://cdn.jsdelivr.net/gh/LinIT-L/ESP32-S3-RLCD-BBK@main/store",
    /* 兜底: GitHub raw */
    "https://raw.githubusercontent.com/LinIT-L/ESP32-S3-RLCD-BBK/main/store",
};

/* ============ 测试渠道源 (2026-11-08 linit) ============
 * 需求: 新增应用先发「测试渠道」自测, 测过了再转正; 测试渠道**只让测试版固件连**,
 *   别人刷公开固件看不到、也连不上。
 *
 * 做法: **编译期**由环境变量注入, 源码里不写死:
 *     LINTOS_TEST_SRC="https://bbk.linit.cn/p/<token>" idf.py build
 *   · 不给环境变量 = 正式固件, 二进制里根本没有这个 URL (strings 也挖不到)
 *   · 给了 = 测试固件, 该源排在「公开源之前、NVS 私有源之后」
 *   · 不写进 sdkconfig / 不写进源码 ⇒ 不会被 git 带出去 (仓库是公开的)
 * 服务器侧靠 URL 里的随机 token 做门禁 —— 本质是"靠 URL 保密", 不是加密;
 *   静态站没有鉴权, 知道完整 URL 的人就能下。这一点不夸大。
 */
#ifdef LINTOS_TEST_SRC
#  define TEST_N 1
#  define TEST_URL LINTOS_TEST_SRC
#else
#  define TEST_N 0
#  define TEST_URL ""
#endif

/* ============ 私有源 (2026-11-08 linit) ============
 * 需求: 自己做的应用上传到服务器, 但**只让本机看到**, 别人用公开固件看不到。
 * 为什么不能用常量数组: 本项目是开源的 (GitHub LinIT-L/ESP32-S3-RLCD-BBK),
 *   私有 URL 一旦写进源码 = 直接公开; 写进固件二进制也会被 strings 挖出来。
 * 做法: URL 只存在**本机 NVS**, 运行时插到源列表**最前面**(优先级最高)。
 *   · 设过私有源 → 商店只认它, 公开源看不到私有应用
 *   · 清掉私有源 → 立刻回到公开源, 别人拿到这台设备也看不出痕迹(除非会 dump NVS)
 * 命令: `store src <url>` 设置 / `store src` 查看 / `store src clear` 清除 (重启生效)
 * 安全性边界(如实说明, 不夸大): 这是"靠 URL 保密", 不是加密。
 *   服务器是纯静态站, 没有鉴权; 知道完整 URL 的人就能下载。 */
#include "svc_nvs.h"

#define PRIV_NS    "store"
#define PRIV_KEY   "priv_src"
#define PRIV_LEN   128

static char s_priv[PRIV_LEN];
static bool s_priv_loaded = false;

static void priv_load(void)
{
    if (s_priv_loaded) return;
    s_priv_loaded = true;                       /* 只读一次, 别让每次取源都碰 NVS */
    s_priv[0] = 0;
    nvs_handle_t h;
    if (svc_nvs_open(PRIV_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_priv;
    if (svc_nvs_get_str(h, PRIV_KEY, s_priv, &n) != ESP_OK) s_priv[0] = 0;
    svc_nvs_close(h);
    s_priv[sizeof s_priv - 1] = 0;
}

int os_sources_count(void) {
    priv_load();
    return (int)(sizeof(S) / sizeof(S[0])) + TEST_N + (s_priv[0] ? 1 : 0);
}

const char *os_sources_get(int i) {
    priv_load();
    if (s_priv[0]) {                 /* ① 私有源永远排第 0 位 */
        if (i == 0) return s_priv;
        i--;
    }
    if (TEST_N) {                    /* ② 编译期注入的测试渠道源 */
        if (i == 0) return TEST_URL;
        i--;
    }
    if (i < 0 || i >= (int)(sizeof(S) / sizeof(S[0]))) return 0;
    return S[i];                     /* ③ 公开源 */
}

/* 当前是否编进了测试渠道源 (给设置页/诊断页显示用; 空串 = 正式固件) */
const char *os_sources_test(void) { return TEST_URL; }

const char *os_sources_private(void) {
    priv_load();
    return s_priv[0] ? s_priv : "";
}

/* url 为空/NULL ⇒ 清除私有源, 回到公开源 */
void os_sources_set_private(const char *url)
{
    nvs_handle_t h;
    if (svc_nvs_open(PRIV_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (url && *url) svc_nvs_set_str(h, PRIV_KEY, url);
    else             svc_nvs_erase_key(h, PRIV_KEY);
    svc_nvs_commit(h);
    svc_nvs_close(h);
    s_priv_loaded = false;           /* 让下次访问重新读 */
    priv_load();
}
