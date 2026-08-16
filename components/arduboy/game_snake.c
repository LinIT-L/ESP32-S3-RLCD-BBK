/**
 * @file game_snake.c
 * @brief 贪吃蛇游戏 (128x64, Arduboy 风格)
 *
 * 操作 (仅 2 个按键, 相对转向):
 *   KEY (BTN_A) 短按 = 逆时针转向 (左转)
 *   BOOT(BTN_B) 短按 = 顺时针转向 (右转)
 *   游戏结束后按任意键重开
 *
 * 网格: 40 列 x 18 行, 每格 3x3 像素, 居中显示
 *   留顶部 10px 给分数显示, 底部刚好到底
 *
 * 蛇身用环形缓冲区存储, 头指针移动, 吃食物时长度 +1.
 */
#include "arduboy.h"
#include "arduboy_emu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define GRID_W          40
#define GRID_H          18
#define CELL            3
#define GRID_OFFSET_X   ((ARDUBOY_WIDTH - GRID_W * CELL) / 2)  /* 4 */
#define GRID_OFFSET_Y   10

/* 蛇身最大长度 = 整个网格 */
#define SNAKE_MAX       (GRID_W * GRID_H)  /* 720 */

/* 方向: 0=上, 1=右, 2=下, 3=左 (顺时针) */
#define DIR_UP    0
#define DIR_RIGHT 1
#define DIR_DOWN  2
#define DIR_LEFT  3
static const int8_t DIR_DX[4] = { 0, 1, 0, -1};
static const int8_t DIR_DY[4] = {-1, 0, 1,  0};

/* 每多少帧移动一步 (30fps / move_delay = 每秒步数) */
#define MOVE_DELAY_FRAMES  6  /* ~5 步/秒 */

typedef struct {
    int8_t  body[SNAKE_MAX][2];  /* 环形缓冲, body[head] = 头 */
    int     head;
    int     len;
    int     dir;
    int8_t  food_x;
    int8_t  food_y;
    int     score;
    int     move_counter;
    bool    game_over;
    unsigned int rng_seed;
} snake_state_t;

static snake_state_t g;

/* 简单 LCG 随机数 (避开 stdlib rand 的全局状态/线程问题) */
static int8_t rng_next(int8_t range) {
    g.rng_seed = g.rng_seed * 1103515245u + 12345u;
    return (int8_t)((g.rng_seed >> 16) % (unsigned int)range);
}

static void snake_spawn_food(void) {
    bool ok = false;
    int tries = 0;
    while (!ok && tries < 200) {
        g.food_x = rng_next((int8_t)GRID_W);
        g.food_y = rng_next((int8_t)GRID_H);
        ok = true;
        for (int i = 0; i < g.len; i++) {
            int idx = (g.head - i + SNAKE_MAX) % SNAKE_MAX;
            if (g.body[idx][0] == g.food_x && g.body[idx][1] == g.food_y) {
                ok = false;
                break;
            }
        }
        tries++;
    }
}

static void snake_init(void) {
    g.len = 3;
    g.dir = DIR_RIGHT;
    g.rng_seed = (unsigned int)arduboy_millis() ^ 0x5A5A;
    if (g.rng_seed == 0) g.rng_seed = 1;
    /* 初始蛇身水平排列, 头在右.
     * 环形缓冲: head 在索引 len-1, 向后 (head-i) 依次是更早的段. */
    g.head = g.len - 1;
    for (int i = 0; i < g.len; i++) {
        int idx = (g.head - i + SNAKE_MAX) % SNAKE_MAX;
        g.body[idx][0] = (int8_t)(10 - i);
        g.body[idx][1] = (int8_t)(GRID_H / 2);
    }
    g.score = 0;
    g.move_counter = 0;
    g.game_over = false;
    snake_spawn_food();
}

