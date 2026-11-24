#!/bin/bash
# run_all.sh — 离线维基 主机侧测试一键复跑
#
# 覆盖：
#   gen_wiki_db.py    literal-only LZ4 基础库        → wiki_db_test   (52 项)
#   gen_real_db.py    真实 lz4.block 压缩库          → wiki_real_test (60 项, ASan)
#   mk_wiki_db.py     真实维基数据生成的生产库        → wiki_accept    (23 项)
#   test_db_integrity 破坏性检测                     (10 项)
#
# 用法：
#   bash tests/wiki/run_all.sh                    # 用 tests/wiki/data/* 跑
#   REAL_DB=/tmp/wiki_real1 bash tests/wiki/run_all.sh   # 额外验收一个真实生产库
#
# 注意：真实生产库（REAL_DB）需要用 tools/wiki/mk_wiki_db.py 从合法数据源生成。

set -e
cd "$(dirname "$0")/../.."

CC=${CC:-clang}
if [ -x /Library/Developer/CommandLineTools/usr/bin/clang ]; then
    CC=/Library/Developer/CommandLineTools/usr/bin/clang
    SDK_FLAGS="-isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"
else
    SDK_FLAGS=""
fi
OUT=${OUT:-/tmp/wikitest}
mkdir -p "$OUT"

SRC="components/services/wiki/wiki_db.c components/services/wiki/lz4_dec.c"
STUB="tests/wiki/stub/esp_stub.c"
INC="-I components/services/wiki -I tests/wiki/stub"

fail=0

build() {   # build <输出> <源文件> <额外参数...>
    local out=$1; shift
    local src=$1; shift
    echo "  编译 $(basename "$out") …"
    $CC $SDK_FLAGS $INC "$@" "$src" $SRC $STUB -o "$out" || { echo "  编译失败"; exit 1; }
}

echo "=========== 1/5 生成测试库 ==========="
python3 tests/wiki/gen_wiki_db.py > /dev/null
echo "  testlib 就绪"
python3 tests/wiki/gen_real_db.py > /dev/null
echo "  reallib 就绪"

echo
echo "=========== 2/5 基础语义测试（literal-only LZ4）==========="
build "$OUT/wt" tests/wiki/wiki_db_test.c -O2 -std=c11 -Wall -Wextra
"$OUT/wt" | tail -3 || fail=1

echo
echo "=========== 3/5 真实 LZ4 测试（ASan）==========="
build "$OUT/wr_asan" tests/wiki/wiki_real_test.c -fsanitize=address -fno-omit-frame-pointer -g -O1 -std=c11 -Wall -Wextra
"$OUT/wr_asan" 2>&1 | tail -3 || fail=1

echo
echo "=========== 4/5 损坏检测 ==========="
build "$OUT/probe" tests/wiki/wiki_open_probe.c -O2 -std=c11 -Wall -Wextra
python3 tests/wiki/test_db_integrity.py tests/wiki/data/reallib "$OUT/probe" 2>&1 | tail -3 || fail=1

echo
echo "=========== 5/5 真实生产库验收 ==========="
if [ -n "$REAL_DB" ]; then
    build "$OUT/wa" tests/wiki/wiki_accept.c -O2 -std=c11 -Wall -Wextra
    "$OUT/wa" "$REAL_DB" ${REAL_LONG_TITLE:+"$REAL_LONG_TITLE"} 2>&1 | grep -vE "^\[E|^\[W" | tail -6 || fail=1
else
    echo "  跳过（未设置 REAL_DB）"
    echo "  例： REAL_DB=/tmp/wiki_real1 REAL_LONG_TITLE=中华人民共和国 bash tests/wiki/run_all.sh"
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "===== 有测试失败 ====="
    exit 1
fi
echo "===== 全部通过 ====="
