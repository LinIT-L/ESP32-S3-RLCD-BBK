/**
 * diag_core.c — 见 diag_core.h。纯 libc 实现, 可在主机端单元测试。
 */
#include "diag_core.h"
#include <stdlib.h>
#include <string.h>

int dc_pct_at(const int *score, int n, int i, int *out_total)
{
    if (!score || n <= 0 || i < 0 || i >= n) return 0;
    int total = 0;
    for (int k = 0; k < n; k++) if (score[k] > 0) total += score[k];
    if (out_total) *out_total = total;
    if (total <= 0) return 0;
    int sc = (score[i] > 0) ? score[i] : 0;
    return sc * 100 / total;
}

void dc_normalize(const int *score, int n, int *pct)
{
    if (!score || !pct || n <= 0) return;
    int total = 0;
    for (int k = 0; k < n; k++) if (score[k] > 0) total += score[k];
    for (int i = 0; i < n; i++) {
        if (total <= 0) { pct[i] = 0; continue; }
        int sc = (score[i] > 0) ? score[i] : 0;
        pct[i] = sc * 100 / total;
    }
}

void dc_sort_desc(const int *score, int n, int *idx)
{
    if (!score || !idx || n <= 0) return;
    for (int i = 0; i < n; i++) idx[i] = i;
    /* 严格大于才交换 → 同分保持原索引顺序 (稳定) */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (score[idx[j]] > score[idx[i]]) {
                int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            }
}

int dc_parse_rc(const char *buf, int *ok_n, int *fail_n)
{
    int ok = 0, bad = 0, n = 0;
    if (buf) {
        const char *p = buf;
        while ((p = strstr(p, "RC=")) != NULL) {
            /* 只认行首或空白后的 RC=, 避免把 "XRC=1" 之类误匹配 */
            if (p == buf || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\r') {
                int code = atoi(p + 3);
                n++;
                if (code == 0) ok++; else bad++;
            }
            p += 3;
        }
    }
    if (ok_n) *ok_n = ok;
    if (fail_n) *fail_n = bad;
    return n;
}

int dc_parse_event_ids(const char *buf, int *ids, int *cnt, int max)
{
    if (!buf || !ids || !cnt || max <= 0) return 0;
    int n = 0;
    const char *p = buf;
    while ((p = strstr(p, "Event ID:")) != NULL) {
        /* 必须是行首字段 (允许前置空白), 避免正文里出现同样字样 */
        const char *ls = p;
        while (ls > buf && ls[-1] != '\n') ls--;
        bool line_start = true;
        for (const char *q = ls; q < p; q++)
            if (*q != ' ' && *q != '\t' && *q != '\r') { line_start = false; break; }

        const char *v = p + 9;
        while (*v == ' ' || *v == '\t') v++;
        int id = atoi(v);
        p += 9;
        if (!line_start || id <= 0) continue;

        int k = 0;
        for (; k < n; k++) if (ids[k] == id) { cnt[k]++; break; }
        if (k == n && n < max) { ids[n] = id; cnt[n] = 1; n++; }
    }
    return n;
}

int dc_count_lines(const char *buf)
{
    if (!buf) return 0;
    int lines = 1;
    for (const char *p = buf; *p; p++) if (*p == '\n') lines++;
    return lines;
}

/* ---- 采集链分节解析 ---- */

/* 大小写无关的子串查找 (Windows 路径大小写由目标机实际目录名决定, 不能假定) */
static bool dc_has_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) return false;
    for (const char *p = hay; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return true;
    }
    return false;
}

