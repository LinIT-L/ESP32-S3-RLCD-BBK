<?php
/**
 * LinTOS 应用提交站 —— 公共库
 * 配置 / SQLite(含迁移) / 会话 / 管理员认证 / 审计日志 /
 * LAPP 解析校验 / 上传校验 / 商店原子发布-下架-回滚
 *
 * 设计约束（改动前请先读）:
 *   1. **商店协议冻结**: index.json 的字段集合、路径写法、ver 整数比较、
 *      crc32 + size 校验，一律不改。固件端 am_store_net.c 只认这些。
 *   2. **发布必须可恢复**: 任何失败都要能把 index.json 与 pkgs/ 恢复原状。
 *   3. **审计只增不改**: audit_log 只追加，不提供修改/删除接口。
 *   4. 不把密码/令牌/会话号写进日志。
 */
declare(strict_types=1);

require_once __DIR__ . '/lib_scan.php';

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
    foreach ([$dir, $dir . '/pkgs', $dir . '/backup'] as $d)
        if (!is_dir($d)) @mkdir($d, 0700, true);

    $pdo = new PDO('sqlite:' . $dir . '/app.db', null, null, [
        PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
    ]);
    /* WAL: 多人同时投稿 + 管理员审核并发写时避免 "database is locked"。
     * busy_timeout 再给 5s 重试窗口 —— 实测(见 tests/server) 20 并发写无失败。 */
    $pdo->exec('PRAGMA journal_mode=WAL');
    $pdo->exec('PRAGMA busy_timeout=5000');
    $pdo->exec('PRAGMA foreign_keys=ON');
    db_migrate($pdo);
    return $pdo;
}

/** 建表 + 只增不改的列迁移。可反复执行。 */
function db_migrate(PDO $pdo): void {
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

    /* ---- 新增: 管理员 ---- */
    $pdo->exec('CREATE TABLE IF NOT EXISTS admins (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        user TEXT UNIQUE NOT NULL,
        pass TEXT NOT NULL,
        must_change INTEGER NOT NULL DEFAULT 1,
        created INTEGER NOT NULL,
        last_login INTEGER,
        fails INTEGER NOT NULL DEFAULT 0,
        locked_until INTEGER NOT NULL DEFAULT 0
    )');
    /* ---- 新增: 审计日志(只增不改) ---- */
    $pdo->exec('CREATE TABLE IF NOT EXISTS audit_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts INTEGER NOT NULL,
        actor TEXT NOT NULL,
        action TEXT NOT NULL,
        app_id TEXT,
        ver INTEGER,
        from_state TEXT,
        to_state TEXT,
        result TEXT NOT NULL,
        note TEXT,
        ip TEXT
    )');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_log(ts)');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_audit_app ON audit_log(app_id)');
    /* ---- 新增: 版本历史 ---- */
    $pdo->exec('CREATE TABLE IF NOT EXISTS versions (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        app_id TEXT NOT NULL,
        ver INTEGER NOT NULL,
        fname TEXT NOT NULL,
        sha TEXT, size INTEGER, crc TEXT, build TEXT,
        src TEXT NOT NULL,
        note TEXT,
        active INTEGER NOT NULL DEFAULT 0,
        created INTEGER NOT NULL
    )');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_ver_app ON versions(app_id, ver)');
    /* ---- 新增: 登录限频(按 IP) ---- */
    $pdo->exec('CREATE TABLE IF NOT EXISTS login_fails (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ip TEXT NOT NULL, ts INTEGER NOT NULL
    )');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_lf_ip ON login_fails(ip, ts)');

    /* ---- subs 只增列 ---- */
    $cols = [];
    foreach ($pdo->query('PRAGMA table_info(subs)') as $r) $cols[$r['name']] = true;
    foreach ([
        'admin_note'   => 'TEXT',
        'scan'         => 'TEXT',           /* 新版词法扫描结果 JSON */
        'verdict'      => 'TEXT',           /* high/med/low/none */
        'updated'      => 'INTEGER',
        'published_at' => 'INTEGER',
        'resub_of'     => 'INTEGER',        /* 由哪条驳回记录改后重投 */
        'build'        => 'TEXT',           /* LAPP 头里的 build=YYYYMMDDHHMM */
    ] as $col => $type) {
        if (!isset($cols[$col])) $pdo->exec("ALTER TABLE subs ADD COLUMN {$col} {$type}");
    }
}

/* ---------------------------------------------------------------- 会话 */

