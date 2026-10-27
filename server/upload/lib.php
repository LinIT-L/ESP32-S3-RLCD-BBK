<?php
/**
 * LinTOS 应用提交站 —— 公共库
 * 单文件包含：配置读取 / SQLite / LAPP 解析与校验 / 危险能力扫描 / SMTP 发信 / 会话与限频
 */
declare(strict_types=1);

function cfg(?string $k = null, $def = null) {
    static $c = null;
    if ($c === null) $c = require __DIR__ . '/config.php';
    if ($k === null) return $c;
    return $c[$k] ?? $def;
}

function h($s): string { return htmlspecialchars((string)$s, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8'); }

/* ---------------------------------------------------------------- 数据库 */

function db(): PDO {
    static $pdo = null;
    if ($pdo) return $pdo;
    $dir = (string)cfg('data_dir');
    foreach ([$dir, $dir . '/pkgs'] as $d)
        if (!is_dir($d)) @mkdir($d, 0700, true);

    $pdo = new PDO('sqlite:' . $dir . '/app.db', null, null, [
        PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
    ]);
    $pdo->exec('PRAGMA journal_mode=WAL');
    $pdo->exec('PRAGMA busy_timeout=5000');

    $pdo->exec('CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        email TEXT UNIQUE NOT NULL,
        created INTEGER NOT NULL,
        verified INTEGER NOT NULL DEFAULT 0,
        last_login INTEGER
    )');
    $pdo->exec('CREATE TABLE IF NOT EXISTS codes (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        email TEXT NOT NULL,
        code TEXT NOT NULL,
        expires INTEGER NOT NULL,
        used INTEGER NOT NULL DEFAULT 0
    )');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_codes_email ON codes(email)');
    $pdo->exec('CREATE TABLE IF NOT EXISTS sessions (
        token TEXT PRIMARY KEY,
        uid INTEGER NOT NULL,
        expires INTEGER NOT NULL
    )');
    $pdo->exec('CREATE TABLE IF NOT EXISTS subs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        uid INTEGER NOT NULL,
        name TEXT, label TEXT, cat TEXT, descr TEXT, ver INTEGER,
        fname TEXT, size INTEGER, crc TEXT, sha TEXT,
        risks TEXT, status TEXT NOT NULL DEFAULT \'pending\',
        created INTEGER NOT NULL, reviewed INTEGER, note TEXT, ip TEXT,
        pub_ver INTEGER
    )');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_subs_uid ON subs(uid)');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_subs_status ON subs(status)');
    return $pdo;
}

/* ---------------------------------------------------------------- 会话 */

function boot_session(): void {
    if (session_status() === PHP_SESSION_ACTIVE) return;
    session_set_cookie_params([
        'lifetime' => 0, 'path' => '/upload/', 'httponly' => true, 'samesite' => 'Lax',
    ]);
    session_name('lintos_upload');
    session_start();
}

function current_user(): ?array {
    boot_session();
    $uid = (int)($_SESSION['uid'] ?? 0);
    if (!$uid) return null;
    static $u = null;
    if ($u && (int)$u['id'] === $uid) return $u;
    $st = db()->prepare('SELECT * FROM users WHERE id = ?');
    $st->execute([$uid]);
    $u = $st->fetch() ?: null;
    return $u;
}

function login_user(int $uid): void {
    boot_session();
    session_regenerate_id(true);
    $_SESSION['uid'] = $uid;
}

function logout_user(): void {
    boot_session();
    $_SESSION = [];
    session_destroy();
}

function csrf(): string {
    boot_session();
    if (empty($_SESSION['csrf'])) $_SESSION['csrf'] = bin2hex(random_bytes(16));
    return $_SESSION['csrf'];
}

function csrf_ok(): bool {
    boot_session();
    return hash_equals((string)($_SESSION['csrf'] ?? ''), (string)($_POST['_csrf'] ?? ''));
}

function flash(string $msg, string $type = 'ok'): void {
    boot_session();
    $_SESSION['flash'] = ['m' => $msg, 't' => $type];
}

