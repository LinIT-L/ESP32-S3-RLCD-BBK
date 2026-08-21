#include <stdio.h>
#include <stdlib.h>
#include <sys/unistd.h>
#include <sys/dir.h>
#include <sys/stat.h>
#include <string.h>
#include <dirent.h>
#include "type.h"
#include "define.h"
#include "../include/lavaxvm.h"
#include "rg_stub.h"

#define MAX_OPEN_FILE 4
#define DIR_FAIL 0
#define DIR_SUC 1
#define LTRUE 0xffffffff
#define LFALSE 0

const char ProgramRoot[]="fat:/lavaXOS/PROGRAM/";
const char ProgramRootSD[]="/sd/lavaXOS/PROGRAM/";
const char ProgramRootVirtual[]="/lavaXOS/PROGRAM/";

FILE *fhandle[MAX_OPEN_FILE];
byte ftask[MAX_OPEN_FILE];

char FileName[I_MAX_PATH];
char CD[I_MAX_PATH]; //���⵱ǰ·��(��β����'/'),RootPath��CD���ӳ�ΪĿ�������ʵ·��
char RootPath[I_MAX_PATH]; //Lvc����ĸ�Ŀ¼(��β��'/')
char HomePath[I_MAX_PATH];
char SharePath[I_MAX_PATH];
char MePath[I_MAX_PATH];
char tbuf[520];

extern int RamBits;
extern byte eval_top;
extern a32 eval_stack;
extern a32 *bp;
extern long a1;

extern void get_val();
extern void put_val();
extern long lRamRead4(a32 addr);

int SetRoot(char *dir)
{
	char tempcd[I_MAX_PATH];
	int len;

	if (dir==NULL || dir[0]==0)
		return 0;
	//if (!(flag&2))
	//	return 0;
	if (strlcpy(tempcd,dir,I_MAX_PATH)>=I_MAX_PATH)
		return 0;

	len=strlen(tempcd);
	if (tempcd[len-1]!='/' &&  tempcd[len-1]!='\\') {
		if (len+1>=I_MAX_PATH)
			return 0;
		tempcd[len]='/';
		tempcd[len+1]=0;
	}
	CD[0]=0;
	strcpy(RootPath,tempcd);
	return 1;
}

void SetLavRoot(char *lav)
{
	char *p;

	if (lav==NULL)
		return;

	if (strstr(lav,ProgramRoot)==lav ||
		strstr(lav,ProgramRootSD)==lav ||
		strstr(lav,ProgramRootVirtual)==lav) {
		//��PROGRAMĿ¼����ɳ�䱣��
		p=strrchr(lav,'/');
		if (p) {
			*p=0;
			if (SetRoot(lav))
				printf("LavaXVM program root: %s\n",RootPath);
			*p='/';
		}
	}
}

static void InitVFS()
{
	memset(fhandle,0,sizeof(fhandle));
	CD[0]=0;
}

void filesys_init()
{
	InitVFS();
	SetRoot("/sdcard/lavaXOS");
}

static void my_cdup(char *dirname)
{
	int i;
	
	i=strlen(dirname)-1;
	if (i>=0 && dirname[i]=='/') i--; //����ĩβ��'/'
	while (i>=0) {
		if (dirname[i]=='/') {
			dirname[i+1]=0;
			return;
		}
		i--;
	}
	dirname[0]=0;
}

static void true_name_0(char *src,char *obj,char *root)
{
	char c;

	strcpy(obj,root);
	if (strlcat(obj,src,I_MAX_PATH)>=I_MAX_PATH) { //��ֹ�������������
		obj[0]=0;
		return;
	}
	for (;;) {
		c=*obj;
		if (c==0) break;
		if (c=='\\') *obj='/';
		obj++;
	}
}

static void true_name(char *src,char *obj)
{
	true_name_0(src,obj,RootPath);
}

