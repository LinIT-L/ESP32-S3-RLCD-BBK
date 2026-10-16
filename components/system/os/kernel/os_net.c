/**
 * os_net.c — 系统网络分析 API 实现.
 *
 * os_net_mac_vendor: 按 MAC 的 OUI(前 3 字节) 查 IEEE 厂商库, 供 netdect 主机列表显示品牌.
 *
 * 数据链路:
 *   macoui_lookup() ← macoui.c ← oui_db.bin (wifi_manager 组件 EMBED_FILES 内嵌)
 *   格式: [4B "OUID"][u32 count][u32 strblob_size][count*7B 记录][NUL 结尾厂商名池]
 *   查询: 对按 OUI 升序排列的记录做**二分查**; 运行时只存 3 个指针 + count (RAM ≈ 13 B)。
 *
 * 历史: V1.0.9x 曾为省 ~288KB flash 把字典摘掉, 只留空桩恒返回 false (调用方签名不变;
 *       netdect 的厂商显示逻辑全部保留至今)。2026-09-22 分区调整
 *       (apps 5.625MB→2MB, factory 4MB→7.625MB) 后空间充裕, 在此重新接回。
 * 归层: services=2 < system=3, 故 os → wifi_manager 属**下行依赖**, 合规。
 */
#include "os_net.h"
#include "macoui.h"

bool os_net_mac_vendor(const uint8_t mac[6], const char **vendor) {
    if (!mac || !vendor) return false;
    return macoui_lookup(mac, vendor);   /* macoui 内部只读 oui[0..2] */
}
