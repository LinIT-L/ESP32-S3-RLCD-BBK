#include <stddef.h>

typedef unsigned short word;
typedef unsigned long a32;

#include "comm_set.h"

//#ifdef USE_TWO_LCD
#define dprintf iprintf
//#else
//#define dprintf G16DPrintf
//#endif

#define VKEY_START	64

#define LCD_WIDTH	256
#define LCD_HEIGHT	192
#define SCREEN_PLANE_SIZE	(LCD_WIDTH * LCD_HEIGHT)
#define SCREEN_BUFFER_SIZE	(SCREEN_PLANE_SIZE * 2)
#define SCROLL_CON	SCREEN_PLANE_SIZE

#define MY_DATA_SIZE    0x100000 //1MB
#define LAVA_TASK_COUNT 8

#define I_MAX_PATH 256

struct TOUCH
{
	byte cmd,pushed;
	word rawX;
	word rawY;
	word x;
	word y;
};

struct TASK
{
	int		attrib;
	char	CD[I_MAX_PATH];
	char	Root[I_MAX_PATH];
	char	ProgramPath[I_MAX_PATH];
	int		first_file;
	int		curr_file;
	int		list_set;
	int		RamBits;
	word	graph_mode;
	word	bgcolor;
	word	fgcolor;
	byte	palette[256*3];
	byte	key_code[16];
	int		ScreenWidth;
	int		ScreenHeight;
	int		display_source_width;
	int		display_source_height;
	int		display_canvas_width;
	int		display_canvas_height;
	int		display_scale;
	int		display_mask_enabled;
	byte	secret;
	byte	*pLAVA;
	byte	*lRam;
	byte	*lPC;
	a32		local_sp;
	a32		local_bp;
	byte	eval_top;
	a32		string_ptr;
};

extern struct TASK task[]; //任务栈
extern int task_lev; //任务级

extern byte *BmpData;
extern byte *lRam;
extern byte *VRam;
extern byte *pLAVA;
extern char FileName[I_MAX_PATH];

extern volatile byte Hz128;
extern long TickCount;
extern int s_year,s_month,s_day,s_hour,s_min,s_sec,s_week;

extern void InitNds();
extern byte *TaskOpen(char *fname);
extern size_t TaskSize(byte *program);
extern void TaskClose(byte *program);
extern void LavaRuntimeError(const char *message);
extern a32 shell_init();
extern void GetTouch(struct TOUCH *touch);
extern void SaveKeyCode();
extern void LoadKeyCode();
extern void SetKeyCode(char *cfg);
extern int ConfigKey(char *name,char *val);
extern void ReadConfig(char *fname);
extern void SetZoom(byte v);
extern int SetDisplayConfig(char *name,char *val);
extern void ResetDisplayConfig(void);
extern void SaveDisplayConfig(struct TASK *target);
extern void LoadDisplayConfig(const struct TASK *source);
extern int GetDisplaySourceWidth(int header_width);
extern int GetDisplaySourceHeight(int header_height);
extern void SetLavRoot(char *lav);
extern void GetLocalTime_os();
extern void SetLocalTime_os();
extern void filesys_init();
extern int c_keyid(byte k);
extern byte c_keyval(byte k);
extern int CheckExitKey();
extern int CheckAppExit();
extern void Color256Init();
extern void ClearNdsScreen();
extern void DmaRefresh();
extern void SetWindow();
extern void SetPalette();
extern void lav_setpalette(byte from,int num,byte *addr);
extern void Save_Palette();
extern void Load_Palette();

#include "gmode16.h"
