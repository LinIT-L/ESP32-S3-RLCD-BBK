#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "type.h"
#include "define.h"
#include "../include/lavaxvm.h"

typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef int LONG;

#define RGB8(r,g,b)  (((r)>>3)|(((g)>>3)<<5)|(((b)>>3)<<10))

typedef struct tagBITMAPFILEHEADER {
        //WORD    bfType;
        DWORD   bfSize;
        WORD    bfReserved1;
        WORD    bfReserved2;
        DWORD   bfOffBits;
} BITMAPFILEHEADER;

typedef struct tagBITMAPINFOHEADER{
        DWORD      biSize;
        LONG       biWidth;
        LONG       biHeight;
        WORD       biPlanes;
        WORD       biBitCount;
        DWORD      biCompression;
        DWORD      biSizeImage;
        LONG       biXPelsPerMeter;
        LONG       biYPelsPerMeter;
        DWORD      biClrUsed;
        DWORD      biClrImportant;
} BITMAPINFOHEADER;

static word ScreenBuffer[LCD_WIDTH*(LCD_HEIGHT+64)];
static word patbuf[16*16];

static void host_path(const char *path, char *buffer, size_t size)
{
    const char *resolved = lavax_host_path(path, buffer, size);
    if (!resolved)
        buffer[0] = 0;
    else if (resolved != buffer)
        snprintf(buffer, size, "%s", resolved);
}
static int col,line;
static word fgcolor,bgcolor;

word *GET_BUF_ADDR(int x,int y)
{
	return ScreenBuffer+y*LCD_WIDTH+x;
}

void G16DrawRect(int x,int y,int w,int h,word color)
{
	word *p;
	int i,j;
	p=GET_BUF_ADDR(x,y);
	for (i=0;i<h;i++) {
		j=w;
		while (j--) {
			*p++=color;
		}
		p+=LCD_WIDTH-w;
	}
}

void G16DrawAlpha(int x,int y,int w,int h,int alpha)
{
	word *p;
	int i,j;
	p=GET_BUF_ADDR(x,y);
	if (alpha) {
		for (i=0;i<h;i++) {
			j=w;
			while (j--) {
				*p++|=0x8000;
			}
			p+=LCD_WIDTH-w;
		}
	} else {
		for (i=0;i<h;i++) {
			j=w;
			while (j--) {
				*p++&=0x7fff;
			}
			p+=LCD_WIDTH-w;
		}
	}
}

void G16DrawImage(int x,int y,int w,int h,word *img)
{
	word *p;
	int i,j;
	p=GET_BUF_ADDR(x,y);
	for (i=0;i<h;i++) {
		j=w;
		while (j--) {
			*p++=*img++;
		}
		p+=LCD_WIDTH-w;
	}
}

static void Font6x12(int x,int y,byte c,word fgcolor,word bgcolr)
{
	byte *addr,t;
	int i;
	addr=ascii+c*12;
	for (i=0;i<12;i++) {
		t=*addr++;
		patbuf[i*6]=(t&0x80) ? fgcolor : bgcolr;
		patbuf[i*6+1]=(t&0x40) ? fgcolor : bgcolr;
		patbuf[i*6+2]=(t&0x20) ? fgcolor : bgcolr;
		patbuf[i*6+3]=(t&0x10) ? fgcolor : bgcolr;
		patbuf[i*6+4]=(t&0x8) ? fgcolor : bgcolr;
		patbuf[i*6+5]=(t&0x4) ? fgcolor : bgcolr;
	}
	G16DrawImage(x,y,6,12,patbuf);
}

