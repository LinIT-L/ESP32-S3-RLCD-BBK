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

int os_sources_count(void) { return (int)(sizeof(S) / sizeof(S[0])); }

const char *os_sources_get(int i) {
    if (i < 0 || i >= os_sources_count()) return 0;
    return S[i];
}
