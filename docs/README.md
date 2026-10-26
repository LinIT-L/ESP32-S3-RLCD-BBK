# LinTOS 文档

ESP32-S3 + ST7305 400×300 1bpp 反射屏。**一个固件通吃三块板**（小智 / 微雪 / 标准扩展）。

## 按任务找文档

| 我要做什么 | 读这份 |
|---|---|
| 开新功能、自检、remcon、构建烧录发版 | [开发指南](开发指南.md) |
| 分层 / 依赖 / 组件规范 / 资源生命周期 / 内存策略 | [架构与工程约定](架构与工程约定.md) |
| 引脚、上拉、撞脚、外设细节、换板 | [硬件与IO](硬件与IO.md) |
| 功耗、待机电流 | [省电方案](省电方案.md) |
| 写 C 应用（注册/图标/界面/弹窗/按键）| [内置应用开发](内置应用开发.md) |
| **列表 / 文本滚动** | [列表滚动规范](列表滚动规范.md) |
| 写 Lua 应用 | [Lua应用开发](Lua应用开发.md) |
| 查 `app.*` 全表 | [Lua桥API参考](Lua桥API参考.md)（生成，勿手改）|
| 存密码 / 密钥 | [加密与密钥管理](加密与密钥管理.md) |
| 读 / 写 `.ltm` 地图数据 | [地图数据格式](地图数据格式.md) |
| 文档怎么写、交付什么 | [文档规范](文档规范.md) |

## 三条真相源

1. 引脚 = `components/drivers/board_pins/board_pins.c`
2. 分区 = `partitions.csv`
3. Lua API = `components/apps/lua_bridge/luat_lib_*.c`（跑 `tools/lua_api_doc.py`）

## 烧录

- 换版本：整片 `0x0` 刷 16MB
- 只改 C：`0x60000`　只改 Lua：`sync_apps.py` 后刷 `0xC00000`
- 带 Lua 应用出整片：`LINTOS_BAKE_LUA_APPS=1 bash merge_flash.sh`

> 引脚总表、8 脚排针、撞脚警告、换板识别都在 [硬件与IO](硬件与IO.md)；
> 代码真相源 `components/drivers/board_pins/board_pins.c`。

> `tools/fonts/` 的字体源（文泉驿、霞鹜文楷）不随仓库分发。设备字库
> `components/ui/fonts/font_zh*.bin` 已在仓库内，正常编译不需要字体源；
> 设备字库 `components/ui/fonts/font_zh*.bin` 已在仓库内，编译不需要字体源。
