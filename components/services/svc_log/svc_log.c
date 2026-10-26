// svc_log — 日志服务 (收拢 main.c 的日志缓存/落盘/串口暂停)
/* V1.0.90: 自动保存日志到 TF 卡 (排查随机重启/内存问题).
 * 通过 esp_log_set_vprintf 接管日志: 串口照常打印, 同时按行缓存进内部缓冲,
 * 再由主循环 svc_log_drain() 追加写入 /sdcard/data/log/bbk.log. 只在 SD 挂载后写盘,
 * 失败静默跳过; 文件超过上限后从 0 回绕重建, 避免无限膨胀.
 * s_log_skip 用于多任务/写盘期间的重入保护, 防止日志递归写盘死循环. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "sd_scan.h"
#include "svc_log.h"

#define LOG_SAVE_BUF       4096
#define LOG_SAVE_MAX       (1024UL * 1024)   /* 单个日志文件上限 1MB */
#define LOG_SAVE_PATH      "/sdcard/data/log/bbk.log"
EXT_RAM_BSS_ATTR static char          s_log_buf[LOG_SAVE_BUF];
static volatile size_t    s_log_len = 0;
static volatile bool      s_log_skip = false;
/* 多任务日志互斥 (蓝牙/音频/WiFi/主循环等任务都会打日志):
 * vprintf 钩子 与 drain 对 s_log_buf/s_log_len 的并发访问用互斥锁保护,
 * 避免 vsnprintf/memmove 交错写坏缓冲. 无 ISR 场景 (确认无 IRAM 日志), 普通 mutex 即可. */
static SemaphoreHandle_t  s_log_lock = NULL;
/* 挂载到电脑期间: 暂停串口控制台输出 (Serial/JTAG 已卸载, 只保留文件日志),
 * 从根源避免日志写向已失效串口导致崩溃. 见 usbh_msc 挂载流程. */
static volatile bool      s_log_console_suspend = false;

void svc_log_console_suspend(bool suspend) { s_log_console_suspend = suspend; }

static int svc_log_vprintf(const char *fmt, va_list arg) {
    int n = 0;
    /* [二分定位·测试版] 浅睡窗口内彻底静默: 连 vsnprintf 也跳过.
     * 原因: vsnprintf 的格式化串与实现位于 flash, 浅睡禁用 flash cache 瞬间若仍有
     * 任务在打印(屏保时主循环每秒 STACK 日志), 会 Cache disabled ... accessed → panic.
     * 之前只跳过串口输出, 文件缓冲的 vsnprintf 仍在执行 → 未根治. */
    if (s_log_console_suspend) return 0;
    /* 1) 串口照常输出 (保留原有监视能力); 挂载期间暂停避免写已卸载串口 */
    if (!s_log_console_suspend) {
        va_list arg_console;
        va_copy(arg_console, arg);
        n = vprintf(fmt, arg_console);
        va_end(arg_console);
    }
    /* 2) 只缓存到文件缓冲, 不阻塞串口输出 (互斥锁保护多任务并发写) */
    if (s_log_lock && xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (!s_log_skip && s_log_len < LOG_SAVE_BUF) {
            s_log_skip = true;
            va_list arg_file;
            va_copy(arg_file, arg);
            int k = vsnprintf(s_log_buf + s_log_len, LOG_SAVE_BUF - s_log_len,
                              fmt, arg_file);
            va_end(arg_file);
            if (k > 0) {
                size_t need = (size_t)k;
                if (need >= LOG_SAVE_BUF - s_log_len)
                    need = LOG_SAVE_BUF - s_log_len - 1;
                s_log_len += need;
            }
            s_log_skip = false;
        }
        xSemaphoreGive(s_log_lock);
    }
    return n;
}

void svc_log_init(void)
{
    s_log_lock = xSemaphoreCreateMutex();   /* 多任务日志互斥锁 (创建失败则跳过文件缓存) */
    esp_log_set_vprintf(&svc_log_vprintf);
}

/* 主循环每帧调用: 把缓冲内容追加写入日志文件. 仅 SD 挂载且缓冲非空时写盘. */
void svc_log_drain(void)
{
    if (!sd_is_mounted()) return;          /* 无卡/未挂载: 保留缓冲稍后再写 */
    if (!s_log_lock) return;
    if (xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;   /* 写盘竞争让位 */
    s_log_skip = true;
    size_t len = s_log_len;
    if (len == 0) { s_log_skip = false; xSemaphoreGive(s_log_lock); return; }
    /* 超限回绕: 重建文件保留最新内容 */
    struct stat st;
    if (stat(LOG_SAVE_PATH, &st) == 0 &&
        st.st_size + (off_t)len > (off_t)LOG_SAVE_MAX) {
        remove(LOG_SAVE_PATH);
    }
    FILE *f = fopen(LOG_SAVE_PATH, "ab");
    if (f) {
        fwrite(s_log_buf, 1, len, f);
        fclose(f);
    }
    /* 移走已写部分, 保留写盘期间新追加的日志 */
    if (s_log_len >= len) s_log_len -= len; else s_log_len = 0;
    if (s_log_len && len) memmove(s_log_buf, s_log_buf + len, s_log_len);
    s_log_skip = false;
    xSemaphoreGive(s_log_lock);
}
