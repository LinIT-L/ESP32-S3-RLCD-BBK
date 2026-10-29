#pragma once

/* ============================================================================
 * apppack — LAPP 单文件应用资源包解析 (V1.2)
 * ----------------------------------------------------------------------------
 * 一个 .lua 文件里同时装: 代码 + 图标 + 元信息 (格式见 docs/应用打包格式.md)。
 * 头部是**注释**行 `--@app key=value`, 因此:
 *   · 本模块可**静态解析**(不执行任何 Lua 代码) 就能拿到名称/分类/简介/图标;
 *   · Lua 解释器把头部当注释忽略 → 普通 .lua 依然直接能跑 (打包是可选增强)。
 * 分层: L1 服务层, 只依赖 stdio (不依赖 os), 供 apps 层的扫描/列表使用。
 * ==========================================================================*/
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APPPACK_ICON_BYTES 512u    /* 64x64 1bpp, MSB first, 1=黑 */

typedef struct {
    char    name[48];              /* 显示名 (UTF-8, 可中文) */
    char    cat[16];               /* 分类: 程序/游戏/系统/运维/网络/学习 (可空) */
    char    desc[96];              /* 商店列表简介 (可空) */
    char    version[12];           /* 版本 (可空) */
    /* ★ 2026-10-27 linit 要求"统一添加更详细的信息": 所有包(含资源包)都要能说明
     *   作者与上架日期。资源包没有 LAPP 头, 它们的这两个字段只存在于 index.json。 */
    char    author[32];            /* 作者 (可空, 缺省显示为 "LinTOS") */
    char    date[12];              /* 上架/更新日期 YYYY-MM-DD (可空) */
    /* ★ 2026-10-30 linit 发版规则: 商店"有没有新版"改为**按构建时间**判定。
     *   build = YYYYMMDDHHMM (12 位定长数字, 无分隔符 —— 不能带空格/冒号,
     *   否则 --@app 的 key=value 扫描会在空格处截断)。
     *   定长数字串的字典序 == 时间先后, 设备端直接 strcmp 即可, 不需要转数值。
     *   空 = 老包没有这个字段 → 商店退回按 version 比较 (向后兼容)。 */
    char    build[16];
    bool    has_icon;              /* icon[] 是否有效 */
    /* 能力声明 `--@app need=wifi`: 应用启动前需要联网。
     * 启动侧据此做门禁 (未连接 → 提示 + 复用系统"连接WiFi"流程), 见 am_lua.c。
     * 未声明 = false, 行为与老应用完全一致。 */
    bool    need_wifi;
    uint8_t icon[APPPACK_ICON_BYTES];
} apppack_meta_t;

/* 静态解析文件头部的 LAPP 元信息。
 * 返回: ESP_OK              解析成功 (至少含一条 --@app; 缺的字段留空/零)
 *       ESP_ERR_NOT_FOUND   不是 LAPP 文件 (无 --@app 行) → 调用方回退老逻辑
 *       ESP_ERR_INVALID_ARG / ESP_FAIL  参数或读文件失败 */
esp_err_t apppack_parse(const char *path, apppack_meta_t *out);
