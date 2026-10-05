/* ============================================================
 * store_parse_test.c — 在**宿主机**上编译固件真正的解析器 (am_store_json.c) 打一遍
 *
 * 为什么值得单独做: index.json 解析是这一环里唯一"错了不崩、只静默变空列表"的地方
 * (字段名写错 / 转义没解对 → 商店永远空着, 串口也只有一行 FAIL)。所以不靠"看着对",
 * 直接把固件那份 .c 拿 clang 编出来, 喂真实索引 + 一堆畸形输入。
 *
 * 用法: store_parse_test <index.json>
 * 输出 (给 tools/store_selftest.py 断言用):
 *   rc=<int>
 *   mir_n=<int>
 *   mir=<url>            (每行一个)
 *   skip=<int>
 *   pkg=<name>|<label>|<cat>|<size>|<crc8hex>|<path>
 * 注意: 本文件不链接 ESP-IDF —— am_store_json.c 刻意不依赖任何 OS 头。
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include "am_store_json.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法: %s <index.json>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "打不开 %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); fprintf(stderr, "文件长度异常 %ld\n", sz); return 2; }
    char *buf = malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    static am_rpkg_t pk[AM_RPKG_MAX];
    static char mir[AM_MIRROR_MAX][AM_MIRROR_LEN];
    int pn = 0, mn = 0;
    int rc = am_store_json_parse(buf, pk, AM_RPKG_MAX, &pn, mir, AM_MIRROR_MAX, &mn);

    printf("rc=%d\n", rc);
    printf("mir_n=%d\n", mn);
    for (int i = 0; i < mn; i++) printf("mir=%s\n", mir[i]);
    printf("skip=%d\n", am_store_json_skipped());
    for (int i = 0; i < pn; i++)
        printf("pkg=%s|%s|%s|%u|%08x|%s\n",
               pk[i].name, pk[i].label, pk[i].cat,
               (unsigned)pk[i].size, (unsigned)pk[i].crc, pk[i].path);
    free(buf);
    return 0;
}
