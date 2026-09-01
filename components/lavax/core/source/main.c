#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "type.h"
#include "base.h"
#include "lavaxvm.h"
#include "font_part.h"

extern byte *ram_base;
extern byte *VRam;
extern byte *lRam;
extern byte *pLAVA;
byte *lav_fonts;
extern void main_loop(void);
extern void InitNds(void);
extern void Color256Init(void);
extern void ReadConfig(char *fname);
extern void filesys_init(void);
extern int SetRoot(char *dir);
extern void lavReset(void);
extern byte *lPC;
extern byte cur_keyb[128];
extern volatile byte lav_key;
extern byte eval_top;
extern a32 string_ptr, local_bp, local_sp;
extern int func_top;
extern int cur_funcid;
extern void c_closeall(void);
extern char CurrentProgramPath[I_MAX_PATH];

struct lava_program
{
    size_t size;
    byte data[];
};

static struct lava_program *program_header(byte *program)
{
    return program ? (struct lava_program *)((byte *)program - offsetof(struct lava_program, data)) : NULL;
}

size_t TaskSize(byte *program)
{
    struct lava_program *header = program_header(program);
    return header ? header->size : 0;
}

void TaskClose(byte *program)
{
    free(program_header(program));
}

byte *TaskOpen(char *fname)
{
    FILE *fp;
    long size;
    byte header[16];
    struct lava_program *allocation;
    byte *program;
    char path[256];
    const char *host_path = lavax_host_path(fname, path, sizeof(path));

    if (!host_path || !(fp = fopen(host_path, "rb")))
        return NULL;
    if (fread(header, 1, sizeof(header), fp) != sizeof(header) ||
        header[0] != 'L' || header[1] != 'A' || header[2] != 'V' || header[3] != 18)
    {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 16 ||
        fseek(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        return NULL;
    }
    allocation = malloc(sizeof(*allocation) + (size_t)size + 1);
    if (!allocation)
    {
        fclose(fp);
        return NULL;
    }
    allocation->size = (size_t)size;
    program = allocation->data;
    if (fread(program, 1, (size_t)size, fp) != (size_t)size)
    {
        free(allocation);
        fclose(fp);
        return NULL;
    }
    program[size] = 0;
    fclose(fp);
    return program;
}

static char last_error[160];
static void *vm_memory;
static void *local_memory;
/* V1.0.9x 黑屏修复: 记录实际分配的 VM RAM 大小 (原版 VRam[0x1000000]=16MB,
 * 固件按 PSRAM 实际分配, 检查器按实际大小放行而非硬编码 MY_DATA_SIZE). */
size_t g_vm_ram_size = 0;

static void set_error(const char *message)
{
    snprintf(last_error, sizeof(last_error), "%s", message ? message : "unknown error");
}

void LavaRuntimeError(const char *message)
{
    set_error(message);
}

const char *lavaxvm_last_error(void)
{
    return last_error[0] ? last_error : NULL;
}

size_t lavaxvm_vm_ram_size(void)
{
    return MY_DATA_SIZE;
}

size_t lavaxvm_local_ram_size(void)
{
    return (size_t)LCD_WIDTH * LCD_HEIGHT;
}

bool lavaxvm_init(void *vm_ram, size_t vm_ram_size, void *local_ram, size_t local_ram_size)
{
    if (!vm_ram || vm_ram_size < MY_DATA_SIZE || !local_ram || local_ram_size < LCD_WIDTH * LCD_HEIGHT)
    {
        set_error("LavaXVM memory allocation failed");
        return false;
    }

    last_error[0] = 0;
    task_lev = 0;
    memset(task, 0, sizeof(task[0]) * LAVA_TASK_COUNT);
    memset(cur_keyb, 0, sizeof(cur_keyb));
    lPC = NULL;
    eval_top = 0;
    string_ptr = 0;
    local_bp = 0;
    local_sp = 0;
    func_top = 0;
    cur_funcid = 0;
    lav_key = 0;
    vm_memory = vm_ram;
    local_memory = local_ram;
    g_vm_ram_size = vm_ram_size;
    ram_base = (byte *)local_memory;
    VRam = (byte *)vm_memory;
    lRam = VRam;
    pLAVA = NULL;
    lav_fonts = (byte *)font_part_map(FONT_SLOT_LAV_OFF, FONT_SLOT_LAV_LEN);
    printf("LavaXVM: InitNds\n");
    InitNds();
    printf("LavaXVM: Color256Init\n");
    Color256Init();
    TickCount = 0;
    Hz128 = 0;
    printf("LavaXVM: filesys_init\n");
    filesys_init();
    printf("LavaXVM: ReadConfig\n");
    ReadConfig("fat:/lava/System/Config.ini");
    printf("LavaXVM: initialization complete\n");
    return true;
}

bool lavaxvm_load_shell(void)
{
    pLAVA = TaskOpen("fat:/lava/System/Shell.sys");
    if (!pLAVA)
    {
        set_error("lava Shell.sys is missing or invalid");
        return false;
    }
    if (!SetRoot("/sd/lava"))
    {
        TaskClose(pLAVA);
        pLAVA = NULL;
        set_error("Unable to set lava system root");
        return false;
    }
    task_lev = 0;
    snprintf(CurrentProgramPath, sizeof(CurrentProgramPath), "%s", "fat:/lava/System/Shell.sys");
    lavax_platform_set_app_path(CurrentProgramPath);
    lavReset();
    return true;
}

void lavaxvm_run(void)
{
    if (pLAVA)
        main_loop();
}

void lavaxvm_shutdown(void)
{
    c_closeall();
    while (task_lev > 0)
    {
        TaskClose(pLAVA);
        pLAVA = task[--task_lev].pLAVA;
        c_closeall();
    }
    TaskClose(pLAVA);
    memset(task, 0, sizeof(task[0]) * LAVA_TASK_COUNT);
    memset(cur_keyb, 0, sizeof(cur_keyb));
    pLAVA = NULL;
    lPC = NULL;
    lRam = NULL;
    VRam = NULL;
    ram_base = NULL;
    eval_top = 0;
    string_ptr = 0;
    local_bp = 0;
    local_sp = 0;
    func_top = 0;
    cur_funcid = 0;
    lav_key = 0;
    CurrentProgramPath[0] = 0;
    lavax_platform_set_app_path(NULL);
    vm_memory = NULL;
    local_memory = NULL;
}
