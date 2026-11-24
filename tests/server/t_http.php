<?php
/**
 * t_http.php — 真实 HTTP 测试（打 http://127.0.0.1:8899，测试副本，不是生产）
 *
 *   php /tmp/bbk_test/t_http.php
 *
 * 依赖: 先启动 php -S 127.0.0.1:8899 -t /tmp/bbk_test/web
 * 覆盖: 未授权访问 / URL 令牌入口已废 / 管理员登录+首登改密+CSRF /
 *       投稿成功与各类拒绝 / 通过发布 / 下架 / 回滚 / 下载鉴权 / 用户隐私
 */
declare(strict_types=1);

$ROOT = getenv('BT_ROOT') ?: '/tmp/bbk_test';
if ($ROOT !== '/tmp/bbk_test' && getenv('BT_ALLOW_ANY') !== '1') {
    fwrite(STDERR, "拒绝执行: BT_ROOT={$ROOT}\n");
    exit(2);
}
$BASE = getenv('BT_BASE') ?: 'http://127.0.0.1:8899';
require $ROOT . '/web/lib.php';

$PASS = 0; $FAIL = 0;
function ok(bool $c, string $label, string $extra = ''): void {
    global $PASS, $FAIL;
    if ($c) { $PASS++; echo "  PASS  {$label}\n"; }
    else    { $FAIL++; echo "  FAIL  {$label}" . ($extra !== '' ? "  ({$extra})" : "") . "\n"; }
}
function has(string $h, string $n): bool { return strpos($h, $n) !== false; }

/** 每个"角色"一个 cookie jar */
function http(string $who, string $method, string $path, $body = null): array {
    global $BASE, $ROOT;
    /* Location 头是相对路径（"index.php"），直接拼会得到 8899index.php 这种废 URL */
    if ($path !== '' && $path[0] !== '/' && strpos($path, 'http') !== 0) $path = '/' . $path;
    $ch = curl_init($BASE . $path);
    curl_setopt_array($ch, [
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_HEADER         => true,
        CURLOPT_FOLLOWLOCATION => false,
        CURLOPT_COOKIEJAR      => $ROOT . "/cookies_{$who}.txt",
        CURLOPT_COOKIEFILE     => $ROOT . "/cookies_{$who}.txt",
        CURLOPT_TIMEOUT        => 20,
    ]);
    if ($method === 'POST') {
        curl_setopt($ch, CURLOPT_POST, true);
        curl_setopt($ch, CURLOPT_POSTFIELDS, $body);
    }
    $raw  = (string)curl_exec($ch);
    $code = (int)curl_getinfo($ch, CURLINFO_HTTP_CODE);
    $hs   = (int)curl_getinfo($ch, CURLINFO_HEADER_SIZE);
    curl_close($ch);
    return ['code' => $code, 'head' => substr($raw, 0, $hs), 'body' => substr($raw, $hs)];
}
function csrf_of(string $html): string {
    return preg_match('/name="_csrf" value="([0-9a-f]+)"/', $html, $m) ? $m[1] : '';
}
function location(string $head): string {
    return preg_match('/^Location:\s*(.+)$/mi', $head, $m) ? trim($m[1]) : '';
}
/** 伪造一个已登录会话（真实站点用邮箱验证码登录；没配 SMTP 时测试直接建会话） */
function forge(string $who, string $sid, array $kv): void {
    $kv['__who'] = $who;
    $sess = '';
    foreach ($kv as $k => $v) $sess .= '$_SESSION[' . var_export($k, true) . ']=' . var_export($v, true) . ';';
    $code = 'session_id(' . var_export($sid, true) . '); session_name("lintos_upload"); session_start();'
          . $sess . 'session_write_close();';
    global $BASE;
    exec('php -r ' . escapeshellarg($code) . ' 2>&1', $o, $rc);
    if ($rc !== 0) { echo "  (forge rc={$rc} " . implode(' ', $o) . ")\n"; return; }
    /* 关键: 光建 session 文件不够, 还得让客户端把这个 session id 当 cookie 发出去,
     * 否则服务端会新建一个空会话 —— 表现就是"已登录却提示请先登录"。 */
    $host = parse_url($BASE, PHP_URL_HOST);
    $jar  = 'cookies_' . $kv['__who'] . '.txt';
    $line = $host . "\tFALSE\t/\tFALSE\t0\tlintos_upload\t" . $sid . "\n";
    global $ROOT;
    @unlink($ROOT . '/' . $jar);
    file_put_contents($ROOT . '/' . $jar, "# Netscape HTTP Cookie File\n" . $line);
}

