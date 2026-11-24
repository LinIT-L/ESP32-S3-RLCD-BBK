/* fault_inject.h — 只给**生产 os_file.c** 用的故障注入层
 *
 * 原理: 用 `-include` 把本文件插到 os_file.c 所有代码之前, 再用宏把
 *       remove/rename/fopen/fwrite/fflush/fclose 改道到 fi_*。
 *       fi_* 默认透传到真实 libc, 由测试按"操作名 + 第几次调用"配置何时失败。
 *
 * 为什么用宏而不是 --wrap:
 *   macOS 的 ld64 没有 GNU 的 --wrap; 宏重定向一行搞定且不依赖链接器特性。
 *
 * 为什么**不**重定向 stat:
 *   os_file.c 里有 `struct stat st;` —— 宏是纯文本替换, `struct stat` 会被改成
 *   `struct fi_stat`, 直接编译失败。"长度校验失败"场景改由 fwrite 短写触发
 *   (短写后 stat 报的真实长度 != 期望长度, 仍会走 -3 分支), 覆盖等价。
 *
 * 注意: 本文件**不得**被测试主体直接 include 宏 (用 FI_NO_REDIRECT 只取原型),
 *       否则测试自己的 fopen 也会被改道, 无法准备/检查文件。
 */
#pragma once

#include <stdio.h>
#include <sys/stat.h>

/* ---- 控制接口 (测试主体用) ---- */
void   fi_reset(void);
void   fi_fail_at(const char *op, int n);      /* 第 n 次(1 基)起**一直**失败; 0=取消 */
void   fi_fail_once(const char *op, int n);    /* 只让第 n 次失败, 之后恢复 */
void   fi_fail_always(const char *op);
void   fi_set_short_write(size_t n);           /* fwrite 每次最多写 n 字节 */
int    fi_calls(const char *op);

/* 注入点原型 (实现在 fault_inject.c) */
int    fi_remove(const char *path);
int    fi_rename(const char *oldp, const char *newp);
FILE  *fi_fopen(const char *path, const char *mode);
size_t fi_fwrite(const void *ptr, size_t size, size_t n, FILE *f);
int    fi_fflush(FILE *f);
int    fi_fclose(FILE *f);

#ifndef FI_NO_REDIRECT
#define remove fi_remove
#define rename fi_rename
#define fopen  fi_fopen
#define fwrite fi_fwrite
#define fflush fi_fflush
#define fclose fi_fclose
#endif
