#include <string.h>
#include "myctype.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "define.h"
#include "../include/lavaxvm.h"
#include "rg_stub.h"
#include "esp_attr.h"

#define NO_RAM_ALIGN 1

typedef void (*CODES)();

#define LTRUE 0xffffffff
#define LFALSE 0

#define CMDLINE	0x700

a32 TextBuffer,string_stack,eval_stack;

byte patbuf[0x400];
byte *ram_base;
byte *pLAVA;

const word sin90[]={
	0,18,36,54,71,89,107,125,
	143,160,178,195,213,230,248,265,
	282,299,316,333,350,367,384,400,
	416,433,449,465,481,496,512,527,
	543,558,573,587,602,616,630,644,
	658,672,685,698,711,724,737,749,
	761,773,784,796,807,818,828,839,
	849,859,868,878,887,896,904,912,
	920,928,935,943,949,956,962,968,
	974,979,984,989,994,998,1002,1005,
	1008,1011,1014,1016,1018,1020,1022,1023,
	1023,1024,1024
};

const byte crc1[256]={
	0x0,0x21,0x42,0x63,0x84,0xa5,0xc6,0xe7,
	0x8,0x29,0x4a,0x6b,0x8c,0xad,0xce,0xef,
	0x31,0x10,0x73,0x52,0xb5,0x94,0xf7,0xd6,
	0x39,0x18,0x7b,0x5a,0xbd,0x9c,0xff,0xde,
	0x62,0x43,0x20,0x1,0xe6,0xc7,0xa4,0x85,
	0x6a,0x4b,0x28,0x9,0xee,0xcf,0xac,0x8d,
	0x53,0x72,0x11,0x30,0xd7,0xf6,0x95,0xb4,
	0x5b,0x7a,0x19,0x38,0xdf,0xfe,0x9d,0xbc,
	0xc4,0xe5,0x86,0xa7,0x40,0x61,0x2,0x23,
	0xcc,0xed,0x8e,0xaf,0x48,0x69,0xa,0x2b,
	0xf5,0xd4,0xb7,0x96,0x71,0x50,0x33,0x12,
	0xfd,0xdc,0xbf,0x9e,0x79,0x58,0x3b,0x1a,
	0xa6,0x87,0xe4,0xc5,0x22,0x3,0x60,0x41,
	0xae,0x8f,0xec,0xcd,0x2a,0xb,0x68,0x49,
	0x97,0xb6,0xd5,0xf4,0x13,0x32,0x51,0x70,
	0x9f,0xbe,0xdd,0xfc,0x1b,0x3a,0x59,0x78,
	0x88,0xa9,0xca,0xeb,0xc,0x2d,0x4e,0x6f,
	0x80,0xa1,0xc2,0xe3,0x4,0x25,0x46,0x67,
	0xb9,0x98,0xfb,0xda,0x3d,0x1c,0x7f,0x5e,
	0xb1,0x90,0xf3,0xd2,0x35,0x14,0x77,0x56,
	0xea,0xcb,0xa8,0x89,0x6e,0x4f,0x2c,0xd,
	0xe2,0xc3,0xa0,0x81,0x66,0x47,0x24,0x5,
	0xdb,0xfa,0x99,0xb8,0x5f,0x7e,0x1d,0x3c,
	0xd3,0xf2,0x91,0xb0,0x57,0x76,0x15,0x34,
	0x4c,0x6d,0xe,0x2f,0xc8,0xe9,0x8a,0xab,
	0x44,0x65,0x6,0x27,0xc0,0xe1,0x82,0xa3,
	0x7d,0x5c,0x3f,0x1e,0xf9,0xd8,0xbb,0x9a,
	0x75,0x54,0x37,0x16,0xf1,0xd0,0xb3,0x92,
	0x2e,0xf,0x6c,0x4d,0xaa,0x8b,0xe8,0xc9,
	0x26,0x7,0x64,0x45,0xa2,0x83,0xe0,0xc1,
	0x1f,0x3e,0x5d,0x7c,0x9b,0xba,0xd9,0xf8,
	0x17,0x36,0x55,0x74,0x93,0xb2,0xd1,0xf0
};
const byte crc2[256]={
	0x0,0x10,0x20,0x30,0x40,0x50,0x60,0x70,
	0x81,0x91,0xa1,0xb1,0xc1,0xd1,0xe1,0xf1,
	0x12,0x2,0x32,0x22,0x52,0x42,0x72,0x62,
	0x93,0x83,0xb3,0xa3,0xd3,0xc3,0xf3,0xe3,
	0x24,0x34,0x4,0x14,0x64,0x74,0x44,0x54,
	0xa5,0xb5,0x85,0x95,0xe5,0xf5,0xc5,0xd5,
	0x36,0x26,0x16,0x6,0x76,0x66,0x56,0x46,
	0xb7,0xa7,0x97,0x87,0xf7,0xe7,0xd7,0xc7,
	0x48,0x58,0x68,0x78,0x8,0x18,0x28,0x38,
	0xc9,0xd9,0xe9,0xf9,0x89,0x99,0xa9,0xb9,
	0x5a,0x4a,0x7a,0x6a,0x1a,0xa,0x3a,0x2a,
	0xdb,0xcb,0xfb,0xeb,0x9b,0x8b,0xbb,0xab,
	0x6c,0x7c,0x4c,0x5c,0x2c,0x3c,0xc,0x1c,
	0xed,0xfd,0xcd,0xdd,0xad,0xbd,0x8d,0x9d,
	0x7e,0x6e,0x5e,0x4e,0x3e,0x2e,0x1e,0xe,
	0xff,0xef,0xdf,0xcf,0xbf,0xaf,0x9f,0x8f,
	0x91,0x81,0xb1,0xa1,0xd1,0xc1,0xf1,0xe1,
	0x10,0x0,0x30,0x20,0x50,0x40,0x70,0x60,
	0x83,0x93,0xa3,0xb3,0xc3,0xd3,0xe3,0xf3,
	0x2,0x12,0x22,0x32,0x42,0x52,0x62,0x72,
	0xb5,0xa5,0x95,0x85,0xf5,0xe5,0xd5,0xc5,
	0x34,0x24,0x14,0x4,0x74,0x64,0x54,0x44,
	0xa7,0xb7,0x87,0x97,0xe7,0xf7,0xc7,0xd7,
	0x26,0x36,0x6,0x16,0x66,0x76,0x46,0x56,
	0xd9,0xc9,0xf9,0xe9,0x99,0x89,0xb9,0xa9,
	0x58,0x48,0x78,0x68,0x18,0x8,0x38,0x28,
	0xcb,0xdb,0xeb,0xfb,0x8b,0x9b,0xab,0xbb,
	0x4a,0x5a,0x6a,0x7a,0xa,0x1a,0x2a,0x3a,
	0xfd,0xed,0xdd,0xcd,0xbd,0xad,0x9d,0x8d,
	0x7c,0x6c,0x5c,0x4c,0x3c,0x2c,0x1c,0xc,
	0xef,0xff,0xcf,0xdf,0xaf,0xbf,0x8f,0x9f,
	0x6e,0x7e,0x4e,0x5e,0x2e,0x3e,0xe,0x1e
};

byte *lPC; //lava����ָ��
byte *VRam;//[0xc0000];//[65536];
EXT_RAM_BSS_ATTR struct TASK task[LAVA_TASK_COUNT]; //����ջ
int task_lev; //����
byte *lRam;
byte eval_top;
byte secret;
long a1,a3,result;
long seed;
a32 string_ptr,local_bp,local_sp;
int RamBits;
a32 ramuses,ramusee,ramarm;
unsigned long timed;
int cur_funcid=0;
int func_stack[1024];
int func_top;
long d_line;
byte wait_key;
long delay;
int have_pen,have_keypad;
int pen_x,pen_y;

byte no_buf,lcmd,negative;
word xx,yy,x0,Y0,x1,Y1;
a32 m1l;
word graph_mode,bgcolor,fgcolor;
byte scr_x,scr_y,scr_mode,curr_RPS,curr_CPR;
word scr_off;
byte small_up,small_down,small_left;
unsigned long line_mode;
byte func_x;
byte negative_tbl[256];
byte fhave[3],fmode[3];
a32 fsize[3];
a32 foffset[3];
int curr_fnum;
char VirtualFullName[I_MAX_PATH];
char CurrentProgramPath[I_MAX_PATH];
a32 ListFileAddr; 

extern int first_file,curr_file;
extern unsigned long GetGBCodeByPY( unsigned int pos, byte *InputBuffer, byte *OutBuffer );

void setscreen(int mode);
void get_val();
void put_val();

float i2f(int i)
{
	union {
		int ii;
		float ff;
	} t;
	t.ii=i;
	//return *(float*)(&i);
	return t.ff;
}

int f2i(float f)
{
	union {
		int ii;
		float ff;
	} t;
	t.ff=f;
	//return *(int *)(&f);
	return t.ii;
}

void make_negative()
{
	int i;
	byte t;
	for (i=0;i<256;i++) {
		t=0;
		if (i&0x80) t|=1;
		if (i&0x40) t|=2;
		if (i&0x20) t|=4;
		if (i&0x10) t|=8;
		if (i&0x8) t|=0x10;
		if (i&0x4) t|=0x20;
		if (i&0x2) t|=0x40;
		if (i&0x1) t|=0x80;
		negative_tbl[i]=t;
	}
}

void set_sys_ram()
{
	TextBuffer=		0x1400; //�����������Լ����Ƿ��ַ���
	string_stack=	0x1c00;
	eval_stack=		0x1b00;
}

void Adjust_Window(int width,int height,int force)
{
	if (width!=ScreenWidth || height!=ScreenHeight || force) {
		ScreenWidth=width;
		ScreenHeight=height;
		SetWindow();
	}
}

void lavReset()
{
	word t1,t2,bak_gmode;

	lPC=pLAVA+16;
	if (*(pLAVA+8)&0x10) RamBits=32;
	else if (*(pLAVA+8)&0x80) RamBits=24;
	else RamBits=16;
	eval_top=0;
	d_line=-1;
	ramuses=0xf00000;
	ramusee=0x0;
	ramarm=1;
	if (RamBits>16) {
		memset(lRam+0x2000,0,(VRam+MY_DATA_SIZE)-(lRam+0x2000));
		set_sys_ram();
		string_ptr=string_stack;
	} else {
		memset(&lRam[0x2000],0,0xe000); //�������
		set_sys_ram();
		string_ptr=string_stack;
	}
	bgcolor=0;
	bak_gmode=graph_mode;
	switch (*(pLAVA+8)&0x60) {
	case 0x40:
		graph_mode=4;
		fgcolor=15;
		break;
	case 0x60:
		graph_mode=8;
		fgcolor=255;
		break;
	default:
		graph_mode=1;
		fgcolor=15;
	}
	switch (*(pLAVA+8)&1) {
	case 1:
		have_pen=1;
		have_keypad=0;
		break;
	default:
		have_pen=0;
		have_keypad=1;
	}
	SetPalette();
	ClearNdsScreen();
	secret=0;
	memset(cur_keyb,0,sizeof(cur_keyb));
	lav_key=0;
	delay=0;
	wait_key=0;
	t1=*(pLAVA+9) ? *(pLAVA+9)<<4 : 160;
	t2=*(pLAVA+10) ? *(pLAVA+10)<<4 : 80;
	t1=GetDisplaySourceWidth(t1);
	t2=GetDisplaySourceHeight(t2);
	if (t1<1) t1=1;
	if (t1>LCD_WIDTH) t1=LCD_WIDTH;
	if (t2<1) t2=1;
	if (t2>LCD_HEIGHT) t2=LCD_HEIGHT;
	Adjust_Window(t1,t2,bak_gmode!=graph_mode);
	line_mode=0;
	setscreen(0);
	seed+=Hz128;
	seed|=1;
	//workdir_init();
	make_negative();
}

//�ȴ����а����ͷ�
static void wait_no_key()
{
	int i,t;

	for (;;) {
		lavax_platform_poll();
		if (CheckExitKey()) break;
		t=0;
		for (i=0;i<sizeof(cur_keyb);i++) t|=cur_keyb[i];
		if (!t) break;
	}
	lav_key=0;
}

static void exit_current_task(int ret_val)
{
	int force;

	c_closeall(); //�رյ�ǰ�������д򿪵��ļ�
	TaskClose(pLAVA);
	task_lev--;
	lPC=task[task_lev].lPC;
	lRam=task[task_lev].lRam;
	eval_top=task[task_lev].eval_top;
	local_sp=task[task_lev].local_sp;
	local_bp=task[task_lev].local_bp;
	string_ptr=task[task_lev].string_ptr;
	//curr_file=task[task_lev].curr_file;
	//first_file=task[task_lev].first_file;
	//list_set=task[task_lev].list_set;
	RamBits=task[task_lev].RamBits;
	force=graph_mode!=task[task_lev].graph_mode;
	graph_mode=task[task_lev].graph_mode;
	if (ScreenWidth!=task[task_lev].ScreenWidth || ScreenHeight!=task[task_lev].ScreenHeight || force)
		Adjust_Window(task[task_lev].ScreenWidth,task[task_lev].ScreenHeight,force);
	bgcolor=task[task_lev].bgcolor;
	fgcolor=task[task_lev].fgcolor;
	Load_Palette();
	LoadKeyCode();
	LoadDisplayConfig(&task[task_lev]);
	secret=task[task_lev].secret;
	strcpy(CD,task[task_lev].CD);
	strcpy(RootPath,task[task_lev].Root);
	strcpy(CurrentProgramPath,task[task_lev].ProgramPath);
	lavax_platform_set_app_path(CurrentProgramPath);
	set_sys_ram();
	pLAVA=task[task_lev].pLAVA;
	a1=ret_val;
	put_val();
}

int CheckAppExit()
{
	if (!lavax_platform_take_app_exit_request())
		return 0;
	if (!task_lev)
		return 0;

	while (task_lev)
		exit_current_task(0);
	while (lavax_platform_app_exit_combo_down() && !CheckExitKey())
		lavax_platform_poll();
	memset(cur_keyb,0,sizeof(cur_keyb));
	lav_key=0;
	return 1;
}

void c_exit()
{
	int ret_val;

	get_val();
	ret_val=a1;
	wait_no_key();
	if (task_lev) {
		exit_current_task(ret_val);
		return;
	}
	lavReset();
}

void good_exit()
{
	a1=0;
	put_val();
	c_exit();
}

void bad_exit()
{
	a1=-1;
	put_val();
	c_exit();
}

void div0_err()
{
	ramuses=0; //ȷ���޷�ͨ����֤
	bad_exit();
}

void sn_err()
{
	;
}

static size_t current_opcode_offset;
static int current_opcode=-1;

long lRamRead4(a32 addr);

static void vm_memory_error(a32 addr,size_t size,const char *operation)
{
	char message[160];

	snprintf(message,sizeof(message),
		"LavaXVM %s outside VM RAM: addr=0x%08lx size=%lu pc=0x%08lx opcode=0x%02x RamBits=%d eval=%u bp=0x%08lx sp=0x%08lx",
		operation,(unsigned long)addr,(unsigned long)size,
		(unsigned long)current_opcode_offset,current_opcode,
		RamBits,(unsigned int)eval_top,(unsigned long)local_bp,(unsigned long)local_sp);
	printf("%s\n",message);
	LavaRuntimeError(message);
	lPC=NULL;
}

static byte *vm_memory_pointer(a32 addr,size_t size,const char *operation)
{
	uintptr_t base=(uintptr_t)VRam;
	uintptr_t current=(uintptr_t)lRam;
	size_t offset;
	size_t available;

	if (!VRam || !lRam || current<base || current-base>MY_DATA_SIZE) {
		vm_memory_error(addr,size,operation);
		return NULL;
	}
	offset=(size_t)(current-base);
	available=MY_DATA_SIZE-offset;
	if (size>available || (size_t)addr>available-size) {
		vm_memory_error(addr,size,operation);
		return NULL;
	}
	return lRam+addr;
}

