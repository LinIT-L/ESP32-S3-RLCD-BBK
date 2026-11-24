/* am_lpk.c — LPK 单文件资源包的安装收尾（见 am_lpk.h 的分工说明） */
#include "am_lpk.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "lpk_mode.h"
#include "am_lpkg.h"
#include "lpk_manifest.h"  /* lpk_mf_load：判断包是否带 manifest（新体系分流） */      /* ★ 新体系：manifest + installed/<app_id>/ */
#include "lpk1.h"

static const char *TAG = "AMLpk";

void am_lpk_stem(const char *path, char *out, unsigned cap)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!path) return;
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    /* ★ 2026-11-01 修正：只去掉**最后一个**扩展名。
     *   原实现遇到第一个 '.' 就停 ⇒ "com.lintos.recorder.lpk" → "com"，
     *   于是 ① 下载中转文件全叫 /sdcard/apps/staging/com.lpk（所有 LPK 共用一个名字，
     *   两个包交替时会互相覆盖）② 老体系安装标记写成 .installed.com。
     *   现在与 am_lpkg.h 里 am_lpkg_stem_of() 的契约完全一致（两份实现必须同一个语义）。 */
    const char *dot = strrchr(b, '.');
    size_t n = (dot && dot != b) ? (size_t)(dot - b) : strlen(b);
    if (n >= cap) n = cap - 1;
    memcpy(out, b, n);
    out[n] = 0;
}

bool am_lpk_finish(const char *lpk_path, const char *target_dir)
{
    return am_lpk_finish_meta2(lpk_path, target_dir, NULL, NULL, 0, 0, 0);
}

bool am_lpk_finish_meta(const char *lpk_path, const char *target_dir,
                        const char *src_ver, const char *src_build)
{
    return am_lpk_finish_meta2(lpk_path, target_dir, src_ver, src_build, 0, 0, 0);
}

bool am_lpk_finish_meta2(const char *lpk_path, const char *target_dir,
                         const char *src_ver, const char *src_build,
                         uint32_t src_mtime, uint32_t src_crc, uint32_t src_size)
{
    if (!lpk_path || !lpk_path[0]) return false;

    /* ★★★ 2026-11-01 新体系分流：
     *   带 manifest.json 的包 ⇒ 统一安装器（校验 → staging → 原子切换 →
     *   installed/<app_id>/ → 写 registry → 注册），**不再走 target_dir + 模式记录**。
     *   不带 manifest 的老包（仙剑 pal.lpk 这类纯资源包）⇒ 保持原逻辑。
     *   ⚠ manifest 存在但**不合法**时必须直接失败，绝不能落到老路径"装上了" ——
     *     那正是"看起来装好、其实坏了"的来源。 */
    {
        lpk_mf_t probe;
        char w2[96] = {0};
        int mrc = lpk_mf_load(lpk_path, &probe, w2, sizeof(w2));
        if (mrc == LPK_MF_OK) {
            char why[160] = {0};
            esp_err_t e = am_lpkg_install_src2(lpk_path, src_ver, src_build,
                                               src_mtime, src_crc, src_size,
                                               why, sizeof(why));
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "新体系安装失败(%s): %s", esp_err_to_name(e), why);
                remove(lpk_path);       /* manifest 不合法/解包失败: staging 里不留垃圾 */
                return false;
            }
            ESP_LOGW(TAG, "新体系安装完成: %s", probe.app_id);
            /* 装完把 staging 里的中间包删掉 —— 内容已经在 installed/<app_id>/，
             * 留着只会在卡上慢慢堆积（每次升级都来一份）。 */
            if (strncmp(lpk_path, "/sdcard/apps/staging/", 21) == 0) remove(lpk_path);
            return true;
        }
        if (mrc != LPK_MF_E_NOENT) {
            ESP_LOGE(TAG, "包内 manifest 不合法，拒绝安装: %s", w2);
            return false;
        }
    }

    /* 走到这里 = 老格式（无 manifest）的纯资源包，target_dir 才是必需的 */
    if (!target_dir || !target_dir[0]) {
        ESP_LOGE(TAG, "老格式 LPK 缺少 target_dir: %s", lpk_path);
        return false;
    }

    /* ① 验包：头 + 索引 + 9 条必备资源。缺一条都不登记 —— 宁可让用户看到
     *   "安装失败"，也不要装一个打开就闪退的游戏（那种更难查）。 */
    char why[160] = {0};
    if (!lpk_mode_lpk_ready(lpk_path, NULL, 0, why, sizeof(why))) {
        ESP_LOGE(TAG, "拒绝登记（包不可用）: %s", why);
        return false;
    }

    char stem[64];
    am_lpk_stem(lpk_path, stem, sizeof(stem));
    if (!stem[0]) { ESP_LOGE(TAG, "取不到包名: %s", lpk_path); return false; }

    const char *base = strrchr(lpk_path, '/');
    base = base ? base + 1 : lpk_path;

    struct stat st;
    if (stat(lpk_path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "包不在或为空: %s", lpk_path);
        return false;
    }

    /* ② 清单：只列这一个文件。am_asset_uninstall() 会照它删，
     *   所以卸载 LPK 包不会误删同目录下的存档（save 目录里的 .rpg 不在清单里）。 */
    char lst[200];
    snprintf(lst, sizeof(lst), "%s/.pkg.%s.lst", target_dir, stem);
    FILE *f = fopen(lst, "wb");
    if (!f) { ESP_LOGE(TAG, "写清单失败: %s", lst); return false; }
    fprintf(f, "%s\n", base);
    fclose(f);

    /* ③ 安装标记："<文件数> <总字节>"，与 zip 包同格式 ⇒ 已安装判定原样可用 */
    char mark[200];
    snprintf(mark, sizeof(mark), "%s/.installed.%s", target_dir, stem);
    f = fopen(mark, "wb");
    if (!f) { ESP_LOGE(TAG, "写安装标记失败: %s", mark); return false; }
    fprintf(f, "1 %llu\n", (unsigned long long)st.st_size);
    fclose(f);

    /* ④ 记安装模式：启动时 lpk_mode_decide() 看到 "lpk" 就直接挂 /lpk */
    if (lpk_mode_record_write(target_dir, 1) != 0) {
        ESP_LOGE(TAG, "写安装记录失败: %s/%s", target_dir, LPK_MODE_RECORD);
        return false;
    }

    int n = 0;
    lpk_t h;
    if (lpk_open(&h, lpk_path, NULL, 0) == LPK_OK) { n = (int)h.count; lpk_close(&h); }
    ESP_LOGW(TAG, "LPK 安装完成: %s（%llu B, %d 条资源）→ 模式=lpk",
             lpk_path, (unsigned long long)st.st_size, n);
    return true;
}

void am_lpk_clear_record(const char *target_dir)
{
    if (!target_dir || !target_dir[0]) return;
    char p[200];
    snprintf(p, sizeof(p), "%s/%s", target_dir, LPK_MODE_RECORD);
    remove(p);
}