/* 后缀匹配 (大小写无关): 判断这一行是不是 .txt 文件 */
static bool dc_ends_ci(const char *s, const char *suffix)
{
    size_t n = s ? strlen(s) : 0, m = suffix ? strlen(suffix) : 0;
    if (n < m) return false;
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/* 这些文件"空"是正常的 (没有代理/没有转储/没有启动项), 不能算采集失败。
 * 其它文件空了 = 命令跑了但没拿到数据, 要计入 empty。 */
/* 这些文件"空"是**正常**的, 不能算采集失败 —— 每一项都要说清为什么:
 *   Proxy.txt         : 没配代理时 `reg query ... | findstr Proxy` 无输出 (绝大多数机器)
 *   Minidump.txt      : 从没蓝屏过就没有 C:\Windows\Minidump 内容
 *   KernelReports.txt : LiveKernelReports 同上 (只有"内核报告但不重启"的故障才留)
 *   Startup.txt       : Win32_StartupCommand 查不到自启动项时 Format-List 一行都不输出
 * 白名单是"允许为空", 不是"忽略": 这些文件**仍然计入 files**, 只是不累加 empty。
 * ⛔ 别把 CPU/RAM/Disk/OS/Users 这类加进来 —— 它们空了就是真没采到。 */
static bool dc_file_may_be_empty(const char *name)
{
    static const char *const ok[] = { "Proxy.txt", "Minidump.txt", "KernelReports.txt", "Startup.txt" };
    if (!name || !name[0]) return false;
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (dc_has_ci(name, ok[i])) return true;
    return false;
}

int dc_parse_chains(const char *buf, dc_chain_t out[DC_CHAIN_N])
{
    if (out) memset(out, 0, sizeof(dc_chain_t) * DC_CHAIN_N);
    if (!buf || !out) return 0;

    /* 只认 "\Logs\<类>\" —— 单认 "\Sys\" 会被别的路径命中 */
    static const char *const key[DC_CHAIN_N] = {
        "\\logs\\sys\\", "\\logs\\log\\", "\\logs\\net\\", "\\logs\\acc\\"
    };

    int sections = 0, cur = -1;
    /* 当前分节的累积状态: 用来判断"这个文件是不是空的" */
    char cur_name[64] = "";
    bool cur_is_txt = false;
    long cur_nonspace = 0;
    bool cur_bom_done = false;      /* 本节第一个正文行是否已做过 BOM 剥离 */

    const char *p = buf;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char lbuf[300];
        size_t cp = (len < sizeof(lbuf) - 1) ? len : sizeof(lbuf) - 1;
        memcpy(lbuf, p, cp);
        lbuf[cp] = '\0';

        if (strncmp(lbuf, "##### ", 6) == 0) {
            /* 结算上一节 */
            if (cur_is_txt && cur_nonspace == 0 && !dc_file_may_be_empty(cur_name))
                out[cur].empty++;

            sections++;
            cur = -1;
            cur_is_txt = false;
            cur_nonspace = 0;
            cur_bom_done = false;
            cur_name[0] = '\0';
            for (int i = 0; i < DC_CHAIN_N; i++)
                if (dc_has_ci(lbuf, key[i])) { cur = i; break; }
            if (cur >= 0) {
                size_t n = strlen(lbuf);
                while (n && (lbuf[n - 1] == '\r' || lbuf[n - 1] == ' ' || lbuf[n - 1] == '\t'))
                    lbuf[--n] = '\0';
                if (dc_ends_ci(lbuf, ".txt") && !dc_has_ci(lbuf, "\\rc.txt")) {
                    out[cur].files++;
                    cur_is_txt = true;
                    /* 只留文件名, 用于空文件白名单判断 */
                    const char *slash = lbuf;
                    for (const char *q = lbuf; *q; q++) if (*q == '\\' || *q == '/') slash = q + 1;
                    size_t m = strlen(slash);
                    if (m >= sizeof(cur_name)) m = sizeof(cur_name) - 1;
                    memcpy(cur_name, slash, m);
                    cur_name[m] = '\0';
                }
            }
        } else if (cur >= 0) {
            const char *q = lbuf;
            while (*q == ' ' || *q == '\t') q++;
            if (strncmp(q, "RC=", 3) == 0) {
                out[cur].rc_n++;
                if (atoi(q + 3) != 0) out[cur].rc_fail++;
            }
            if (cur_is_txt) {           /* 统计非空白字符: 判断"文件在但没内容" */
                const char *r = lbuf;
                /* ★ 2026-10-30 第三轮: **UTF-8 BOM 不算正文**。
                 *   目标机若用某些工具写出 "EF BB BF" + 空内容, 原来会把 BOM 的 3 个字节
                 *   当成"有内容", 于是空文件被当成功, 反过来又说"文件齐了但没数据"。
                 *   只在本节**第一个正文行**剥一次: BOM 只可能在文件开头。
                 *   ⛔ 不要按字符集整体转码 —— 这里只做字节级"跳过前 3 字节",
                 *      多字节汉字的后续字节一个都不动, 所以不会破坏 UTF-8。 */
                if (!cur_bom_done) {
                    cur_bom_done = true;
                    if ((unsigned char)r[0] == 0xEF && (unsigned char)r[1] == 0xBB &&
                        (unsigned char)r[2] == 0xBF) r += 3;
                }
                /* 空白判定只认 ASCII 控制字符: 汉字三个字节都 >= 0x80,
                 * 不会被误判成空白 (这是"多字节不被破坏"的根本原因)。 */
                for (; *r; r++)
                    if (*r != ' ' && *r != '\t' && *r != '\r' && *r != '\n' &&
                        *r != '\v' && *r != '\f')
                        cur_nonspace++;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    /* 结算最后一节 */
    if (cur >= 0 && cur_is_txt && cur_nonspace == 0 && !dc_file_may_be_empty(cur_name))
        out[cur].empty++;
    return sections;
}

int dc_combine(int support, int oppose)
{
    int s = support - oppose / 2;
    return (s < 0) ? 0 : s;
}

int dc_clamp(int score, int cap)
{
    if (score < 0) return 0;
    if (score > cap) return cap;
    return score;
}

dc_chain_st_t dc_chain_status(const dc_chain_t *c)
{
    if (!c) return DC_CHAIN_UNKNOWN;
    /* ① 没有任何"这条链跑完了"的证据 → 未知 (不能默认成功) */
    if (c->rc_n == 0) return DC_CHAIN_UNKNOWN;
    /* ② 一个文件都没有: 命令可能返回 0 却没产出 (WMIC 被移除的典型表现) */
    if (c->files <= 0) return DC_CHAIN_EMPTY;
    /* ③ 没给"应有几个文件"时, 只能退回看退出码 */
    if (c->expect <= 0) return (c->rc_fail > 0) ? DC_CHAIN_FAIL : DC_CHAIN_OK;
    /* ④ 给了预期: 以**有内容的文件数**为准 (比链尾退出码硬)。
     *   空文件不算采到 —— "命令跑了但什么都没产出"和"没跑"一样不能用。
     *   rc 非 0 但文件齐 → 多半是链尾"给人看"的显示命令没匹配到 (findstr 返回 1),
     *   不是采集失败; 返回码另外在汇总行里显示, 不在这里制造误判。 */
    if (c->files - c->empty < c->expect) return DC_CHAIN_PARTIAL;
    return DC_CHAIN_OK;
}

bool dc_parse_end(const char *buf, int *files)
{
    if (files) *files = 0;
    if (!buf) return false;
    /* 取**最后一次**出现: 万一某份日志正文里也写了 "##### END" (几乎不可能, 但不排除),
     * 以最后一条为准 —— 真正的收尾标志永远在最后。 */
    const char *last = NULL;
    for (const char *p = buf; (p = strstr(p, "##### END")) != NULL; p += 9) last = p;
    if (!last) return false;
    const char *v = last + 9;
    while (*v == ' ' || *v == '\t') v++;
    if (files) *files = atoi(v);
    return true;
}

/* "##### SKIP <路径>": 目标机读这个文件时抛了异常 (被独占打开/权限/路径过长)。
 * 与"文件没生成"必须分开: 后者是采集命令没产出, 前者是文件在但拿不回来。 */
int dc_count_skips(const char *buf)
{
    if (!buf) return 0;
    int n = 0;
    const char *p = buf;
    while ((p = strstr(p, "##### SKIP")) != NULL) { n++; p += 10; }
    return n;
}

dc_ld_state_t dc_ld_state(const dc_ld_ctx_t *c)
{
    if (!c) return DC_LD_USB_OFF;
    if (c->usb_busy)     return DC_LD_BOOT;        /* 启动中优先: 这期间按钮都该忽略 */
    if (c->sending)      return DC_LD_SEND;
    if (c->collecting)   return DC_LD_RUN;
    if (!c->usb_running) return DC_LD_USB_OFF;
    if (c->dev_lost || c->xfer_fail) return DC_LD_FAIL;   /* 中断优先于"未连接" */
    if (!c->pc_connected) return DC_LD_NO_PC;      /* USB 起了 ≠ 电脑连上了 */
    if (c->analyzed)     return DC_LD_DONE;
    return DC_LD_READY;
}

/* ---- 采集链"应有文件"清单: expect 数字的唯一依据 ---- */
static const char *const s_f_sys[] = {
    "name.txt", "CPU.txt", "RAM.txt", "Board.txt", "Disk.txt",
    "OS.txt", "SystemInfo.txt", "Startup.txt", "Software.txt",
};
static const char *const s_f_log[] = {
    "SystemErrors.txt", "AppErrors.txt", "Critical.txt",
    "Minidump.txt", "KernelReports.txt", "Drivers.txt",
};
static const char *const s_f_net[] = {
    "IP.txt", "DNS.txt", "ARP.txt", "Route.txt", "Ports.txt",
    "Firewall.txt", "Shares.txt", "Proxy.txt", "Hosts.txt",
};
static const char *const s_f_acc[] = {
    "Users.txt", "Admins.txt", "Tasks.txt", "RDP.txt", "Shares.txt",
};

static const char *const *s_files[DC_CHAIN_N] = { s_f_sys, s_f_log, s_f_net, s_f_acc };
static const int s_files_n[DC_CHAIN_N] = {
    (int)(sizeof(s_f_sys) / sizeof(s_f_sys[0])),
    (int)(sizeof(s_f_log) / sizeof(s_f_log[0])),
    (int)(sizeof(s_f_net) / sizeof(s_f_net[0])),
    (int)(sizeof(s_f_acc) / sizeof(s_f_acc[0])),
};

int dc_chain_expect_len(int chain)
{
    if (chain < 0 || chain >= DC_CHAIN_N) return 0;
    return s_files_n[chain];
}

const char *dc_chain_expect_name(int chain, int idx)
{
    if (chain < 0 || chain >= DC_CHAIN_N) return NULL;
    if (idx < 0 || idx >= s_files_n[chain]) return NULL;
    return s_files[chain][idx];
}

dc_xfer_t dc_xfer_state(unsigned since_last_rx_ms, unsigned elapsed_ms,
                        unsigned idle_limit_ms, unsigned total_limit_ms)
{
    if (total_limit_ms && elapsed_ms >= total_limit_ms) return DC_XFER_TOTAL_TIMEOUT;
    if (idle_limit_ms && since_last_rx_ms >= idle_limit_ms) return DC_XFER_IDLE_TIMEOUT;
    return DC_XFER_OK;
}