static char *vm_cstring_pointer(a32 addr,const char *operation)
{
	byte *start=vm_memory_pointer(addr,1,operation);
	uintptr_t base=(uintptr_t)VRam;
	size_t task_offset;
	size_t available;

	if (!start) return NULL;
	task_offset=(size_t)((uintptr_t)lRam-base);
	available=MY_DATA_SIZE-task_offset-(size_t)addr;
	if (!memchr(start,0,available)) {
		vm_memory_error(addr,available,operation);
		return NULL;
	}
	return (char *)start;
}

byte lRamRead(a32 addr)
{
	byte *pointer=vm_memory_pointer(addr,1,"read");
	return pointer ? pointer[0] : 0;
}

short lRamRead2(a32 addr)
{
	byte *pointer=vm_memory_pointer(addr,2,"read16");
	if (!pointer) return 0;
	return (short)(pointer[0]|(pointer[1]<<8));
}

long lRamRead4(a32 addr)
{
	byte *pointer=vm_memory_pointer(addr,4,"read32");
	if (!pointer) return 0;
	return (long)((uint32_t)pointer[0]|((uint32_t)pointer[1]<<8)|
		((uint32_t)pointer[2]<<16)|((uint32_t)pointer[3]<<24));
}

void lRamWrite(a32 addr,byte t)
{
	byte *pointer=vm_memory_pointer(addr,1,"write");
	if (pointer) pointer[0]=t;
}

void lRamWrite2(a32 addr,short t)
{
	byte *pointer=vm_memory_pointer(addr,2,"write16");
	if (!pointer) return;
	pointer[0]=(byte)t;
	pointer[1]=(byte)(t>>8);
}

void lRamWrite4(a32 addr,long t)
{
	byte *pointer=vm_memory_pointer(addr,4,"write32");
	if (!pointer) return;
	pointer[0]=(byte)t;
	pointer[1]=(byte)(t>>8);
	pointer[2]=(byte)(t>>16);
	pointer[3]=(byte)(t>>24);
}

void lmemcpy(a32 src,a32 obj,int size,int src_du,int obj_du)
{
	byte *destination;
	byte *source;

	if (size<=0) return;
	destination=vm_memory_pointer(src,(size_t)size,"copy destination");
	source=vm_memory_pointer(obj,(size_t)size,"copy source");
	if (destination && source) memmove(destination,source,(size_t)size);
}

static int code_pointer_valid(byte *pointer)
{
	size_t size=TaskSize(pLAVA);
	uintptr_t start=(uintptr_t)pLAVA;
	uintptr_t address=(uintptr_t)pointer;
	return size>=16 && address>=start+16 && address<start+size;
}

static int set_code_pointer(size_t offset)
{
	size_t size=TaskSize(pLAVA);
	if (offset<16 || offset>=size) {
		LavaRuntimeError("LavaXVM bytecode jump is outside the program");
		lPC=NULL;
		return 0;
	}
	lPC=pLAVA+offset;
	return 1;
}

byte getcode()
{
	if (!code_pointer_valid(lPC)) {
		LavaRuntimeError("LavaXVM bytecode read is outside the program");
		lPC=NULL;
		return 0;
	}
	return *(lPC++);
}

a32 get_vaddr()
{
	a32 t;
	t=getcode();
	t+=getcode()<<8;
	if (RamBits>16) t+=getcode()<<16;
	return t;
}

a32 get_lvaddr()
{
	a32 t;
	t=getcode();
	t+=getcode()<<8;
	if (RamBits>16) t+=getcode()<<16;
	return t+local_bp;
}

static int pop_address_offset(a32 *offset)
{
	if (eval_top<4) {
		vm_memory_error(eval_stack+eval_top,4,"address stack underflow");
		*offset=0;
		return 0;
	}
	eval_top-=4;
	*offset=(a32)lRamRead(eval_stack+eval_top)|
		((a32)lRamRead(eval_stack+eval_top+1)<<8);
	if (RamBits>16)
		*offset|=(a32)lRamRead(eval_stack+eval_top+2)<<16;
	return lPC!=NULL;
}

a32 get_gaddr()
{
	a32 t,offset;
	t=getcode();
	t+=getcode()<<8;
	if (RamBits>16) t+=getcode()<<16;
	if (!pop_address_offset(&offset)) return 0;
	return t+offset;
}

a32 get_lgaddr()
{
	a32 t,offset;
	t=getcode();
	t+=getcode()<<8;
	if (RamBits>16) t+=getcode()<<16;
	if (!pop_address_offset(&offset)) return 0;
	return t+local_bp+offset;
}

static int eval_stack_pop(long *value)
{
	if (eval_top<4) {
		vm_memory_error(eval_stack+eval_top,4,"evaluation stack underflow");
		*value=0;
		return 0;
	}
	eval_top-=4;
	*value=lRamRead4(eval_stack+eval_top);
	return lPC!=NULL;
}

void get_val()
{
	eval_stack_pop(&a1);
}

void get_val2()
{
	eval_stack_pop(&a3);
}

void get_vals()
{
	get_val2();
	if (lPC) get_val();
}

void put_val()
{
	if (eval_top>252) {
		vm_memory_error(eval_stack+eval_top,4,"evaluation stack overflow");
		return;
	}
	lRamWrite4(eval_stack+eval_top,a1);
	if (lPC) eval_top+=4;
}

void put_string()
{
	lRam[eval_stack+eval_top++]=(byte)string_ptr;
	lRam[eval_stack+eval_top++]=(byte)(string_ptr>>8);
	if (RamBits>16)
		lRam[eval_stack+eval_top++]=(byte)(string_ptr>>16);
	else
		lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
}

void push_char()
{
	lRam[eval_stack+eval_top++]=getcode();
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
}

void push_int()
{
	byte t;
	lRam[eval_stack+eval_top++]=getcode();
	t=getcode();
	lRam[eval_stack+eval_top++]=t;
	if (t&0x80) {
		lRam[eval_stack+eval_top++]=0xff;
		lRam[eval_stack+eval_top++]=0xff;
	} else {
		lRam[eval_stack+eval_top++]=0;
		lRam[eval_stack+eval_top++]=0;
	}
}

void push_long()
{
	lRam[eval_stack+eval_top++]=getcode();
	lRam[eval_stack+eval_top++]=getcode();
	lRam[eval_stack+eval_top++]=getcode();
	lRam[eval_stack+eval_top++]=getcode();
}

void push_vchar()
{
	a32 t;
	t=get_vaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
}

void push_lvchar()
{
	a32 t;
	t=get_lvaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
}

void push_vint()
{
	a32 t;
	byte t2;
	t=get_vaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	t2=lRamRead(t+1);
	lRam[eval_stack+eval_top++]=t2;
	if (t2&0x80) {
		lRam[eval_stack+eval_top++]=0xff;
		lRam[eval_stack+eval_top++]=0xff;
	} else {
		lRam[eval_stack+eval_top++]=0;
		lRam[eval_stack+eval_top++]=0;
	}
}

void push_lvint()
{
	a32 t;
	byte t2;
	t=get_lvaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	t2=lRamRead(t+1);
	lRam[eval_stack+eval_top++]=t2;
	if (t2&0x80) {
		lRam[eval_stack+eval_top++]=0xff;
		lRam[eval_stack+eval_top++]=0xff;
	} else {
		lRam[eval_stack+eval_top++]=0;
		lRam[eval_stack+eval_top++]=0;
	}
}

void push_vlong()
{
	a32 t;
	t=get_vaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	lRam[eval_stack+eval_top++]=lRamRead(t+1);
	lRam[eval_stack+eval_top++]=lRamRead(t+2);
	lRam[eval_stack+eval_top++]=lRamRead(t+3);
}

void push_lvlong()
{
	a32 t;
	t=get_lvaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	lRam[eval_stack+eval_top++]=lRamRead(t+1);
	lRam[eval_stack+eval_top++]=lRamRead(t+2);
	lRam[eval_stack+eval_top++]=lRamRead(t+3);
}

void push_gchar()
{
	a32 t;
	// ע�⣺��������벻�ɺϲ�ΪlRam[eval_stack+eval_top++]=lRamRead(get_gaddr())
	// ��Ϊget_gaddr()��ı�eval_top������ʽ����õ���eval_top
	// ����nds�������Ĳ�ǡ���Ż�����ʹ��eval_top���ձ�Ԥ��ֵ��4���Ӷ��������ջ��ƽ��
	// ���ǣ���gba�ϱ���ȴ����ȷ�ģ�vc��Ҳ��ȷ
	t=get_gaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
}

void push_lgchar()
{
	a32 t;
	t=get_lgaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
}

void push_gint()
{
	a32 t;
	byte t2;
	t=get_gaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	t2=lRamRead(t+1);
	lRam[eval_stack+eval_top++]=t2;
	if (t2&0x80) {
		lRam[eval_stack+eval_top++]=0xff;
		lRam[eval_stack+eval_top++]=0xff;
	} else {
		lRam[eval_stack+eval_top++]=0;
		lRam[eval_stack+eval_top++]=0;
	}
}

void push_lgint()
{
	a32 t;
	byte t2;
	t=get_lgaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	t2=lRamRead(t+1);
	lRam[eval_stack+eval_top++]=t2;
	if (t2&0x80) {
		lRam[eval_stack+eval_top++]=0xff;
		lRam[eval_stack+eval_top++]=0xff;
	} else {
		lRam[eval_stack+eval_top++]=0;
		lRam[eval_stack+eval_top++]=0;
	}
}

void push_glong()
{
	a32 t;
	t=get_gaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	lRam[eval_stack+eval_top++]=lRamRead(t+1);
	lRam[eval_stack+eval_top++]=lRamRead(t+2);
	lRam[eval_stack+eval_top++]=lRamRead(t+3);
}

void push_lglong()
{
	a32 t;
	t=get_lgaddr();
	lRam[eval_stack+eval_top++]=lRamRead(t);
	lRam[eval_stack+eval_top++]=lRamRead(t+1);
	lRam[eval_stack+eval_top++]=lRamRead(t+2);
	lRam[eval_stack+eval_top++]=lRamRead(t+3);
}

void push_ax()
{
	a32 t;
	t=get_gaddr();
	lRam[eval_stack+eval_top++]=(byte)(t&0xff);
	lRam[eval_stack+eval_top++]=(byte)((t>>8)&0xff);
	lRam[eval_stack+eval_top++]=(byte)((t>>16)&0xff);
	lRam[eval_stack+eval_top++]=(byte)((t>>24)&0xff);
}

void push_achar()
{
	a32 t;
	t=get_gaddr();
	lRam[eval_stack+eval_top++]=(byte)(t&0xff);
	lRam[eval_stack+eval_top++]=(byte)((t>>8)&0xff);
	if (RamBits>16) {
		lRam[eval_stack+eval_top++]=(byte)((t>>16)&0xff);
		lRam[eval_stack+eval_top++]=1;
	} else {
		lRam[eval_stack+eval_top++]=1;
		lRam[eval_stack+eval_top++]=0;
	}
}

void push_lachar()
{
	a32 t;
	t=get_lgaddr();
	lRam[eval_stack+eval_top++]=(byte)(t&0xff);
	lRam[eval_stack+eval_top++]=(byte)((t>>8)&0xff);
	if (RamBits>16) {
		lRam[eval_stack+eval_top++]=(byte)((t>>16)&0xff);
		lRam[eval_stack+eval_top++]=1;
	} else {
		lRam[eval_stack+eval_top++]=1;
		lRam[eval_stack+eval_top++]=0;
	}
}

void push_aint()
{
	a32 t;
	t=get_gaddr();
	lRam[eval_stack+eval_top++]=(byte)(t&0xff);
	lRam[eval_stack+eval_top++]=(byte)((t>>8)&0xff);
	if (RamBits>16) {
		lRam[eval_stack+eval_top++]=(byte)((t>>16)&0xff);
		lRam[eval_stack+eval_top++]=2;
	} else {
		lRam[eval_stack+eval_top++]=2;
		lRam[eval_stack+eval_top++]=0;
	}
}

void push_laint()
{
	a32 t;
	t=get_lgaddr();
	lRam[eval_stack+eval_top++]=(byte)(t&0xff);
	lRam[eval_stack+eval_top++]=(byte)((t>>8)&0xff);
	if (RamBits>16) {
		lRam[eval_stack+eval_top++]=(byte)((t>>16)&0xff);
		lRam[eval_stack+eval_top++]=2;
	} else {
		lRam[eval_stack+eval_top++]=2;
		lRam[eval_stack+eval_top++]=0;
	}
}

void push_along()
{
	a32 t;
	t=get_gaddr();
	lRam[eval_stack+eval_top++]=(byte)(t&0xff);
	lRam[eval_stack+eval_top++]=(byte)((t>>8)&0xff);
	if (RamBits>16)
		lRam[eval_stack+eval_top++]=(byte)((t>>16)&0xff);
	else
		lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
}

void push_lalong()
{
	a32 t;
	t=get_lgaddr();
	lRam[eval_stack+eval_top++]=(byte)(t&0xff);
	lRam[eval_stack+eval_top++]=(byte)((t>>8)&0xff);
	if (RamBits>16)
		lRam[eval_stack+eval_top++]=(byte)((t>>16)&0xff);
	else
		lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
}

void push_string()
{
	byte t;
	put_string();
	for (;;) {
		t=getcode()^secret;
		lRam[string_ptr++]=t;
		if (t==0) break;
	}
	if (string_ptr>=string_stack+768)
		string_ptr=string_stack;
}

void push_llong()
{
	a32 t;
	t=get_vaddr()+local_bp;
	lRam[eval_stack+eval_top++]=(byte)(t&0xff);
	lRam[eval_stack+eval_top++]=(byte)((t>>8)&0xff);
	if (RamBits>16)
		lRam[eval_stack+eval_top++]=(byte)((t>>16)&0xff);
	else
		lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
}

void push_text()
{
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	ramuses=0; //ȷ���޷�ͨ����֤
	bad_exit();
}

void push_graph() //����0��ʾ��֧��_GRAPH
{
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	ramuses=0; //ȷ���޷�ͨ����֤
	bad_exit();
}

void push_gbuf() //����0��ʾ��֧��_GBUF
{
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	lRam[eval_stack+eval_top++]=0;
	ramuses=0; //ȷ���޷�ͨ����֤
	bad_exit();
}

void push_sub0()
{
	get_val();
	a1=0-a1;
	put_val();
}

void inc_dec_com()
{
	long t;
	get_val2();
	if (RamBits>16) {
		t=(a3>>24)&0x7f;
		if (a3&0x80000000) a3+=local_bp; //???�п���Խ�磬��Ҳ���Բ�����
		a3&=0xffffff;
		if (t==1) a1=lRamRead(a3);
		else if (t==2) a1=lRamRead2(a3);
		else a1=lRamRead4(a3);
		a3|=t<<24;
	} else {
		t=(a3>>16)&0x7f;
		if (a3&0x800000) a3+=local_bp; //???�п���Խ�磬��Ҳ���Բ�����
		a3&=0xffff;
		if (t==1) a1=lRamRead(a3);
		else if (t==2) a1=lRamRead2(a3);
		else a1=lRamRead4(a3);
		a3|=t<<16;
	}
}

void inc_save()
{
	int t;
	if (RamBits>16) {
		t=(a3>>24)&0x7f;
		a3&=0xffffff;
	} else {
		t=(a3>>16)&0x7f;
		a3&=0xffff;
	}
	if (t==1) lRamWrite(a3,(byte)(a1&0xff));
	else if (t==2) lRamWrite2(a3,(short)(a1&0xffff));
	else lRamWrite4(a3,a1);
}

void cal_inc()
{
	inc_dec_com();
	a1++;
	put_val();
	inc_save();
}

void cal_dec()
{
	inc_dec_com();
	a1--;
	put_val();
	inc_save();
}

void cal_INC()
{
	inc_dec_com();
	put_val();
	a1++;
	inc_save();
}

void cal_DEC()
{
	inc_dec_com();
	put_val();
	a1--;
	inc_save();
}

