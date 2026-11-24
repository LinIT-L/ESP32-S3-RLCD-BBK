/**
 * applock.h — 通用「应用锁」模块 (PIN 门禁)
 *
 * 任何页面想加锁, 三步:
 *   ① 进入页面时:  `s_authed = applock_gate(ctx, "fav", "收藏夹");`  // 返回 false 表示已切到锁屏
 *   ② 渲染时:      `if (applock_active()) { applock_render(ctx->lcd); return; }`
 *   ③ 按键/触摸时:  `if (applock_active()) { applock_action(ctx, a); return; }`
 *                   `if (applock_active()) { applock_touch(ctx, x, y); return; }`
 *   验证通过后 applock_active() 自动变 false, 页面照常走自己的逻辑。
 *
 * 存储/校验复用系统 keyvault (os_keyvault_*), 失败计数与锁定全局共享。
 */
#pragma once

#include <stdbool.h>
#include "os_app_api.h"   /* ui_ctx_t / os_action_t */
#include "st7305.h"       /* st7305_handle_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 进入页面时调用。
 * 返回 true  = 不需要锁 / 本次开机已解锁 / 未设置 PIN → 页面直接可用
 * 返回 false = 已切换到锁屏 → 页面本次不要渲染自己内容 */
bool applock_gate(ui_ctx_t *ctx, const char *app_id, const char *title);

/* 每帧轮询 (页面 poll 里调用): 检测手指按下/抬起, 触发一次重绘 */
void applock_tick(ui_ctx_t *ctx);

/* 锁屏是否正在显示 */
bool applock_active(void);

/* 锁屏绘制 (页面在 render 里最先调) */
void applock_render(st7305_handle_t *lcd);

/* 锁屏按键 / 触摸 (返回 true = 已消费) */
bool applock_action(ui_ctx_t *ctx, os_action_t a);
bool applock_touch(ui_ctx_t *ctx, int x, int y);

/* ---- 密钥与加解密 (解锁后有效; 未解锁返回 NULL / 0) ----
 * 其它模块要做"加密存储", 直接拿这个密钥走:
 *   size_t n = applock_encrypt(plain, plen, buf);   // buf 容量 ≥ plen + 28
 *   size_t m = applock_decrypt(buf, n, plain);      // 返回明文长度, 0=失败 */
const uint8_t *applock_key(void);
size_t applock_encrypt(const void *in, size_t n, uint8_t *out);
size_t applock_decrypt(const uint8_t *in, size_t n, void *out);

/* 清空本次开机的解锁记录 (切换用户/退出登录时用) */
void applock_forget_all(void);

#ifdef __cplusplus
}
#endif