function take_flash(): ?array {
    boot_session();
    $f = $_SESSION['flash'] ?? null;
    unset($_SESSION['flash']);
    return $f;
}

/* ---------------------------------------------------------------- LAPP 解析 */

/**
 * 从 `--@app` 行体里拆出所有 key=value。**两种写法都要支持**：
 *   ① 一行多个：  --@app name=QQ堂冒险 cat=游戏 desc=泡泡炸砖块 ver=1 icon=<b64>
 *   ② 一行一个：  --@app name=矢量地图 / --@app cat=工具 ...
 * 不能简单按空格切开 —— desc 里可能有空格（"背包 技能"），按空格切会截断。
 * 做法：先找出所有 "key=" 的位置，值 = 从等号后到下一个 "key=" 之前。
 */
function parse_kv_line(string $body): array {
    $out = [];
    if (!preg_match_all('/(?:^|\s)([A-Za-z_]+)=/', $body, $mm, PREG_OFFSET_CAPTURE)) return $out;
    $n = count($mm[0]);
    for ($i = 0; $i < $n; $i++) {
        $key  = $mm[1][$i][0];
        $from = $mm[0][$i][1] + strlen($mm[0][$i][0]);
        $to   = ($i + 1 < $n) ? $mm[0][$i + 1][1] : strlen($body);
        $out[$key] = rtrim(substr($body, $from, max(0, $to - $from)));
    }
    return $out;
}

/**
 * 解析 `--@app key=value` 头。规则与固件 components/services/apppack/apppack.c 一致：
 * 头部结束于第一行非注释非空行。icon 是 64x64 1bpp 的 base64(512B)。
 */
function lapp_parse(string $src): array {
    $m = [
        'has_head' => false, 'name' => '', 'cat' => '', 'desc' => '',
        'ver' => '', 'icon' => '', 'icon_bytes' => 0, 'need' => [],
        'fields' => [],
    ];
    $head = substr($src, 0, 8192);
    foreach (preg_split('/\r\n|\n|\r/', $head) as $line) {
        if (strncmp($line, '--@app ', 7) === 0) {
            $m['has_head'] = true;
            foreach (parse_kv_line(substr($line, 7)) as $k => $v) {
                $m['fields'][$k] = $v;
                switch ($k) {
                    case 'name': $m['name'] = $v; break;
                    case 'cat':  $m['cat']  = $v; break;
                    case 'desc': $m['desc'] = $v; break;
                    case 'ver':
                    case 'version': $m['ver'] = $v; break;
                    case 'icon':
                        $m['icon'] = $v;
                        $raw = base64_decode(preg_replace('/\s+/', '', $v), true);
                        $m['icon_bytes'] = $raw === false ? 0 : strlen($raw);
                        break;
                    case 'need':
                        $m['need'] = array_filter(array_map('trim', explode(',', strtolower($v))));
                        break;
                }
            }
        } elseif ($line !== '' && $line[0] !== '-') {
            break;   /* 头部结束 */
        }
    }
    return $m;
}

/**
 * 分类归一化 —— **与 tools/store_build.py 的 CAT_NORMALIZE 完全一致**。
 * 老应用头里常写 工具/程序/系统/网络，这些不是商店的 5 个分类，
 * 若各写一套映射，同一应用在商店里会落进不同分类（违反"全局约束只能有一套"）。
 */
function norm_cat(string $c): string {
    static $map = [
        '程序' => '软件', '工具' => '软件', '应用' => '软件',
        '系统' => '标准', '设置' => '标准',
        '网络' => '运维',
    ];
    $c = trim($c);
    if ($c === '') return '软件';
    $c = $map[$c] ?? $c;
    return in_array($c, (array)cfg('cats'), true) ? $c : '软件';
}

/** 版本号归一化：`ver=1.0` / `ver=v2` 都取第一段数字 → 2（0 = 解析不出） */
function norm_ver(string $v): int {
    if (preg_match('/(\d{1,3})/', $v, $m)) return max(1, (int)$m[1]);
    return 0;
}

