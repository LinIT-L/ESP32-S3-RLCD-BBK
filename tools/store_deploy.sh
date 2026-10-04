#!/bin/bash
# store_deploy.sh — 把 store/ 发布到公网（主源 linit.cn + 可选 GitHub）
#
# 前置（一次性）：把本机公钥加到服务器，或改用别的上传方式
#   ssh-copy-id 会把 ~/.ssh/id_ed25519.pub 追加到服务器的 ~/.ssh/authorized_keys
#
# 用法：
#   bash tools/store_deploy.sh --host root@47.122.46.5 --dir /www/bbk.linit.cn/bbk
#   bash tools/store_deploy.sh --dry-run          # 只看会传什么
set -e
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${PROJECT_DIR}/store"

HOST=""; DIR=""; DRYRUN=""; GIT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --host) HOST="$2"; shift 2 ;;
    --dir)  DIR="$2";  shift 2 ;;
    --git)  GIT=1;     shift ;;
    --dry-run) DRYRUN="-n"; shift ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

[ -d "$SRC" ] || { echo "✗ 没有 $SRC —— 先跑 tools/store_build.py"; exit 1; }
echo "本地: $SRC  ($(find "$SRC" -type f | wc -l | tr -d ' ') 个文件, $(du -sh "$SRC" | cut -f1))"
echo

if [ -n "$GIT" ]; then
  echo "== ① 推到 GitHub（真相源）=="
  cd "$PROJECT_DIR"
  /opt/homebrew/bin/git add -A store
  /opt/homebrew/bin/git commit -q -m "store: 发布分发包与索引 ($(date +%F))" || echo "   (无变化)"
  /opt/homebrew/bin/git push origin main
  echo "   ✓ 已推送"
  echo
fi

if [ -z "$HOST" ]; then
  echo "== ② 服务器同步：未指定 --host，跳过 =="
  echo
  echo "  给你服务器上的目录要求（⚠ 你的站是 WordPress，必须单独开静态目录）："
  echo "    1) 建目录:      mkdir -p ${DIR:-/www/bbk.linit.cn/bbk}"
  echo "    2) nginx 里加:"
  echo "         location ^~ /bbk/ { alias ${DIR:-/www/bbk.linit.cn/bbk}/; }"
  echo "       ⚠ 前缀必须是 ^~ —— 否则会被 WordPress 的 try_files 接走，"
  echo "         设备拿到的是 HTML 页面而不是文件字节（这是最容易踩的坑）"
  echo "    3) reload:      nginx -s reload"
  echo
  echo "  然后任选一种上传方式："
  echo "    A) 我直接传（推荐）: 把下面这行公钥加到服务器 ~/.ssh/authorized_keys，"
  echo "       再把这个脚本配好 --host/--dir 跑一次即可"
  echo "       公钥: $(cat ~/.ssh/id_ed25519.pub 2>/dev/null || echo '(找不到 ~/.ssh/id_ed25519.pub)')"
  echo "    B) 你自己传:  scp -r store/* 用户@47.122.46.5:${DIR:-/www/bbk.linit.cn/bbk}/"
  echo "       或用宝塔面板/阿里云 Workbench 的文件管理直接拖上去"
  exit 0
fi

echo "== ② 同步到服务器 =="
echo "   $SRC/  →  $HOST:$DIR/"
rsync -avz $DRYRUN --delete \
  -e "ssh -o StrictHostKeyChecking=accept-new" \
  "$SRC/" "$HOST:$DIR/"
echo "   ✓ 完成"

echo
echo "== ③ 自检（务必看到 200 + JSON，而不是 HTML）=="
echo "   curl -s https://bbk.linit.cn/bbk/index.json | head -c 200"
