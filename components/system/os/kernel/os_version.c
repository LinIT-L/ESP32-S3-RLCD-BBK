/**
 * os_version.c — 固件版本信息取用接口（唯一来源见 include/lintos_version.h）
 *
 * 版本号是**编译期常量**（改版本只改 lintos_version.h）；
 * 构建信息由 CMake 配置期注入，取不到时退化为编译时刻。
 */
#include "lintos_version.h"

#ifndef LINTOS_GIT_DESC
#define LINTOS_GIT_DESC "unknown"
#endif

#ifndef LINTOS_BUILD_STAMP
/* 没走 CMake 注入（例如直接把本文件丢进别的构建）时，用编译时刻兜底 */
#define LINTOS_BUILD_STAMP __DATE__ " " __TIME__
#endif

const char *os_version_str(void)  { return LINTOS_VERSION_STR;  }
const char *os_version_date(void) { return LINTOS_VERSION_DATE; }
const char *os_version_name(void) { return LINTOS_VERSION_NAME; }
const char *os_build_stamp(void)  { return LINTOS_BUILD_STAMP;  }
const char *os_git_desc(void)     { return LINTOS_GIT_DESC;     }
