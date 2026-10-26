#!/bin/bash
# store_deploy.sh — 把 store/ 发布到公网（应用商店 + 矢量地图包）
#
# ★ 2026-10-05 实测修正（第一次真跑踩到的，全部是"脚本默认值与服务器实际不符"）：
#   ① 服务器**没装 rsync** → 原脚本的 `rsync -avz` 直接 `bash: rsync: command not found`。
#      改用 **scp**（纯新增/覆盖，语义上也不再需要 `--delete`）。
#   ② SSH 端口不是 22（22 被安全组拒绝，实测 **2546**）。
#   ③ 站点真实目录是 **/www/wwwroot/bbk.linit.cn/bbk**（不是 /www/bbk.linit.cn/bbk）。
#   ④ 上传后要 `chown -R www:www`，否则 nginx 读不到（scp 以 root 落盘是 root:root）。
#
# 用法：
#   bash tools/store_deploy.sh                 # 用下面的默认值直接发
#   bash tools/store_deploy.sh --dry-run       # 只列出会传什么，不碰服务器
#   bash tools/store_deploy.sh --port 2546 --key ~/Desktop/xxx_id_ed25519
set -e
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${PROJECT_DIR}/store"

# 已验证的默认值 (2026-10-05)
HOST_DEF="root@47.122.46.5"
PORT_DEF="2546"
DIR_DEF="/www/wwwroot/bbk.linit.cn/bbk"
KEY_DEF="$HOME/Desktop/47.122.46.5_id_ed25519"
SITE_DEF="https://bbk.linit.cn/bbk"

HOST="$HOST_DEF"; PORT="$PORT_DEF"; DIR="$DIR_DEF"; KEY="$KEY_DEF"
DRYRUN=""; GIT=""; SITE="$SITE_DEF"
while [ $# -gt 0 ]; do
  case "$1" in
    --host) HOST="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --dir)  DIR="$2";  shift 2 ;;
    --key)  KEY="$2";  shift 2 ;;
    --site) SITE="$2"; shift 2 ;;
    --git)  GIT=1;     shift ;;
    --dry-run) DRYRUN=1; shift ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

[ -d "$SRC" ] || { echo "✗ 没有 $SRC —— 先跑 tools/store_build.py 和 tools/map_pack.py"; exit 1; }
[ -f "$KEY" ] || { echo "✗ 找不到私钥 $KEY（用 --key 指定）"; exit 1; }
# ⛔ ssh 的端口是 `-p`，**scp 是 `-P`**（大写）。混用会把端口号当成文件名:
#    `scp: stat local "2546": No such file or directory` —— 踩过一次。
SSHOPT=(-p "$PORT" -i "$KEY" -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15)
SCPOPT=(-P "$PORT" -i "$KEY" -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15)

echo "本地: $SRC  ($(find "$SRC" -type f | wc -l | tr -d ' ') 个文件, $(du -sh "$SRC" | cut -f1))"
echo "目标: $HOST:$DIR/   (ssh 端口 $PORT)"
echo
echo "要传的文件:"
(cd "$SRC" && find . -type f | sed 's|^\./|   |' | sort)
echo

if [ -n "$DRYRUN" ]; then
  echo "[--dry-run] 只连服务器看一眼现状, 不写任何东西"
  ssh "${SSHOPT[@]}" "$HOST" "echo '   服务器现有:'; ls -la '$DIR' 2>/dev/null | tail -n +2 | sed 's/^/     /'" || true
  echo "[--dry-run] 结束"
  exit 0
fi

if [ -n "$GIT" ]; then
  echo "== ① 推到 GitHub =="
  echo "   ⚠ 这是**公开仓库**，会公开全部分发包。非必要不要加 --git。"
  cd "$PROJECT_DIR"
  /opt/homebrew/bin/git add -A store
  /opt/homebrew/bin/git commit -q -m "store: 发布分发包与索引 ($(date +%F))" || echo "   (无变化)"
  /opt/homebrew/bin/git push origin main
  echo "   ✓ 已推送"
  echo
fi