/***********************************************
·����ʽ��
����Ի����·��ת��Ϊ�淶�ľ���·��������.��..
�Ƿ�·������1
***********************************************/
static int my_pathformat(char *dirname,char *tempcd)
{
	char c,flag;
	int i,j;
	
	if (dirname[0]=='/') {
		j=0;
		i=1;
	} else {
		strcpy(tempcd,CD);
		j=strlen(tempcd);
		if (j) tempcd[j++]='/';
		i=0;
	}
	flag=1;
	for (;j<I_MAX_PATH;) {
		c=dirname[i++];
		if (c==0) break;
		if (c=='.' && flag) {
			c=dirname[i++];
			if (c==0) break;
			else if (c=='/') {
				;
			} else if (c=='.') {
				c=dirname[i++];
				if (c==0 || c=='/') {
					tempcd[j]=0;
					my_cdup(tempcd);
					j=strlen(tempcd);
					if (c==0) break;
				} else return 1;		
			} else return 1;
		} else if (c=='/') {
			if (flag==0) tempcd[j++]=c; //���Զ����'/'
			flag=1;
		} else if (c==':' || c=='\\') {
			return 1;
		} else {
			tempcd[j++]=c;
			flag=0;
		}
	}
	if (j>=I_MAX_PATH) //��ֹ�������������
		return 1;
	if (j>0 && tempcd[j-1]=='/') j--;
	tempcd[j]=0;
	return 0;
}

int my_chdir(char *dirname)
{
	char tempcd[I_MAX_PATH],truecd[I_MAX_PATH];
	struct stat st;

	if (my_pathformat(dirname,tempcd)) return 1;
	true_name(tempcd,truecd);
	{
		char host_name[I_MAX_PATH];
		const char *host_path=lavax_host_path(truecd,host_name,sizeof(host_name));
		if (!host_path || stat(host_path,&st)) return 1;
	}
	if (st.st_mode&S_IFDIR) {
		strcpy(CD,tempcd);
	} else return 1;	
	return 0;
}

int my_mkdir(char *dirname)
{
	char tempcd[I_MAX_PATH],truecd[I_MAX_PATH];
	char host_name[I_MAX_PATH];
	const char *host_path;

	if (my_pathformat(dirname,tempcd)) return 1;
	true_name(tempcd,truecd);
	host_path=lavax_host_path(truecd,host_name,sizeof(host_name));
	if (!host_path || mkdir(host_path,0)) return 1;
	return 0;
}

static char *get_open_mode()
{
	a32 str;

	get_val();
	if (RamBits>16) str=a1&0xffffff;
	else str=(word)a1;
	return (char*)lRam+str;
}

static void get_name()
{
	a32 str;
	char tempcd[I_MAX_PATH];

	get_val();
	if (RamBits>16) str=a1&0xffffff;
	else str=(word)a1;
	if (my_pathformat((char*)lRam+str,tempcd))
		FileName[0]=0;
	else
		true_name(tempcd,FileName);
}

void get_exe_name()
{
	get_name();
}

void c_fopen()
{
	FILE *fp;
	char *mode;
	int i;
	byte this_file;

	mode=get_open_mode();
	get_name();
	for (i=0;i<MAX_OPEN_FILE;i++) if (fhandle[i]==0) break;
	if (i==MAX_OPEN_FILE) {
		a1=0;
		put_val();
		return; //����ͬʱ���ļ�������
	}
	this_file=i;
	if (!FileName[0]) fp=NULL; //�ļ���Ϊ��
	else {
		char host_name[I_MAX_PATH];
		const char *host_path = lavax_host_path(FileName, host_name, sizeof(host_name));
		fp=host_path ? fopen(host_path,mode) : NULL;
	}
	if (fp==NULL) {
		a1=0;
		put_val();
		return; //�ļ��޷���
	}
	fhandle[this_file]=fp;
	ftask[this_file]=task_lev;
	a1=this_file|0x80;
	put_val();
}

static int check_handle()
{
	byte t;
	get_val();
	t=(byte)a1;
	if (t>=0x80 && t<0x80+MAX_OPEN_FILE) {
		t&=0x7f;
		if (fhandle[t]) {
			return t;
		}
	}
	return -1;
}

static FILE *get_handle()
{
	int t;
	t=check_handle();
	if (t>=0) {
		return fhandle[t];
	}
	return NULL;
}

void c_fclose()
{
	int t;

	t=check_handle();
	if (t>=0) {
		fclose(fhandle[t]);
		fhandle[t]=NULL;
	}
}

void c_closeall()
{
	int i;

	for (i=0;i<MAX_OPEN_FILE;i++) {
		if (fhandle[i]) {
			if (ftask[i]==task_lev) { //ֻ�رյ�ǰ����򿪵��ļ�
				fclose(fhandle[i]);
				fhandle[i]=NULL;
			}
		}
	}
}

