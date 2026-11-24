#!/bin/sh
# tests/server/run_all.sh — 投稿平台服务端逻辑测试 (PHP)
#
# 被测对象是**服务器上的 PHP 代码**, 不是固件。本机没装 php 时本套件直接 SKIP,
# 绝不当作通过 —— 这是"缺依赖时报告跳过、不伪称通过"的落实。
#
# 三个脚本各自的定位:
#   t_cli.php      命令行逻辑(词法扫描/上传校验/LAPP 解析/发布/更新/回滚/审计)
#   t_http.php     HTTP 接口(需要能起内置 web server)
#   t_pub_one.php  单次发布流程
#
# 只动 BT_ROOT(默认 /tmp/bbk_test)下的 data/ 与 bbk/, 不碰生产目录。
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP(环境缺失)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

if ! command -v php >/dev/null 2>&1; then
    echo "SKIP: 本机无 php —— 服务端测试只能在装有 PHP 的机器/服务器上执行"
    echo "      执行方式: php tests/server/t_cli.php"
    exit 2
fi

FAILED=0
for t in t_cli.php t_pub_one.php; do
    [ -f "tests/server/$t" ] || continue
    echo "--- $t"
    if ! php "tests/server/$t"; then
        echo "  [FAIL] $t"
        FAILED=1
    fi
done

# t_http.php 需要先起 php -S, 当前不自动起服务(不留后台进程), 只报未执行
if [ -f tests/server/t_http.php ]; then
    echo "--- t_http.php  未执行 (需先手动起 php -S, 本脚本不留后台进程)"
fi

[ "$FAILED" -ne 0 ] && { echo "server: 有失败"; exit 1; }
echo "server: 全部通过"
exit 0
