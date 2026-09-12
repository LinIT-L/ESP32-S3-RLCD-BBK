/**
 * keyboard.c — 标准 52 键(五排)虚拟键盘模块.
 *
 * 布局与虚拟键鼠 k76 全键盘完全一致(含 Fn 三档数字/F1-F12/符号, Caps/Shift 大小写).
 * 从 terminal.c 抽离, 供终端/密码输入等任意界面复用. Enter 拉长横跨两排, 方向键画三角箭头.
 */
#include "keyboard.h"
#include <string.h>

/* 与虚拟键鼠共用同一套 ASCII 字形 (定义于 menu_system.c) */
extern const uint8_t (*FONT8X12)[16];

#define KBD_GB_W 8
#define KBD_GB_H 16

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

/* ============ 编辑键盘布局 (收藏夹/编辑场景) ============
 * 整体行高压低 1/3; 删除并入退格(Bksp); 原删除位改为空格; 回车改名(Enter).
 * 排坐标 y 与 s_keys 同排, 但用 KBD_EDIT_ROW 排高绘制. 复用 s_keys 键位几何,
 * 仅把动作按需求映射. 为简洁直接复用 s_keys 布局表(键位一致), 只增加编辑专用绘制/命中. */
const kbd_t kbd_edit = { s_keys, (int)KBD_N, ST7305_WIDTH, false, false, 0 };

/* ================= 绘制原语 ================= */
static void kpx(st7305_handle_t* l, int x, int y, int w, int c){ if(x<0||x>=w)return; st7305_draw_pixel(l,x,y,c); }
static void kpf(st7305_handle_t* l, int w, int x0, int y0, int x1, int y1, int c){
    int t; if(x0>x1){t=x0;x0=x1;x1=t;} if(y0>y1){t=y0;y0=y1;y1=t;}
    for(int yy=y0;yy<=y1;yy++)for(int xx=x0;xx<=x1;xx++)kpx(l,xx,yy,w,c);
}
static void kdascii(st7305_handle_t* l, int w, int x, int y, const char* s, int fg){
    int cx=x; for(const unsigned char*p=(const unsigned char*)s;*p;p++){
        int idx=((*p>=' '&&*p<=0x7E)?(*p-0x20):('?'-0x20));
        const uint8_t*b=FONT8X12[idx];
        for(int cy=0;cy<KBD_GB_H;cy++){ uint8_t bits=b[cy]; for(int cc=0;cc<KBD_GB_W;cc++) if(bits&(0x80u>>cc)) kpx(l,cx+cc,y+cy,w,fg); }
        cx+=KBD_GB_W; }
}
static int kasw(const char* s){ int w=0; for(const unsigned char*p=(const unsigned char*)s;*p;p++) w+=KBD_GB_W; return w; }

/* Enter 拉长横跨两排 */
static int krows(const kbd_key_t* k){ return (k->act==KBD_ACT_ENTER)?2:1; }

/* 方向键三角箭头 (窄边在尖, 宽边在另一端) */
static void kdraw_arrow(st7305_handle_t* l, int w, int cx, int cy, int act, int inv){
    int c=inv?1:0;
    if(act==KBD_ACT_UP){        for(int r=0;r<8;r++)for(int o=-r;o<=r;o++)kpx(l,cx+o,cy-8+r,w,c); }
    else if(act==KBD_ACT_DOWN){ for(int r=0;r<8;r++)for(int o=-r;o<=r;o++)kpx(l,cx+o,cy-r,w,c); }
    else if(act==KBD_ACT_LEFT){ for(int r=0;r<8;r++)for(int o=-r;o<=r;o++)kpx(l,cx-8+r,cy+o,w,c); }
    else {                      for(int r=0;r<8;r++)for(int o=-r;o<=r;o++)kpx(l,cx-r,cy+o,w,c); }
}

static void kdraw_key(st7305_handle_t* l, int w, int base_y, const kbd_t* kb, int i, int inv, int row_h){
    const kbd_key_t* k=&kb->keys[i]; int rows=krows(k);
    int x0=k->x, x1=k->x+k->w-1, y0=base_y+k->y*row_h, y1=y0+rows*row_h-1;
    kpf(l,w,x0,y0,x1,y1,inv?0:1);                 /* 背景: 按下/激活反黑, 否则白 */
    kpf(l,w,x0,y0,x1,y0,inv?1:0); kpf(l,w,x0,y1,x1,y1,inv?1:0);
    kpf(l,w,x0,y0,x0,y1,inv?1:0); kpf(l,w,x1,y0,x1,y1,inv?1:0);
    if(k->act==KBD_ACT_UP||k->act==KBD_ACT_DOWN||k->act==KBD_ACT_LEFT||k->act==KBD_ACT_RIGHT){
        kdraw_arrow(l,w,x0+k->w/2,y0+(rows*row_h)/2,k->act,inv);
        return;
    }
    const char* lab=kbd_label(kb,i);
    int lw=kasw(lab); kdascii(l,w,x0+(k->w-lw)/2,y0+(rows*row_h-KBD_GB_H)/2,lab,inv?1:0);
}

/* 对外接口: 只绘制前 rows 排 (供需要去掉"Fn/方向键/空格"底排的界面, 如摩斯密码). */
void kbd_draw_rows(st7305_handle_t* l, int base_y, const kbd_t* k, int press, int rows){
    for(int i=0;i<k->n;i++){
        if(k->keys[i].y >= rows) continue;   /* 跳过被裁掉的排 */
        int inv=(i==press)||kbd_layer_on(k,i);
        kdraw_key(l,k->w,base_y,k,i,inv,KBD_ROW);
    }
}
void kbd_draw(st7305_handle_t* l, int base_y, const kbd_t* k, int press){
    kbd_draw_rows(l, base_y, k, press, KBD_ROWS);
}

