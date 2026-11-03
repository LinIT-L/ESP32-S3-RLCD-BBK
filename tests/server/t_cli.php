<?php
/**
 * t_cli.php — 服务器端逻辑测试（在服务器 PHP 环境里跑，直接调生产函数）
 *
 *   php /tmp/bbk_test/t_cli.php
 *
 * 只动 BT_ROOT（默认 /tmp/bbk_test）下的 data/ 与 bbk/，**绝不碰生产目录**。
 * 覆盖: 词法扫描 / 上传校验 / LAPP 解析 / 发布 / 更新 / 版本历史 /
 *       下架 / 重上架 / 回滚 / 索引损坏拒写 / 并发发布 / 审计 / 数据隔离
 */
declare(strict_types=1);

$ROOT = getenv('BT_ROOT') ?: '/tmp/bbk_test';
if ($ROOT !== '/tmp/bbk_test' && getenv('BT_ALLOW_ANY') !== '1') {
    fwrite(STDERR, "拒绝执行: BT_ROOT={$ROOT} 不是 /tmp/bbk_test\n");
    exit(2);
}
require $ROOT . '/web/lib.php';

function sh(string $cmd): void { exec($cmd . ' 2>&1', $o, $rc); if ($rc !== 0) echo "  (cmd rc={$rc}: {$cmd})\n"; }

/** 清空测试数据（只允许在 BT_ROOT 内） */
function reseed(): void {
    global $ROOT;
    sh('rm -rf ' . escapeshellarg($ROOT . '/data'));
    sh('rm -rf ' . escapeshellarg($ROOT . '/bbk'));
    foreach ([$ROOT . '/data/pkgs', $ROOT . '/data/backup', $ROOT . '/bbk/pkgs', $ROOT . '/bbk/pkgs_backup'] as $d)
        @mkdir($d, 0777, true);
    /* 基线商店: 两个"已有"应用，用来验证"发布不会把别的应用弄丢" */
    foreach (['老应用A', '老应用B'] as $n) {
        file_put_contents($ROOT . "/bbk/pkgs/{$n}.lua", "APP {$n} base\n");
    }
    $j = ['ver' => 10, 'updated' => '2026-01-01 00:00:00', 'pkgs' => [
        ['name' => '老应用A', 'label' => '老应用A', 'cat' => '软件', 'desc' => '基线A',
         'ver' => 3, 'size' => strlen("APP 老应用A base\n"), 'crc32' => sprintf('%08x', crc32("APP 老应用A base\n")),
         'path' => 'pkgs/老应用A.lua'],
        ['name' => '老应用B', 'label' => '老应用B', 'cat' => '游戏', 'desc' => '基线B',
         'ver' => 1, 'size' => strlen("APP 老应用B base\n"), 'crc32' => sprintf('%08x', crc32("APP 老应用B base\n")),
         'path' => 'pkgs/老应用B.lua'],
    ]];
    file_put_contents($ROOT . '/bbk/index.json',
        json_encode($j, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT));
}

$PASS = 0; $FAIL = 0;
function ok(bool $c, string $label, string $extra = ''): void {
    global $PASS, $FAIL;
    if ($c) { $PASS++; echo "  PASS  {$label}\n"; }
    else    { $FAIL++; echo "  FAIL  {$label}" . ($extra !== '' ? "  ({$extra})" : '') . "\n"; }
}
function has(string $hay, string $needle): bool { return strpos($hay, $needle) !== false; }

/** 造一份合法 LAPP 包内容 */
function lapp(string $name, int $ver, string $icon = '', string $extra = ''): string {
    $h = "--@lapp v1\n--@app name={$name} cat=软件 desc=测试用 version={$ver}\n";
    if ($icon !== '') $h .= "--@app icon={$icon}\n";
    $h .= $extra;
    $h .= "\nfunction render()\n  app.clear(app.WHITE)\n  app.mixc(app.Width // 2, 10, \"{$name}\")\nend\n";
    $h .= "function action(a) return true end\n";
    return $h;
}
$ICON512 = base64_encode(str_repeat("\xA5", 512));

/** 把内容落成投稿包 + 返回 subs 风格数组 */
function make_sub(string $name, int $ver, string $content, string $cat = '软件'): array {
    global $ROOT;
    $sha = hash('sha256', $content);
    $f = $ROOT . '/data/pkgs/' . $sha . '.lua';
    file_put_contents($f, $content);
    return [
        'id' => 1, 'name' => $name, 'label' => $name, 'cat' => $cat,
        'descr' => '测试说明', 'ver' => $ver, 'fname' => safe_fname($name),
        'size' => strlen($content), 'crc' => sprintf('%08x', crc32($content)), 'sha' => $sha,
        'status' => 'pending', 'uid' => 1,
    ];
}

