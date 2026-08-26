#ifndef APP_META_H
#define APP_META_H
#include <stdint.h>

/* === 应用 / 模块元数据 (Phase A 扩展接口预留) ===
 *
 * 本文定义: 模块向系统声明的能力 (权限), 以及模块类型的扩展位.
 * 用途:
 *   - 应用管理页据此展示每类模块需要的能力;
 *   - 未来加入"资源包商店/壁纸程序/电子白板"等新模块时, 只需在注册表
 *     s_modules[] 增加一条带 caps 的条目即可接入, 不改动本文件以下结构.
 *   - 权限采用"声明式 + 高位掩码", 便于后续按需授权与审计.
 *
 * 注意: 位值一经发布不得复用/重排 (旧配置位图依赖稳定语义).
 */

/* 模块申请的系统能力 (权限位). 使用 uint32_t, 预留高位置给未来能力. */
typedef enum {
    APP_CAP_SCREEN   = (1u << 0),   /* 屏幕渲染 */
    APP_CAP_AUDIO    = (1u << 1),   /* 音频输出 */
    APP_CAP_TOUCH    = (1u << 2),   /* 触摸输入 */
    APP_CAP_STORAGE  = (1u << 3),   /* TF 卡 / 存储 */
    APP_CAP_INPUT    = (1u << 4),   /* 按键 / 手柄 */
    APP_CAP_BT       = (1u << 5),   /* 蓝牙 */
    APP_CAP_NETWORK  = (1u << 6),   /* 联网 (未来资源包商店) */
    APP_CAP_USB      = (1u << 7),   /* USB HID / MSC */
    /* 预留能力位 (高位, 未来扩展): 8..31 可作为所需走读/反射/渲染器等
     * 能力申请命名; 已占用高位定义见下说明, 不得复用. */
    APP_CAP_ALL      = 0xFFFFFFFFu,
} app_cap_t;

/* 模块/应用分类 (与 bbk_module_kind_t 对应, 供未来商店/权限审计使用).
 * 此枚举仅作元数据, 不替代注册表的 kind 字段. */
typedef enum {
    APP_KIND_FORCED   = 0,   /* 内置强制 */
    APP_KIND_REGULAR  = 1,   /* 常规可开关 */
    APP_KIND_GAME     = 2,   /* 游戏引擎 (彩蛋/可装) */
    APP_KIND_RESOURCE = 3,   /* 未来: 资源包 (壁纸/主题/白板模板) */
} app_kind_t;

/* 模块版本/校验 (未来资源包下载用, 现预留给商店) */
typedef struct {
    uint32_t        ver_major;
    uint32_t        ver_minor;
    uint32_t        ver_patch;
    uint64_t        size_bytes;    /* 资源包总大小 */
    uint32_t        crc32;         /* 资源包校验 */
} app_version_t;

#ifdef __cplusplus
extern "C" {
#endif

/* 能力位到可读名称描述 (给应用管理 UI 展示). 返回静态字符串, 无需释放. */
const char *app_cap_describe(uint32_t caps);

#ifdef __cplusplus
}
#endif

#endif /* APP_META_H */