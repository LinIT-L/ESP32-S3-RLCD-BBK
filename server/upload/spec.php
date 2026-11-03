<?php
/**
 * spec.php — 第三方 Lua 应用开发规范（内容以固件真实代码为准）
 *
 * 这里写的每一条都有源码依据，不编 API:
 *   LAPP 头解析      components/services/apppack/apppack.c
 *   契约函数         components/apps/pages/page_lua.c （全局函数 + pcall）
 *   app.* 接口清单   components/apps/lua_bridge/luat_lib_*.c
 *   第三方限制       components/apps/lua_bridge/lua_sandbox.c
 *   index.json 字段  components/apps/pages/am_store_net.c
 *
 * ?tpl=1 直接下载最小模板（minimal.lua）。
 */
declare(strict_types=1);
require __DIR__ . '/lib.php';

if (isset($_GET['tpl'])) {
    $f = __DIR__ . '/minimal.lua';
    if (!is_file($f)) { http_response_code(404); echo '模板文件缺失'; exit; }
    header('Content-Type: text/plain; charset=utf-8');
    header('Content-Length: ' . filesize($f));
    header("Content-Disposition: attachment; filename*=UTF-8''" . rawurlencode('我的应用.lua'));
    readfile($f);
    exit;
}

$cats = (array)cfg('cats');
?>
<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>开发规范 — <?= h(cfg('site_name')) ?></title>
<style>
  :root{--bg:#f5f5f7;--card:#fff;--ink:#1d1d1f;--sub:#6e6e73;--line:#e5e5ea;--accent:#0071e3;--ok:#1a7f37;--warn:#b25000;--err:#c0392b}
  @media (prefers-color-scheme:dark){:root{--bg:#000;--card:#1c1c1e;--ink:#f5f5f7;--sub:#98989d;--line:#38383a;--accent:#0a84ff}}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--ink);
    font:16px/1.65 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif;padding:36px 20px}
  .wrap{max-width:760px;margin:0 auto}
  h1{font-size:28px;margin:0 0 6px}
  h2{font-size:19px;margin:32px 0 10px;padding-top:8px;border-top:1px solid var(--line)}
  h3{font-size:15px;margin:20px 0 6px;color:var(--sub)}
  p,li{font-size:15px}
  .lede{color:var(--sub);margin:0 0 20px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:20px;margin:16px 0}
  pre{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;overflow:auto;
      font:12.5px/1.6 ui-monospace,Menlo,monospace;margin:10px 0}
  code{background:var(--card);padding:2px 6px;border-radius:5px;font-size:13px}
  table{border-collapse:collapse;width:100%;font-size:14px;margin:10px 0}
  th{text-align:left;color:var(--sub);font-weight:500;font-size:12px;padding:6px 8px;border-bottom:1px solid var(--line)}
  td{padding:7px 8px;border-bottom:1px solid var(--line);vertical-align:top}
  .no{color:var(--err);font-weight:600}
  .yes{color:var(--ok)}
  .bar{border-radius:12px;padding:12px 16px;font-size:14px;border:1px solid;
       background:rgba(178,80,0,.08);border-color:rgba(178,80,0,.3);color:var(--warn)}
  a{color:var(--accent);text-decoration:none}
  .btn{display:inline-block;background:var(--accent);color:#fff;padding:10px 20px;border-radius:10px;
       font-weight:600;font-size:14px;margin-top:6px}
  .toc{columns:2;font-size:14px}
  @media(max-width:600px){.toc{columns:1}}
</style></head><body><div class="wrap">

<h1>LinTOS Lua 应用开发规范</h1>
<p class="lede">这份文档里的接口与限制都来自固件当前源码，不是设计稿。
  <a href="spec.php?tpl=1">↓ 下载最小模板（.lua）</a></p>

<div class="card toc">
  <a href="#h1">1. 文件格式（LAPP 头）</a><br>
  <a href="#h2">2. 应用 ID 与命名</a><br>
  <a href="#h3">3. 分类 / 版本 / 图标</a><br>
  <a href="#h4">4. 契约函数</a><br>
  <a href="#h5">5. 可用 API</a><br>
  <a href="#h6">6. 数据存储与文件</a><br>
  <a href="#h7">7. 第三方应用的限制</a><br>
  <a href="#h8">8. 资源上限</a><br>
  <a href="#h9">9. 提交与审核流程</a><br>
  <a href="#h10">10. 常见驳回原因</a>
</div>

<h2 id="h1">1. 文件格式（LAPP 头）</h2>
<p>一个应用就是**单个 <code>.lua</code> 文本文件**（UTF-8，无 BOM，≤ 512 KB）。
文件开头是若干行注释形式的头部，固件**静态解析、不执行**：</p>
<pre>--@lapp v1
--@app name=我的应用 cat=软件 desc=一句话说明 version=1
--@app icon=&lt;base64，512 字节&gt;
--@app build=202610311200      ← 可选，打包器自动写，手写可不写</pre>
<p>规则：</p>
<ul>
  <li>两种写法都支持：<b>一行多个 key=value</b>，或<b>一行一个</b>（<code>--@app name=…</code> 单独一行）。</li>
  <li>头部结束于**第一行既不是注释也不是空行**的代码。所以头部必须写在最前面。</li>
  <li>值里可以有空格（如 <code>desc=背包 技能 升级</code>），解析器按 <code>key=</code> 切分，不会截断。</li>
  <li>不认识的键会被忽略；旧固件也忽略新键，所以**加新键是安全的**。</li>
</ul>

<h2 id="h2">2. 应用 ID 与命名</h2>
<p>商店里的应用 ID = <b>去掉 <code>.lua</code> 后的文件名主干</b>，也是设备上
<code>bbk/pkgs/&lt;应用ID&gt;.lua</code> 的文件名。所以：</p>
<ul>
  <li><code>name</code> 不能含 <code>\ / : * ? " &lt; &gt; |</code>，长度 ≤ 24 字；</li>
  <li>同一个应用更新时**必须用完全相同的 name**，否则会被当成新应用（旧版本仍留在商店）；</li>
  <li>中文名可以用（现有商店里就有「英雄坛说」「QQ堂冒险」）。</li>
</ul>

<h2 id="h3">3. 分类 / 版本 / 图标</h2>
<p><b>分类</b>只能是：<?php foreach ($cats as $c): ?><code><?= h($c) ?></code> <?php endforeach; ?>。
旧版常用的「程序/工具/应用 → 软件」「系统/设置 → 标准」「网络 → 运维」会被自动归一化。</p>
<p><b>版本</b>：整数，<b>必须比商店里当前版本大</b>；固件就是靠 <code>index.json</code> 里的
<code>ver</code> 变大来提示「更新」的。写 <code>1.0</code> / <code>v2</code> 只取第一段数字。</p>
<p><b>图标</b>：64×64 单色点阵，<b>base64 解码后必须正好 512 字节</b>（每行 8 字节，MSB 在前，1=黑）。
不写图标也能过审，商店会用该分类的默认图标。</p>

<h2 id="h4">4. 契约函数</h2>
<p>框架会按名字找下面这些<b>全局函数</b>（写成 <code>local function</code> 会被当成不存在，表现是"界面全白且无报错"）：</p>
<table>
  <tr><th>函数</th><th>何时调用</th><th>必须</th></tr>
  <tr><td><code>render()</code></td><td>每帧。只画，不要读文件/联网/睡等</td><td class="yes">是</td></tr>
  <tr><td><code>action(a)</code></td><td>按键。<code>a</code> = <code>app.UP/DOWN/LEFT/RIGHT/OK/CONFIRM/BACK/HOME</code>。返回 <code>false</code> = 交回框架（BACK 会弹"退出程序?"）</td><td>否</td></tr>
  <tr><td><code>on_enter()</code></td><td>进入页面时一次</td><td>否</td></tr>
  <tr><td><code>poll()</code></td><td>每帧结尾，只做非阻塞的事</td><td>否</td></tr>
  <tr><td><code>on_exit()</code></td><td>退出前（停定时器/释放句柄）</td><td>否</td></tr>
</table>
<p>触摸走 <code>app.touch()</code> / <code>app.touch_start()</code> 等主动查询接口，不需要额外回调。</p>

<h2 id="h5">5. 可用 API</h2>
<p>全部挂在全局表 <code>app</code> 下，按模块分组（下表按模块列名字，具体参数请在固件 API 参考手册里查同一名字）：</p>
<table>
  <tr><th>模块</th><th>函数</th><th>第三方可用</th></tr>
  <tr><td><code>app.gfx</code></td><td>clear px fill rect store_rect statusbar hline vline text textc textw big bigc zh zh24 bmp bmpr zhs texts textm textsb zhsm zhsb mix mixc mixw poly</td><td class="yes">✓</td></tr>
  <tr><td><code>app</code>（顶层）</td><td>millis key delay key_down keys_mask key_repeat touch touch_start touch_hold redraw W H 及各颜色/按键常量</td><td class="yes">✓</td></tr>
  <tr><td><code>app.fs</code></td><td>exists read read_at size listdir write append remove rename mkdir</td><td class="yes">✓ 只限自身数据目录</td></tr>
  <tr><td><code>app.db</code></td><td>set get del seti geti</td><td class="yes">✓ 每个应用独立命名空间</td></tr>
  <tr><td><code>app.nvs</code></td><td>get_int set_int get_str set_str</td><td class="yes">✓ 同样按应用隔离</td></tr>
  <tr><td><code>app.util</code></td><td>after every cancel rand randf crc32 b64 unb64 serialize unserialize epoch sleep clamp lerp dist round sign</td><td class="yes">✓ sleep 会被剩余运行时间夹紧</td></tr>
  <tr><td><code>app.json</code></td><td>pick num str bool</td><td class="yes">✓</td></tr>
  <tr><td><code>app.net</code></td><td>ip mac is_connected get_ip mirrors http_get fetch_start fetch_poll fetch_got ping port arp arp_list ipinfo vendor</td><td class="yes">✓ 联网是允许的</td></tr>
  <tr><td><code>app.wifi</code></td><td>init scan scan_done ap_count ap_info probe_init probe_stop probe_count probe_info release ch_stat cur_ssid connecting connect setup connected clear_saved</td><td class="yes">✓</td></tr>
  <tr><td><code>app.sys</code></td><td>log tone tone_async melody vibrate volume mute bt exit exit_confirm sbar battery heap_free time_ms date clock toast confirm list result dlg</td><td class="yes">✓</td></tr>
  <tr><td><code>app.img</code></td><td>load blit size free freeall</td><td class="yes">✓ load 只限自身数据目录</td></tr>
  <tr><td><code>app.kbd</code></td><td>style x y w row_h rows use reset draw hit geom height count press char label layer_on get set</td><td class="yes">✓ 软键盘</td></tr>
  <tr><td><code>app.espnow</code></td><td>start stop ready mac channel add del send bcast recv pending stats</td><td class="yes">✓</td></tr>
  <tr><td><code>app.gps</code></td><td>proto sys prn el az snr used start stop fix sats demo reset</td><td class="yes">✓</td></tr>
  <tr><td><code>app.rec</code></td><td>start stop active ms bytes peak mic_available spec spec_bins play mplay mplaying mstop stop_play playing play_ms play_total_ms</td><td><span class="no">✗ start 禁用</span>（麦克风），播放可用</td></tr>
  <tr><td><code>app.hid</code></td><td>start stop connected running key key_str mouse_move mouse_click shortcut cdc_read cdc_connected</td><td><span class="no">✗ 全部禁用</span>（USB 键鼠 / BadUSB）</td></tr>
  <tr><td><code>app.wiki</code></td><td>count name lines id title info search titles load line close</td><td><span class="no">✗ open 禁用</span>（指定目录读文件），其余在官方库上仍可用</td></tr>
</table>
<div class="bar">上表的「可用」是<b>运行时强制</b>的：被禁的能力在沙箱里会返回
 <code>permission denied: &lt;api&gt;</code>，不是靠应用自觉遵守。也不要指望"换个写法绕过" ——
 路径参数在 C 层统一校验。</div>

<h2 id="h6">6. 数据存储与文件</h2>
<p>第三方应用的数据目录是 <code>/sdcard/apps/.data/&lt;应用ID&gt;/</code>（首次启动自动创建）。</p>
<pre>-- 推荐: 用相对路径, 自动落到自己的数据目录
app.fs.write("save.dat", data)
local s = app.fs.read("save.dat")

-- 绝对路径只允许写/读自己的数据目录
app.fs.write("/sdcard/apps/.data/我的应用ID/save.dat", data)   -- ok
app.fs.read("/sdcard/别的东西")                                  -- permission denied
app.fs.read("/store/apps/x.lua")                                -- permission denied</pre>
<p>小配置优先用 <code>app.db.set/get</code>（掉电不丢、按应用隔离），别自己写文件。</p>

<h2 id="h7">7. 第三方应用的限制</h2>
<table>
  <tr><th>能力</th><th>状态</th><th>原因</th></tr>
  <tr><td><code>os.execute</code> / <code>io.popen</code></td><td class="no">已移除</td><td>命令执行</td></tr>
  <tr><td><code>loadfile</code> / <code>dofile</code> / <code>package.loadlib</code></td><td class="no">已移除</td><td>任意代码加载</td></tr>
  <tr><td><code>require</code> 加载磁盘模块</td><td class="no">已限制</td><td>单文件应用不需要；只保留已注册的 C 库</td></tr>
  <tr><td><code>app.hid.*</code></td><td class="no">已禁用</td><td>USB 键鼠可当 BadUSB 用</td></tr>
  <tr><td><code>app.rec.start</code></td><td class="no">已禁用</td><td>静默录音是隐私问题</td></tr>
  <tr><td><code>app.wiki.open</code></td><td class="no">已禁用</td><td>目录参数=任意路径读取</td></tr>
  <tr><td>网络 / WiFi / ESP-NOW</td><td class="yes">允许</td><td>正常需求，但请在界面上说明会联网</td></tr>
</table>
<p>还有两条硬约束：文件路径由 C 层统一归一化（<code>..</code> 与反斜杠都按 FatFs 语义处理），
跨应用目录一律拒绝；<code>app.db</code>/<code>app.nvs</code> 的命名空间按应用隔离，读不到别人的、也读不到系统 KV。</p>

<h2 id="h8">8. 资源上限</h2>
<table>
  <tr><th>项</th><th>限制</th><th>超了会怎样</th></tr>
  <tr><td>文件大小</td><td>512 KB</td><td>提交被拒</td></tr>
  <tr><td>Lua 内存</td><td>4 MB（只算 Lua 自己分配的）</td><td>抛 <code>not enough memory</code>，应用退出，系统不受影响</td></tr>
  <tr><td>单次运行时长</td><td>10 秒</td><td>抛 <code>timeout: 应用运行超过 10000 ms</code>，应用退出</td></tr>
</table>
<div class="bar">10 秒是**单次脚本执行**的上限，不是"应用只能用 10 秒"：
 正常写法是 <code>render()</code> 每帧返回，框架反复调用，不会累积。真正会被掐的是死循环
 （<code>while true do end</code>）和递归爆栈这类"回不来"的代码。
 阻塞调用（<code>app.util.sleep</code>、<code>app.net.http_get</code>）的时长会被夹到剩余预算内。</div>

<h2 id="h9">9. 提交与审核流程</h2>
<pre>上传 .lua → 服务端校验（大小/UTF-8/NUL/头部/图标/骨架/重复/版本）
   → 待审核 → 站主审阅（看代码、看能力扫描、必要时下载实测）
   → 通过并发布：写入 bbk/pkgs/ 与 index.json（ver+1），设备刷新商店即可见
   → 或驳回：附原因；改好后可在「我的提交」里点「改好后重新提交」</pre>
<p>状态：待审核 / 审核中 / 审核通过 / 已发布 / 已驳回 / 已下架。</p>
<p class="lede">扫描结果是**辅助**：它能识别危险 API 调用（含注释/字符串区分、<code>app.fs . write</code> 这类写法），
但动态拼出来的代码无法静态判定。真正兜底的是固件里的运行时隔离。</p>

<h2 id="h10">10. 常见驳回原因</h2>
<ul>
  <li>没有 LAPP 头，或 <code>name</code>/<code>ver</code> 缺失；</li>
  <li><code>render</code> 不是全局函数（写成 <code>local function render</code>）；</li>
  <li>版本号没有变大就重复提交；</li>
  <li>图标不是 512 字节；</li>
  <li>用了 <code>app.hid.*</code> / <code>app.rec.start</code> 等第三方禁用能力；</li>
  <li>试图读写别的应用或系统目录（沙箱里本来也会失败）；</li>
  <li>死循环 / 每帧阻塞（会被 10 秒上限打断，体验很差）；</li>
  <li>文件不是 UTF-8（中文显示成乱码）。</li>
</ul>

<p style="margin-top:32px"><a class="btn" href="spec.php?tpl=1">下载最小模板</a>
  <a href="index.php" style="margin-left:16px">去提交 →</a></p>

</div></body></html>
