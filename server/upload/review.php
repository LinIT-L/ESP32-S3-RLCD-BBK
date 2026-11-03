<?php
/**
 * review.php — 管理员审核后台
 *
 * 鉴权: **登录会话 + 角色**（不是 URL 里的令牌）。所有写操作走 POST + CSRF。
 *   GET  ?st=<状态>          按状态筛选列表（默认待审核）
 *   GET  ?id=N               详情
 *   GET  ?log=1              操作日志
 *   POST a=claim|approve|approve_only|reject|edit|unpublish|relist|rollback
 *
 * ⚠ 旧的 review.php?k=<review_key> 授权方式已移除：令牌会进 nginx access log、
 *   浏览器历史和 Referer。现在没有 k 参数可用，持 key 也必须先登录。
 */
declare(strict_types=1);
require __DIR__ . '/lib.php';

$me = admin_require();
$actor = (string)$me['user'];
$msg = null;
$id  = (int)($_REQUEST['id'] ?? 0);

/* ============================ 写操作 ============================ */
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    if (!csrf_ok()) {
        $msg = ['err', '页面过期（CSRF 校验失败），请重新操作'];
    } else {
        $a  = (string)($_POST['a'] ?? '');
        $st = db()->prepare('SELECT * FROM subs WHERE id = ?');
        $st->execute([$id]);
        $sub = $st->fetch() ?: null;

        if (!$sub) {
            $msg = ['err', '这条投稿不存在'];
        } else {
            $appId = (string)$sub['name'];
            $from  = (string)$sub['status'];

            /* ---------- 认领（审核中） ---------- */
            if ($a === 'claim') {
                if ($from !== 'pending') {
                    $msg = ['warn', '只有「待审核」才能认领'];
                } else {
                    db()->prepare("UPDATE subs SET status='reviewing', updated=? WHERE id=?")->execute([time(), $id]);
                    audit($actor, 'sub.review', $appId, (int)$sub['ver'], $from, 'reviewing', 'ok');
                    $msg = ['ok', '已标记为「审核中」'];
                }
            }

            /* ---------- 通过并发布 ---------- */
            elseif ($a === 'approve' || $a === 'approve_only') {
                if (!status_can_approve($from)) {
                    $msg = ['warn', '当前状态不允许通过：' . status_label($from)];
                } else {
                    $file = rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sub['sha'] . '.lua';
                    if (!is_file($file)) {
                        $msg = ['err', '包文件不见了（data/pkgs/' . $sub['sha'] . '.lua）'];
                        audit($actor, 'sub.approve', $appId, (int)$sub['ver'], $from, $from, 'fail', '包文件缺失');
                    } elseif ($a === 'approve_only') {
                        db()->prepare("UPDATE subs SET status='approved', reviewed=?, updated=? WHERE id=?")
                            ->execute([time(), time(), $id]);
                        audit($actor, 'sub.approve_only', $appId, (int)$sub['ver'], $from, 'approved', 'ok', '审核通过，暂不发布');
                        $msg = ['ok', '已标记「审核通过」（未发布到商店）'];
                    } else {
                        [$ok, $why, $info] = store_publish($sub, $file);
                        if ($ok) {
                            db()->prepare("UPDATE subs SET status='published', reviewed=?, pub_ver=?, published_at=?, updated=? WHERE id=?")
                                ->execute([time(), (int)$info['ver'], time(), time(), $id]);
                            audit($actor, 'sub.publish', $appId, (int)$info['ver'], $from, 'published', 'ok', $why);
                            $msg = ['ok', $why . ' — 已写入商店索引（index.json ver+1）'];
                        } else {
                            audit($actor, 'sub.publish', $appId, (int)$sub['ver'], $from, $from, 'fail', $why);
                            $msg = ['err', '发布失败：' . $why];
                        }
                    }
                }
            }

            /* ---------- 驳回 ---------- */
            elseif ($a === 'reject') {
                $note = trim((string)($_POST['note'] ?? ''));
                if ($note === '') {
                    $msg = ['err', '驳回必须填写原因（会展示给投稿者）'];
                } elseif (!in_array($from, ['pending', 'reviewing', 'approved'], true)) {
                    $msg = ['warn', '当前状态不允许驳回：' . status_label($from)];
                } else {
                    db()->prepare("UPDATE subs SET status='rejected', reviewed=?, note=?, updated=? WHERE id=?")
                        ->execute([time(), mb_substr($note, 0, 300), time(), $id]);
                    audit($actor, 'sub.reject', $appId, (int)$sub['ver'], $from, 'rejected', 'ok', $note);
                    $msg = ['warn', '已驳回：' . $note];
                }
            }

            /* ---------- 修改展示信息 ---------- */
            elseif ($a === 'edit') {
                $label = trim((string)($_POST['label'] ?? ''));
                $cat   = (string)($_POST['cat'] ?? '');
                $desc  = trim((string)($_POST['descr'] ?? ''));
                if ($label === '')                              $msg = ['err', '展示名不能为空'];
                elseif (!in_array($cat, (array)cfg('cats'), true)) $msg = ['err', '分类不合法'];
                elseif (preg_match('#[\\\\/:*?"<>|]#u', $label)) $msg = ['err', '展示名不能包含 \\ / : * ? " < > |'];
                else {
                    db()->prepare('UPDATE subs SET label=?, cat=?, descr=?, updated=? WHERE id=?')
                        ->execute([mb_substr($label, 0, 24), $cat, mb_substr($desc, 0, 40), time(), $id]);
                    [$ok, $why] = store_edit_display($appId, mb_substr($label, 0, 24), $cat, mb_substr($desc, 0, 40));
                    audit($actor, 'sub.edit', $appId, (int)$sub['ver'], $from, $from, $ok ? 'ok' : 'fail',
                          "展示名={$label} 分类={$cat}" . ($ok ? '' : " / {$why}"));
                    $msg = [$ok ? 'ok' : 'err', $why];
                }
            }

            /* ---------- 下架 ---------- */
            elseif ($a === 'unpublish') {
                if ($from !== 'published') {
                    $msg = ['warn', '只有「已发布」才能下架'];
                } else {
                    [$ok, $why] = store_unpublish($appId);
                    if ($ok) {
                        db()->prepare("UPDATE subs SET status='unpublished', updated=? WHERE id=?")->execute([time(), $id]);
                        audit($actor, 'app.unpublish', $appId, (int)$sub['pub_ver'], $from, 'unpublished', 'ok', $why);
                        $msg = ['ok', $why];
                    } else {
                        audit($actor, 'app.unpublish', $appId, (int)$sub['pub_ver'], $from, $from, 'fail', $why);
                        $msg = ['err', '下架失败：' . $why];
                    }
                }
            }

            /* ---------- 重新上架 ---------- */
            elseif ($a === 'relist') {
                if (!in_array($from, ['unpublished', 'published', 'approved'], true)) {
                    $msg = ['warn', '当前状态不能重新上架：' . status_label($from)];
                } else {
                    [$ok, $why] = store_relist($appId, $sub);
                    if ($ok) {
                        db()->prepare("UPDATE subs SET status='published', published_at=?, updated=? WHERE id=?")
                            ->execute([time(), time(), $id]);
                        audit($actor, 'app.relist', $appId, (int)$sub['ver'], $from, 'published', 'ok', $why);
                        $msg = ['ok', $why];
                    } else {
                        audit($actor, 'app.relist', $appId, (int)$sub['ver'], $from, $from, 'fail', $why);
                        $msg = ['err', '重新上架失败：' . $why];
                    }
                }
            }

            /* ---------- 回滚 ---------- */
            elseif ($a === 'rollback') {
                $toVer = (int)($_POST['to_ver'] ?? 0);
                if ($toVer <= 0) {
                    $msg = ['err', '没指定要回滚到哪个版本'];
                } elseif ((string)($_POST['confirm'] ?? '') !== 'yes') {
                    $msg = ['warn', '回滚需要显式确认'];
                } else {
                    [$ok, $why] = store_rollback($appId, $toVer, $sub, $actor);
                    if ($ok) {
                        $newVer = version_active($appId);
                        db()->prepare("UPDATE subs SET status='published', pub_ver=?, published_at=?, updated=? WHERE id=?")
                            ->execute([(int)($newVer['ver'] ?? 0), time(), time(), $id]);
                        audit($actor, 'app.rollback', $appId, $toVer, $from, 'published', 'ok', $why);
                        $msg = ['ok', $why];
                    } else {
                        audit($actor, 'app.rollback', $appId, $toVer, $from, $from, 'fail', $why);
                        $msg = ['err', '回滚失败：' . $why];
                    }
                }
            } else {
                $msg = ['err', '未知操作'];
            }
        }
    }
}