/* 编辑键盘: 压低 1/3 行高绘制 (KBD_EDIT_ROW), 供收藏夹编辑抽屉用 */
void kbd_draw_edit(st7305_handle_t* l, int base_y, const kbd_t* k, int press){
    for(int i=0;i<k->n;i++){
        int inv=(i==press)||kbd_layer_on(k,i);
        kdraw_key(l,k->w,base_y,k,i,inv,KBD_EDIT_ROW);
    }
}

/* 命中: 仅在前 rows 排内 (与 kbd_draw_rows 裁剪一致) */
int kbd_hit_rows(const kbd_t* k, int base_y, int sx, int sy, int rows){
    int y=sy-base_y; if(y<0||y>=rows*KBD_ROW)return -1; int row=y/KBD_ROW;
    if(row>=rows) return -1;
    for(int i=0;i<k->n;i++){
        const kbd_key_t* e=&k->keys[i];
        if(e->y>=rows) continue;
        if(row>=e->y && row<e->y+krows(e) && sx>=e->x && sx<e->x+e->w) return i;
    }
    return -1;
}
int kbd_hit(const kbd_t* k, int base_y, int sx, int sy){
    return kbd_hit_rows(k, base_y, sx, sy, KBD_ROWS);
}

/* 编辑键盘命中 (KBD_EDIT_ROW 排高) */
int kbd_hit_edit(const kbd_t* k, int base_y, int sx, int sy){
    int y=sy-base_y; if(y<0)return -1; int row=y/KBD_EDIT_ROW;
    for(int i=0;i<k->n;i++){
        const kbd_key_t* e=&k->keys[i];
        if(row>=e->y && row<e->y+krows(e) && sx>=e->x && sx<e->x+e->w) return i;
    }
    return -1;
}

/* ===== 弹窗缩放键盘 (WiFi 密码等): 满宽布局缩放适配弹窗宽度 ===== */
void kbd_draw_edit_at(st7305_handle_t* l, int base_x, int base_y, int width, int row_h, const kbd_t* k, int press){
    const int full = (k && k->w>0) ? k->w : ST7305_WIDTH;
    for(int i=0;i<k->n;i++){
        const kbd_key_t* ke=&k->keys[i];
        int rows=(ke->act==KBD_ACT_ENTER)?2:1;
        int X0 = base_x + ke->x*width/full;
        int X1 = base_x + (ke->x+ke->w)*width/full - 1;
        if(X1<X0) X1=X0;
        int Y0=base_y+ke->y*row_h, Y1=Y0+rows*row_h-1;
        int inv=(i==press)||kbd_layer_on(k,i);
        kpf(l,k->w,X0,Y0,X1,Y1,inv?0:1);                 /* 背景 */
        kpf(l,k->w,X0,Y0,X1,Y0,inv?1:0); kpf(l,k->w,X0,Y1,X1,Y1,inv?1:0);
        kpf(l,k->w,X0,Y0,X0,Y1,inv?1:0); kpf(l,k->w,X1,Y0,X1,Y1,inv?1:0);
        if(ke->act==KBD_ACT_UP||ke->act==KBD_ACT_DOWN||ke->act==KBD_ACT_LEFT||ke->act==KBD_ACT_RIGHT){
            kdraw_arrow(l,k->w,(X0+X1)/2,Y0+rows*row_h/2,ke->act,inv); continue;
        }
        const char* lab=kbd_label(k,i);
        int lw=kasw(lab);
        kdascii(l,k->w,X0+(X1-X0+1-lw)/2,Y0+(rows*row_h-KBD_GB_H)/2,lab,inv?1:0);
    }
}
int kbd_hit_edit_at(const kbd_t* k, int base_x, int base_y, int width, int row_h, int sx, int sy){
    const int full = (k && k->w>0) ? k->w : ST7305_WIDTH;
    if(sy<base_y) return -1;
    int row=(sy-base_y)/row_h; if(row<0) return -1;
    for(int i=0;i<k->n;i++){
        const kbd_key_t* e=&k->keys[i];
        int rows=(e->act==KBD_ACT_ENTER)?2:1;
        if(row>=e->y && row<e->y+rows){
            int X0 = base_x + e->x*width/full;
            int X1 = base_x + (e->x+e->w)*width/full;
            if(sx>=X0 && sx<X1) return i;
        }
    }
    return -1;
}

int kbd_press(kbd_t* k, int idx){
    if(idx<0||idx>=k->n) return -1;
    switch(k->keys[idx].act){
        case KBD_ACT_CAPS:  k->caps=!k->caps; return KBD_ACT_CAPS;
        case KBD_ACT_SHIFT: k->shift=!k->shift; return KBD_ACT_SHIFT;
        case KBD_ACT_FN:    k->fn_mode=(k->fn_mode+1)%3; return KBD_ACT_FN;
        default:            return k->keys[idx].act;
    }
}

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

bool kbd_layer_on(const kbd_t* k, int idx){
    if(idx<0||idx>=k->n) return false;
    const kbd_key_t* e=&k->keys[idx];
    if(e->act==KBD_ACT_CAPS)  return k->caps;
    if(e->act==KBD_ACT_SHIFT) return k->shift;
    if(e->act==KBD_ACT_FN)    return k->fn_mode!=0;
    return false;
}