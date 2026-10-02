/**
 * keyboard.c — 键盘「统一输入」部分 (布局 + 状态 + 字符/动作映射 + 几何命中).
 *
 * 与 kbd_ui.c (绘制) 拆分: 本文件只负责"输入模型"——键位布局表 (kbd_std/kbd_edit),
 * 按键状态 (kbd_t), 以及 索引→字符/动作 的映射 (kbd_press/kbd_char/kbd_label/
 * kbd_layer_on) 与 屏幕坐标→键索引 的几何命中 (kbd_hit*). 不依赖任何绘制, 可在
 * 任意 UI 渲染器下复用. 新增键盘外观只需在 kbd_ui.c 加渲染器/调不同参数, 不必重造
 * 输入逻辑 —— 这正是"适配不同情况、不生成很多不同键盘"的落点.
 */
#include "keyboard.h"

/* ================= 标准 52 键布局表 (与虚拟键鼠 k76 全键盘一致) =================
 * fn = Fn 档1(F1-F12) 标签; sym = Fn 档2(符号) 文本; act 码见 keyboard.h */
static const kbd_key_t s_keys[] = {
    {"Esc",0,KBD_ACT_ESC,0,28,0,0,0},{"1",'1',KBD_ACT_CHAR,28,31,0,"F1","`"},{"2",'2',KBD_ACT_CHAR,59,31,0,"F2","~"},{"3",'3',KBD_ACT_CHAR,90,31,0,"F3","["},
    {"4",'4',KBD_ACT_CHAR,121,31,0,"F4","]"},{"5",'5',KBD_ACT_CHAR,152,31,0,"F5","{"},{"6",'6',KBD_ACT_CHAR,183,31,0,"F6","}"},{"7",'7',KBD_ACT_CHAR,214,31,0,"F7",";"},
    {"8",'8',KBD_ACT_CHAR,245,31,0,"F8","'"},{"9",'9',KBD_ACT_CHAR,276,31,0,"F9",","},{"0",'0',KBD_ACT_CHAR,307,31,0,"F10","."},{".",'.',KBD_ACT_CHAR,338,31,0,"F11","/"},{"?",'?',KBD_ACT_CHAR,369,31,0,"F12","?"},
    {"Tab",0,KBD_ACT_TAB,0,34,1,0,0},{"Q",'q',KBD_ACT_CHAR,34,33,1,0,0},{"W",'w',KBD_ACT_CHAR,67,33,1,0,0},{"E",'e',KBD_ACT_CHAR,100,33,1,0,0},{"R",'r',KBD_ACT_CHAR,133,33,1,0,0},
    {"T",'t',KBD_ACT_CHAR,166,33,1,0,0},{"Y",'y',KBD_ACT_CHAR,199,33,1,0,0},{"U",'u',KBD_ACT_CHAR,232,33,1,0,0},{"I",'i',KBD_ACT_CHAR,265,33,1,0,0},{"O",'o',KBD_ACT_CHAR,298,33,1,0,0},
    {"P",'p',KBD_ACT_CHAR,331,33,1,0,0},{"Bksp",0,KBD_ACT_BKSP,364,36,1,0,0},
    {"Caps",0,KBD_ACT_CAPS,0,54,2,0,0},{"A",'a',KBD_ACT_CHAR,54,33,2,0,0},{"S",'s',KBD_ACT_CHAR,87,33,2,0,0},{"D",'d',KBD_ACT_CHAR,120,33,2,0,0},{"F",'f',KBD_ACT_CHAR,153,33,2,0,0},
    {"G",'g',KBD_ACT_CHAR,186,33,2,0,0},{"H",'h',KBD_ACT_CHAR,219,33,2,0,0},{"J",'j',KBD_ACT_CHAR,252,33,2,0,0},{"K",'k',KBD_ACT_CHAR,285,33,2,0,0},{"L",'l',KBD_ACT_CHAR,318,32,2,0,0},
    {"Enter",0,KBD_ACT_ENTER,350,50,2,0,0},
    {"Shift",0,KBD_ACT_SHIFT,0,70,3,0,0},{"Z",'z',KBD_ACT_CHAR,70,34,3,0,0},{"X",'x',KBD_ACT_CHAR,104,34,3,0,0},{"C",'c',KBD_ACT_CHAR,138,34,3,0,0},{"V",'v',KBD_ACT_CHAR,172,34,3,0,0},
    {"B",'b',KBD_ACT_CHAR,206,34,3,0,0},{"N",'n',KBD_ACT_CHAR,240,33,3,0,0},{"M",'m',KBD_ACT_CHAR,273,32,3,0,0},{"^",0,KBD_ACT_UP,305,45,3,0,0},                       /* 第4排末 → ↑ */
    {"Fn",0,KBD_ACT_FN,0,50,4,0,0},{"Ctrl",0,KBD_ACT_CTRL,50,50,4,0,0},{"Alt",0,KBD_ACT_ALT,100,50,4,0,0},{"Space",0,KBD_ACT_SPACE,150,110,4,0,0},
    {"<-",0,KBD_ACT_LEFT,260,45,4,0,0},{"v",0,KBD_ACT_DOWN,305,45,4,0,0},{"->",0,KBD_ACT_RIGHT,350,50,4,0,0},                                          /* 第5排: Fn Ctrl Alt Space ← ↓ → */
};
#define KBD_N (sizeof(s_keys)/sizeof(s_keys[0]))