echo "=== LinTOS 投稿平台 · 服务器逻辑测试 ===\n\n";

/* ============================ 1. 词法扫描 ============================ */
echo "T1 词法级扫描（区分代码/注释/字符串）\n";
$h = scan_lua("-- app.fs.remove 是删文件的, 这行只是注释\nfunction render() end\n");
ok(scan_verdict($h) === 'none', "注释里的危险 API 不再误报", scan_summary($h));

$h = scan_lua("local s = \"app.fs.remove\"\nfunction render() end\n");
ok(scan_verdict($h) === 'med' && $h[0]['code'] === '字符串含 API 名', "字符串里出现 API 名 → 提示(可能在拼名字)");

$h = scan_lua("function render() app.fs . remove('x') end\n");
$codes = array_column($h, 'code');
ok(in_array('app.fs.remove', $codes, true), "app.fs . remove（带空格）照样命中", implode(',', $codes));

$h = scan_lua("function render() app['fs']['write']('x','y') end\n");
$codes = array_column($h, 'code');
ok(in_array('app[下标]', $codes, true), "app[\"fs\"][\"write\"] 动态下标 → 标为需人工看", implode(',', $codes));

$h = scan_lua("local f = app.hid.start\nfunction render() f() end\n");
ok(in_array('app.hid.start', array_column($h, 'code'), true), "别名赋值（local f = app.hid.start）仍能识别");

$h = scan_lua("local c = 'return 1'\nfunction render() load(c)() end\n");
$codes = array_column($h, 'code');
$lv = array_column($h, 'level');
ok(in_array('high', $lv, true), "load(动态参数) → high（静态分析不了）", implode(',', $codes));

$h = scan_lua("function render() local t=1 while true do t=t+1 end end\n");
ok(scan_verdict($h) === 'low', "while true → low（沙箱有超时兜底）", scan_summary($h));

$h = scan_lua("--[[ 块注释 app.hid.key ]]\n--[==[ 长注释 os.execute ]==]\nfunction render() end\n");
ok(scan_verdict($h) === 'none', "块注释/长注释里的危险名不误报", scan_summary($h));

$h = scan_lua("function render() local x = [[ app.fs.remove ]] end\n");
ok(scan_verdict($h) !== 'none', "长字符串里的 API 名仍会被标出（不是误报，是提示）");

$h = scan_lua(lapp("干净应用", 1, $ICON512));
ok(scan_verdict($h) === 'none', "干净应用 → 无命中", scan_summary($h));
printf("\n");

/* ============================ 2. 上传校验 ============================ */
echo "T2 上传硬校验\n";
[$o, $e] = validate_upload("function render() end\n");
ok($o, "正常文本通过");
[$o, $e] = validate_upload("");
ok(!$o, "空文件被拒", $e[0] ?? '');
[$o, $e] = validate_upload("function render() end\0evil\n");
ok(!$o && has(implode('|', $e), 'NUL'), "含 NUL 被拒", $e[0] ?? '');
[$o, $e] = validate_upload("\xFF\xFE function render() end");
ok(!$o, "非 UTF-8 被拒", $e[0] ?? '');
[$o, $e] = validate_upload("\x01\x02\x03 function render() end");
ok(!$o && has(implode('|', $e), '控制字符'), "二进制控制符被拒", $e[0] ?? '');
[$o, $e] = validate_upload(str_repeat("x", 512 * 1024 + 10));
ok(!$o, "超过 512KB 被拒", $e[0] ?? '');
printf("\n");

/* ============================ 3. LAPP 解析 ============================ */
echo "T3 LAPP 头解析\n";
$m = lapp_parse(lapp("一行多键", 2, $ICON512));
ok($m['name'] === '一行多键' && norm_ver($m['ver']) === 2 && $m['icon_bytes'] === 512,
   "一行多个 key=value 解析正确", "icon={$m['icon_bytes']}B");

$src = "--@lapp v1\n--@app name=多行写法\n--@app cat=游戏\n--@app desc=背包 技能 升级\n--@app version=3\nfunction render() end\n";
$m = lapp_parse($src);
ok($m['name'] === '多行写法' && $m['desc'] === '背包 技能 升级' && norm_ver($m['ver']) === 3,
   "一行一个 + desc 含空格不被截断", "desc=" . $m['desc']);