/** 校验元信息 + 代码骨架。返回 [errors[], warnings[]] —— errors 阻挡提交，warnings 只提示。 */
function lapp_validate(array $m, string $src): array {
    $err = [];
    $warn = [];
    $cats = (array)cfg('cats');

    if (!$m['has_head']) {
        $err[] = '文件没有 LAPP 头。第一行应形如：--@app name=应用名 cat=游戏 desc=一句话说明 ver=1 icon=<base64>';
    }
    if ($m['name'] === '') {
        $err[] = '缺少 name（应用显示名）';
    } elseif (mb_strlen($m['name'], 'UTF-8') > 24) {
        $err[] = 'name 太长（最多 24 个字）';
    }

    /* 分类：老应用写 工具/程序/系统/网络 的很多，norm_cat 能兜底到 5 类，
     * 所以这里只**提示**不拦截（拦住的话这些老应用永远更新不了）。 */
    if ($m['cat'] === '') {
        $warn[] = '缺少 cat（分类），会归到「软件」';
    } elseif (!in_array($m['cat'], $cats, true)) {
        $warn[] = 'cat="' . $m['cat'] . '" 不是商店分类，会归到「' . norm_cat($m['cat']) . '」';
    }

    if ($m['desc'] === '') {
        $warn[] = '缺少 desc（商店列表第二行的说明），会显示为空';
    } elseif (mb_strlen($m['desc'], 'UTF-8') > 40) {
        $warn[] = 'desc 超过 40 个字，商店里会被截断（建议 ≤ 40）';
    }

    $verNum = norm_ver((string)$m['ver']);
    if ($verNum <= 0) {
        $err[] = '缺少 ver（版本号）。必须是整数，例如 ver=1；发布更新时 +1';
    } elseif ((string)$m['ver'] !== (string)$verNum) {
        $warn[] = '版本号写的是 ' . $m['ver'] . '，会按整数 v' . $verNum . ' 记录（下次更新请直接写 v' . ($verNum + 1) . '）';
    }

    if ($m['icon'] === '') {
        $warn[] = '缺少 icon（64×64 图标）。上架后商店会用该分类的默认图标';
    } elseif ($m['icon_bytes'] !== 512) {
        $err[] = 'icon 解码后是 ' . $m['icon_bytes'] . ' 字节，必须正好 512 字节（64×64 1bpp）';
    }

    /* 代码骨架：Lua 应用必须提供 render。两种写法都算 ——
     *   ① return { render = render_fn }    ② function render() ... end */
    if (!preg_match('/\brender\s*=\s*[A-Za-z_]/', $src)
        && !preg_match('/function\s+render\s*\(/', $src)) {
        $err[] = '代码里没有 render（界面绘制函数）。Lua 应用必须 return { render=..., action=... }';
    }
    if (!preg_match('/\baction\s*=\s*[A-Za-z_]/', $src) && !preg_match('/function\s+action\s*\(/', $src)) {
        $warn[] = '没找到 action（按键处理）。触摸专用的应用可以忽略';
    }
    if (strlen($src) > (int)cfg('limits')['max_bytes']) {
        $err[] = '文件超过 ' . (int)(cfg('limits')['max_bytes'] / 1024) . ' KB';
    }
    /* 非 UTF-8 会让固件的中文字体画出乱码 */
    if (!mb_check_encoding($src, 'UTF-8')) {
        $err[] = '文件不是 UTF-8 编码（中文会显示成乱码），请另存为 UTF-8';
    }
    return [$err, $warn];
}

