# LinTOS 应用提交站（server/upload）

让第三方通过网页提交 Lua 应用，经人工审核后自动进商店 `bbk/index.json`。

线上地址：<https://bbk.linit.cn/upload/>

## 文件

| 文件 | 作用 |
|---|---|
| `index.php` | 主页面：邮箱登录 + 上传表单（选文件后**自动读出** LAPP 头、画图标预览）+ 我的提交 |
| `auth.php` | 发验证码 / 校验登录 / 退出 |
| `submit.php` | 接收上传：解析 → 校验 → 危险 API 扫描 → 落盘 → 入库 → 邮件通知 |
| `review.php` | 审核面板（凭 `review_key` 打开，不需要登录）：列表、详情、代码预览、通过/驳回 |
| `lib.php` | 公共库：SQLite、LAPP 解析、校验、风险扫描、SMTP、商店读写、会话与限频 |
| `config.example.php` | 配置模板（**复制成 config.php 再填**，`config.php` 不入库） |

运行时数据（不入库）：`data/app.db`（SQLite）、`data/pkgs/`（待审包）。

## 首次部署

```bash
cd /www/wwwroot/bbk.linit.cn/upload
cp config.example.php config.php
# ① 填 admin_email（审核通知收件人）
# ② 生成审核令牌并填进 review_key
php -r 'echo bin2hex(random_bytes(16)), "\n";'
# ③ 填 smtp（见下）
mkdir -p data/pkgs && chmod 700 data data/pkgs && chown -R www:www .
```

审核面板：`https://bbk.linit.cn/upload/review.php?k=<review_key>`

## 邮件：必须走 SMTP 中继

阿里云**封了 25 端口出站**，PHP 的 `mail()`（走本机 postfix）发不出去。实测 465/587 可通。

`config.php` 里填一个邮箱的 **SMTP 授权码**（不是登录密码）：

```php
'smtp' => [
    'host' => 'smtp.qq.com',
    'port' => 465,          // 465=SSL 直连；587=STARTTLS
    'user' => 'you@qq.com', // 发件邮箱
    'pass' => '授权码',      // QQ 邮箱：设置→账户→开启 SMTP 后生成
    'from' => '',           // 留空 = 用 user
    'name' => 'LinTOS 应用商店',
],
```

没填 SMTP 时：页面顶部显示「提交功能暂未开放」，上传仍能入库（审核页看得到），只是不发邮件。

## nginx

只需要禁止访问 `data/` 和放开上传体积（已在
`/www/server/panel/vhost/nginx/extension/bbk.linit.cn/upload.conf`）：

```nginx
location ^~ /upload/data/ { deny all; return 404; }
client_max_body_size 4m;
```

⚠ **不要**给 `/upload/` 加 `location ^~ /upload/`：`^~` 匹配后不再走正则 location，
`.php` 会被当静态文件下发源码（实测会泄露 SMTP 授权码和审核令牌）。PHP 交给站点原有的
`location ~ \.php$`。

⚠ 手工改 `index.json` 后必须 `nginx -s reload`：open_file_cache 持有旧句柄，
原地重写会让设备端拉到空内容。程序写入用「写 `.tmp` → rename」。

## 数据表

| 表 | 说明 |
|---|---|
| `users` | 邮箱账号（一次验证码即注册，verified=1） |
| `codes` | 登录验证码（10 分钟有效、一次性） |
| `sessions` | 预留（实际用 PHP 原生 session） |
| `subs` | 每次提交：元信息、sha256、crc32、风险扫描结果、状态 pending/approved/rejected、驳回原因 |

## 上架规则

- 版本号**必须递增**：`ver` ≤ 商店当前版本会被直接拒收。
- 同名应用已有待审的一份时挡住第二次提交。
- 通过后覆盖 `bbk/pkgs/<应用名>.lua`，旧包备份到 `bbk/pkgs_backup/`，`index.json` 的
  `pkgs[].ver` 写新版本、`ver`（索引代次）+1。
- 分类与 `tools/store_build.py` 共用同一张归一化表（工具→软件、系统→标准、网络→运维），
  避免两处各写一套。
