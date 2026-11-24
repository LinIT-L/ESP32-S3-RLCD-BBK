/* sms_cbridge.cpp — SMS/GG (SMSPlus) C 桥: 取 RGB565 帧 + 存档包装 */
#include <string>
#include <span>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

#include "sms.hpp"
#include "os_file.h"

extern "C" int sms_video_rgb565(uint8_t *dst, int max_bytes, int *w, int *h)
{
    auto span = get_sms_video_buffer();
    if (span.empty() || !dst) return 0;
    size_t n = span.size();
    if (n > (size_t)max_bytes) n = (size_t)max_bytes;
    memcpy(dst, span.data(), n);
    if (w) *w = (n == 160 * 144 * 2) ? 160 : 256;
    if (h) *h = (n == 160 * 144 * 2) ? 144 : 192;
    return (int)n;
}

/* 任何真实 savestate 都远大于这个数; 低于它说明 fopen 失败或卡写满, 不替换旧档。 */
static constexpr long SAVE_MIN_BYTES = 512;

extern "C" void sms_emu_savestate_load(const char *path)
{
    if (!path || !path[0]) return;
    /* 掉电恢复: 上次保存若断在"旧档已挪成 .old、新档未就位"之间, 先把 .old 复位。 */
    if (os_file_recover_auto(path) == OS_FS_REC_RESTORED)
        printf("[SMS] 存档掉电恢复: %s 已由备份复位\n", path);
    load_sms(std::string_view(path));
}

/* ★★ 2026-11-22 (第3回合) 存档安全: 原来是直接调 save_sms() —— 内部
 *   `fopen(path,"wb")` 先截断旧档再写, 写到一半掉电/拔卡就把唯一存档弄坏。
 *   改成: 写 <path>.tmp → 校验确实写出了内容 → os_file_replace() 三段替换。
 *   格式完全由 system_save_state() 决定, 一个字节没改, 旧档照常读。
 *   残余风险: system_save_state() 不查 fwrite 返回值, "卡恰好写满"仍可能得到偏短的
 *   .tmp; 用 512B 门槛挡住"完全没写成", 要做到逐字节可信必须改上游 core (不在本次范围)。 */
extern "C" void sms_emu_savestate_save(const char *path)
{
    if (!path || !path[0]) return;

    const std::string tmp = std::string(path) + ".tmp";
    const std::string old = std::string(path) + OS_FILE_OLD_SUFFIX;

    (void)remove(tmp.c_str());
    save_sms(std::string_view(tmp));

    struct stat st;
    if (stat(tmp.c_str(), &st) != 0 || st.st_size < SAVE_MIN_BYTES) {
        (void)remove(tmp.c_str());
        printf("[SMS] 存档保存失败(临时文件无效), 旧存档未改动\n");
        return;
    }
    if (!os_file_replace(tmp.c_str(), path, old.c_str())) {
        (void)remove(tmp.c_str());
        printf("[SMS] 存档替换失败, 旧存档已回滚\n");
    }
}