/* ---------- 重置测试数据 ---------- */
exec('rm -rf ' . escapeshellarg($ROOT . '/data') . ' ' . escapeshellarg($ROOT . '/bbk'));
exec('rm -f ' . $ROOT . '/cookies_*.txt');
foreach ([$ROOT . '/data/pkgs', $ROOT . '/data/backup', $ROOT . '/bbk/pkgs', $ROOT . '/bbk/pkgs_backup'] as $d)
    @mkdir($d, 0777, true);
$base = "APP base\n";
file_put_contents($ROOT . '/bbk/pkgs/基线应用.lua', $base);
file_put_contents($ROOT . '/bbk/index.json', json_encode([
    'ver'  => 5,
    'pkgs' => [[
        'name' => '基线应用', 'label' => '基线应用', 'cat' => '软件', 'desc' => '基线',
        'ver' => 1, 'size' => strlen($base), 'crc32' => sprintf('%08x', crc32($base)),
        'path' => 'pkgs/基线应用.lua',
    ]],
], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT));

$db = db();
$db->exec('DELETE FROM users');
$db->exec('DELETE FROM subs');
$db->exec('DELETE FROM admins');
$db->exec('DELETE FROM versions');
$db->exec('DELETE FROM audit_log');
$db->exec('DELETE FROM login_fails');
$db->prepare('INSERT INTO users (email, created, verified) VALUES (?,?,1)')->execute(['a@test.local', time()]);
$uidA = (int)$db->lastInsertId();
$db->prepare('INSERT INTO users (email, created, verified) VALUES (?,?,1)')->execute(['b@test.local', time()]);
$uidB = (int)$db->lastInsertId();

echo "=== LinTOS 投稿平台 · HTTP 测试 ({$BASE}) ===\n\n";

/* ============================ A. 未授权访问 ============================ */
echo "A 未授权访问\n";
$r = http('anon', 'GET', '/review.php');
ok($r['code'] === 403, "review.php 未登录 → 403", 'code=' . $r['code']);
$r = http('anon', 'GET', '/review.php?k=anything');
ok($r['code'] === 403, "review.php?k=xxx → 仍然 403（URL 令牌授权已移除）", 'code=' . $r['code']);
$r = http('anon', 'GET', '/review.php?log=1');
ok($r['code'] === 403, "操作日志页未登录 → 403");
$r = http('anon', 'GET', '/admin_cli.php');
ok($r['code'] === 403 || has($r['body'], '只能在服务器命令行'),
   "admin_cli.php 通过 HTTP 访问被拒", 'code=' . $r['code']);
$r = http('anon', 'GET', '/download.php?id=1');
ok($r['code'] === 403, "download.php 未登录 → 403");
$r = http('anon', 'GET', '/index.php');
ok($r['code'] === 200 && has($r['body'], '提交一个应用'), "投稿首页可公开访问");
$r = http('anon', 'GET', '/spec.php');
ok($r['code'] === 200 && has($r['body'], 'Lua 应用开发规范'), "开发规范页可公开访问");
$r = http('anon', 'GET', '/spec.php?tpl=1');
ok($r['code'] === 200 && has($r['body'], '--@lapp v1') && has($r['body'], 'function render'),
   "模板可下载且是最小可运行代码");
