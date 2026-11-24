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
#include "esp_heap_caps.h"   /* 64 KB 拷贝缓冲放 PSRAM（内部 RAM 最大块只有 31 KB） */
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

static uint16_t __attribute__((unused)) rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t __attribute__((unused)) rd32(const uint8_t *p)
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

/* 对外入口 (2026-10-31): 多文件直接下载要给每个文件建父目录,
 * 与这里共用同一份实现 —— 免得两处对"末段要不要建"理解不一致。 */
void am_asset_mkdirs(const char *path) { if (path && path[0]) mkdirs(path); }

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

/* 逐块拷贝。prog 可为 NULL —— 每拷一块回调一次 (done, total)。
 * ★ 2026-10-31: 加回调是为了让"解包大文件"时进度条也在动。
 *   原来只在**每个文件**解完后调一次 cb，41 MB 的包里 11 MB 的 SSS.MKF
 *   写一次要好几秒，界面看着就是"卡在 70%"。
 * ★★ 2026-10-31 提速：缓冲由调用方传入（PSRAM 64 KB），不再用 4 KB static。
 *   4 KB 意味着 41.7 MB 要 1 万多次 fread+fwrite，每次都要过 FATFS + SDMMC 的
 *   块开销 ⇒ 真机实测解包要好几分钟。64 KB 一次搬完，实测快一个数量级。 */
static bool copy_range(FILE *fi, long off, uint32_t len, const char *dst,
                       uint8_t *buf, size_t bufsz,
                       void (*prog)(uint32_t done, uint32_t total))
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
    uint32_t left = len, done_b = 0;
    bool ok = true;
    while (left > 0) {
        size_t want = left > bufsz ? bufsz : left;
        size_t got = fread(buf, 1, want, fi);
        if (got == 0) { ok = false; break; }
        if (fwrite(buf, 1, got, fo) != got) { ok = false; break; }
        left -= (uint32_t)got;
        done_b += (uint32_t)got;
        if (prog) prog(done_b, len);
    }
    fclose(fo);
    if (!ok) {
        ESP_LOGE(TAG, "写出不完整: %s", dst);
        remove(dst);
    }
    return ok;
}

/* miniz 的解压回调 —— 2026-10-31 起 deflate 条目不再走 miniz（见 stored_data_ofs 注释），
 * 这里保留以备将来需要，避免编译器报 unused。 */
__attribute__((unused))
static size_t asset_write_cb(void *opaque, mz_uint64 file_ofs, const void *buf, size_t n)
{
    (void)file_ofs;
    return fwrite(buf, 1, n, (FILE *)opaque);
}

/* ★★★ 2026-10-31 (linit): **stored 条目不走 miniz**，直接从 zip 里搬字节。
 *
 *   真机现场（两次，20 KB 和 32 KB 栈都复现）：
 *     Guru Meditation (LoadProhibited)  EXCVADDR=0x28
 *     PC = mz_zip_reader_extract_to_callback  miniz_zip.c:1909
 *     A4 = NULL, A2 = A3 = 0x10000(64KB)
 *   ⇒ miniz 在 1909 行用 pRead_buf 时它已经是 NULL：
 *     它内部 `m_pAlloc(1, MZ_ZIP_MAX_IO_BUF_SIZE=64KB)` 在本机拿不到内存
 *     （内部 RAM largest 只有 31 KB；虽然配了 SPIRAM_USE_MALLOC，这条路径仍然失败），
 *     而 1909 行之前那处 NULL 检查没有拦住（deflate 分支的读缓冲在别处分配）。
 *   ⇒ 结论：**跟压缩算法、跟栈大小都无关，是 miniz 的缓冲分配在本机不可靠**。
 *
 *   对策：stored 条目本来就不需要解压器 —— zip 里存的就是原文件字节，
 *   只要算出数据起点（local header + 30 + 文件名长 + extra 长）直接拷即可。
 *   零分配、零解压器、零风险。这也是为什么打包约定必须是 ZIP_STORED。
 *
 *   返回数据区绝对偏移，失败 -1。 */