void cal_idx()
{
	byte t,t2;

	get_val2();
	t=getcode();
	if (t&0x80) a3+=local_bp;
	t2=t&0x1f;
	if (t2==1) a1=lRamRead(a3);
	else if (t2==2) a1=lRamRead2(a3);
	else a1=lRamRead4(a3);
	t=(t>>5)&3;
	switch (t) {
	case 0:
		a1++;
		put_val();
		break;
	case 1:
		a1--;
		put_val();
		break;
	case 2:
		put_val();
		a1++;
		break;
	case 3:
		put_val();
		a1--;
		break;
	}
	if (t2==1) lRamWrite(a3,(byte)(a1&0xff));
	else if (t2==2) lRamWrite2(a3,(short)(a1&0xffff));
	else lRamWrite4(a3,a1);
}

void cal_add()
{
	get_vals();
	a1+=a3;
	put_val();
}

void cal_sub()
{
	get_vals();
	a1-=a3;
	put_val();
}

void cal_and()
{
	get_vals();
	a1&=a3;
	put_val();
}

void cal_or()
{
	get_vals();
	a1|=a3;
	put_val();
}

void push_not()
{
	get_val();
	a1=a1^0xffffffff;
	put_val();
}

void cal_xor()
{
	get_vals();
	a1^=a3;
	put_val();
}

