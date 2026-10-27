<?php
/**
 * 审核面板：/upload/review.php?k=<review_key>
 *   ?k=..            列出所有待审
 *   ?k=..&id=12      看详情
 *   POST（同样带 k）  a=approve 上架 / a=reject 驳回（可带原因）
 * 只用 review_key 鉴权，不需要登录；POST 必须带 k，攻击者拿不到 k 就发不出请求。
 */
require __DIR__ . '/lib.php';

$key = (string)cfg('review_key');
$k   = (string)($_GET['k'] ?? $_POST['k'] ?? '');
if ($key === '' || $key === 'CHANGE_ME_REVIEW_KEY' || !hash_equals($key, $k)) {
    http_response_code(403);
    echo '<!doctype html><meta charset="utf-8"><p style="font:16px -apple-system,sans-serif;padding:40px">'
       . '链接无效。请在 config.php 里把 review_key 改成随机串，再用 /review.php?k=那串 打开。</p>';
    exit;
}

$msg = null;
$id  = (int)($_REQUEST['id'] ?? 0);

/* ---------------- POST: 通过 / 驳回 ---------------- */
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $a = (string)($_POST['a'] ?? '');
    $st = db()->prepare('SELECT * FROM subs WHERE id = ?');
    $st->execute([$id]);
    $sub = $st->fetch();
    if (!$sub) {
        $msg = ['err', '这条提交不存在'];
    } elseif ($sub['status'] !== 'pending') {
        $msg = ['warn', '这条已经处理过了（' . $sub['status'] . '）'];
    } elseif ($a === 'approve') {
        $file = rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sub['sha'] . '.lua';
        if (!is_file($file)) {
            $msg = ['err', '包文件不见了（data/pkgs/' . $sub['sha'] . '.lua），没法上架'];
        } else {
            [$ok, $why] = store_publish($sub, $file);
            if ($ok) {
                db()->prepare("UPDATE subs SET status='approved', reviewed=?, pub_ver=? WHERE id=?")
                    ->execute([time(), (int)$sub['ver'], $id]);
                $msg = ['ok', $why . ' 已写入商店索引'];
                notify_user((int)$sub['uid'], '你的应用已上架', $sub, true, '');
            } else {
                $msg = ['err', '上架失败：' . $why];
            }
        }
    } elseif ($a === 'reject') {
        $note = trim((string)($_POST['note'] ?? ''));
        db()->prepare("UPDATE subs SET status='rejected', reviewed=?, note=? WHERE id=?")
            ->execute([time(), $note, $id]);
        $msg = ['warn', '已驳回' . ($note !== '' ? '：' . $note : '')];
        notify_user((int)$sub['uid'], '你的应用被驳回', $sub, false, $note);
    }
}

/** 通知提交者（邮件发不出去也不影响审核结果） */
function notify_user(int $uid, string $title, array $sub, bool $pass, string $note): void {
    $st = db()->prepare('SELECT email FROM users WHERE id = ?');
    $st->execute([$uid]);
    $email = (string)($st->fetch()['email'] ?? '');
    if ($email === '') return;
    $html = '<div style="font:15px/1.6 -apple-system,\'PingFang SC\',sans-serif;color:#1d1d1f">'
          . '<p><b>' . h($sub['label'] ?: $sub['name']) . ' v' . (int)$sub['ver'] . '</b> — ' . h($title) . '</p>'
          . ($pass
              ? '<p style="color:#1a7f37">已经进商店了。设备上刷新商店即可看到（新装显示「下载」，已装旧版显示「更新」）。</p>'
              : '<p style="color:#c0392b">原因：' . h($note !== '' ? $note : '未说明') . '</p>'
                . '<p>改好之后可以重新提交，记得把 ver 加上去。</p>')
          . '<p style="color:#98989d;font-size:12px">— ' . h(cfg('site_name')) . '</p></div>';
    smtp_send($email, $title . ' — ' . ($sub['label'] ?: $sub['name']), $html);
}

