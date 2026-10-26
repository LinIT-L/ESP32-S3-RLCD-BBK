// pal_rix2wav.cpp — 把仙剑 MUS.MKF 里的 RIX 曲目离线渲染成 WAV（PC 端工具）
//
// 为什么需要它：OPL2 软合成在 ESP32-S3 上实时因子 < 1（实测两种配置都跑不到实时），
// 但离线渲染没有实时约束 —— 在 PC 上用同一套 adplug 代码慢慢算，算出成品音频，
// 设备端只负责解码播放（minimp3，CPU 极低）。
//
// 用法：pal_rix2wav <MUS.MKF> <输出目录> [采样率=22050] [最长秒数=240]
// 产物：out/rix_<n>.wav（16bit 立体声）
//
// 依赖：third_party/sdlpal/adplug/*（LGPL/GPL，与本项目一致）

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

#include <strings.h>

/* SDLPal 的 adplug 分支用到 SDL_strcasecmp（host 上没有 SDL 库, 这里补一个） */
extern "C" int SDL_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }

#include "rix.h"
#include "emuopls.h"

static void write_u32(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f); }
static void write_u16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <MUS.MKF> <outdir> [rate=22050] [maxsec=240]\n", argv[0]);
        return 2;
    }
    const char *mkf = argv[1];
    const char *outdir = argv[2];
    const int   rate = (argc > 3) ? atoi(argv[3]) : 22050;
    const int   maxsec = (argc > 4) ? atoi(argv[4]) : 240;

    mkdir(outdir, 0755);

    /* LINTOS: 与固件同一个合成核 (emu8950), 这样主机渲染结果可直接当"正确性基准" */
    Copl *opl = CEmuopl::CreateEmuopl(OPLCORE::EMU8950, Copl::TYPE_OPL2, rate);
    if (!opl) { fprintf(stderr, "opl create failed\n"); return 1; }

    CrixPlayer rix(opl);
    if (!rix.load(std::string(mkf), CProvider_Filesystem())) {
        fprintf(stderr, "load %s failed\n", mkf);
        return 1;
    }

    const unsigned nsub = rix.getsubsongs();
    printf("subsongs=%u rate=%d\n", nsub, rate);

    std::vector<short> buf((size_t)rate / 70 * 2 + 8);   // 每 tick ≈ 1/70s 的立体声帧
    const int frames_per_tick = rate / 70;

    for (unsigned song = 1; song <= nsub; song++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/rix_%u.wav", outdir, song);
        FILE *f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "open %s failed\n", path); continue; }

        // WAV 头先占位，最后回填长度
        fwrite("RIFF", 1, 4, f); write_u32(f, 0); fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f); write_u32(f, 16);
        /* ★★★ 2026-10-25 [LINTOS] 这里是「转换出来的音乐完全不是原来那首」的根因:
         *   Copl::update()/EMU8950OPL2::Generate() 只填 samples 个 short —— 即**单声道**。
         *   原来这里声明 2ch 并写 samples*2 个 short, 于是左右交错:
         *     L = 真实音频, R = 未初始化的垃圾
         *   听感就是声音完全变形/像另一首曲子。改成单声道写入。
         *   (体积也更小: 单声道 16bit 正好省一半) */
        write_u16(f, 1); write_u16(f, 1);            // PCM, 1ch (mono)
        write_u32(f, (unsigned)rate);
        write_u32(f, (unsigned)rate * 2);
        write_u16(f, 2); write_u16(f, 16);
        fwrite("data", 1, 4, f); write_u32(f, 0);

        rix.rewind((int)song);
        long long written = 0;
        const long long cap = (long long)rate * maxsec;
        int ticks = 0;

        while (written < cap) {
            if (!rix.update()) break;                // 曲子放完
            opl->update(buf.data(), frames_per_tick);
            fwrite(buf.data(), sizeof(short), (size_t)frames_per_tick, f);
            written += frames_per_tick;
            ticks++;
        }

        const unsigned data_bytes = (unsigned)(written * 2);
        fseek(f, 4, SEEK_SET);  write_u32(f, 36 + data_bytes);
        fseek(f, 40, SEEK_SET); write_u32(f, data_bytes);
        fclose(f);
        printf("  rix_%u.wav  %.1fs\n", song, (double)written / rate);
        fflush(stdout);
    }
    delete opl;
    return 0;
}
