/**
 * bayer_demo.c — 8×8 / 4×4 Bayer 有序抖动在 ST7305 400×300 1bpp 上的可视对照
 *
 * 目的：在没有 ROM、不能真机烧录的前提下，先看清"换 Bayer 到底长什么样"。
 *   ⛔ 这不是 ESP32 模拟器，也不改 LinTOS 源码；只是把同一套量化数学
 *      在主机上算出 400×300 的 1bpp 图，导出成灰度预览图。
 *   ✅ 它输出的是**最终会写进 fb 的位**（1=白 0=黑），与真机显示位一一对应。
 *
 * 编译运行：
 *   cc -O2 -std=c11 tools/bayer_demo.c -o /tmp/bayer_demo && /tmp/bayer_demo /tmp
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define W 400
#define H 300

/* 8×8 Bayer（64 相位）—— 教科书标准矩阵 */
static const unsigned char bayer8[64] = {
     0,32, 8,40, 2,34,10,42,
    48,16,56,24,50,18,58,26,
    12,44, 4,36,14,46, 6,38,
    60,28,52,20,62,30,54,22,
     3,35,11,43, 1,33, 9,41,
    51,19,59,27,49,17,57,25,
    15,47, 7,39,13,45, 5,37,
    63,31,55,23,61,29,53,21,
};

/* 4×4 Bayer（16 相位）—— 低成本路径 */
static const unsigned char bayer4[16] = {
     0, 8, 2,10,
    12, 4,14, 6,
     3,11, 1, 9,
    15, 7,13, 5,
};

/* 现有 LinTOS 做法：2×2 块按"黑点数"聚簇（dots 曲线默认 {0,1,3,4}） */
static const unsigned char pat_2x2[4][4] = {   /* 1=白 0=黑；左上 右上 左下 右下 */
    {1,1,1,1},   /* 0 黑点 → 全白 */
    {0,1,1,1},   /* 1 黑点 */
    {0,0,0,1},   /* 3 黑点 */
    {0,0,0,0},   /* 4 黑点 → 全黑 */
};

typedef struct { unsigned char *px; } img_t;

static void img_new(img_t *m) { m->px = malloc((size_t)W * H); }

/* 生成测试图案的亮度场（0..255，0=黑 255=白）：渐变 + 圆 + 细线 + 文字块 */
static void build_scene(unsigned char *lum)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned char v;
            if (y < 120) {
                v = (unsigned char)(x * 255 / (W - 1));          /* 横向 256 级灰阶 */
            } else if (y < 150) {
                v = (unsigned char)((x / 50) & 1 ? 40 : 215);     /* 交替暗/亮块 */
            } else if (y < 190) {
                /* 径向渐变（模拟阴影过渡） */
                int dx = x - W / 2, dy = y - 170;
                int d = dx * dx + dy * dy;
                int r = 30 * 30;
                v = (unsigned char)(d > r ? 255 : (255 - (d * 200 / r)));
            } else if (y < 230) {
                /* 细线测试：1px 竖线/横线（检验细线是否消失或粘连） */
                v = (unsigned char)(((x % 8) == 0 || (y % 8) == 0) ? 0 : 255);
            } else {
                /* 文字块：用 5×7 点阵画 "LinTOS" 与 "50%" */
                v = 255;
            }
            lum[y * W + x] = v;
        }
    }
    /* 简单的 5×7 点阵字，落在 y=240..276 */
    static const unsigned char glyphs[][7] = {
        /* L */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
        /* i */ {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},
        /* n */ {0x00,0x00,0x1A,0x15,0x15,0x15,0x15},
        /* T */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
        /* O */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
        /* S */ {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},
    };
    int cx = 40;
    for (unsigned gi = 0; gi < sizeof(glyphs) / sizeof(glyphs[0]); gi++) {
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (glyphs[gi][r] & (0x10 >> c))
                    for (int sy = 0; sy < 3; sy++)
                        for (int sx = 0; sx < 3; sx++)
                            lum[(240 + r * 3 + sy) * W + cx + c * 3 + sx] = 0;
        cx += 20;
    }
}

/* 方案 A：现有 4 档 + 2×2 聚簇（2 倍放大的物理像素感） */
static void render_2x2_cluster(img_t *m, const unsigned char *lum)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned L = lum[y * W + x];
            int shade;
            if (L >= 190) shade = 0; else if (L >= 130) shade = 1;
            else if (L >= 60) shade = 2; else shade = 3;
            int q = ((y & 1) << 1) | (x & 1);
            m->px[y * W + x] = pat_2x2[shade][q] ? 255 : 0;
        }
    }
}

