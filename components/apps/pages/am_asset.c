/**
 * am_asset.c — 商店「资源包」安装器实现 (stored ZIP 解包)
 *
 * 走中央目录 (End of Central Directory → Central Directory Entry) 找每个文件,
 * 再按 local header 偏移定位数据区。**只处理 method=0 (stored)**:
 *   · 不需要 inflate 库 ⇒ 固件零新依赖;
 *   · 解压不占额外内存 (4KB 缓冲逐块搬运), 26MB 的资源包也不会 OOM。
 *
 * 安全: 拒绝绝对路径和带 ".." 的条目, 只允许写到 target_dir 之下。
 */
#include "am_asset.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define TAG "AM_ASSET"

#define ZIP_EOCD_SIG   0x06054b50u
#define ZIP_CD_SIG     0x02014b50u
#define ZIP_LOC_SIG    0x04034b50u
#define EOCD_SCAN_MAX  (64 * 1024)
#define COPY_BUF       4096

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 逐级建目录 (mkdir 已存在会失败, 忽略) */
static void mkdirs(const char *path)
{
    char tmp[256];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return;
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(tmp, 0777);
        *p = '/';
    }
    mkdir(tmp, 0777);
}

/* 目录内某个路径的父目录 */
static void parent_of(const char *path, char *out, size_t cap)
{
    const char *s = strrchr(path, '/');
    if (!s) { if (cap) out[0] = 0; return; }
    size_t n = (size_t)(s - path);
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

static bool copy_range(FILE *fi, long off, uint32_t len, const char *dst)
{
    char dir[256];
    parent_of(dst, dir, sizeof(dir));
    if (dir[0]) mkdirs(dir);

    if (fseek(fi, off, SEEK_SET) != 0) return false;
    FILE *fo = fopen(dst, "wb");
    if (!fo) {
        ESP_LOGE(TAG, "写文件失败: %s", dst);
        return false;
    }
    static uint8_t buf[COPY_BUF];
    uint32_t left = len;
    bool ok = true;
    while (left > 0) {
        size_t want = left > COPY_BUF ? COPY_BUF : left;
        size_t got = fread(buf, 1, want, fi);
        if (got == 0) { ok = false; break; }
        if (fwrite(buf, 1, got, fo) != got) { ok = false; break; }
        left -= (uint32_t)got;
    }
    fclose(fo);
    if (!ok) {
        ESP_LOGE(TAG, "写出不完整: %s", dst);
        remove(dst);
    }
    return ok;
}

bool am_asset_install_cb(const char *zip_path, const char *target_dir,
                         am_asset_prog_cb_t cb)
{
    if (!zip_path || !target_dir || !target_dir[0]) return false;

    FILE *fi = fopen(zip_path, "rb");
    if (!fi) {
        ESP_LOGE(TAG, "打不开资源包: %s", zip_path);
        return false;
    }
    fseek(fi, 0, SEEK_END);
    long fsz = ftell(fi);
    if (fsz < 22) { fclose(fi); ESP_LOGE(TAG, "文件太小, 不是 zip"); return false; }

    /* ---- 找 EOCD (从尾部往前扫) ---- */
    long scan = fsz < EOCD_SCAN_MAX ? fsz : EOCD_SCAN_MAX;
    uint8_t hdr[22];
    long eocd = -1;
    for (long i = fsz - 22; i >= fsz - scan; i--) {
        if (fseek(fi, i, SEEK_SET) != 0) break;
        if (fread(hdr, 1, 4, fi) != 4) break;
        if (rd32(hdr) == ZIP_EOCD_SIG) { eocd = i; break; }
    }
    if (eocd < 0) { fclose(fi); ESP_LOGE(TAG, "找不到 EOCD (不是 zip?)"); return false; }
    if (fseek(fi, eocd, SEEK_SET) != 0 || fread(hdr, 1, 22, fi) != 22) { fclose(fi); return false; }
    uint16_t total   = rd16(hdr + 10);
    uint32_t cd_off  = rd32(hdr + 16);
    ESP_LOGI(TAG, "资源包 %s: %ld 字节, %u 个条目", zip_path, fsz, (unsigned)total);

    mkdirs(target_dir);

    char dst[320];
    int done = 0, skipped = 0, failed = 0;
    long cd_pos = (long)cd_off;
    for (uint16_t k = 0; k < total; k++) {
        uint8_t ce[46];
        if (fseek(fi, cd_pos, SEEK_SET) != 0 || fread(ce, 1, 46, fi) != 46) { failed++; break; }
        if (rd32(ce) != ZIP_CD_SIG) { failed++; break; }
        uint16_t method  = rd16(ce + 10);
        uint32_t csize   = rd32(ce + 20);
        uint16_t nlen    = rd16(ce + 28);
        uint16_t elen    = rd16(ce + 30);
        uint16_t clen    = rd16(ce + 32);
        uint32_t loff    = rd32(ce + 42);

        char name[220];
        uint16_t nl = nlen < sizeof(name) - 1 ? nlen : (uint16_t)(sizeof(name) - 1);
        if (fseek(fi, cd_pos + 46, SEEK_SET) != 0 || fread(name, 1, nl, fi) != nl) { failed++; break; }
        name[nl] = 0;
        cd_pos += 46 + nlen + elen + clen;

        if (!name[0] || name[nl - 1] == '/') { skipped++; continue; }   /* 目录条目 */
        if (name[0] == '/' || strstr(name, "..")) {                     /* 路径穿越 */
            ESP_LOGW(TAG, "拒绝可疑路径: %s", name);
            skipped++;
            continue;
        }
        if (method != 0) {
            ESP_LOGW(TAG, "只支持不压缩包, 跳过 method=%u: %s", (unsigned)method, name);
            skipped++;
            continue;
        }

        /* local header → 数据偏移 */
        uint8_t lh[30];
        if (fseek(fi, (long)loff, SEEK_SET) != 0 || fread(lh, 1, 30, fi) != 30) { failed++; continue; }
        if (rd32(lh) != ZIP_LOC_SIG) { failed++; continue; }
        long data_off = (long)loff + 30 + rd16(lh + 26) + rd16(lh + 28);

        snprintf(dst, sizeof(dst), "%s/%s", target_dir, name);
        if (!copy_range(fi, data_off, csize, dst)) { failed++; continue; }
        done++;
        if (cb) cb((int)((long)(k + 1) * 1000 / (long)(total ? total : 1)));
    }
    fclose(fi);

    ESP_LOGI(TAG, "解包完成: 成功 %d / 跳过 %d / 失败 %d → %s", done, skipped, failed, target_dir);
    return (failed == 0 && done > 0);
}

/* 递归删除目录 */
static bool rm_tree(const char *dir, int depth)
{
    if (depth > 8) return false;
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    char p[320];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) rm_tree(p, depth + 1);
        else remove(p);
    }
    closedir(d);
    return rmdir(dir) == 0;
}

bool am_asset_rmdir(const char *dir)
{
    if (!dir || !dir[0]) return false;
    struct stat st;
    if (stat(dir, &st) != 0) return true;      /* 本来就没有 */
    bool ok = rm_tree(dir, 0);
    if (!ok) ESP_LOGW(TAG, "删除目录失败: %s", dir);
    return ok;
}

/* 不带进度的兼容入口 */
bool am_asset_install(const char *zip_path, const char *target_dir)
{
    return am_asset_install_cb(zip_path, target_dir, NULL);
}
