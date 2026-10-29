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
#include <errno.h>          /* stat() 失败原因: ENOENT=文件不存在(正常), 其余=状态不可信 */
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
    /* ★★★ 2026-10-29 修正 (实现与注释矛盾): 原代码第一句是
     *     `if (s_log_console_suspend) return 0;`
     *   —— 直接返回, 连下面的**文件缓冲也一起跳过了**。
     *   但这个开关的语义 (见本文件第 29 行注释 + usbh_msc_sdspi.c:34 的调用注释) 是:
     *     "暂停串口控制台输出 (Serial/JTAG 已卸载), **只保留文件日志**"
     *   ⇒ 挂载到电脑期间日志被**全部丢弃**, 事后完全查不出挂载过程发生了什么。
     *   现在按注释语义执行: **只跳过串口输出, 文件缓冲照写**。
     *
     *   ⚠ 原注释担心的"浅睡禁用 flash cache 时 vsnprintf 会 panic"依然成立, 但那是另一个
     *     场景, 不能用同一个开关表达(否则 USB 挂载语义被连带破坏)。若将来真需要
     *     "连格式化都停"的硬暂停, 应另加独立接口(如 svc_log_hard_pause), 在调用处显式二选一。
     *     当前全仓没有任何浅睡路径调用本函数(只有 usbh_msc / usb_net 两个 USB 挂载场景)。
     */
    /* 1) 串口输出 (挂载期间暂停, 避免写已卸载的 Serial/JTAG) */
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
/* ═══════════════════════════════════════════════════════════════════════════
 * 把 RAM 缓冲追加写进 bbk.log —— svc_log_drain() 与关机前刷新**共用的唯一实现**。
 *
 * 2026-10-29 重写原因: 原来 svc_log_drain() 不检查 fopen/fwrite/fclose 的任何返回值,
 *   而且无论成败都把缓冲"消费"掉 ⇒ 写盘失败时日志直接消失, 连痕迹都没有;
 *   更糟的是它先 remove(旧文件) 再 fopen(新文件), 一旦 remove 成功而 fopen 失败,
 *   **1MB 历史 + 本次数据同时消失**。
 *
 * 失败处理按"能确定多少"分级(不引入全局偏移状态):
 *   · SD 未挂载 / 未初始化 / 锁等待超时  → 从未尝试, 缓冲**一字不动**
 *   · 文件已达 1MB 上限                  → 从未尝试, 缓冲**一字不动**; **绝不 remove 旧日志**
 *   · fopen 失败                         → 确定没写, 缓冲**一字不动**
 *   · fwrite 短写 (w < len)              → **精确知道写了 w 字节**: memmove 移走这 w 字节,
 *                                          保留没写的部分(下次接着写, 不会重复)
 *   · 全写但 fclose 失败                 → ⚠ 文件状态**不可知**:
 *                                          fwrite 只证明数据进了 libc 的 FILE 缓冲,
 *                                          fclose 才触发 f_flush→f_sync→disk_write。
 *                                          这里按"已写"处理并清空, **优先避免整段重复追加**;
 *                                          代价是**这一次的日志可能丢失(≤4KB)**。
 *                                          —— 重复会让排查彻底跑偏, 少一段只是信息不全。
 *   · 全部成功                           → 清空
 *
 * ⚠ 本函数内部**禁止调用 ESP_LOGx**: 那会重进 svc_log_vprintf 并抢同一把锁。
 *   失败只通过返回值反映, 由调用方在函数返回后打日志。
 * ⚠ 必须在 sd_unmount() 之前调用 —— 卸载后 sd_is_mounted()==false, 会直接返回。
 * ═══════════════════════════════════════════════════════════════════════════ */
esp_err_t svc_log_flush_once(void)
{
    if (!s_log_lock)      return ESP_ERR_INVALID_STATE;
    if (!sd_is_mounted()) return ESP_ERR_INVALID_STATE;   /* 无卡/未挂载: 不碰 FS */

    if (xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(50)) != pdTRUE)
        return ESP_ERR_TIMEOUT;                           /* 拿不到锁: 不阻塞调用方 */

    s_log_skip = true;
    esp_err_t ret = ESP_OK;
    size_t len = s_log_len;
    if (len == 0) goto done;

    /* ---- 容量检查: 状态不可信就拒绝; 到顶只拒绝, 绝不删旧文件 ---- */
    {
        struct stat st;
        off_t cur = 0;
        if (stat(LOG_SAVE_PATH, &st) == 0) {
            cur = st.st_size;
            if (cur < 0 || cur > (off_t)LOG_SAVE_MAX + (off_t)LOG_SAVE_BUF)
                cur = (off_t)LOG_SAVE_MAX;                /* 大小异常: 按已满处理 */
        } else if (errno != ENOENT) {
            ret = ESP_FAIL;                               /* 无法确认文件状态: 不写 */
            goto done;
        }
        if (cur + (off_t)len > (off_t)LOG_SAVE_MAX) {
            ret = ESP_ERR_INVALID_SIZE;                   /* 到顶: 拒绝, 旧日志原样保留 */
            goto done;
        }
    }

    /* ---- 写入 ---- */
    {
        FILE *f = fopen(LOG_SAVE_PATH, "ab");
        if (!f) {
            ret = ESP_FAIL;                               /* 确定没写: 缓冲保留 */
            goto done;
        }
        size_t w = fwrite(s_log_buf, 1, len, f);
        int    cr = fclose(f);

        if (w < len) {
            /* 短写: 精确移走已写出的 w 字节, 剩余保留(下次续写, 不重复) */
            if (w > 0) {
                size_t keep = len - w;
                memmove(s_log_buf, s_log_buf + w, keep);
                s_log_len = keep;
            }
            ret = ESP_FAIL;
        } else if (cr != 0) {
            /* 全写但 fclose 失败: 文件状态不可知 ⇒ 按已写处理(避免整段重复), 可能丢 */
            s_log_len = 0;
            ret = ESP_FAIL;
        } else {
            s_log_len = 0;                                /* 完全成功 */
            ret = ESP_OK;
        }
    }

done:
    s_log_skip = false;
    xSemaphoreGive(s_log_lock);
    return ret;
}

/* 主循环/手动 logsave 用的旧接口: 语义是"尽力而为", 由 flush_once 承担全部实现,
 * 保证两个入口只有**一套**写入规则(不会出现 drain 与本接口规则分叉)。 */
void svc_log_drain(void) { (void)svc_log_flush_once(); }