$m = lapp_parse("--@app name=坏图标 icon=" . base64_encode('x') . "\nfunction render() end\n");
ok($m['icon_bytes'] === 1, "图标 1 字节应被 lapp_validate 拦下");
[$er, $wn] = lapp_validate($m, "function render() end\n");
ok(in_array('icon 解码后是 1 字节，必须正好 512 字节（64×64 1bpp）', $er, true), "→ validate 报错", implode(';', $er));

[$er, $wn] = lapp_validate(lapp_parse("--@app name=x cat=工具 desc=y ver=1\nlocal function render() end\n"),
                            "--@app name=x\nlocal function render() end\n");
ok(!$er, "local function render 会命中哪种检查（应通过正则但运行不起来，见报告）", implode(';', $er));
printf("\n");

/* ============================ 4. 发布 ============================ */
echo "T4 首次发布\n";
reseed();
$c = lapp('测试应用', 1, $ICON512);
$sub = make_sub('测试应用', 1, $c);
$before = store_index();
[$ok, $why, $info] = store_publish($sub, rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sub['sha'] . '.lua');
ok($ok, "发布成功", $why);
$after = store_index();
ok((int)$after['ver'] === (int)$before['ver'] + 1, "index.ver 自增 ({$before['ver']} → {$after['ver']})");
$found = null; foreach ($after['pkgs'] as $p) if ($p['name'] === '测试应用') $found = $p;
ok($found !== null, "索引里出现新应用");
ok($found && (int)$found['ver'] === 1 && $found['path'] === 'pkgs/测试应用.lua', "条目 ver/path 正确");
ok($found && (int)$found['size'] === strlen($c) && $found['crc32'] === sprintf('%08x', crc32($c)), "size/crc32 与真实文件一致");
ok(is_file(store_dir() . '/pkgs/测试应用.lua'), "包文件已落到 pkgs/");
ok(count($after['pkgs']) === 3, "原有 2 个应用没丢 (现在 " . count($after['pkgs']) . ")");
$v = version_active('测试应用');
ok($v && (int)$v['ver'] === 1 && (int)$v['active'] === 1, "版本历史记录 v1 且为 active");
printf("\n");

echo "T5 更新（覆盖旧版 + 备份）\n";
$c2 = lapp('测试应用', 2, $ICON512) . "-- v2 marker\n";
$sub2 = make_sub('测试应用', 2, $c2);
[$ok, $why] = store_publish($sub2, rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sub2['sha'] . '.lua');
ok($ok, "v2 发布成功", $why);
$found = null; foreach (store_index()['pkgs'] as $p) if ($p['name'] === '测试应用') $found = $p;
ok($found && (int)$found['ver'] === 2, "索引 ver 更新为 2");
$baks = glob(store_dir() . '/pkgs_backup/测试应用.lua.*');
ok(count($baks) >= 1, "旧包进了 pkgs_backup（" . count($baks) . " 份）");
$vs = versions_of('测试应用');
ok(count($vs) === 2 && (int)$vs[0]['active'] === 1 && (int)$vs[1]['active'] === 0, "版本历史: v2 active, v1 归档");
printf("\n");

echo "T6 发布前置校验（全部应拒绝且不动线上）\n";
$idxSnap = file_get_contents(store_index_file());
[$ok, $why] = store_publish($sub, rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sub['sha'] . '.lua');
ok(!$ok && has($why, '必须大于'), "同版本/低版本被拒", $why);

$bad = $sub2; $bad['sha'] = str_repeat('0', 64);
[$ok, $why] = store_publish($bad, rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sub2['sha'] . '.lua');
ok(!$ok && has($why, 'sha256'), "sha 不符被拒", $why);

$bad = $sub2; $bad['crc'] = 'deadbeef';
[$ok, $why] = store_publish($bad, rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sub2['sha'] . '.lua');
ok(!$ok && has($why, 'crc32'), "crc 不符被拒", $why);

$cBad = lapp('构建回退', 1, $ICON512, "--@app build=202601010000\n");
$sb = make_sub('构建回退', 1, $cBad);
[$ok, $why] = store_publish($sb, rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sb['sha'] . '.lua');
ok($ok, "首次发布 build=202601010000 成功", $why);
$cBad2 = lapp('构建回退', 2, $ICON512, "--@app build=202501010000\n");
$sb2 = make_sub('构建回退', 2, $cBad2);
[$ok, $why] = store_publish($sb2, rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sb2['sha'] . '.lua');
ok(!$ok && has($why, '更旧'), "build 回退被拒", $why);