const kbd_t kbd_std = { s_keys, (int)KBD_N, ST7305_WIDTH, false, false, 0 };

/* 编辑键盘布局 (复用 s_keys 键位几何, 仅用 KBD_EDIT_ROW 排高绘制/命中) */
const kbd_t kbd_edit = { s_keys, (int)KBD_N, ST7305_WIDTH, false, false, 0 };

/* ===== 按压逻辑: 对 Caps/Shift/Fn 翻转内部状态, 其余返回动作码 ===== */
int kbd_press(kbd_t* k, int idx){
    if(idx<0||idx>=k->n) return -1;
    switch(k->keys[idx].act){
        case KBD_ACT_CAPS:  k->caps=!k->caps; return KBD_ACT_CAPS;
        case KBD_ACT_SHIFT: k->shift=!k->shift; return KBD_ACT_SHIFT;
        case KBD_ACT_FN:    k->fn_mode=(k->fn_mode+1)%3; return KBD_ACT_FN;
        default:            return k->keys[idx].act;
    }
}

/* 读取当前状态下的可输入字符 (仅 KBD_ACT_CHAR; Fn 功能档或特殊键返回 0) */
char kbd_char(const kbd_t* k, int idx){
    if(idx<0||idx>=k->n) return 0;
    const kbd_key_t* e=&k->keys[idx];
    if(e->act!=KBD_ACT_CHAR) return 0;
    char c;
    if(k->fn_mode==1 && e->fn){ return 0; }              /* F11/F12 等功能键档, 不输入字符 */
    else if(k->fn_mode==2 && e->sym){ c=e->sym[0]; }
    else { c=e->ch; if((k->caps||k->shift)&&c>='a'&&c<='z') c=(char)(c-'a'+'A'); }
    return c;
}

/* 键当前应显示标签 (Fn 档/Caps/Shift 联动) */
const char* kbd_label(const kbd_t* k, int idx){
    if(idx<0||idx>=k->n) return "";
    const kbd_key_t* e=&k->keys[idx];
    if(e->act==KBD_ACT_FN){ static const char* f[3]={"Fn","Fn1","Fn2"}; return f[k->fn_mode%3]; }
    if(e->ch && k->fn_mode==1 && e->fn) return e->fn;
    if(e->ch && k->fn_mode==2 && e->sym) return e->sym;
    if(e->ch>='a'&&e->ch<='z'){ static char b[2]; b[0]=(k->caps||k->shift)?(char)(e->ch-'a'+'A'):e->ch; b[1]=0; return b; }
    if(e->ch){ static char b[2]; b[0]=e->ch; b[1]=0; return b; }
    return e->label;
}

/* 键是否处于激活层 (Caps/Shift/Fn 开启时对应键整键反黑) */
bool kbd_layer_on(const kbd_t* k, int idx){
    if(idx<0||idx>=k->n) return false;
    const kbd_key_t* e=&k->keys[idx];
    if(e->act==KBD_ACT_CAPS)  return k->caps;
    if(e->act==KBD_ACT_SHIFT) return k->shift;
    if(e->act==KBD_ACT_FN)    return k->fn_mode!=0;
    return false;
}

/* ===== 统一几何命中: 屏幕坐标 → 键索引 (核心, 其余 kbd_hit* 皆委托此) =====
 * base_x/base_y: 键盘绘制原点偏移; draw_w: 布局映射到的像素宽 (满宽= k->w, 弹窗= width);
 * row_h: 每排高 (KBD_ROW / KBD_EDIT_ROW / 自定义); rows: 参与命中的排数 (<=0 表示不裁)。 */
static int kbd_hit_at(const kbd_t* k, int base_x, int base_y, int draw_w, int row_h, int rows, int sx, int sy){
    int y = sy - base_y; if(y<0) return -1;
    int row = y / row_h;
    if(rows>0 && row>=rows) return -1;
    const int full = (k && k->w>0) ? k->w : ST7305_WIDTH;
    for(int i=0;i<k->n;i++){
        const kbd_key_t* e=&k->keys[i];
        if(rows>0 && e->y>=rows) continue;
        int spans=(e->act==KBD_ACT_ENTER)?2:1;
        if(row>=e->y && row<e->y+spans){
            int X0 = base_x + e->x*draw_w/full;
            int X1 = base_x + (e->x+e->w)*draw_w/full;
            if(sx>=X0 && sx<X1) return i;
        }
    }
    return -1;
}

/* 命中: 屏幕坐标 → 键索引(-1 未命中) */
int kbd_hit_rows(const kbd_t* k, int base_y, int sx, int sy, int rows){
    return kbd_hit_at(k, 0, base_y, k->w, KBD_ROW, rows, sx, sy);
}
int kbd_hit(const kbd_t* k, int base_y, int sx, int sy){
    return kbd_hit_at(k, 0, base_y, k->w, KBD_ROW, KBD_ROWS, sx, sy);
}
/* 编辑键盘命中 (KBD_EDIT_ROW 排高, 不裁排) */
int kbd_hit_edit(const kbd_t* k, int base_y, int sx, int sy){
    return kbd_hit_at(k, 0, base_y, k->w, KBD_EDIT_ROW, -1, sx, sy);
}
/* 弹窗缩放命中 (缩放规则同 kbd_draw_edit_at) */
int kbd_hit_edit_at(const kbd_t* k, int base_x, int base_y, int width, int row_h, int sx, int sy){
    return kbd_hit_at(k, base_x, base_y, width, row_h, -1, sx, sy);
}
