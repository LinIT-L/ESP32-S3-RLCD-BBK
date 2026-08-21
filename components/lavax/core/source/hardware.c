#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "type.h"
#include "base.h"
#include "../include/lavaxvm.h"
#include "rg_stub.h"

volatile byte lav_key;
byte cur_keyb[128];
static byte old_keyb[128];
static byte key_code[] = {0, 0x14, 0x15, 0x17, 0x16, 0x0d, 0x1b, 0x1c, 0x1d, 0x1f, 0x19, 0x13, 0x0e};
const byte default_key_code[] = {0, 0x14, 0x15, 0x17, 0x16, 0x0d, 0x1b, 0x1c, 0x1d, 0x1f, 0x19, 0x13, 0x0e};
volatile byte Hz128;
long TickCount;
int s_year, s_month, s_day, s_hour, s_min, s_sec, s_week;

static const uint32_t input_masks[] = {
    0, RG_KEY_UP, RG_KEY_DOWN, RG_KEY_LEFT, RG_KEY_RIGHT, RG_KEY_A,
    RG_KEY_B, RG_KEY_SELECT, RG_KEY_START, RG_KEY_L, RG_KEY_R, RG_KEY_X, RG_KEY_Y
};

static const char *key_name[] = {
    "", "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "SELECT", "START", "L1", "R1", "X", "Y"
};

struct lava_key_code
{
    const char *name;
    byte code;
};

static const struct lava_key_code lava_key_codes[] = {
    {"F1", 0x1c}, {"F2", 0x1d}, {"F3", 0x1e}, {"F4", 0x1f},
    {"HELP", 0x19}, {"SHIFT", 0x1a}, {"CAPS", 0x12}, {"ESC", 0x1b}, {"ENTER", 0x0d},
    {"UP", 0x14}, {"DOWN", 0x15}, {"LEFT", 0x17}, {"RIGHT", 0x16},
    {"PAGEUP", 0x13}, {"PAGEDOWN", 0x0e}, {"SPACE", 0x20}, {"POINT", 0x2e}
};

void GetTouch(struct TOUCH *touch) { memset(touch, 0, sizeof(*touch)); }
void SaveKeyCode(void) { memcpy(task[task_lev].key_code, key_code, sizeof(default_key_code)); }
void LoadKeyCode(void) { memcpy(key_code, task[task_lev].key_code, sizeof(default_key_code)); }

void SetKeyCode(char *cfg)
{
    memcpy(key_code, default_key_code, sizeof(default_key_code));
    ReadConfig(cfg);
}

int ConfigKey(char *name, char *value)
{
    int i;
    int id = 0;
    int code = 0;

    for (i = 1; i < (int)(sizeof(key_name) / sizeof(key_name[0])); i++)
    {
        if (strcmp(name, key_name[i]) == 0)
        {
            id = i;
            break;
        }
    }
    if (!id)
        return 0;

    for (i = 0; i < (int)(sizeof(lava_key_codes) / sizeof(lava_key_codes[0])); i++)
    {
        if (strcmp(value, lava_key_codes[i].name) == 0)
        {
            code = lava_key_codes[i].code;
            break;
        }
    }
    if (!code)
    {
        size_t length = strlen(value);
        if (length == 1)
        {
            if (value[0] >= 'A' && value[0] <= 'Z')
                code = value[0] + ('a' - 'A');
            else if (value[0] >= '0' && value[0] <= '9')
                code = value[0];
        }
        else if (length >= 2)
        {
            code = atoi(value[0] == '0' ? value + 1 : value);
        }
    }
    if (code <= 0 || code >= 128)
        return 0;

    key_code[id] = (byte)code;
    return 1;
}

extern char CurrentProgramPath[I_MAX_PATH];

static int running_shell(void)
{
    const char *name = strrchr(CurrentProgramPath, '/');

    name = name ? name + 1 : CurrentProgramPath;
    return strcmp(name, "Shell.sys") == 0;
}

static byte active_key_code(int id)
{
    if (!running_shell())
    {
        if (id == 11)
            return key_code[9];
        if (id == 12)
            return key_code[10];
    }
    return key_code[id];
}

int c_keyid(byte key)
{
    int i;
    for (i = 1; i < (int)sizeof(key_code); i++) if (active_key_code(i) == key) return i;
    return 0;
}
byte c_keyval(byte key) { return key < sizeof(key_code) ? active_key_code(key) : 0; }

void GetKeyboardState(byte *keys)
{
    uint32_t state = rg_input_read_gamepad();
    int i;
    memcpy(old_keyb, keys, sizeof(old_keyb));
    memset(keys, 0, sizeof(cur_keyb));
    for (i = 1; i < (int)(sizeof(input_masks) / sizeof(input_masks[0])); i++)
        keys[i] = (state & input_masks[i]) != 0;
    for (i = 1; i < (int)sizeof(key_code); i++)
    {
        byte code = active_key_code(i);

        if (keys[i] && !old_keyb[i] && lav_key < 128)
            lav_key = code | 0x80;
    }
}

void LavaHardwarePollInput(void)
{
    GetKeyboardState(cur_keyb);
}

int CheckExitKey(void)
{
    return lavax_platform_should_exit();
}

void InitNds(void)
{
    TickCount = 0;
    Hz128 = 0;
    memset(cur_keyb, 0, sizeof(cur_keyb));
    memset(old_keyb, 0, sizeof(old_keyb));
}

void LavaHardwareTick(unsigned int ticks)
{
    while (ticks--)
    {
        TickCount++;
        Hz128++;
    }
}

void GetLocalTime_os(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    s_year = tm_now.tm_year + 1900; s_month = tm_now.tm_mon + 1; s_day = tm_now.tm_mday;
    s_hour = tm_now.tm_hour; s_min = tm_now.tm_min; s_sec = tm_now.tm_sec; s_week = tm_now.tm_wday;
}
void SetLocalTime_os(void)
{
    struct tm tm_local = {0};
    time_t value;

    if (s_year < 1970 || s_year > 2099 || s_month < 1 || s_month > 12 ||
        s_day < 1 || s_day > 31 || s_hour < 0 || s_hour > 23 ||
        s_min < 0 || s_min > 59 || s_sec < 0 || s_sec > 59)
        return;
    tm_local.tm_year = s_year - 1900;
    tm_local.tm_mon = s_month - 1;
    tm_local.tm_mday = s_day;
    tm_local.tm_hour = s_hour;
    tm_local.tm_min = s_min;
    tm_local.tm_sec = s_sec;
    tm_local.tm_isdst = -1;
    value = mktime(&tm_local);
    if (value == (time_t)-1)
        return;
    rg_system_set_time(value);
}