ok(!has($r['body'], '<?php'), "模板里没有 PHP 代码");
printf("\n");

/* ============================ B. 管理员初始化与登录 ============================ */
echo "B 管理员初始化 / 登录 / 首登改密 / CSRF\n";
exec('cd ' . escapeshellarg($ROOT . '/web') . ' && php admin_cli.php init boss 2>&1', $o, $rc);
$out = implode("\n", $o);
ok($rc === 0 && has($out, '初始口令'), "CLI 初始化成功", substr($out, 0, 80));
$initPass = '';
if (preg_match('/初始口令:\s*(\S+)/', $out, $m)) $initPass = $m[1];
ok($initPass !== '' && strlen($initPass) >= 20, "生成了随机口令（长度 " . strlen($initPass) . "）");

exec('cd ' . escapeshellarg($ROOT . '/web') . ' && php admin_cli.php init boss2 2>&1', $o2, $rc2);
ok($rc2 !== 0 && has(implode("\n", $o2), '已经有管理员'), "重复 init 被拒（不会重置已有账户）");

/* 登录页 */
$r = http('adm', 'GET', '/admin_login.php');
ok($r['code'] === 200 && has($r['body'], '管理登录'), "登录页可访问");
$csrf = csrf_of($r['body']);
ok($csrf !== '', "登录页带 CSRF");

/* 缺 CSRF 的登录 */
$r = http('adm', 'POST', '/admin_login.php', ['do' => 'login', 'user' => 'boss', 'pass' => $initPass]);
ok(has($r['body'], '页面过期'), '缺 CSRF 的登录被拒');

/* 密码错 */
$r = http('adm', 'POST', '/admin_login.php', ['_csrf' => $csrf, 'do' => 'login', 'user' => 'boss', 'pass' => 'wrong-pass-1']);
ok(has($r['body'], '用户名或密码不对'), '错误口令被拒（不区分账号是否存在）');

/* 密码对 */
$r = http('adm', 'POST', '/admin_login.php', ['_csrf' => $csrf, 'do' => 'login', 'user' => 'boss', 'pass' => $initPass]);
ok($r['code'] === 302 && has(location($r['head']), 'change=1'), "首登必须改密（跳到 change=1）", location($r['head']));

/* 未改密前访问后台 */
$r = http('adm', 'GET', '/review.php');
ok($r['code'] === 302 && has(location($r['head']), 'admin_login'), "未改密时后台不可用", 'code=' . $r['code']);

/* 改密：先太短 */
$r = http('adm', 'GET', '/admin_login.php?change=1');
$csrf = csrf_of($r['body']);
$r = http('adm', 'POST', '/admin_login.php', ['_csrf' => $csrf, 'do' => 'passwd', 'p1' => 'short', 'p2' => 'short']);
ok(has($r['body'], '至少 10 位'), "弱口令被拒");
$r = http('adm', 'POST', '/admin_login.php', ['_csrf' => $csrf, 'do' => 'passwd', 'p1' => 'LinTOS-test-2026', 'p2' => 'nope-nope-nope']);
ok(has($r['body'], '不一致'), "两次不一致被拒");
$r = http('adm', 'POST', '/admin_login.php', ['_csrf' => $csrf, 'do' => 'passwd',
                                              'p1' => 'LinTOS-test-2026', 'p2' => 'LinTOS-test-2026']);
ok($r['code'] === 302 && has(location($r['head']), 'review.php'), "改密成功进入后台", location($r['head']));

$r = http('adm', 'GET', '/review.php');
ok($r['code'] === 200 && has($r['body'], '审核后台'), "后台可访问");
ok(!has($r['body'], 'review_key'), "页面里不再出现 review_key");
printf("\n");

/* ============================ C. 投稿 ============================ */
echo "C 投稿（登录用户）\n";
$sidA = 'testsessA' . bin2hex(random_bytes(5));   /* PHP 只允许字母数字 , - */
$csrfA = bin2hex(random_bytes(16));
forge('ua', $sidA, ['uid' => $uidA, 'csrf' => $csrfA]);