/* 方案 B：8×8 Bayer，多档灰（levels = 2..64，用 (levels-1) 个密度台阶） */
static void render_bayer8(img_t *m, const unsigned char *lum, int levels)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned L = lum[y * W + x];
            /* 实黑/实白保护：两端各留 8 级不吃抖动，避免黑白画面发灰 */
            if (L <= 7)   { m->px[y * W + x] = 0;   continue; }
            if (L >= 248) { m->px[y * W + x] = 255; continue; }
            /* 亮度 → 密度（0..63），再与 Bayer 阈值比较：LUT 化后实际只需一次查表 */
            unsigned dens = (unsigned)((255 - L) * 63u / 255u);
            unsigned th = bayer8[((y & 7) << 3) | (x & 7)];
            /* levels 量化：把连续密度压到 levels 个台阶 */
            if (levels < 64) dens = (dens * (unsigned)levels) / 64u * (64u / (unsigned)levels);
            m->px[y * W + x] = (dens > th) ? 0 : 255;
        }
    }
}

/* 方案 C：4×4 Bayer（低成本路径，16 相位） */
static void render_bayer4(img_t *m, const unsigned char *lum, int levels)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned L = lum[y * W + x];
            if (L <= 7)   { m->px[y * W + x] = 0;   continue; }
            if (L >= 248) { m->px[y * W + x] = 255; continue; }
            unsigned dens = (unsigned)((255 - L) * 15u / 255u);
            unsigned th = bayer4[((y & 3) << 2) | (x & 3)];
            if (levels < 16) dens = (dens * (unsigned)levels) / 16u * (16u / (unsigned)levels);
            m->px[y * W + x] = (dens > th) ? 0 : 255;
        }
    }
}

static void write_pgm(const char *path, const img_t *m)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P5\n%d %d\n255\n", W, H);
    fwrite(m->px, 1, (size_t)W * H, f);
    fclose(f);
    printf("wrote %s\n", path);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    unsigned char *lum = malloc((size_t)W * H);
    img_t a, b, c, d;
    img_new(&a); img_new(&b); img_new(&c); img_new(&d);
    build_scene(lum);

    render_2x2_cluster(&a, lum);        /* 现状：4 档 + 2×2 */
    render_bayer8(&b, lum, 4);          /* 4 档灰 + 8×8 Bayer */
    render_bayer8(&c, lum, 16);         /* 16 档 + 8×8 Bayer */
    render_bayer4(&d, lum, 16);         /* 16 档 + 4×4 Bayer（低成本） */

    char p[512];
    snprintf(p, sizeof(p), "%s/bayer_a_2x2_4lvl.pgm", dir);   write_pgm(p, &a);
    snprintf(p, sizeof(p), "%s/bayer_b_8x8_4lvl.pgm", dir);   write_pgm(p, &b);
    snprintf(p, sizeof(p), "%s/bayer_c_8x8_16lvl.pgm", dir);  write_pgm(p, &c);
    snprintf(p, sizeof(p), "%s/bayer_d_4x4_16lvl.pgm", dir);  write_pgm(p, &d);

    /* 50% 灰的纹理统计：8×8 Bayer 在 2×2 物理块内的黑点数分布 */
    printf("\n50%% 灰 (L=128) 在 2x2 块内的黑点数分布（看是否出现全黑/全白块 ⇒ 颗粒感）:\n");
    for (int variant = 0; variant < 2; variant++) {
        int hist[5] = {0};
        for (int y = 0; y < H; y += 2)
            for (int x = 0; x < W; x += 2) {
                int black = 0;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        unsigned L = 128;
                        /* 两种方案的密度量程不同(64 / 16)，必须各按自己的量程算 */
                        unsigned dens = variant ? (unsigned)((255 - L) * 15u / 255u)
                                                : (unsigned)((255 - L) * 63u / 255u);
                        unsigned th = variant ? bayer4[((y + dy) & 3) << 2 | ((x + dx) & 3)]
                                              : bayer8[((y + dy) & 7) << 3 | ((x + dx) & 7)];
                        if (dens > th) black++;
                    }
                hist[black]++;
            }
        printf("  %s: 0/4=%d 1/4=%d 2/4=%d 3/4=%d 4/4=%d  (总数 %d)\n",
               variant ? "4x4" : "8x8", hist[0], hist[1], hist[2], hist[3], hist[4],
               (W / 2) * (H / 2));
    }
    free(lum); free(a.px); free(b.px); free(c.px); free(d.px);
    return 0;
}