void c_fread()
{
	FILE *fp;
	a32 len,str;

	fp=get_handle();
	if (!fp) {
		eval_top-=12;
		a1=0;
		put_val();
		return;
	}
	get_val();
	if (RamBits>16) len=a1&0xffffff;
	else len=(word)a1;
	get_val();
	get_val();
	if (RamBits>16) str=a1&0xffffff;
	else str=(word)a1;
	a1=fread(lRam+str,1,len,fp);
	put_val();
}

void c_fwrite()
{
	FILE *fp;
	a32 len,str;

	fp=get_handle();
	if (!fp) {
		eval_top-=12;
		a1=0;
		put_val();
		return;
	}
	get_val();
	if (RamBits>16) len=a1&0xffffff;
	else len=(word)a1;
	get_val();
	get_val();
	if (RamBits>16) str=a1&0xffffff;
	else str=(word)a1;
	a1=fwrite(lRam+str,1,len,fp);
	put_val();
}

void c_fseek()
{
	FILE *fp;
	byte mode;
	long p;

	get_val();
	mode=(byte)a1;
	get_val();
	p=a1;
	fp=get_handle();
	if (!fp || mode>2) {
		a1=-1;
		put_val();
		return;
	}
	a1=fseek(fp,p,mode);
	put_val();
}

void c_ftell()
{
	FILE *fp;
	fp=get_handle();
	if (!fp) {
		a1=-1;
		put_val();
		return;
	}
	a1=ftell(fp);
	put_val();
}

void c_feof()
{
	FILE *fp;
	fp=get_handle();
	if (!fp) {
		a1=-1;
		put_val();
		return;
	}
	a1=feof(fp);
	put_val();
}

void c_rewind()
{
	FILE *fp;
	fp=get_handle();
	if (fp) {
		rewind(fp);
	}
}

void c_getc()
{
	FILE *fp;
	fp=get_handle();
	if (!fp) {
		a1=-1;
		put_val();
		return;
	}
	a1=getc(fp);
	put_val();
}

void c_putc()
{
	FILE *fp;
	byte c;
	fp=get_handle();
	if (!fp) {
		get_val();
		a1=-1;
		put_val();
		return;
	}
	get_val();
	c=(byte)a1;
	a1=putc(c,fp);
	put_val();
}

void c_makedir()
{
	a32 str;

	get_val();
	if (RamBits>16) str=a1&0xffffff;
	else str=(word)a1;
	if (my_mkdir((char*)lRam+str)) {
		a1=LFALSE;
	} else {
		a1=LTRUE;
	}
	put_val();
}

void c_deletefile()
{
	get_name();
	// Σ�պ������Ƿ�ʵ��?
	char host_name[I_MAX_PATH];
	const char *host_path = lavax_host_path(FileName, host_name, sizeof(host_name));
	if (host_path && remove(host_path)==0) {
		a1=LTRUE;
	} else {
		a1=LFALSE;
	}
	put_val();
}

void c_chdir()
{
	a32 str;

	get_val();
	if (RamBits>16) str=a1&0xffffff;
	else str=(word)a1;
	if (my_chdir((char*)lRam+str)) {
		a1=LFALSE;
	} else {
		a1=LTRUE;
	}
	put_val();
}

void c_filelist()
{
	;
}

static size_t utf8_to_lava_name(const char *source,char *target,size_t size)
{
	const char *p=source;
	size_t length=0;

	if (!target || size==0) return 0;
	while (*p && length+1<size) {
		const char *next=p;
		int unicode=rg_utf8_decode(&next);
		unsigned int oem;

		if (unicode<0) {
			target[length++]='?';
			p++;
			continue;
		}
		if (unicode<0x80) {
			target[length++]=(char)unicode;
			p=next;
			continue;
		}
		oem=ff_uni2oem((unsigned int)unicode,936);
		if (oem>0xff) {
			if (length+2>=size) break;
			target[length++]=(char)(oem>>8);
			target[length++]=(char)oem;
		} else {
			target[length++]='?';
		}
		p=next;
	}
	target[length]=0;
	return length;
}

