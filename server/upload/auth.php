<?php
/** 邮箱验证码：登录 / 自动注册 / 退出 */
require __DIR__ . '/lib.php';

function back(string $email = ''): void {
    $q = $email !== '' ? '?email=' . urlencode($email) : '';
    header('Location: index.php' . $q);
    exit;
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') back();
if (!csrf_ok()) { flash('页面过期了，请重试', 'err'); back(); }

$do    = (string)($_POST['do'] ?? '');
$email = strtolower(trim((string)($_POST['email'] ?? '')));

if ($do === 'logout') { logout_user(); flash('已退出'); back(); }

if (!filter_var($email, FILTER_VALIDATE_EMAIL)) { flash('邮箱格式不对', 'err'); back(); }

/* ---------------- 发验证码 ---------------- */
if ($do === 'send') {
    [$can, $wait] = code_can_send($email);
    if (!$can) { flash("发得太频繁了，{$wait} 秒后再试", 'err'); back($email); }

    $code = rand_code();
    $exp  = time() + (int)cfg('limits')['code_ttl'];
    db()->prepare('INSERT INTO codes (email, code, expires) VALUES (?, ?, ?)')
        ->execute([$email, $code, $exp]);

    $html = '<div style="font:16px/1.6 -apple-system,\'PingFang SC\',sans-serif;color:#1d1d1f">'
          . '<p>你在 <b>' . h(cfg('site_name')) . '</b> 请求了登录验证码：</p>'
          . '<p style="font-size:32px;letter-spacing:6px;font-weight:700;margin:24px 0">' . h($code) . '</p>'
          . '<p style="color:#6e6e73;font-size:14px">10 分钟内有效。不是你本人操作就忽略这封邮件。</p></div>';
    [$ok, $why] = smtp_send($email, '登录验证码 ' . $code, $html);
    if (!$ok) {
        /* 发不出去就把这条码作废，避免留下一个用户永远收不到的验证码 */
        db()->prepare('DELETE FROM codes WHERE email = ? AND code = ?')->execute([$email, $code]);
        flash('验证码发送失败：' . $why, 'err');
        back($email);
    }
    flash('验证码已发送，请查收邮箱（含垃圾箱）');
    back($email);
}

/* ---------------- 校验并登录 ---------------- */
if ($do === 'verify') {
    $code = preg_replace('/\D/', '', (string)($_POST['code'] ?? ''));
    if (strlen($code) !== 6) { flash('验证码是 6 位数字', 'err'); back($email); }

    $st = db()->prepare('SELECT * FROM codes WHERE email = ? AND code = ? AND used = 0
                         ORDER BY id DESC LIMIT 1');
    $st->execute([$email, $code]);
    $row = $st->fetch();
    if (!$row)              { flash('验证码不对，或者已经用过', 'err'); back($email); }
    if ((int)$row['expires'] < time()) { flash('验证码过期了，请重新发送', 'err'); back($email); }

    db()->prepare('UPDATE codes SET used = 1 WHERE id = ?')->execute([(int)$row['id']]);

    $st = db()->prepare('SELECT * FROM users WHERE email = ?');
    $st->execute([$email]);
    $u = $st->fetch();
    if ($u) {
        db()->prepare('UPDATE users SET verified = 1, last_login = ? WHERE id = ?')
            ->execute([time(), (int)$u['id']]);
        $uid = (int)$u['id'];
    } else {
        db()->prepare('INSERT INTO users (email, created, verified, last_login) VALUES (?, ?, 1, ?)')
            ->execute([$email, time(), time()]);
        $uid = (int)db()->lastInsertId();
    }
    login_user($uid);
    flash('登录成功');
    back();
}

flash('未知操作', 'err');
back();
