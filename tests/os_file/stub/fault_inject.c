/* fault_inject.c — fi_* 的实现与故障计划
 *
 * 编译时必须**不**带 -include fault_inject.h 的宏 (否则内部 fopen 会递归) ——
 * 本文件自己 #define FI_NO_REDIRECT 再 include, 只拿原型。
 */
#define FI_NO_REDIRECT 1
#include "fault_inject.h"

#include <string.h>

/* ---- 可注入的操作 ---- */
enum { OP_REMOVE, OP_RENAME, OP_FOPEN, OP_FWRITE, OP_FFLUSH, OP_FCLOSE, OP_N };

static const char *const k_opname[OP_N] = {
    "remove", "rename", "fopen", "fwrite", "fflush", "fclose",
};

typedef struct {
    int  calls;        /* 已调用次数 */
    int  fail_at;      /* 从第几次(1 基)开始**持续**失败; 0 = 不启用 */
    int  fail_once;    /* 只让第几次失败; 0 = 不启用 */
    size_t short_write;/* >0 = fwrite 只写这么多字节 (模拟短写) */
} plan_t;

static plan_t g_plan[OP_N];

static int op_index(const char *name)
{
    for (int i = 0; i < OP_N; i++)
        if (strcmp(name, k_opname[i]) == 0) return i;
    return -1;
}

/* ===================== 测试用控制接口 ===================== */

void fi_reset(void)
{
    for (int i = 0; i < OP_N; i++) {
        g_plan[i].calls = 0;
        g_plan[i].fail_at = 0;
        g_plan[i].fail_once = 0;
        g_plan[i].short_write = 0;
    }
}

/* 从第 `n` 次调用开始**一直**失败 (n=1 表示第一次就失败); n=0 取消 */
void fi_fail_at(const char *op, int n)
{
    int i = op_index(op);
    if (i >= 0) { g_plan[i].fail_at = n; g_plan[i].fail_once = 0; }
}

/* 只让第 `n` 次调用失败, 其余正常 —— 用于"注一次失败, 回滚那次要成功" */
void fi_fail_once(const char *op, int n)
{
    int i = op_index(op);
    if (i >= 0) { g_plan[i].fail_once = n; g_plan[i].fail_at = 0; }
}

/* 全部调用都失败 */
void fi_fail_always(const char *op)
{
    fi_fail_at(op, 1);
}

/* fwrite 每次最多只写 n 字节 (模拟卡满/掉卡导致的短写) */
void fi_set_short_write(size_t n)
{
    g_plan[OP_FWRITE].short_write = n;
}

int fi_calls(const char *op)
{
    int i = op_index(op);
    return (i >= 0) ? g_plan[i].calls : -1;
}

/* ===================== 注入实现 =====================
 * 失败判定规则: fail_at > 0 && calls >= fail_at
 * 先自增 calls, 再用"本次是第几次"判断, 语义直观。
 */

static int should_fail(int idx)
{
    if (g_plan[idx].fail_once > 0 && g_plan[idx].calls == g_plan[idx].fail_once)
        return 1;
    return (g_plan[idx].fail_at > 0 && g_plan[idx].calls >= g_plan[idx].fail_at);
}

int fi_remove(const char *path)
{
    int i = OP_REMOVE;
    g_plan[i].calls++;
    if (should_fail(i)) return -1;
    return remove(path);
}

int fi_rename(const char *oldp, const char *newp)
{
    int i = OP_RENAME;
    g_plan[i].calls++;
    if (should_fail(i)) return -1;
    return rename(oldp, newp);
}

FILE *fi_fopen(const char *path, const char *mode)
{
    int i = OP_FOPEN;
    g_plan[i].calls++;
    if (should_fail(i)) return NULL;
    return fopen(path, mode);
}

size_t fi_fwrite(const void *ptr, size_t size, size_t n, FILE *f)
{
    int i = OP_FWRITE;
    g_plan[i].calls++;
    if (should_fail(i)) return 0;
    if (g_plan[i].short_write > 0) {
        /* 只写一小截, 模拟"fwrite 报成功但少写"之后的长度校验分支 */
        size_t want = size * n;
        size_t give = g_plan[i].short_write;
        if (give >= want) give = want;
        size_t wrote = fwrite(ptr, 1, give, f);
        return (size ? wrote / size : wrote);
    }
    return fwrite(ptr, size, n, f);
}

int fi_fflush(FILE *f)
{
    int i = OP_FFLUSH;
    g_plan[i].calls++;
    if (should_fail(i)) return -1;
    return fflush(f);
}

int fi_fclose(FILE *f)
{
    int i = OP_FCLOSE;
    g_plan[i].calls++;
    if (should_fail(i)) { fclose(f); return -1; }   /* 真关掉句柄, 只是谎报失败 */
    return fclose(f);
}
