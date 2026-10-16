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
            const char *kv = line + 7;
            const char *eq = strchr(kv, '=');
            if (eq) {
                size_t klen = (size_t)(eq - kv);
                const char *v = eq + 1;
                /* 去掉行尾 \r */
                size_t vlen = strlen(v);
                while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ')) vlen--;
#define KEY_IS(k) (klen == sizeof(k) - 1 && strncmp(kv, k, klen) == 0)
                if (KEY_IS("name"))         snprintf(out->name, sizeof(out->name), "%.*s", (int)vlen, v);
                else if (KEY_IS("cat"))     snprintf(out->cat, sizeof(out->cat), "%.*s", (int)vlen, v);
                else if (KEY_IS("desc"))    snprintf(out->desc, sizeof(out->desc), "%.*s", (int)vlen, v);
                else if (KEY_IS("version")) snprintf(out->version, sizeof(out->version), "%.*s", (int)vlen, v);
                else if (KEY_IS("need")) {
                    /* 逗号分隔的能力声明 (目前只有 wifi); 大小写不敏感, 只在 vlen 内找 */
                    for (size_t i = 0; i + 4 <= vlen; i++)
                        if ((v[i] | 0x20) == 'w' && (v[i+1] | 0x20) == 'i' &&
                            (v[i+2] | 0x20) == 'f' && (v[i+3] | 0x20) == 'i') {
                            out->need_wifi = true; break;
                        }
                }
                else if (KEY_IS("icon")) {
                    int got = b64decode(v, out->icon, (int)APPPACK_ICON_BYTES);
                    out->has_icon = (got == (int)APPPACK_ICON_BYTES);
                    if (!out->has_icon && got >= 0)
                        ESP_LOGD(TAG, "icon 尺寸异常 %d B (期望 %u), 忽略",
                                 got, (unsigned)APPPACK_ICON_BYTES);
                }
#undef KEY_IS
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