static void snake_update(void) {
    uint8_t pressed = arduboy_buttons_pressed();

    /* 游戏结束: 任意键重开 */
    if (g.game_over) {
        if (pressed & (ARDUBOY_BTN_A | ARDUBOY_BTN_B)) {
            snake_init();
        }
        return;
    }

    /* 转向: A = 逆时针 (dir-1), B = 顺时针 (dir+1) */
    if (pressed & ARDUBOY_BTN_A) g.dir = (g.dir + 3) & 3;  /* +3 == -1 mod 4 */
    if (pressed & ARDUBOY_BTN_B) g.dir = (g.dir + 1) & 3;

    /* 按帧节流移动 */
    g.move_counter++;
    if (g.move_counter < MOVE_DELAY_FRAMES) return;
    g.move_counter = 0;

    int nx = g.body[g.head][0] + DIR_DX[g.dir];
    int ny = g.body[g.head][1] + DIR_DY[g.dir];

    /* 撞墙 */
    if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) {
        g.game_over = true;
        return;
    }

    /* 撞自身 (除尾部, 因为尾部本帧会移走 — 但若吃食物则尾部保留)
     * 简单起见: 检查除尾外的所有身体段 */
    int check_len = g.len;
    for (int i = 1; i < check_len; i++) {
        int idx = (g.head - i + SNAKE_MAX) % SNAKE_MAX;
        if (g.body[idx][0] == nx && g.body[idx][1] == ny) {
            g.game_over = true;
            return;
        }
    }

    /* 移动头: 推进环形缓冲的 head 指针 */
    int new_head = (g.head + 1) % SNAKE_MAX;
    g.body[new_head][0] = (int8_t)nx;
    g.body[new_head][1] = (int8_t)ny;
    g.head = new_head;

    /* 吃食物: 长度 +1 (尾部保留), 生成新食物 */
    if (nx == g.food_x && ny == g.food_y) {
        g.score += 10;
        if (g.len < SNAKE_MAX) {
            g.len++;
        }
        snake_spawn_food();
    }
    /* 否则长度不变, 尾部被自然丢弃 (环形缓冲下次覆盖) */
}

static void snake_render(void) {
    /* 边框 */
    arduboy_draw_rect(GRID_OFFSET_X - 1, GRID_OFFSET_Y - 1,
                      GRID_W * CELL + 2, GRID_H * CELL + 2,
                      ARDUBOY_WHITE);

    /* 蛇身 (环形缓冲: 从头到尾) */
    for (int i = 0; i < g.len; i++) {
        int idx = (g.head - i + SNAKE_MAX) % SNAKE_MAX;
        int px = GRID_OFFSET_X + g.body[idx][0] * CELL;
        int py = GRID_OFFSET_Y + g.body[idx][1] * CELL;
        arduboy_fill_rect(px, py, CELL, CELL, ARDUBOY_WHITE);
    }

    /* 食物 (画成中心点 + 边框, 与蛇身区分) */
    {
        int px = GRID_OFFSET_X + g.food_x * CELL;
        int py = GRID_OFFSET_Y + g.food_y * CELL;
        arduboy_fill_rect(px, py, CELL, CELL, ARDUBOY_WHITE);
    }

    /* 分数 */
    char buf[16];
    snprintf(buf, sizeof(buf), "S:%d", g.score);
    arduboy_set_cursor(0, 1);
    arduboy_print(buf);

    /* 提示 */
    arduboy_set_cursor(ARDUBOY_WIDTH - 60, 1);
    arduboy_print("A<L B>R");

    if (g.game_over) {
        arduboy_fill_rect(20, 26, 88, 14, ARDUBOY_BLACK);
        arduboy_draw_rect(20, 26, 88, 14, ARDUBOY_WHITE);
        arduboy_set_cursor(34, 30);
        arduboy_print("GAME OVER");
        arduboy_set_cursor(28, 44);
        arduboy_print("PRESS TO RESET");
    }
}

const arduboy_game_impl_t snake_game = {
    .init   = snake_init,
    .update = snake_update,
    .render = snake_render,
    .name   = "Snake",
};
