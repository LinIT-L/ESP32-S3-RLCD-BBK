/**
 * os_version.c — 固件版本信息取用接口（唯一来源见 include/lintos_version.h）
 *
 * 版本号是**编译期常量**（改版本只改 lintos_version.h）；
 * 构建信息（git 描述 / 迭代号 / 构建时刻）由 CMake 配置期注入，取不到时退化。
 */
#include <stdio.h>
#include "lintos_version.h"

#ifndef LINTOS_GIT_DESC
#define LINTOS_GIT_DESC "unknown"
#endif

#ifndef LINTOS_GIT_ITER
#define LINTOS_GIT_ITER "0"
#endif

#ifndef LINTOS_BUILD_STAMP
/* 没走 CMake 注入（例如直接把本文件丢进别的构建）时，用编译时刻兜底 */
#define LINTOS_BUILD_STAMP __DATE__ " " __TIME__
#endif

/* 内部完整版本 "1.3.1+12": 三段 + git 迭代号。
 * 用静态缓冲缓存 —— 本函数可能被渲染路径反复调用，不做重复 snprintf。 */
static char s_full[24];

const char *os_version_public(void) { return LINTOS_VERSION_PUBLIC; }
const char *os_version_str(void)    { return LINTOS_VERSION_STR;    }
const char *os_version_iter(void)   { return LINTOS_GIT_ITER;       }
const char *os_version_date(void)   { return LINTOS_VERSION_DATE;   }
const char *os_version_name(void)   { return LINTOS_VERSION_NAME;   }
const char *os_build_stamp(void)    { return LINTOS_BUILD_STAMP;    }
const char *os_git_desc(void)       { return LINTOS_GIT_DESC;       }

const char *os_version_full(void)
{
    if (!s_full[0]) snprintf(s_full, sizeof(s_full), "%s+%s", LINTOS_VERSION_STR, LINTOS_GIT_ITER);
    return s_full;
}