/* ============================ 取数据 ============================ */
$one  = null;
$list = [];
$showLog = isset($_GET['log']);
$stFilter = (string)($_GET['st'] ?? 'pending');
$validSt = ['pending', 'reviewing', 'approved', 'rejected', 'published', 'unpublished', 'all'];

if ($id > 0) {
    $st = db()->prepare('SELECT s.*, u.email FROM subs s LEFT JOIN users u ON u.id = s.uid WHERE s.id = ?');
    $st->execute([$id]);
    $one = $st->fetch() ?: null;
} elseif (!$showLog) {
    if (!in_array($stFilter, $validSt, true)) $stFilter = 'pending';
    if ($stFilter === 'all' || $stFilter === 'published') {
        /* 「已发布」和「全部」都带上商店侧的版本，方便对照 */
        $list = db()->query('SELECT s.*, u.email FROM subs s LEFT JOIN users u ON u.id = s.uid
                             ORDER BY s.id DESC LIMIT 200')->fetchAll();
    } else {
        $st = db()->prepare('SELECT s.*, u.email FROM subs s LEFT JOIN users u ON u.id = s.uid
                             WHERE s.status = ? ORDER BY s.id ASC LIMIT 200');
        $st->execute([$stFilter]);
        $list = $st->fetchAll();
    }
}

$counts = [];
foreach (['pending', 'reviewing', 'approved', 'rejected', 'published', 'unpublished'] as $s) {
    $q = db()->prepare('SELECT COUNT(*) c FROM subs WHERE status = ?');
    $q->execute([$s]);
    $counts[$s] = (int)($q->fetch()['c'] ?? 0);
}

function lvl_color(string $l): string {
    return $l === 'high' ? '#c0392b' : ($l === 'med' ? '#b25000' : '#6e6e73');
}
$STATUS = null;
?>
<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>审核后台 — <?= h(cfg('site_name')) ?></title>
<style>
  :root{--bg:#f5f5f7;--card:#fff;--ink:#1d1d1f;--sub:#6e6e73;--line:#e5e5ea;--accent:#0071e3;--ok:#1a7f37;--warn:#b25000;--err:#c0392b}
  @media (prefers-color-scheme:dark){:root{--bg:#000;--card:#1c1c1e;--ink:#f5f5f7;--sub:#98989d;--line:#38383a;--accent:#0a84ff}}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.55 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif;padding:26px 20px}
  .wrap{max-width:900px;margin:0 auto}
  h1{font-size:24px;margin:0 0 14px}
  h2{font-size:16px;margin:22px 0 10px}
  .nav{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:18px;align-items:center}
  .nav a{padding:6px 12px;border-radius:20px;background:var(--card);border:1px solid var(--line);
         color:var(--ink);text-decoration:none;font-size:13px}
  .nav a.on{background:var(--accent);color:#fff;border-color:var(--accent)}
  .nav .sp{flex:1}
  .card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:20px;margin-bottom:16px}
  .bar{border-radius:12px;padding:12px 16px;font-size:14px;margin-bottom:16px;border:1px solid}
  .bar.ok{background:rgba(26,127,55,.08);border-color:rgba(26,127,55,.3);color:var(--ok)}
  .bar.err{background:rgba(192,57,43,.08);border-color:rgba(192,57,43,.3);color:var(--err)}
  .bar.warn{background:rgba(178,80,0,.08);border-color:rgba(178,80,0,.3);color:var(--warn)}
  table.kv{border-collapse:collapse;font-size:14px;width:100%}
  table.kv td{padding:6px 0;vertical-align:top}
  table.kv td:first-child{color:var(--sub);width:96px}
  table.list{border-collapse:collapse;width:100%;font-size:14px}
  table.list th{text-align:left;color:var(--sub);font-weight:500;font-size:12px;padding:6px 8px;border-bottom:1px solid var(--line)}
  table.list td{padding:8px;border-bottom:1px solid var(--line);vertical-align:top}
  pre{background:var(--bg);border:1px solid var(--line);border-radius:10px;padding:14px;overflow:auto;
      font:12px/1.5 ui-monospace,Menlo,monospace;max-height:340px}
  ul.risks{margin:8px 0 0;padding-left:18px;font-size:14px}
  ul.risks li{margin:4px 0}
  button{font:inherit;font-size:14px;font-weight:600;border:0;border-radius:10px;padding:10px 18px;cursor:pointer}
  .b-ok{background:var(--accent);color:#fff}
  .b-no{background:rgba(192,57,43,.12);color:var(--err)}
  .b-mid{background:rgba(178,80,0,.12);color:var(--warn)}
  .b-ghost{background:transparent;color:var(--accent);padding:6px 0}
  input[type=text],select,textarea{width:100%;padding:9px 12px;font:inherit;font-size:14px;color:var(--ink);
    background:var(--bg);border:1px solid var(--line);border-radius:9px;margin-top:6px}
  .row{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-top:14px}
  .row form{display:inline}
  .t{font-size:12px;padding:2px 9px;border-radius:20px;white-space:nowrap}
  .t-ok{background:rgba(26,127,55,.12);color:var(--ok)}
  .t-warn{background:rgba(178,80,0,.12);color:var(--warn)}
  .t-err{background:rgba(192,57,43,.12);color:var(--err)}
  .meta{color:var(--sub);font-size:12px}
  code{background:var(--bg);padding:2px 6px;border-radius:5px;font-size:13px}
  a{color:var(--accent);text-decoration:none}
  .grid2{display:grid;grid-template-columns:1fr 1fr;gap:14px}
  @media(max-width:700px){.grid2{grid-template-columns:1fr}}
</style></head><body><div class="wrap">

<h1>审核后台</h1>
<div class="nav">
  <a href="?st=pending"     class="<?= $stFilter === 'pending' && !$id && !$showLog ? 'on' : '' ?>">待审核 <?= $counts['pending'] ?></a>
  <a href="?st=reviewing"   class="<?= $stFilter === 'reviewing' ? 'on' : '' ?>">审核中 <?= $counts['reviewing'] ?></a>
  <a href="?st=approved"    class="<?= $stFilter === 'approved' ? 'on' : '' ?>">待发布 <?= $counts['approved'] ?></a>
  <a href="?st=published"   class="<?= $stFilter === 'published' ? 'on' : '' ?>">已发布 <?= $counts['published'] ?></a>
  <a href="?st=unpublished" class="<?= $stFilter === 'unpublished' ? 'on' : '' ?>">已下架 <?= $counts['unpublished'] ?></a>
  <a href="?st=rejected"    class="<?= $stFilter === 'rejected' ? 'on' : '' ?>">已驳回 <?= $counts['rejected'] ?></a>
  <a href="?log=1"          class="<?= $showLog ? 'on' : '' ?>">操作日志</a>
  <span class="sp"></span>
  <a href="index.php">投稿页</a>
  <a href="admin_login.php"><?= h($actor) ?></a>
</div>

<?php if ($msg): ?><div class="bar <?= h($msg[0]) ?>"><?= h($msg[1]) ?></div><?php endif; ?>

<?php /* ============================ 操作日志 ============================ */ ?>
<?php if ($showLog): ?>
  <div class="card">
    <table class="list">
      <tr><th>时间</th><th>操作者</th><th>动作</th><th>应用</th><th>版本</th><th>状态变化</th><th>结果</th><th>说明</th></tr>
      <?php foreach (audit_recent(200) as $r): ?>
      <tr>
        <td class="meta"><?= h(date('m-d H:i:s', (int)$r['ts'])) ?></td>
        <td><?= h($r['actor']) ?></td>
        <td><code><?= h($r['action']) ?></code></td>
        <td><?= h($r['app_id'] ?? '') ?></td>
        <td><?= $r['ver'] !== null ? 'v' . (int)$r['ver'] : '' ?></td>
        <td class="meta"><?= h(($r['from_state'] ? status_label((string)$r['from_state']) : '-') . ' → ' . ($r['to_state'] ? status_label((string)$r['to_state']) : '-')) ?></td>
        <td><span class="t <?= $r['result'] === 'ok' ? 't-ok' : 't-err' ?>"><?= h($r['result']) ?></span></td>
        <td class="meta"><?= h($r['note'] ?? '') ?></td>
      </tr>
      <?php endforeach; ?>
      <?php if (!audit_recent(1)): ?><tr><td colspan="8" class="meta">还没有记录。</td></tr><?php endif; ?>
    </table>
  </div>

<?php /* ============================ 详情 ============================ */ ?>
<?php elseif ($one): $scan = json_decode((string)$one['scan'], true) ?: (json_decode((string)$one['risks'], true) ?: []); ?>
  <p><a href="?st=<?= h($stFilter) ?>">← 返回列表</a></p>

  <div class="card">
    <table class="kv">
      <tr><td>应用 ID</td><td><code><?= h($one['name']) ?></code> <span class="meta">（= 商店文件名主干）</span></td></tr>
      <tr><td>展示名</td><td><b><?= h($one['label'] ?: $one['name']) ?></b></td></tr>
      <tr><td>分类</td><td><?= h($one['cat']) ?></td></tr>
      <tr><td>说明</td><td><?= h($one['descr']) ?></td></tr>
      <tr><td>版本</td><td>v<?= (int)$one['ver'] ?>
        <?php if ($one['pub_ver'] && (int)$one['pub_ver'] !== (int)$one['ver']): ?>
          <span class="meta">（商店里当前 v<?= (int)$one['pub_ver'] ?>）</span>
        <?php endif; ?></td></tr>
      <tr><td>包</td><td><code><?= h($one['fname']) ?></code> · <?= number_format((int)$one['size']) ?> B
        · crc32 <code><?= h($one['crc']) ?></code></td></tr>
      <tr><td>提交者</td><td><?= h($one['email']) ?> · <?= h($one['ip']) ?>
        · <?= h(date('Y-m-d H:i', (int)$one['created'])) ?></td></tr>
      <tr><td>状态</td><td><span class="t t-<?= h(status_tone((string)$one['status'])) ?>"><?= h(status_label((string)$one['status'])) ?></span>
        <?php if ($one['admin_note']): ?><span class="meta"> · <?= h($one['admin_note']) ?></span><?php endif; ?></td></tr>
      <?php if ($one['note']): ?>
      <tr><td>驳回原因</td><td style="color:var(--err)"><?= h($one['note']) ?></td></tr>
      <?php endif; ?>
    </table>
  </div>

  <?php
  /* LAPP 头 vs 网页展示信息 —— 两边不一致要显式告警 */
  $pfile = rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $one['sha'] . '.lua';
  $srcc  = is_file($pfile) ? (string)file_get_contents($pfile) : '';
  $lapp  = $srcc !== '' ? lapp_parse($srcc) : null;
  ?>
  <div class="card">
    <h2 style="margin-top:0">包内 LAPP 头（固件实际会读到的）</h2>
    <?php if (!$lapp): ?>
      <p class="meta" style="color:var(--err);margin:0">包文件不存在，无法核对。</p>
    <?php else: ?>
      <table class="kv">
        <tr><td>name</td><td><?= h($lapp['name']) ?></td></tr>
        <tr><td>cat</td><td><?= h($lapp['cat']) ?></td></tr>
        <tr><td>desc</td><td><?= h($lapp['desc']) ?></td></tr>
        <tr><td>ver</td><td><?= h($lapp['ver']) ?></td></tr>
        <tr><td>icon</td><td><?= (int)$lapp['icon_bytes'] ?> 字节<?= (int)$lapp['icon_bytes'] === 512 ? ' ✓' : '（应为 512）' ?></td></tr>
      </table>
      <?php
      $diff = [];
      if ($lapp['cat'] !== '' && norm_cat($lapp['cat']) !== $one['cat']) $diff[] = '分类';
      if ($lapp['desc'] !== '' && $lapp['desc'] !== $one['descr'])      $diff[] = '说明';
      if ($lapp['ver'] !== '' && norm_ver($lapp['ver']) !== (int)$one['ver']) $diff[] = '版本';
      ?>
      <?php if ($diff): ?>
        <div class="bar warn" style="margin:12px 0 0">包内头部与网页展示不一致：<?= h(implode('、', $diff)) ?>
          —— 商店列表显示的是网页这份，装到设备上应用自己显示的是包内那份。</div>
      <?php else: ?>
        <p class="meta" style="color:var(--ok);margin:10px 0 0">包内头部与网页展示一致 ✓</p>
      <?php endif; ?>
    <?php endif; ?>
  </div>

  <div class="card">
    <h2 style="margin-top:0">代码能力扫描（词法级）</h2>
    <?php
      $verdict = (string)($one['verdict'] ?? scan_verdict(is_array($scan) ? $scan : []));
      $tone = $verdict === 'high' ? 't-err' : ($verdict === 'med' ? 't-warn' : 't-ok');
      $txt  = ['high' => '含高风险能力，必须人工确认', 'med' => '含需确认的能力',
               'low' => '仅提示项', 'none' => '未命中任何规则'][$verdict] ?? $verdict;
    ?>
    <p><span class="t <?= $tone ?>"><?= h($txt) ?></span>
       <span class="meta">· <?= h(scan_summary(is_array($scan) ? $scan : [])) ?></span></p>
    <?php if (!$scan): ?>
      <p class="meta" style="margin:0">没有命中规则。注意：扫描通过不等于安全，运行时沙箱才是边界。</p>
    <?php else: ?>
      <ul class="risks">
      <?php foreach ($scan as $r): ?>
        <li style="color:<?= lvl_color((string)$r['level']) ?>">
          <?= $r['level'] === 'high' ? '⛔' : ($r['level'] === 'med' ? '⚠' : '·') ?>
          <code><?= h($r['code'] ?? $r['api'] ?? '?') ?></code>
          <?php if (!empty($r['line'])): ?><span class="meta">:行 <?= (int)$r['line'] ?></span><?php endif; ?>
          — <?= h($r['why'] ?? '') ?>
        </li>
      <?php endforeach; ?>
      </ul>
    <?php endif; ?>
  </div>

  <div class="card">
    <h2 style="margin-top:0">代码预览（前 60 行）</h2>
    <?php $lines = $srcc !== '' ? array_slice(preg_split('/\r\n|\n|\r/', $srcc), 0, 60) : []; ?>
    <pre><?= h(implode("\n", $lines) ?: '（包文件不存在）') ?></pre>
  </div>

  <?php /* ---------- 版本历史 ---------- */ $vers = versions_of((string)$one['name']); ?>
  <div class="card">
    <h2 style="margin-top:0">版本历史（<?= count($vers) ?>）</h2>
    <?php if (!$vers): ?>
      <p class="meta" style="margin:0">还没有发布记录。第一次通过后会出现。</p>
    <?php else: ?>
      <table class="list">
        <tr><th>版本</th><th>发布时间</th><th>大小</th><th>crc32</th><th>状态</th><th>说明</th><th>操作</th></tr>
        <?php foreach ($vers as $v): ?>
        <tr>
          <td>v<?= (int)$v['ver'] ?></td>
          <td class="meta"><?= h(date('Y-m-d H:i', (int)$v['created'])) ?></td>
          <td class="meta"><?= number_format((int)$v['size']) ?> B</td>
          <td class="meta"><code><?= h($v['crc']) ?></code></td>
          <td><?= (int)$v['active'] ? '<span class="t t-ok">当前</span>' : '<span class="t t-warn">历史</span>' ?></td>
          <td class="meta"><?= h($v['note'] ?? '') ?></td>
          <td>
            <?php if ((int)$v['active'] === 0): ?>
            <form method="post" onsubmit="return confirm('确认回滚到 v<?= (int)$v['ver'] ?>？\n会以「新版本号」重新发布这份内容，当前的版本仍保留在历史里。')">
              <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
              <input type="hidden" name="id" value="<?= (int)$one['id'] ?>">
              <input type="hidden" name="a" value="rollback">
              <input type="hidden" name="to_ver" value="<?= (int)$v['ver'] ?>">
              <input type="hidden" name="confirm" value="yes">
              <button class="b-mid" type="submit">回滚到这一版</button>
            </form>
            <?php else: ?><span class="meta">—</span><?php endif; ?>
          </td>
        </tr>
        <?php endforeach; ?>
      </table>
      <p class="meta" style="margin:10px 0 0">回滚是以「新版本号」重发旧内容：固件只在 index 的 ver 变大时才提示更新，
        直接把版本号退回去会让已经装了坏版本的用户永远收不到修复。</p>
    <?php endif; ?>
  </div>

  <?php /* ---------- 审核轨迹 ---------- */ ?>
  <?php
  $q = db()->prepare('SELECT * FROM audit_log WHERE app_id = ? ORDER BY id DESC LIMIT 30');
  $q->execute([(string)$one['name']]);
  $trail = $q->fetchAll();
  ?>
  <div class="card">
    <h2 style="margin-top:0">这条应用的操作记录</h2>
    <?php if (!$trail): ?><p class="meta" style="margin:0">暂无。</p><?php else: ?>
      <table class="list">
        <?php foreach ($trail as $r): ?>
        <tr>
          <td class="meta" style="width:110px"><?= h(date('m-d H:i', (int)$r['ts'])) ?></td>
          <td><?= h($r['actor']) ?></td>
          <td><code><?= h($r['action']) ?></code></td>
          <td><span class="t <?= $r['result'] === 'ok' ? 't-ok' : 't-err' ?>"><?= h($r['result']) ?></span></td>
          <td class="meta"><?= h($r['note'] ?? '') ?></td>
        </tr>
        <?php endforeach; ?>
      </table>
    <?php endif; ?>
  </div>

  <?php /* ---------- 操作区 ---------- */ ?>
  <div class="card">
    <h2 style="margin-top:0">操作</h2>

    <?php if (status_can_approve((string)$one['status'])): ?>
    <div class="row">
      <?php if ($one['status'] === 'pending'): ?>
      <form method="post">
        <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
        <input type="hidden" name="id" value="<?= (int)$one['id'] ?>">
        <input type="hidden" name="a" value="claim">
        <button class="b-mid" type="submit">开始审核</button>
      </form>
      <?php endif; ?>
      <form method="post" onsubmit="return confirm('通过并立即发布到商店？')">
        <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
        <input type="hidden" name="id" value="<?= (int)$one['id'] ?>">
        <input type="hidden" name="a" value="approve">
        <button class="b-ok" type="submit">通过并发布</button>
      </form>
      <form method="post">
        <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
        <input type="hidden" name="id" value="<?= (int)$one['id'] ?>">
        <input type="hidden" name="a" value="approve_only">
        <button type="submit" style="background:var(--bg);border:1px solid var(--line);color:var(--ink)">通过但暂不发布</button>
      </form>
    </div>
    <?php endif; ?>

    <?php if (in_array((string)$one['status'], ['pending', 'reviewing', 'approved'], true)): ?>
    <form method="post" style="margin-top:18px">
      <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="id" value="<?= (int)$one['id'] ?>">
      <input type="hidden" name="a" value="reject">
      <label class="meta">驳回原因（必填，会展示给投稿者）</label>
      <textarea name="note" rows="2" maxlength="300" required placeholder="例如：缺少图标；app.hid.* 属于 USB 控制能力，第三方应用不允许使用。"></textarea>
      <div class="row"><button class="b-no" type="submit">驳回</button></div>
    </form>
    <?php endif; ?>

    <form method="post" style="margin-top:22px">
      <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="id" value="<?= (int)$one['id'] ?>">
      <input type="hidden" name="a" value="edit">
      <div class="grid2">
        <div><label class="meta">展示名（≤24 字）</label>
          <input type="text" name="label" maxlength="24" value="<?= h($one['label'] ?: $one['name']) ?>"></div>
        <div><label class="meta">分类</label>
          <select name="cat">
            <?php foreach ((array)cfg('cats') as $c): ?>
              <option value="<?= h($c) ?>" <?= $c === $one['cat'] ? 'selected' : '' ?>><?= h($c) ?></option>
            <?php endforeach; ?>
          </select></div>
      </div>
      <label class="meta">一句话说明（≤40 字）</label>
      <input type="text" name="descr" maxlength="40" value="<?= h($one['descr']) ?>">
      <div class="row"><button type="submit" style="background:var(--bg);border:1px solid var(--line);color:var(--ink)">保存展示信息</button>
        <span class="meta">只改商店怎么显示，不改包、不改版本</span></div>
    </form>

    <div class="row" style="margin-top:22px">
      <?php if ($one['status'] === 'published'): ?>
      <form method="post" onsubmit="return confirm('确认下架？应用会从商店索引移除（已完成安装的设备不受影响，文件与历史版本保留）。')">
        <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
        <input type="hidden" name="id" value="<?= (int)$one['id'] ?>">
        <input type="hidden" name="a" value="unpublish">
        <button class="b-no" type="submit">下架</button>
      </form>
      <?php elseif (in_array((string)$one['status'], ['unpublished', 'published', 'approved'], true)): ?>
      <form method="post" onsubmit="return confirm('重新上架？会用当前记录的版本文件重建索引条目。')">
        <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
        <input type="hidden" name="id" value="<?= (int)$one['id'] ?>">
        <input type="hidden" name="a" value="relist">
        <button class="b-ok" type="submit">重新上架</button>
      </form>
      <?php endif; ?>
      <a class="meta" href="download.php?id=<?= (int)$one['id'] ?>">下载原始投稿文件</a>
    </div>
  </div>

<?php /* ============================ 列表 ============================ */ ?>
<?php else: ?>
  <div class="card">
    <?php if (!$list): ?>
      <p class="meta" style="margin:0">这个状态下没有投稿。</p>
    <?php else: ?>
      <table class="list">
        <tr><th>应用</th><th>版本</th><th>大小</th><th>扫描</th><th>投稿者</th><th>状态</th><th></th></tr>
        <?php foreach ($list as $s):
          $sc = json_decode((string)($s['scan'] ?? ''), true) ?: [];
          $vd = (string)($s['verdict'] ?? scan_verdict(is_array($sc) ? $sc : []));
        ?>
        <tr>
          <td><b><?= h($s['label'] ?: $s['name']) ?></b>
            <div class="meta"><?= h($s['descr']) ?></div></td>
          <td>v<?= (int)$s['ver'] ?></td>
          <td class="meta"><?= number_format((int)$s['size']) ?> B</td>
          <td><span class="t <?= $vd === 'high' ? 't-err' : ($vd === 'med' ? 't-warn' : 't-ok') ?>"><?= h($vd === 'none' ? '干净' : $vd) ?></span></td>
          <td class="meta"><?= h($s['email']) ?><br><?= h(date('m-d H:i', (int)$s['created'])) ?></td>
          <td><span class="t t-<?= h(status_tone((string)$s['status'])) ?>"><?= h(status_label((string)$s['status'])) ?></span></td>
          <td><a href="?id=<?= (int)$s['id'] ?>">审核 →</a></td>
        </tr>
        <?php endforeach; ?>
      </table>
    <?php endif; ?>
  </div>
<?php endif; ?>

</div></body></html>
