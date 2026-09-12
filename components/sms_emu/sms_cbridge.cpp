/* sms_cbridge.cpp — SMS/GG (SMSPlus) C 桥: 取 RGB565 帧 + 存档包装 */
#include <string>
#include <span>
#include <cstring>

#include "sms.hpp"

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

extern "C" void sms_emu_savestate_load(const char *path)
{
    if (!path || !path[0]) return;
    load_sms(std::string_view(path));
}

extern "C" void sms_emu_savestate_save(const char *path)
{
    if (!path || !path[0]) return;
    save_sms(std::string_view(path));
}