/* ---------------- 取数据 ---------------- */
$one = null; $list = [];
if ($id > 0) {
    $st = db()->prepare('SELECT s.*, u.email FROM subs s LEFT JOIN users u ON u.id = s.uid WHERE s.id = ?');
    $st->execute([$id]);
    $one = $st->fetch() ?: null;
} else {
    $list = db()->query("SELECT s.*, u.email FROM subs s LEFT JOIN users u ON u.id = s.uid
                         WHERE s.status = 'pending' ORDER BY s.id ASC")->fetchAll();
}
$STATUS = ['pending' => '待审核', 'approved' => '已上架', 'rejected' => '已驳回'];
function lvl_color(string $l): string {
    return $l === 'high' ? '#c0392b' : ($l === 'med' ? '#b25000' : '#6e6e73');
}
?>
<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>审核 — <?= h(cfg('site_name')) ?></title>
<style>
  :root{--bg:#f5f5f7;--card:#fff;--ink:#1d1d1f;--sub:#6e6e73;--line:#e5e5ea;--accent:#0071e3;--ok:#1a7f37;--warn:#b25000;--err:#c0392b}
  @media (prefers-color-scheme:dark){:root{--bg:#000;--card:#1c1c1e;--ink:#f5f5f7;--sub:#98989d;--line:#38383a;--accent:#0a84ff}}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.55 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif;padding:36px 20px}
  .wrap{max-width:760px;margin:0 auto}
  h1{font-size:28px;margin:0 0 18px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:22px;margin-bottom:16px}
  .bar{border-radius:12px;padding:12px 16px;font-size:14px;margin-bottom:16px;border:1px solid}
  .bar.ok{background:rgba(26,127,55,.08);border-color:rgba(26,127,55,.3);color:var(--ok)}
  .bar.err{background:rgba(192,57,43,.08);border-color:rgba(192,57,43,.3);color:var(--err)}
  .bar.warn{background:rgba(178,80,0,.08);border-color:rgba(178,80,0,.3);color:var(--warn)}
  table.kv{border-collapse:collapse;font-size:14px;width:100%}
  table.kv td{padding:7px 0;vertical-align:top}
  table.kv td:first-child{color:var(--sub);width:80px}
  pre{background:var(--bg);border:1px solid var(--line);border-radius:10px;padding:14px;overflow:auto;
      font:12px/1.5 ui-monospace,Menlo,monospace;max-height:320px;color:var(--ink)}
  ul.risks{margin:8px 0 0;padding-left:20px;font-size:14px}
  ul.risks li{margin:4px 0}
  button{font:inherit;font-size:15px;font-weight:600;border:0;border-radius:10px;padding:12px 24px;cursor:pointer}
  .b-ok{background:var(--accent);color:#fff}
  .b-no{background:rgba(192,57,43,.12);color:var(--err)}
  .b-back{background:transparent;color:var(--accent);padding:8px 0}
  input[type=text]{width:100%;padding:11px 13px;font:inherit;color:var(--ink);background:var(--bg);
    border:1px solid var(--line);border-radius:10px;margin-top:8px}
  .row{display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-top:16px}
  .subs{list-style:none;margin:0;padding:0}
  .subs li{padding:14px 0;border-top:1px solid var(--line)}
  .subs li:first-child{border-top:0}
  .t{font-size:12px;padding:3px 9px;border-radius:20px;white-space:nowrap}
  .t-pending{background:rgba(178,80,0,.12);color:var(--warn)}
  .t-approved{background:rgba(26,127,55,.12);color:var(--ok)}
  .t-rejected{background:rgba(192,57,43,.12);color:var(--err)}
  .meta{color:var(--sub);font-size:12px}
  code{background:var(--bg);padding:2px 6px;border-radius:5px;font-size:13px}
  a{color:var(--accent);text-decoration:none}
</style></head><body><div class="wrap">
<h1><?= $one ? '审核单 #' . (int)$one['id'] : '待审核（' . count($list) . '）' ?></h1>

<?php if ($msg): ?><div class="bar <?= h($msg[0]) ?>"><?= h($msg[1]) ?></div><?php endif; ?>

<?php if (!$one && !$msg): ?>
  <?php if (!$list): ?>
    <div class="card"><p class="meta" style="margin:0">没有待审的提交。</p></div>
  <?php else: ?>
    <div class="card"><ul class="subs">
      <?php foreach ($list as $s):
        $rk = json_decode((string)$s['risks'], true) ?: [];
        $hi = 0; foreach ($rk as $r) if ($r['level'] === 'high') $hi++;
      ?>
        <li>
          <div style="display:flex;justify-content:space-between;gap:12px;align-items:flex-start">
            <div>
              <div><strong><?= h($s['label'] ?: $s['name']) ?></strong>
                <span class="meta">v<?= (int)$s['ver'] ?> · <?= h($s['cat']) ?> · <?= number_format((int)$s['size']) ?> B</span></div>
              <div class="meta"><?= h($s['descr']) ?></div>
              <div class="meta"><?= h($s['email']) ?> · <?= h(date('m-d H:i', (int)$s['created'])) ?>
                <?php if ($hi): ?><span style="color:var(--err);font-weight:600"> · 高危 <?= $hi ?> 项</span><?php endif; ?>
              </div>
            </div>
            <a href="?k=<?= h(urlencode($k)) ?>&id=<?= (int)$s['id'] ?>">审核</a>
          </div>
        </li>
      <?php endforeach; ?>
    </ul></div>
  <?php endif; ?>
<?php endif; ?>

<?php if ($one): $rk = json_decode((string)$one['risks'], true) ?: []; ?>
  <div class="card">
    <table class="kv">
      <tr><td>应用名</td><td><b><?= h($one['label'] ?: $one['name']) ?></b></td></tr>
      <tr><td>商店名</td><td><code><?= h($one['name']) ?></code> （文件名 <code><?= h($one['fname']) ?></code>）</td></tr>
      <tr><td>分类</td><td><?= h($one['cat']) ?></td></tr>
      <tr><td>说明</td><td><?= h($one['descr']) ?></td></tr>
      <tr><td>版本</td><td>v<?= (int)$one['ver'] ?></td></tr>
      <tr><td>大小</td><td><?= number_format((int)$one['size']) ?> 字节</td></tr>
      <tr><td>crc32</td><td><code><?= h($one['crc']) ?></code></td></tr>
      <tr><td>提交者</td><td><?= h($one['email']) ?> · <?= h($one['ip']) ?></td></tr>
      <tr><td>状态</td><td><span class="t t-<?= h($one['status']) ?>"><?= h($STATUS[$one['status']] ?? $one['status']) ?></span>
        <?php if ($one['note']): ?><span class="meta"> · <?= h($one['note']) ?></span><?php endif; ?></td></tr>
    </table>
  </div>

  <div class="card">
    <strong style="font-size:15px">代码能力扫描</strong>
    <?php if (!$rk): ?>
      <p class="meta" style="margin:8px 0 0;color:var(--ok)">没有命中危险 API。</p>
    <?php else: ?>
      <ul class="risks">
      <?php foreach ($rk as $r): ?>
        <li style="color:<?= lvl_color($r['level']) ?>">
          <?= $r['level'] === 'high' ? '⛔' : ($r['level'] === 'med' ? '⚠' : '·') ?>
          <code><?= h($r['api']) ?></code> — <?= h($r['why']) ?>
        </li>
      <?php endforeach; ?>
      </ul>
    <?php endif; ?>
  </div>

  <div class="card">
    <strong style="font-size:15px">代码预览（前 40 行）</strong>
    <?php $p = rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $one['sha'] . '.lua';
          $lines = is_file($p) ? array_slice(file($p, FILE_IGNORE_NEW_LINES), 0, 40) : []; ?>
    <pre><?= h(implode("\n", $lines) ?: '（包文件不存在）') ?></pre>
  </div>

  <?php if ($one['status'] === 'pending'): ?>
  <div class="card">
    <form method="post" style="margin:0">
      <input type="hidden" name="k" value="<?= h($k) ?>">
      <input type="hidden" name="id" value="<?= (int)$one['id'] ?>">
      <div class="row">
        <button class="b-ok" type="submit" name="a" value="approve">通过并上架</button>
        <button class="b-no" type="submit" name="a" value="reject">驳回</button>
      </div>
      <input type="text" name="note" placeholder="驳回原因（可选，会发给提交者）" maxlength="120">
    </form>
  </div>
  <?php endif; ?>
  <p><a class="b-back" href="?k=<?= h(urlencode($k)) ?>">← 返回待审列表</a></p>
<?php endif; ?>
</div></body></html>
