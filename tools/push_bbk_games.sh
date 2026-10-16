#!/usr/bin/env bash
# push_bbk_games.sh — 一键把「伏魔 / 三国」等电子词典游戏推进掌机 TF 卡
#
# 做的事（全自动）：
#   1) 让设备切到 U 盘模式（走 remcon 注入按键）
#   2) 等 TF 卡在 macOS 挂载出来
#   3) 读出设备卡上 *已有* 的分类目录，自动挑最合适的（优先「角色扮演」）
#   4) 拷 .gam → gam/<分类>/   .lav → lava/<分类>/
#   5) 清理 macOS 的 ._* 影子文件、sync、卸载
#   6) 提示按一下设备按键退出 U 盘模式
#
# 用法:  bash tools/push_bbk_games.sh
set -u
PORT=/dev/cu.usbmodem101
SRC_GAM="$HOME/Desktop/文曲星/喵比特-步步高+文曲星+gb+fc/roms/gam4980"
PY="$HOME/.workbuddy/binaries/python/envs/default/bin/python"

echo "▶ 1/5 等待设备串口（若设备在休眠，请按一下设备上的按键 / 电源键）..."
for i in $(seq 1 120); do [ -e "$PORT" ] && break; sleep 1; done
if [ ! -e "$PORT" ]; then echo "✗ 120 秒内没等到 $PORT —— 请唤醒设备后重跑本脚本"; exit 1; fi
echo "  ✓ 串口已就绪"

echo "▶ 2/5 让设备切入 U 盘模式 ..."
"$PY" - "$PORT" <<'PYEOF'
import serial, sys, time
P = sys.argv[1]
ser = serial.Serial(P, 115200, timeout=0.2)
try: ser.dtr = False; ser.rts = False
except Exception: pass
def drain(s):
    e = time.time() + s
    while time.time() < e:
        try: ser.read(4096)
        except Exception: break
def send(c, w=0.4):
    try: ser.write((c + "\n").encode()); ser.flush()
    except Exception: pass
    time.sleep(w)
drain(1.5)
send("key back", 0.4); drain(1.0)          # 若在应用里，先退回
send("page storage", 0.6); drain(2.0)
send("key down", 0.5);     drain(1.2)      # 移到「挂载到电脑」
send("key confirm", 0.5);  drain(2.0)
send("key confirm", 0.5);  drain(6.0)      # 二次确认
ser.close()
print("  ✓ 指令已发，USB 即将切换（串口会断开）")
PYEOF

echo "▶ 3/5 等待 TF 卡挂载 ..."
VOL=""
for i in $(seq 1 60); do
  V=$(ls /Volumes/ 2>/dev/null | grep -viE '^Macintosh|^Recovery$|^\.' | head -1)
  [ -n "$V" ] && { VOL="/Volumes/$V"; break; }
  sleep 1
done
[ -n "$VOL" ] || { echo "✗ 没等到 TF 卡挂在 /Volumes —— 请手动挂载后重跑"; exit 1; }
echo "  ✓ 已挂载: $VOL"

echo "▶ 4/5 拷贝游戏文件 ..."
pick_cat() {   # $1=根目录  输出一个可用的分类目录名
  local root="$1"
  for want in 角色扮演 益智休闲 策略战棋 动作射击 动作游戏 体育竞速 格斗对战 动作冒险 其他; do
    [ -d "$root/$want" ] && { echo "$want"; return; }
  done
  local first
  first=$(ls -d "$root"/*/ 2>/dev/null | head -1)
  [ -n "$first" ] && { basename "$first"; return; }
  echo "角色扮演"   # 都没有就新建
}

GAM_ROOT="$VOL/gam"; LAVA_ROOT="$VOL/lava"
mkdir -p "$GAM_ROOT" "$LAVA_ROOT"
GAM_CAT=$(pick_cat "$GAM_ROOT"); LAVA_CAT=$(pick_cat "$LAVA_ROOT")
mkdir -p "$GAM_ROOT/$GAM_CAT" "$LAVA_ROOT/$LAVA_CAT"
echo "  分类: gam/ → 「$GAM_CAT」   lava/ → 「$LAVA_CAT」"

# .gam 游戏
for f in "$SRC_GAM"/*.gam; do
  [ -e "$f" ] || continue
  cp -f "$f" "$GAM_ROOT/$GAM_CAT/" && echo "    + gam/$GAM_CAT/$(basename "$f")"
done

# .lav 游戏（伏魔记 / 三国志 / Lee三国）—— 同名只取一份（按路径长度短的优先，通常是主目录那份）
find "$HOME/Desktop/文曲星" -name '*.lav' \
     \( -iname '*伏魔*' -o -iname '*三国*' \) 2>/dev/null \
  | awk -F/ '{ print length($0)"\t"$0 }' | sort -n | cut -f2- \
  | awk -F/ '!seen[$NF]++ { print }' | while read -r f; do
  cp -f "$f" "$LAVA_ROOT/$LAVA_CAT/" && echo "    + lava/$LAVA_CAT/$(basename "$f")"
done

echo "▶ 5/5 清理 + 卸载 ..."
find "$GAM_ROOT" "$LAVA_ROOT" -name '._*' -delete 2>/dev/null
sync
diskutil unmount "$VOL" >/dev/null 2>&1 && echo "  ✓ 已卸载（FAT 一致）" || echo "  ⚠ 卸载返回非零（多半已卸载）"
echo
echo "✅ 完成！请在设备上按一下按键退出 U 盘模式，然后："
echo "   • 「伏魔 / 三国」会出现在游戏列表里（分类见上面打印的路径）"
echo "   • 若没看到，退出应用管家重新进一次（列表会重扫）"