/** 危险能力扫描。返回 [ ['level'=>'high|med|low','api'=>..,'why'=>..], ... ] */
function scan_risks(string $src): array {
    static $rules = [
        ['app.hid.start',      'high', '能模拟 USB 键盘/鼠标（BadUSB 风险，插到电脑上可直接操作对方机器）'],
        ['app.hid.shortcut',   'high', '能发送快捷键组合'],
        ['app.hid.key',        'high', '能发送按键'],
        ['app.hid.mouse_click','high', '能控制鼠标点击'],
        ['app.hid.cdc_read',   'med',  '能读取 USB 串口数据'],
        ['app.fs.remove',      'high', '能删除设备上的文件'],
        ['app.fs.write',       'med',  '能写入设备上的文件'],
        ['app.fs.rename',      'med',  '能改名/移动文件'],
        ['app.net.http_get',   'med',  '会联网获取数据'],
        ['app.net.fetch_start','med',  '会后台联网下载'],
        ['app.net.port',       'med',  '能做端口探测'],
        ['app.espnow',         'med',  '使用 ESP-NOW 无线组网收发'],
        ['app.wifi',           'low',  '会操作 WiFi'],
        ['app.rec.start',      'med',  '会启用麦克风录音'],
        ['while true do',      'low',  '含死循环（游戏主循环常见，确认不会卡死系统）'],
    ];
    $out = [];
    foreach ($rules as [$api, $lvl, $why]) {
        if (strpos($src, $api) !== false) {
            $out[] = ['level' => $lvl, 'api' => $api, 'why' => $why];
        }
    }
    /* 硬编码 IP:端口 —— 外联到具体服务器，值得看一眼 */
    if (preg_match_all('/\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}:\d{2,5}\b/', $src, $mm)) {
        $out[] = ['level' => 'med', 'api' => '硬编码地址',
                  'why' => '直连 IP：' . implode(', ', array_unique(array_slice($mm[0], 0, 3)))];
    }
    /* 标准库逃逸：若沙箱暴露了 io/os，脚本能绕过桥直接操作 */
    if (preg_match('/\b(io\.open|io\.popen|os\.execute|loadfile|dofile)\s*\(/', $src, $mm)) {
        $out[] = ['level' => 'high', 'api' => $mm[1],
                  'why' => '调用了标准库 file/os 接口，可能绕过应用桥的权限边界'];
    }
    return $out;
}

function risk_level_summary(array $risks): string {
    $hi = 0; $md = 0;
    foreach ($risks as $r) {
        if ($r['level'] === 'high') $hi++;
        elseif ($r['level'] === 'med') $md++;
    }
    if ($hi) return "高 {$hi} · 中 {$md}";
    if ($md) return "中 {$md}";
    return $risks ? '低' : '无';
}

/* ---------------------------------------------------------------- 商店读写 */

/** 读 index.json（不存在/坏掉都返回空骨架，不让整个页面崩） */
function store_index(): array {
    $p = rtrim((string)cfg('store_dir'), '/') . '/index.json';
    if (!is_file($p)) return ['ver' => 1, 'pkgs' => []];
    $j = json_decode((string)file_get_contents($p), true);
    if (!is_array($j)) return ['ver' => 1, 'pkgs' => []];
    if (!isset($j['pkgs']) || !is_array($j['pkgs'])) $j['pkgs'] = [];
    return $j;
}

/** 商店里已有的同名包（按 name 精确匹配），没有返回 null */
function store_find(string $name): ?array {
    foreach (store_index()['pkgs'] as $p)
        if (($p['name'] ?? '') === $name) return $p;
    return null;
}

/**
 * 把待审包正式发布进商店：复制到 pkgs/、更新 index.json 对应条目、整体 ver+1。
 * 返回 [bool ok, string 说明]
 */
