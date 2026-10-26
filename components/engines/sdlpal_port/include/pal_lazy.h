/* ★ 2026-10-25 linit: 仙剑大缓冲懒分配 (见 pal_lazy.c) */
#pragma once
#include <stddef.h>

void *pal_lazy_get(void **slot, size_t size);
void  pal_lazy_release_all(void);
