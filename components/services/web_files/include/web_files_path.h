/**
 * web_files_path.h — /files/ 的路径与参数规整（纯 C，无 ESP 依赖，可宿主机单测）。
 *
 * 为什么单独抽出来：路径穿越是这套功能唯一的安全红线，必须能用测试证明，
 * 不能只靠"代码走查"。web_files.c 与 tests/web_files 用的是**同一份**实现。
 */
#ifndef WEB_FILES_PATH_H
#define WEB_FILES_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef WF_ROOT
#define WF_ROOT     "/sdcard"
#endif
#ifndef WF_PATH_MAX
#define WF_PATH_MAX 200
#endif

/* 取 %XX 的十六进制值；非 hex 返回 -1。 */
static inline int wf_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * percent 解码（'+' 当普通字符 —— encodeURIComponent 会把 '+' 编成 %2B）。
 * 只解 %XX，不把 '+' 变空格，避免文件名里的加号被悄悄改掉。
 */
static inline void wf_urldecode(const char *in, char *out, size_t cap)
{
    size_t k = 0;
    if (!out || cap == 0) return;
    while (in && *in && k + 1 < cap) {
        if (in[0] == '%' && in[1] && in[2]) {
            int hi = wf_hex(in[1]), lo = wf_hex(in[2]);
            if (hi >= 0 && lo >= 0) { out[k++] = (char)((hi << 4) | lo); in += 3; continue; }
        }
        out[k++] = *in++;
    }
    out[k] = 0;
}

/**
 * 路径归一化：输入可以是 ""、"/"、"a/b"、"/sdcard/a/b"，输出一定是 /sdcard 下的绝对路径。
 *
 * 拒绝：`..` 段、控制字符、反斜杠、超出 buffer、"/sdcardXxx" 这种伪装前缀。
 * `.` 与空段被吃掉。这是所有文件操作唯一的入口 —— 不要在别处另写一套判断。
 */
static inline bool wf_norm(const char *in, char *out, size_t cap)
{
    if (!out || cap < sizeof(WF_ROOT) + 1) return false;
    const char *p = in ? in : "";
    if (strncmp(p, WF_ROOT, sizeof(WF_ROOT) - 1) == 0) {
        p += sizeof(WF_ROOT) - 1;
        if (*p == '/')      p++;
        else if (*p != 0)   return false;   /* "/sdcardXxx" 不是我们的路径 */
    }
    size_t n = sizeof(WF_ROOT) - 1;
    memcpy(out, WF_ROOT, n);
    out[n] = 0;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') {
            unsigned char c = (unsigned char)*p;
            if (c < 0x20 || c == '\\') return false;
            p++;
        }
        size_t len = (size_t)(p - seg);
        if (len == 0) continue;
        if (len == 1 && seg[0] == '.') continue;                          /* 忽略 "." */
        if (len == 2 && seg[0] == '.' && seg[1] == '.') return false;     /* 明确拒绝 ".." */
        if (n + 1 + len >= cap) return false;
        out[n++] = '/';
        memcpy(out + n, seg, len);
        n += len;
        out[n] = 0;
    }
    return true;
}

/* 目录 + 名字 拼成完整路径再归一化（名字里允许带子目录，文件夹上传将来要用）。 */
static inline bool wf_join(const char *dir, const char *name, char *out, size_t cap)
{
    char tmp[WF_PATH_MAX * 2];
    if (!name || !name[0]) return false;
    int n = snprintf(tmp, sizeof(tmp), "%s/%s", dir ? dir : WF_ROOT, name);
    if (n <= 0 || n >= (int)sizeof(tmp)) return false;
    return wf_norm(tmp, out, cap);
}

#endif /* WEB_FILES_PATH_H */