function store_publish(array $sub, string $srcfile): array {
    $root = rtrim((string)cfg('store_dir'), '/');
    $pkgs = $root . '/pkgs';
    if (!is_dir($pkgs) && !@mkdir($pkgs, 0755, true)) return [false, 'pkgs/ 目录建不出来'];

    $fname = (string)$sub['fname'];
    $dst   = $pkgs . '/' . $fname;

    /* 覆盖前留一份备份，上架错了还能手工回退 */
    if (is_file($dst)) {
        $bak = $root . '/pkgs_backup';
        if (!is_dir($bak)) @mkdir($bak, 0755, true);
        @copy($dst, $bak . '/' . $fname . '.' . date('YmdHis'));
    }
    if (!@copy($srcfile, $dst)) return [false, '复制包到 pkgs/ 失败'];
    @chmod($dst, 0644);

    $data = (string)file_get_contents($dst);
    $size = strlen($data);
    $crc  = sprintf('%08x', crc32($data));

    $j    = store_index();
    $list = $j['pkgs'];
    $hit  = false;
    foreach ($list as $i => $p) {
        if (($p['name'] ?? '') !== (string)$sub['name']) continue;
        $list[$i]['label'] = (string)$sub['label'];
        $list[$i]['cat']   = (string)$sub['cat'];
        $list[$i]['desc']  = (string)$sub['descr'];
        $list[$i]['ver']   = (int)$sub['ver'];
        $list[$i]['size']  = $size;
        $list[$i]['crc32'] = $crc;
        $list[$i]['path']  = 'pkgs/' . $fname;
        $hit = true;
        break;
    }
    if (!$hit) {
        $list[] = [
            'name'  => (string)$sub['name'],
            'label' => (string)$sub['label'],
            'cat'   => (string)$sub['cat'],
            'desc'  => (string)$sub['descr'],
            'ver'   => (int)$sub['ver'],
            'size'  => $size,
            'crc32' => $crc,
            'path'  => 'pkgs/' . $fname,
        ];
    }
    $j['pkgs']    = $list;
    $j['ver']     = (int)($j['ver'] ?? 1) + 1;   /* 索引自身代次 +1，客户端据此刷新 */
    $j['updated'] = date('Y-m-d H:i:s');

    $json = json_encode($j, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT);
    if ($json === false) return [false, 'index.json 编码失败'];
    $tmp = $root . '/index.json.tmp';
    if (@file_put_contents($tmp, $json) === false) return [false, 'index.json 写不进去（权限？）'];
    if (!@rename($tmp, $root . '/index.json')) { @unlink($tmp); return [false, 'index.json 替换失败']; }

    return [true, $hit ? '已更新（v' . (int)$sub['ver'] . '）' : '已新增（v' . (int)$sub['ver'] . '）'];
}

/** 应用名 → 安全的文件名（保留中文，去掉路径分隔符与 FAT 非法字符） */
function safe_fname(string $appName): string {
    $n = trim($appName);
    $n = preg_replace('/\.lua$/i', '', $n);          /* 已带后缀就别再加一遍 */
    $n = preg_replace('/[\\\\\/:*?"<>|\x00-\x1f]/u', '', $n);
    $n = preg_replace('/\s+/u', ' ', $n);
    if ($n === '' || $n === null) $n = 'app';
    if (mb_strlen($n, 'UTF-8') > 40) $n = mb_substr($n, 0, 40, 'UTF-8');
    return $n . '.lua';
}

/* ---------------------------------------------------------------- 邮件 (SMTP) */

/**
 * 发一封 HTML 邮件。返回 [bool ok, string 说明]
 * 阿里云封 25 端口 ⇒ 必须走 465(SSL) / 587(STARTTLS) 的邮箱授权码。
 */
