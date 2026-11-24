#!/bin/sh
# tests/backup/run_all.sh — 备份服务 (svc_backup.c) 一键复跑
#
# 测的是**生产源码** components/services/backup/svc_backup.c。
# 目录/槽数通过生产接口 bkp_policy_set() 注入 /tmp, 不修改一行生产代码。
# NVS 备份入口 (bkp_save_nvs/bkp_load_nvs) 用空桩占位 —— 属真机项。
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_backup_test}

[ -x "$CC" ] || { echo "SKIP: 找不到 $CC"; exit 2; }
if [ -d "$SDK" ]; then SYSROOT="-isysroot $SDK"; else SYSROOT=""; fi

rm -rf "$WORK"; mkdir -p "$WORK" || exit 1

INC="-I tests/backup/stub -I components/services/backup/include -I components/libs/osutil/include"
SRC="tests/backup/test_backup.c tests/backup/stub/nvs_stub.c components/services/backup/svc_backup.c components/libs/osutil/os_file.c"
FLAGS="-O2 -std=c11 -Wall -Wextra -Werror -D_DARWIN_C_SOURCE=1"

echo "=========== 1) 编译（生产 svc_backup.c + ESP 桩） ==========="
# shellcheck disable=SC2086
$CC $SYSROOT $FLAGS $INC $SRC -o "$WORK/bkp" || { echo "FAIL: 编译失败"; exit 1; }

echo
echo "=========== 2) 运行备份语义断言 ==========="
"$WORK/bkp" 2>&1 | grep -vE '^\[BKP\]' | tee "$WORK/run.log" | tail -4 || {
    echo "FAIL: 断言未通过"; exit 1; }
grep -q "ALL PASS" "$WORK/run.log" || { echo "FAIL: 未全通过 (详见 $WORK/run.log)"; exit 1; }

echo
echo "=========== 3) ASan / UBSan 复跑 ==========="
# shellcheck disable=SC2086
$CC $SYSROOT -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer -g -O1 -std=c11 -Wall -Wextra -D_DARWIN_C_SOURCE=1 \
    $INC $SRC -o "$WORK/bkp_san" || { echo "FAIL: ASan 编译失败"; exit 1; }
if "$WORK/bkp_san" > "$WORK/san.log" 2>&1; then
    grep -E "PASS=|结果" "$WORK/san.log"
else
    echo "FAIL: ASan/UBSan 报错"; tail -30 "$WORK/san.log"; exit 1
fi

echo
echo "backup: 全部通过"
echo "⚠ 未覆盖 (需真机): NVS 命名空间导出/回灌、SD 卡真正写满、拔卡瞬间"
exit 0
