/* wiki_open_probe.c — 极小的开库探针，供 test_db_integrity.py 做损坏检测
 *
 * 输出一行： OPEN <篇数|0>
 * 退出码： 0 = 打开成功；1 = 拒绝打开
 *
 * 用法： ./probe <库目录>
 */
#include "wiki_db.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) { printf("OPEN 0\n"); return 1; }
    int n = wiki_open(argv[1]);
    printf("OPEN %d\n", n > 0 ? n : 0);
    if (n > 0) {
        /* 顺手读一篇，确认不是"能开但读不了" */
        int lines = wiki_load(0);
        printf("LOAD %d\n", lines);
        wiki_close();
        return 0;
    }
    return 1;
}