static void Font12x12(int x,int y,byte c1,byte c2,word fgcolor,word bgcolor)
{
	byte *addr,t;
	int i,j;
	if (c1<0xb0) addr=gbfont+(((c1-0xa1)*94+(c2-0xa1))*24);
	else addr=gbfont+(((c1-0xa7)*94+(c2-0xa1))*24);
	for (i=0,j=0;i<24;i++) {
		t=*addr++;
		patbuf[j++]=(t&0x80) ? fgcolor : bgcolor;
		patbuf[j++]=(t&0x40) ? fgcolor: bgcolor;
		patbuf[j++]=(t&0x20) ? fgcolor: bgcolor;
		patbuf[j++]=(t&0x10) ? fgcolor: bgcolor;
		if (i&1) continue;
		patbuf[j++]=(t&0x8) ? fgcolor : bgcolor;
		patbuf[j++]=(t&0x4) ? fgcolor: bgcolor;
		patbuf[j++]=(t&0x2) ? fgcolor: bgcolor;
		patbuf[j++]=(t&0x1) ? fgcolor: bgcolor;
	}
	G16DrawImage(x,y,12,12,patbuf);
}

static void Font8x16(int x,int y,byte c,word fgcolor,word bgcolor)
{
	byte *addr;
	byte t;
	int i;
	addr=ascii8+c*16;
	for (i=0;i<16;i++) {
		t=*addr++;
		patbuf[i*8]=(t&0x80) ? fgcolor : bgcolor;
		patbuf[i*8+1]=(t&0x40) ? fgcolor : bgcolor;
		patbuf[i*8+2]=(t&0x20) ? fgcolor : bgcolor;
		patbuf[i*8+3]=(t&0x10) ? fgcolor : bgcolor;
		patbuf[i*8+4]=(t&0x8) ? fgcolor : bgcolor;
		patbuf[i*8+5]=(t&0x4) ? fgcolor : bgcolor;
		patbuf[i*8+6]=(t&0x2) ? fgcolor : bgcolor;
		patbuf[i*8+7]=(t&0x1) ? fgcolor : bgcolor;
	}
	G16DrawImage(x,y,8,16,patbuf);
}

static void Font16x16(int x,int y,byte c1,byte c2,word fgcolor,word bgcolor)
{
	byte *addr;
	byte t;
	int i;
	if (c1<0xb0) addr=gbfont16+(((c1-0xa1)*94+(c2-0xa1))*32);
	else addr=gbfont16+(((c1-0xa7)*94+(c2-0xa1))*32);
	for (i=0;i<32;i++) {
		t=*addr++;
		patbuf[i*8]=(t&0x80) ? fgcolor : bgcolor;
		patbuf[i*8+1]=(t&0x40) ? fgcolor : bgcolor;
		patbuf[i*8+2]=(t&0x20) ? fgcolor : bgcolor;
		patbuf[i*8+3]=(t&0x10) ? fgcolor : bgcolor;
		patbuf[i*8+4]=(t&0x8) ? fgcolor : bgcolor;
		patbuf[i*8+5]=(t&0x4) ? fgcolor : bgcolor;
		patbuf[i*8+6]=(t&0x2) ? fgcolor : bgcolor;
		patbuf[i*8+7]=(t&0x1) ? fgcolor : bgcolor;
	}
	G16DrawImage(x,y,16,16,patbuf);
}

void G16Locate(int y,int x)
{
	line=y;
	col=x;
}

void G16PrintSamll(char *str,word fgcolor,word bgcolor)
{
	byte *p,c,c2;
	int x,y;
	p=(byte*)str;
	y=line*16+2;
	x=col*6+8;
	while ((c=*p++)) {
		if (c<128) {
			if (c=='\r') {
				continue;
			}
			if (c=='\n') {
				x=8;
				y+=16;
				continue;
			}
			if (x+6>248) {
				x=8;
				y+=16;
			}
			Font6x12(x,y,c,fgcolor,bgcolor);
			x+=6;
		} else {
			c2=*p++;
			if (c2==0) {
				break;
			}
			if (x+12>248) {
				x=8;
				y+=16;
			}
			Font12x12(x,y,c,c2,fgcolor,bgcolor);
			x+=12;
		}
	}
	line=(y-2)/16;
	col=(x-8)/6;
}

static int next_line(int y)
{
	if (y>=LCD_HEIGHT) {
		y+=16;
		if (y>=LCD_HEIGHT+VKEY_START) {
			memmove(ScreenBuffer+LCD_WIDTH*LCD_HEIGHT,ScreenBuffer+LCD_WIDTH*(LCD_HEIGHT+16),LCD_WIDTH*(VKEY_START-16)*2);
			G16DrawRect(0,LCD_HEIGHT+VKEY_START-16,LCD_WIDTH,16,0x8000);
			y-=16;
		}
	} else {
		y+=16;
	}
	return y;
}