function lapp_src(string $name, int $ver, string $extra = ''): string {
    $icon = base64_encode(str_repeat("\xA5", 512));
    return "--@lapp v1\n--@app name={$name} cat=软件 desc=投稿测试 version={$ver}\n--@app icon={$icon}\n{$extra}"
         . "function render() app.clear(app.WHITE) end\nfunction action(a) return true end\n";
}
/** 带文件的 multipart POST */
function post_file(string $who, array $fields, string $fname, string $content): array {
    global $BASE, $ROOT;
    $fields['lua'] = new CURLFile(self_tmp($content), 'text/plain', $fname);
    $ch = curl_init($BASE . '/submit.php');
    curl_setopt_array($ch, [
        CURLOPT_RETURNTRANSFER => true, CURLOPT_HEADER => true, CURLOPT_FOLLOWLOCATION => false,
        CURLOPT_COOKIEJAR => $ROOT . "/cookies_{$who}.txt", CURLOPT_COOKIEFILE => $ROOT . "/cookies_{$who}.txt",
        CURLOPT_POST => true, CURLOPT_POSTFIELDS => $fields, CURLOPT_TIMEOUT => 25,
    ]);
    $raw = (string)curl_exec($ch);
    $code = (int)curl_getinfo($ch, CURLINFO_HTTP_CODE);
    $hs = (int)curl_getinfo($ch, CURLINFO_HEADER_SIZE);
    curl_close($ch);
    return ['code' => $code, 'head' => substr($raw, 0, $hs), 'body' => substr($raw, $hs)];
}
function self_tmp(string $c): string {
    static $i = 0;
    $p = sys_get_temp_dir() . '/upl_' . getmypid() . '_' . (++$i) . '.lua';
    file_put_contents($p, $c);
    return $p;
}
/** 取 flash 消息（跟随跳转回 index.php） */
function flash_after(string $who, string $loc): string {
    $r = http($who, 'GET', $loc);
    return $r['body'];
}

/* 未登录上传 */
$r = http('anon2', 'POST', '/submit.php', ['_csrf' => 'x']);
ok($r['code'] === 302 && has(location($r['head']), 'index.php'), "未登录 POST submit 被弹回首页", location($r['head']));

/* 缺 CSRF */
$r = post_file('ua', ['_csrf' => 'deadbeef', 'name' => 'X', 'ver' => '1'], 'x.lua', lapp_src('X', 1));
ok($r['code'] === 302 && has(location($r['head']), 'index.php'), "缺 CSRF 的上传被拒");
$body = flash_after('ua', location($r['head']));
ok(has($body, '页面过期'), "提示 CSRF 失败");

/* 正常投稿 */
$r = post_file('ua', ['_csrf' => $csrfA, 'name' => '投稿应用', 'cat' => '软件', 'descr' => '投稿测试说明', 'ver' => '1'],
               '投稿应用.lua', lapp_src('投稿应用', 1));
ok($r['code'] === 302, "正常投稿返回 302");
$body = flash_after('ua', location($r['head']));
ok(has($body, '已提交'), "提示已提交", mb_substr(strip_tags($body), 0, 0));
$row = $db->query("SELECT * FROM subs WHERE name='投稿应用'")->fetch();
ok($row && $row['status'] === 'pending', "入库且状态 pending");
ok($row && $row['sha'] === hash('sha256', lapp_src('投稿应用', 1)), "sha 由服务端重算且正确");
ok($row && $row['verdict'] === 'none', "扫描结论 none（干净代码）", $row['verdict'] ?? '');
$subId = (int)($row['id'] ?? 0);

/* 同 ID 再投（应拒） */
$r = post_file('ua', ['_csrf' => $csrfA, 'name' => '投稿应用', 'cat' => '软件', 'descr' => 'x', 'ver' => '2'],
               '投稿应用.lua', lapp_src('投稿应用', 2));
