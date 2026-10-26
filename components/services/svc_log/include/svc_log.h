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

/* 主循环每帧调用: 把缓冲内容追加写入日志文件 (仅 SD 挂载且缓冲非空时写盘) */
void svc_log_drain(void);

/* 暂停/恢复串口控制台输出 (仅文件日志), 供 USB 挂载为磁盘期间调用 */
void svc_log_console_suspend(bool suspend);

#ifdef __cplusplus
}
#endif

#endif /* LIN_TOS_SVC_LOG_H */