void G16Print(char *str,word fgcolor,word bgcolor)
{
	byte *p,c,c2;
	int x,y;
	p=(byte*)str;
	y=line*16;
	x=col*8+8;
	while ((c=*p++)) {
		if (c<128) {
			if (c=='\r') {
				continue;
			}
			if (c=='\n') {
				x=8;
				y=next_line(y);
				continue;
			}
			if (x+8>248) {
				x=8;
				y=next_line(y);
			}
			Font8x16(x,y,c,fgcolor,bgcolor);
			x+=8;
		} else {
			c2=*p++;
			if (c2==0) {
				break;
			}
			if (x+16>248) {
				x=8;
				y=next_line(y);
			}
			Font16x16(x,y,c,c2,fgcolor,bgcolor);
			x+=16;
		}
	}
	line=y/16;
	col=(x-8)/8;
}

void G16SetFgColor(word color)
{
	fgcolor=color;
}

void G16SetBgColor(word color)
{
	bgcolor=color;
}

void G16Printf(char *format, ...)
{
	va_list aptr;
	char buffer[4096];

	va_start(aptr, format);
	vsprintf(buffer, format, aptr);
	va_end(aptr);

	G16Print(buffer,fgcolor,bgcolor);
	G16Refresh();
}

void G16DPrintf(char *format, ...)
{
	va_list aptr;
	char buffer[4096];

	va_start(aptr, format);
	vsprintf(buffer, format, aptr);
	va_end(aptr);

	if (line<12 || line>=12+VKEY_START/16) {
		line=12;
		col=0;
	}
	G16Print(buffer,fgcolor,bgcolor);
	G16Refresh();
}

void G16Refresh()
{
	lavax_platform_present_rgb555(ScreenBuffer, LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH,
	                               LCD_WIDTH, LCD_HEIGHT, 1, 0);
}

int G16LoadBmp(char *fname,word *gbuf,int w,int h)
{
	FILE *fp;
	char host_name[256];
	BITMAPFILEHEADER bfh;
	BITMAPINFOHEADER bih;
	int i,j,width,height,widths;
	word *g;
	byte buf[4];
	word c16;

	host_path(fname, host_name, sizeof(host_name));
	fp=fopen(host_name,"rb");
	if (!fp) {
		return 1;
	}
	fread(buf,1,2,fp);
	if (buf[0]!='B' && buf[1]!='M') {
		fclose(fp);
		return 1;
	}
	fread(&bfh,1,sizeof(bfh),fp);
	fread(&bih,1,sizeof(bih),fp);
	width=bih.biWidth;
	height=bih.biHeight;
	if (width!=w || height!=h) {
		fclose(fp);
		return 1;
	}

	if (bih.biBitCount==24) {
		g=gbuf;
		widths=(width*3+3)&~3;
		for (i=0;i<height;i++) {
			fseek(fp,bfh.bfOffBits+(height-1-i)*widths,SEEK_SET);
			for (j=0;j<width;j++) {
				fread(buf,1,3,fp);
				*g++=RGB8(buf[2],buf[1],buf[0])|0x8000;
			}
			g+=256-w;
		}
	} else if (bih.biBitCount==16) {
		g=gbuf;
		widths=(width*2+3)&~3;
		for (i=0;i<height;i++) {
			fseek(fp,bfh.bfOffBits+(height-1-i)*widths,SEEK_SET);
			for (j=0;j<width;j++) {
				fread(&c16,1,2,fp);
				*g++=((c16&0x7c00)>>10)|((c16&0x3e0)>>0)|((c16&0x1f)<<10)|0x8000;
			}
			g+=256-w;
		}
	} else {
		fclose(fp);
		return 1;
	}

	fclose(fp);
	return 0;
}

int G16LoadWallPaper(char *fname)
{
	return G16LoadBmp(fname,ScreenBuffer,256,192);
}
