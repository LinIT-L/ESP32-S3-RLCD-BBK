#include "type.h"
#include "base.h"
#include "ram.h"

#define MY_LOCAL_RAM	0x000000 //240*160

extern byte *lav_fonts;
extern int ScreenWidth,ScreenHeight; //可变的屏幕宽高
extern volatile byte lav_key;
extern byte cur_keyb[128];
extern char CD[I_MAX_PATH];
extern char RootPath[I_MAX_PATH];

extern void get_exe_name();
extern void c_fopen();
extern void c_fclose();
extern void c_closeall();
extern void c_fread();
extern void c_fwrite();
extern void c_fseek();
extern void c_ftell();
extern void c_feof();
extern void c_rewind();
extern void c_getc();
extern void c_putc();
extern void c_makedir();
extern void c_deletefile();
extern void c_chdir();
extern void c_filelist();
extern void c_findfile();
extern void c_getfilenum();
extern void c_setlist();
extern void sys_findfile_ex();
extern void sys_getfilenum_ex();
extern void sys_GetFileAttributes();
