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
#include "lmz.h"   /* ★ 2026-10-27: zip 解压(stored+deflate) */

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

/* miniz 的解压回调: 逐块写文件 —— 流式, 内存不随包体增长(11MB 的 GOP.MKF 也扛得住) */
static size_t asset_write_cb(void *opaque, mz_uint64 file_ofs, const void *buf, size_t n)
{
    (void)file_ofs;
    return fwrite(buf, 1, n, (FILE *)opaque);
}

bool am_asset_install_cb(const char *zip_path, const char *target_dir,
                         am_asset_prog_cb_t cb)
{
    if (!zip_path || !target_dir || !target_dir[0]) return false;

    /* ★★★ 2026-10-27 linit: 改用 miniz 的 zip 读取器, **同时支持 stored(0) 与 deflate(8)**。
     *   原来手写的解析只认 method=0 ⇒ 常规压缩的 pal.zip 里 109 个文件全部被"跳过",
     *   解出来是空目录 ⇒ 仙剑提示"资源未安装"; 而商店只看目录存在又显示"可卸载"。
     *   mz_zip_reader_extract_to_callback 是流式的, 内存不随包体增长。 */
    mz_zip_archive z;
    memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_file(&z, zip_path, 0)) {
        ESP_LOGE(TAG, "打不开资源包(zip): %s", zip_path);
        return false;
    }
    int total = (int)mz_zip_reader_get_num_files(&z);
    ESP_LOGI(TAG, "资源包 %s: %d 个条目", zip_path, total);
    if (total <= 0) { mz_zip_reader_end(&z); return false; }

    mkdirs(target_dir);

    char dst[320];
    int done = 0, skipped = 0, failed = 0;
    for (int i = 0; i < total; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) { failed++; continue; }
        const char *name = st.m_filename;
        if (!name || !name[0] || st.m_is_directory) { skipped++; continue; }
        /* 安全: 拒绝绝对路径与 ".." (防 zip slip 写出 target 之外) */
        if (name[0] == '/' || strstr(name, "..")) {
            ESP_LOGW(TAG, "拒绝可疑路径: %s", name);
            skipped++;
            continue;
        }
        snprintf(dst, sizeof(dst), "%s/%s", target_dir, name);
        char pdir[256];
        parent_of(dst, pdir, sizeof(pdir));
        if (pdir[0]) mkdirs(pdir);

        FILE *fo = fopen(dst, "wb");
        if (!fo) { ESP_LOGE(TAG, "写文件失败: %s", dst); failed++; continue; }
        bool ok = mz_zip_reader_extract_to_callback(&z, (mz_uint)i, asset_write_cb, fo, 0);
        fclose(fo);
        if (!ok) {
            ESP_LOGE(TAG, "解压失败: %s", dst);
            remove(dst);
            failed++;
            continue;
        }
        done++;
        if (cb) cb((int)((long)(i + 1) * 1000 / (long)total));
    }
    mz_zip_reader_end(&z);

    ESP_LOGI(TAG, "解包完成: 成功 %d / 跳过 %d / 失败 %d → %s", done, skipped, failed, target_dir);
    bool all_ok = (failed == 0 && done > 0);
    /* ★★★ 2026-10-27 linit: **全部成功才写 .installed 标记**。
     *   "是否已安装"改成验证这个标记, 不再只看目录存在 —— 否则解压失败/空目录
     *   也会显示"可卸载", 用户点进应用却提示"资源未安装"(就是这次的坑)。 */
    if (all_ok) {
        char mark[320];
        snprintf(mark, sizeof(mark), "%s/.installed", target_dir);
        FILE *mf = fopen(mark, "w");
        if (mf) { fprintf(mf, "%d\n", done); fclose(mf); }
        else ESP_LOGW(TAG, "写安装标记失败: %s", mark);
    } else {
        ESP_LOGW(TAG, "解包未全部成功 (成功 %d/失败 %d), 不写安装标记", done, failed);
    }
    return all_ok;
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
