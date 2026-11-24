#!/bin/sh
# LPK 主机测试一键复跑
#   1) 用 tools/pack_lpk.py 打真实包（含中文路径、子目录、大文件）
#   2) 用生产源码 lpk1.c 读它，并逐字节比对源文件
#   3) 造坏包，验证每一种畸形都被**安全拒绝**而不是崩
#   4) G1 大小写兼容 / G2 读写分流 / G3 从 zip 直打包核对
#   5) 模式判定矩阵（LPK 单文件 / ZIP 目录 / 安装记录 / 各种回落）
#   6) 仙剑真实访问顺序 + 逐条 CRC 三方对齐（需要真实 pal_stored.zip）
#
# 用法: sh tests/lpk/run_all.sh [pal_stored.zip 路径]
#       不给参数则找 /tmp/pal_fix/pal_stored.zip；再没有就跳过第 6 段（会明确打印"跳过"）。
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lpk_test}
REAL_ZIP=${1:-/tmp/pal_fix/pal_stored.zip}
cd "$ROOT"

rm -rf "$WORK"
mkdir -p "$WORK/src/music" "$WORK/src/中文目录"

# --- 造测试素材 ---
printf 'local M = {}\nfunction M.run() return 42 end\nreturn M\n' > "$WORK/src/main.lua"
head -c 1022564 /dev/urandom > "$WORK/src/ABC.MKF"          # ~1MB 大文件
head -c 134704  /dev/urandom > "$WORK/src/music/9.mp3"
head -c 64      /dev/urandom > "$WORK/src/小文件.bin"
printf '中文资源内容\n'        > "$WORK/src/中文目录/说明.txt"

echo "=========== 1) 打包 ==========="
python3 tools/pack_lpk.py --lua "$WORK/src/main.lua" --assets "$WORK/src" \
        -o "$WORK/pal.lpk" --name test 2>&1 | tail -6

echo
echo "=========== 2) 检查（逐文件 CRC） ==========="
python3 tools/pack_lpk.py --inspect "$WORK/pal.lpk" --verify 2>&1 | head -12

echo
echo "=========== 3) 编译并运行读取器测试 ==========="
"$CC" -isysroot "$SDK" -O2 -std=c11 -Wall -Wextra \
      -I components/services/lpk/include \
      tests/lpk/lpk_test.c components/services/lpk/lpk1.c \
      -o "$WORK/lpk_test"
python3 tests/lpk/gen_bad.py "$WORK/pal.lpk" "$WORK/bad"
python3 tests/lpk/gen_dups.py "$WORK/bad"          # 重名 / 大小写冲突：open 时整包拒绝
"$WORK/lpk_test" "$WORK/pal.lpk" "$WORK/src" "$WORK/bad"

