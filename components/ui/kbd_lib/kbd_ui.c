/**
 * kbd_ui.c — 键盘「UI 渲染」部分 (绘制 + 弹窗缩放).
 *
 * 与 keyboard.c (输入模型) 拆分: 本文件只负责"把统一输入模型画出来", 不含任何
 * 字符/动作逻辑 (那部分在 keyboard.c). 所有对外绘制接口 (kbd_draw / kbd_draw_rows
 * / kbd_draw_edit / kbd_draw_edit_at) 均委托给单一内部渲染器 kbd_render, 由参数
 * (base_x / draw_w / row_h / rows) 适配满宽 / 编辑 / 弹窗缩放等任意场景, 不再为每个
 * 场景复制一份绘制代码 —— 新增外观只需加一个 thin wrapper 或调不同参数, 不必重造键盘.
 */
#include "keyboard.h"
#include <string.h>

/* 与虚拟键鼠共用同一套 ASCII 字形 (定义于 menu_system.c) */
extern const uint8_t (*FONT8X12)[16];

#define KBD_GB_W 8
#define KBD_GB_H 16

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

/* 方向键三角箭头 (尖端在前, 底边在后)
 * ★ 2026-10-21 修"箭头不在格子中心": 原来 UP 从 cy-8 往上画 8 行、DOWN 从 cy 往上画,
 *   三角形重心比格子中心高 4px (左右同理偏左 4px)。改成以 cy 为中心上下各 4 行。 */
static void kdraw_arrow(st7305_handle_t* l, int w, int cx, int cy, int act, int inv){
    int c=inv?1:0;
    if(act==KBD_ACT_UP){                 /* 尖在上, 逐行变宽 */
        for(int r=0;r<8;r++) for(int o=-r;o<=r;o++) kpx(l,cx+o,cy-4+r,w,c);
    } else if(act==KBD_ACT_DOWN){        /* 尖在下: 顶行最宽 */
        for(int r=0;r<8;r++){ int hw=7-r; for(int o=-hw;o<=hw;o++) kpx(l,cx+o,cy-4+r,w,c); }
    } else if(act==KBD_ACT_LEFT){        /* 尖在左 */
        for(int r=0;r<8;r++){ int hw=r; for(int o=-hw;o<=hw;o++) kpx(l,cx-4+r,cy+o,w,c); }
    } else {                             /* 尖在右 */
        for(int r=0;r<8;r++){ int hw=r; for(int o=-hw;o<=hw;o++) kpx(l,cx+3-r,cy+o,w,c); }
    }
}

/* ===== 统一渲染: 屏幕原点 (base_x,base_y) + 映射宽 draw_w + 每排高 row_h + 参与渲染排数 rows(<=0 不裁) =====
 * draw_w = k->w 即满宽(比例=1); 弹窗场景 draw_w = 弹窗宽 即横向缩放. 其余对外接口皆委托此. */
static void kbd_render(st7305_handle_t* l, int base_x, int base_y, int draw_w, int row_h, int rows, const kbd_t* k, int press){
    const int full = (k && k->w>0) ? k->w : ST7305_WIDTH;
    for(int i=0;i<k->n;i++){
        const kbd_key_t* ke=&k->keys[i];
        if(rows>0 && ke->y>=rows) continue;
        int spans=(ke->act==KBD_ACT_ENTER)?2:1;
        int X0 = base_x + ke->x*draw_w/full;
        int X1 = base_x + (ke->x+ke->w)*draw_w/full - 1;
        if(X1<X0) X1=X0;
        int Y0=base_y+ke->y*row_h, Y1=Y0+spans*row_h-1;
        int inv=(i==press)||kbd_layer_on(k,i);
        kpf(l,k->w,X0,Y0,X1,Y1,inv?0:1);                 /* 背景: 按下/激活反黑, 否则白 */
        kpf(l,k->w,X0,Y0,X1,Y0,inv?1:0); kpf(l,k->w,X0,Y1,X1,Y1,inv?1:0);
        kpf(l,k->w,X0,Y0,X0,Y1,inv?1:0); kpf(l,k->w,X1,Y0,X1,Y1,inv?1:0);
        if(ke->act==KBD_ACT_UP||ke->act==KBD_ACT_DOWN||ke->act==KBD_ACT_LEFT||ke->act==KBD_ACT_RIGHT){
            kdraw_arrow(l,k->w,(X0+X1)/2,Y0+spans*row_h/2,ke->act,inv);
            continue;
        }
        const char* lab=kbd_label(k,i);
        int lw=kasw(lab); kdascii(l,k->w,X0+(X1-X0+1-lw)/2,Y0+(spans*row_h-KBD_GB_H)/2,lab,inv?1:0);
    }
}