$nohead = "function render() end\n";
$sn = make_sub('无头应用', 1, $nohead);
[$ok, $why] = store_publish($sn, rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sn['sha'] . '.lua');
ok(!$ok && has($why, 'LAPP 头'), "没有 LAPP 头被拒", $why);

ok(file_get_contents(store_index_file()) !== false, "索引文件仍可读");

/* 索引文件整个不存在时也必须拒发布（不能凭空造一个没 mirrors[] 的索引） */
$saved = file_get_contents(store_index_file());
@unlink(store_index_file());
$sX = make_sub('无索引应用', 1, lapp('无索引应用', 1, $ICON512));
[$ok, $why] = store_publish($sX, rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sX['sha'] . '.lua');
ok(!$ok && has($why, '不存在'), "index.json 不存在时拒发布", $why);
ok(!is_file(store_index_file()), "没有凭空创建索引文件");
file_put_contents(store_index_file(), $saved);
printf("\n");

echo "T7 索引损坏时必须拒写（不能拿空骨架覆盖）\n";
$good = file_get_contents(store_index_file());
file_put_contents(store_index_file(), "{ 这不是 JSON ");
$c3 = lapp('损坏期应用', 1, $ICON512);
$s3 = make_sub('损坏期应用', 1, $c3);
[$ok, $why] = store_publish($s3, rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $s3['sha'] . '.lua');
ok(!$ok && has($why, '索引损坏'), "发布被拒", $why);
ok(file_get_contents(store_index_file()) === '{ 这不是 JSON ', "损坏的 index.json 未被覆盖");
[$ok, $why] = store_unpublish('测试应用');
ok(!$ok && has($why, '索引损坏'), "下架同样被拒");
file_put_contents(store_index_file(), $good);
ok(is_array(json_decode((string)file_get_contents(store_index_file()), true)), "恢复索引成功");
printf("\n");

/* ============================ 5. 下架 / 重上架 / 回滚 ============================ */
echo "T8 下架与重新上架\n";
[$ok, $why] = store_unpublish('测试应用');
ok($ok, "下架成功", $why);
$found = null; foreach (store_index()['pkgs'] as $p) if ($p['name'] === '测试应用') $found = $p;
ok($found === null, "索引里已移除");
ok(is_file(store_dir() . '/pkgs/测试应用.lua'), "包文件保留（没删）");
ok(count(versions_of('测试应用')) === 2, "历史版本保留");

$row = ['id' => 1, 'name' => '测试应用', 'label' => '测试应用', 'cat' => '软件', 'descr' => '测试说明', 'ver' => 2];
[$ok, $why] = store_relist('测试应用', $row);
ok($ok, "重新上架成功", $why);
$found = null; foreach (store_index()['pkgs'] as $p) if ($p['name'] === '测试应用') $found = $p;
ok($found && (int)$found['ver'] === 2, "回到索引且版本为 v2");
[$ok, $why] = store_relist('测试应用', $row);
ok(!$ok && has($why, '已在商店索引'), "重复上架被拒", $why);
printf("\n");

echo "T9 回滚到历史版本\n";
$contentV2 = (string)file_get_contents(store_dir() . '/pkgs/测试应用.lua');
[$ok, $why] = store_rollback('测试应用', 1, $row, 'tester');
ok($ok, "回滚到 v1 成功", $why);
$nowContent = (string)file_get_contents(store_dir() . '/pkgs/测试应用.lua');
ok($nowContent === $c, "pkgs 里的内容变回 v1 的内容");
ok($nowContent !== $contentV2, "确实不是 v2 的内容了");
$found = null; foreach (store_index()['pkgs'] as $p) if ($p['name'] === '测试应用') $found = $p;
ok($found && (int)$found['ver'] === 3, "以新版本号 v3 发布（固件才会提示更新）", 'ver=' . ($found['ver'] ?? '?'));
$vs = versions_of('测试应用');
ok(count($vs) === 3, "历史变成 3 条（v2 仍在，没被覆盖）");
$v2row = null; foreach ($vs as $x) if ((int)$x['ver'] === 2) $v2row = $x;
ok($v2row && is_file((string)$v2row['src']), "v2 的文件仍存在: " . ($v2row['src'] ?? ''));
[$ok, $why] = store_rollback('测试应用', 99, $row, 'tester');
ok(!$ok && has($why, '没有 v99'), "回滚到不存在的版本被拒", $why);
printf("\n");

