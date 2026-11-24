#!/bin/sh
# 安装器（am_lpkg.c）宿主机行为测试 —— 一键复跑
#
# 与 tests/store_state 互补：
#   store_state 测**判定**（纯逻辑，零依赖）
#   installer   测**动作**：真写文件、真改名、真回滚、真冷启动恢复
#
# 做法：用 -DLPKG_ROOT 把安装根指到临时目录，编译**同一份生产源码** am_lpkg.c；
#   每个用例一个独立进程（= 设备重启后的冷启动），用例之间清空根目录。
#
# 覆盖：首次安装 / 手动安装 / 更新 / 记录写失败回滚 / 坏包回滚 / 掉电自愈 /
#       冷启动恢复 / 老记录兼容 / 无记录但有目录 / 记录损坏 / 多点 app_id / 主干取值
#
# 用法: sh tests/installer/run_all.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_installer_test}
APPROOT="$WORK/root"
cd "$ROOT"

rm -rf "$WORK"
mkdir -p "$WORK/src" "$WORK/idx"
echo '{"ver":1,"pkgs":[]}' > "$WORK/idx/empty.json"   # 打包工具的重复检查用空索引，别碰仓库里的

# ---------- 造包 ----------
mkapp() {   # $1=目录 $2=app_id $3=版本 $4=build $5=入口内容 $6=输出
    mkdir -p "$1/app"
    cat > "$1/manifest.json" <<EOF
{
  "schema_version": 1,
  "app_id": "$2",
  "name": "T",
  "label": "测试应用",
  "description": "installer test",
  "category": "software",
  "package_type": "app",
  "runtime": "lua",
  "entry": "app/entry.lua",
  "version": "$3",
  "build": "$4",
  "author": "test",
  "dependencies": [],
  "permissions": [],
  "data_policy": "preserve",
  "min_firmware": "2.0.0"
}
EOF
    printf '%s\n' "$5" > "$1/app/entry.lua"
    python3 tools/pack_lpk.py --app "$1" -o "$6" --index "$WORK/idx/empty.json" >/dev/null
}

mkapp "$WORK/src/v1" com.test.app 1 202610010000 "-- V1"        "$WORK/v1.lpk"
mkapp "$WORK/src/v2" com.test.app 2 202611011048 "-- V2"        "$WORK/v2.lpk"
mkapp "$WORK/src/dots" com.example.deep.nest 1 202610010000 "-- V1" "$WORK/dots.lpk"
head -c 1500 "$WORK/v1.lpk" > "$WORK/bad.lpk"          # 截断包：头里 total_size 与实际不符

echo "=========== 1) 编译（生产源码 am_lpkg.c + am_store_state.c） ==========="
# macOS 的 clang 在 -std=c11 下不认 POSIX 的 system()/chmod，需要 _DARWIN_C_SOURCE
CFLAGS="-isysroot $SDK -O2 -std=c11 -Wall -Wextra -Werror -D_DARWIN_C_SOURCE=1 \
        -DLPKG_ROOT=\"$APPROOT\" \
        -Itests/installer/stub -Icomponents/apps/pages -Icomponents/services/lpk/include"
# shellcheck disable=SC2086
"$CC" $CFLAGS tests/installer/test_install.c \
      components/apps/pages/am_lpkg.c components/apps/pages/am_store_state.c \
      components/services/lpk/lpk1.c components/services/lpk/lpk_manifest.c \
      -o "$WORK/test_install"

run() {   # $1=标签 $2...=用例参数
    label=$1; shift
    rm -rf "$APPROOT"; mkdir -p "$APPROOT"
    echo "--- $label"
    "$WORK/test_install" "$@" || { echo "  ✗ 用例失败: $label"; exit 1; }
}

echo
echo "=========== 2) 主干/扩展名边界 ==========="
"$WORK/test_install" stemof | tail -3

echo
echo "=========== 3) 首次安装（双版本记录） ==========="
run "首次安装" install_fresh "$WORK/v1.lpk"

echo
echo "=========== 4) 手动安装（无商店版本） ==========="
run "手动安装" install_nometa "$WORK/v1.lpk"

echo
echo "=========== 5) 更新（数据保留 / 记录唯一 / 无残留） ==========="
run "更新" update "$WORK/v1.lpk" "$WORK/v2.lpk"

echo
echo "=========== 6) 冷启动恢复（新进程读同一份磁盘状态） ==========="
rm -rf "$APPROOT"; mkdir -p "$APPROOT"
"$WORK/test_install" update "$WORK/v1.lpk" "$WORK/v2.lpk" >/dev/null
echo "--- 冷启动恢复（不清目录，直接再起一个进程）"
"$WORK/test_install" recover 2 2 || { echo "  ✗ 冷启动恢复失败"; exit 1; }

echo
echo "=========== 7) 记录写失败 → 回滚 ==========="
run "记录写失败回滚" update_fail_reg "$WORK/v1.lpk" "$WORK/v2.lpk"

echo
echo "=========== 8) 坏包 → 拒绝且旧版完好 ==========="
run "坏包回滚" update_fail_pkg "$WORK/v1.lpk" "$WORK/bad.lpk"

echo
echo "=========== 9) 掉电现场 → 自愈 ==========="
run "掉电自愈" powerloss "$WORK/v1.lpk" "$WORK/v2.lpk"

echo
echo "=========== 10) 旧记录兼容 / 无记录 / 记录损坏 ==========="
run "老格式记录" legacy
run "只有目录无记录" legacy_noreg
run "记录损坏" corrupt_reg

echo
echo "=========== 11) 多点 app_id ==========="
run "多点 app_id" dots "$WORK/dots.lpk"

echo
echo "=========== 11b) 卸载清桌面图标归属（mainmenu + mmhide 归零） ==========="
run "卸载清桌面归属" uninst_desktop "$WORK/v1.lpk"

echo
echo "=========== 12) ASan / UBSan 再跑一遍（安装器无内存问题） ==========="
SANFLAGS="-isysroot $SDK -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -DLPKG_ROOT=\"$APPROOT\" \
          -Itests/installer/stub -Icomponents/apps/pages -Icomponents/services/lpk/include"
# shellcheck disable=SC2086
"$CC" $SANFLAGS tests/installer/test_install.c \
      components/apps/pages/am_lpkg.c components/apps/pages/am_store_state.c \
      components/services/lpk/lpk1.c components/services/lpk/lpk_manifest.c \
      -o "$WORK/test_install_san"

san() {
    label=$1; shift
    rm -rf "$APPROOT"; mkdir -p "$APPROOT"
    "$WORK/test_install_san" "$@" > "$WORK/san_$label.log" 2>&1 || {
        echo "  ✗ ASan 用例失败: $label"; tail -20 "$WORK/san_$label.log"; exit 1; }
    echo "  ASan OK: $label"
}
san fresh       install_fresh "$WORK/v1.lpk"
san update      update "$WORK/v1.lpk" "$WORK/v2.lpk"
san fail_reg    update_fail_reg "$WORK/v1.lpk" "$WORK/v2.lpk"
san powerloss   powerloss "$WORK/v1.lpk" "$WORK/v2.lpk"
san legacy      legacy

echo
echo "全部通过"
