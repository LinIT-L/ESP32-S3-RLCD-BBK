#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "book_flow.h"
typedef struct { const uint8_t *d; uint32_t len; } mf_t;
static size_t mr(void *ud, uint32_t pos, uint8_t *b, size_t w){ mf_t*m=ud; if(pos>=m->len)return 0; size_t x=w; if(x>m->len-pos)x=m->len-pos; memcpy(b,m->d+pos,x); return x; }
int main(void){
  uint32_t len=512*1024;
  uint8_t *d=malloc(len); uint32_t x=12345;
  for(uint32_t i=0;i<len;i++){ x=x*1664525u+1013904223u; d[i]=(uint8_t)(x>>24); }
  mf_t m={d,len}; bf_src_t src={mr,&m};
  static uint32_t pages[100]; uint32_t pn=0;
  uint8_t *chunk=malloc(32768), *brk=malloc(BF_MAX_WIN), *scr=malloc(BF_MAX_WIN*8+8);
  bf_ch_t *win=malloc(sizeof(bf_ch_t)*BF_MAX_WIN); uint32_t *off=malloc(BF_MAX_WIN*4);
  /* 手动跑第一窗口 */
  int n=0; uint32_t cpos=0, wb=0, wl=0;
  while(n<BF_MAX_WIN && cpos<len){
    if(cpos+4>wb+wl || cpos<wb){ uint32_t want=32768; if(want>len-cpos)want=len-cpos; size_t g=mr(&m,cpos,chunk,want); if(!g)break; wb=cpos; wl=(uint32_t)g; }
    bf_ch_t ch=bf_next_ch(chunk+(cpos-wb), chunk+wl, BF_ENC_UTF8);
    if(ch.adv==0){ printf("adv=0 at n=%d cpos=%u\n", n, cpos); break; }
    win[n]=ch; off[n]=cpos; n++; cpos+=ch.adv;
  }
  printf("first window n=%d cpos=%u fsz=%u\n", n, cpos, len);
  bf_breaks(win,n,BF_ENC_UTF8,brk,scr,BF_MAX_WIN*8+8);
  int le[BF_MAX_LINES],lc,bd;
  bf_layout(win,n,brk,18,384,le,&lc,&bd);
  printf("lc=%d boundary=%d n=%d\n", lc, bd, n);
  return 0;
}