function boot_session(): void {
    if (session_status() === PHP_SESSION_ACTIVE) return;
    session_set_cookie_params([
        /* path 默认 /upload/（生产就部署在这个子目录下）。
         * 测试环境把站点挂在根上时必须能改，否则 Set-Cookie 的 path 与实际路径不匹配。 */
        'lifetime' => 0, 'path' => (string)cfg('cookie_path', '/upload/'), 'httponly' => true,
        'samesite' => 'Lax', 'secure' => !empty($_SERVER['HTTPS']),
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
    $t = (string)($_POST['_csrf'] ?? '');
    return $t !== '' && hash_equals((string)($_SESSION['csrf'] ?? ''), $t);
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

/* ---------------------------------------------------------------- 管理员认证 */

/** 管理员空闲超时(秒): 超过就要求重新登录 */
const ADMIN_IDLE_TTL = 1800;

function admin_count(): int {
    try { return (int)db()->query('SELECT COUNT(*) c FROM admins')->fetch()['c']; }
    catch (Throwable $e) { return 0; }
}

function admin_by_name(string $user): ?array {
    $st = db()->prepare('SELECT * FROM admins WHERE user = ?');
    $st->execute([$user]);
    return $st->fetch() ?: null;
}

/** 当前登录的管理员; 未登录/超时返回 null */
function admin_current(): ?array {
    boot_session();
    $aid = (int)($_SESSION['aid'] ?? 0);
    if (!$aid) return null;
    $ttl = (int)cfg('admin_idle_ttl', ADMIN_IDLE_TTL);
    if ($ttl <= 0) $ttl = ADMIN_IDLE_TTL;
    if ((int)($_SESSION['aid_seen'] ?? 0) + $ttl < time()) {
        unset($_SESSION['aid'], $_SESSION['aid_seen']);
        return null;                     /* 空闲超时: 静默失效, 不泄露原因给页面 */
    }
    $_SESSION['aid_seen'] = time();
    static $a = null;
    if ($a && (int)$a['id'] === $aid) return $a;
    $st = db()->prepare('SELECT * FROM admins WHERE id = ?');
    $st->execute([$aid]);
    $a = $st->fetch() ?: null;
    return $a;
}

/** 管理员登录。返回 [bool ok, string why] */
function admin_login(string $user, string $pass, string $ip): array {
    $a = admin_by_name($user);
    if (!$a) {
        /* 不区分"用户不存在"和"密码错", 也不提示哪个对 */
        usleep(200000);
        return [false, '用户名或密码不对'];
    }
    if ((int)$a['locked_until'] > time()) {
        return [false, '尝试次数过多，请 ' . max(1, intdiv((int)$a['locked_until'] - time(), 60) + 1) . ' 分钟后再试'];
    }
    if (!password_verify($pass, (string)$a['pass'])) {
        $fails = (int)$a['fails'] + 1;
        $lock  = $fails >= 5 ? time() + 900 : 0;      /* 连续 5 次错 → 锁 15 分钟 */
        db()->prepare('UPDATE admins SET fails = ?, locked_until = ? WHERE id = ?')
            ->execute([$lock ? 0 : $fails, $lock, (int)$a['id']]);
        usleep(200000);
        return [false, '用户名或密码不对'];
    }
    db()->prepare('UPDATE admins SET fails = 0, locked_until = 0, last_login = ? WHERE id = ?')
        ->execute([time(), (int)$a['id']]);
    boot_session();
    session_regenerate_id(true);
    $_SESSION['aid'] = (int)$a['id'];
    $_SESSION['aid_seen'] = time();
    return [true, (int)$a['must_change'] ? 'must_change' : 'ok'];
}

function admin_logout(): void {
    boot_session();
    unset($_SESSION['aid'], $_SESSION['aid_seen']);
    session_regenerate_id(true);
}

/** 要求已登录且(可选)已改过初始密码; 不满足直接 403 并终止 */
function admin_require(bool $allow_must_change = false): array {
    $a = admin_current();
    if (!$a) {
        http_response_code(403);
        header('Content-Type: text/html; charset=utf-8');
        echo '<!doctype html><meta charset="utf-8"><div style="font:16px -apple-system,sans-serif;padding:40px">'
           . '需要管理员登录。<a href="admin_login.php">去登录</a></div>';
        exit;
    }
    if (!$allow_must_change && (int)$a['must_change'] === 1) {
        header('Location: admin_login.php?change=1');
        exit;
    }
    return $a;
}

/* ---------------------------------------------------------------- 审计 */

function audit(string $actor, string $action, ?string $appId, ?int $ver,
               ?string $from, ?string $to, string $result, string $note = ''): void {
    try {
        db()->prepare('INSERT INTO audit_log (ts, actor, action, app_id, ver, from_state, to_state, result, note, ip)
                       VALUES (?,?,?,?,?,?,?,?,?,?)')
            ->execute([time(), $actor, $action, $appId, $ver, $from, $to, $result,
                       mb_substr($note, 0, 500), client_ip()]);
    } catch (Throwable $e) {
        /* 审计失败不能把主流程带崩, 但要在 error_log 留痕 */
        error_log('audit failed: ' . $e->getMessage());
    }
}

function audit_recent(int $limit = 200): array {
    $st = db()->prepare('SELECT * FROM audit_log ORDER BY id DESC LIMIT ?');
    $st->execute([$limit]);
    return $st->fetchAll();
}

/* ---------------------------------------------------------------- LAPP 解析 */

/**
 * 从 `--@app` 行体里拆出所有 key=value。**两种写法都要支持**：
 *   ① 一行多个：  --@app name=QQ堂冒险 cat=游戏 desc=泡泡砖块 ver=1 icon=<b64>
 *   ② 一行一个：  --@app name=矢量地图 / --@app cat=工具 ...
 * 不能按空格切 —— desc 里可能有空格（"背包 技能"），按空格切会截断。
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

/** 解析 `--@app key=value` 头。规则与固件 components/services/apppack/apppack.c 一致。 */
function lapp_parse(string $src): array {
    $m = [
        'has_head' => false, 'name' => '', 'cat' => '', 'desc' => '',
        'ver' => '', 'build' => '', 'icon' => '', 'icon_bytes' => 0, 'need' => [], 'fields' => [],
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
                    case 'build':   $m['build'] = $v; break;   /* YYYYMMDDHHMM, 打包器写入 */
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

/** 分类归一化 —— 与 tools/store_build.py 的 CAT_NORMALIZE 一致 */
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

/** 版本号归一化: `ver=1.0` / `ver=v2` 都取第一段数字 → 2（0 = 解析不出） */
function norm_ver(string $v): int {
    if (preg_match('/(\d{1,3})/', $v, $m)) return max(1, (int)$m[1]);
    return 0;
}

/** 校验元信息 + 代码骨架。返回 [errors[], warnings[]] */
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
    } elseif (preg_match('#[\\\\/:*?"<>|]#u', (string)$m['name'])) {
        $err[] = 'name 里不能有 \\ / : * ? " < > | 这些字符（会变成商店文件名）';
    }

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
    if (!mb_check_encoding($src, 'UTF-8')) {
        $err[] = '文件不是 UTF-8 编码（中文会显示成乱码），请另存为 UTF-8';
    }
    return [$err, $warn];
}

/**
 * 上传内容硬校验（服务端，不信任客户端任何声明）。
 * 返回 [ok, errors[]]
 */
function validate_upload(string $src): array {
    $e = [];
    $lim = (array)cfg('limits');
    if ($src === '') $e[] = '文件是空的';
    if (strlen($src) > (int)$lim['max_bytes']) $e[] = '超过 ' . (int)($lim['max_bytes'] / 1024) . ' KB 上限';
    if (strpos($src, "\0") !== false) $e[] = '文件里有 NUL 字节（不是纯文本 Lua 源码）';
    if (!mb_check_encoding($src, 'UTF-8')) $e[] = '不是合法 UTF-8（中文会乱码）';
    /* 控制字符兜底: 允许 \t \n \r，其它 C0 控制符视为二进制 */
    if (preg_match('/[\x00-\x08\x0B\x0C\x0E-\x1F]/', $src)) $e[] = '包含二进制控制字符，只接受纯文本 .lua';
    return [$e === [], $e];
}

/* ---------------------------------------------------------------- 商店读写 */

function store_dir(): string { return rtrim((string)cfg('store_dir'), '/'); }
function store_index_file(): string { return store_dir() . '/index.json'; }

/**
 * 写路径专用: 索引存在但解析不出来 ⇒ **拒绝写**。
 * 为什么不能沿用宽松版: store_index() 对坏文件返回空骨架, 发布时就会拿这个空骨架
 * 覆盖掉整个 index.json —— 27 个应用一次性消失, 这是最严重的"发布副作用"。
 * 只读展示用宽松版无所谓, 写之前必须先过这一关。
 */
function store_index_strict(): array {
    $p = store_index_file();
    /* 索引不存在 ⇒ 拒绝。固件解析器要求顶层有 mirrors[]，凭空创建一个空骨架
     * 会让**整店**在新固件上解析失败（-1 没解析出 mirrors）。宁可报错让人放好索引。 */
    if (!is_file($p)) return [false, null, 'index.json 不存在（store_dir 配错了？）'];
    $raw = (string)@file_get_contents($p);
    if ($raw === '') return [false, null, 'index.json 是空的'];
    $j = json_decode($raw, true);
    if (!is_array($j)) return [false, null, 'index.json 不是合法 JSON: ' . json_last_error_msg()];
    if (!isset($j['pkgs']) || !is_array($j['pkgs'])) return [false, null, 'index.json 缺少 pkgs 数组'];
    return [true, $j, ''];
}

/** 读 index.json（不存在/坏掉都返回空骨架，不让页面崩） */
function store_index(): array {
    $p = store_index_file();
    if (!is_file($p)) return ['ver' => 1, 'pkgs' => []];
    $j = json_decode((string)file_get_contents($p), true);
    if (!is_array($j)) return ['ver' => 1, 'pkgs' => []];
    if (!isset($j['pkgs']) || !is_array($j['pkgs'])) $j['pkgs'] = [];
    return $j;
}

function store_find(string $name): ?array {
    foreach (store_index()['pkgs'] as $p)
        if (($p['name'] ?? '') === $name) return $p;
    return null;
}

/** 发布互斥锁: 同一时刻只允许一个发布事务 */
function store_lock() {
    $f = store_dir() . '/.publish.lock';
    $fp = @fopen($f, 'c');
    if (!$fp) return null;
    if (!flock($fp, LOCK_EX)) { fclose($fp); return null; }
    return $fp;
}

function store_unlock($fp): void {
    if ($fp) { flock($fp, LOCK_UN); fclose($fp); }
}

/** 原子写 index.json：写 tmp → 自检 JSON → rename。返回 [ok, why] */
function index_write_atomic(array $j): array {
    $json = json_encode($j, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT);
    if ($json === false) return [false, 'index.json 编码失败'];
    $tmp = store_dir() . '/index.json.tmp';
    if (@file_put_contents($tmp, $json) === false) return [false, 'index.json 写不进去（权限？）'];
    /* 自检: 落盘内容必须能解析回来, 且 pkgs 数量一致 —— 防"写了一半" */
    $back = json_decode((string)@file_get_contents($tmp), true);
    if (!is_array($back) || !isset($back['pkgs']) || count($back['pkgs']) !== count($j['pkgs'])) {
        @unlink($tmp);
        return [false, 'index.json 自检失败（写入内容不可解析）'];
    }
    if (!@rename($tmp, store_index_file())) { @unlink($tmp); return [false, 'index.json 替换失败']; }
    return [true, 'ok'];
}

/** 备份 index.json, 只保留最近 20 份 */
function index_backup(): ?string {
    $p = store_index_file();
    if (!is_file($p)) return null;
    $dir = rtrim((string)cfg('data_dir'), '/') . '/backup';
    if (!is_dir($dir)) @mkdir($dir, 0700, true);
    $dst = $dir . '/index-' . date('Ymd-His') . '-' . bin2hex(random_bytes(3)) . '.json';
    if (!@copy($p, $dst)) return null;
    $old = glob($dir . '/index-*.json') ?: [];
    sort($old);
    while (count($old) > 20) { $f = array_shift($old); @unlink($f); }
    return $dst;
}

/** 应用名 → 安全文件名（保留中文，去掉路径分隔符与 FAT 非法字符） */
function safe_fname(string $appName): string {
    $n = trim($appName);
    $n = preg_replace('/\.lua$/i', '', $n);
    $n = preg_replace('/[\\\\\/:*?"<>|\x00-\x1f]/u', '', $n);
    $n = preg_replace('/\s+/u', ' ', $n);
    if ($n === '' || $n === null) $n = 'app';
    if (mb_strlen($n, 'UTF-8') > 40) $n = mb_substr($n, 0, 40, 'UTF-8');
    return $n . '.lua';
}

/** 商店里的 name（= 文件名主干）；不允许为空或含分隔符 */
function store_app_id(string $name): string {
    $n = preg_replace('/\.lua$/i', '', safe_fname($name));
    return $n === '' ? '' : $n;
}

/* ---------------------------------------------------------------- 版本历史 */

function versions_of(string $appId): array {
    $st = db()->prepare('SELECT * FROM versions WHERE app_id = ? ORDER BY ver DESC');
    $st->execute([$appId]);
    return $st->fetchAll();
}

function version_active(string $appId): ?array {
    $st = db()->prepare('SELECT * FROM versions WHERE app_id = ? AND active = 1 ORDER BY ver DESC LIMIT 1');
    $st->execute([$appId]);
    return $st->fetch() ?: null;
}

/* ---------------------------------------------------------------- 发布事务 */

/**
 * 把待审包正式发布进商店。
 * 十项前置校验 → 锁 → 备份 → 原子写包 → 原子写索引 → 自检 → 失败全量回滚。
 *
 * $note 会写进 versions 与 audit（回滚时用来说明来源）。
 * 返回 [bool ok, string why, array info]
 */
function store_publish(array $sub, string $srcfile, string $note = ''): array {
    $appId = (string)$sub['name'];
    $ver   = (int)$sub['ver'];
    $fname = (string)$sub['fname'];

    /* ---- 前置校验（先做, 不碰任何线上文件） ---- */
    if ($appId === '' || $fname === '' || $ver <= 0) return [false, '应用 ID / 文件名 / 版本号不合法', []];
    if (preg_match('#[/\\\\]#', $fname)) return [false, '文件名含路径分隔符', []];
    if (!is_file($srcfile)) return [false, '包文件不存在：' . $srcfile, []];

    $data = (string)file_get_contents($srcfile);
    if ($data === '') return [false, '包文件是空的', []];
    $size = strlen($data);
    if ($size > (int)cfg('limits')['max_bytes']) return [false, '包文件超过大小上限', []];

    $sha = hash('sha256', $data);
    if (!empty($sub['sha']) && !hash_equals((string)$sub['sha'], $sha))
        return [false, '包的 sha256 与投稿记录不符（文件被改过？）', []];
    $crc = sprintf('%08x', crc32($data));
    if (!empty($sub['crc']) && (string)$sub['crc'] !== $crc)
        return [false, '包的 crc32 与投稿记录不符', []];

    $meta = lapp_parse($data);
    if (!$meta['has_head']) return [false, '包内没有 LAPP 头，固件会装不上', []];
    if ($meta['name'] !== '' && store_app_id($meta['name']) !== $appId)
        return [false, '包内 name（' . $meta['name'] . '）与审核记录的应用 ID（' . $appId . '）不一致', []];
    if ($meta['icon'] !== '' && $meta['icon_bytes'] !== 512)
        return [false, '图标不是 512 字节（64×64 1bpp）', []];

    $fp = store_lock();
    if (!$fp) return [false, '拿不到发布锁（另一个发布正在进行？）', []];

    [$idxOk, $idxBefore, $idxWhy] = store_index_strict();
    if (!$idxOk) { store_unlock($fp); return [false, '商店索引损坏，拒绝发布（先从 data/backup/ 恢复）：' . $idxWhy, []]; }
    $countBefore = count($idxBefore['pkgs']);
    $oldEntry = null;
    foreach ($idxBefore['pkgs'] as $p) if (($p['name'] ?? '') === $appId) { $oldEntry = $p; break; }

    /* build 单调性: LAPP 头里的 build=YYYYMMDDHHMM 由打包器写入。
     * 固件更新只认 index 的 ver, 但 build 回退通常意味着"拿旧包改了版本号",
     * 属于要拦的误操作。没有旧 build(老包没写)时跳过, 不假报。 */
    $newBuild = (string)($meta['build'] ?? '');
    if ($newBuild !== '' && !preg_match('/^\d{12}$/', $newBuild)) {
        store_unlock($fp);
        return [false, '包内 build=' . $newBuild . ' 不是 YYYYMMDDHHMM 十二位数字', []];
    }
    if ($newBuild !== '') {
        /* 比的是**包内 build**（versions.build 存的就是包内值），不是索引里的发布时间。
         * 无历史包内 build（老包没写）时跳过，不假报。 */
        $prev = version_active($appId);
        $pb = (string)($prev['build'] ?? '');
        if ($pb !== '' && preg_match('/^\d{12}$/', $pb) && strcmp($newBuild, $pb) < 0) {
            store_unlock($fp);
            return [false, '包内 build（' . $newBuild . '）比已发布的（' . $pb . '）更旧，像是拿旧包改了版本号', []];
        }
    }

    if ($oldEntry && $ver <= (int)($oldEntry['ver'] ?? 0)) {
        store_unlock($fp);
        return [false, "商店里已经是 v" . (int)$oldEntry['ver'] . "，新版本号必须大于它（本次 v{$ver}）", []];
    }

    $root = store_dir();
    $pkgs = $root . '/pkgs';
    if (!is_dir($pkgs) && !@mkdir($pkgs, 0755, true)) { store_unlock($fp); return [false, 'pkgs/ 建不出来', []]; }
    $dst = $pkgs . '/' . $fname;

    /* ---- 备份（索引 + 旧包） ---- */
    $idxBackup = index_backup();
    $bakDir = $root . '/pkgs_backup';
    if (!is_dir($bakDir)) @mkdir($bakDir, 0755, true);
    $oldBak = null;
    if (is_file($dst)) {
        $oldBak = $bakDir . '/' . $fname . '.' . date('YmdHis') . '.' . bin2hex(random_bytes(2));
        if (!@copy($dst, $oldBak)) {
            store_unlock($fp);
            return [false, '旧包备份失败，已放弃发布（没有动任何线上文件）', []];
        }
    }

    /* ---- 原子写包: tmp → rename ---- */
    $ptmp = $pkgs . '/.' . $fname . '.tmp';
    if (@file_put_contents($ptmp, $data) === false) {
        store_unlock($fp);
        return [false, '写临时包失败', []];
    }
    @chmod($ptmp, 0644);
    if (!@rename($ptmp, $dst)) {
        @unlink($ptmp);
        store_unlock($fp);
        return [false, '包替换失败', []];
    }

    /* ---- 改索引并原子写 ---- */
    $j = $idxBefore;
    $list = $j['pkgs'];
    $hit = false;
    for ($i = 0; $i < count($list); $i++) {
        if (($list[$i]['name'] ?? '') !== $appId) continue;
        $list[$i]['label'] = (string)$sub['label'];
        $list[$i]['cat']   = (string)$sub['cat'];
        $list[$i]['desc']  = (string)$sub['descr'];
        $list[$i]['ver']   = $ver;
        $list[$i]['size']  = $size;
        $list[$i]['crc32'] = $crc;
        $list[$i]['path']  = 'pkgs/' . $fname;
        /* ★ build: 固件 2026-10-30 起按"构建时间"判定更新（定长 12 位数字，字典序=时间序），
         *   build 为空才退回按 ver 比较。这里统一写**发布时间**：它天然单调递增，
         *   回滚（内容旧、版本新）也能让设备收到更新；
         *   而 LAPP 头里的 build 仍原样留在包内，只用于查"是不是拿旧包改了版本号"。 */
        $list[$i]['build'] = date('YmdHi');
        if (($list[$i]['date'] ?? '') === '') $list[$i]['date'] = date('Y-m-d');
        $hit = true;
        break;
    }
    if (!$hit) {
        /* 新条目：只写固件认识的字段（旧固件对未知字段是"忽略"），
         * 不塞任何"必需的新字段" —— 冻结协议。 */
        $list[] = [
            'name' => $appId, 'label' => (string)$sub['label'], 'cat' => (string)$sub['cat'],
            'desc' => (string)$sub['descr'], 'ver' => $ver,
            'size' => $size, 'crc32' => $crc, 'path' => 'pkgs/' . $fname,
            'build' => date('YmdHi'), 'date' => date('Y-m-d'),
        ];
    }
    $j['pkgs'] = $list;
    $j['ver'] = (int)($j['ver'] ?? 1) + 1;
    $j['updated'] = date('Y-m-d H:i:s');

    [$wok, $why] = index_write_atomic($j);
    if (!$wok) {
        /* 索引没写成 → 把包恢复原状 */
        if ($oldBak) { @copy($oldBak, $dst); @unlink($oldBak); }
        else         { @unlink($dst); }
        store_unlock($fp);
        return [false, '索引写入失败：' . $why . '（包已恢复）', []];
    }

    /* ---- 发布后自检: 索引能读回来、条目对、数量没少 ---- */
    $after = store_index();
    $okEntry = null;
    foreach ($after['pkgs'] as $p) if (($p['name'] ?? '') === $appId) { $okEntry = $p; break; }
    $bad = '';
    if (!$okEntry)                                     $bad = '索引里找不到该应用';
    elseif ((int)$okEntry['ver'] !== $ver)              $bad = '索引版本号不对';
    elseif ((int)$okEntry['size'] !== $size)            $bad = '索引大小不对';
    elseif ((string)$okEntry['crc32'] !== $crc)         $bad = '索引 crc32 不对';
    elseif (count($after['pkgs']) < $countBefore)       $bad = '应用数量减少了（索引被破坏）';
    elseif (!is_file($root . '/' . (string)$okEntry['path'])) $bad = '索引指向的文件不存在';

    if ($bad !== '') {
        /* 全量回滚: 索引 + 包 */
        if ($idxBackup && is_file($idxBackup)) @copy($idxBackup, store_index_file());
        if ($oldBak) { @copy($oldBak, $dst); @unlink($oldBak); }
        else         { @unlink($dst); }
        store_unlock($fp);
        return [false, '发布自检失败（' . $bad . '），已回滚到发布前状态', []];
    }

    /* ---- 记录版本历史 ---- */
    $prev = version_active($appId);
    if ($prev) {
        db()->prepare('UPDATE versions SET active = 0, src = ? WHERE id = ?')
            ->execute([$oldBak ?: (string)$prev['src'], (int)$prev['id']]);
    }
    db()->prepare('INSERT INTO versions (app_id, ver, fname, sha, size, crc, build, src, note, active, created)
                   VALUES (?,?,?,?,?,?,?,?,?,1,?)')
        ->execute([$appId, $ver, $fname, $sha, $size, $crc, $newBuild, $dst, $note, time()]);

    store_unlock($fp);
    return [true, ($hit ? '已更新到 v' . $ver : '已上架 v' . $ver), ['ver' => $ver, 'crc' => $crc, 'size' => $size]];
}

/**
 * 只改商店展示信息（label / cat / desc），**不改版本、不换包**。
 * 已上架的应用改完立刻生效（索引条目原地更新）；未上架的只改 subs 表。
 * 注意: 这只影响商店列表怎么显示；LAPP 头里的 name/cat/desc 仍是应用自己的声明,
 * 所以调用方要把两边都展示出来, 避免"网页说一套、装上去看到另一套"。
 */
function store_edit_display(string $appId, string $label, string $cat, string $desc): array {
    $fp = store_lock();
    if (!$fp) return [false, '拿不到发布锁'];
    $idxBackup = index_backup();
    [$iok, $j, $iwhy] = store_index_strict();
    if (!$iok) { store_unlock($fp); return [false, '商店索引损坏，拒绝修改：' . $iwhy]; }
    $hit = false;
    for ($i = 0; $i < count($j['pkgs']); $i++) {
        if (($j['pkgs'][$i]['name'] ?? '') !== $appId) continue;
        $j['pkgs'][$i]['label'] = $label;
        $j['pkgs'][$i]['cat']   = $cat;
        $j['pkgs'][$i]['desc']  = $desc;
        $hit = true;
        break;
    }
    if (!$hit) {                       /* 没上架: 没有需要改的索引 */
        store_unlock($fp);
        return [true, '已保存（该应用未上架，只更新了投稿记录）'];
    }
    $j['ver'] = (int)($j['ver'] ?? 1) + 1;
    $j['updated'] = date('Y-m-d H:i:s');
    [$ok, $why] = index_write_atomic($j);
    if (!$ok) {
        if ($idxBackup) @copy($idxBackup, store_index_file());
        store_unlock($fp);
        return [false, '写索引失败：' . $why];
    }
    /* 自检: 条目必须还在, 且字段已生效 */
    $chk = store_index();
    $found = null;
    foreach ($chk['pkgs'] as $p) if (($p['name'] ?? '') === $appId) { $found = $p; break; }
    if (!$found || $found['label'] !== $label) {
        if ($idxBackup) @copy($idxBackup, store_index_file());
        store_unlock($fp);
        return [false, '自检失败，已回滚'];
    }
    store_unlock($fp);
    return [true, '商店展示信息已更新'];
}

/** 下架: 从公开索引移除条目, **不动任何文件** */function store_unpublish(string $appId): array {
    $fp = store_lock();
    if (!$fp) return [false, '拿不到发布锁'];
    $idxBackup = index_backup();
    [$iok, $j, $iwhy] = store_index_strict();
    if (!$iok) { store_unlock($fp); return [false, '商店索引损坏，拒绝下架：' . $iwhy]; }
    $list = [];
    $hit = false;
    foreach ($j['pkgs'] as $p) {
        if (($p['name'] ?? '') === $appId) { $hit = true; continue; }
        $list[] = $p;
    }
    if (!$hit) { store_unlock($fp); return [false, '索引里没有这个应用']; }
    $j['pkgs'] = $list;
    $j['ver'] = (int)($j['ver'] ?? 1) + 1;
    $j['updated'] = date('Y-m-d H:i:s');
    [$ok, $why] = index_write_atomic($j);
    if (!$ok) {
        if ($idxBackup) @copy($idxBackup, store_index_file());
        store_unlock($fp);
        return [false, '下架写索引失败：' . $why];
    }
    /* 自检: 条目确实没了, 且没把别的应用一起删掉 */
    $chk = store_index();
    foreach ($chk['pkgs'] as $p) if (($p['name'] ?? '') === $appId) {
        if ($idxBackup) @copy($idxBackup, store_index_file());
        store_unlock($fp);
        return [false, '下架自检失败, 已回滚'];
    }
    store_unlock($fp);
    return [true, '已下架（文件与历史版本保留）'];
}

/**
 * 重新上架: 用**当前 active 版本**的文件重建索引条目。
 * 前置校验与发布一致, 失败不动线上。
 */
function store_relist(string $appId, array $sub): array {
    $v = version_active($appId);
    if (!$v) return [false, '没有可用的版本记录，请重新发布'];
    if (!is_file((string)$v['src'])) return [false, '版本文件不见了：' . $v['src']];
    $data = (string)file_get_contents((string)$v['src']);
    if ($data === '') return [false, '版本文件是空的'];
    if (sprintf('%08x', crc32($data)) !== (string)$v['crc']) return [false, '版本文件 crc32 与记录不符'];

    $fp = store_lock();
    if (!$fp) return [false, '拿不到发布锁'];
    $idxBackup = index_backup();
    [$iok, $j, $iwhy] = store_index_strict();
    if (!$iok) { store_unlock($fp); return [false, '商店索引损坏，拒绝上架：' . $iwhy]; }
    foreach ($j['pkgs'] as $p) if (($p['name'] ?? '') === $appId) {
        store_unlock($fp);
        return [false, '该应用已在商店索引里（无需重新上架）'];
    }
    $j['pkgs'][] = [
        'name' => $appId, 'label' => (string)$sub['label'], 'cat' => (string)$sub['cat'],
        'desc' => (string)$sub['descr'], 'ver' => (int)$v['ver'],
        'size' => (int)$v['size'], 'crc32' => (string)$v['crc'], 'path' => 'pkgs/' . $v['fname'],
    ];
    $j['ver'] = (int)($j['ver'] ?? 1) + 1;
    $j['updated'] = date('Y-m-d H:i:s');
    [$ok, $why] = index_write_atomic($j);
    if (!$ok) {
        if ($idxBackup) @copy($idxBackup, store_index_file());
        store_unlock($fp);
        return [false, '重新上架失败：' . $why];
    }
    store_unlock($fp);
    return [true, '已重新上架 v' . (int)$v['ver']];
}

/**
 * 回滚到历史版本。
 *
 * 为什么版本号是"新号"而不是退回旧号: 固件只在 index 的 ver > 已装 ver 时才提示更新，
 * 直接把 ver 退回去 = 已经装了坏版本的用户永远不会收到修复。所以回滚 =
 * 用旧内容 + 新版本号重新发布，并在 versions/audit 里记录"回滚自 vN"。
 */
function store_rollback(string $appId, int $toVer, array $sub, string $actor): array {
    $st = db()->prepare('SELECT * FROM versions WHERE app_id = ? AND ver = ? ORDER BY id DESC LIMIT 1');
    $st->execute([$appId, $toVer]);
    $v = $st->fetch();
    if (!$v) return [false, "没有 v{$toVer} 的历史记录"];
    if (!is_file((string)$v['src'])) return [false, "v{$toVer} 的文件不见了：" . $v['src']];

    $data = (string)file_get_contents((string)$v['src']);
    if ($data === '') return [false, "v{$toVer} 的文件是空的"];
    if (sprintf('%08x', crc32($data)) !== (string)$v['crc']) return [false, "v{$toVer} 的文件已损坏（crc 不符）"];

    $maxRow = db()->prepare('SELECT MAX(ver) m FROM versions WHERE app_id = ?');
    $maxRow->execute([$appId]);
    $newVer = (int)($maxRow->fetch()['m'] ?? 0) + 1;
    if ($newVer <= $toVer) $newVer = $toVer + 1;

    /* 先把旧内容落到待发布文件, 再走同一条发布路径(十项校验 + 锁 + 自检)。
     * ★ 暂存目录必须是 data/tmp/，**不能**用 data/pkgs/：
     *   data/pkgs/<sha>.lua 是"投稿包"的正式存储，下载/重审都靠它；
     *   回滚结束时会把暂存文件 unlink 掉，若共用同一个目录，
     *   就会把原始投稿包一起删掉（实测表现：审核页下载 404）。 */
    $tmpDir = rtrim((string)cfg('data_dir'), '/') . '/tmp';
    if (!is_dir($tmpDir)) @mkdir($tmpDir, 0700, true);
    $tmpf = $tmpDir . '/rollback-' . hash('sha256', $data) . '.lua';
    if (@file_put_contents($tmpf, $data) === false) return [false, '写临时文件失败'];
    @chmod($tmpf, 0600);

    /* 回滚要用的 subs 字段可能不全(从 index 走的调用没有 fname),
     * 缺的一律从 versions 记录/应用 ID 里补齐, 不能靠"调用方给全了"。 */
    $fake = $sub;
    $fake['name']   = $appId;
    $fake['ver']    = $newVer;
    $fake['sha']    = hash('sha256', $data);
    $fake['crc']    = sprintf('%08x', crc32($data));
    $fake['fname']  = (string)($v['fname'] ?? '') !== '' ? (string)$v['fname'] : safe_fname($appId);
    $fake['label']  = (string)($sub['label'] ?? '') !== '' ? (string)$sub['label'] : $appId;
    $fake['cat']    = (string)($sub['cat'] ?? '')   !== '' ? (string)$sub['cat']   : '软件';
    $fake['descr']  = (string)($sub['descr'] ?? '');

    [$ok, $why] = store_publish($fake, $tmpf, "回滚自 v{$toVer}（操作者 {$actor}）");
    @unlink($tmpf);
    if (!$ok) return [false, $why];
    return [true, "已回滚到 v{$toVer} 的内容，发布为 v{$newVer}（{$why}）"];
}

/* ---------------------------------------------------------------- 邮件 (SMTP) */

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

    if ($port !== 465) {
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

/** 同邮箱发码间隔限制 */
function code_can_send(string $email): array {
    $st = db()->prepare('SELECT expires FROM codes WHERE email = ? AND used = 0 ORDER BY id DESC LIMIT 1');
    $st->execute([$email]);
    $row = $st->fetch();
    if (!$row) return [true, 0];
    $sent = (int)$row['expires'] - (int)cfg('limits')['code_ttl'];
    $wait = (int)cfg('limits')['code_interval'] - (time() - $sent);
    return $wait > 0 ? [false, $wait] : [true, 0];
}

/** 投稿状态 → 中文 */
function status_label(string $s): string {
    static $m = [
        'pending' => '待审核', 'reviewing' => '审核中', 'approved' => '审核通过',
        'rejected' => '已驳回', 'published' => '已发布', 'unpublished' => '已下架',
    ];
    return $m[$s] ?? $s;
}

function status_tone(string $s): string {
    static $m = [
        'pending' => 'warn', 'reviewing' => 'warn', 'approved' => 'ok',
        'rejected' => 'err', 'published' => 'ok', 'unpublished' => 'err',
    ];
    return $m[$s] ?? 'warn';
}

/** 一个提交是否允许用户改后重投 */
function status_can_resubmit(string $s): bool {
    return in_array($s, ['rejected'], true);
}

/** 一个提交是否允许管理员执行"通过并发布" */
function status_can_approve(string $s): bool {
    return in_array($s, ['pending', 'reviewing', 'approved'], true);
}
