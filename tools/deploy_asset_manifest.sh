#!/bin/sh
# deploy_asset_manifest.sh — 把「清单 + 文件树」部署到 bbk.linit.cn，并给 index.json 加 manifest 字段
#
# 默认 **干跑（dry-run）**：只打印将要执行的命令，不碰服务器。
# 确认后加 --go 才真正上传与改索引。
#
# 用法:
#   sh tools/deploy_asset_manifest.sh --dir /tmp/asset_pal --name pal            # 看要做什么
#   sh tools/deploy_asset_manifest.sh --dir /tmp/asset_pal --name pal --go       # 真部署
#
# 安全措施:
#   · 上传到新目录 bbk/pkgs/<name>/，**不覆盖**任何现有文件；
#   · index.json 改动前先备份成 index.json.bak-<时间戳>，改完自检条目数不变；
#   · 只加一个 manifest 字段 + ver+1，size/crc32/path 一概不动（老固件仍然下 zip）；
#   · 最后清 nginx FastCGI 缓存并 curl 验证（清单 + 一个真实文件）。
set -e

DIR=""; NAME=""; GO=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dir)  DIR=$2;  shift 2 ;;
        --name) NAME=$2; shift 2 ;;
        --go)   GO=1;    shift ;;
        *) echo "未知参数: $1"; exit 2 ;;
    esac
done
[ -n "$DIR" ] && [ -n "$NAME" ] || { echo "用法: $0 --dir <本地目录> --name <包名> [--go]"; exit 2; }
[ -f "$DIR/manifest.json" ] || { echo "⛔ $DIR/manifest.json 不存在（先跑 tools/mk_asset_manifest.py）"; exit 2; }

KEY=~/.ssh/bbk_linit_key
HOST=root@120.24.110.41
PORT=2546
SITE=/www/wwwroot/bbk.linit.cn
REL="pkgs/$NAME"

echo "== 本地清单自检 =="
python3 - "$DIR/manifest.json" "$NAME" <<'PY'
import json, sys
p, name = sys.argv[1], sys.argv[2]
d = json.load(open(p, encoding="utf-8"))
assert d["fmt"] == 1
assert d["count"] == len(d["files"]), "count 与文件表不一致"
assert d["total"] == sum(x["size"] for x in d["files"]), "total 不自洽"
assert d.get("pkg", name) == name, "包名不一致: %s vs %s" % (d.get("pkg"), name)
print("  ✓ %d 个文件 / %d B / 清单 %d B" % (d["count"], d["total"], __import__("os").path.getsize(p)))
PY

echo
echo "== 将要执行 =="
echo "  1) scp -r $DIR  ->  $SITE/bbk/$REL/"
echo "  2) chown -R www:www $SITE/bbk/$REL"
echo "  3) index.json: 给 name=$NAME 的条目加 \"manifest\":\"$REL/manifest.json\"，ver+1（先备份+自检）"
echo "  4) 清 nginx FastCGI 缓存"
echo "  5) curl 验证 清单 + 首个文件"

if [ "$GO" -eq 0 ]; then
    echo
    echo "（干跑结束。确认无误后加 --go 执行）"
    exit 0
fi

echo
echo "== 1) 上传 =="
ssh -o BatchMode=yes -i "$KEY" -p $PORT $HOST "mkdir -p $SITE/bbk/$REL"
scp -o BatchMode=yes -i "$KEY" -P $PORT -r "$DIR"/. "$HOST:$SITE/bbk/$REL/"
ssh -o BatchMode=yes -i "$KEY" -p $PORT $HOST "chown -R www:www $SITE/bbk/$REL && chmod -R a+rX $SITE/bbk/$REL && find $SITE/bbk/$REL -type f | wc -l"

echo
echo "== 2) 改索引 =="
ssh -o BatchMode=yes -i "$KEY" -p $PORT $HOST "NAME='$NAME' REL='$REL' python3 - <<'PY'
import json, os, shutil, time
root = '/www/wwwroot/bbk.linit.cn/bbk'
idx = os.path.join(root, 'index.json')
name = os.environ['NAME']; rel = os.environ['REL']
bak = idx + '.bak-' + time.strftime('%Y%m%d-%H%M%S')
shutil.copy2(idx, bak)
d = json.load(open(idx, encoding='utf-8'))
before = len(d['pkgs'])
hit = 0
for p in d['pkgs']:
    if p.get('name') == name:
        p['manifest'] = rel + '/manifest.json'
        p['ver'] = int(p.get('ver', 1)) + 1
        hit += 1
if hit != 1:
    print('⛔ 命中 %d 条（应为 1），未改动' % hit); raise SystemExit(1)
d['ver'] = int(d.get('ver', 1)) + 1
d['updated'] = time.strftime('%Y-%m-%d %H:%M:%S')
tmp = idx + '.tmp'
with open(tmp, 'w', encoding='utf-8') as f:
    json.dump(d, f, ensure_ascii=False, indent=2); f.write('\n')
chk = json.load(open(tmp, encoding='utf-8'))
assert len(chk['pkgs']) == before, '条目数变了'
os.replace(tmp, idx)
os.system('chown www:www ' + idx)
print('  ✓ %s 已加 manifest=%s/manifest.json | index ver -> %d | 备份 %s'
      % (name, rel, d['ver'], bak))
PY"

echo
echo "== 3) 清缓存 + 验证 =="
ssh -o BatchMode=yes -i "$KEY" -p $PORT $HOST "find /www/server/fastcgi_cache -type f -delete 2>/dev/null; true"
FIRST=$(python3 -c "import json;print(json.load(open('$DIR/manifest.json',encoding='utf-8'))['files'][0]['path'])" | sed 's/ /%20/g')
curl -s -o /dev/null -m 20 -w "  清单 HTTP=%{http_code} %{size_download}B\n" "https://bbk.linit.cn/bbk/$REL/manifest.json"
curl -s -o /dev/null -m 30 -w "  文件 HTTP=%{http_code} %{size_download}B  ($FIRST)\n" "https://bbk.linit.cn/bbk/$REL/$FIRST"
echo
echo "✅ 部署完成。设备端下一次进入商店（索引 ver 已变）即会走多文件下载。"
echo "   回滚: 恢复 index.json.bak-* 并 rm -rf $SITE/bbk/$REL"
