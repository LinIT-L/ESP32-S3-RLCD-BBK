/* lpk_mf_test.c — manifest.json 读取/校验的单用例断言器（宿主运行）
 *
 * 用法: lpk_mf_test <file.lpk> <期望返回码> [期望 app_id] [期望 package_type]
 * 退出码: 0 = 用例通过, 1 = 不符, 2 = 参数错误
 *
 * 为什么做成"一个用例一个进程"：坏包会让解析器走不同分支，
 * 同一进程里跑多个用例容易互相污染状态，出错时也分不清是哪个包。
 */
#include "lpk_manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("用法: %s <file.lpk> <期望rc> [期望app_id] [期望package_type]\n", argv[0]);
        return 2;
    }

    lpk_mf_t m;
    char why[160] = {0};
    int want = atoi(argv[2]);
    int rc = lpk_mf_load(argv[1], &m, why, sizeof(why));
    int bad = 0;

    printf("  %-44s rc=%-3d want=%-3d", argv[1], rc, want);
    if (rc != want) bad = 1;

    if (rc == LPK_MF_OK) {
        printf("  [%s] %s type=%s rt=%s entry=%s",
               m.app_id, m.label, m.package_type, m.runtime[0] ? m.runtime : "-",
               m.entry[0] ? m.entry : "-");
        if (argc > 3 && strcmp(m.app_id, argv[3]) != 0) bad = 1;
        if (argc > 4 && strcmp(m.package_type, argv[4]) != 0) bad = 1;
        if (bad) printf("  ✗ 期望 %s/%s", argc > 3 ? argv[3] : "-", argc > 4 ? argv[4] : "-");
    } else {
        printf("  why=%s", why[0] ? why : "(无)");
    }

    printf("  ⇒ %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