static long stored_data_ofs(FILE *zf, mz_uint64 local_header_ofs)
{
    uint8_t h[30];
    if (!zf) return -1;
    if (local_header_ofs > 0x7FFFFFF0ull) return -1;
    if (fseek(zf, (long)local_header_ofs, SEEK_SET) != 0) return -1;
    if (fread(h, 1, sizeof(h), zf) != sizeof(h)) return -1;
    if (h[0] != 'P' || h[1] != 'K' || h[2] != 3 || h[3] != 4) return -1;
    uint16_t nlen = (uint16_t)(h[26] | (h[27] << 8));
    uint16_t elen = (uint16_t)(h[28] | (h[29] << 8));
    return (long)local_header_ofs + 30 + nlen + elen;
}

/* ===== 解包进度（按**字节**推进，不是按文件数） =====
 * 只在下载任务的上下文里用（单线程），不需要加锁。
 * 目的：41 MB 的包里 11 MB 的大文件写一次要好几秒，按文件数回报会看着"卡住"。 */
static am_asset_prog_cb_t s_prog_cb;
static unsigned long long s_prog_total;      /* 全部条目解压后总字节 */
static unsigned long long s_prog_done;       /* 已经写完的字节（不含当前文件） */

static void prog_bytes(uint32_t done_b, uint32_t total_b)
{
    (void)total_b;
    if (!s_prog_cb || s_prog_total == 0) return;
    unsigned long long d = s_prog_done + done_b;
    int pm = (int)(d * 1000ull / s_prog_total);
    if (pm > 1000) pm = 1000;
    if (pm < 0) pm = 0;
    s_prog_cb(pm);
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

    /* ★ 2026-10-31 (linit): 统计压缩方法并告警。
     *   服务器上曾经混进过 **deflate(method=8)** 打的 pal.zip / wuxia.zip：
     *   解包走 tinfl，栈帧比 stored 深得多，真机上直接
     *   `Guru Meditation (LoadProhibited)` + 重启，SD 上只留一个 .tmp。
     *   以后再有压缩包，看这一行日志就能立刻定位，不用再反汇编数栈帧。
     *   （正常应该全是 stored —— tools/pack_asset.py 就是 ZIP_STORED。） */
    {
        int n_deflate = 0, n_other = 0;
        unsigned long long tot = 0;
        for (int i = 0; i < total; i++) {
            mz_zip_archive_file_stat st0;
            if (!mz_zip_reader_file_stat(&z, i, &st0) || st0.m_is_directory) continue;
            if (st0.m_method == MZ_DEFLATED) n_deflate++;
            else if (st0.m_method != 0)     n_other++;
            tot += st0.m_uncomp_size;
        }
        if (n_deflate || n_other) {
            ESP_LOGW(TAG, "资源包 %s 含压缩条目: deflate=%d 其它=%d (共 %d 条) —— "
                          "走 tinfl 解压、栈需求更大；建议用 tools/pack_asset.py 以 ZIP_STORED 重打",
                     zip_path, n_deflate, n_other, total);
        }
        /* 进度基准：按总解压字节推进（大文件写入期间进度条也在动） */
        s_prog_cb    = cb;
        s_prog_total = tot;
        s_prog_done  = 0;
    }

    mkdirs(target_dir);

    /* ★★★ 2026-10-30: 记录"本包装了哪些文件"。
     *   字体包的 target 是**多个包共用**的 /sdcard/fonts，而卸载走的是
     *   am_asset_rmdir() 递归删目录 —— 删掉一个字体会把用户所有字体一起清掉。
     *   有了这份清单就能"只删自己装的文件"。清单名带 zip 主干，互不覆盖。 */
    char lst[336] = {0};
    char stem[64] = {0};          /* ★ 2026-10-31: 提到块外 —— 安装标记也要带它 */
    {
        const char *b = strrchr(zip_path, '/');
        b = b ? b + 1 : zip_path;
        size_t k = 0;
        while (b[k] && b[k] != '.' && k + 1 < sizeof(stem)) { stem[k] = b[k]; k++; }
        stem[k] = 0;
        snprintf(lst, sizeof(lst), "%s/.pkg.%s.lst", target_dir, stem);
    }
    FILE *fl = fopen(lst, "w");
    /* stored 条目直接从这里搬字节（不经 miniz），失败不影响 miniz 读中央目录 */
    FILE *zf = fopen(zip_path, "rb");

    /* ★★★ 2026-10-31 提速核心：拷贝缓冲 **512 KB**（PSRAM）。
     *
     *   解包慢的真正原因不是 CPU，而是**源和目标在同一张 SD 卡上**：
     *   每读一块就要写一块，读写交替是闪存最慢的模式。
     *   原来缓冲只有 4 KB ⇒ 41.7 MB 要 1 万多次 fread+fwrite 交替，
     *   真机上解包比下载还慢（用户实测好几分钟）。
     *   512 KB 之后同样数据只需 82 次交替，实测提速一个数量级。
     *   PSRAM 有 7 MB 空闲，512 KB 只占 7%；分不到就逐级回退，不会因它失败。 */
    size_t cpbuf_sz = 1024 * 1024;
    uint8_t *cpbuf = heap_caps_malloc(cpbuf_sz, MALLOC_CAP_SPIRAM);
    if (!cpbuf) { cpbuf_sz = 512 * 1024; cpbuf = heap_caps_malloc(cpbuf_sz, MALLOC_CAP_SPIRAM); }
    if (!cpbuf) { cpbuf_sz = 256 * 1024; cpbuf = heap_caps_malloc(cpbuf_sz, MALLOC_CAP_SPIRAM); }
    if (!cpbuf) { cpbuf_sz = 64  * 1024; cpbuf = heap_caps_malloc(cpbuf_sz, MALLOC_CAP_SPIRAM); }
    if (!cpbuf) { cpbuf_sz = 16  * 1024; cpbuf = heap_caps_malloc(cpbuf_sz, MALLOC_CAP_SPIRAM); }
    if (!cpbuf) { cpbuf_sz = 4   * 1024; cpbuf = heap_caps_malloc(cpbuf_sz, MALLOC_CAP_8BIT);   }
    if (!cpbuf) {
        ESP_LOGE(TAG, "拷贝缓冲分配失败（PSRAM/内部都拿不到）");
        mz_zip_reader_end(&z);
        if (fl) fclose(fl);
        if (zf) fclose(zf);
        return false;
    }
    ESP_LOGI(TAG, "解包缓冲 %u KB（PSRAM 优先）", (unsigned)(cpbuf_sz / 1024));

    char dst[320];
    int done = 0, skipped = 0, failed = 0;
    unsigned long long out_bytes = 0;   /* ★ 2026-10-31: 解出的总字节，写进安装标记供回验 */
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

        /* ★★★ 2026-10-30: 改成「先写 .tmp 再 rename」的原子落盘。
         *   原来是 fopen(dst,"wb") 直接截断目标文件、解压失败再 remove(dst) ——
         *   对**共享目录** (/sdcard/fonts) 来说等于"装一个坏包把用户原有的好字体毁掉"。
         *   字体资源包的 target 就是 /sdcard/fonts，必须保证失败时原文件分毫不动。 */
        char tmp[336];
        /* ★ 提速：目标不存在（首次安装的常态）直接写目标文件，
         *   省掉 ".tmp + remove + rename" 三次元数据操作；
         *   已存在才走临时文件，保证写到一半掉电不会毁掉旧文件。 */
        struct stat st_old;
        bool existed = (stat(dst, &st_old) == 0);
        if (existed) snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
        else         snprintf(tmp, sizeof(tmp), "%s", dst);
        bool ok = false;
        if (st.m_method == 0) {
            /* ★ stored：直接搬字节。copy_range 自己 fopen(tmp,"wb") 逐块拷贝，
             *   用 PSRAM 的 512 KB 大缓冲，大幅减少 SD 卡读写交替。 */
            long ofs = stored_data_ofs(zf, st.m_local_header_ofs);
            if (ofs < 0) {
                ESP_LOGE(TAG, "定位数据区失败: %s", name);
            } else {
                ok = copy_range(zf, ofs, (uint32_t)st.m_comp_size, tmp,
                                cpbuf, cpbuf_sz, prog_bytes);
            }
        } else {
            /* ★ deflate 不再尝试用 miniz 解 —— 它在 64 KB 缓冲分配失败时会空指针崩溃
             *   （真机 miniz_zip.c:1909）。按打包约定资源包必须是 ZIP_STORED，
             *   遇到压缩包直接判失败并说清原因，绝不再把设备搞重启。 */
            ESP_LOGE(TAG, "该条目是压缩格式(method=%u)，固件只支持 ZIP_STORED: %s",
                     (unsigned)st.m_method, name);
            remove(tmp);
        }
        if (!ok) {
            ESP_LOGE(TAG, "解压失败: %s", dst);
            remove(tmp);                 /* 只删临时文件, 目标位置的旧文件不动 */
            failed++;
            continue;
        }
        /* 只有走临时文件时才需要 rename；直接写目标的那条路径到此已落盘 */
        if (existed) {
            /* FATFS 的 rename 在目标已存在时可能失败 ⇒ 先移除旧文件。
             * 此刻新内容已完整落在 .tmp，即使下一步掉电，旧文件也只是"没了"而不是"半截"。 */
            remove(dst);
            if (rename(tmp, dst) != 0) {
                ESP_LOGE(TAG, "落盘失败 (rename): %s", dst);
                remove(tmp);
                failed++;
                continue;
            }
        }
        done++;
        out_bytes += (unsigned long long)st.m_uncomp_size;
        if (fl) fprintf(fl, "%s\n", name);       /* 记进清单, 卸载时按它删, 不整目录删 */
        if (cb) cb((int)((long)(i + 1) * 1000 / (long)total));
    }
    mz_zip_reader_end(&z);
    if (fl) fclose(fl);
    if (zf) fclose(zf);
    if (cpbuf) { heap_caps_free(cpbuf); cpbuf = NULL; }   /* 1 MB 缓冲用完即还，不常驻 */

    ESP_LOGI(TAG, "解包完成: 成功 %d / 跳过 %d / 失败 %d → %s", done, skipped, failed, target_dir);
    bool all_ok = (failed == 0 && done > 0);
    /* ★★★ 2026-10-27 linit: **全部成功才写 .installed 标记**。
     *   "是否已安装"改成验证这个标记, 不再只看目录存在 —— 否则解压失败/空目录
     *   也会显示"可卸载", 用户点进应用却提示"资源未安装"(就是这次的坑)。 */
    if (all_ok) {
        /* ★★★ 2026-10-31: 标记必须**按包区分**。
         *   字体包的 target 是 13 个包共用的 /sdcard/fonts，目录级标记
         *   `<target>/.installed` 会让"装成一个 = 全部显示已安装"，
         *   其余 12 个点不到下载按钮。改成 `.installed.<zip主干>`。 */
        char mark[384];
        if (stem[0]) snprintf(mark, sizeof(mark), "%s/.installed.%s", target_dir, stem);
        else         snprintf(mark, sizeof(mark), "%s/.installed", target_dir);
        FILE *mf = fopen(mark, "w");
        /* ★ 2026-10-31: 除了文件数，再记**解出的总字节**。
         *   商店的"已安装"不能只信这个标记文件 —— 用户可能删了 .lbf 却留着标记，
         *   或者卡拔了/文件被覆盖。回验时按清单逐个 stat 累加，与这里两数对上才算装了。 */
        if (mf) { fprintf(mf, "%d %llu\n", done, out_bytes); fclose(mf); }
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

/* ===== 2026-10-30: 按清单卸载 (共享目录必需) ================================
 * 背景: 字体包的 target 是 /sdcard/fonts，多个包共用同一个目录。
 *   老卸载走 am_asset_rmdir() 递归删目录 ⇒ 删一个字体把所有字体（含用户手动放的）
 *   全部清掉。这里按安装时写下的 .pkg.<zip主干>.lst 逐个删，
 *   目录里还有别的东西就保留目录。
 * 返回 true = 已处理；false = 没有清单（旧包），调用方应回退到 rmdir。 */
bool am_asset_uninstall(const char *zip_path, const char *target_dir)
{
    if (!target_dir || !target_dir[0]) return false;

    const char *b = (zip_path && zip_path[0]) ? strrchr(zip_path, '/') : NULL;
    b = b ? b + 1 : (zip_path ? zip_path : "");
    char stem[64];
    size_t k = 0;
    while (b[k] && b[k] != '.' && k + 1 < sizeof(stem)) { stem[k] = b[k]; k++; }
    stem[k] = 0;
    if (!stem[0]) return false;

    char lst[336];
    snprintf(lst, sizeof(lst), "%s/.pkg.%s.lst", target_dir, stem);

    FILE *f = fopen(lst, "r");
    if (!f) return false;                      /* 没有清单 = 旧包, 交给调用方 */

    char line[288];
    int removed = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (line[0] == '/' || strstr(line, "..")) continue;   /* 防御: 清单被改坏 */
        char p[336];
        snprintf(p, sizeof(p), "%s/%s", target_dir, line);
        if (remove(p) == 0) removed++;
    }
    fclose(f);

    /* ★ 2026-10-31 真机验证时补的两个残留（之前都留着）:
     *   ① 空子目录 —— 仙剑这类包有 music/ 之类子目录, 文件删光了目录还在,
     *      看目录大小/备份时会以为"没卸干净"。再扫一遍清单, 把每个人的父目录 rmdir
     *      一次(非空的会失败, 忽略即可)。
     *   ② `.installed.<主干>` 标记 —— 原来只删 .lst 不删它, 留一个孤零零的标记文件。
     *      虽然"已安装"判定要求 .lst 也在(所以不会误判), 但残留本身不该有。 */
    f = fopen(lst, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (!n || line[0] == '/' || strstr(line, "..")) continue;
            char *sl = strrchr(line, '/');
            if (!sl) continue;                     /* 顶层文件, 没有子目录 */
            *sl = 0;
            char p[336];
            snprintf(p, sizeof(p), "%s/%s", target_dir, line);
            rmdir(p);                              /* 非空/不存在都无妨 */
        }
        fclose(f);
    }
    remove(lst);
    {
        char mark[336];
        snprintf(mark, sizeof(mark), "%s/.installed.%s", target_dir, stem);
        remove(mark);
    }

    /* 目录空了才删；还有别的字体/文件就保持原样 */
    DIR *d = opendir(target_dir);
    if (d) {
        int cnt = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) cnt++;
        closedir(d);
        if (cnt == 0) rmdir(target_dir);
    }
    ESP_LOGI(TAG, "按清单卸载 %s: 删除 %d 个文件 (目录保留=%s)",
             target_dir, removed, "见上");
    return true;
}

