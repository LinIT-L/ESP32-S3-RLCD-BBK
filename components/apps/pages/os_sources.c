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
 * ⚠ 这里的顺序就是尝试顺序: 国内源在前 (自有站最快), 境外源在后。
 * ⚠ 加 GitHub 之前先确认它**真的能下到**: 一个死链每次都会白等一次超时,
 *   拖慢整个回退过程。等真把 store/ 推上去了再取消注释。
 */
#include "os_sources.h"

static const char *const S[] = {
    /* 主源: linit 自有站 (阿里云国内节点) */
    "https://bbk.linit.cn/bbk",
    /* 备源: Gitee raw (国内可达, 与应用商店的备源是同一个仓库) */
    "https://gitee.com/LinIT-L/ESP32-S3-RLCD-BBK/raw/master/store",
    /* 备源: GitHub raw / jsDelivr CDN —— **等真推上去了再启用** */
    /* "https://raw.githubusercontent.com/LinIT-L/ESP32-S3-RLCD-BBK/main/store", */
    /* "https://cdn.jsdelivr.net/gh/LinIT-L/ESP32-S3-RLCD-BBK@main/store", */
};

int os_sources_count(void) { return (int)(sizeof(S) / sizeof(S[0])); }

const char *os_sources_get(int i) {
    if (i < 0 || i >= os_sources_count()) return 0;
    return S[i];
}