$body = flash_after('ua', location($r['head']));
ok(has($body, '等审核'), "同一应用已有待审 → 拒绝重复提交");

/* NUL 字节 */
$r = post_file('ua', ['_csrf' => $csrfA, 'name' => '坏文件', 'cat' => '软件', 'descr' => 'x', 'ver' => '1'],
               '坏文件.lua', lapp_src('坏文件', 1) . "\0BIN");
$body = flash_after('ua', location($r['head']));
ok(has($body, 'NUL'), "含 NUL 的文件被拒");

/* 非 UTF-8 */
$r = post_file('ua', ['_csrf' => $csrfA, 'name' => '乱码', 'cat' => '软件', 'descr' => 'x', 'ver' => '1'],
               '乱码.lua', "\xFF\xFE" . lapp_src('乱码', 1));
$body = flash_after('ua', location($r['head']));
ok(has($body, 'UTF-8'), "非 UTF-8 被拒");

/* 没有 LAPP 头 */
$r = post_file('ua', ['_csrf' => $csrfA, 'name' => '无头', 'cat' => '软件', 'descr' => 'x', 'ver' => '1'],
               '无头.lua', "function render() end\n");
$body = flash_after('ua', location($r['head']));
ok(has($body, 'LAPP 头'), "没有 LAPP 头被拒");

/* 版本号不比商店里大 */
$r = post_file('ua', ['_csrf' => $csrfA, 'name' => '基线应用', 'cat' => '软件', 'descr' => 'x', 'ver' => '1'],
               '基线应用.lua', lapp_src('基线应用', 1));
$body = flash_after('ua', location($r['head']));
ok(has($body, '必须大于'), "版本号不大于商店版本 → 拒绝");

/* 恶意文件名：内容合法，但文件名带路径 —— 落盘名应由内容决定 */
$r = post_file('ua', ['_csrf' => $csrfA, 'name' => '路径攻击', 'cat' => '软件', 'descr' => 'x', 'ver' => '1'],
               '../../evil.php', lapp_src('路径攻击', 1));
ok($r['code'] === 302, "带 .. 的上传文件名不导致服务端错误");
$r2 = http('ua', 'GET', '/index.php');
ok(!has($r2['body'], 'evil.php'), "页面不显示原始上传文件名");
ok(!is_file($ROOT . '/data/pkgs/evil.php') && !is_file('/tmp/evil.php'), "服务端没有按客户端文件名落盘");
printf("\n");

/* ============================ D. 隐私隔离 ============================ */
echo "D 用户隐私隔离\n";
$sidB = 'testsessB' . bin2hex(random_bytes(5));
$csrfB = bin2hex(random_bytes(16));
forge('ub', $sidB, ['uid' => $uidB, 'csrf' => $csrfB]);
$r = http('ub', 'GET', '/index.php');
ok(!has($r['body'], '投稿应用'), "B 看不到 A 的投稿");
$r = http('ua', 'GET', '/index.php');
ok(has($r['body'], '投稿应用'), "A 能看到自己的投稿");
ok(has($r['body'], '待审核'), "状态显示为中文");
printf("\n");

/* ============================ E. 审核通过 → 发布 ============================ */
echo "E 审核通过并发布\n";
$r = http('adm', 'GET', '/review.php?id=' . $subId);
ok($r['code'] === 200 && has($r['body'], '投稿应用'), "详情页可打开");
$csrfAdm = csrf_of($r['body']);
ok(has($r['body'], '包内 LAPP 头'), "详情页展示包内 LAPP 头（可与网页展示对照）");
ok(has($r['body'], '代码能力扫描'), "详情页展示扫描结果");

/* 缺 CSRF 的通过操作 */
$r = http('adm', 'POST', '/review.php', ['id' => $subId, 'a' => 'approve']);
ok(has($r['body'], 'CSRF'), "审核动作缺 CSRF 被拒");

