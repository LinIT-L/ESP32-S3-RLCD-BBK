<?php
require __DIR__ . '/lib.php';
$u = current_user();
$flash = take_flash();
$smtpReady = !empty(cfg('smtp')['user']) && !empty(cfg('smtp')['pass']);
$cats = (array)cfg('cats');
$mySubs = [];
if ($u) {
    $st = db()->prepare('SELECT * FROM subs WHERE uid = ? ORDER BY id DESC LIMIT 30');
    $st->execute([(int)$u['id']]);
    $mySubs = $st->fetchAll();
}
$STATUS = ['pending' => '待审核', 'approved' => '已上架', 'rejected' => '已驳回'];
?>
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title><?= h(cfg('site_name')) ?></title>
<style>
  :root{
    --bg:#f5f5f7; --card:#fff; --ink:#1d1d1f; --sub:#6e6e73;
    --line:#e5e5ea; --accent:#0071e3; --ok:#1a7f37; --warn:#b25000; --err:#c0392b;
  }
  @media (prefers-color-scheme: dark){
    :root{ --bg:#000; --card:#1c1c1e; --ink:#f5f5f7; --sub:#98989d; --line:#38383a; --accent:#0a84ff; }
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--ink);
    font:16px/1.5 -apple-system,BlinkMacSystemFont,"SF Pro SC","PingFang SC","Helvetica Neue",Arial,sans-serif;
    -webkit-font-smoothing:antialiased;padding:40px 20px}
  .wrap{max-width:680px;margin:0 auto}
  h1{font-size:32px;letter-spacing:-.02em;margin:0 0 6px;font-weight:700}
  .lede{color:var(--sub);margin:0 0 28px;font-size:15px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:24px;margin-bottom:18px}
  .card h2{font-size:18px;margin:0 0 16px;font-weight:600}
  label{display:block;font-size:13px;color:var(--sub);margin:14px 0 6px;font-weight:500}
  input[type=text],input[type=email],select,textarea{
    width:100%;padding:11px 13px;font-size:15px;font-family:inherit;color:var(--ink);
    background:var(--bg);border:1px solid var(--line);border-radius:10px;outline:none}
  input:focus,select:focus,textarea:focus{border-color:var(--accent);background:var(--card)}
  textarea{resize:vertical;min-height:64px}
  button{font-family:inherit;font-size:15px;font-weight:600;border:0;border-radius:10px;
    padding:12px 22px;cursor:pointer;background:var(--accent);color:#fff;transition:opacity .15s}
  button:hover{opacity:.86}
  button:disabled{opacity:.4;cursor:default}
  button.ghost{background:transparent;color:var(--accent);padding:8px 0}
  .row{display:flex;gap:12px;align-items:flex-end}
  .row>*{flex:1}
  .row .fix{flex:0 0 auto}
  .hint{font-size:13px;color:var(--sub);margin-top:8px}
  .bar{border-radius:12px;padding:12px 16px;font-size:14px;margin-bottom:18px;border:1px solid}
  .bar.ok{background:rgba(26,127,55,.08);border-color:rgba(26,127,55,.3);color:var(--ok)}
  .bar.err{background:rgba(192,57,43,.08);border-color:rgba(192,57,43,.3);color:var(--err)}
  .bar.warn{background:rgba(178,80,0,.08);border-color:rgba(178,80,0,.3);color:var(--warn)}
  .msg{font-size:13px;margin:6px 0}
  .msg.e{color:var(--err)} .msg.w{color:var(--warn)} .msg.g{color:var(--ok)}
  ul.issues{margin:10px 0 0;padding-left:20px;font-size:13px}
  ul.issues li{margin:3px 0}
  .iconbox{display:flex;gap:14px;align-items:center;margin-top:10px}
  canvas{border:1px solid var(--line);border-radius:10px;background:#fff;image-rendering:pixelated}
  .subs{list-style:none;margin:0;padding:0}
  .subs li{display:flex;justify-content:space-between;gap:12px;padding:12px 0;border-top:1px solid var(--line);font-size:14px}
  .subs li:first-child{border-top:0}
  .tag{font-size:12px;padding:3px 9px;border-radius:20px;white-space:nowrap}
  .t-pending{background:rgba(178,80,0,.12);color:var(--warn)}
  .t-approved{background:rgba(26,127,55,.12);color:var(--ok)}
  .t-rejected{background:rgba(192,57,43,.12);color:var(--err)}
  .meta{color:var(--sub);font-size:12px;margin-top:2px}
  code{background:var(--bg);padding:2px 6px;border-radius:5px;font-size:13px}
  a{color:var(--accent);text-decoration:none}
  .foot{color:var(--sub);font-size:13px;text-align:center;margin-top:28px}
</style>
</head>
<body>
<div class="wrap">
  <h1>提交一个应用</h1>
  <p class="lede">上传单个 <code>.lua</code> 文件，会自动读出里面的应用名、分类、说明和版本号。提交后由站主审核，通过即进商店。</p>

<?php if ($flash): ?>
  <div class="bar <?= h($flash['t']) ?>"><?= h($flash['m']) ?></div>
<?php endif; ?>

<?php if (!$smtpReady): ?>
  <div class="bar warn">提交功能暂未开放：站点还没配好邮件发送（收不到验证码）。</div>
<?php endif; ?>

<?php if (!$u): ?>
  <!-- ============ 登录 / 注册（邮箱验证码，一次搞定） ============ -->
  <div class="card">
    <h2>用邮箱登录</h2>
    <p class="hint" style="margin-top:-6px">第一次用某个邮箱会自动创建账号，不需要单独注册。</p>
    <form method="post" action="auth.php" id="frm-login">
      <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="do" value="send">
      <label for="email">邮箱</label>
      <div class="row">
        <input id="email" type="email" name="email" placeholder="you@example.com" required
               value="<?= h($_GET['email'] ?? '') ?>" <?= $smtpReady ? '' : 'disabled' ?>>
        <button class="fix" type="submit" <?= $smtpReady ? '' : 'disabled' ?>>发验证码</button>
      </div>
    </form>

    <form method="post" action="auth.php">
      <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="do" value="verify">
      <label for="email2">邮箱</label>
      <input id="email2" type="email" name="email" required value="<?= h($_GET['email'] ?? '') ?>">
      <label for="code">6 位验证码</label>
      <div class="row">
        <input id="code" type="text" name="code" inputmode="numeric" pattern="\d{6}" maxlength="6"
               placeholder="000000" required>
        <button class="fix" type="submit">登录</button>
      </div>
      <p class="hint">验证码 10 分钟内有效。没收到就等 60 秒再发一次。</p>
    </form>
  </div>

<?php else: ?>
  <!-- ============ 已登录：上传 ============ -->
  <div class="card" style="display:flex;justify-content:space-between;align-items:center;padding:16px 24px">
    <div><strong><?= h($u['email']) ?></strong>
      <div class="meta">今日还可提交 <?= upload_quota_left((int)$u['id']) ?> 个</div></div>
    <form method="post" action="auth.php" style="margin:0">
      <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="do" value="logout">
      <button class="ghost" type="submit">退出</button>
    </form>
  </div>

  <div class="card">
    <h2>选择应用文件</h2>
    <form method="post" action="submit.php" enctype="multipart/form-data" id="frm-up">
      <input type="hidden" name="_csrf" value="<?= h(csrf()) ?>">
      <input type="file" id="file" name="lua" accept=".lua,text/plain" required
             style="width:100%;padding:14px;border:1.5px dashed var(--line);border-radius:12px;background:var(--bg)">
      <p class="hint">单个 .lua 文件，≤ <?= (int)(cfg('limits')['max_bytes'] / 1024) ?> KB。选完会自动读出头部信息，下面可以改。</p>

      <div id="preview" style="display:none">
        <div class="iconbox">
          <canvas id="icon" width="64" height="64" style="width:64px;height:64px"></canvas>
          <div>
            <div id="pv-name" style="font-weight:600">—</div>
            <div class="meta" id="pv-cat">—</div>
          </div>
        </div>
      </div>

      <label for="name">应用名称</label>
      <input id="name" type="text" name="name" maxlength="24" placeholder="从文件头自动读取">
      <label for="cat">分类</label>
      <select id="cat" name="cat">
        <option value="">请选择</option>
        <?php foreach ($cats as $c): ?>
          <option value="<?= h($c) ?>"><?= h($c) ?></option>
        <?php endforeach; ?>
      </select>
      <label for="descr">一句话说明</label>
      <textarea id="descr" name="descr" maxlength="40" placeholder="商店列表里显示的第二行"></textarea>
      <label for="ver">版本号（整数，发布新版时 +1）</label>
      <input id="ver" type="text" name="ver" inputmode="numeric" pattern="\d{1,3}" placeholder="1">

      <div id="issues" style="margin-top:14px"></div>
      <div style="margin-top:20px"><button type="submit" id="btn-up">提交审核</button></div>
    </form>
  </div>

  <div class="card">
    <h2>我的提交</h2>
    <?php if (!$mySubs): ?>
      <p class="hint" style="margin:0">还没有提交过。</p>
    <?php else: ?>
      <ul class="subs">
      <?php foreach ($mySubs as $s): $st = $s['status']; ?>
        <li>
          <div>
            <div><strong><?= h($s['label'] ?: $s['name']) ?></strong>
              <span class="meta">v<?= (int)$s['ver'] ?> · <?= h($s['cat']) ?> · <?= h(date('m-d H:i', (int)$s['created'])) ?></span>
            </div>
            <?php if ($st === 'rejected' && $s['note']): ?>
              <div class="meta" style="color:var(--err)">驳回原因：<?= h($s['note']) ?></div>
            <?php elseif ($st === 'approved' && $s['pub_ver']): ?>
              <div class="meta">商店版本 v<?= (int)$s['pub_ver'] ?></div>
            <?php endif; ?>
          </div>
          <span class="tag t-<?= h($st) ?>"><?= h($STATUS[$st] ?? $st) ?></span>
        </li>
      <?php endforeach; ?>
      </ul>
    <?php endif; ?>
  </div>
<?php endif; ?>

  <p class="foot">LinTOS 应用商店 · 提交即表示同意由站主审核后上架</p>
</div>

<script>
/* 选中文件 → 读文本 → 解析 LAPP 头 → 自动填表 + 画图标
   与固件 apppack.c 同一套规则：每行一个 --@app key=value */
const $ = (id) => document.getElementById(id);
const fileEl = $('file');
if (fileEl) {
  fileEl.addEventListener('change', async () => {
    const f = fileEl.files[0];
    $('issues').innerHTML = '';
    $('preview').style.display = 'none';
    if (!f) return;
    const buf = await f.arrayBuffer();
    const bytes = new Uint8Array(buf);
    let text;
    try { text = new TextDecoder('utf-8', { fatal: true }).decode(bytes); }
    catch (e) {
      $('issues').innerHTML = '<div class="msg e">文件不是 UTF-8 编码，中文会显示成乱码，请另存为 UTF-8 再传。</div>';
      return;
    }
    /* 解析头部：同时支持「一行多个 key=value」和「一行一个」两种写法。
       不能按空格直接切 —— desc 里可能有空格，按空格切会截断。 */
    const parseKv = (body) => {
      const out = {}, hits = [];
      const re = /(?:^|\s)([A-Za-z_]+)=/g;
      let mm;
      while ((mm = re.exec(body)) !== null) {
        hits.push({ key: mm[1], from: mm.index + mm[0].length, at: mm.index });
      }
      hits.forEach((hit, i) => {
        const to = (i + 1 < hits.length) ? hits[i + 1].at : body.length;
        out[hit.key] = body.slice(hit.from, to).trim();
      });
      return out;
    };
    const head = text.slice(0, 8192);
    const meta = {};
    let hasHead = false;
    for (const line of head.split(/\r\n|\n|\r/)) {
      if (line.startsWith('--@app ')) {
        hasHead = true;
        Object.assign(meta, parseKv(line.slice(7)));
      } else if (line !== '' && !line.startsWith('-')) break;
    }
    const issues = [];
    if (!hasHead) issues.push(['e', '文件没有 LAPP 头：第一行应为 --@app name=… cat=… desc=… ver=1']);

    if (meta.name) $('name').value = meta.name;
    if (meta.cat) {
      /* 与后端、与 tools/store_build.py 同一张归一化表（老应用常写 工具/程序/系统） */
      const CATMAP = {'程序':'软件','工具':'软件','应用':'软件','系统':'标准','设置':'标准','网络':'运维'};
      $('cat').value = CATMAP[meta.cat] || meta.cat;
      if (!$('cat').value) {
        $('cat').value = '软件';
        issues.push(['w', '分类「' + meta.cat + '」不在商店分类里，已按「软件」处理，可在下面改']);
      }
    }
    if (meta.desc) $('descr').value = meta.desc;
    /* 版本号：1.0 / v2 一律取整数段，与后端一致 */
    const verRaw = meta.ver || meta.version || '';
    const verNum = (String(verRaw).match(/(\d{1,3})/) || [])[1];
    if (verNum) $('ver').value = verNum;

    /* 图标预览：base64 → 512 字节 → 64x64 1bpp（MSB first, 1=黑） */
    if (meta.icon) {
      try {
        const bin = atob(meta.icon.replace(/\s+/g, ''));
        if (bin.length !== 512) {
          issues.push(['e', '图标解码后 ' + bin.length + ' 字节，必须是 512 字节（64×64 1bpp）']);
        } else {
          const cv = $('icon'), cx = cv.getContext('2d');
          const img = cx.createImageData(64, 64);
          for (let y = 0; y < 64; y++) for (let x = 0; x < 64; x++) {
            const b = bin.charCodeAt(y * 8 + (x >> 3));
            const bit = (b >> (7 - (x & 7))) & 1;
            const o = (y * 64 + x) * 4;
            const v = bit ? 0 : 255;
            img.data[o] = img.data[o + 1] = img.data[o + 2] = v; img.data[o + 3] = 255;
          }
          cx.putImageData(img, 0, 0);
          $('preview').style.display = 'block';
        }
      } catch (e) { issues.push(['e', '图标不是合法的 base64']); }
    } else {
      issues.push(['w', '没有图标，商店会使用该分类的默认图标']);
    }
    $('pv-name').textContent = meta.name || '（未命名）';
    $('pv-cat').textContent  = (meta.cat || '未分类') + (verNum ? ' · v' + verNum : '');

    /* 危险能力提示（真正的判定以后端为准） */
    const RISK = [
      ['app.hid.start', '能模拟 USB 键盘/鼠标（高危）'],
      ['app.fs.remove', '能删除设备文件（高危）'],
      ['app.net.http_get', '会联网'],
      ['app.espnow', '使用 ESP-NOW 无线通信'],
      ['app.rec.start', '会启用麦克风'],
    ];
    for (const [api, why] of RISK) if (text.includes(api)) issues.push(['w', '检测到 ' + api + '：' + why]);

    if (issues.length) {
      $('issues').innerHTML = '<ul class="issues">' +
        issues.map(([t, m]) => '<li class="msg ' + t + '">' + m + '</li>').join('') + '</ul>';
    }
  });
}
</script>
</body>
</html>
