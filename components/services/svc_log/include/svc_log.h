#ifndef LIN_TOS_SVC_LOG_H
#define LIN_TOS_SVC_LOG_H

// svc_log — 日志服务 (收拢 main.c 的日志缓存/落盘/串口暂停)
// 经 esp_log_set_vprintf 接管日志: 串口照常打印, 同时按行缓存进内部缓冲,
// 再由主循环 svc_log_drain() 追加写入 /sdcard/data/log/bbk.log.

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化日志引擎 (创建互斥锁 + 注册 vprintf 钩子), 开机时调用一次 */
void svc_log_init(void);

/* 主循环每帧调用: 把缓冲内容追加写入日志文件 (仅 SD 挂载且缓冲非空时写盘)。
 * 语义 = "尽力而为", 不返回结果。★ 2026-10-29: 实现已统一到 svc_log_flush_once(),
 * 两个入口共用**同一套**写入规则, 不会各自维护缓冲区消费逻辑。 */
void svc_log_drain(void);

/* ★ 2026-10-29 新增: 关机/深睡前调用**一次**, 把 RAM 缓冲落盘。
 * 与 svc_log_drain() 是同一实现, 区别只是**返回结果**供调用方判断与记录。
 *   ESP_OK                全部写入 + fclose 成功 ⇒ 数据已提交到 SD 驱动
 *   ESP_ERR_INVALID_STATE 未初始化 或 SD 未挂载 (从未尝试, 缓冲保留)
 *   ESP_ERR_TIMEOUT       等锁 50ms 失败       (从未尝试, 缓冲保留)
 *   ESP_ERR_INVALID_SIZE  日志文件已达 1MB 上限 (拒绝写入, **不删旧日志**; 缓冲保留)
 *   ESP_FAIL              fopen 失败 (确定没写, 缓冲保留) /
 *                         fwrite 短写 (已按实际写出的字节移走, 剩余保留) /
 *                         fclose 失败 (⚠ 文件状态不可知, 按已写处理 ⇒ 这一段可能丢失)
 * 不重试、不做回绕、不重新挂载 SD。
 * ⚠ 必须在 sd_unmount() 之前调用 —— 卸载后 sd_is_mounted()==false 会直接返回。 */
esp_err_t svc_log_flush_once(void);

/* 暂停/恢复串口控制台输出 (仅文件日志), 供 USB 挂载为磁盘期间调用 */
void svc_log_console_suspend(bool suspend);

#ifdef __cplusplus
}
#endif

#endif /* LIN_TOS_SVC_LOG_H */