void cal_land()
{
	get_vals();
	if (a1 && a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_lor()
{
	get_vals();
	if (a1 || a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_lnot()
{
	get_val();
	if (a1==0) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_mul()
{
	get_vals();
	a1*=a3;
	put_val();
}

void cal_div()
{
	get_vals();
	if (a3==0) div0_err();
	else a1/=a3;
	put_val();
}

void cal_mod()
{
	get_vals();
	if (a3==0) div0_err();
	else a1%=a3;
	put_val();
}

void cal_lshift()
{
	get_vals();
	if (a3<0) a1=0;
	else if (a3==0) ;
	else a1<<=a3;
	put_val();
}

void cal_rshift()
{
	get_vals();
	if (a3<0) a1=0;
	else if (a3==0) ;
	else a1=(unsigned long)a1>>a3;
	put_val();
}

void cal_equ()
{
	get_vals();
	if (a1==a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_neq()
{
	get_vals();
	if (a1!=a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_le()
{
	get_vals();
	if (a1<=a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_ge()
{
	get_vals();
	if (a1>=a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_great()
{
	get_vals();
	if (a1>a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_less()
{
	get_vals();
	if (a1<a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void c_let()
{
	long t,t1;
	get_vals();
	if (RamBits>16) {
		if (a1&0x80000000) t1=(a1+local_bp)&0xffffff; //???�п���Խ�磬��Ҳ���Բ�����
		else t1=a1&0xffffff;
		t=(a1>>24)&0x7f;
	} else {
		if (a1&0x800000) t1=(a1+local_bp)&0xffff; //???�п���Խ�磬��Ҳ���Բ�����
		else t1=a1&0xffff;
		t=(a1>>16)&0x7f;
	}
	if (t==1) lRamWrite(t1,(byte)(a3&0xff));
	else if (t==2) lRamWrite2(t1,(short)(a3&0xffff));
	else lRamWrite4(t1,a3);
	a1=a3;
	put_val();
}

void c_letx()
{
	byte t;

	get_vals();
	t=getcode();
	if (t&0x80) a1+=local_bp;
	t&=0x7f;
	//a1&=0xffffff;
	if (t==1) lRamWrite(a1,(byte)(a3&0xff));
	else if (t==2) lRamWrite2(a1,(short)(a3&0xffff));
	else lRamWrite4(a1,a3);
	a1=a3;
	put_val();
}

void c_ptr()
{
	get_val();
	if (RamBits>16)
		a1&=0xffffff;
	else
		a1&=0xffff;
	a1=lRamRead(a1);
	put_val();
}

void c_iptr()
{
	get_val();
	if (RamBits>16)
		a1&=0xffffff;
	else
		a1&=0xffff;
	a1=lRamRead(a1)+(lRamRead(a1+1)<<8);
	a1&=0xffff;
	if (a1&0x8000) a1|=0xffff0000; //����
	put_val();
}

void c_lptr()
{
	get_val();
	if (RamBits>16)
		a1&=0xffffff;
	else
		a1&=0xffff;
	a1=lRamRead(a1)+(lRamRead(a1+1)<<8)+(lRamRead(a1+2)<<16)+(lRamRead(a1+3)<<24);
	put_val();
}

void c_cptr()
{
	get_val();
	if (RamBits>16)
		a1=(a1&0xffffff)|0x1000000;
	else
		a1=(a1&0xffff)|0x10000;
	put_val();
}

void c_ciptr()
{
	get_val();
	if (RamBits>16)
		a1=(a1&0xffffff)|0x2000000;
	else
		a1=(a1&0xffff)|0x20000;
	put_val();
}

void c_clptr()
{
	get_val();
	if (RamBits>16)
		a1=(a1&0xffffff)|0x4000000;
	else
		a1=(a1&0xffff)|0x40000;
	put_val();
}

void pop_val()
{
	get_val();
	result=a1;
}

void c_jmp()
{
	long t;
	t=getcode();
	t+=getcode()<<8;
	t+=getcode()<<16;
	if (!lPC || !set_code_pointer((size_t)t)) return;
	if (CheckExitKey()) {
		good_exit();
	}
}

void c_jmpe()
{
	if (result==0) {
		c_jmp();
	} else {
		getcode();
		getcode();
		getcode();
	}
}

void c_jmpn()
{
	if (result) {
		c_jmp();
	} else {
		getcode();
		getcode();
		getcode();
	}
}

void set_sp()
{
	local_sp=getcode();
	local_sp+=getcode()<<8;
	if (RamBits>16) local_sp+=getcode()<<16;
	//if (local_sp&3) local_sp+=4-(local_sp&3); //���ջ��ʼ��4�ֽڱ߽�
}

void c_call()
{
	lRamWrite(local_sp,(byte)((lPC+3-pLAVA)&0xff));
	lRamWrite(local_sp+1,(byte)(((lPC+3-pLAVA)>>8)&0xff));
	lRamWrite(local_sp+2,(byte)(((lPC+3-pLAVA)>>16)&0xff));
	c_jmp();
}

void add_bp()
{
	long t;
	byte t2;
	int i;
	t=local_bp;
	local_bp=local_sp;
	lRamWrite(local_bp+3,(byte)(t&0xff));
	lRamWrite(local_bp+4,(byte)((t>>8)&0xff));
	if (RamBits>16) lRamWrite(local_bp+5,(byte)((t>>16)&0xff));
	t=getcode();
	t+=getcode()<<8;
	if (RamBits>16) t+=getcode()<<16;
	local_sp=local_bp+(t&0xffffff);
	//if (local_sp&3) local_sp+=4-(local_sp&3); //���ջ��ʼ��4�ֽڱ߽�
	t=getcode()*4;
	if (t) {
		eval_top-=(word)t;
		t2=eval_top;
		i=0;
		if (RamBits==32) {
			while (t) {
				lRamWrite(local_bp+8+i++,lRam[eval_stack+t2++]);
				t--;
			}
		} else if (RamBits>16) {
			while (t) {
				lRamWrite(local_bp+6+i++,lRam[eval_stack+t2++]);
				t--;
			}
		} else {
			while (t) {
				lRamWrite(local_bp+5+i++,lRam[eval_stack+t2++]);
				t--;
			}
		}
	}
}

void sub_bp()
{
	long t;
	local_sp=local_bp;
	t=lRamRead(local_bp);
	t+=lRamRead(local_bp+1)<<8;
	t+=lRamRead(local_bp+2)<<16;
	if (!set_code_pointer((size_t)t)) return;
	t=lRamRead(local_bp+3);
	t+=lRamRead(local_bp+4)<<8;
	if (RamBits>16) t+=lRamRead(local_bp+5)<<16;
	local_bp=t&0xffffff;
	if (func_top) cur_funcid=func_stack[--func_top];
}

void c_preset()
{
	int addr,len;
	addr=getcode();
	addr+=getcode()<<8;
	if (RamBits>16) addr+=getcode()<<16;
	len=getcode();
	len+=getcode()<<8;
	while (len) {
		lRamWrite(addr++,getcode());
		len--;
	}
}

void c_secret()
{
	secret=getcode();
}

void c_loadall()
{
	;
}

void getint()
{
	a3=getcode();
	a3+=getcode()<<8;
	if (a3&0x8000) a3|=0xffff0000;
}

void cal_qadd()
{
	get_val();
	getint();
	a1+=a3;
	put_val();
}

void cal_qsub()
{
	get_val();
	getint();
	a1-=a3;
	put_val();
}

void cal_qmul()
{
	get_val();
	getint();
	a1*=a3;
	put_val();
}

void cal_qdiv()
{
	get_val();
	getint();
	if (a3==0) div0_err();
	else a1/=a3;
	put_val();
}

void cal_qmod()
{
	get_val();
	getint();
	if (a3==0) div0_err();
	else a1%=a3;
	put_val();
}

void cal_qlshift()
{
	get_val();
	getint();
	a1<<=a3;
	put_val();
}

void cal_qrshift()
{
	get_val();
	getint();
	a1=(unsigned long)a1>>a3;
	put_val();
}

void cal_qequ()
{
	get_val();
	getint();
	if (a1==a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_qneq()
{
	get_val();
	getint();
	if (a1!=a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_qgreat()
{
	get_val();
	getint();
	if (a1>a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_qless()
{
	get_val();
	getint();
	if (a1<a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_qge()
{
	get_val();
	getint();
	if (a1>=a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_qle()
{
	get_val();
	getint();
	if (a1<=a3) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void c_icf()
{
	get_val();
	a1=f2i((float)a1);
	put_val();
}

void c_fci()
{
	get_val();
	a1=(long)(i2f(a1));
	put_val();
}

void c_lcc()
{
	get_val();
	a1&=0xff;
	put_val();
}

void c_lci()
{
	get_val();
	a1&=0xffff;
	if (a1&0x8000) a1|=0xffff0000;
	put_val();
}

void cal_addff()
{
	get_vals();
	a1=f2i(i2f(a1)+i2f(a3));
	put_val();
}

void cal_addf()
{
	get_vals();
	a1=f2i(i2f(a1)+a3);
	put_val();
}

void cal_add0f()
{
	get_vals();
	a1=f2i(a1+i2f(a3));
	put_val();
}

void cal_subff()
{
	get_vals();
	a1=f2i(i2f(a1)-i2f(a3));
	put_val();
}

void cal_subf()
{
	get_vals();
	a1=f2i(i2f(a1)-a3);
	put_val();
}

void cal_sub0f()
{
	get_vals();
	a1=f2i(a1-i2f(a3));
	put_val();
}

void cal_mulff()
{
	get_vals();
	a1=f2i(i2f(a1)*i2f(a3));
	put_val();
}

void cal_mulf()
{
	get_vals();
	a1=f2i(i2f(a1)*a3);
	put_val();
}

void cal_mul0f()
{
	get_vals();
	a1=f2i(a1*i2f(a3));
	put_val();
}

void cal_divff()
{
	get_vals();
	if (a3==0) div0_err();
	a1=f2i(i2f(a1)/i2f(a3));
	put_val();
}

void cal_divf()
{
	get_vals();
	if (a3==0) div0_err();
	a1=f2i(i2f(a1)/a3);
	put_val();
}

void cal_div0f()
{
	get_vals();
	if (a3==0) div0_err();
	a1=f2i(a1/i2f(a3));
	put_val();
}

void push_sub0f()
{
	get_val();
	a1=f2i(0-i2f(a1));
	put_val();
}

void cal_lessf()
{
	get_vals();
	if (i2f(a1)<i2f(a3)) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_greatf()
{
	get_vals();
	if (i2f(a1)>i2f(a3)) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_equf()
{
	get_vals();
	if (i2f(a1)==i2f(a3)) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_neqf()
{
	get_vals();
	if (i2f(a1)!=i2f(a3)) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_lef()
{
	get_vals();
	if (i2f(a1)<=i2f(a3)) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void cal_gef()
{
	get_vals();
	if (i2f(a1)>=i2f(a3)) a1=LTRUE;
	else a1=LFALSE;
	put_val();
}

void c_f0()
{
	get_val();
	a1&=0x7fffffff;
	put_val();
}

void c_pass()
{
	getcode();
}

void c_void()
{
	;
}

void c_debug()
{
	int line;
	line=getcode();
	line+=getcode()<<8;
	line+=getcode()<<16;
	d_line=line;
}

void c_funcid()
{
	int line;
	line=getcode();
	line+=getcode()<<8;
	line+=getcode()<<16;
	func_stack[func_top++]=cur_funcid;
	cur_funcid=line;
}

CODES codes[]={
	sn_err,push_char,push_int,push_long,push_vchar,push_vint,push_vlong, //0
	push_gchar,push_gint,push_glong,push_achar,push_aint,push_along,push_string, //7
	push_lvchar,push_lvint,push_lvlong,push_lgchar,push_lgint,push_lglong, //14
	push_lachar,push_laint,push_lalong,push_along,push_lalong,push_llong, //20
	push_text,push_graph,push_sub0,cal_inc,cal_dec,cal_INC,cal_DEC, //26
	cal_add,cal_sub,cal_and,cal_or,push_not,cal_xor,cal_land,cal_lor,cal_lnot, //33
	cal_mul,cal_div,cal_mod,cal_lshift,cal_rshift,cal_equ,cal_neq,cal_le,cal_ge,cal_great,cal_less, //42
	c_let,c_ptr,c_cptr,pop_val,c_jmpe,c_jmpn,c_jmp,set_sp,c_call,add_bp,sub_bp, //53
	good_exit,c_preset,push_gbuf,c_secret,c_loadall, //64
	cal_qadd,cal_qsub,cal_qmul,cal_qdiv,cal_qmod,cal_qlshift,cal_qrshift, //69
	cal_qequ,cal_qneq,cal_qgreat,cal_qless,cal_qge,cal_qle, //76
	c_iptr,c_lptr, //82
	c_icf,c_fci,cal_addff,cal_addf,cal_add0f,cal_subff,cal_subf,cal_sub0f, //84
	cal_mulff,cal_mulf,cal_mul0f,cal_divff,cal_divf,cal_div0f,push_sub0f, //92
	cal_lessf,cal_greatf,cal_equf,cal_neqf,cal_lef,cal_gef,c_f0, //99
	c_ciptr,c_clptr,c_lcc,c_lci,c_letx,push_ax,cal_idx,c_pass,c_void, //106
	c_debug,c_funcid
};

void ByteAddr()
{
	m1l=yy*LCD_WIDTH+xx;
	if (!no_buf) m1l+=SCROLL_CON;
}

void put_dot()
{
	if (xx>=ScreenWidth) return; //��pc���������������У��Ͼ���������LCD����ICON��
	if (yy>=ScreenHeight) return;
	ByteAddr();
	if (graph_mode==1) {
		switch (lcmd) {
		case 0:
			BmpData[m1l]=0;
			break;
		case 1:
			BmpData[m1l]=1;
			break;
		case 2:
			BmpData[m1l]^=1;
			break;
		}
	} else if (graph_mode==4) {
		switch (lcmd) {
		case 0:
			BmpData[m1l]=(byte)bgcolor;
			break;
		case 1:
			BmpData[m1l]=(byte)fgcolor;
			break;
		case 2:
			BmpData[m1l]^=15;
			break;
		}
	} else {
		switch (lcmd) {
		case 0:
			BmpData[m1l]=(byte)bgcolor;
			break;
		case 1:
			BmpData[m1l]=(byte)fgcolor;
			break;
		case 2:
			BmpData[m1l]^=255;
			break;
		}
	}
}

word get_dot()
{
	if (xx>=ScreenWidth || yy>=ScreenHeight) return 0;
	ByteAddr();
	return BmpData[m1l];
}

static void write81(word x,word y,word width,word height,byte *data)
{
	int i;
	register int j;
	byte t;
	word widths;
	int x_bak;
	byte *td;
	register byte *disp;
	int temp;

	x_bak=x;
	widths=width;	
	if (negative) {
		td=ram_base+MY_LOCAL_RAM;
		memcpy(td,data,widths*height);
		for (i=0;i<height;i++)
			for (j=0;j<widths/2;j++) {
				t=td[i*widths+j];
				td[i*widths+j]=td[i*widths+widths-1-j];
				td[i*widths+widths-1-j]=t;
			}
	} else
		td=data;
		
	if (y>=ScreenHeight) {
		temp=0x10000-y;
		if (height<=temp) return; //ȫ�ڻ����⣬����Ҫ��
		height-=temp;
		td+=temp*widths;
		y=0;
	}
	if (y+height>ScreenHeight) height=ScreenHeight-y; //��ȥ����������
	if (x_bak>=ScreenWidth) {
		temp=0x10000-x_bak;
		if (width<=temp) return; //ȫ�ڻ����⣬����Ҫ��
		width-=temp;
		td+=temp;
		x_bak=0;
	}
	if (x_bak+width>ScreenWidth) width=ScreenWidth-x_bak;

	xx=x_bak;yy=y;
	ByteAddr();
	disp=BmpData+m1l;
	j=height;
	while (j--) {
		memcpy(disp,td,width);
		td+=widths;
		disp+=LCD_WIDTH;
	}
}

static void write86(word x,word y,word width,word height,byte *data)
{
	int i;
	register int j;
	byte t;
	word widths;
	int x_bak;
	byte *td;
	register byte *disp,*tt;
	int temp;

	x_bak=x;
	widths=width;	
	if (negative) {
		td=ram_base+MY_LOCAL_RAM;
		memcpy(td,data,widths*height);
		for (i=0;i<height;i++)
			for (j=0;j<widths/2;j++) {
				t=td[i*widths+j];
				td[i*widths+j]=td[i*widths+widths-1-j];
				td[i*widths+widths-1-j]=t;
			}
	} else
		td=data;
		
	if (y>=ScreenHeight) {
		temp=0x10000-y;
		if (height<=temp) return; //ȫ�ڻ����⣬����Ҫ��
		height-=temp;
		td+=temp*widths;
		y=0;
	}
	if (y+height>ScreenHeight) height=ScreenHeight-y; //��ȥ����������
	if (x_bak>=ScreenWidth) {
		temp=0x10000-x_bak;
		if (width<=temp) return; //ȫ�ڻ����⣬����Ҫ��
		width-=temp;
		td+=temp;
		x_bak=0;
	}
	if (x_bak+width>ScreenWidth) width=ScreenWidth-x_bak;

	xx=x_bak;yy=y;
	ByteAddr();
	disp=BmpData+m1l;
	j=height;
	while (j--) {
		i=width;
		tt=td;
		while (i--) {
			t=*tt++;
			if (t) *disp++=t;
			else disp++;
		}
		td+=widths;
		disp+=LCD_WIDTH-width;
	}
}

static void write8(word x,word y,word width,word height,byte *data)
{
	int i;
	register int j;
	byte t;
	word widths;
	int x_bak;
	byte *td;
	register byte *disp,*tt;
	int temp;

	x_bak=x;
	widths=width;	
	if (negative) {
		td=ram_base+MY_LOCAL_RAM;
		memcpy(td,data,widths*height);
		for (i=0;i<height;i++)
			for (j=0;j<widths/2;j++) {
				t=td[i*widths+j];
				td[i*widths+j]=td[i*widths+widths-1-j];
				td[i*widths+widths-1-j]=t;
			}
	} else
		td=data;
		
	if (y>=ScreenHeight) {
		temp=0x10000-y;
		if (height<=temp) return; //ȫ�ڻ����⣬����Ҫ��
		height-=temp;
		td+=temp*widths;
		y=0;
	}
	if (y+height>ScreenHeight) height=ScreenHeight-y; //��ȥ����������
	if (x_bak>=ScreenWidth) {
		temp=0x10000-x_bak;
		if (width<=temp) return; //ȫ�ڻ����⣬����Ҫ��
		width-=temp;
		td+=temp;
		x_bak=0;
	}
	if (x_bak+width>ScreenWidth) width=ScreenWidth-x_bak;

	for (i=0;i<height;i++,y++) {
		j=width;
		xx=x_bak;yy=y;
		ByteAddr();
		disp=BmpData+m1l;
		tt=td;
		if ((lcmd&8) || (lcmd&7)==2) {
			switch (lcmd&7) {
			case 3:
				while (j--) {
					*disp|=(*tt++)^0xff;
					disp++;
				}
				break;
			case 4:
				while (j--) {
					*disp&=(*tt++)^0xff;
					disp++;
				}
				break;
			case 5:
				while (j--) {
					*disp^=(*tt++)^0xff;
					disp++;
				}
				break;													
			default:
				while (j--) {
					*disp++=(*tt++)^0xff;
				}
				break;
			}
		} else {
			switch (lcmd&7) {
			case 3:
				while (j--) {
					*disp|=*tt++;
					disp++;
				}
				break;
			case 4:
				while (j--) {
					*disp&=*tt++;
					disp++;
				}
				break;
			case 5:
				while (j--) {
					*disp^=*tt++;
					disp++;
				}
				break;
			case 6:
				while (j--) {
					t=*tt++;
					if (t) *disp++=t;
					else disp++;
				}
				break;
			default:
				/*while (j--) {
					*disp++=*tt++;
				}*/
				memcpy(disp,tt,j);
			}
		}
		td+=widths;
	}
}

void write_comm(word x,word y,word width,word height,byte *data)
{
	int i;
	register int j;
	byte t;
	word widths;
	int x_bak;
	const static byte msktbl[]={128,64,32,16,8,4,2,1}; 
	byte *td;
	register byte *disp,*tt;
	int temp;

	if (graph_mode==8) {
		if (lcmd==1) {
			write81(x,y,width,height,data);
		} else if (lcmd==6) {
			write86(x,y,width,height,data);
		} else {
			write8(x,y,width,height,data);
		}
		return;
	}
	x_bak=x;
	if (graph_mode==1) {
		widths=(width+7)>>3;
		if ((width&7)==0 && negative) {
			td=ram_base+MY_LOCAL_RAM;
			memcpy(td,data,widths*height);
			for (i=0;i<height;i++)
				for (j=0;j<widths/2;j++) {
					t=td[i*widths+j];
					td[i*widths+j]=td[i*widths+widths-1-j];
					td[i*widths+widths-1-j]=t;
				}
			for (i=0;i<widths*height;i++)
				td[i]=negative_tbl[td[i]];
		} else
			td=data;

		if (y>=ScreenHeight) {
			temp=0x10000-y;
			if (height<=temp) return; //ȫ�ڻ����⣬����Ҫ��
			height-=temp;
			td+=temp*widths;
			y=0;
		}
		if (y+height>ScreenHeight) height=ScreenHeight-y; //��ȥ����������
		if (x_bak>=ScreenWidth) {
			temp=0x10000-x_bak;
			if (width<=temp) return; //ȫ�ڻ����⣬����Ҫ��
			width-=temp;
			td+=temp>>3;
			if (temp&7) {
				width++;
				x_bak=0-(temp&7);
				if (width>ScreenWidth-x_bak) width=ScreenWidth-x_bak;
			} else {
				x_bak=0;
				if (width>ScreenWidth) width=ScreenWidth;
			}
		} else {
			if (x_bak+width>ScreenWidth) width=ScreenWidth-x_bak;
		}

		for (i=0;i<height;i++,y++) {
			if (x_bak<0) {
				j=-x_bak;
				x=0;
			} else {
				j=0;
				x=x_bak;
			}
			xx=x;yy=y;
			ByteAddr();
			disp=BmpData+m1l;
			tt=td;
			t=*tt++;
			if ((lcmd&8) || (lcmd&7)==2) {
				switch (lcmd&7) {
				case 3:
					while (j<width) {		
						if (!(t&msktbl[j&7])) *disp=1;
						disp++;
						if ((j&7)==7) t=*tt++;
						j++;	
					}
					break;
				case 4:
					while (j<width) {		
						if (t&msktbl[j&7]) *disp=0;
						disp++;
						if ((j&7)==7) t=*tt++;
						j++;	
					}
					break;
				case 5:
					while (j<width) {		
						if (!(t&msktbl[j&7])) *disp^=1;
						disp++;
						if ((j&7)==7) t=*tt++;
						j++;	
					}
					break;
				default:
					while (j<width) {		
						if (t&msktbl[j&7]) *disp=0;
						else *disp=1;
						disp++;
						if ((j&7)==7) t=*tt++;
						j++;	
					}
					break;
				}
			} else {
				switch (lcmd&7) {
					case 3:
					while (j<width) {		
						if (t&msktbl[j&7]) *disp=1;
						disp++;
						if ((j&7)==7) t=*tt++;
						j++;	
					}
					break;
				case 4:
					while (j<width) {		
						if (!(t&msktbl[j&7])) *disp=0;
						disp++;
						if ((j&7)==7) t=*tt++;
						j++;	
					}
					break;
				case 5:
					while (j<width) {		
						if (t&msktbl[j&7]) *disp^=1;
						disp++;
						if ((j&7)==7) t=*tt++;
						j++;	
					}
					break;
				default:
					while (j<width) {		
						if (t&msktbl[j&7]) *disp=1;
						else *disp=0;
						disp++;
						if ((j&7)==7) t=*tt++;
						j++;	
					}
					break;
				}
			}
			td+=widths;
		}
	} else if (graph_mode==4) {
		widths=(width+1)>>1;	
		if ((width&1)==0 && negative) {
			td=ram_base+MY_LOCAL_RAM;
			memcpy(td,data,widths*height);
			for (i=0;i<height;i++)
				for (j=0;j<widths/2;j++) {
					t=td[i*widths+j];
					td[i*widths+j]=td[i*widths+widths-1-j];
					td[i*widths+widths-1-j]=t;
				}
			for (i=0;i<widths*height;i++) {
				t=td[i];
				td[i]=(t<<4)|(t>>4);
			}
		} else
			td=data;
			
		if (y>=ScreenHeight) {
			temp=0x10000-y;
			if (height<=temp) return; //ȫ�ڻ����⣬����Ҫ��
			height-=temp;
			td+=temp*widths;
			y=0;
		}
		if (y+height>ScreenHeight) height=ScreenHeight-y; //��ȥ����������
		if (x_bak>=ScreenWidth) {
			temp=0x10000-x_bak;
			if (width<=temp) return; //ȫ�ڻ����⣬����Ҫ��
			width-=temp;
			td+=temp>>1;
			if (temp&1) {
				width++;
				x_bak=-1;
				if (width>ScreenWidth+1) width=ScreenWidth+1;
			} else {
				x_bak=0;
				if (width>ScreenWidth) width=ScreenWidth;
			}
		} else {
			if (x_bak+width>ScreenWidth) width=ScreenWidth-x_bak;
		}

		for (i=0;i<height;i++,y++) {
			if (x_bak==-1) {
				j=1;
				x=0;
			} else {
				j=0;
				x=x_bak;
			}
			xx=x;yy=y;
			ByteAddr();
			disp=BmpData+m1l;
			tt=td;
			t=*tt++;
			if ((lcmd&8) || (lcmd&7)==2) {
				switch (lcmd&7) {
				case 3:
					while (j<width) {		
						if (j&1) {
							*disp|=(t&0xf)^0xf;
							t=*tt++;
						} else *disp|=(t>>4)^0xf;
						disp++;
						j++;
					}
					break;
				case 4:
					while (j<width) {		
						if (j&1) {
							*disp&=(t&0xf)^0xf;
							t=*tt++;
						} else *disp&=(t>>4)^0xf;
						disp++;
						j++;
					}
					break;
				case 5:
					while (j<width) {		
						if (j&1) {
							*disp^=(t&0xf)^0xf;
							t=*tt++;
						} else *disp^=(t>>4)^0xf;
						disp++;
						j++;
					}
					break;													
				default:
					while (j<width) {		
						if (j&1) {
							*disp++=(t&0xf)^0xf;
							t=*tt++;
						} else *disp++=(t>>4)^0xf;
						j++;
					}
					break;
				}
			} else {
				switch (lcmd&7) {
				case 3:
					while (j<width) {		
						if (j&1) {
							*disp|=t&0xf;
							t=*tt++;
						} else *disp|=t>>4;
						disp++;
						j++;	
					}
					break;
				case 4:
					while (j<width) {		
						if (j&1) {
							*disp&=t&0xf;
							t=*tt++;
						} else *disp&=t>>4;
						disp++;
						j++;	
					}
					break;
				case 5:
					while (j<width) {		
						if (j&1) {
							*disp^=t&0xf;
							t=*tt++;
						} else *disp^=t>>4;
						disp++;
						j++;	
					}
					break;				
				default:
					while (j<width) {		
						if (j&1) {
							*disp++=t&0xf;
							t=*tt++;
						} else *disp++=t>>4;
						j++;
					}
				}
			}
			td+=widths;
		}
	}
}

void font_6x12(word x,word y,byte c)
{
	byte *addr;
	byte t,t2;
	int i;
	if (graph_mode==1) {
		if (c>=128) memset(patbuf,0,12);
		else memcpy(patbuf,ascii+c*12,12);
	} else if (graph_mode==4) {
		if (c>=128) memset(patbuf,0,36);
		else {
			addr=ascii+c*12;
			for (i=0;i<12;i++) {
				t=*addr++;
				if (t&0x80) t2=fgcolor<<4;
				else t2=bgcolor<<4;
				if (t&0x40) t2|=fgcolor;
				else t2|=bgcolor;
				patbuf[i*3]=t2;
				if (t&0x20) t2=fgcolor<<4;
				else t2=bgcolor<<4;
				if (t&0x10) t2|=fgcolor;
				else t2|=bgcolor;
				patbuf[i*3+1]=t2;
				if (t&8) t2=fgcolor<<4;
				else t2=bgcolor<<4;
				if (t&4) t2|=fgcolor;
				else t2|=bgcolor;
				patbuf[i*3+2]=t2;
			}
		}
	} else {
		if (c>=128) memset(patbuf,0,72);
		else {
			addr=ascii+c*12;
			for (i=0;i<12;i++) {
				t=*addr++;
				if (t&0x80) patbuf[i*6]=(byte)fgcolor;
				else patbuf[i*6]=(byte)bgcolor;
				if (t&0x40) patbuf[i*6+1]=(byte)fgcolor;
				else patbuf[i*6+1]=(byte)bgcolor;
				if (t&0x20) patbuf[i*6+2]=(byte)fgcolor;
				else patbuf[i*6+2]=(byte)bgcolor;
				if (t&0x10) patbuf[i*6+3]=(byte)fgcolor;
				else patbuf[i*6+3]=(byte)bgcolor;
				if (t&0x8) patbuf[i*6+4]=(byte)fgcolor;
				else patbuf[i*6+4]=(byte)bgcolor;
				if (t&0x4) patbuf[i*6+5]=(byte)fgcolor;
				else patbuf[i*6+5]=(byte)bgcolor;
			}
		}
	}
	write_comm(x,y,6,12,patbuf);
}

void font_8x16(word x,word y,byte c)
{
	byte *addr;
	byte t,t2;
	int i;
	if (graph_mode==1) {
		if (c>=128) memset(patbuf,0,16);
		else memcpy(patbuf,ascii8+c*16,16);
	} else if (graph_mode==4) {
		if (c>=128) memset(patbuf,0,64);
		else {
			addr=ascii8+c*16;
			for (i=0;i<16;i++) {
				t=*addr++;
				if (t&0x80) t2=fgcolor<<4;
				else t2=bgcolor<<4;
				if (t&0x40) t2|=fgcolor;
				else t2|=bgcolor;
				patbuf[i*4]=t2;
				if (t&0x20) t2=fgcolor<<4;
				else t2=bgcolor<<4;
				if (t&0x10) t2|=fgcolor;
				else t2|=bgcolor;
				patbuf[i*4+1]=t2;
				if (t&8) t2=fgcolor<<4;
				else t2=bgcolor<<4;
				if (t&4) t2|=fgcolor;
				else t2|=bgcolor;
				patbuf[i*4+2]=t2;
				if (t&2) t2=fgcolor<<4;
				else t2=bgcolor<<4;
				if (t&1) t2|=fgcolor;
				else t2|=bgcolor;
				patbuf[i*4+3]=t2;
			}
		}
	} else {
		if (c>=128) memset(patbuf,0,128);
		else {
			addr=ascii8+c*16;
			for (i=0;i<16;i++) {
				t=*addr++;
				if (t&0x80) patbuf[i*8]=(byte)fgcolor;
				else patbuf[i*8]=(byte)bgcolor;
				if (t&0x40) patbuf[i*8+1]=(byte)fgcolor;
				else patbuf[i*8+1]=(byte)bgcolor;
				if (t&0x20) patbuf[i*8+2]=(byte)fgcolor;
				else patbuf[i*8+2]=(byte)bgcolor;
				if (t&0x10) patbuf[i*8+3]=(byte)fgcolor;
				else patbuf[i*8+3]=(byte)bgcolor;
				if (t&0x8) patbuf[i*8+4]=(byte)fgcolor;
				else patbuf[i*8+4]=(byte)bgcolor;
				if (t&0x4) patbuf[i*8+5]=(byte)fgcolor;
				else patbuf[i*8+5]=(byte)bgcolor;
				if (t&0x2) patbuf[i*8+6]=(byte)fgcolor;
				else patbuf[i*8+6]=(byte)bgcolor;
				if (t&0x1) patbuf[i*8+7]=(byte)fgcolor;
				else patbuf[i*8+7]=(byte)bgcolor;
			}
		}
	}
	write_comm(x,y,8,16,patbuf);
}

void font_12x12(word x,word y,byte c1,byte c2)
{
	byte *addr;
	byte t,t2;
	int i,j;
	if (c1<0xb0) addr=gbfont+(((c1-0xa1)*94+(c2-0xa1))*24);
	else addr=gbfont+(((c1-0xa7)*94+(c2-0xa1))*24);
	if (graph_mode==1) memcpy(patbuf,addr,24);
	else if (graph_mode==4) {
		for (i=0,j=0;i<24;i++) {
			t=*addr++;
			if (t&0x80) t2=fgcolor<<4;
			else t2=bgcolor<<4;
			if (t&0x40) t2|=fgcolor;
			else t2|=bgcolor;
			patbuf[j++]=t2;
			if (t&0x20) t2=fgcolor<<4;
			else t2=bgcolor<<4;
			if (t&0x10) t2|=fgcolor;
			else t2|=bgcolor;
			patbuf[j++]=t2;
			if (i&1) continue;
			if (t&8) t2=fgcolor<<4;
			else t2=bgcolor<<4;
			if (t&4) t2|=fgcolor;
			else t2|=bgcolor;
			patbuf[j++]=t2;
			if (t&2) t2=fgcolor<<4;
			else t2=bgcolor<<4;
			if (t&1) t2|=fgcolor;
			else t2|=bgcolor;
			patbuf[j++]=t2;
		}
	} else {
		for (i=0,j=0;i<24;i++) {
			t=*addr++;
			if (t&0x80) patbuf[j++]=(byte)fgcolor;
			else patbuf[j++]=(byte)bgcolor;
			if (t&0x40) patbuf[j++]=(byte)fgcolor;
			else patbuf[j++]=(byte)bgcolor;
			if (t&0x20) patbuf[j++]=(byte)fgcolor;
			else patbuf[j++]=(byte)bgcolor;
			if (t&0x10) patbuf[j++]=(byte)fgcolor;
			else patbuf[j++]=(byte)bgcolor;
			if (i&1) continue;
			if (t&0x8) patbuf[j++]=(byte)fgcolor;
			else patbuf[j++]=(byte)bgcolor;
			if (t&0x4) patbuf[j++]=(byte)fgcolor;
			else patbuf[j++]=(byte)bgcolor;
			if (t&0x2) patbuf[j++]=(byte)fgcolor;
			else patbuf[j++]=(byte)bgcolor;
			if (t&0x1) patbuf[j++]=(byte)fgcolor;
			else patbuf[j++]=(byte)bgcolor;
		}
	}
	write_comm(x,y,12,12,patbuf);
}

void font_16x16(word x,word y,byte c1,byte c2)
{
	byte *addr;
	byte t,t2;
	int i;
	if (c1<0xb0) addr=gbfont16+(((c1-0xa1)*94+(c2-0xa1))*32);
	else addr=gbfont16+(((c1-0xa7)*94+(c2-0xa1))*32);
	if (graph_mode==1) memcpy(patbuf,addr,32);
	else  if (graph_mode==4) {
		for (i=0;i<32;i++) {
			t=*addr++;
			if (t&0x80) t2=fgcolor<<4;
			else t2=bgcolor<<4;
			if (t&0x40) t2|=fgcolor;
			else t2|=bgcolor;
			patbuf[i*4]=t2;
			if (t&0x20) t2=fgcolor<<4;
			else t2=bgcolor<<4;
			if (t&0x10) t2|=fgcolor;
			else t2|=bgcolor;
			patbuf[i*4+1]=t2;
			if (t&8) t2=fgcolor<<4;
			else t2=bgcolor<<4;
			if (t&4) t2|=fgcolor;
			else t2|=bgcolor;
			patbuf[i*4+2]=t2;
			if (t&2) t2=fgcolor<<4;
			else t2=bgcolor<<4;
			if (t&1) t2|=fgcolor;
			else t2|=bgcolor;
			patbuf[i*4+3]=t2;
		}
	} else {
		for (i=0;i<32;i++) {
			t=*addr++;
			if (t&0x80) patbuf[i*8]=(byte)fgcolor;
			else patbuf[i*8]=(byte)bgcolor;
			if (t&0x40) patbuf[i*8+1]=(byte)fgcolor;
			else patbuf[i*8+1]=(byte)bgcolor;
			if (t&0x20) patbuf[i*8+2]=(byte)fgcolor;
			else patbuf[i*8+2]=(byte)bgcolor;
			if (t&0x10) patbuf[i*8+3]=(byte)fgcolor;
			else patbuf[i*8+3]=(byte)bgcolor;
			if (t&0x8) patbuf[i*8+4]=(byte)fgcolor;
			else patbuf[i*8+4]=(byte)bgcolor;
			if (t&0x4) patbuf[i*8+5]=(byte)fgcolor;
			else patbuf[i*8+5]=(byte)bgcolor;
			if (t&0x2) patbuf[i*8+6]=(byte)fgcolor;
			else patbuf[i*8+6]=(byte)bgcolor;
			if (t&0x1) patbuf[i*8+7]=(byte)fgcolor;
			else patbuf[i*8+7]=(byte)bgcolor;
		}
	}
	write_comm(x,y,16,16,patbuf);
}

void block_comm()
{
	get_val();
	no_buf=a1&0x40;
	lcmd=a1&3;
	get_val();
	Y1=(word)a1;
	get_val();
	x1=(word)a1;
	get_val();
	Y0=(word)a1;
	get_val();
	x0=(word)a1;
}

void vline()
{
	xx=x0;
	if (Y0<Y1)
		for (yy=Y0;yy<=Y1;yy++) put_dot();
	else
		for (yy=Y1;yy<=Y0;yy++) put_dot();
}

void hline() 
//����ǰ�������Y0,x0,x1ʹ�����㣺
//Y0<ScreenHeight x0<ScreenWidth x1<ScreenWidth x0<=x1
{
	word width;
	byte *p;
	yy=Y0;
	xx=x0;
	ByteAddr();
	width=x1-x0+1;
	p=BmpData+m1l;
	//for (xx=x0;xx<=x1;xx++) put_dot();
	if (graph_mode==1) {
		switch (lcmd) {
		case 0:
			memset(p,0,width);
			break;
		case 1:
			memset(p,1,width);
			break;
		case 2:
			while (width--) {
				*p^=1;
				p++;
			}
			break;
		}
	} else if (graph_mode==4) {
		switch (lcmd) {
		case 0:
			memset(p,(byte)bgcolor,width);
			break;
		case 1:
			memset(p,(byte)fgcolor,width);
			break;
		case 2:
			while (width--) {
				*p^=15;
				p++;
			}
			break;
		}
	} else {
		switch (lcmd) {
		case 0:
			memset(p,(byte)bgcolor,width);
			break;
		case 1:
			memset(p,(byte)fgcolor,width);
			break;
		case 2:
			while (width--) {
				*p^=255;
				p++;
			}
			break;
		}
	}
}

void block_check()
{
	word t;
	if (Y0>Y1) {
		t=Y0;
		Y0=Y1;
		Y1=t;
	}
	if (x0>x1) {
		t=x0;
		x0=x1;
		x1=t;
	}
	if (x0>=ScreenWidth) x0=ScreenWidth-1;
	if (x1>=ScreenWidth) x1=ScreenWidth-1;
	if (Y0>=ScreenHeight) Y0=ScreenHeight-1;
	if (Y1>=ScreenHeight) Y1=ScreenHeight-1;
}

int hline_check()
{
	word t;
	if (Y0>=ScreenHeight) return 0; //������Ļ��
	if (x0>x1) {
		t=x0;
		x0=x1;
		x1=t;
	}
	if (x0>=ScreenWidth) return 0; //������Ļ��
	if (x1>=ScreenWidth) x1=ScreenWidth-1;
	return 1;
}

void block_draw()
{
	word t;
	block_check();
	t=Y1-Y0+1;
	while (t) {
		hline();
		Y0++;
		t--;
	}
}

void square_draw()
{
	word t;
	block_check();
	hline();
	vline();
	t=x0;
	x0=x1;
	vline();
	x0=t;
	Y0=Y1;
	hline();
}

void scr_up()
{
	memcpy(lRam+TextBuffer,lRam+TextBuffer+curr_CPR,curr_CPR*(curr_RPS-1));
	memset(lRam+TextBuffer+curr_CPR*(curr_RPS-1),0x20,curr_CPR);
	scr_off=curr_CPR*(curr_RPS-1);
	scr_x=0;
	scr_y=curr_RPS-1;
}

void cout(byte c)
{
	if (scr_y>=curr_RPS) scr_up();
	if (c==0x0d) return;
	if (c==0x0a) {
		scr_x=0;
		scr_y++;
		if (scr_y>=curr_RPS) scr_up();
		else scr_off=scr_y*curr_CPR;
		return;
	}
	if (c==9) c=0x20;
	lRam[TextBuffer+scr_off++]=c;
	if (++scr_x>=curr_CPR) {
		scr_x=0;
		scr_y++;
	}
}

void update_lcd_small() //Test û�п��Ƿ���
{
	byte c,c2;
	unsigned long mask;
	int i,j,color;
	if (curr_RPS<=8) mask=0x80;
	else if (curr_RPS<=16) mask=0x8000;
	else mask=0x800000;
	no_buf=0x40;
	lcmd=0;
	if (graph_mode==1) color=0;
	else color=bgcolor;
	for (i=0;i<small_up;i++)
		memset(BmpData+i*LCD_WIDTH,color,LCD_WIDTH);
	for (i=ScreenHeight-small_down;i<ScreenHeight;i++)
		memset(BmpData+i*LCD_WIDTH,color,LCD_WIDTH);
	for (i=1;i<curr_RPS;i++)
		memset(BmpData+(i*13+small_up-1)*LCD_WIDTH,color,LCD_WIDTH);
	Y0=0;Y1=ScreenHeight-1;
	for (i=0;i<small_left;i++) {
		x0=i;vline();
		x0=ScreenWidth-1-i;vline();
	}
	lcmd=1;
	for (i=0;i<curr_RPS;i++,mask>>=1) {
		if (mask&line_mode) continue;
		for (j=0;j<curr_CPR;j++) {
			c=lRam[TextBuffer+i*curr_CPR+j];
			if (c<128) font_6x12((word)(j*6+small_left),(word)(i*13+small_up),c);
			else {
				c2=lRam[TextBuffer+i*curr_CPR+j+1];
				font_12x12((word)(j*6+small_left),(word)(i*13+small_up),c,c2);
				j++;
			}
		}
	}
}

void update_lcd_large() //Test û�п��Ƿ���
{
	byte c,c2;
	unsigned long mask;
	int i,j;
	if (curr_RPS<=8) mask=0x80;
	else if (curr_RPS<=16) mask=0x8000;
	else mask=0x800000;
	no_buf=0x40;
	lcmd=1;
	for (i=0;i<curr_RPS;i++,mask>>=1) {
		if (mask&line_mode) continue;
		for (j=0;j<curr_CPR;j++) {
			c=lRam[TextBuffer+i*curr_CPR+j];
			if (c<128) font_8x16((word)(j<<3),(word)(i<<4),c);
			else {
				c2=lRam[TextBuffer+i*curr_CPR+j+1];
				font_16x16((word)(j<<3),(word)(i<<4),c,c2);
				j++;
			}
		}
	}
}

void update_lcd_0()
{
	negative=0;
	if (scr_mode==0) update_lcd_large();
	else update_lcd_small();
}

void next_arg()
{
	func_x+=4;
	a1=*(long *)(lRam+eval_stack+func_x);
}

void printint(int digit,int flag)
{
	char num[20];
	int i,real_digit;

	next_arg();
	sprintf(num,"%ld",a1);
	real_digit=strlen(num);
	if (digit==0 || real_digit>=digit) {
		for (i=0;;i++) {
			if (num[i]) cout(num[i]);
			else break;
		}
	} else {
		if (flag==0x80) {
			for (i=0;;i++) {
				if (num[i]) cout(num[i]);
				else break;
			}
			for (i=0;i<digit-real_digit;i++) {
				cout(' ');
			}
		} else if (flag==0x40) {
			for (i=0;i<digit-real_digit;i++) {
				cout('0');
			}
			for (i=0;;i++) {
				if (num[i]) cout(num[i]);
				else break;
			}
		} else {
			for (i=0;i<digit-real_digit;i++) {
				cout(' ');
			}
			for (i=0;;i++) {
				if (num[i]) cout(num[i]);
				else break;
			}
		}
	}
}

void printfloat()
{
	char num[20];
	int i,flag;
	next_arg();
	if (((a1>>23)&0xff)==0xff) {
		strcpy(num,"error");
	} else
		sprintf(num,"%g",i2f(a1));
	flag=0;
	for (i=0;;i++) {
		if (num[i]=='e' && i) flag=1;
		if (flag) flag++;
		if (flag==4) continue; //ת��x.xxxxxe+0yyΪx.xxxxxe+yy
		if (num[i]) cout(num[i]);
		else break;
	}
}

void PutPixelx(int x,int y)
{
	if (x<0 || y<0 || x>=ScreenWidth || y>=ScreenHeight) return;
	xx=x;yy=y;
	put_dot();
}

int circle_buf[320];

void put_dot4(int x0,int y0,int x,int y,int mode)
{
	if (mode) {
		if (y0-y>=0 && y0-y<ScreenHeight)
			if (x>=circle_buf[y0-y]) circle_buf[y0-y]=x;
		if (y0+y>=0 && y0+y<ScreenHeight)
			if (x>=circle_buf[y0+y]) circle_buf[y0+y]=x;	
	} else {
		if (x==0) {
			PutPixelx(x0,y0+y);
			PutPixelx(x0,y0-y);
		} else if (y==0) {
			PutPixelx(x0+x,y0);
			PutPixelx(x0-x,y0);
		} else {
			PutPixelx(x0+x,y0+y);
			PutPixelx(x0-x,y0+y);
			PutPixelx(x0+x,y0-y);
			PutPixelx(x0-x,y0-y);
		}
	}
}

void Ellipsex(short x0,short y0,word r1,word r2,int mode)
{
	int i,j,fxy,fx,fy,incx,incy,temp_x,temp_y;
	word delta_x,delta_y,distant_a,distant_b,circle_r,dot_start;

	if (mode) for (i=0;i<ScreenHeight;i++) circle_buf[i]=-1;
	distant_a=r1;
	distant_b=r2;
	dot_start=0;
	if (distant_a==0 && distant_a==0) {
		PutPixelx(x0,y0);
		return;
	}
	circle_r=(distant_a>distant_b)?distant_a:distant_b;
	incx=-1;
	incy=1;
	fy=1;
	fx=1-2*circle_r;
	fxy=0;
	delta_x=0;
	delta_y=0;
	temp_x=distant_a;
	temp_y=0;
	put_dot4(x0,y0,temp_x,temp_y,mode);
	do {
	if (fxy>=0) {
		delta_x+=distant_a;
		if (delta_x>=circle_r) {
			temp_x+=incx;
			delta_x-=circle_r;
			if (temp_x+1!=distant_a)
				put_dot4(x0,y0,temp_x,temp_y,mode);
		}
		fxy-=abs(fx);
		fx+=2;
		if (fx<0 || fx>=3) continue;
		incy=-incy;
		fy=-fy+2;
		fxy=-fxy;
	} else {
		delta_y+=distant_b;
		if (delta_y>=circle_r) {
			delta_y-=circle_r;
			temp_y+=incy;
			if ((temp_y==1 || temp_y==2) && dot_start==0) {
				put_dot4(x0,y0,distant_a,temp_y,mode);
			} else {
				dot_start=1;
				put_dot4(x0,y0,temp_x,temp_y,mode);
			}
		}
		fxy=fxy+abs(fy);
		fy+=2;
		if (fy<0 || fy>2) continue;
		incx=-incx;
		fx=-fx+2;
		fxy=-fxy;
	}
	} while (temp_x);
	if (mode) {
		for (i=0;i<ScreenHeight;i++) {
			if (circle_buf[i]>=0) {
				for (j=0;j<circle_buf[i]*2+1;j++) PutPixelx(x0-circle_buf[i]+j,i);
			}
		}
	}
}

void c_putchar()
{
	get_val();
	cout((byte)a1);
	update_lcd_0();
}

a32 fenxi_fmt(a32 s,int *digit,int *flag)
{
	byte c;
	int have_0,have_fu,total_wei;

	have_0=0;
	have_fu=0;
	total_wei=0;
	for (;;) {
		c=lRamRead(s++);
		if (c=='0') have_0=1;
		else if (c=='-') have_fu=1;
		else if (c>'0' && c<='9') {
			total_wei=c-'0';
			for (;;) {
				c=lRamRead(s++);
				if (c>='0' && c<='9') {
					total_wei=total_wei*10+c-'0';
				} else {
					s--;
					break;
				}
			}
		} else {
			s--;
			break;
		}
	}
	*digit=total_wei;
	if (total_wei) {
		if (have_fu) *flag=0x80;
		else if (have_0) *flag=0x40;
	} else *flag=0;
	return s;
}

void c_printf()
{
	a32 fmt_str,str2;
	byte c;
	int digit,flag;

	get_val();
	eval_top-=(byte)(a1<<2);
	func_x=eval_top;
	if (RamBits>16) fmt_str=*(long *)(lRam+eval_stack+eval_top)&0xffffff;
	else fmt_str=*(word *)(lRam+eval_stack+eval_top);
	for (;;) {
		c=lRamRead(fmt_str++);
		if (c==0) break;
		if (c=='%') {
			fmt_str=fenxi_fmt(fmt_str,&digit,&flag);
			c=lRamRead(fmt_str++);
			if (c==0) break;
			if (c=='d') printint(digit,flag);
			else if (c=='f') printfloat();
			else if (c=='%') cout(c);
			else if (c=='c') {
				next_arg();
				cout((byte)a1);
			} else if (c=='s') {
				next_arg();
				if (RamBits>16) str2=a1&0xffffff;
				else str2=(word)a1;
				for (;;) {
					c=lRamRead(str2++);
					if (c==0) break;
					if (c<128) cout(c);
					else {
						if (scr_x+1>=curr_CPR) cout(0x20);
						cout(c);
						c=lRamRead(str2++);
						if (c==0) break;
						cout(c);
					}
				}
			} else cout(c);
		} else if (c<128) {cout(c);
		} else {
			if (scr_x+1>=curr_CPR) cout(0x20);
			cout(c);
			c=lRamRead(fmt_str++);
			if (c==0) break;
			cout(c);
		}
	}
	update_lcd_0();
}

void c_strcpy()
{
	a32 t1,t2;
	int i;
	get_val2();
	get_val();
	if (RamBits>16) {
		t1=a1&0xffffff;
		t2=a3&0xffffff;
	} else {
		t1=(word)a1;
		t2=(word)a3;
	}
	for (i=0;;i++) if (lRamRead(t2+i)==0) break;
	i++;
	lmemcpy(t1,t2,i,2,1);
}

void c_strlen()
{
	a32 t;
	get_val();
	if (RamBits>16) t=a1&0xffffff;
	else t=(word)a1;
	for (a1=0;;a1++) if (lRamRead(t+a1)==0) break;
	put_val();
}

void setscreen(int mode)
{
	if (mode) {
		scr_mode=1;
		curr_CPR=((ScreenWidth-2)/6)&0xfe;
		curr_RPS=(ScreenHeight-1)/13;
		small_left=(ScreenWidth-curr_CPR*6)/2;
		small_up=(ScreenHeight-(curr_RPS*13-1))/2;
		small_down=ScreenHeight-(curr_RPS*13-1)-small_up;
	} else {
		scr_mode=0;
		curr_CPR=ScreenWidth/8;
		curr_RPS=ScreenHeight/16;
	}
	scr_x=0;scr_y=0;scr_off=0;
	memset(lRam+TextBuffer,0,curr_CPR*curr_RPS);
}

void c_setscreen()
{
	get_val();
	setscreen(a1&0xff);
}

void c_updatelcd()
{
	get_val();
	line_mode=a1;
	update_lcd_0();
	line_mode=0;
}

void c_writeblock()
{
	a32 data;
	word x,y,width,height;
	get_val();
	if (RamBits>16) data=a1&0xffffff;
	else data=(word)a1;
	get_val();
	no_buf=a1&0x40;
	negative=a1&0x20;
	lcmd=a1&0xf;
	get_val();
	height=(word)a1;
	get_val();
	width=(word)a1;
	get_val();
	y=(word)a1;
	get_val();
	x=(word)a1;
	{
		size_t row_bytes;
		byte *bitmap;
		if (graph_mode==1) row_bytes=((size_t)width+7)/8;
		else if (graph_mode==4) row_bytes=((size_t)width+1)/2;
		else row_bytes=width;
		bitmap=vm_memory_pointer(data,row_bytes*height,"bitmap data");
		if (!bitmap) return;
		write_comm(x,y,width,height,bitmap);
	}
}

void scroll_to_lcd()
{
	memcpy(BmpData,BmpData+SCROLL_CON,LCD_WIDTH*LCD_HEIGHT);
}

void c_textout()
{
	byte t,c;
	word font_x,font_y;
	a32 addr;
	get_val();
	no_buf=a1&0x40;
	negative=a1&0x20;
	lcmd=a1&0xf;
	t=(byte)a1;
	get_val();
	if (RamBits>16) addr=a1&0xffffff;
	else addr=(word)a1;
	get_val();
	font_y=(word)a1;
	get_val();
	font_x=(word)a1;
	if ((t&0x80)==0) {
		for (;;) {
			if (font_x>=ScreenWidth) break;
			c=lRamRead(addr++);
			if (c==0) break;
			if (c<128) {
				font_6x12(font_x,font_y,c);
				font_x+=6;
			} else {
				t=lRamRead(addr++);
				font_12x12(font_x,font_y,c,t);
				font_x+=12;
			}
		}
	} else {
		for (;;) {
			if (font_x>=ScreenWidth) break;
			c=lRamRead(addr++);
			if (c==0) break;
			if (c<128) {
				font_8x16(font_x,font_y,c);
				font_x+=8;
			} else {
				t=lRamRead(addr++);
				font_16x16(font_x,font_y,c,t);
				font_x+=16;
			}
		}
	}
}

void c_block()
{
	block_comm();
	block_draw();
}

void c_rectangle()
{
	block_comm();
	square_draw();
}

void c_clearscreen()
{
	int i;

	if (graph_mode==1) {
		for (i=0;i<ScreenHeight;i++) {
			memset(BmpData+SCROLL_CON+i*LCD_WIDTH,0,ScreenWidth);
		}
	} else {
		for (i=0;i<ScreenHeight;i++) {
			memset(BmpData+SCROLL_CON+i*LCD_WIDTH,bgcolor,ScreenWidth);
		}
	}
}

void c_abs()
{
	get_val();
	if (a1<0) a1=0-a1;
	put_val();
}

void c_rand()
{
	a1=seed*0x15a4e35+1;
	seed=a1;
	a1=(a1>>16)&0x7fff;
	put_val();
}

void c_srand()
{
	get_val();
	seed=a1;
}

void c_locate()
{
	byte t;
	get_val();
	t=(byte)a1;
	if (t>=curr_CPR) t=curr_CPR-1;
	scr_x=t;
	get_val();
	t=(byte)a1;
	if (t>=curr_RPS) t=curr_RPS-1;
	scr_y=t;
	scr_off=scr_y*curr_CPR+scr_x;
}

void c_point()
{
	get_val();
	no_buf=(a1&0x40)^0x40;
	lcmd=a1&3;
	get_val();
	yy=(word)a1;
	get_val();
	xx=(word)a1;
	put_dot();
}

void c_getpoint()
{
	get_val();
	yy=(word)a1;
	get_val();
	xx=(word)a1;
	no_buf=0x40;
	a1=get_dot();
	put_val();
}

void c_line()
{
	word delta_x,delta_y,distance,tt,xerr,yerr;
	word t;
	int incy;
	block_comm();
	no_buf^=0x40;
	if (x0==x1) {
		vline();
		return;
	}
	if (Y0==Y1) {
		if (hline_check())
			hline();
		return;
	}
	if (x1<x0) {
		t=x0;
		x0=x1;
		x1=t;
		t=Y0;
		Y0=Y1;
		Y1=t;
	}
	delta_x=x1-x0;
	delta_y=abs(Y1-Y0);
	if (Y1>Y0) incy=1;
	else incy=-1;
	distance=(delta_x>delta_y)?delta_x:delta_y;
	tt=0;
	xerr=0;
	yerr=0;
	xx=x0;
	yy=Y0;
	for (;;) {
		put_dot();
		xerr+=delta_x;
		yerr+=delta_y;
		if (xerr>=distance) {
			xerr-=distance;
			xx++;
		}
		if (yerr>=distance) {
			yerr-=distance;
			yy+=incy;
		}
		tt++;
		if (distance<tt) break;
	}
}

void c_box()
{
	byte t;
	get_val();
	lcmd=a1&3;
	no_buf=0x40;
	get_val();
	t=(byte)a1;
	get_val();
	Y1=(word)a1;
	get_val();
	x1=(word)a1;
	get_val();
	Y0=(word)a1;
	get_val();
	x0=(word)a1;
	if (t) block_draw();
	else square_draw();
}

void XorLine(int s,int e)
{
	x0=0;x1=ScreenWidth-1;
	Y0=s;Y1=e;
	lcmd=2;no_buf=0x40;
	block_draw();
}

void c_circle()
{
	word mode,r,x,y;
	get_val();
	no_buf=(a1&0x40)^0x40;
	lcmd=a1&3;
	get_val();
	mode=(word)a1;
	get_val();
	r=(word)a1;
	get_val();
	y=(word)a1;
	get_val();
	x=(word)a1;
	Ellipsex(x,y,r,r,mode);
}

void c_ellipse()
{
	word mode,ra,rb,x,y;
	get_val();
	no_buf=(a1&0x40)^0x40;
	lcmd=a1&3;
	get_val();
	mode=(word)a1;
	get_val();
	rb=(word)a1;
	get_val();
	ra=(word)a1;
	get_val();
	y=(word)a1;
	get_val();
	x=(word)a1;
	Ellipsex(x,y,ra,rb,mode);
}

void c_beep()
{
	;
}

void c_isalnum()
{
	get_val();
	a1=isalnum(a1);
	if (a1) a1=LTRUE;
	put_val();
}

void c_isalpha()
{
	get_val();
	a1=isalpha(a1);
	if (a1) a1=LTRUE;
	put_val();
}

void c_iscntrl()
{
	get_val();
	a1=iscntrl(a1);
	if (a1) a1=LTRUE;
	put_val();
}

void c_isdigit()
{
	get_val();
	a1=isdigit(a1);
	if (a1) a1=LTRUE;
	put_val();
}

void c_isgraph()
{
	get_val();
	a1=isgraph(a1);
	if (a1) a1=LTRUE;
	put_val();
}

void c_islower()
{
	get_val();
	a1=islower(a1);
	if (a1) a1=LTRUE;
	put_val();
}

void c_isprint()
{
	get_val();
	a1=isprint(a1);
	if (a1) a1=LTRUE;
	put_val();
}

void c_ispunct()
{
	get_val();
	a1=ispunct(a1);
	if (a1) a1=LTRUE;
	put_val();
}

void c_isspace()
{
	get_val();
	a1=isspace(a1);
	if (a1) a1=LTRUE;
	put_val();
}

void c_isupper()
{
	get_val();
	a1=isupper(a1);
	if (a1) a1=LTRUE;
	put_val();
}

void c_isxdigit()
{
	get_val();
	a1=isxdigit(a1);
	if (a1) a1=LTRUE;
	put_val();
}

void c_strcat()
{
	a32 t1,t2;
	byte c;
	get_val2();
	get_val();
	if (RamBits>16) {
		t1=a1&0xffffff;
		t2=a3&0xffffff;
	} else {
		t1=(word)a1;
		t2=(word)a3;
	}
	for (;;) {
		if (lRamRead(t1)==0) break;
		t1++;
	}
	for (;;) {
		c=lRamRead(t2++);
		lRamWrite(t1++,c);
		if (c==0) break;
	}
}

void c_strchr()
{
	byte *p;
	a32 t1;
	byte t2;
	get_val2();
	get_val();
	if (RamBits>16) t1=a1&0xffffff;
	else t1=(word)a1;
	t2=(byte)a3;
	{
		char *string=vm_cstring_pointer(t1,"strchr string");
		if (!string) return;
		p=(byte*)strchr(string,t2);
	}
	if (p==NULL) a1=0;
	else a1=p-lRam;
	put_val();
}

void c_strcmp()
{
	a32 t1,t2;
	int t;
	get_val2();
	get_val();
	if (RamBits>16) {
		t1=a1&0xffffff;
		t2=a3&0xffffff;
	} else {
		t1=(word)a1;
		t2=(word)a3;
	}
	{
		char *left=vm_cstring_pointer(t1,"strcmp left string");
		char *right=vm_cstring_pointer(t2,"strcmp right string");
		if (!left || !right) return;
		t=strcmp(left,right);
	}
	if (t==0) a1=0;
	else if (t>0) a1=1;
	else a1=-1;
	put_val();
}

void c_strstr()
{
	byte *p;
	a32 t1,t2;
	get_val2();
	get_val();
	if (RamBits>16) {
		t1=a1&0xffffff;
		t2=a3&0xffffff;
	} else {
		t1=(word)a1;
		t2=(word)a3;
	}
	{
		char *haystack=vm_cstring_pointer(t1,"strstr haystack");
		char *needle=vm_cstring_pointer(t2,"strstr needle");
		if (!haystack || !needle) return;
		p=(byte*)strstr(haystack,needle);
	}
	if (p==NULL) a1=0;
	else a1=p-lRam;
	put_val();
}

void c_tolower()
{
	byte c;
	get_val();
	c=(byte)a1;
	a1=tolower(c);
	put_val();
}

void c_toupper()
{
	byte c;
	get_val();
	c=(byte)a1;
	a1=toupper(c);
	put_val();
}

void c_memset()
{
	a32 len;
	get_val();
	if (RamBits>16) len=a1&0xffffff;
	else len=(word)a1;
	get_val2();
	get_val();
	if (RamBits>16) a1&=0xffffff;
	else a1&=0xffff;
	if (len==0) return;
	{
		byte *destination=vm_memory_pointer((a32)a1,len,"memset");
		if (destination) memset(destination,(byte)a3,len);
	}
}

void c_memcpy()
{
	a32 len;
	get_val();
	if (RamBits>16) len=a1&0xffffff;
	else len=(word)a1;
	get_val2();
	get_val();
	if (RamBits>16) {
		a1&=0xffffff;
		a3&=0xffffff;
	} else {
		a1&=0xffff;
		a3&=0xffff;
	}
	if (len==0) return;
	lmemcpy(a1,a3,len,6,7);
}

void c_sprintf()
{
	a32 str1,fmt_str,str2;
	byte c;
	char num[20];
	int i,digit,flag,real_digit;

	get_val();
	eval_top-=(byte)(a1<<2);
	func_x=eval_top;
	if (RamBits>16) str1=*(long *)(lRam+eval_stack+eval_top)&0xffffff;
	else str1=*(word *)(lRam+eval_stack+eval_top);
	next_arg();
	if (RamBits>16) fmt_str=a1&0xffffff;
	else fmt_str=(word)a1;
	for (;;) {
		c=lRamRead(fmt_str++);
		if (c==0) break;
		if (c=='%') {
			fmt_str=fenxi_fmt(fmt_str,&digit,&flag);
			c=lRamRead(fmt_str++);
			if (c==0) break;
			if (c=='d') {
				next_arg();
				sprintf(num,"%ld",a1);
				real_digit=strlen(num);
				if (digit==0 || real_digit>=digit) {
					for (i=0;;i++) {
						if (num[i]) lRamWrite(str1++,num[i]);
						else break;
					}
				} else {
					if (flag==0x80) {
						for (i=0;;i++) {
							if (num[i]) lRamWrite(str1++,num[i]);
							else break;
						}
						for (i=0;i<digit-real_digit;i++) {
							lRamWrite(str1++,' ');
						}
					} else if (flag==0x40) {
						for (i=0;i<digit-real_digit;i++) {
							lRamWrite(str1++,'0');
						}
						for (i=0;;i++) {
							if (num[i]) lRamWrite(str1++,num[i]);
							else break;
						}
					} else {
						for (i=0;i<digit-real_digit;i++) {
							lRamWrite(str1++,' ');
						}
						for (i=0;;i++) {
							if (num[i]) lRamWrite(str1++,num[i]);
							else break;
						}
					}
				}
			} else if (c=='f') {
				int flag;
				next_arg();
				if (((a1>>23)&0xff)==0xff) {
					strcpy(num,"error");
				} else
					sprintf(num,"%g",i2f(a1));
				flag=0;
				for (i=0;;i++) {
					if (num[i]=='e' && i) flag=1;
					if (flag) flag++;
					if (flag==4) continue; //ת��x.xxxxxe+0yyΪx.xxxxxe+yy
					if (num[i]) lRamWrite(str1++,num[i]);
					else break;
				}
			} else if (c=='%') lRamWrite(str1++,c);
			else if (c=='c') {
				next_arg();
				lRamWrite(str1++,(byte)a1);
			} else if (c=='s') {
				next_arg();
				if (RamBits>16) str2=a1&0xffffff;
				else str2=(word)a1;
				for (;;) {
					c=lRamRead(str2++);
					if (c==0) break;
					lRamWrite(str1++,c);
				}
			} else lRamWrite(str1++,c);
		} else {
			lRamWrite(str1++,c);
		}
	}
	lRamWrite(str1,0);
}

void c_memmove()
{
	a32 len;
	get_val();
	if (RamBits>16) len=a1&0xffffff;
	else len=(word)a1;
	get_val2();
	get_val();
	if (RamBits>16) {
		a1&=0xffffff;
		a3&=0xffffff;
	} else {
		a1&=0xffff;
		a3&=0xffff;
	}
	if (len==0) return;
	lmemcpy((a32)a1,(a32)a3,(int)len,6,7);
}

void c_crc16()
{
	a32 t1,t2;
	byte c1,c2,x;
	get_val2();
	get_val();
	if (RamBits>16) {
		t1=a1&0xffffff;
		t2=a3&0xffffff;
	} else {
		t1=(word)a1;
		t2=(word)a3;
	}
	c1=0;c2=0;
	while (t2--) {
		x=c2^lRamRead(t1++);
		c2=crc2[x]^c1;
		c1=crc1[x];
	}
	a1=c1+(c2<<8);
	put_val();
}

void c_jiami()
{
	a32 str1,str2,len;
	int i;
	byte c;
	get_val();
	if (RamBits>16) str2=a1&0xffffff;
	else str2=(word)a1;
	get_val2();
	get_val();
	if (RamBits>16) str1=a1&0xffffff;
	else str1=(word)a1;
	if (RamBits>16) len=a3&0xffffff;
	else len=(word)a3;
	i=0;
	while (len--) {
		c=lRamRead(str2+i++);
		if (c==0) {
			c=lRamRead(str2);
			i=1;
		}
		lRam[str1]^=c;
		str1++;
	}
}

void c_xdraw()
{
	byte t;
	byte tb[320];
	int i,j;
	get_val();
	t=(byte)a1;
	if (graph_mode==1) {
		if (t==0) {
			j=0;
			for (i=0;i<ScreenHeight;i++) {
				memcpy(BmpData+SCROLL_CON+j,BmpData+SCROLL_CON+j+1,ScreenWidth-1);
				BmpData[SCROLL_CON+j+ScreenWidth-1]=0;
				j+=LCD_WIDTH;
			}
		} else if (t==1) {
			j=0;
			for (i=0;i<ScreenHeight;i++) {
				memmove(BmpData+SCROLL_CON+j+1,BmpData+SCROLL_CON+j,ScreenWidth-1);
				BmpData[SCROLL_CON+j]=0;
				j+=LCD_WIDTH;
			}
		} else if (t==2) {
			memcpy(BmpData+SCROLL_CON,BmpData+SCROLL_CON+LCD_WIDTH,LCD_WIDTH*(ScreenHeight-1));
			memset(BmpData+SCROLL_CON+LCD_WIDTH*(ScreenHeight-1),0,LCD_WIDTH);
		} else if (t==3) {
			memmove(BmpData+SCROLL_CON+LCD_WIDTH,BmpData+SCROLL_CON,LCD_WIDTH*(ScreenHeight-1));
			memset(BmpData+SCROLL_CON,0,LCD_WIDTH);
		} else if (t==4) {
			for (i=0;i<ScreenHeight;i++)
				for (j=0;j<ScreenWidth/2;j++) {
					t=BmpData[SCROLL_CON+i*LCD_WIDTH+j];
					BmpData[SCROLL_CON+i*LCD_WIDTH+j]=BmpData[SCROLL_CON+i*LCD_WIDTH+ScreenWidth-1-j];
					BmpData[SCROLL_CON+i*LCD_WIDTH+ScreenWidth-1-j]=t;
				}
		} else if (t==5) {
			for (i=0;i<ScreenHeight/2;i++) {
				memcpy(tb,BmpData+SCROLL_CON+i*LCD_WIDTH,ScreenWidth);
				memcpy(BmpData+SCROLL_CON+i*LCD_WIDTH,BmpData+SCROLL_CON+(ScreenHeight-1-i)*LCD_WIDTH,ScreenWidth);
				memcpy(BmpData+SCROLL_CON+(ScreenHeight-1-i)*LCD_WIDTH,tb,ScreenWidth);
			}
		} else if (t==6) {
			memcpy(BmpData+SCROLL_CON,BmpData,LCD_WIDTH*LCD_HEIGHT);
		}
	} else {
		if (t==0) {
			j=0;
			for (i=0;i<ScreenHeight;i++) {
				memcpy(BmpData+SCROLL_CON+j,BmpData+SCROLL_CON+j+1,ScreenWidth-1);
				BmpData[SCROLL_CON+j+ScreenWidth-1]=(byte)bgcolor;
				j+=LCD_WIDTH;
			}
		} else if (t==1) {
			j=0;
			for (i=0;i<ScreenHeight;i++) {
				memmove(BmpData+SCROLL_CON+j+1,BmpData+SCROLL_CON+j,ScreenWidth-1);
				BmpData[SCROLL_CON+j]=(byte)bgcolor;
				j+=LCD_WIDTH;
			}
		} else if (t==2) {
			memcpy(BmpData+SCROLL_CON,BmpData+SCROLL_CON+LCD_WIDTH,LCD_WIDTH*(ScreenHeight-1));
			memset(BmpData+SCROLL_CON+LCD_WIDTH*(ScreenHeight-1),bgcolor,LCD_WIDTH);
		} else if (t==3) {
			memmove(BmpData+SCROLL_CON+LCD_WIDTH,BmpData+SCROLL_CON,LCD_WIDTH*(ScreenHeight-1));
			memset(BmpData+SCROLL_CON,bgcolor,LCD_WIDTH);
		} else if (t==4) {
			for (i=0;i<ScreenHeight;i++)
				for (j=0;j<ScreenWidth/2;j++) {
					t=BmpData[SCROLL_CON+i*LCD_WIDTH+j];
					BmpData[SCROLL_CON+i*LCD_WIDTH+j]=BmpData[SCROLL_CON+i*LCD_WIDTH+ScreenWidth-1-j];
					BmpData[SCROLL_CON+i*LCD_WIDTH+ScreenWidth-1-j]=t;
				}
		} else if (t==5) {
			for (i=0;i<ScreenHeight/2;i++) {
				memcpy(tb,BmpData+SCROLL_CON+i*LCD_WIDTH,ScreenWidth);
				memcpy(BmpData+SCROLL_CON+i*LCD_WIDTH,BmpData+SCROLL_CON+(ScreenHeight-1-i)*LCD_WIDTH,ScreenWidth);
				memcpy(BmpData+SCROLL_CON+(ScreenHeight-1-i)*LCD_WIDTH,tb,ScreenWidth);
			}
		} else if (t==6) {
			memcpy(BmpData+SCROLL_CON,BmpData,LCD_WIDTH*LCD_HEIGHT);
		}
	}
}

void c_getblock()
{
	word width,height;
	int i,j;
	byte t;

	get_val2();
	get_val();
	no_buf=a1&0x40;
	get_val();
	height=(word)a1;
	get_val();
	if (graph_mode==1)
		width=(word)a1>>3;
	else if (graph_mode==4)
		width=(word)(a1&0xfff8)>>1; //ʹ������8��������
	else
		width=(word)a1;
	get_val();
	yy=(word)a1;
	get_val();
	if (graph_mode==8) xx=(word)a1;
	else xx=(word)(a1&0xfff8); //ʹxx��8��������
	if (width==0 || height==0 || xx>=ScreenWidth || yy>=ScreenHeight) return; //�ݴ�
	if (graph_mode==1 && width>(ScreenWidth-xx)/8) width=(ScreenWidth-xx)/8;
	else if (graph_mode==4 && width>(ScreenWidth-xx)/2) width=(ScreenWidth-xx)/2;
	else if (graph_mode==8 && width>ScreenWidth-xx) width=ScreenWidth-xx;
	if (height>ScreenHeight-yy) height=ScreenHeight-yy;
	if (width==0 || height==0) return;
	ByteAddr();
	if (RamBits>16) a3&=0xffffff;
	else a3&=0xffff;
	if (graph_mode==1) {
		for (i=0;i<height;i++) {
			for (j=0;j<width;j++) {
				t=0;
				if (BmpData[m1l++]) t|=0x80;
				if (BmpData[m1l++]) t|=0x40;
				if (BmpData[m1l++]) t|=0x20;
				if (BmpData[m1l++]) t|=0x10;
				if (BmpData[m1l++]) t|=0x8;
				if (BmpData[m1l++]) t|=0x4;
				if (BmpData[m1l++]) t|=0x2;
				if (BmpData[m1l++]) t|=0x1;
				lRamWrite(a3++,t);
			}
			m1l+=LCD_WIDTH-width*8;
			if (RamBits>16) a3&=0xffffff;
			else a3&=0xffff;
		}
	} else if (graph_mode==4) {
		for (i=0;i<height;i++) {
			for (j=0;j<width;j++) {
				t=(BmpData[m1l++]&0xf)<<4;
				t|=BmpData[m1l++]&0xf;
				lRamWrite(a3++,t);
			}
			m1l+=LCD_WIDTH-width*2;
			if (RamBits>16) a3&=0xffffff;
			else a3&=0xffff;
		}
	} else {
		for (i=0;i<height;i++) {
			for (j=0;j<width;j++) {
				lRamWrite(a3++,BmpData[m1l++]);
			}
			m1l+=LCD_WIDTH-width;
			if (RamBits>16) a3&=0xffffff;
			else a3&=0xffff;
		}
	}
}

void c_sin()
{
	get_val();
	a1=(a1&0xffff)%360;
	if (a1<90) a1=sin90[a1];
	else if (a1<180) a1=sin90[180-a1];
	else if (a1<270) a1=0-sin90[a1-180];
	else a1=0-sin90[360-a1];
	put_val();
}

void c_cos()
{
	get_val();
	a1=(a1&0xffff)%360;
	if (a1>=270) a1-=270;
	else a1+=90;
	if (a1<90) a1=sin90[a1];
	else if (a1<180) a1=sin90[180-a1];
	else if (a1<270) a1=0-sin90[a1-180];
	else a1=0-sin90[360-a1];
	put_val();
}

void c_fill()
{
	get_val();
	get_val();
	get_val();
}

void c_setgraphmode()
{
	int i,t;
	t=graph_mode;
	get_val();
	a1&=0xff;
	if (a1==1 || a1==4 || a1==8) {
		if (graph_mode!=(word)a1) {
			if (a1==4) {bgcolor=0;fgcolor=15;}
			else if (a1==8) {bgcolor=0;fgcolor=255;}
			if ((word)a1==1) {
				for (i=0;i<ScreenHeight;i++) {
					memset(BmpData+i*LCD_WIDTH,0,ScreenWidth);
				}
			} else {
				for (i=0;i<ScreenHeight;i++) {
					memset(BmpData+i*LCD_WIDTH,bgcolor,ScreenWidth);
				}
			}
			graph_mode=(word)a1;
			SetPalette();
		}
		a1=t;
	} else if (a1==0) a1=t;
	else a1=0;
	put_val();
}

void c_setbgcolor()
{
	get_val();
	if (graph_mode==8) bgcolor=a1&0xff;
	else bgcolor=a1&0xf;
}

void c_setfgcolor()
{
	get_val();
	if (graph_mode==8) fgcolor=a1&0xff;
	else fgcolor=a1&0xf;
}

void c_fade()
{
	int i;
	byte t,fa;
	byte *src,*obj;

	get_val();
	if (graph_mode==1) return;
	fa=(a1&0xf)^0xf;
	src=(byte *)BmpData+SCROLL_CON;
	obj=(byte *)BmpData;
	i=LCD_WIDTH*ScreenHeight;
	while (i) {
		t=*src++;
		*obj++=(t<fa)?fa:t;
		i--;
	}
}

void c_math()
{
	float t;

	get_val();
	switch (a1) {
	case 0: //GetVertion
		a1=0x100;
		break;
	case 7: //sin
		get_val();
		memcpy(&t,&a1,4);
		t=(float)sin(t);
		memcpy(&a1,&t,4);
		break;
	case 8: //cos
		get_val();
		memcpy(&t,&a1,4);
		t=(float)cos(t);
		memcpy(&a1,&t,4);
		break;
	case 9: //tan
		get_val();
		memcpy(&t,&a1,4);
		t=(float)tan(t);
		memcpy(&a1,&t,4);
		break;
	case 10: //asin
		get_val();
		memcpy(&t,&a1,4);
		t=(float)asin(t);
		memcpy(&a1,&t,4);
		break;
	case 11: //acos
		get_val();
		memcpy(&t,&a1,4);
		t=(float)acos(t);
		memcpy(&a1,&t,4);
		break;
	case 12: //atan
		get_val();
		memcpy(&t,&a1,4);
		t=(float)atan(t);
		memcpy(&a1,&t,4);
		break;
	case 13: //sqrt
		get_val();
		memcpy(&t,&a1,4);
		t=(float)sqrt(t);
		memcpy(&a1,&t,4);
		break;
	case 14: //exp
		get_val();
		memcpy(&t,&a1,4);
		t=(float)exp(t);
		memcpy(&a1,&t,4);
		break;
	case 15: //log
		get_val();
		memcpy(&t,&a1,4);
		t=(float)log(t);
		memcpy(&a1,&t,4);
		break;
	case 19: //abs
		get_val();
		memcpy(&t,&a1,4);
		if (t<0) t=-t;
		memcpy(&a1,&t,4);
		break;
	default:
		a1=0;
	}
	put_val();
}

//////////////////////////////

void c_getchar()
{
	while (lav_key<128) {
		lavax_platform_poll();
		if (CheckExitKey() || CheckAppExit()) return;
	}
	a1=lav_key&0x7f;
	lav_key&=0x7f;
	put_val();
}

void c_inkey()
{
	if (lav_key<128) a1=0;
	else {
		a1=lav_key&0x7f;
		lav_key&=0x7f;
	}
	put_val();
}

void c_checkkey()
{
	byte k;
	int i;
	get_val();
	k=(byte)a1;
	a1=LFALSE;
	if (k<128) {
		k=c_keyid(k);
		if (k && (cur_keyb[k])) a1=LTRUE;
	} else {
		for (i=0;i<sizeof(cur_keyb);i++) {
			if (cur_keyb[i]) {
				k=c_keyval((byte)i);
				if (k) {
					a1=k;
					break;
				}
			}
		}
	}
	put_val();
}

void c_releasekey()
{
	byte k,t;
	int i;
	get_val();
	k=(byte)a1;
	t=k;
	if (k<128) {
		k=c_keyid(k);
		if (k) {
			cur_keyb[k]=0;
			if ((t|0x80)==lav_key) lav_key=t;
		}
	} else {
		lav_key=0;
		for (i=0;i<sizeof(cur_keyb);i++) cur_keyb[i]&=0x7f;
	}
}

void c_getword()
{
	get_val();
	c_getchar();
}

void c_delay()
{
	byte t;

	get_val();
	delay=(a1&0x7fff)*256/1000;
	t=Hz128;
	while (delay) {
		lavax_platform_poll();
		if (CheckExitKey() || CheckAppExit()) break;
		if (t!=Hz128) {
			t=Hz128;
			delay--;
		}
	}
}

void c_getms()
{
	a1=Hz128;
	put_val();
}

void c_gettime()
{
	get_val();
	if (RamBits>16) a1&=0xffffff;
	else a1&=0xffff;
	GetLocalTime_os();
	lRamWrite((a32)a1,(byte)s_year);
	lRamWrite((a32)a1+1,(byte)(s_year>>8));
	lRamWrite((a32)a1+2,s_month);
	lRamWrite((a32)a1+3,s_day);
	lRamWrite((a32)a1+4,s_hour);
	lRamWrite((a32)a1+5,s_min);
	lRamWrite((a32)a1+6,s_sec);
	lRamWrite((a32)a1+7,s_week);
}

void c_settime()
{
	get_val();
	if (RamBits>16) a1&=0xffffff;
	else a1&=0xffff;
	s_year=lRamRead((a32)a1)+(lRamRead((a32)a1+1)<<8);
	s_month=lRamRead((a32)a1+2);
	s_day=lRamRead((a32)a1+3);
	s_hour=lRamRead((a32)a1+4);
	s_min=lRamRead((a32)a1+5);
	s_sec=lRamRead((a32)a1+6);
	s_week=lRamRead((a32)a1+7);
	if (lPC) SetLocalTime_os();
}

void c_exec()
{
	byte *lav;
	char *p;
	char *arguments;
	char *arguments_copy;
	long set;
	a32 arguments_addr;
	size_t task_offset;
	size_t available;
	size_t command_length;
	size_t arguments_length;
	const size_t child_ram_minimum=0x10000;

	get_val();
	set=a1;
	get_val2();
	arguments_addr=RamBits>16 ? a3&0xffffff : (word)a3;
	get_exe_name();
	a1=-1;
	if (!FileName[0]) {
		printf("LavaXVM exec rejected: empty or invalid file name\n");
		put_val();
		return;
	}
	if (task_lev >= LAVA_TASK_COUNT - 1) {
		printf("LavaXVM exec rejected: task limit reached for %s\n",FileName);
		put_val();
		return;
	}
	if (task_lev && (task[task_lev].attrib&0x80)) {
		printf("LavaXVM exec rejected: child execution disabled for %s\n",FileName);
		put_val();
		return;
	}
	if (!VRam || !lRam || (uintptr_t)lRam<(uintptr_t)VRam ||
		(uintptr_t)lRam-(uintptr_t)VRam>MY_DATA_SIZE) {
		printf("LavaXVM exec rejected: invalid VM RAM base for %s\n",FileName);
		put_val();
		return;
	}
	task_offset=(size_t)((uintptr_t)lRam-(uintptr_t)VRam);
	available=MY_DATA_SIZE-task_offset;
	if ((size_t)local_sp>available || child_ram_minimum>available-(size_t)local_sp) {
		printf("LavaXVM exec rejected: insufficient VM RAM for %s (offset=0x%08lx available=%lu)\n",
			FileName,(unsigned long)local_sp,(unsigned long)available);
		put_val();
		return;
	}
	arguments=vm_cstring_pointer(arguments_addr,"exec arguments");
	if (!arguments) {
		put_val();
		return;
	}
	command_length=strlen(VirtualFullName)+1;
	arguments_length=strlen(arguments);
	if (command_length+arguments_length+1>child_ram_minimum-CMDLINE) {
		printf("LavaXVM exec rejected: command line too long for %s\n",FileName);
		put_val();
		return;
	}
	arguments_copy=malloc(arguments_length+1);
	if (!arguments_copy) {
		printf("LavaXVM exec rejected: no memory for arguments to %s\n",FileName);
		put_val();
		return;
	}
	memcpy(arguments_copy,arguments,arguments_length+1);

	printf("LavaXVM exec: level=%d RamBits=%d file=%s\n",task_lev,RamBits,FileName);
	lav=TaskOpen(FileName);
	if (!lav) {
		printf("LavaXVM exec failed to open or validate: %s\n",FileName);
		free(arguments_copy);
		put_val();
		return;
	}
	task[task_lev+1].attrib=set;
	task[task_lev].local_bp=local_bp;
	task[task_lev].local_sp=local_sp;
	task[task_lev].eval_top=eval_top;
	task[task_lev].string_ptr=string_ptr;
	task[task_lev].lRam=lRam;
	task[task_lev].lPC=lPC;
	//task[task_lev].curr_file=curr_file;
	//task[task_lev].first_file=first_file;
	//task[task_lev].list_set=list_set;
	task[task_lev].RamBits=RamBits;
	task[task_lev].graph_mode=graph_mode;
	task[task_lev].bgcolor=bgcolor;
	task[task_lev].fgcolor=fgcolor;
	Save_Palette();
	SaveKeyCode();
	SaveDisplayConfig(&task[task_lev]);
	task[task_lev].ScreenWidth=ScreenWidth;
	task[task_lev].ScreenHeight=ScreenHeight;
	task[task_lev].secret=secret;
	strcpy(task[task_lev].CD,CD);
	strcpy(task[task_lev].Root,RootPath);
	strcpy(task[task_lev].ProgramPath,CurrentProgramPath);
	{
		char *cmdline=(char *)lRam+local_sp+CMDLINE;

		memcpy(cmdline,VirtualFullName,command_length-1);
		cmdline[command_length-1]=' ';
		memcpy(cmdline+command_length,arguments_copy,arguments_length+1);
	}
	free(arguments_copy);
	task[task_lev].pLAVA=pLAVA;

	wait_no_key();
	task_lev++;
	lRam+=local_sp;
	pLAVA=lav;
	strcpy(CurrentProgramPath,FileName);
	lavax_platform_set_app_path(CurrentProgramPath);
	SetLavRoot(FileName);
	p=strrchr(FileName,'/');
	if (p) {
		strcpy(p+1,"Config.ini");
	}
	ResetDisplayConfig();
	SetKeyCode(FileName);
	lavReset();
}

void c_getcmdline()
{
	get_val();
	if (RamBits>16) a1&=0xffffff;
	else a1&=0xffff;
	strcpy((char*)lRam+a1,(char*)lRam+CMDLINE);
}

void FlmDecode()
{
	a32 src,obj,end_src;
	int len,type,i;
	byte t,t2;

	get_vals();
	if (RamBits>16) src=a1&0xffffff;
	else src=(word)a1;
	if (RamBits>16) obj=a3&0xffffff;
	else obj=(word)a3;
	len=lRamRead2(src)&0xffff;
	type=(len>>13)&7;
	len&=0x1fff;
	end_src=src+len;
	src+=2;
	if (type==0) lmemcpy(obj,src,len-2,6,7);
	else if (type==1) {
		for (;;) {
			if (src>=end_src) break;
			len=lRamRead(src++);
			if (len<64) {
				len&=0x3f;
				if (!len) len=64;
				for (i=0;i<len;i++) lRamWrite(obj++,0);
			} else if (len<128) {
				len&=0x3f;
				if (!len) len=64;
				for (i=0;i<len;i++) lRamWrite(obj++,0xff);
			} else if (len<192) {
				len&=0x3f;
				if (!len) len=64;
				for (i=0;i<len;i++)
					lRamWrite(obj++,lRamRead(src++));
			} else {
				len&=0x3f;
				if (!len) len=64;
				t=lRamRead(src++);
				for (i=0;i<len;i++)
					lRamWrite(obj++,t);
			}
		}
	} else if (type==2) {
		for (;;) {
			if (src>=end_src) break;
			len=lRamRead(src++);
			if (len<64) {
				len&=0x3f;
				if (!len) len=64;
				for (i=0;i<len;i++) obj++;
			} else if (len<128) {
				len&=0x3f;
				if (!len) len=64;
				for (i=0;i<len;i++) {
					t2=lRamRead(obj)+0xff;
					lRamWrite(obj++,t2);
				}
			} else if (len<192) {
				len&=0x3f;
				if (!len) len=64;
				for (i=0;i<len;i++) {
					t2=lRamRead(obj)+lRamRead(src++);
					lRamWrite(obj++,t2);
				}
			} else {
				len&=0x3f;
				if (!len) len=64;
				t=lRamRead(src++);
				for (i=0;i<len;i++) {
					t2=t+lRamRead(obj);
					lRamWrite(obj++,t2);
				}
			}
		}
	}
}

void PY2GB()
{
	a32 src,obj;

	get_vals();
	if (RamBits>16) src=a1&0xffffff;
	else src=(word)a1;
	if (RamBits>16) obj=a3&0xffffff;
	else obj=(word)a3;
	get_val();
	a1=GetGBCodeByPY(a1,lRam+src,lRam+obj);
}

void c_system()
{
	long system_id;

	get_val();
	system_id=a1;
	switch (system_id) {
	case 0: //GetPID
		//a1=('L'<<24)+1;
		a1=0;
		break;
	case 1: //SetBrightness
		get_val();
		break;
	case 2: //GetBrightness
		a1=0;
		break;
	case 3: //ComOpen;
	case 4: //ComClose;
	case 5: //ComWaitReady
		break;
	case 6: //ComSetTimer
		get_val();
		break;
	case 7: //ComGetc
		break;
	case 8: //ComPutc
		get_val();
		break;
	case 9: //ComRead
	case 10: //ComWrite
	case 11: //ComXor
		get_vals();
		break;
	case 12: //RamRead
		get_val();
		get_vals();
		break;
	case 13: //DiskReclaim
	case 14: //DiskCheck
		break;
	case 15: //FlmDecode
		FlmDecode();
		break;
	case 20: //PY2GB
		PY2GB();
		break;
	case 29: //FindFileEx
		sys_findfile_ex();
		break;
	case 30:
		sys_getfilenum_ex();
		break;
	case 31: //GetTickCount
		a1=TickCount;
		break;
	case 32: //PeekMessage
		get_vals();
		if (RamBits>16) a1&=0xffffff;
		else a1&=0xffff;
		if (RamBits>16 && lRamRead((a32)a1)==1) {
			byte *touch=vm_memory_pointer((a32)a1,sizeof(struct TOUCH),"touch event");
			if (!touch) return;
			GetTouch((struct TOUCH*)touch);
			a1=-1;
		} else {
			a1=0;
		}
		break;
	case 33: //GetFileAttributes
		sys_GetFileAttributes();
		break;
	case 35: //GetBatteryPercent (LavaXOS extension)
	{
		rg_battery_t battery=rg_input_read_battery();
		a1=battery.present ? (long)(battery.level+0.5f) : -1;
		break;
	}
	case 36: //GetBatteryMillivolts (LavaXOS extension)
	{
		rg_battery_t battery=rg_input_read_battery();
		a1=battery.present ? (long)(battery.volts*1000.0f+0.5f) : -1;
		break;
	}
	case 37: //GetBatteryCharging (LavaXOS extension)
	{
		rg_battery_t battery=rg_input_read_battery();
		a1=battery.present ? battery.charging : -1;
		break;
	}
	default:
		a1=0;
	}
	put_val();
}

void c_setpalette()
{
	a32 addr;

	get_val();
	if (RamBits>16) addr=a1&0xffffff;
	else addr=a1&0xffff;
	get_vals();
	a1&=0xff;
	a3&=0x7fff;
	if (a1+a3>256) a3=256-a1;
	{
		byte *palette=vm_memory_pointer(addr,(size_t)a3*4,"palette data");
		if (!palette) return;
		lav_setpalette((byte)a1,a3,palette);
	}
	a1=a3;
	put_val();
}

CODES codes2[]={
	c_putchar,c_getchar,c_printf,c_strcpy,c_strlen,c_setscreen,c_updatelcd,
	c_delay,c_writeblock,scroll_to_lcd,c_textout,c_block,c_rectangle,
	c_exit,c_clearscreen,c_abs,c_rand,c_srand,c_locate,c_inkey,c_point,
	c_getpoint,c_line,c_box,c_circle,c_ellipse,c_beep,c_isalnum,c_isalpha,
	c_iscntrl,c_isdigit,c_isgraph,c_islower,c_isprint,c_ispunct,c_isspace,
	c_isupper,c_isxdigit,c_strcat,c_strchr,c_strcmp,c_strstr,c_tolower,
	c_toupper,c_memset,c_memcpy,c_fopen,c_fclose,c_fread,c_fwrite,
	c_fseek,c_ftell,c_feof,c_rewind,c_getc,c_putc,c_sprintf,c_makedir,
	c_deletefile,c_getms,c_checkkey,c_memmove,c_crc16,c_jiami,c_chdir,
	c_filelist,c_gettime,c_settime,c_getword,c_xdraw,c_releasekey,c_getblock,
	c_sin,c_cos,c_fill,c_setgraphmode,c_setbgcolor,c_setfgcolor,
	c_setlist,c_fade,c_exec,c_findfile,c_getfilenum,c_system,c_math,
	c_setpalette,c_getcmdline
};

void main_loop()
{
	int t;
	CODES code;

	if (!pLAVA)
		return;

	for (;;) {
		lavax_platform_poll();
		if (CheckExitKey())
			return;
		CheckAppExit();
		current_opcode_offset=lPC && pLAVA ? (size_t)(lPC-pLAVA) : 0;
		current_opcode=-1;
		t=getcode();
		if (!lPC) return;
		current_opcode=t;
		if (t&0x80) {
			if ((size_t)(t&0x7f)>=sizeof(codes2)/sizeof(codes2[0])) {
				LavaRuntimeError("LavaXVM system opcode is invalid");
				return;
			}
			code=codes2[t&0x7f];
			(*code)();
		} else {
			if ((size_t)t>=sizeof(codes)/sizeof(codes[0])) {
				LavaRuntimeError("LavaXVM opcode is invalid");
				return;
			}
			code=codes[t];
			(*code)();
		}
		if (!lPC) return;
	}
}
