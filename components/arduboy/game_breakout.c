/**
 * @file game_breakout.c
 * @brief 打砖块游戏 (128x64, Arduboy 风格)
 *
 * 操作 (仅 2 个按键):
 *   KEY (BTN_A) 按住  = 挡板左移
 *   BOOT(BTN_B) 按住  = 挡板右移
 *   球粘在挡板上时自动发射 (约 1 秒后), 也可按任意键立即发射
 *   游戏结束后按任意键重开
 *
 * 布局:
 *   顶部 4..23  : 砖块区 (5 行 x 10 列, 每个 12x4, 留 1px 间隔)
 *   底部 58..60 : 挡板 (20x3)
 *   球 2x2
 */
#include "arduboy.h"
#include "arduboy_emu.h"
#include <stdio.h>
#include <string.h>

#define PADDLE_W       20
#define PADDLE_H       3
#define PADDLE_Y       58
#define PADDLE_SPEED   2

#define BALL_SIZE      2

#define BRICK_COLS     10
#define BRICK_ROWS     5
#define BRICK_W        12
#define BRICK_H        4
#define BRICK_OFFSET_X 4
#define BRICK_OFFSET_Y 4

/* 球粘在挡板上多少帧后自动发射 (30fps -> ~1 秒) */
#define STUCK_AUTO_LAUNCH_FRAMES  30

typedef struct {
    int  paddle_x;
    int  ball_x;
    int  ball_y;
    int  ball_dx;
    int  ball_dy;
    bool ball_stuck;
    int  stuck_frames;
    uint8_t bricks[BRICK_ROWS][BRICK_COLS];
    int  score;
    int  lives;
    bool game_over;
    bool win;
} breakout_state_t;

static breakout_state_t g;

static void breakout_init(void) {
    g.paddle_x = (ARDUBOY_WIDTH - PADDLE_W) / 2;
    g.ball_stuck = true;
    g.stuck_frames = 0;
    g.ball_dx = 1;
    g.ball_dy = -1;
    g.ball_x = g.paddle_x + PADDLE_W / 2 - 1;
    g.ball_y = PADDLE_Y - BALL_SIZE;
    for (int r = 0; r < BRICK_ROWS; r++)
        for (int c = 0; c < BRICK_COLS; c++)
            g.bricks[r][c] = 1;
    g.score = 0;
    g.lives = 3;
    g.game_over = false;
    g.win = false;
}

/* 重置球到挡板上 (失误后) */
static void reset_ball(void) {
    g.ball_stuck = true;
    g.stuck_frames = 0;
    g.ball_dx = 1;
    g.ball_dy = -1;
    g.ball_x = g.paddle_x + PADDLE_W / 2 - 1;
    g.ball_y = PADDLE_Y - BALL_SIZE;
}

static void launch_ball(void) {
    g.ball_stuck = false;
    /* 发射方向略偏, 增加趣味性 */
    g.ball_dx = (g.paddle_x + PADDLE_W / 2 < ARDUBOY_WIDTH / 2) ? 1 : -1;
    g.ball_dy = -1;
}

