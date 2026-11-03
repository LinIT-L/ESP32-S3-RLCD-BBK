<?php
/**
 * t_pub_one.php — 并发测试用: 一个进程发布一个应用
 *   php t_pub_one.php <应用名> <版本>
 * 只用于 /tmp/bbk_test 测试环境。
 */
declare(strict_types=1);
$ROOT = getenv('BT_ROOT') ?: '/tmp/bbk_test';
if ($ROOT !== '/tmp/bbk_test' && getenv('BT_ALLOW_ANY') !== '1') {
    fwrite(STDERR, "拒绝执行: BT_ROOT={$ROOT}\n");
    exit(2);
}
require $ROOT . '/web/lib.php';

$name = (string)($argv[1] ?? 'x');
$ver  = (int)($argv[2] ?? 1);
$icon = base64_encode(str_repeat("\xA5", 512));
$src  = "--@lapp v1\n--@app name={$name} cat=软件 desc=并发测试 version={$ver}\n"
      . "--@app icon={$icon}\n"
      . "function render() app.clear(app.WHITE) end\n";

$sha = hash('sha256', $src);
$f = rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sha . '.lua';
@mkdir(dirname($f), 0700, true);
file_put_contents($f, $src);

$sub = [
    'name' => $name, 'label' => $name, 'cat' => '软件', 'descr' => '并发', 'ver' => $ver,
    'fname' => safe_fname($name), 'size' => strlen($src),
    'crc' => sprintf('%08x', crc32($src)), 'sha' => $sha,
];
[$ok, $why] = store_publish($sub, $f);
echo ($ok ? "[OK] " : "[FAIL] ") . $name . ' — ' . $why . "\n";
exit($ok ? 0 : 1);
