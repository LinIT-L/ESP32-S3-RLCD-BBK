#!/bin/bash
# dl_chunks.sh — 多连接分块下载大文件 (Range 并行)
#   为什么需要: download.openstreetmap.fr 单连接被限速 ~5~18 KB/s,
#   46 MB 单线程要 45+ 分钟。分 16 路 Range 并行 → 几分钟。
#   ⚠ 必须**先探测真实 Content-Length**, 不能凭印象估 (踩过: 少算了 20 MB,
#     拼出来的 PBF 解压报 "data error" —— osmium 只在 blob 处才发现, 不早报)。
#
#   用法: dl_chunks.sh <url> <outfile> [并发数=16]
set -euo pipefail

URL="$1"
OUT="$2"
N="${3:-16}"
DIR="$(mktemp -d "${TMPDIR:-/tmp}/dlc.XXXXXX")"
trap 'rm -rf "$DIR"' EXIT

# ---- ① 探测真实总长 ----
LEN=$(curl -sSI --max-time 40 -H 'Accept-Encoding: identity' "$URL" \
      | tr -d '\r' | awk 'tolower($1)=="content-length:"{v=$2} END{print v}')
case "$LEN" in
  ''|*[!0-9]*) echo "✗ 拿不到 Content-Length" >&2; exit 1 ;;
esac
echo "总长 $LEN B ($(echo "scale=1; $LEN/1048576" | bc) MB)  并发 $N"

# ---- ② 算分块边界 (Python 保证不手抖) ----
<HOME>/.workbuddy/binaries/python/envs/default/bin/python - "$LEN" "$N" > "$DIR/ranges" <<'PY'
import sys
n = int(sys.argv[1]); k = int(sys.argv[2])
step = (n + k - 1) // k
for i in range(k):
    a = i * step
    if a >= n:
        break
    b = min(a + step, n) - 1
    print(i, a, b)
PY

# ---- ③ 并行拉 ----
t0=$(date +%s)
while read -r i a b; do
  ( curl -sS -L --retry 6 --retry-delay 2 --max-time 1200 \
        -r "$a-$b" -o "$DIR/p$i" "$URL" \
      && echo "  ✓ p$i  $(( b - a + 1 )) B" ) &
done < "$DIR/ranges"
wait

# ---- ④ 校验每块尺寸 + 拼接 ----
total=0
while read -r i a b; do
  want=$(( b - a + 1 ))
  got=$(stat -f%z "$DIR/p$i" 2>/dev/null || echo 0)
  if [ "$got" != "$want" ]; then
    echo "✗ p$i 尺寸不符: 期望 $want 实得 $got" >&2; exit 1
  fi
  total=$(( total + got ))
done < "$DIR/ranges"

if [ "$total" != "$LEN" ]; then
  echo "✗ 合计 $total ≠ $LEN" >&2; exit 1
fi

cat $(while read -r i _ _; do echo "$DIR/p$i"; done < "$DIR/ranges") > "$OUT"
sz=$(stat -f%z "$OUT")
echo "✓ $OUT  $sz B  用时 $(( $(date +%s) - t0 ))s"
[ "$sz" = "$LEN" ] || { echo "✗ 落地尺寸不符" >&2; exit 1; }
echo "接着务必用 osmium 真解析一遍 (拼接错误只有解析才发现)"
