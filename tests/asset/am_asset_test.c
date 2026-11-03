/* am_asset_test.c — 资源包解包审核（主机侧，模拟固件栈限制）
 *
 * 为什么要限制栈：ESP32 上解包跑在 `am_dl_task` 里，栈 20480 B。
 *   主机默认栈是 MB 级，直接跑永远"成功"，测不出真机的问题。
 *   这里用 pthread 显式把栈设成指定大小，让"栈不够"在主机上也能复现成崩溃。
 *
 * 用法：
 *   ./asset_test <zip> <目标目录> <栈KB>
 * 输出：
 *   RESULT OK|FAIL files=<解出文件数> bytes=<总字节> ms=<耗时>
 * 退出码： 0=成功  1=解包返回失败  139/其他=崩溃（栈溢出等）
 *
 * 编译见 tests/asset/run_asset_audit.sh
 */
#include "am_asset.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

typedef struct {
    const char *zip;
    const char *dst;
    int ok;
} job_t;

static void *worker(void *arg) {
    job_t *j = (job_t *)arg;
    j->ok = am_asset_install(j->zip, j->dst) ? 1 : 0;
    return NULL;
}

/* 递归统计目录里的文件数与总字节 */
static void walk(const char *dir, int *nfiles, long long *bytes) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[1024];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) walk(p, nfiles, bytes);
        else { (*nfiles)++; *bytes += (long long)st.st_size; }
    }
    closedir(d);
}

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "用法: %s <zip> <目标目录> <栈KB>\n", argv[0]);
        return 2;
    }
    const char *zip = argv[1];
    const char *dst = argv[2];
    int stack_kb = atoi(argv[3]);
    if (stack_kb < 16) stack_kb = 16;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* 显式指定栈：这就是固件上的约束 */
    if (pthread_attr_setstacksize(&attr, (size_t)stack_kb * 1024) != 0) {
        fprintf(stderr, "设置栈 %dKB 失败\n", stack_kb);
        return 2;
    }

    job_t j = { zip, dst, 0 };
    pthread_t t;
    double t0 = now_ms();
    if (pthread_create(&t, &attr, worker, &j) != 0) {
        fprintf(stderr, "创建线程失败（栈 %dKB 可能太小）\n", stack_kb);
        return 3;
    }
    pthread_join(t, NULL);
    double dt = now_ms() - t0;

    int nf = 0;
    long long bytes = 0;
    walk(dst, &nf, &bytes);

    printf("RESULT %s files=%d bytes=%lld ms=%.0f stack=%dKB\n",
           j.ok ? "OK" : "FAIL", nf, bytes, dt, stack_kb);
    return j.ok ? 0 : 1;
}
