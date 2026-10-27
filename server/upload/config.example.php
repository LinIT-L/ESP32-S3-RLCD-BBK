<?php
/**
 * LinTOS 应用提交站 —— 配置模板（部署时复制成 config.php 再填）
 * 部署位置: /www/wwwroot/bbk.linit.cn/upload/
 *
 * ⚠ 本文件含 SMTP 授权码与审核令牌。PHP 文件本身不会把源码吐给浏览器，
 *   但请在 nginx 里同时禁止访问 data/（已经写进部署说明）。
 * ⚠ 改完立刻生效，不需要重启。
 */
return [

    'site_name'   => 'LinTOS 应用提交',
    'base_url'    => 'https://bbk.linit.cn/upload',

    /* 审核通知收件人（有新提交就发这里；一键通过/驳回的链接也发这里） */
    'admin_email' => '',            /* ← 审核通知收件人（必填） */

    /* 商店根目录：index.json 与 pkgs/ 就在这里，审核通过后直接改它 */
    'store_dir'   => '/www/wwwroot/bbk.linit.cn/bbk',

    /* 运行时数据（SQLite + 待审包），必须禁止外部访问 */
    'data_dir'    => __DIR__ . '/data',

    /* 审核面板令牌：浏览器打开 /upload/review.php?k=<这串> 即可，不用登录
     * 生成命令: php -r "echo bin2hex(random_bytes(16));" */
    'review_key'  => 'CHANGE_ME_REVIEW_KEY',

    /* SMTP：25 端口被阿里云封了，必须用 465(SSL) 或 587(STARTTLS)
     * pass 填的是「邮箱授权码」，不是邮箱登录密码 */
    'smtp' => [
        'host' => 'smtp.qq.com',
        'port' => 465,
        'user' => '',            // 发件邮箱，例如 you@qq.com
        'pass' => '',            // 授权码
        'from' => '',            // 留空 = 用 user
        'name' => 'LinTOS 应用商店',
    ],

    'limits' => [
        'max_bytes'      => 512 * 1024,  // 单个 .lua 上限 512KB
        'per_day_upload' => 5,           // 每账号每天最多提交几个
        'code_ttl'       => 600,         // 验证码有效期(秒)
        'code_interval'  => 60,          // 同一邮箱两次发码的最小间隔(秒)
    ],

    /* 商店允许的分类 —— 与 docs/Lua应用开发.md 保持一致 */
    'cats' => ['游戏', '软件', '标准', '运维', '学习'],
];