static int findfile(char *dir,int from,int num,char *buf)
{
	DIR *dp;
	struct dirent *pent;
	int i,end;
	char host_name[I_MAX_PATH];
	const char *host_path=lavax_host_path(dir,host_name,sizeof(host_name));

	dp=host_path ? opendir(host_path) : NULL;
	if (!dp) {
		return -1;
	}
	i=1;
	if (from==0) {
		strcpy(buf,"..");
		buf+=16;
	}
	end=from+num;
	while (i<end) {
		if ((pent=readdir(dp))==NULL) {
			break;
		}
		if (strcmp(pent->d_name,".")==0) ;
		else if (strcmp(pent->d_name,"..")==0) ;
		else {
			char lava_name[16];
			size_t name_length=utf8_to_lava_name(pent->d_name,lava_name,sizeof(lava_name));

			if (name_length==0 || name_length>14) ;
			else {
				if (i>=from) {
					memset(buf,0,16);
					memcpy(buf,lava_name,name_length);
					buf+=16;
				}
				i++;
			}
		}
	}
	closedir(dp);
	return i-from;
}

void c_findfile()
{
	word index,num;
	a32 namebuf;
	int found;

	get_val();
	if (RamBits==16) namebuf=(word)a1;
	else namebuf=a1&0xffffff;
	get_val();
	num=(word)a1;
	get_val();
	index=(word)a1;
	true_name(CD,FileName);
	found=findfile(FileName,index,num,(char*)lRam+namebuf);
	a1=found;
	put_val();
}

static int getfilenum(char *dir)
{
	DIR *dp;
	struct dirent *pent;
	int total;
	char host_name[I_MAX_PATH];
	const char *host_path=lavax_host_path(dir,host_name,sizeof(host_name));

	dp=host_path ? opendir(host_path) : NULL;
	if (!dp) {
		return -1;
	}
	total=0;
	for (;;) {
		if ((pent=readdir(dp))==NULL) {
			break;
		}
		if (strcmp(pent->d_name,".")==0) ;
		else if (strcmp(pent->d_name,"..")==0) ;
		else {
			char lava_name[16];
			size_t name_length=utf8_to_lava_name(pent->d_name,lava_name,sizeof(lava_name));
			if (name_length>0 && name_length<=14) total++;
		}
	}
	closedir(dp);
	return total;
}

void c_getfilenum()
{
	get_name();
	if (FileName[0]==0) a1=-1;
	else {
		a1=getfilenum(FileName);
	}
	put_val();
}

void c_setlist()
{
}

void sys_findfile_ex()
{
}

void sys_getfilenum_ex()
{
}

void sys_GetFileAttributes()
{
}

void workdir_init()
{
}

#define MAX_CONFIG_SIZE 4096

static int is_space(char c)
{
	return c==' ' || c=='\t' || c=='\r';
}

static int is_stringchar(char c)
{
	return (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='\'';
}

static char *skip_space(char *p)
{
	while (is_space(*p)) {
		p++;
	}
	return p;
}

static char *get_string(char *p,char *buf,int blen)
{
	int i=0;
	while (is_stringchar(*p)) {
		if (i<blen-1) {
			buf[i++]=*p;
		}
		p++;
	}
	buf[i]=0;
	return p;
}

static char *to_next_line(char *p)
{
	while (*p && *p!='\n') {
		p++;
	}
	while (*p=='\n') {
		p++;
	}
	return p;
}

static void set_config(char *name,char *val)
{
	if (strcmp(name,"ZOOM_SCREEN")==0) {
		SetZoom(strcmp(val,"1")==0 ? 1 : 0);
		return;
	}
	if (SetDisplayConfig(name,val))
		return;
	ConfigKey(name,val);
}

void ReadConfig(char *fname)
{
	FILE *fp;
	int len;
	char *buf;
	char name[64];
	char val[260];
	char *p;
	char host_name[I_MAX_PATH];
	const char *host_path=lavax_host_path(fname,host_name,sizeof(host_name));

	fp=host_path ? fopen(host_path,"rb") : NULL;
	if (!fp) {
		return;
	}
	buf=malloc(MAX_CONFIG_SIZE);
	if (!buf) {
		fclose(fp);
		return;
	}
	len=fread(buf,1,MAX_CONFIG_SIZE-1,fp);
	fclose(fp);
	buf[len]=0;

	p=buf;
	for (;;) {
		if (!p[0]) {
			break;
		}
		p=skip_space(p);
		p=get_string(p,name,sizeof(name));
		if (name[0]) {
			p=skip_space(p);
			if (*p=='=') {
				p=skip_space(p+1);
				p=get_string(p,val,sizeof(val));
				if (val[0]) {
					set_config(name,val);
				}
			}
		}
		p=to_next_line(p);
	}
	free(buf);
}
