#!/bin/sh
# tests/sd_vfs/run_all.sh — SD / VFS 契约检查 (主机侧)
#
# sd_scan.c 的挂载路径与 esp_vfs_fat_* / sdmmc_* 强耦合, 主机无法真挂 SD 卡。
# 这里做**源码契约检查**: 目录表共用、mkdir_p 幂等、卸载两级兜底、状态复位、锁配对。
# 真实的插拔/挂载失败/格式化行为列入真机清单。
#
# 退出码: 0=PASS, 1=FAIL
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

echo "=========== SD / VFS 契约检查 ==========="
python3 tests/sd_vfs/contract_check.py "$ROOT"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "FAIL: 契约检查未通过 (rc=$rc)"
    exit 1
fi

echo
echo "sd_vfs: 全部通过"
echo "⚠ 未覆盖 (需真机): 真实插拔 / 挂载失败 / 格式化 / 卡写满 / 交卡给电脑后再取回"
exit 0