/* 正常通过 */
$r = http('adm', 'POST', '/review.php', ['_csrf' => $csrfAdm, 'id' => $subId, 'a' => 'approve']);
ok(has($r['body'], '已写入商店索引') || has($r['body'], '已上架'), "通过并发布成功",
   mb_substr(strip_tags($r['body']), 0, 0));
$idx = json_decode((string)file_get_contents($ROOT . '/bbk/index.json'), true);
$names = array_column($idx['pkgs'] ?? [], 'name');
ok(in_array('投稿应用', $names, true), "index.json 里出现新应用");
ok(in_array('基线应用', $names, true), "原有应用没丢");
$e = null; foreach ($idx['pkgs'] as $p) if ($p['name'] === '投稿应用') $e = $p;
ok($e && isset($e['label'], $e['cat'], $e['desc'], $e['ver'], $e['size'], $e['crc32'], $e['path']),
   "条目字段齐全（冻结协议未被改）");
/* 条目里的键必须都在固件已知集合内（多一个未知键不会让旧固件崩，但没必要） */
$known = ['name','label','cat','desc','ver','size','crc32','path','type','target','author','date','build'];
$extra = array_diff(array_keys($e ?: []), $known);
ok($e && !$extra, "条目字段都在固件已知集合内" . ($extra ? '（多出：' . implode(',', $extra) . '）' : ''));
ok($e && isset($e['build']) && preg_match('/^\d{12}$/', (string)$e['build']) === 1,
   "写入了 build=YYYYMMDDHHMM（固件按它判更新）", $e['build'] ?? '');
ok(is_file($ROOT . '/bbk/pkgs/投稿应用.lua'), "包已落到 pkgs/");
$st = $db->query("SELECT * FROM subs WHERE name='投稿应用'")->fetch();
ok($st && $st['status'] === 'published', "投稿状态 → published");
$au = $db->query("SELECT * FROM audit_log WHERE action='sub.publish' ORDER BY id DESC LIMIT 1")->fetch();
ok($au && $au['app_id'] === '投稿应用' && $au['result'] === 'ok', "审计日志记录了发布");
printf("\n");

/* ============================ F. 下架 / 重上架 / 回滚 ============================ */
echo "F 下架 / 重新上架 / 回滚\n";
$r = http('adm', 'GET', '/review.php?id=' . $subId);
$csrfAdm = csrf_of($r['body']);
$r = http('adm', 'POST', '/review.php', ['_csrf' => $csrfAdm, 'id' => $subId, 'a' => 'unpublish']);
ok(has($r['body'], '已下架'), "下架成功", mb_substr(strip_tags($r['body']), 0, 0));
$idx = json_decode((string)file_get_contents($ROOT . '/bbk/index.json'), true);
ok(!in_array('投稿应用', array_column($idx['pkgs'], 'name'), true), "索引里已移除");
ok(is_file($ROOT . '/bbk/pkgs/投稿应用.lua'), "包文件保留");

$r = http('adm', 'GET', '/review.php?id=' . $subId);
$csrfAdm = csrf_of($r['body']);
$r = http('adm', 'POST', '/review.php', ['_csrf' => $csrfAdm, 'id' => $subId, 'a' => 'relist']);
ok(has($r['body'], '已重新上架'), "重新上架成功");
$idx = json_decode((string)file_get_contents($ROOT . '/bbk/index.json'), true);
ok(in_array('投稿应用', array_column($idx['pkgs'], 'name'), true), "回到索引");

/* 用 v2 覆盖后再回滚到 v1 */
$sidA2 = $sidA;
$r = post_file('ua', ['_csrf' => $csrfA, 'name' => '投稿应用', 'cat' => '软件', 'descr' => 'v2', 'ver' => '2'],
               '投稿应用.lua', lapp_src('投稿应用', 2, "-- v2 marker\n"));
