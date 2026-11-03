<?php
/**
 * admin_login.php — 管理员登录 / 首次改密 / 退出
 *
 * 安全点:
 *   - 只接受 POST 做认证动作，全部带 CSRF
 *   - 按 IP 限频（10 分钟内最多 10 次失败），账号连续 5 次错锁 15 分钟（在 admin_login() 里）
 *   - 登录成功 session_regenerate_id
 *   - 首次登录强制改密，未改密前 review.php 会把你踢回这里
 *   - 任何提示都不区分"用户名不存在"与"密码错"
 */
declare(strict_types=1);
require __DIR__ . '/lib.php';

$me    = admin_current();
$flash = take_flash();
$wantChange = isset($_GET['change']) || ($me && (int)$me['must_change'] === 1);
$err = '';
$ok  = '';

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    if (!csrf_ok()) {
        $err = '页面过期了，请重试';
    } else {
        $do = (string)($_POST['do'] ?? '');

        if ($do === 'logout') {
            admin_logout();
            header('Location: admin_login.php');
            exit;
        }

        if ($do === 'login') {
            /* --- IP 限频 --- */
            $ip = client_ip();
            $st = db()->prepare('SELECT COUNT(*) c FROM login_fails WHERE ip = ? AND ts > ?');
            $st->execute([$ip, time() - 600]);
            if ((int)($st->fetch()['c'] ?? 0) >= 10) {
                $err = '这个 IP 的失败次数太多了，请 10 分钟后再试';
            } else {
                [$good, $why] = admin_login((string)($_POST['user'] ?? ''), (string)($_POST['pass'] ?? ''), $ip);
                if ($good) {
                    db()->prepare('DELETE FROM login_fails WHERE ip = ?')->execute([$ip]);
                    audit((string)($_POST['user'] ?? ''), 'admin.login', null, null, null, null, 'ok', '');
                    if ($why === 'must_change') { header('Location: admin_login.php?change=1'); exit; }
                    header('Location: review.php');
                    exit;
                }
                db()->prepare('INSERT INTO login_fails (ip, ts) VALUES (?,?)')->execute([$ip, time()]);
                audit((string)($_POST['user'] ?? ''), 'admin.login', null, null, null, null, 'fail', '口令错误');
                $err = $why;
            }
        }

        if ($do === 'passwd') {
            if (!$me) { $err = '请先登录'; }
            else {
                $p1 = (string)($_POST['p1'] ?? '');
                $p2 = (string)($_POST['p2'] ?? '');
                if (mb_strlen($p1) < 10)                       $err = '新密码至少 10 位';
                elseif (preg_match('/^(.)\1*$/', $p1))          $err = '新密码不能是同一个字符重复';
                elseif ($p1 !== $p2)                            $err = '两次输入不一致';
                elseif (password_verify($p1, (string)$me['pass'])) $err = '新密码不能和旧密码一样';
                else {
                    db()->prepare('UPDATE admins SET pass = ?, must_change = 0 WHERE id = ?')
                        ->execute([password_hash($p1, PASSWORD_DEFAULT), (int)$me['id']]);
                    audit((string)$me['user'], 'admin.passwd', null, null, null, null, 'ok', '管理员自行改密');
                    $_SESSION['aid_seen'] = time();
                    header('Location: review.php');
                    exit;
                }
            }
        }
    }
}

$me = admin_current();
$mustChange = $me && (int)$me['must_change'] === 1;
?>
<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>管理登录 — <?= h(cfg('site_name')) ?></title>
<style>
  :root{--bg:#f5f5f7;--card:#fff;--ink:#1d1d1f;--sub:#6e6e73;--line:#e5e5ea;--accent:#0071e3;--ok:#1a7f37;--err:#c0392b}
  @media (prefers-color-scheme:dark){:root{--bg:#000;--card:#1c1c1e;--ink:#f5f5f7;--sub:#98989d;--line:#38383a;--accent:#0a84ff}}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.55 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif;
       padding:60px 20px;display:flex;justify-content:center}
  .card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:28px;width:100%;max-width:420px}
  h1{font-size:22px;margin:0 0 6px}
  p.lede{color:var(--sub);font-size:14px;margin:0 0 20px}
  label{display:block;font-size:13px;color:var(--sub);margin:14px 0 6px}
  input{width:100%;padding:11px 13px;font:inherit;color:var(--ink);background:var(--bg);
        border:1px solid var(--line);border-radius:10px;outline:none}
  input:focus{border-color:var(--accent);background:var(--card)}
  button{width:100%;margin-top:22px;font:inherit;font-size:15px;font-weight:600;border:0;border-radius:10px;
         padding:12px;cursor:pointer;background:var(--accent);color:#fff}
  .bar{border-radius:10px;padding:11px 14px;font-size:14px;margin-bottom:16px;border:1px solid}
  .bar.err{background:rgba(192,57,43,.08);border-color:rgba(192,57,43,.3);color:var(--err)}
  .bar.warn{background:rgba(178,80,0,.08);border-color:rgba(178,80,0,.3);color:#b25000}
  .hint{font-size:13px;color:var(--sub);margin-top:14px}
  a{color:var(--accent);text-decoration:none}
</style></head><body>
<div class="card">
<?php if ($mustChange): ?>
  <h1>设置新密码</h1>
  <p class="lede">初始口令是一次性的。请设一个只有你知道的新密码（至少 10 位）。</p>
  <?php if ($err): ?><div class="bar err"><?= h($err) ?></div><?php endif; ?>
  <form method="post">
    <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
    <input type="hidden" name="do" value="passwd">
    <label for="p1">新密码</label>
    <input id="p1" type="password" name="p1" minlength="10" required autocomplete="new-password">
    <label for="p2">再输一遍</label>
    <input id="p2" type="password" name="p2" minlength="10" required autocomplete="new-password">
    <button type="submit">保存并进入后台</button>
  </form>
  <p class="hint">当前账号：<?= h($me['user'] ?? '') ?></p>
<?php elseif ($me): ?>
  <h1>已登录</h1>
  <p class="lede">当前账号：<?= h($me['user']) ?></p>
  <p><a href="review.php">→ 进入审核后台</a></p>
  <form method="post" style="margin-top:18px">
    <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
    <input type="hidden" name="do" value="logout">
    <button type="submit" style="background:transparent;color:var(--accent);border:1px solid var(--line)">退出登录</button>
  </form>
<?php else: ?>
  <h1>管理登录</h1>
  <p class="lede">仅站主使用。没有账号？在服务器上执行 <code>php admin_cli.php init</code>。</p>
  <?php if ($err): ?><div class="bar err"><?= h($err) ?></div><?php endif; ?>
  <?php if ($flash): ?><div class="bar warn"><?= h($flash['m']) ?></div><?php endif; ?>
  <form method="post">
    <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
    <input type="hidden" name="do" value="login">
    <label for="user">用户名</label>
    <input id="user" type="text" name="user" required autocomplete="username" autofocus>
    <label for="pass">密码</label>
    <input id="pass" type="password" name="pass" required autocomplete="current-password">
    <button type="submit">登录</button>
  </form>
  <p class="hint">普通用户投稿请回 <a href="index.php">投稿页</a>。</p>
<?php endif; ?>
</div>
</body></html>