/* ===== 2026-10-30: 该包是否装了某个具体文件 ==================================
 * 卸载前用来判断"正在使用的字体是不是这个包带来的"。
 * 只比对**清单**，不做目录前缀匹配 —— 否则 /sdcard/fonts 下装了 10 个字体，
 * 卸任何一个都会被"目录里有正在用的字体"误拦。
 * 返回 false 也可能是"没有清单"(旧包)，调用方自行决定回退策略。 */
bool am_asset_has_file(const char *zip_path, const char *target_dir, const char *fullpath)
{
    if (!zip_path || !zip_path[0] || !target_dir || !target_dir[0] || !fullpath) return false;

    const char *b = strrchr(zip_path, '/');
    b = b ? b + 1 : zip_path;
    char stem[64];
    size_t k = 0;
    while (b[k] && b[k] != '.' && k + 1 < sizeof(stem)) { stem[k] = b[k]; k++; }
    stem[k] = 0;
    if (!stem[0]) return false;

    char lst[336];
    snprintf(lst, sizeof(lst), "%s/.pkg.%s.lst", target_dir, stem);
    FILE *f = fopen(lst, "r");
    if (!f) return false;

    char line[288];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        char p[336];
        snprintf(p, sizeof(p), "%s/%s", target_dir, line);
        if (strcmp(p, fullpath) == 0) { found = true; break; }
    }
    fclose(f);
    return found;
}