$row2 = $db->query("SELECT * FROM subs WHERE name='投稿应用' AND ver=2")->fetch();
ok($row2 !== false && $row2 !== null, "v2 投稿入库");
$r = http('adm', 'POST', '/review.php', ['_csrf' => $csrfAdm, 'id' => (int)$row2['id'], 'a' => 'approve']);
$idx = json_decode((string)file_get_contents($ROOT . '/bbk/index.json'), true);
$e = null; foreach ($idx['pkgs'] as $p) if ($p['name'] === '投稿应用') $e = $p;
ok($e && (int)$e['ver'] === 2, "商店里变成 v2");

$r = http('adm', 'GET', '/review.php?id=' . $subId);
$csrfAdm = csrf_of($r['body']);
ok(has($r['body'], '版本历史'), "详情页有版本历史");
$r = http('adm', 'POST', '/review.php', ['_csrf' => $csrfAdm, 'id' => $subId, 'a' => 'rollback', 'to_ver' => '1']);
ok(has($r['body'], '需要显式确认'), "回滚未确认被拒");
$r = http('adm', 'POST', '/review.php', ['_csrf' => $csrfAdm, 'id' => $subId, 'a' => 'rollback',
                                          'to_ver' => '1', 'confirm' => 'yes']);
ok(has($r['body'], '已回滚'), "回滚成功", mb_substr(strip_tags($r['body']), 0, 0));
$content = (string)file_get_contents($ROOT . '/bbk/pkgs/投稿应用.lua');
ok(!has($content, 'v2 marker'), "pkgs 内容变回 v1（没有 v2 marker）");
$idx = json_decode((string)file_get_contents($ROOT . '/bbk/index.json'), true);
$e = null; foreach ($idx['pkgs'] as $p) if ($p['name'] === '投稿应用') $e = $p;
ok($e && (int)$e['ver'] === 3, "回滚后以 v3 发布（固件才会提示更新）", 'ver=' . ($e['ver'] ?? '?'));
printf("\n");

/* ============================ G. 改展示信息 ============================ */
echo "G 修改商店展示信息\n";
$r = http('adm', 'POST', '/review.php', ['_csrf' => $csrfAdm, 'id' => $subId, 'a' => 'edit',
                                          'label' => '投稿应用改', 'cat' => '游戏', 'descr' => '改过的说明']);
ok(has($r['body'], '展示信息已更新') || has($r['body'], '已保存'), "保存成功",
   mb_substr(strip_tags($r['body']), 0, 0));
$idx = json_decode((string)file_get_contents($ROOT . '/bbk/index.json'), true);
$e = null; foreach ($idx['pkgs'] as $p) if ($p['name'] === '投稿应用') $e = $p;
ok($e && $e['label'] === '投稿应用改' && $e['cat'] === '游戏', "索引里的展示信息已变");
ok($e && (int)$e['ver'] === 3, "版本号没被改动");
printf("\n");

/* ============================ H. 下载鉴权 ============================ */
echo "H 下载原始投稿文件\n";
$r = http('adm', 'GET', '/download.php?id=' . $subId);
ok($r['code'] === 200 && has($r['body'], '--@app name=投稿应用'), "管理员可下载且内容是原包");
ok(has($r['head'], 'Content-Disposition: attachment'), "以附件形式下发（不会被当成页面执行）");
$r = http('ua', 'GET', '/download.php?id=' . $subId);
ok($r['code'] === 403, "普通用户下载 → 403");
$r = http('adm', 'GET', '/download.php?id=999999');
ok($r['code'] === 404, "不存在的 id → 404");
printf("\n");

/* ============================ I. 操作日志页 ============================ */
echo "I 操作日志\n";
$r = http('adm', 'GET', '/review.php?log=1');
ok($r['code'] === 200 && has($r['body'], 'sub.publish') && has($r['body'], 'app.rollback'),
   "日志页包含发布与回滚记录");
ok(!has($r['body'], 'LinTOS-test-2026'), "日志里没有管理员口令");
printf("\n");

echo "================ {$PASS} PASS / {$FAIL} FAIL ================\n";
exit($FAIL ? 1 : 0);
