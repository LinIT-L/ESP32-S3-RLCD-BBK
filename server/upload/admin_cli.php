<?php
/**
 * admin_cli.php — 管理员账户初始化 / 重置（**只能在服务器命令行跑**）
 *
 *   php admin_cli.php init [用户名]     首个管理员；随机口令只打印一次
 *   php admin_cli.php passwd <用户名>   重置口令，并强制下次登录改密
 *   php admin_cli.php list              列出管理员（不含任何口令信息）
 *
 * 规则:
 *   - 不提供网页注册入口，也不接受 HTTP 调用（非 CLI 直接 403）。
 *   - 口令用 password_hash() 存，不落明文；随机口令只显示一次，丢了就 passwd 重置。
 *   - init 重复执行**不会**重置已有账户：已有管理员时直接拒绝并提示用 passwd。
 */
declare(strict_types=1);

if (PHP_SAPI !== 'cli') {
    http_response_code(403);
    header('Content-Type: text/plain; charset=utf-8');
    echo "只能在服务器命令行执行。\n";
    exit(1);
}

require __DIR__ . '/lib.php';

/** 人类可读但强度足够的随机口令: 24 字符, 去掉易混字符 */
function gen_password(int $len = 24): string
{
    $abc = 'abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789';
    $n = strlen($abc) - 1;
    $s = '';
    for ($i = 0; $i < $len; $i++) $s .= $abc[random_int(0, $n)];
    return $s;
}

$cmd = $argv[1] ?? 'help';

try {
    switch ($cmd) {
        case 'init': {
            $user = $argv[2] ?? 'admin';
            if (!preg_match('/^[A-Za-z0-9_.-]{3,32}$/', $user)) {
                fwrite(STDERR, "用户名只能用字母数字 _ . - ，长度 3~32\n");
                exit(2);
            }
            if (admin_count() > 0) {
                fwrite(STDERR, "已经有管理员账户了。新增/重置请用：php admin_cli.php passwd {$user}\n");
                fwrite(STDERR, "（init 不会重置已有账户，这是故意的）\n");
                exit(3);
            }
            $pass = gen_password();
            $hash = password_hash($pass, PASSWORD_DEFAULT);
            db()->prepare('INSERT INTO admins (user, pass, must_change, created) VALUES (?,?,1,?)')
                ->execute([$user, $hash, time()]);
            audit($user, 'admin.init', null, null, null, 'active', 'ok', 'CLI 初始化首个管理员');
            echo "==================================================\n";
            echo " 管理员已创建（首次登录必须改密码）\n";
            echo "   用户名: {$user}\n";
            echo "   初始口令: {$pass}\n";
            echo "==================================================\n";
            echo "这串口令只显示这一次。请立刻登录 " . rtrim((string)cfg('base_url'), '/') . "/admin_login.php\n";
            echo "登录后系统会要求你设置新密码（至少 10 位）。\n";
            break;
        }
        case 'passwd': {
            $user = $argv[2] ?? '';
            if ($user === '' || !admin_by_name($user)) {
                fwrite(STDERR, "没有这个管理员：{$user}\n");
                exit(2);
            }
            $pass = gen_password();
            db()->prepare('UPDATE admins SET pass = ?, must_change = 1, fails = 0, locked_until = 0 WHERE user = ?')
                ->execute([password_hash($pass, PASSWORD_DEFAULT), $user]);
            audit($user, 'admin.passwd', null, null, null, null, 'ok', 'CLI 重置口令');
            echo "已重置 {$user} 的口令（下次登录必须改）：\n   新口令: {$pass}\n";
            break;
        }
        case 'list': {
            $rows = db()->query('SELECT id, user, must_change, created, last_login, locked_until FROM admins ORDER BY id')->fetchAll();
            if (!$rows) { echo "（还没有管理员，先跑 php admin_cli.php init）\n"; break; }
            printf("%-4s %-20s %-8s %-20s %-20s %s\n", 'ID', '用户名', '待改密', '创建时间', '最后登录', '锁定到');
            foreach ($rows as $r) {
                printf("%-4d %-20s %-8s %-20s %-20s %s\n", (int)$r['id'], $r['user'],
                    (int)$r['must_change'] ? '是' : '否',
                    $r['created'] ? date('Y-m-d H:i:s', (int)$r['created']) : '-',
                    $r['last_login'] ? date('Y-m-d H:i:s', (int)$r['last_login']) : '-',
                    (int)$r['locked_until'] > time() ? date('H:i:s', (int)$r['locked_until']) : '-');
            }
            break;
        }
        default:
            echo "用法:\n";
            echo "  php admin_cli.php init [用户名]     创建首个管理员（随机口令只显示一次）\n";
            echo "  php admin_cli.php passwd <用户名>   重置口令\n";
            echo "  php admin_cli.php list              列出管理员\n";
    }
} catch (Throwable $e) {
    fwrite(STDERR, '执行失败: ' . $e->getMessage() . "\n");
    exit(1);
}
