/**
 * lintos_version.h — LinTOS 固件版本 ★ 单一来源 (Single Source of Truth)
 *
 * 规则 (2026-10-02 定)：
 *   ① **改版本号只改本文件**。其余任何地方一律调 `os_version_str()` 等接口取用，
 *      ⛔ 不许再写死版本字符串 —— 旧代码在「设置→系统信息」里硬编码过 "1.3"，
 *      而代码注释一路涨到 V1.7.x，导致设备上显示的版本是错的。
 *   ② 发版时在 `CHANGELOG.md` 顶部加一节（标题 `## vX.Y.Z`），
 *      `tools/version_check.py` 会校验"本文件 ↔ CHANGELOG 顶部"一致。
 *   ③ 构建信息 (git 描述 / 构建时刻) 由 `components/system/os/CMakeLists.txt`
 *      在 **CMake 配置期** 注入；取不到时退化（git 描述 = "unknown"，
 *      构建时刻 = 本文件编译时刻 `__DATE__ __TIME__`）。
 *      ⚠ 所以 `os_build_stamp()` 反映的是**最近一次 cmake 重新配置**的时刻，
 *      增量编译（不重配）时它是旧的 —— 判"烧的是哪版"请以 `os_git_desc()` 为准。
 */
#pragma once

/* ===== 版本号 (唯一来源) ===== */
#define LINTOS_VERSION_MAJOR   1
#define LINTOS_VERSION_MINOR   7
#define LINTOS_VERSION_PATCH   0
#define LINTOS_VERSION_STR     "1.7.0"
#define LINTOS_VERSION_DATE    "2026-10-02"
#define LINTOS_VERSION_NAME    "\xe5\x8f\x8c\xe6\x89\x8b\xe6\x9f\x84\xe6\xa1\xa3\xe6\xa1\x88\xe8\xa1\xa8"  /* 本版主题: 双手柄档案表 */

/* ===== 取用接口 (实现: kernel/os_version.c) ===== */
const char *os_version_str(void);    /* "1.7.0" */
const char *os_version_date(void);   /* "2026-10-02" */
const char *os_version_name(void);   /* 本版主题 (一句话) */
const char *os_build_stamp(void);    /* "2026-10-02 10:58" 最近一次 cmake 配置时刻 */
const char *os_git_desc(void);       /* "v1.7.0-3-g88140ce-dirty" / "unknown" */