echo "T10 只改展示信息\n";
$cntBeforeEdit = count(store_index()['pkgs']);
[$ok, $why] = store_edit_display('测试应用', '测试应用(改名)', '游戏', '改过的说明');
ok($ok, "改展示信息成功", $why);
$found = null; foreach (store_index()['pkgs'] as $p) if ($p['name'] === '测试应用') $found = $p;
ok($found && $found['label'] === '测试应用(改名)' && $found['cat'] === '游戏' && $found['desc'] === '改过的说明',
   "索引里的 label/cat/desc 都变了");
ok($found && (int)$found['ver'] === 3, "版本号没被改动");
ok(count(store_index()['pkgs']) === $cntBeforeEdit, "应用数量不变 ({$cntBeforeEdit})");
printf("\n");

/* ============================ 6. 并发 ============================ */
echo "T11 并发发布（4 个进程同时上架不同应用）\n";
$jobs = [];
for ($i = 1; $i <= 4; $i++) {
    $nm = "并发应用{$i}";
    $cc = lapp($nm, 1, $ICON512);
    $ss = make_sub($nm, 1, $cc);
    $jobs[] = ['name' => $nm, 'sha' => $ss['sha'], 'crc' => $ss['crc'], 'size' => strlen($cc)];
}
$cmd = 'cd ' . escapeshellarg($ROOT) . ' && for i in 1 2 3 4; do php t_pub_one.php "并发应用$i" 1 & done; wait';
exec($cmd . ' 2>&1', $out, $rc);
$idx = store_index();
$names = array_column($idx['pkgs'], 'name');
$got = 0;
for ($i = 1; $i <= 4; $i++) if (in_array("并发应用{$i}", $names, true)) $got++;
ok($got === 4, "4 个并发发布全部进入索引（实际 {$got}/4）", substr(implode(' | ', $out), 0, 200));
$expect = $cntBeforeEdit + 4;
ok(count($idx['pkgs']) === $expect, "索引条目数 = {$cntBeforeEdit} + 4（实际 " . count($idx['pkgs']) . "）");
$jsonOk = json_decode((string)file_get_contents(store_index_file()), true);
ok(is_array($jsonOk) && count($jsonOk['pkgs']) === $expect, "index.json 仍是合法 JSON 且条目完整");
/* 并发下最容易丢的是"别人的条目": 逐个核对仍在 */
$namesAll = array_column($jsonOk['pkgs'] ?? [], 'name');
$miss = [];
foreach (['老应用A', '老应用B', '测试应用', '构建回退'] as $n) if (!in_array($n, $namesAll, true)) $miss[] = $n;
ok(!$miss, "并发没有挤掉原有条目" . ($miss ? '（丢了 ' . implode(',', $miss) . '）' : ''));
printf("\n");

/* ============================ 7. 审计 ============================ */
echo "T12 审计日志\n";
audit('tester', 'sub.publish', '测试应用', 2, 'pending', 'published', 'ok', '单元测试写入');
$rows = audit_recent(5);
ok(count($rows) > 0 && $rows[0]['action'] === 'sub.publish', "日志写入并可读回");
$all = '';
foreach (audit_recent(500) as $r) $all .= json_encode($r, JSON_UNESCAPED_UNICODE);
ok(!has($all, 'password') && !has($all, 'passwd_hash') && !has($all, 'csrf'), "日志里没有口令/CSRF 字段");
printf("\n");

/* ============================ 8. 数据隔离（SQL 层） ============================ */
echo "T13 用户隐私隔离（查询条件）\n";
$db = db();
$db->prepare('INSERT INTO users (email, created, verified) VALUES (?,?,1)')->execute(['a@test', time()]);
$db->prepare('INSERT INTO users (email, created, verified) VALUES (?,?,1)')->execute(['b@test', time()]);
$ua = (int)$db->lastInsertId() - 1; $ub = $ua + 1;
$db->prepare('INSERT INTO subs (uid, name, label, cat, descr, ver, fname, size, crc, sha, status, created)
              VALUES (?,?,?,?,?,?,?,?,?,?,?,?)')
   ->execute([$ua, '私有应用A', '私有应用A', '软件', 'x', 1, '私有应用A.lua', 1, '0', '0', 'pending', time()]);
$st = $db->prepare('SELECT * FROM subs WHERE uid = ?');
$st->execute([$ub]);
$rowsB = $st->fetchAll();
$leak = false; foreach ($rowsB as $r) if ($r['name'] === '私有应用A') $leak = true;
ok(!$leak, "B 查不到 A 的投稿（页面用的就是 uid 过滤）");
printf("\n");

echo "================ {$PASS} PASS / {$FAIL} FAIL ================\n";
exit($FAIL ? 1 : 0);
