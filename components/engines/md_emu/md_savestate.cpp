/* md_savestate.cpp — 把 genesis.cpp 的 C++ 存档接口包装成 C 函数 (md_emu.c 调用) */
#include <string>

extern "C" {
void load_genesis(std::string_view save_path);
void save_genesis(std::string_view save_path);
}

extern "C" void md_emu_savestate_load(const char *path)
{
    if (!path || !path[0]) return;
    load_genesis(std::string_view(path));
}

extern "C" void md_emu_savestate_save(const char *path)
{
    if (!path || !path[0]) return;
    save_genesis(std::string_view(path));
}