echo "== ② 上传 (scp; 服务器没装 rsync) =="
ssh "${SSHOPT[@]}" "$HOST" "mkdir -p '$DIR'"
scp "${SCPOPT[@]}" -r "$SRC"/* "$HOST:$DIR/"
ssh "${SSHOPT[@]}" "$HOST" "chown -R www:www '$DIR'"
echo "   ✓ 完成 (已 chown www:www)"

echo
echo "== ③ 公网自检（要 200 + JSON，**不是** HTML）=="
# ⛔ 别用 `read x y < <(curl ...)`: `set -e` 下 read 到 EOF 返回 1 → 整个脚本静默退出,
#    自检一行都不打（踩过一次）。改成两条独立命令 + `|| true`。
fail=0
check() {   # $1=相对路径  $2=期望字节数(可空)
  local http size extra=""
  http=$(curl -sSL -m 25 -o /tmp/_dep.bin -w "%{http_code}" "$SITE/$1" 2>/dev/null || echo 000)
  size=$(wc -c < /tmp/_dep.bin 2>/dev/null | tr -d ' ')
  [ "$http" = "200" ] || { extra="  ✗ HTTP $http"; fail=1; }
  if [ -n "$2" ] && [ "$size" != "$2" ]; then extra="$extra  ✗ 期望 $2 字节"; fail=1; fi
  printf "   %-30s HTTP %s  %9s B%s\n" "$1" "$http" "$size" "$extra"
}
check "index.json"
check "maps.json"
# 地图包数量会变 (road/contour/poi/...) ⇒ **按本地实际文件逐个查**, 不写死
for f in "$SRC"/maps/*.ltm; do
  [ -e "$f" ] || continue
  b=$(basename "$f")
  check "maps/$b" "$(wc -c < "$f" | tr -d ' ')"
done

# 索引必须是 JSON（首字符 '{'）—— 站点对不存在的路径会回退成 WordPress 首页 HTML
if ! curl -sSL -m 20 "$SITE/maps.json" | head -c 1 | grep -q '{'; then
  echo "   ✗ maps.json 返回的不是 JSON（可能走了 WordPress 回退页）—— 查 nginx 的 location ^~ /bbk/"
  fail=1
fi
if ! curl -sSL -m 20 "$SITE/index.json" | head -c 1 | grep -q '{'; then
  echo "   ✗ index.json 返回的不是 JSON"
  fail=1
fi


echo
echo "== ④ 分发包 crc 与 index.json 对账 (**以服务器实际下发的字节为准**) =="
# ⛔ 为什么必须"以服务器为准": 2026-10-15 真机下载 BIOS模拟器 失败, 根因就是
#    index.json 是旧的 (本地包重建过、索引没跟着重建) → 设备算出的 crc 与索引对不上。
#    这一道在**部署时**就把这类漂移挡掉 —— 设备端永远拿到"互相一致"的一对。
PY_BIN="/Users/linit/.workbuddy/binaries/python/envs/default/bin/python"
[ -x "$PY_BIN" ] || PY_BIN=python3
"$PY_BIN" - "$SITE" <<'PYEOF' || fail=1
import json, sys, urllib.request, binascii
site = sys.argv[1].rstrip("/")
idx = json.loads(urllib.request.urlopen(site + "/index.json", timeout=25).read())
pk = idx.get("pkgs", [])
bad = 0
for p in pk:
    try:
        b = urllib.request.urlopen(site + "/" + p["path"], timeout=40).read()
    except Exception as e:
        print("   %-14s ✗ 取不到: %s" % (p["name"], e)); bad += 1; continue
    c = format(binascii.crc32(b) & 0xffffffff, "08x")
    ok = (len(b) == p["size"] and c == p["crc32"])
    if not ok: bad += 1
    print("   %-14s %s %7dB crc=%s  (索引 %dB/%s)"
          % (p["name"], "OK " if ok else "✗  ", len(b), c, p["size"], p["crc32"]))
print("   → %d/%d 个包与索引一致" % (len(pk) - bad, len(pk)))
sys.exit(1 if bad else 0)
PYEOF

[ "$fail" = 0 ] && echo "   ✅ 全部通过" || { echo "   ❌ 有失败项"; exit 1; }