static void breakout_update(void) {
    uint8_t held    = arduboy_buttons_state();
    uint8_t pressed = arduboy_buttons_pressed();

    /* 游戏结束/胜利: 任意键重开 */
    if (g.game_over || g.win) {
        if (pressed & (ARDUBOY_BTN_A | ARDUBOY_BTN_B)) {
            breakout_init();
        }
        return;
    }

    /* 挡板移动 */
    if (held & ARDUBOY_BTN_A) g.paddle_x -= PADDLE_SPEED;
    if (held & ARDUBOY_BTN_B) g.paddle_x += PADDLE_SPEED;
    if (g.paddle_x < 0) g.paddle_x = 0;
    if (g.paddle_x > ARDUBOY_WIDTH - PADDLE_W)
        g.paddle_x = ARDUBOY_WIDTH - PADDLE_W;

    if (g.ball_stuck) {
        /* 球跟随挡板 */
        g.ball_x = g.paddle_x + PADDLE_W / 2 - 1;
        g.ball_y = PADDLE_Y - BALL_SIZE;
        g.stuck_frames++;
        /* 自动发射 或 按任意键发射 */
        if (g.stuck_frames >= STUCK_AUTO_LAUNCH_FRAMES ||
            (pressed & (ARDUBOY_BTN_A | ARDUBOY_BTN_B))) {
            launch_ball();
        }
        return;
    }

    /* 球移动 */
    g.ball_x += g.ball_dx;
    g.ball_y += g.ball_dy;

    /* 墙壁碰撞 */
    if (g.ball_x <= 0) {
        g.ball_x = 0;
        g.ball_dx = 1;
    } else if (g.ball_x >= ARDUBOY_WIDTH - BALL_SIZE) {
        g.ball_x = ARDUBOY_WIDTH - BALL_SIZE;
        g.ball_dx = -1;
    }
    if (g.ball_y <= 0) {
        g.ball_y = 0;
        g.ball_dy = 1;
    }

    /* 挡板碰撞 */
    if (g.ball_dy > 0 &&
        g.ball_y + BALL_SIZE >= PADDLE_Y &&
        g.ball_y + BALL_SIZE <= PADDLE_Y + PADDLE_H &&
        g.ball_x + BALL_SIZE >= g.paddle_x &&
        g.ball_x <= g.paddle_x + PADDLE_W) {
        g.ball_dy = -1;
        g.ball_y = PADDLE_Y - BALL_SIZE;
        /* 根据击中位置调整水平方向: 左 1/3 反弹左, 右 1/3 反弹右, 中间保持 */
        int hit = g.ball_x - g.paddle_x;
        if (hit < PADDLE_W / 3)       g.ball_dx = -1;
        else if (hit >= PADDLE_W * 2 / 3) g.ball_dx = 1;
    }

    /* 球落出底部 */
    if (g.ball_y > ARDUBOY_HEIGHT) {
        g.lives--;
        if (g.lives <= 0) {
            g.game_over = true;
        } else {
            reset_ball();
        }
        return;
    }

    /* 砖块碰撞 */
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (!g.bricks[r][c]) continue;
            int bx = BRICK_OFFSET_X + c * BRICK_W;
            int by = BRICK_OFFSET_Y + r * BRICK_H;
            if (g.ball_x + BALL_SIZE > bx &&
                g.ball_x < bx + BRICK_W &&
                g.ball_y + BALL_SIZE > by &&
                g.ball_y < by + BRICK_H) {
                g.bricks[r][c] = 0;
                g.score += 10;
                g.ball_dy = -g.ball_dy;  /* 简单反弹: 垂直翻转 */
                /* 检查胜利 */
                bool any = false;
                for (int rr = 0; rr < BRICK_ROWS && !any; rr++)
                    for (int cc = 0; cc < BRICK_COLS && !any; cc++)
                        if (g.bricks[rr][cc]) any = true;
                if (!any) g.win = true;
                return;
            }
        }
    }
}

static void breakout_render(void) {
    /* 砖块 (留 1px 间隔, 画 10x3 实心块) */
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (g.bricks[r][c]) {
                arduboy_fill_rect(BRICK_OFFSET_X + c * BRICK_W + 1,
                                  BRICK_OFFSET_Y + r * BRICK_H + 1,
                                  BRICK_W - 2, BRICK_H - 2,
                                  ARDUBOY_WHITE);
            }
        }
    }

    /* 挡板 */
    arduboy_fill_rect(g.paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H, ARDUBOY_WHITE);

    /* 球 */
    arduboy_fill_rect(g.ball_x, g.ball_y, BALL_SIZE, BALL_SIZE, ARDUBOY_WHITE);

    /* 顶部信息: 分数 / 生命 */
    char buf[20];
    snprintf(buf, sizeof(buf), "S:%d  L:%d", g.score, g.lives);
    /* 信息画在底部空白区 (y=63 一行, 但被挡板占用, 改画在砖块下方 y=27) */
    arduboy_set_cursor(0, 27);
    arduboy_print(buf);

    if (g.ball_stuck && !g.game_over && !g.win) {
        arduboy_set_cursor(40, 40);
        arduboy_print("READY");
    }

    if (g.game_over) {
        arduboy_fill_rect(20, 26, 88, 14, ARDUBOY_BLACK);
        arduboy_draw_rect(20, 26, 88, 14, ARDUBOY_WHITE);
        arduboy_set_cursor(34, 30);
        arduboy_print("GAME OVER");
        arduboy_set_cursor(28, 44);
        arduboy_print("PRESS TO RESET");
    }

    if (g.win) {
        arduboy_fill_rect(24, 26, 80, 14, ARDUBOY_BLACK);
        arduboy_draw_rect(24, 26, 80, 14, ARDUBOY_WHITE);
        arduboy_set_cursor(40, 30);
        arduboy_print("YOU WIN!");
        arduboy_set_cursor(28, 44);
        arduboy_print("PRESS TO RESET");
    }
}

const arduboy_game_impl_t breakout_game = {
    .init   = breakout_init,
    .update = breakout_update,
    .render = breakout_render,
    .name   = "Breakout",
};
