#!/bin/sh
# 资源商店「多文件直接下载」测试一键复跑
#
# 覆盖:
#   1) 清单生成器      tools/mk_asset_manifest.py（从真实 41.7 MB 仙剑资源包）
#   2) 清单解析+守卫   直接编译**生产源码** am_manifest.c（零 ESP 依赖）
#   3) 部署布局        按设备拼 URL 的规则，用本地静态服务器真取一遍
#
# 用法:  sh tests/asset/run_all.sh [pal_stored.zip 路径]
#        不给参数则用 /tmp/pal_fix/pal_stored.zip，不存在就跳过 2/3。
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ZIP=${1:-/tmp/pal_fix/pal_stored.zip}
CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
cd "$ROOT"

echo "=========== 1) 清单解析 / 路径守卫（生产源码） ==========="
"$CC" -isysroot "$SDK" -O2 -std=c11 -Wall -Wextra \
      -I components/apps/pages \
      tests/asset/am_manifest_test.c components/apps/pages/am_manifest.c \
      -o /tmp/am_mft
/tmp/am_mft tests/asset/data/pal_manifest.json

if [ -f "$ZIP" ]; then
    echo
    echo "=========== 2) 从真实资源包重新生成清单 ==========="
    rm -rf /tmp/asset_regen
    python3 tools/mk_asset_manifest.py --name pal --zip "$ZIP" \
            --out /tmp/asset_regen --title 仙剑奇侠传 --ver 3 | tail -6

    echo
    echo "=========== 3) 部署布局校验（按设备拼 URL 的规则真取） ==========="
    rm -rf /tmp/serve_regen
    mkdir -p /tmp/serve_regen/bbk/pkgs
    cp -R /tmp/asset_regen /tmp/serve_regen/bbk/pkgs/pal
    python3 tests/asset/check_manifest_deploy.py /tmp/serve_regen \
            bbk/pkgs/pal/manifest.json --sample 4 2>/dev/null | grep -vE "^127\.0\.0\.1"
else
    echo
    echo "（跳过 2/3：没有找到 $ZIP）"
fi