echo
echo "=========== 4) 解包回主机目录再逐字节比对 ==========="
python3 tools/pack_lpk.py --extract "$WORK/pal.lpk" --out "$WORK/x" >/dev/null
ok=0; bad=0
for f in "$WORK"/src/ABC.MKF "$WORK"/src/music/9.mp3 "$WORK"/src/小文件.bin "$WORK"/src/中文目录/说明.txt; do
    rel=${f#"$WORK"/src/}
    if cmp -s "$f" "$WORK/x/$rel"; then ok=$((ok+1)); else bad=$((bad+1)); echo "  ✗ $rel"; fi
done
echo "  解包比对: $ok 一致 / $bad 不一致"
[ "$bad" -eq 0 ] || exit 1

echo
echo "=========== 5) G1 大小写兼容 / 重名歧义 / 非法路径 ==========="
"$CC" -isysroot "$SDK" -O2 -std=c11 -Wall -Wextra \
      -I components/services/lpk/include \
      tests/lpk/lpk_ci_test.c components/services/lpk/lpk1.c \
      -o "$WORK/ci_test"
"$WORK/ci_test"

echo
echo "=========== 6) G2 读写分流（资源走 LPK / 存档配置走真实 FAT）==========="
"$CC" -isysroot "$SDK" -O2 -std=c11 -Wall -Wextra \
      -I components/services/lpk/include \
      tests/lpk/lpk_mount_test.c components/services/lpk/lpk1.c \
      components/services/lpk/lpk_mount.c \
      -o "$WORK/mount_test"
rm -rf "$WORK/backend" && mkdir -p "$WORK/backend"
"$WORK/mount_test" "$WORK/pal.lpk" "$WORK/backend" "$WORK/src"

echo
echo "=========== 7) G3 从 zip 直打包 + 与 zip 一一核对 ==========="
python3 - "$WORK" <<'PY'
import os, sys, zipfile
w = sys.argv[1]
src = os.path.join(w, "src")
# 故意塞两个"非资源"文件，验证默认排除表真的把它们挡在外面
open(os.path.join(src, "manifest.json"), "w").write('{"junk":1}')
open(os.path.join(src, "Thumbs.db"), "wb").write(b"\x00" * 16)
with zipfile.ZipFile(os.path.join(w, "src.zip"), "w", zipfile.ZIP_STORED) as z:
    for dp, _dn, fns in os.walk(src):
        for fn in fns:
            full = os.path.join(dp, fn)
            z.write(full, os.path.relpath(full, src).replace("\\", "/"))
print("  造 zip: %d 条目（含 2 个应被排除的）" % len(zipfile.ZipFile(os.path.join(w, "src.zip")).namelist()))
PY
python3 tools/pack_lpk.py --zip "$WORK/src.zip" -o "$WORK/fromzip.lpk" --name ziptest 2>&1 | tail -6
python3 tests/lpk/verify_pack.py "$WORK/fromzip.lpk" "$WORK/src.zip" || exit 1

echo
echo "=========== 8) 模式判定矩阵（LPK 单文件 / ZIP 目录 / 安装记录）==========="
# 造 6 个目录，覆盖：目录完整/缺项、只有包、两者都有、包被截断、空目录
M="$WORK/mode"
rm -rf "$M"
mkdir -p "$M/src" "$M/dir_ok" "$M/dir_incomplete" "$M/dir_lpk" "$M/dir_both" "$M/dir_badlpk" "$M/empty"
# 9 条必备资源（lpk_mode.c 的 REQUIRED[]，两模式共用同一份清单）
i=0
for n in ABC.MKF FBP.MKF GOP.MKF MAP.MKF MGO.MKF RNG.MKF M.MSG WORD.DAT FONTGLYPH.BIN; do
    i=$((i+1))
    head -c $((1000 + i * 37)) /dev/urandom > "$M/src/$n"
    cp "$M/src/$n" "$M/dir_ok/$n"
    cp "$M/src/$n" "$M/dir_both/$n"
    cp "$M/src/$n" "$M/dir_badlpk/$n"
    if [ "$n" != "FONTGLYPH.BIN" ]; then cp "$M/src/$n" "$M/dir_incomplete/$n"; fi
done
python3 tools/pack_lpk.py --assets "$M/src" -o "$M/dir_lpk/pal.lpk" --name modetest 2>&1 | tail -3
cp "$M/dir_lpk/pal.lpk" "$M/dir_both/pal.lpk"
# 截断包：前 4096 字节（头里声明的 total_size 与真实大小不符 ⇒ 必须被拒）
head -c 4096 "$M/dir_lpk/pal.lpk" > "$M/dir_badlpk/pal.lpk"
"$CC" -isysroot "$SDK" -O2 -std=c11 -Wall -Wextra \
      -I components/services/lpk/include \
      tests/lpk/lpk_mode_test.c components/services/lpk/lpk1.c \
      components/services/lpk/lpk_mode.c \
      -o "$WORK/mode_test"
"$WORK/mode_test" "$M/dir_ok" "$M/dir_incomplete" "$M/dir_lpk" \
                  "$M/dir_both" "$M/dir_badlpk" "$M/empty"

echo
echo "=========== 9) 仙剑真实访问顺序 + 逐条 CRC 三方对齐 ==========="
if [ -f "$REAL_ZIP" ]; then
    echo "  源包: $REAL_ZIP ($(wc -c < "$REAL_ZIP") B)"
    python3 tools/pack_lpk.py --zip "$REAL_ZIP" -o "$WORK/pal_real.lpk" --name pal 2>&1 | tail -6
    python3 tests/lpk/verify_pack.py "$WORK/pal_real.lpk" "$REAL_ZIP" || exit 1
    # 解出原件做"逐字节一致"的比对基准
    rm -rf "$WORK/pal_origin" "$WORK/pal_base"
    mkdir -p "$WORK/pal_origin" "$WORK/pal_base"
    python3 - "$REAL_ZIP" "$WORK/pal_origin" <<'PY'
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PY
    "$CC" -isysroot "$SDK" -O2 -std=c11 -Wall -Wextra \
          -I components/services/lpk/include \
          tests/lpk/lpk_pal_order_test.c components/services/lpk/lpk1.c \
          components/services/lpk/lpk_mount.c \
          -o "$WORK/pal_order_test"
    "$WORK/pal_order_test" "$WORK/pal_real.lpk" "$WORK/pal_base" "$WORK/pal_origin"
else
    echo "  ⚠ 跳过：没找到 $REAL_ZIP"
    echo "    真实包是仙剑 109 个资源的那一份（约 41.7 MB, crc32=deec847b）。"
    echo "    拿不到就别宣称「仙剑资源一致」—— 合成包只能验证逻辑，验证不了真实数据。"
fi

echo
echo "=========== 10) 句柄独立性 / 并发 / 生命周期 ==========="
if [ -f "$REAL_ZIP" ] && [ -d "$WORK/pal_origin" ]; then
    "$CC" -isysroot "$SDK" -O2 -std=c11 -Wall -Wextra \
          -I components/services/lpk/include \
          tests/lpk/lpk_handle_test.c components/services/lpk/lpk1.c \
          components/services/lpk/lpk_mount.c -pthread \
          -o "$WORK/handle_test"
    rm -rf "$WORK/handle_base" && mkdir -p "$WORK/handle_base"
    "$WORK/handle_test" "$WORK/pal_real.lpk" "$WORK/handle_base" "$WORK/pal_origin"
else
    echo "  （跳过：需要真实包，见第 9 段）"
fi

echo
echo
echo "=========== 10b) 应用包 manifest 校验（10 项）==========="
python3 tests/lpk/lpk_mf_cases.py || exit 1

echo "=========== 11) ASan / UBSan（内存与未定义行为）==========="
# 这一段的用处：句柄表、名字池、跨块读都是"指针 + 长度"的手工算术，
# 普通用例只证明"结果对"，证明不了"没越界写"。ASan 能当场抓住。
# 用小包跑（快），真实包那段在 SAN=1 时额外跑一遍。
SAN=${SAN:-1}
if [ "$SAN" = "1" ]; then
    SANFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1"
    set +e
    "$CC" -isysroot "$SDK" $SANFLAGS -std=c11 -Wall -Wextra -I components/services/lpk/include \
          tests/lpk/lpk_test.c components/services/lpk/lpk1.c -o "$WORK/lpk_test_san"
    "$WORK/lpk_test_san" "$WORK/pal.lpk" "$WORK/src" "$WORK/bad" > "$WORK/san1.log" 2>&1
    r1=$?
    tail -3 "$WORK/san1.log"
    "$CC" -isysroot "$SDK" $SANFLAGS -std=c11 -Wall -Wextra -I components/services/lpk/include \
          tests/lpk/lpk_mount_test.c components/services/lpk/lpk1.c \
          components/services/lpk/lpk_mount.c -o "$WORK/mount_test_san"
    rm -rf "$WORK/backend_san" && mkdir -p "$WORK/backend_san"
    "$WORK/mount_test_san" "$WORK/pal.lpk" "$WORK/backend_san" "$WORK/src" > "$WORK/san2.log" 2>&1
    r2=$?
    tail -3 "$WORK/san2.log"
    "$CC" -isysroot "$SDK" $SANFLAGS -std=c11 -Wall -Wextra -I components/services/lpk/include \
          tests/lpk/lpk_mode_test.c components/services/lpk/lpk1.c \
          components/services/lpk/lpk_mode.c -o "$WORK/mode_test_san"
    "$WORK/mode_test_san" "$M/dir_ok" "$M/dir_incomplete" "$M/dir_lpk" \
                          "$M/dir_both" "$M/dir_badlpk" "$M/empty" > "$WORK/san3.log" 2>&1
    r3=$?
    tail -3 "$WORK/san3.log"
    # 句柄/并发那段在 ASan 下再跑一遍（数据竞争以外的内存错误都能抓到）
    if [ -x "$WORK/handle_test" -o -f "$REAL_ZIP" ]; then
        "$CC" -isysroot "$SDK" $SANFLAGS -std=c11 -Wall -Wextra -I components/services/lpk/include \
              tests/lpk/lpk_handle_test.c components/services/lpk/lpk1.c \
              components/services/lpk/lpk_mount.c -pthread -o "$WORK/handle_san"
        rm -rf "$WORK/handle_base_san" && mkdir -p "$WORK/handle_base_san"
        "$WORK/handle_san" "$WORK/pal_real.lpk" "$WORK/handle_base_san" "$WORK/pal_origin" > "$WORK/san5.log" 2>&1
        r5=$?
        tail -3 "$WORK/san5.log"
        r5=$([ "$r5" -eq 0 ] && echo 0 || echo 1)
    else
        r5=0
    fi
    # 真实 41.7 MB 包在 ASan 下会慢不少，只在 SAN=1 且源包存在时跑
    if [ -f "$REAL_ZIP" ] && [ -x "$WORK/pal_order_test" ]; then
        "$CC" -isysroot "$SDK" $SANFLAGS -std=c11 -Wall -Wextra -I components/services/lpk/include \
              tests/lpk/lpk_pal_order_test.c components/services/lpk/lpk1.c \
              components/services/lpk/lpk_mount.c -o "$WORK/pal_order_san"
        rm -rf "$WORK/pal_base_san" && mkdir -p "$WORK/pal_base_san"
        "$WORK/pal_order_san" "$WORK/pal_real.lpk" "$WORK/pal_base_san" "$WORK/pal_origin" > "$WORK/san4.log" 2>&1
        r4=$?
        tail -3 "$WORK/san4.log"
        r4=$([ "$r4" -eq 0 ] && echo 0 || echo 1)
    else
        r4=0; echo "  （跳过真实包的 ASan：未提供源包）"
    fi
    set -e
    echo "  ASan/UBSan: 读取器=$r1 挂载=$r2 模式=$r3 真实包=$r4 句柄并发=$r5 （0 = 通过）"
    [ "$r1" -eq 0 ] && [ "$r2" -eq 0 ] && [ "$r3" -eq 0 ] && [ "$r4" -eq 0 ] && [ "$r5" -eq 0 ] || {
        echo "  ✗ sanitizer 报错，详见 $WORK/san*.log"; exit 1; }
else
    echo "  （已跳过：SAN=0）"
fi

echo
echo "全部通过"