function smtp_send(string $to, string $subject, string $html): array {
    $c = (array)cfg('smtp');
    if (empty($c['user']) || empty($c['pass'])) {
        return [false, '未配置 SMTP 发件账号（config.php 的 smtp.user / smtp.pass）'];
    }
    $from    = $c['from'] ?: $c['user'];
    $fromName = $c['name'] ?: 'LinTOS';
    $port    = (int)$c['port'];
    $scheme  = ($port === 465) ? 'ssl' : 'tcp';

    $errno = 0; $errstr = '';
    $fp = @stream_socket_client("{$scheme}://{$c['host']}:{$port}", $errno, $errstr, 15);
    if (!$fp) return [false, "连接 {$c['host']}:{$port} 失败：{$errstr}"];
    stream_set_timeout($fp, 20);

    $read = static function () use ($fp): string {
        $out = '';
        while (($line = fgets($fp, 2048)) !== false) {
            $out .= $line;
            if (strlen($line) >= 4 && $line[3] === ' ') break;
        }
        return $out;
    };
    $say = static function (string $s) use ($fp, $read): string {
        fwrite($fp, $s . "\r\n");
        return $read();
    };
    $ok = static fn(string $r): bool => (bool)preg_match('/^[23]\d\d/', $r);

    $r = $read();                                  if (!$ok($r)) { fclose($fp); return [false, "握手失败: " . trim($r)]; }
    $r = $say('EHLO lintos');                      if (!$ok($r)) { fclose($fp); return [false, "EHLO 失败: " . trim($r)]; }

    if ($port !== 465) {                           /* 587: 升级 TLS */
        $r = $say('STARTTLS');
        if (!$ok($r)) { fclose($fp); return [false, "STARTTLS 被拒: " . trim($r)]; }
        if (!@stream_socket_enable_crypto($fp, true, STREAM_CRYPTO_METHOD_TLS_CLIENT)) {
            fclose($fp); return [false, 'TLS 协商失败'];
        }
        $r = $say('EHLO lintos');                  if (!$ok($r)) { fclose($fp); return [false, "EHLO(TLS) 失败: " . trim($r)]; }
    }

    $r = $say('AUTH LOGIN');
    if (!$ok($r)) { fclose($fp); return [false, "服务器不支持 AUTH LOGIN: " . trim($r)]; }
    $r = $say(base64_encode($c['user']));
    if (!$ok($r)) { fclose($fp); return [false, "账号被拒: " . trim($r)]; }
    $r = $say(base64_encode($c['pass']));
    if (!$ok($r)) { fclose($fp); return [false, "授权码被拒: " . trim($r)]; }

    $r = $say("MAIL FROM:<{$from}>");              if (!$ok($r)) { fclose($fp); return [false, "MAIL FROM 失败: " . trim($r)]; }
    $r = $say("RCPT TO:<{$to}>");                  if (!$ok($r)) { fclose($fp); return [false, "收件人被拒: " . trim($r)]; }
    $r = $say('DATA');                             if (!$ok($r)) { fclose($fp); return [false, "DATA 被拒: " . trim($r)]; }

    $body = rtrim(chunk_split(base64_encode($html), 76, "\r\n"));
    $headers = [
        'From: =?UTF-8?B?' . base64_encode($fromName) . "?= <{$from}>",
        "To: <{$to}>",
        'Subject: =?UTF-8?B?' . base64_encode($subject) . '?=',
        'MIME-Version: 1.0',
        'Content-Type: text/html; charset=UTF-8',
        'Content-Transfer-Encoding: base64',
        'Date: ' . date('r'),
        'Message-ID: <' . bin2hex(random_bytes(8)) . '@' . ($c['host']) . '>',
    ];
    fwrite($fp, implode("\r\n", $headers) . "\r\n\r\n" . $body . "\r\n.\r\n");
    $r = $read();
    fwrite($fp, "QUIT\r\n");
    fclose($fp);
    if (!$ok($r)) return [false, "投递失败: " . trim($r)];
    return [true, '已发送'];
}

/* ---------------------------------------------------------------- 其它 */

function rand_code(int $len = 6): string {
    $s = '';
    for ($i = 0; $i < $len; $i++) $s .= (string)random_int(0, 9);
    return $s;
}

function client_ip(): string {
    return (string)($_SERVER['REMOTE_ADDR'] ?? '');
}

/** 每账号 24 小时内提交数是否还允许 */
function upload_quota_left(int $uid): int {
    $st = db()->prepare('SELECT COUNT(*) c FROM subs WHERE uid = ? AND created > ?');
    $st->execute([$uid, time() - 86400]);
    $used = (int)($st->fetch()['c'] ?? 0);
    return max(0, (int)cfg('limits')['per_day_upload'] - $used);
}

/** 同邮箱发码间隔限制（用 expires-ttl 反推发送时刻，表里不另存时间） */
function code_can_send(string $email): array {
    $st = db()->prepare('SELECT expires FROM codes WHERE email = ? AND used = 0 ORDER BY id DESC LIMIT 1');
    $st->execute([$email]);
    $row = $st->fetch();
    if (!$row) return [true, 0];
    $sent = (int)$row['expires'] - (int)cfg('limits')['code_ttl'];
    $wait = (int)cfg('limits')['code_interval'] - (time() - $sent);
    return $wait > 0 ? [false, $wait] : [true, 0];
}
