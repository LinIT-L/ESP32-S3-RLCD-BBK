/* os_sources.h — 分发包的**镜像源清单** (固件内唯一真相源)
 *
 * 为什么要单独一个文件: 应用商店 (am_store_net.c 拉 index.json) 和
 * Lua 地图应用 (map.lua 拉 maps.json) 要下的是**同一批源**。
 * 历史上商店把主源硬编码在自己文件里 —— 再加一个消费者就会出现"两份清单",
 * 换源时改一处漏一处。抽出来后:
 *   加源 / 换源 / 调顺序 **只改 os_sources.c 一处**, 商店和 Lua 自动同步。
 *
 * ⚠ 顺序 = 设备尝试顺序, 所以国内源在前、境外源在后 (TLS 握手本身 1~3 秒/次)。
 *
 * ⚠ 位置: 放 apps/include (组件公共头) 而不是 pages/, 因为消费者跨了 pages 和 lua_bridge
 *   两个目录 —— 留在 pages/ 会让 lua_bridge 反向依赖 pages, 形成目录级循环。
 */
#pragma once

/** 源的数量 */
int os_sources_count(void);

/** 第 i 个源的前缀 (不含尾斜杠); i 越界返回 NULL */
const char *os_sources_get(int i);
