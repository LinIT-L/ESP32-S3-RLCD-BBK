/* md_savestate.cpp — 把 genesis.cpp 的 C++ 存档接口包装成 C 函数 (md_emu.c 调用)
 *
 * ★★ 2026-11-22 (第3回合) 存档安全整改 ★★
 *   原来这里只是两个一行转发, 真正落盘在 genesis.cpp 的 save_genesis():
 *     `savestate_fp = fopen(path, "wb"); gwenesis_save_state(); fclose();`
 *   两个真实问题:
 *     ① fopen 失败不判空 → gwenesis_save_state() 对 NULL fwrite ⇒ 崩 (已在 genesis.cpp 修)
 *     ② "wb" 先截断旧档 → 写到一半掉电/拔卡, 原存档已经没了, 只剩半截新数据
 *   这里改成: 先写 <path>.tmp, 校验确实写出了内容, 再走 os_file_replace()
 *   (旧档挪 .old → tmp 换名 → 成功后才删 .old; 失败回滚)。
 *   存档内容与格式**完全由 gwenesis_save_state() 决定, 一个字节没改**, 旧档照常读。
 *
 *   ⚠ 残余风险(诚实说明): gwenesis_save_state() 内部不检查 fwrite 返回值, 所以
 *     "卡恰好写满"时会得到一个偏短的 .tmp。这里用"非空 + 不小于 512B"当门槛挡住
 *     "完全没写成"; 要做到逐字节可信, 得改上游 core 的写盘逻辑, 不在本次范围。
 *     即便如此, 现在的行为也比原来严格更好: 原来连"旧档先被毁"都挡不住。
 */
#include <string>
#include <cstdio>
#include <sys/stat.h>

#include "os_file.h"

extern "C" {
void load_genesis(std::string_view save_path);
void save_genesis(std::string_view save_path);
}

/* 任何真实 savestate 都远大于这个数; 低于它说明 fopen 失败或卡写满, 不替换旧档。 */
static constexpr long SAVE_MIN_BYTES = 512;

extern "C" void md_emu_savestate_load(const char *path)
{
    if (!path || !path[0]) return;
    /* 掉电恢复: 上次保存若断在"旧档已挪成 .old、新档未就位"之间, 先把 .old 复位。 */
    if (os_file_recover_auto(path) == OS_FS_REC_RESTORED) {
        /* 只报事实, 不打 ERR (恢复成功属于正常兜底) */
        printf("[MD] 存档掉电恢复: %s 已由备份复位\n", path);
    }
    load_genesis(std::string_view(path));
}

extern "C" void md_emu_savestate_save(const char *path)
{
    if (!path || !path[0]) return;

    const std::string tmp = std::string(path) + ".tmp";
    const std::string old = std::string(path) + OS_FILE_OLD_SUFFIX;

    (void)remove(tmp.c_str());                 /* 上次残留 (半截, 不能接在后面写) */
    save_genesis(std::string_view(tmp));

    struct stat st;
    if (stat(tmp.c_str(), &st) != 0 || st.st_size < SAVE_MIN_BYTES) {
        /* 没写出来 / 写得太少: 扔掉临时件, **旧存档保持原样** */
        (void)remove(tmp.c_str());
        printf("[MD] 存档保存失败(临时文件无效), 旧存档未改动\n");
        return;
    }
    if (!os_file_replace(tmp.c_str(), path, old.c_str())) {
        (void)remove(tmp.c_str());
        printf("[MD] 存档替换失败, 旧存档已回滚\n");
    }
}