/* ================= 对外接口 (全部委托 kbd_render, 参数化适配不同场景) ================= */
/* 只绘制前 rows 排 (供需要去掉"Fn/方向键/空格"底排的界面, 如摩斯密码). */
void kbd_draw_rows(st7305_handle_t* l, int base_y, const kbd_t* k, int press, int rows){
    kbd_render(l, 0, base_y, k->w, KBD_ROW, rows, k, press);
}
void kbd_draw(st7305_handle_t* l, int base_y, const kbd_t* k, int press){
    kbd_render(l, 0, base_y, k->w, KBD_ROW, KBD_ROWS, k, press);
}
/* 编辑键盘: 压低 1/3 行高 (KBD_EDIT_ROW), 绘制全部排 (收藏夹/编辑场景用) */
void kbd_draw_edit(st7305_handle_t* l, int base_y, const kbd_t* k, int press){
    kbd_render(l, 0, base_y, k->w, KBD_EDIT_ROW, -1, k, press);
}
/* 弹窗缩放绘制: 满宽布局缩放平移到 [base_x, base_x+width-1] 区域, 排高 row_h */
void kbd_draw_edit_at(st7305_handle_t* l, int base_x, int base_y, int width, int row_h, const kbd_t* k, int press){
    kbd_render(l, base_x, base_y, width, row_h, -1, k, press);
}

/* ================= 统一样式入口 (见 keyboard.h 说明) =================
 * 这 5 个函数是"唯一推荐用法": draw 与 hit 共用同一个 kbd_geom_t,
 * 结构上不可能出现"画用一种几何、判命用另一种"的错配。 */

const char* kbd_style_name(kbd_style_t s){
    switch(s){
        case KBD_STYLE_EDIT:   return "edit";
        case KBD_STYLE_ROWS:   return "rows";
        case KBD_STYLE_DIALOG: return "dialog";
        default:               return "std";
    }
}
int kbd_style_by_name(const char* name){
    if(!name) return -1;
    if(name[0]=='s'||name[0]=='S') return KBD_STYLE_STD;
    if(name[0]=='e'||name[0]=='E') return KBD_STYLE_EDIT;
    if(name[0]=='r'||name[0]=='R') return KBD_STYLE_ROWS;
    if(name[0]=='d'||name[0]=='D') return KBD_STYLE_DIALOG;
    return -1;
}
/* 按样式填默认几何: y 由调用方给, 其余取该样式的常规值 */
void kbd_geom_default(kbd_geom_t* g, kbd_style_t s, int y){
    if(!g) return;
    g->style = s;
    g->x     = 0;
    g->y     = y;
    g->w     = ST7305_WIDTH;
    g->row_h = (s == KBD_STYLE_EDIT) ? KBD_EDIT_ROW : KBD_ROW;
    g->rows  = KBD_ROWS;
}
int kbd_style_height(kbd_style_t s, int rows, int row_h){
    switch(s){
        case KBD_STYLE_EDIT:   return KBD_EDIT_ROW * KBD_ROWS;
        case KBD_STYLE_ROWS:   return KBD_ROW * ((rows > 0 && rows <= KBD_ROWS) ? rows : KBD_ROWS);
        case KBD_STYLE_DIALOG: return (row_h > 0 ? row_h : KBD_ROW) * KBD_ROWS;
        default:               return KBD_H;
    }
}
void kbd_draw_style(st7305_handle_t* l, const kbd_geom_t* g, const kbd_t* k, int press){
    if(!l || !g || !k) return;
    switch(g->style){
        case KBD_STYLE_EDIT:   kbd_draw_edit(l, g->y, k, press); break;
        case KBD_STYLE_ROWS:   kbd_draw_rows(l, g->y, k, press,
                                             (g->rows > 0) ? g->rows : KBD_ROWS); break;
        case KBD_STYLE_DIALOG: kbd_draw_edit_at(l, g->x, g->y, g->w,
                                               (g->row_h > 0) ? g->row_h : KBD_ROW, k, press); break;
        default:               kbd_draw(l, g->y, k, press); break;
    }
}
int kbd_hit_style(const kbd_t* k, const kbd_geom_t* g, int sx, int sy){
    if(!k || !g) return -1;
    switch(g->style){
        case KBD_STYLE_EDIT:   return kbd_hit_edit(k, g->y, sx, sy);
        case KBD_STYLE_ROWS:   return kbd_hit_rows(k, g->y, sx, sy,
                                                   (g->rows > 0) ? g->rows : KBD_ROWS);
        case KBD_STYLE_DIALOG: return kbd_hit_edit_at(k, g->x, g->y, g->w,
                                                      (g->row_h > 0) ? g->row_h : KBD_ROW, sx, sy);
        default:               return kbd_hit(k, g->y, sx, sy);
    }
}
