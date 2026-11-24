#include "apppack.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "APPPACK";

/* 头部扫描上限: 图标 base64 约 684 字符 + 其余字段, 4KB 足够 */
#define HEAD_SCAN_BYTES 4096

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* 标准 base64 解码 (忽略 '='); 返回写入字节数, 超容量返回 -1 */
static int b64decode(const char *s, uint8_t *out, int cap) {
    int acc = 0, nbits = 0, n = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '=') break;
        int v = b64val((unsigned char)*p);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)((acc >> nbits) & 0xFF);
            acc &= (1 << nbits) - 1;
        }
    }
    return n;
}

esp_err_t apppack_parse(const char *path, apppack_meta_t *out)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    char *buf = (char *)malloc(HEAD_SCAN_BYTES + 1);
    if (!buf) { fclose(f); return ESP_FAIL; }
    size_t n = fread(buf, 1, HEAD_SCAN_BYTES, f);
    fclose(f);
    buf[n] = 0;

    bool found = false;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strncmp(line, "--@app ", 7) == 0) {
            found = true;
            /* ★★★ 2026-10-27 必修: 一行里可能有**多个** key=value。
             *   文档(docs/Lua应用开发.md)与 pack_app.py 产物都是这种写法:
             *     --@app name=QQ堂冒险 cat=游戏 desc=泡泡炸砖块 ver=1 icon=<b64>
             *   原来只取第 1 个 '=' 到行尾当值 ⇒ name 会变成
             *   "QQ堂冒险 cat=游戏 desc=..." 一整串(中文名超长/分类全错/版本为空)。
             *   老包还有"一行一个键"的写法(如 矢量地图), 必须同时兼容。
             *   做法: 逐个扫 key=value。值 = 等号后到下一个 " key=" 之前 ——
             *   不能简单按空格切, desc 里本来就有空格。 */
            char *p = line + 7;
            while (*p) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;
                char *ks = p;                       /* key 起点 */
                char *eq = strchr(ks, '=');
                if (!eq) break;
                size_t klen = (size_t)(eq - ks);
                bool bad = false;                   /* key 中间夹空格 ⇒ 不是键值对, 跳过 */
                for (size_t i = 0; i < klen; i++)
                    if (ks[i] == ' ' || ks[i] == '\t') { bad = true; break; }
                if (bad || klen == 0) { p = eq + 1; continue; }

                char *v = eq + 1;                   /* 值起点 */
                char *next = NULL;                  /* 下一个 " key=" 的空格位置 */
                for (char *q = v; *q; q++) {
                    if (*q != ' ' && *q != '\t') continue;
                    char *r = q + 1;
                    while (*r && *r != '=' && *r != ' ' && *r != '\t') r++;
                    if (*r == '=' && r > q + 1) { next = q; break; }
                }
                size_t vlen = next ? (size_t)(next - v) : strlen(v);
                while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ')) vlen--;

#define KEY_IS(k) (klen == sizeof(k) - 1 && strncmp(ks, k, klen) == 0)
                if (KEY_IS("name"))         snprintf(out->name, sizeof(out->name), "%.*s", (int)vlen, v);
                else if (KEY_IS("cat"))     snprintf(out->cat, sizeof(out->cat), "%.*s", (int)vlen, v);
                else if (KEY_IS("desc"))    snprintf(out->desc, sizeof(out->desc), "%.*s", (int)vlen, v);
                /* ★ 2026-10-27 修: 文档与 store_build.py 用的键是 **ver=**, 而这里原来只认
                 *   "version=" ⇒ 版本号从来没被读出来过(全为零) ⇒ 商店无法判断"有没有新版"。
                 *   两个键都收, 以 ver 为主(现有包都是 ver=)。 */
                else if (KEY_IS("ver") || KEY_IS("version"))
                    snprintf(out->version, sizeof(out->version), "%.*s", (int)vlen, v);
                /* ★ 2026-10-27: 统一元信息 —— 作者 / 日期 */
                else if (KEY_IS("author"))
                    snprintf(out->author, sizeof(out->author), "%.*s", (int)vlen, v);
                else if (KEY_IS("date"))
                    snprintf(out->date, sizeof(out->date), "%.*s", (int)vlen, v);
                /* ★ 2026-10-30: 构建时间 YYYYMMDDHHMM —— 商店按它判"有没有新版"。
                 *   只收纯数字(带分隔符的写法会让定长字典序比较失效), 非数字直接忽略。 */
                else if (KEY_IS("build")) {
                    bool digits = (vlen > 0);
                    for (size_t i = 0; i < vlen; i++)
                        if (v[i] < '0' || v[i] > '9') { digits = false; break; }
                    if (digits)
                        snprintf(out->build, sizeof(out->build), "%.*s", (int)vlen, v);
                }
                else if (KEY_IS("need")) {
                    /* 逗号分隔的能力声明 (目前只有 wifi); 大小写不敏感, 只在 vlen 内找 */
                    for (size_t i = 0; i + 4 <= vlen; i++)
                        if ((v[i] | 0x20) == 'w' && (v[i+1] | 0x20) == 'i' &&
                            (v[i+2] | 0x20) == 'f' && (v[i+3] | 0x20) == 'i') {
                            out->need_wifi = true; break;
                        }
                }
                else if (KEY_IS("icon")) {
                    /* 值后面可能紧跟别的键 ⇒ 必须先截断到 vlen 再解码 */
                    char ib[800];
                    size_t il = vlen < sizeof(ib) - 1 ? vlen : sizeof(ib) - 1;
                    memcpy(ib, v, il);
                    ib[il] = 0;
                    int got = b64decode(ib, out->icon, (int)APPPACK_ICON_BYTES);
                    out->has_icon = (got == (int)APPPACK_ICON_BYTES);
                    if (!out->has_icon && got >= 0)
                        ESP_LOGD(TAG, "icon 尺寸异常 %d B (期望 %u), 忽略",
                                 got, (unsigned)APPPACK_ICON_BYTES);
                }
#undef KEY_IS
                if (!next) break;
                p = next;
            }
        } else if (line[0] != '-' && line[0] != '\n' && line[0] != 0) {
            break;                      /* 头部落结束 → 后面是代码, 不再扫 */
        }
        line = nl ? nl + 1 : NULL;
    }
    free(buf);

    if (!found) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}
