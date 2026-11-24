/* lua_life_types.h — 只给 lua_bridge.h 通过的最小类型集合
 *
 * lua_bridge.h 会 include 三十多个 ESP/Lua 头。本测试不编译任何模块实现,
 * 只编译 lua_bridge.c (两张函数表 + setup + reset 调度), 因此这些头只需
 * 提供类型定义, 其余全空。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

typedef struct lua_State lua_State;

typedef struct st7305_dev_s *st7305_handle_t;
typedef struct ui_ctx_s ui_ctx_t;
typedef int nvs_handle_t;

/* lua_bridge.h 里有 `extern kbd_t s_kbd;`(按值) ⇒ 必须是完整类型 */
typedef struct { int dummy; } kbd_t;
typedef struct { int dummy; } kbd_geom_t;
