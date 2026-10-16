/**
 * lintos_version.h — LinTOS 固件版本 ★ 单一来源 (Single Source of Truth)
 *
 * ── 两套版本号 (2026-10-02 linit 定) ────────────────────────────────────────
 *  ① **对外版本** `LINTOS_VERSION_PUBLIC`  —— 给外面看的, 形式 `主.次` (如 "1.3")。
 *     ⛔ **只在"攒够改动、推给外面"时才改**。平时不动, 保持与已发布的一致。
 *  ② **内部版本** `主.次.修订` (如 "1.3.1") —— 自测/烧机/复现/对比用。
 *     每完成一个改动批次 `修订+1`; 再往后细分的粒度**由 git 自动给出**:
 *     显示成 `1.3.1+12` 里的 `+12` = 距最近内部 tag 的提交数 (不用手改)。
 *
 * ── 为什么这么分 ──────────────────────────────────────────────────────────
 *   以前一版直接跳到 1.7.0, 而外面才到 1.3 —— 幅度太大, 数字失去参考意义。
 *   现在: 内部小步走 (1.3.1 → 1.3.2 → …), 对外只在推送时跨 (1.3 → 1.4)。
 *
 * ── 规则 ─────────────────────────────────────────────────────────────────
 *   ① 改版本号**只改本文件**; 其余地方一律调 `os_version_*()` 取用。
 *      ⛔ 不许再写死 —— 旧代码在「设置→系统信息」里硬编码过 "1.3", 而代码注释
 *      一路涨到 V1.7.x, 设备上显示的版本与实际固件长期不符。
 *   ② 内部迭代在 `CHANGELOG.md` 顶部记一节; `tools/version_check.py` 校验
 *      "本文件 ↔ CHANGELOG ↔ git tag" 一致。
 *   ③ 构建信息 (git 描述 / 迭代数 / 构建时刻) 由 `components/system/os/CMakeLists.txt`
 *      在 **CMake 配置期** 注入; 取不到时退化(真值请以 `os_git_desc()` 为准)。
 *   ④ 小步发版用 `tools/version_bump.py` (自动改版本号 + 建 CHANGELOG 小节 + 提示打 tag)。
 */
#pragma once

/* ===== 对外版本 (只往 外面 推送时改) ===== */
#define LINTOS_VERSION_PUBLIC  "1.4"

/* ===== 内部版本 (自测用, 小步走) ===== */
#define LINTOS_VERSION_MAJOR   1
#define LINTOS_VERSION_MINOR   4
#define LINTOS_VERSION_PATCH   0
#define LINTOS_VERSION_STR     "1.3.1"    /* 内部三段: 主.次.修订 */
#define LINTOS_VERSION_DATE    "2026-10-02"
#define LINTOS_VERSION_NAME    "\xe5\x8f\x8c\xe6\x89\x8b\xe6\x9f\x84\xe6\xa1\xa3\xe6\xa1\x88\xe8\xa1\xa8"  /* 本次迭代主题: 双手柄档案表 */

/* ===== 取用接口 (实现: kernel/os_version.c) ===== */
const char *os_version_public(void);  /* 对外: "1.3" */
const char *os_version_str(void);     /* 内部三段: "1.3.1" */
const char *os_version_iter(void);    /* 内部迭代号(距最近 tag 的提交数): "12", 未知为 "0" */
const char *os_version_full(void);    /* 内部完整: "1.3.1+12" (自测/断言用) */
const char *os_version_date(void);    /* "2026-10-02" */
const char *os_version_name(void);    /* 本次迭代主题 */
const char *os_build_stamp(void);     /* "2026-10-02 11:00" 最近一次 cmake 配置时刻 */
const char *os_git_desc(void);        /* "v1.3.1-12-g88140ce-dirty" / "unknown" */
