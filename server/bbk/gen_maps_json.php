<?php
/**
 * 重新生成 /bbk/maps.json —— 把 files[].path 换成**带签名的路径**
 *
 *   maps/fj.bin  →  dl.php/<过期>/<签名>/fj.bin
 *
 * 为什么是路径式而不是 ?f= 查询串：设备端 map.lua 用 path 的最后一段当落盘文件名，
 * 查询串会污染文件名（详见 server/bbk/dl.php 顶部注释）。
 *
 * 结构保持 ver/packs/files/f16/mirrors 全部不变，只改 path 字段，
 * 所以老设备的解析逻辑（app.json.pick "path,size,f16,t"）不受影响。
 *
 * 用法：
 *   php server/bbk/gen_maps_json.php            # 预览（不写）
 *   php server/bbk/gen_maps_json.php --apply     # 写入（自动备份旧文件）
 *   php server/bbk/gen_maps_json.php --apply --days=90
 */
declare(strict_types=1);

$APPLY = in_array('--apply', $argv, true);
$FORCE = in_array('--force', $argv, true);
$DAYS  = 90;
foreach ($argv as $a) {
    if (preg_match('/^--days=(\d+)$/', $a, $m)) { $DAYS = max(1, min(3650, (int) $m[1])); }
}

const MAPS_JSON   = '/www/wwwroot/bbk.linit.cn/bbk/maps.json';
const SECRET_FILE = '/www/wwwroot/.bbk_secret';

if (!is_file(SECRET_FILE)) { exit("✗ 缺少密钥文件 " . SECRET_FILE . "（先生成：openssl rand -hex 32 > " . SECRET_FILE . "）\n"); }
$secret = trim((string) file_get_contents(SECRET_FILE));
if (strlen($secret) < 16) { exit("✗ 密钥太短\n"); }

$raw = file_get_contents(MAPS_JSON);
$d = json_decode($raw, true);
if (!is_array($d) || empty($d['files'])) { exit("✗ maps.json 读不出来或没有 files[]\n"); }

$exp = time() + $DAYS * 86400;
$changed = 0;
$preview = array();

foreach ($d['files'] as $i => $f) {
    if (empty($f['path'])) { continue; }
    $p = (string) $f['path'];
    /* 文件名永远是最后一段：静态形式 maps/fj.bin 与签名形式 dl.php/<e>/<s>/fj.bin 都适用 */
    $name = basename($p);
    if ($name === '') { continue; }
    /* 已是签名形式且剩余有效期 > 30 天 → 不动（保证可重复执行，不产生无谓改动）*/
    if (!$FORCE && strpos($p, 'dl.php/') === 0) {
        $segs = explode('/', $p);
        if (isset($segs[1]) && ctype_digit($segs[1]) && (int) $segs[1] > time() + 30 * 86400) { continue; }
    }
    $new = 'dl.php/' . $exp . '/' . substr(hash_hmac('sha256', (string) $exp . '|' . $name, $secret), 0, 32)
         . '/' . rawurlencode($name);
    if (!$FORCE && $p === $new) { continue; }
    $d['files'][$i]['path'] = $new;
    $changed++;
    if (count($preview) < 5) { $preview[] = $p . '  →  ' . $new; }
}

$d['updated'] = date('Y-m-d');
$d['dl_exp']  = $exp;

echo "过期时间: " . date('Y-m-d H:i:s', $exp) . "（$DAYS 天）\n";
echo "改写条目: $changed / " . count($d['files']) . "\n";
foreach ($preview as $line) { echo "  " . $line . "\n"; }

if (!$changed && empty($APPLY)) { echo "（无需改写）\n"; }

if (!$APPLY) {
    echo "\n（预览模式，未写入。加 --apply 生效）\n";
    exit(0);
}

$out = json_encode($d, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT) . "\n";
$tmp = MAPS_JSON . '.tmp';
if (file_put_contents($tmp, $out) === false) { exit("✗ 写入失败 $tmp\n"); }

$bak = MAPS_JSON . '.bak.' . date('Ymd_His');
copy(MAPS_JSON, $bak);
if (!rename($tmp, MAPS_JSON)) { exit("✗ rename 失败\n"); }
@chown(MAPS_JSON, 'www');
@chgrp(MAPS_JSON, 'www');
@chmod(MAPS_JSON, 0644);

echo "✓ 已写入 " . MAPS_JSON . "（备份 " . basename($bak) . "）\n";
