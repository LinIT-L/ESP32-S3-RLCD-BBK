/*
 * e0c6200_cpu.c — E0C6200 4-bit CPU core (C port).
 *
 * 1:1 faithful translation of
 *   third_party/vpet-emu-zepp/utils/cpu.js
 * The JS module-level `let` variables become statics here; dynamic
 * allocations (RAM/VRAM) become fixed static buffers inside the module so
 * nothing needs to be freed externally. The ROM is supplied by the caller
 * as an array of big-endian 16-bit opcode words (uint16_t *rom).
 *
 * The JS `sound` module is replaced by an internal sound state machine that
 * drives the `vpet_tone_cb` call-back (see header). All registers, I/O
 * side effects (incl. timer/stopwatch/ptimer/watchdog-style control,
 * interrupts, clock) and every opcode execution path are preserved exactly.
 */

#include "e0c6200_cpu.h"

#include <string.h>
#include "esp_attr.h"

/* ------------------------------------------------------------------ */
/* Clocks                                                             */
/* ------------------------------------------------------------------ */
enum {
    OSC1_CLOCK            = 32768,
    TIMER_CLOCK_DIV       = OSC1_CLOCK / 256,   /* 128 */
};
/* STOPWATCH_CLOCK_DIV = OSC1_CLOCK / 100 = 327.68 (fractional, JS float) */
#define STOPWATCH_CLOCK_DIV ((double)OSC1_CLOCK / 100)

/* PTIMER_CLOCK_DIV[0..7] from cpu.js. index 0/1 unused. */
static const double PTIMER_CLOCK_DIV[8] = {
    0,
    0,
    (double)OSC1_CLOCK / 256,  /* 128 */
    (double)OSC1_CLOCK / 512,  /*  64 */
    (double)OSC1_CLOCK / 1024, /*  32 */
    (double)OSC1_CLOCK / 2048, /*  16 */
    (double)OSC1_CLOCK / 4096, /*   8 */
    (double)OSC1_CLOCK / 8192, /*   4 */
};

/* ------------------------------------------------------------------ */
/* Memory-map constants                                               */
/* ------------------------------------------------------------------ */
enum {
    RAM_SIZE           = 0x300,
    VRAM_SIZE          = 0x0a0,
    VRAM_PART_SIZE     = 0x050,
    VRAM_PART1_OFFSET  = 0xe00,
    VRAM_PART2_OFFSET  = 0xe80,
    VRAM_PART1_END     = VRAM_PART1_OFFSET + VRAM_PART_SIZE,
    VRAM_PART2_END     = VRAM_PART2_OFFSET + VRAM_PART_SIZE,
    IORAM_OFFSET       = 0xf00,
    IORAM_SIZE         = 0x07f,  /* entries 0x00..0x7e */
    IORAM_END          = IORAM_OFFSET + IORAM_SIZE,
};

/* Empty / full VRAM fallbacks (returned when LCD is off / forced on). */
static uint8_t _EMPTY_VRAM[VRAM_SIZE];
static uint8_t _FULL_VRAM[VRAM_SIZE];
static int _buffers_initialised = 0;

/* ------------------------------------------------------------------ */
/* IO register bit masks (cpu.js)                                     */
/* ------------------------------------------------------------------ */
#define IO_IT1   8
#define IO_IT2   4
#define IO_IT8   2
#define IO_IT32  1
#define IO_ISW0  2
#define IO_ISW1  1
#define IO_IPT   1
#define IO_IK0   1
#define IO_IK1   1
#define IO_TM2   4
#define IO_TM7   8
#define IO_TM6   4
#define IO_TM4   1
#define IO_R33   8
#define IO_R43   8
#define IO_CLKCHG 8
#define IO_ALOFF 8
#define IO_ALON  4
#define IO_SVDDT 8
#define IO_SHOTPW 8
#define IO_BZFQ  7
#define IO_BZSHOT 8
#define IO_ENVRST 4
#define IO_ENVRT 2
#define IO_ENVON 1
#define IO_TMRST 2
#define IO_SWRST 2
#define IO_SWRUN 1
#define IO_PTRST 2
#define IO_PTRUN 1
#define IO_PTCOUT 8
#define IO_PTC   7
#define IO_IOC3  8
#define IO_IOC2  4
#define IO_IOC1  2
#define IO_IOC0  1
#define IO_PUP3  8
#define IO_PUP2  4
#define IO_PUP1  2
#define IO_PUP0  1

/* ------------------------------------------------------------------ */
/* Sound state + call-back (replaces utils/sound.js)                  */
/* ------------------------------------------------------------------ */
#define SOUND_CLOCK_DIV 128
/* BUZZER_FREQ_DIV[0..7] */
static const int BUZZER_FREQ_DIV[8] = { 8, 10, 12, 14, 16, 20, 24, 28 };
/* ONE_SHOT_PULSE_WIDTH_DIV = {8*128, 16*128} */
static const int ONE_SHOT_PULSE_WIDTH_DIV[2] = { 8 * SOUND_CLOCK_DIV, 16 * SOUND_CLOCK_DIV };
/* ENVELOPE_CYCLE_DIV = {16*128, 32*128} */
static const int ENVELOPE_CYCLE_DIV[2] = { 16 * SOUND_CLOCK_DIV, 32 * SOUND_CLOCK_DIV };

static vpet_tone_cb _tone_cb = NULL;
static void *_tone_ctx = NULL;

static int _buzzer_freq;
static int _one_shot_counter;
static int _envelope_step;
static int _envelope_cycle;
static int _envelope_counter;
static int _envelope_on;
static int _sound_on;
static int _cycle_counter;

static void tone_play(void) {
    if (_tone_cb) _tone_cb(_tone_ctx, _buzzer_freq, 1);
}
static void tone_stop(void) {
    if (_tone_cb) _tone_cb(_tone_ctx, _buzzer_freq, 0);
}

static void sound_init(uint32_t clock) {
    (void)clock;
    _one_shot_counter = 0;
    _buzzer_freq = (int)(OSC1_CLOCK / BUZZER_FREQ_DIV[0]);
    _envelope_step = 0;
    _envelope_cycle = ENVELOPE_CYCLE_DIV[0];
    _envelope_counter = 0;
    _envelope_on = 0;
    _sound_on = 0;
    _cycle_counter = 0;
}

/* Advance n OSC1 ticks through the sound engine. n is small (< every
 * divisor) so each active counter fires at most once. */
static void sound_clockBatch(int n) {
    _cycle_counter += n;
    if (_one_shot_counter > 0) {
        _one_shot_counter -= n;
        if (_one_shot_counter <= 0) {
            tone_stop();
        }
    }
    if (_envelope_counter > 0) {
        _envelope_counter -= n;
        if (_envelope_counter <= 0) {
            _envelope_step -= 1;
            tone_play();
            _envelope_counter = _envelope_cycle;
        }
    }
}

static void sound_set_freq(int value) {
    _buzzer_freq = (int)(OSC1_CLOCK / BUZZER_FREQ_DIV[value]);
    if (_sound_on) {
        tone_play();
    }
}

static void sound_set_envelope_on(void) {
    _envelope_on = 1;
    _envelope_step = 7;
}

static void sound_set_envelope_off(void) {
    _envelope_on = 0;
    _envelope_step = 0;
    _envelope_counter = 0;
    tone_stop();
}

static void sound_set_envelope_cycle(int cycle) {
    _envelope_cycle = ENVELOPE_CYCLE_DIV[cycle];
}

static void sound_reset_envelope(void) {
    _envelope_step = 7;
}

static void sound_one_shot(int duration) {
    if (_one_shot_counter == 0) {
        _one_shot_counter = ONE_SHOT_PULSE_WIDTH_DIV[duration];
        if (!_sound_on) {
            tone_play();
        }
    }
}

static void sound_set_buzzer_on(void) {
    _sound_on = 1;
    _one_shot_counter = 0;
    tone_play();
    if (_envelope_on) {
        _envelope_counter = _envelope_cycle;
    }
}

static void sound_set_buzzer_off(void) {
    _sound_on = 0;
    _envelope_counter = 0;
    _one_shot_counter = 0;
    tone_stop();
}

static int sound_is_one_shot_ringing(void) {
    return _one_shot_counter > 0;
}

/* ------------------------------------------------------------------ */
/* CPU module-level state                                             */
/* ------------------------------------------------------------------ */
static uint16_t *_ROM = NULL;
static int _rom_size = 0;

static double _OSC1_clock_div;
static double _OSC1_counter;
static double _timer_counter;
static double _ptimer_counter;
static double _stopwatch_counter;
static int _if_delay;
static int _RESET;

static int    (*_io_get[IORAM_SIZE])(void);
static void   (*_io_set[IORAM_SIZE])(int value);

static int _port_pullup_K0 = 15;
static int _port_pullup_K1 = 15;
static int _p3_dedicated = 0;

/* Registers */
static int _A, _B;                 /* 4-bit */
static uint16_t _IX, _IY;          /* 12-bit page+offset */
static int _SP;                    /* 8-bit */
static int _PC, _NPC;              /* 13-bit */
static int _CF, _ZF, _DF, _IF;     /* flags */
static uint8_t _RAM[RAM_SIZE];
static uint8_t _VRAM[VRAM_SIZE];
static int _HALT;

static int _P0_OUTPUT_DATA, _P1_OUTPUT_DATA, _P2_OUTPUT_DATA, _P3_OUTPUT_DATA;
static int _IT, _ISW, _IPT, _ISIO, _IK0, _IK1;
static int _EIT, _EISW, _EIPT, _EISIO, _EIK0, _EIK1;
static int _TM, _SWL, _SWH, _PT, _RD, _SD;
static int _K0, _DFK0, _K1;
static int _R0, _R1, _R2, _R3, _R4;
static int _P0, _P1, _P2, _P3;
static int _CTRL_OSC, _CTRL_LCD, _LC, _CTRL_SVD;
static int _CTRL_BZ1, _CTRL_BZ2, _CTRL_SW, _CTRL_PT;
static int _PTC, _SC, _HZR, _IOC, _PUP;

/* opcode table and A/B/MX/MY accessor tables */
EXT_RAM_BSS_ATTR static int (*_execute[4096])(uint16_t opcode);
static int (*_get_abmxmy[4])(void);
static void (*_set_abmxmy[4])(int value);

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */
static int get_mem_impl(int addr);
static void set_mem_impl(int addr, int value);
static double clock_impl(void);
static void _clock_OSC1(int osc1Ticks);
static void _initRegisters(void);
static int _interrupt(int vector);

/* ------------------------------------------------------------------ */
/* Dummy I/O handlers                                                 */
/* ------------------------------------------------------------------ */
static int _get_io_dummy(void)          { (void)0; return 0; }
static void _set_io_dummy(int value)    { (void)value; }

static int _get_io_it(void)             { int ret = _IT; _IT = 0; return ret; }
static int _get_io_isw(void)            { int ret = _ISW; _ISW = 0; return ret; }
static int _get_io_ipt(void)            { int ret = _IPT; _IPT = 0; return ret; }
static int _get_io_isio(void)           { int ret = _ISIO; _ISIO = 0; return ret; }
static int _get_io_ik0(void)            { int ret = _IK0; _IK0 = 0; return ret; }
static int _get_io_ik1(void)            { int ret = _IK1; _IK1 = 0; return ret; }

static int _get_io_eit(void)            { return _EIT; }
static void _set_io_eit(int value)      { _EIT = value; }

static int _get_io_eisw(void)           { return _EISW; }
static void _set_io_eisw(int value)     { _EISW = value & 0x3; }

static int _get_io_eipt(void)           { return _EIPT; }
static void _set_io_eipt(int value)     { _EIPT = value & 0x1; }

static int _get_io_eisio(void)          { return _EISIO; }
static void _set_io_eisio(int value)    { _EISIO = value & 0x1; }

static int _get_io_eik0(void)           { return _EIK0; }
static void _set_io_eik0(int value)     { _EIK0 = value; }

static int _get_io_eik1(void)           { return _EIK1; }
static void _set_io_eik1(int value)     { _EIK1 = value; }

static int _get_io_tm30(void)           { return _TM & 0xf; }
static int _get_io_tm74(void)           { return (_TM >> 4) & 0xf; }
static int _get_io_swl(void)            { return _SWL & 0xf; }
static int _get_io_swh(void)            { return _SWH & 0xf; }
static int _get_io_pt30(void)           { return _PT & 0xf; }
static int _get_io_pt74(void)           { return (_PT >> 4) & 0xf; }

static int _get_io_rd30(void)           { return _RD & 0xf; }
static void _set_io_rd30(int value)     { _RD = (_RD & 0xf0) | (value & 0x0f); }
static int _get_io_rd74(void)           { return (_RD >> 4) & 0xf; }
static void _set_io_rd74(int value)     { _RD = (_RD & 0x0f) | ((value << 4) & 0xf0); }

static int _get_io_sd30(void)           { return _SD & 0xf; }
static void _set_io_sd30(int value)     { _SD = (_SD & 0xf0) | (value & 0x0f); }
static int _get_io_sd74(void)           { return (_SD >> 4) & 0xf; }
static void _set_io_sd74(int value)     { _SD = (_SD & 0x0f) | ((value << 4) & 0xf0); }

static int _get_io_k0(void)             { return _K0; }
static int _get_io_dfk0(void)           { return _DFK0; }
static void _set_io_dfk0(int value)     { _DFK0 = value; }
static int _get_io_k1(void)             { return _K1; }

static int _get_io_r0(void)             { return _R0; }
static void _set_io_r0(int value)       { _R0 = value; }
static int _get_io_r1(void)             { return _R1; }
static void _set_io_r1(int value)       { _R1 = value; }
static int _get_io_r2(void)             { return _R2; }
static void _set_io_r2(int value)       { _R2 = value; }
static int _get_io_r3(void)             { return _R3; }
static void _set_io_r3(int value)       { _R3 = value; }
static int _get_io_r4(void)             { return _R4; }
static void _set_io_r4(int value) {
    _R4 = value;
    if (value & IO_R43) {
        sound_set_buzzer_off();
    } else {
        sound_set_buzzer_on();
    }
}

static int _get_io_p0(void)             { return _P0; }
static void _set_io_p0(int value) {
    _P0_OUTPUT_DATA = value;
    if (_IOC & IO_IOC0) { _P0 = value; }
}
static int _get_io_p1(void)             { return _P1; }
static void _set_io_p1(int value) {
    _P1_OUTPUT_DATA = value;
    if (_IOC & IO_IOC1) { _P1 = value; }
}
static int _get_io_p2(void)             { return _P2; }
static void _set_io_p2(int value) {
    _P2_OUTPUT_DATA = value;
    if (_IOC & IO_IOC2) { _P2 = value; }
}
static int _get_io_p3(void)             { return _P3; }
static void _set_io_p3(int value) {
    _P3_OUTPUT_DATA = value;
    if ((_IOC & IO_IOC3) || _p3_dedicated) { _P3 = value; }
}

static int _get_io_ioc(void)            { return _IOC; }
static void _set_io_ioc(int value) {
    _IOC = value;
    if (_IOC & IO_IOC0) { _P0 = _P0_OUTPUT_DATA; }
    if (_IOC & IO_IOC1) { _P1 = _P1_OUTPUT_DATA; }
    if (_IOC & IO_IOC2) { _P2 = _P2_OUTPUT_DATA; }
    if (_IOC & IO_IOC3) { _P3 = _P3_OUTPUT_DATA; }
}

static int _get_io_pup(void)            { return _PUP; }
static void _set_io_pup(int value)      { _PUP = value; }

static int _get_io_ctrl_osc(void)       { return _CTRL_OSC; }
static void _set_io_ctrl_osc(int value) { _CTRL_OSC = value; }

static int _get_io_ctrl_lcd(void)       { return _CTRL_LCD; }
static void _set_io_ctrl_lcd(int value) { _CTRL_LCD = value; }

static int _get_io_lc(void)             { return _LC; }
static void _set_io_lc(int value)       { _LC = value; }

static int _get_io_ctrl_svd(void)       { return 0; }

static int _get_io_ctrl_bz1(void)       { return _CTRL_BZ1; }
static void _set_io_ctrl_bz1(int value) {
    _CTRL_BZ1 = value;
    sound_set_freq(_CTRL_BZ1 & IO_BZFQ);
}

static int _get_io_ctrl_bz2(void) {
    int isOneShotRinging = sound_is_one_shot_ringing() ? 1 : 0;
    return (_CTRL_BZ2 & (IO_ENVRT | IO_ENVON)) | (IO_BZSHOT * isOneShotRinging);
}
static void _set_io_ctrl_bz2(int value) {
    _CTRL_BZ2 = value & (IO_ENVRT | IO_ENVON);

    int cycle = (value & IO_ENVRT) > 0 ? 1 : 0;
    sound_set_envelope_cycle(cycle);
    if (value & IO_BZSHOT) {
        int duration = (_CTRL_BZ1 & IO_SHOTPW) > 0 ? 1 : 0;
        sound_one_shot(duration);
    }
    if (value & IO_ENVON) {
        sound_set_envelope_on();
    } else {
        sound_set_envelope_off();
    }
    if (value & IO_ENVRST) {
        sound_reset_envelope();
    }
}

static void _set_io_ctrl_tm(int value) {
    if (value & IO_TMRST) { _TM = 0; }
}

static int _get_io_ctrl_sw(void)        { return _CTRL_SW & IO_SWRUN; }
static void _set_io_ctrl_sw(int value) {
    if (value & IO_SWRST) { _SWL = _SWH = 0; }
    _CTRL_SW = value & IO_SWRUN;
}

static int _get_io_ctrl_pt(void)        { return _CTRL_PT & IO_PTRUN; }
static void _set_io_ctrl_pt(int value) {
    if (value & IO_PTRST) { _PT = _RD; }
    _CTRL_PT = value & IO_PTRUN;
}

static int _get_io_ptc(void)            { return _PTC; }
static void _set_io_ptc(int value)      { _PTC = value; }

/* ------------------------------------------------------------------ */
/* A/B/MX/MY accessors                                                */
/* ------------------------------------------------------------------ */
static int  get_A_impl(void)          { return _A; }
static void set_A_impl(int value)     { _A = value & 0xf; }
static int  get_B_impl(void)          { return _B; }
static void set_B_impl(int value)     { _B = value & 0xf; }
static int  get_MX_impl(void)         { return get_mem_impl(_IX); }
static void set_MX_impl(int value)    { set_mem_impl(_IX, value); }
static int  get_MY_impl(void)         { return get_mem_impl(_IY); }
static void set_MY_impl(int value)    { set_mem_impl(_IY, value); }

/* ------------------------------------------------------------------ */
/* Memory access                                                      */
/* ------------------------------------------------------------------ */
static int get_mem_impl(int addr) {
    if (addr < RAM_SIZE) {
        return _RAM[addr];
    }
    if (addr >= VRAM_PART1_OFFSET && addr < VRAM_PART1_END) {
        return _VRAM[addr - VRAM_PART1_OFFSET];
    }
    if (addr >= VRAM_PART2_OFFSET && addr < VRAM_PART2_END) {
        return _VRAM[addr - VRAM_PART2_OFFSET + VRAM_PART_SIZE];
    }
    if (addr >= IORAM_OFFSET && addr < IORAM_END) {
        int idx = addr - IORAM_OFFSET;
        if (_io_get[idx]) {
            return _io_get[idx]();
        }
    }
    return 0;
}

static void set_mem_impl(int addr, int value) {
    if (addr < RAM_SIZE) {
        _RAM[addr] = (uint8_t)(value & 0xf);
    } else if (addr >= VRAM_PART1_OFFSET && addr < VRAM_PART1_END) {
        _VRAM[addr - VRAM_PART1_OFFSET] = (uint8_t)(value & 0xf);
    } else if (addr >= VRAM_PART2_OFFSET && addr < VRAM_PART2_END) {
        _VRAM[addr - VRAM_PART2_OFFSET + VRAM_PART_SIZE] = (uint8_t)(value & 0xf);
    } else if (addr >= IORAM_OFFSET && addr < IORAM_END) {
        int idx = addr - IORAM_OFFSET;
        if (_io_set[idx]) {
            _io_set[idx](value);   /* raw value, not masked (matches JS) */
        }
    }
}

int get_mem(int addr)  { return get_mem_impl(addr); }
void set_mem(int addr, int value) { set_mem_impl(addr, value); }

int  get_A(void)  { return get_A_impl(); }
void set_A(int value) { set_A_impl(value); }
int  get_B(void)  { return get_B_impl(); }
void set_B(int value) { set_B_impl(value); }
int  get_MX(void) { return get_MX_impl(); }
void set_MX(int value) { set_MX_impl(value); }
int  get_MY(void) { return get_MY_impl(); }
void set_MY(int value) { set_MY_impl(value); }

/* ------------------------------------------------------------------ */
/* Register / memory reset                                            */
/* ------------------------------------------------------------------ */
static void _initRegisters(void) {
    _A = 0;  _B = 0;  _IX = 0;  _IY = 0;  _SP = 0;

    _PC = 0x100;
    _NPC = 0x100;

    _CF = 0;  _ZF = 0;  _DF = 0;  _IF = 0;

    memset(_RAM, 0, RAM_SIZE);
    memset(_VRAM, 0, VRAM_SIZE);

    _HALT = 0;

    _P0_OUTPUT_DATA = 0;
    _P1_OUTPUT_DATA = 0;
    _P2_OUTPUT_DATA = 0;
    _P3_OUTPUT_DATA = 0;

    _IT = 0;   _ISW = 0;  _IPT = 0;  _ISIO = 0;  _IK0 = 0;  _IK1 = 0;
    _EIT = 0;  _EISW = 0; _EIPT = 0; _EISIO = 0; _EIK0 = 0; _EIK1 = 0;
    _TM = 0;   _SWL = 0;  _SWH = 0;  _PT = 0;    _RD = 0;   _SD = 0;
    _K0 = _port_pullup_K0;
    _DFK0 = 0xf;
    _K1 = _port_pullup_K1;
    _R0 = 0;  _R1 = 0;  _R2 = 0;  _R3 = 0;  _R4 = 0xf;
    _P0 = 0;  _P1 = 0;  _P2 = 0;  _P3 = 0;
    _CTRL_OSC = 0;
    _CTRL_LCD = IO_ALOFF;
    _LC = 0;
    _CTRL_SVD = IO_SVDDT;
    _CTRL_BZ1 = 0;
    _CTRL_BZ2 = 0;
    _CTRL_SW = 0;
    _CTRL_PT = 0;
    _PTC = 0;
    _SC = 0;
    _HZR = 0;
    _IOC = 0;
    _PUP = 0;
}

void reset(void) {
    _initRegisters();

    _OSC1_counter = 0;
    _timer_counter = 0;
    _stopwatch_counter = 0;
    sound_set_buzzer_off();
    sound_set_envelope_off();
}

/* ------------------------------------------------------------------ */
/* Save / load state                                                  */
/* ------------------------------------------------------------------ */
uint32_t vpet_save_state_size(void) { return E0C6200_SAVE_STATE_SIZE; }

static const uint8_t SAVE_STATE_MAGIC[4] = { 0x56, 0x50, 0x45, 0x54 }; /* "VPET" */
#define SAVE_STATE_VERSION 1

void saveState(uint8_t *out) {
    uint8_t *p = out;
    int i;

    for (i = 0; i < 4; i++) { *p++ = SAVE_STATE_MAGIC[i]; }
    *p++ = SAVE_STATE_VERSION;
    *p++ = (uint8_t)_A;
    *p++ = (uint8_t)_B;
    *p++ = (uint8_t)_SP;
    *p++ = (uint8_t)_CF;
    *p++ = (uint8_t)_ZF;
    *p++ = (uint8_t)_DF;
    *p++ = (uint8_t)_IF;
    *p++ = (uint8_t)_HALT;
    *p++ = _if_delay ? 1 : 0;
    *p++ = (uint8_t)_P0_OUTPUT_DATA;
    *p++ = (uint8_t)_P1_OUTPUT_DATA;
    *p++ = (uint8_t)_P2_OUTPUT_DATA;
    *p++ = (uint8_t)_P3_OUTPUT_DATA;
    *p++ = (uint8_t)_IT;
    *p++ = (uint8_t)_ISW;
    *p++ = (uint8_t)_IPT;
    *p++ = (uint8_t)_ISIO;
    *p++ = (uint8_t)_IK0;
    *p++ = (uint8_t)_IK1;
    *p++ = (uint8_t)_EIT;
    *p++ = (uint8_t)_EISW;
    *p++ = (uint8_t)_EIPT;
    *p++ = (uint8_t)_EISIO;
    *p++ = (uint8_t)_EIK0;
    *p++ = (uint8_t)_EIK1;
    *p++ = (uint8_t)_TM;
    *p++ = (uint8_t)_SWL;
    *p++ = (uint8_t)_SWH;
    *p++ = (uint8_t)_PT;
    *p++ = (uint8_t)_RD;
    *p++ = (uint8_t)_SD;
    *p++ = (uint8_t)_K0;
    *p++ = (uint8_t)_DFK0;
    *p++ = (uint8_t)_K1;
    *p++ = (uint8_t)_R0;
    *p++ = (uint8_t)_R1;
    *p++ = (uint8_t)_R2;
    *p++ = (uint8_t)_R3;
    *p++ = (uint8_t)_R4;
    *p++ = (uint8_t)_P0;
    *p++ = (uint8_t)_P1;
    *p++ = (uint8_t)_P2;
    *p++ = (uint8_t)_P3;
    *p++ = (uint8_t)_CTRL_OSC;
    *p++ = (uint8_t)_CTRL_LCD;
    *p++ = (uint8_t)_LC;
    *p++ = (uint8_t)_CTRL_SVD;
    *p++ = (uint8_t)_CTRL_BZ1;
    *p++ = (uint8_t)_CTRL_BZ2;
    *p++ = (uint8_t)_CTRL_SW;
    *p++ = (uint8_t)_CTRL_PT;
    *p++ = (uint8_t)_PTC;
    *p++ = (uint8_t)_SC;
    *p++ = (uint8_t)_HZR;
    *p++ = (uint8_t)_IOC;
    *p++ = (uint8_t)_PUP;
    *p++ = (uint8_t)(_IX & 0xff);       /* setUint16 LE (matches JS little-endian) */
    *p++ = (uint8_t)((_IX >> 8) & 0xff);
    *p++ = (uint8_t)(_IY & 0xff);
    *p++ = (uint8_t)((_IY >> 8) & 0xff);
    *p++ = (uint8_t)(_PC & 0xff);
    *p++ = (uint8_t)((_PC >> 8) & 0xff);
    *p++ = (uint8_t)(_NPC & 0xff);
    *p++ = (uint8_t)((_NPC >> 8) & 0xff);
    {
        uint32_t o0 = (uint32_t)_OSC1_counter;        /* setUint32 == ToUint32 */
        uint32_t t0 = (uint32_t)_timer_counter;
        uint32_t p0 = (uint32_t)_ptimer_counter;
        uint32_t s0 = (uint32_t)_stopwatch_counter;
        *p++ = (uint8_t)(o0 & 0xff); *p++ = (uint8_t)((o0 >> 8) & 0xff);
        *p++ = (uint8_t)((o0 >> 16) & 0xff); *p++ = (uint8_t)((o0 >> 24) & 0xff);
        *p++ = (uint8_t)(t0 & 0xff); *p++ = (uint8_t)((t0 >> 8) & 0xff);
        *p++ = (uint8_t)((t0 >> 16) & 0xff); *p++ = (uint8_t)((t0 >> 24) & 0xff);
        *p++ = (uint8_t)(p0 & 0xff); *p++ = (uint8_t)((p0 >> 8) & 0xff);
        *p++ = (uint8_t)((p0 >> 16) & 0xff); *p++ = (uint8_t)((p0 >> 24) & 0xff);
        *p++ = (uint8_t)(s0 & 0xff); *p++ = (uint8_t)((s0 >> 8) & 0xff);
        *p++ = (uint8_t)((s0 >> 16) & 0xff); *p++ = (uint8_t)((s0 >> 24) & 0xff);
    }
    memcpy(p, _RAM, RAM_SIZE);  p += RAM_SIZE;
    memcpy(p, _VRAM, VRAM_SIZE);
}

static uint16_t rd_le16(const uint8_t *b) {
    return (uint16_t)(b[0] | (b[1] << 8));
}
static uint32_t rd_le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

int loadState(const uint8_t *buf) {
    const uint8_t *p = buf;
    int i;

    for (i = 0; i < 4; i++) {
        if (*p++ != SAVE_STATE_MAGIC[i]) return -2;
    }
    if (*p++ != SAVE_STATE_VERSION) return -3;

    _A = *p++;
    _B = *p++;
    _SP = *p++;
    _CF = *p++;
    _ZF = *p++;
    _DF = *p++;
    _IF = *p++;
    _HALT = *p++;
    _if_delay = (*p++ != 0);
    _P0_OUTPUT_DATA = *p++;
    _P1_OUTPUT_DATA = *p++;
    _P2_OUTPUT_DATA = *p++;
    _P3_OUTPUT_DATA = *p++;
    _IT = *p++;
    _ISW = *p++;
    _IPT = *p++;
    _ISIO = *p++;
    _IK0 = *p++;
    _IK1 = *p++;
    _EIT = *p++;
    _EISW = *p++;
    _EIPT = *p++;
    _EISIO = *p++;
    _EIK0 = *p++;
    _EIK1 = *p++;
    _TM = *p++;
    _SWL = *p++;
    _SWH = *p++;
    _PT = *p++;
    _RD = *p++;
    _SD = *p++;
    _K0 = *p++;
    _DFK0 = *p++;
    _K1 = *p++;
    _R0 = *p++;
    _R1 = *p++;
    _R2 = *p++;
    _R3 = *p++;
    _R4 = *p++;
    _P0 = *p++;
    _P1 = *p++;
    _P2 = *p++;
    _P3 = *p++;
    _CTRL_OSC = *p++;
    _CTRL_LCD = *p++;
    _LC = *p++;
    _CTRL_SVD = *p++;
    _CTRL_BZ1 = *p++;
    _CTRL_BZ2 = *p++;
    _CTRL_SW = *p++;
    _CTRL_PT = *p++;
    _PTC = *p++;
    _SC = *p++;
    _HZR = *p++;
    _IOC = *p++;
    _PUP = *p++;
    _IX = rd_le16(p); p += 2;
    _IY = rd_le16(p); p += 2;
    _PC = rd_le16(p); p += 2;
    _NPC = rd_le16(p); p += 2;
    _OSC1_counter = rd_le32(p); p += 4;
    _timer_counter = rd_le32(p); p += 4;
    _ptimer_counter = rd_le32(p); p += 4;
    _stopwatch_counter = rd_le32(p); p += 4;
    memcpy(_RAM, p, RAM_SIZE); p += RAM_SIZE;
    memcpy(_VRAM, p, VRAM_SIZE);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Opcode implementations                                             */
/* ------------------------------------------------------------------ */
static int _jp_s(uint16_t opcode) {
    /* PCB←NBP, PCP←NPP, PCS←s7~s0 */
    _PC = (_NPC & 0x1f00) | (opcode & 0x0ff);
    return 5;
}

static int _retd_l(uint16_t opcode) {
    /* PCSL←M(SP), PCSH←M(SP+1), PCP←M(SP+2) SP←SP+3, M(X)←l3~l0, M(X+1)←l7~l4, X←X+2 */
    _PC = _NPC =
        (_PC & 0x1000) | (_RAM[_SP + 2] << 8) | (_RAM[_SP + 1] << 4) | _RAM[_SP];
    _SP = (_SP + 3) & 0xff;
    set_mem_impl(_IX, opcode & 0x00f);
    set_mem_impl((_IX & 0xf00) | ((_IX + 1) & 0xff), (opcode >> 4) & 0x00f);
    _IX = (_IX & 0xf00) | ((_IX + 2) & 0xff);
    return 12;
}

static int _jp_c_s(uint16_t opcode) {
    /* PCB←NBP, PCP←NPP, PCS←s7~s0 if C=1 */
    if (_CF) {
        _PC = (_NPC & 0x1f00) | (opcode & 0x0ff);
    } else {
        _PC = _NPC = (_PC & 0x1000) | ((_PC + 1) & 0xfff);
    }
    return 5;
}

static int _jp_nc_s(uint16_t opcode) {
    /* ... if C=0 */
    if (!_CF) {
        _PC = (_NPC & 0x1f00) | (opcode & 0x0ff);
    } else {
        _PC = _NPC = (_PC & 0x1000) | ((_PC + 1) & 0xfff);
    }
    return 5;
}

static int _call_s(uint16_t opcode) {
    /* M(SP-1)←PCP, M(SP-2)←PCSH, M(SP-3)←PCSL+1 SP←SP-3, PCP←NPP, PCS←s7~s0 */
    set_mem_impl((_SP - 1) & 0xff, ((_PC + 1) >> 8) & 0x0f);
    set_mem_impl((_SP - 2) & 0xff, ((_PC + 1) >> 4) & 0x0f);
    _SP = (_SP - 3) & 0xff;
    set_mem_impl(_SP, (_PC + 1) & 0x0f);
    _PC = (_NPC & 0x1f00) | (opcode & 0x0ff);
    return 7;
}

static int _calz_s(uint16_t opcode) {
    /* ... PCP←0, PCS←s7~s0 */
    set_mem_impl((_SP - 1) & 0xff, ((_PC + 1) >> 8) & 0x0f);
    set_mem_impl((_SP - 2) & 0xff, ((_PC + 1) >> 4) & 0x0f);
    _SP = (_SP - 3) & 0xff;
    set_mem_impl(_SP, (_PC + 1) & 0x0f);
    _PC = _NPC = (_NPC & 0x1000) | (opcode & 0x0ff);
    return 7;
}

static int _jp_z_s(uint16_t opcode) {
    /* ... if Z=1 */
    if (_ZF) {
        _PC = (_NPC & 0x1f00) | (opcode & 0x0ff);
    } else {
        _PC = _NPC = (_PC & 0x1000) | ((_PC + 1) & 0xfff);
    }
    return 5;
}

static int _jp_nz_s(uint16_t opcode) {
    /* ... if Z=0 */
    if (!_ZF) {
        _PC = (_NPC & 0x1f00) | (opcode & 0x0ff);
    } else {
        _PC = _NPC = (_PC & 0x1000) | ((_PC + 1) & 0xfff);
    }
    return 5;
}

static int _ld_y_y(uint16_t opcode) {
    /* YH←y7~y4, YL←y3~y0 */
    _IY = (_IY & 0xf00) | (opcode & 0x0ff);
    _PC = _NPC = (_PC & 0x1000) | ((_PC + 1) & 0xfff);
    return 5;
}

static int _lbpx_mx_l(uint16_t opcode) {
    /* M(X)←l3~l0, M(X+1)←l7~l4, X←X+2 */
    set_mem_impl(_IX, opcode & 0x00f);
    set_mem_impl((_IX & 0xf00) | ((_IX + 1) & 0xff), (opcode >> 4) & 0x00f);
    _IX = (_IX & 0xf00) | ((_IX + 2) & 0xff);
    _PC = _NPC = (_PC & 0x1000) | ((_PC + 1) & 0xfff);
    return 5;
}

#define INC_PC() ((_PC & 0x1000) | ((_PC + 1) & 0xfff))

static int _adc_xh_i(uint16_t opcode) {
    /* XH←XH+i3~i0+C */
    int xh = ((_IX >> 4) & 0x00f) + (opcode & 0x00f) + _CF;
    _ZF = (xh & 0xf) == 0 ? 1 : 0;
    _CF = xh > 15 ? 1 : 0;
    _IX = (_IX & 0xf0f) | ((xh << 4) & 0x0f0);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _adc_xl_i(uint16_t opcode) {
    /* XL←XL+i3~i0+C */
    int xl = (_IX & 0x00f) + (opcode & 0x00f) + _CF;
    _ZF = (xl & 0xf) == 0 ? 1 : 0;
    _CF = xl > 15 ? 1 : 0;
    _IX = (_IX & 0xff0) | (xl & 0x00f);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _adc_yh_i(uint16_t opcode) {
    /* YH←YH+i3~i0+C */
    int yh = ((_IY >> 4) & 0x00f) + (opcode & 0x00f) + _CF;
    _ZF = (yh & 0xf) == 0 ? 1 : 0;
    _CF = yh > 15 ? 1 : 0;
    _IY = (_IY & 0xf0f) | ((yh << 4) & 0x0f0);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _adc_yl_i(uint16_t opcode) {
    /* YL←YL+i3~i0+C */
    int yl = (_IY & 0x00f) + (opcode & 0x00f) + _CF;
    _ZF = (yl & 0xf) == 0 ? 1 : 0;
    _CF = yl > 15 ? 1 : 0;
    _IY = (_IY & 0xff0) | (yl & 0x00f);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _cp_xh_i(uint16_t opcode) {
    /* XH-i3~i0 */
    int cp = ((_IX >> 4) & 0x00f) - (opcode & 0x00f);
    _ZF = cp == 0 ? 1 : 0;
    _CF = cp < 0 ? 1 : 0;
    _PC = _NPC = INC_PC();
    return 7;
}

static int _cp_xl_i(uint16_t opcode) {
    /* XL-i3~i0 */
    int cp = (_IX & 0x00f) - (opcode & 0x00f);
    _ZF = cp == 0 ? 1 : 0;
    _CF = cp < 0 ? 1 : 0;
    _PC = _NPC = INC_PC();
    return 7;
}

static int _cp_yh_i(uint16_t opcode) {
    /* YH-i3~i0 */
    int cp = ((_IY >> 4) & 0x00f) - (opcode & 0x00f);
    _ZF = cp == 0 ? 1 : 0;
    _CF = cp < 0 ? 1 : 0;
    _PC = _NPC = INC_PC();
    return 7;
}

static int _cp_yl_i(uint16_t opcode) {
    /* YL-i3~i0 */
    int cp = (_IY & 0x00f) - (opcode & 0x00f);
    _ZF = cp == 0 ? 1 : 0;
    _CF = cp < 0 ? 1 : 0;
    _PC = _NPC = INC_PC();
    return 7;
}

static int _add_r_q(uint16_t opcode) {
    /* r←r+q */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    int res = _get_abmxmy[r]() + _get_abmxmy[q]();
    _CF = (res > 15) ? 1 : 0;
    if (_DF && res > 9) { res += 6; _CF = 1; }
    _ZF = (res & 0xf) == 0 ? 1 : 0;
    _set_abmxmy[r](res & 0xf);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _adc_r_q(uint16_t opcode) {
    /* r←r+q+C */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    int res = _get_abmxmy[r]() + _get_abmxmy[q]() + _CF;
    _CF = (res > 15) ? 1 : 0;
    if (_DF && res > 9) { res += 6; _CF = 1; }
    _ZF = (res & 0xf) == 0 ? 1 : 0;
    _set_abmxmy[r](res & 0xf);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _sub_r_q(uint16_t opcode) {
    /* r←r-q */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    int res = _get_abmxmy[r]() - _get_abmxmy[q]();
    _CF = (res < 0) ? 1 : 0;
    if (_DF && res < 0) { res += 10; }
    _ZF = (res & 0xf) == 0 ? 1 : 0;
    _set_abmxmy[r](res & 0xf);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _sbc_r_q(uint16_t opcode) {
    /* r←r-q-C */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    int res = _get_abmxmy[r]() - _get_abmxmy[q]() - _CF;
    _CF = (res < 0) ? 1 : 0;
    if (_DF && res < 0) { res += 10; }
    _ZF = (res & 0xf) == 0 ? 1 : 0;
    _set_abmxmy[r](res & 0xf);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _and_r_q(uint16_t opcode) {
    /* r←r && q */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    int res = _get_abmxmy[r]() & _get_abmxmy[q]();
    _ZF = res == 0 ? 1 : 0;
    _set_abmxmy[r](res);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _or_r_q(uint16_t opcode) {
    /* r←r or q */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    int res = _get_abmxmy[r]() | _get_abmxmy[q]();
    _ZF = res == 0 ? 1 : 0;
    _set_abmxmy[r](res);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _xor_r_q(uint16_t opcode) {
    /* r←r xor q */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    int res = _get_abmxmy[r]() ^ _get_abmxmy[q]();
    _ZF = res == 0 ? 1 : 0;
    _set_abmxmy[r](res);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _rlc_r(uint16_t opcode) {
    /* d3←d2, d2←d1, d1←d0, d0←C, C←d3 */
    int r = opcode & 0x3;
    int res = (_get_abmxmy[r]() << 1) + _CF;
    _CF = (res > 15) ? 1 : 0;
    _set_abmxmy[r](res & 0xf);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _ld_x_x(uint16_t opcode) {
    /* XH←x7~x4, XL←x3~x0 */
    _IX = (_IX & 0xf00) | (opcode & 0x0ff);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _add_r_i(uint16_t opcode) {
    /* r←r+i3~i0 */
    int r = (opcode >> 4) & 0x3;
    int res = _get_abmxmy[r]() + (opcode & 0x00f);
    _CF = (res > 15) ? 1 : 0;
    if (_DF && res > 9) { res += 6; _CF = 1; }
    _ZF = (res & 0xf) == 0 ? 1 : 0;
    _set_abmxmy[r](res & 0xf);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _adc_r_i(uint16_t opcode) {
    /* r←r+i3~i0+C */
    int r = (opcode >> 4) & 0x3;
    int res = _get_abmxmy[r]() + (opcode & 0x00f) + _CF;
    _CF = (res > 15) ? 1 : 0;
    if (_DF && res > 9) { res += 6; _CF = 1; }
    _ZF = (res & 0xf) == 0 ? 1 : 0;
    _set_abmxmy[r](res & 0xf);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _and_r_i(uint16_t opcode) {
    /* r←r && i3~i0 */
    int r = (opcode >> 4) & 0x3;
    int res = _get_abmxmy[r]() & opcode & 0x00f;
    _ZF = res == 0 ? 1 : 0;
    _set_abmxmy[r](res);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _or_r_i(uint16_t opcode) {
    /* r←r | i3~i0 */
    int r = (opcode >> 4) & 0x3;
    int res = _get_abmxmy[r]() | (opcode & 0x00f);
    _ZF = res == 0 ? 1 : 0;
    _set_abmxmy[r](res);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _xor_r_i(uint16_t opcode) {
    /* r←r ^ i3~i0 */
    int r = (opcode >> 4) & 0x3;
    int res = _get_abmxmy[r]() ^ (opcode & 0x00f);
    _ZF = res == 0 ? 1 : 0;
    _set_abmxmy[r](res);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _sbc_r_i(uint16_t opcode) {
    /* r←r-i3~i0-C */
    int r = (opcode >> 4) & 0x3;
    int res = _get_abmxmy[r]() - (opcode & 0x00f) - _CF;
    _CF = (res < 0) ? 1 : 0;
    if (_DF && _CF) { res += 10; }
    _ZF = (res & 0xf) == 0 ? 1 : 0;
    _set_abmxmy[r](res & 0xf);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _fan_r_i(uint16_t opcode) {
    /* r && i3~i0 */
    int r = (opcode >> 4) & 0x3;
    _ZF = (_get_abmxmy[r]() & opcode & 0x00f) == 0 ? 1 : 0;
    _PC = _NPC = INC_PC();
    return 7;
}

static int _cp_r_i(uint16_t opcode) {
    /* r-i3~i0 */
    int r = (opcode >> 4) & 0x3;
    int cp = _get_abmxmy[r]() - (opcode & 0x00f);
    _ZF = cp == 0 ? 1 : 0;
    _CF = cp < 0 ? 1 : 0;
    _PC = _NPC = INC_PC();
    return 7;
}

static int _ld_r_i(uint16_t opcode) {
    /* r←i3~i0 */
    int r = (opcode >> 4) & 0x3;
    _set_abmxmy[r](opcode & 0x00f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _pset_p(uint16_t opcode) {
    /* NBP←p4, NPP←p3~p0 */
    _if_delay = 1;
    _NPC = (opcode << 8) & 0x1f00;
    _PC = (_PC & 0x1000) | ((_PC + 1) & 0xfff);
    return 5;
}

static int _ldpx_mx_i(uint16_t opcode) {
    /* M(X)←i3~i0, X←X+1 */
    set_mem_impl(_IX, opcode & 0x00f);
    _IX = (_IX & 0xf00) | ((_IX + 1) & 0xff);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ldpy_my_i(uint16_t opcode) {
    /* M(Y)←i3~i0, Y←Y+1 */
    set_mem_impl(_IY, opcode & 0x00f);
    _IY = (_IY & 0xf00) | ((_IY + 1) & 0xff);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_xp_r(uint16_t opcode) {
    /* XP←r */
    int r = opcode & 0x3;
    _IX = (_get_abmxmy[r]() << 8) | (_IX & 0x0ff);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_xh_r(uint16_t opcode) {
    /* XH←r */
    int r = opcode & 0x3;
    _IX = (_get_abmxmy[r]() << 4) | (_IX & 0xf0f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_xl_r(uint16_t opcode) {
    /* XL←r */
    int r = opcode & 0x3;
    _IX = _get_abmxmy[r]() | (_IX & 0xff0);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _rrc_r(uint16_t opcode) {
    /* d3←C, d2←d3, d1←d2, d0←d1, C←d0 */
    int r = opcode & 0x3;
    int res = _get_abmxmy[r]() + (_CF << 4);
    _CF = res & 0x1;
    _set_abmxmy[r](res >> 1);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_yp_r(uint16_t opcode) {
    /* YP←r */
    int r = opcode & 0x3;
    _IY = (_get_abmxmy[r]() << 8) | (_IY & 0x0ff);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_yh_r(uint16_t opcode) {
    /* YH←r */
    int r = opcode & 0x3;
    _IY = (_get_abmxmy[r]() << 4) | (_IY & 0xf0f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_yl_r(uint16_t opcode) {
    /* YL←r */
    int r = opcode & 0x3;
    _IY = _get_abmxmy[r]() | (_IY & 0xff0);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _dummy(uint16_t opcode) { (void)opcode; return 5; }

static int _ld_r_xp(uint16_t opcode) {
    /* r←XP */
    int r = opcode & 0x3;
    _set_abmxmy[r](_IX >> 8);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_r_xh(uint16_t opcode) {
    /* r←XH */
    int r = opcode & 0x3;
    _set_abmxmy[r]((_IX >> 4) & 0x00f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_r_xl(uint16_t opcode) {
    /* r←XL */
    int r = opcode & 0x3;
    _set_abmxmy[r](_IX & 0x00f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_r_yp(uint16_t opcode) {
    /* r←YP */
    int r = opcode & 0x3;
    _set_abmxmy[r](_IY >> 8);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_r_yh(uint16_t opcode) {
    /* r←YH */
    int r = opcode & 0x3;
    _set_abmxmy[r]((_IY >> 4) & 0x00f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_r_yl(uint16_t opcode) {
    /* r←YL */
    int r = opcode & 0x3;
    _set_abmxmy[r](_IY & 0x00f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_r_q(uint16_t opcode) {
    /* r←q */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    _set_abmxmy[r](_get_abmxmy[q]());
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ldpx_r_q(uint16_t opcode) {
    /* r←q, X←X+1 */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    _set_abmxmy[r](_get_abmxmy[q]());
    _IX = (_IX & 0xf00) | ((_IX + 1) & 0xff);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ldpy_r_q(uint16_t opcode) {
    /* r←q, Y←Y+1 */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    _set_abmxmy[r](_get_abmxmy[q]());
    _IY = (_IY & 0xf00) | ((_IY + 1) & 0xff);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _cp_r_q(uint16_t opcode) {
    /* r-q */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    int cp = _get_abmxmy[r]() - _get_abmxmy[q]();
    _ZF = cp == 0 ? 1 : 0;
    _CF = cp < 0 ? 1 : 0;
    _PC = _NPC = INC_PC();
    return 7;
}

static int _fan_r_q(uint16_t opcode) {
    /* r && q */
    int r = (opcode >> 2) & 0x3;
    int q = opcode & 0x3;
    _ZF = (_get_abmxmy[r]() & _get_abmxmy[q]()) == 0 ? 1 : 0;
    _PC = _NPC = INC_PC();
    return 7;
}

static int _acpx_mx_r(uint16_t opcode) {
    /* M(X)←M(X)+r+C, X←X+1 */
    int r = opcode & 0x3;
    int res = get_mem_impl(_IX) + _get_abmxmy[r]() + _CF;
    _CF = (res > 15) ? 1 : 0;
    if (_DF && res > 9) { res += 6; _CF = 1; }
    _ZF = (res & 0xf) == 0 ? 1 : 0;
    set_mem_impl(_IX, res & 0xf);
    _IX = (_IX & 0xf00) | ((_IX + 1) & 0xff);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _acpy_my_r(uint16_t opcode) {
    /* M(Y)←M(Y)+r+C, Y←Y+1 */
    int r = opcode & 0x3;
    int res = get_mem_impl(_IY) + _get_abmxmy[r]() + _CF;
    _CF = (res > 15) ? 1 : 0;
    if (_DF && res > 9) { res += 6; _CF = 1; }
    _ZF = (res & 0xf) == 0 ? 1 : 0;
    set_mem_impl(_IY, res & 0xf);
    _IY = (_IY & 0xf00) | ((_IY + 1) & 0xff);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _scpx_mx_r(uint16_t opcode) {
    /* M(X)←M(X)-r-C, X←X+1 */
    int r = opcode & 0x3;
    int res = get_mem_impl(_IX) - _get_abmxmy[r]() - _CF;
    _CF = (res < 0) ? 1 : 0;
    if (_DF && res < 0) { res += 10; }
    _ZF = (res & 0xf) == 0 ? 1 : 0;
    set_mem_impl(_IX, res & 0xf);
    _IX = (_IX & 0xf00) | ((_IX + 1) & 0xff);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _scpy_my_r(uint16_t opcode) {
    /* M(Y)←M(Y)-r-C, Y←Y+1 */
    int r = opcode & 0x3;
    int res = get_mem_impl(_IY) - _get_abmxmy[r]() - _CF;
    _CF = (res < 0) ? 1 : 0;
    if (_DF && res < 0) { res += 10; }
    _ZF = (res & 0xf) == 0 ? 1 : 0;
    set_mem_impl(_IY, res & 0xf);
    _IY = (_IY & 0xf00) | ((_IY + 1) & 0xff);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _set_f_i(uint16_t opcode) {
    /* F←F or i3~i0 */
    _CF |= opcode & 0x001;
    _ZF |= (opcode >> 1) & 0x001;
    _DF |= (opcode >> 2) & 0x001;
    int new_IF = (opcode >> 3) & 0x001;
    _if_delay = new_IF && !_IF;
    _IF |= new_IF;
    _PC = _NPC = INC_PC();
    return 7;
}

static int _rst_f_i(uint16_t opcode) {
    /* F←F & ~i3~i0 */
    _CF &= opcode;
    _ZF &= opcode >> 1;
    _DF &= opcode >> 2;
    _IF &= opcode >> 3;
    _PC = _NPC = INC_PC();
    return 7;
}

static int _inc_mn(uint16_t opcode) {
    /* M(n3~n0)←M(n3~n0)+1 */
    int mn = opcode & 0x00f;
    int res = get_mem_impl(mn) + 1;
    _ZF = res == 16 ? 1 : 0;
    _CF = (res > 15) ? 1 : 0;
    set_mem_impl(mn, res & 0xf);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _dec_mn(uint16_t opcode) {
    /* M(n3~n0)←M(n3~n0)-1 */
    int mn = opcode & 0x00f;
    int res = get_mem_impl(mn) - 1;
    _ZF = res == 0 ? 1 : 0;
    _CF = (res < 0) ? 1 : 0;
    set_mem_impl(mn, res & 0xf);
    _PC = _NPC = INC_PC();
    return 7;
}

static int _ld_mn_a(uint16_t opcode) {
    /* M(n3~n0)←A */
    set_mem_impl(opcode & 0x00f, _A & 0xf);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_mn_b(uint16_t opcode) {
    /* M(n3~n0)←B */
    set_mem_impl(opcode & 0x00f, _B & 0xf);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_a_mn(uint16_t opcode) {
    /* A←M(n3~n0) */
    _A = get_mem_impl(opcode & 0x00f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_b_mn(uint16_t opcode) {
    /* B←M(n3~n0) */
    _B = get_mem_impl(opcode & 0x00f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _push_r(uint16_t opcode) {
    /* SP←SP-1, M(SP)←r */
    int r = opcode & 0x3;
    _SP = (_SP - 1) & 0xff;
    set_mem_impl(_SP, _get_abmxmy[r]());
    _PC = _NPC = INC_PC();
    return 5;
}

static int _push_xp(uint16_t opcode) { (void)opcode;
    _SP = (_SP - 1) & 0xff;
    set_mem_impl(_SP, _IX >> 8);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _push_xh(uint16_t opcode) { (void)opcode;
    _SP = (_SP - 1) & 0xff;
    set_mem_impl(_SP, (_IX >> 4) & 0x00f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _push_xl(uint16_t opcode) { (void)opcode;
    _SP = (_SP - 1) & 0xff;
    set_mem_impl(_SP, _IX & 0x00f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _push_yp(uint16_t opcode) { (void)opcode;
    _SP = (_SP - 1) & 0xff;
    set_mem_impl(_SP, _IY >> 8);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _push_yh(uint16_t opcode) { (void)opcode;
    _SP = (_SP - 1) & 0xff;
    set_mem_impl(_SP, (_IY >> 4) & 0x00f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _push_yl(uint16_t opcode) { (void)opcode;
    _SP = (_SP - 1) & 0xff;
    set_mem_impl(_SP, _IY & 0x00f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _push_f(uint16_t opcode) { (void)opcode;
    _SP = (_SP - 1) & 0xff;
    set_mem_impl(_SP, (_IF << 3) | (_DF << 2) | (_ZF << 1) | _CF);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _dec_sp(uint16_t opcode) { (void)opcode;
    _SP = (_SP - 1) & 0xff;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _pop_r(uint16_t opcode) {
    /* r←M(SP), SP←SP+1 */
    int r = opcode & 0x3;
    _set_abmxmy[r](get_mem_impl(_SP));
    _SP = (_SP + 1) & 0xff;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _pop_xp(uint16_t opcode) { (void)opcode;
    _IX = (get_mem_impl(_SP) << 8) | (_IX & 0x0ff);
    _SP = (_SP + 1) & 0xff;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _pop_xh(uint16_t opcode) { (void)opcode;
    _IX = (get_mem_impl(_SP) << 4) | (_IX & 0xf0f);
    _SP = (_SP + 1) & 0xff;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _pop_xl(uint16_t opcode) { (void)opcode;
    _IX = get_mem_impl(_SP) | (_IX & 0xff0);
    _SP = (_SP + 1) & 0xff;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _pop_yp(uint16_t opcode) { (void)opcode;
    _IY = (get_mem_impl(_SP) << 8) | (_IY & 0x0ff);
    _SP = (_SP + 1) & 0xff;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _pop_yh(uint16_t opcode) { (void)opcode;
    _IY = (get_mem_impl(_SP) << 4) | (_IY & 0xf0f);
    _SP = (_SP + 1) & 0xff;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _pop_yl(uint16_t opcode) { (void)opcode;
    _IY = get_mem_impl(_SP) | (_IY & 0xff0);
    _SP = (_SP + 1) & 0xff;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _pop_f(uint16_t opcode) { (void)opcode;
    int f = get_mem_impl(_SP);
    _CF = f & 0x1;
    _ZF = (f >> 1) & 0x1;
    _DF = (f >> 2) & 0x1;
    int new_IF = (f >> 3) & 0x1;
    _if_delay = new_IF && !_IF;
    _IF = new_IF;
    _SP = (_SP + 1) & 0xff;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _inc_sp(uint16_t opcode) { (void)opcode;
    _SP = (_SP + 1) & 0xff;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _rets(uint16_t opcode) { (void)opcode;
    /* PCSL←M(SP), PCSH←M(SP+1), PCP←M(SP+2) SP←SP+3, PC←PC+1 */
    _PC = (_PC & 0x1000) |
          get_mem_impl(_SP) |
          (get_mem_impl(_SP + 1) << 4) |
          (get_mem_impl(_SP + 2) << 8);
    _SP = (_SP + 3) & 0xff;
    _PC = _NPC = INC_PC();
    return 12;
}

static int _ret(uint16_t opcode) { (void)opcode;
    /* PCSL←M(SP), PCSH←M(SP+1), PCP←M(SP+2) SP←SP+3 */
    _PC = _NPC = (_PC & 0x1000) |
          get_mem_impl(_SP) |
          (get_mem_impl(_SP + 1) << 4) |
          (get_mem_impl(_SP + 2) << 8);
    _SP = (_SP + 3) & 0xff;
    return 7;
}

static int _ld_sph_r(uint16_t opcode) {
    /* SPH←r */
    int r = opcode & 0x3;
    _SP = (_get_abmxmy[r]() << 4) | (_SP & 0x0f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_r_sph(uint16_t opcode) {
    /* r←SPH */
    int r = opcode & 0x3;
    _set_abmxmy[r](_SP >> 4);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _jpba(uint16_t opcode) { (void)opcode;
    /* PCB←NBP, PCP←NPP, PCSH←B, PCSL←A */
    _PC = (_NPC & 0x1f00) | (_B << 4) | _A;
    return 5;
}

static int _ld_spl_r(uint16_t opcode) {
    /* SPL←r */
    int r = opcode & 0x3;
    _SP = _get_abmxmy[r]() | (_SP & 0xf0);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _ld_r_spl(uint16_t opcode) { (void)opcode;
    int r = opcode & 0x3;
    _set_abmxmy[r](_SP & 0x0f);
    _PC = _NPC = INC_PC();
    return 5;
}

static int _halt(uint16_t opcode) { (void)opcode;
    _HALT = 1;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _nop5(uint16_t opcode) { (void)opcode;
    _PC = _NPC = INC_PC();
    return 5;
}

static int _nop7(uint16_t opcode) { (void)opcode;
    _PC = _NPC = INC_PC();
    return 7;
}

/* ------------------------------------------------------------------ */
/* opcode table construction                                          */
/* ------------------------------------------------------------------ */
static int fill_op_range(int start, int count, int (*fn)(uint16_t)) {
    int i;
    for (i = 0; i < count; i++) {
        _execute[start + i] = fn;
    }
    return start + count;
}

static void build_execute_table(void) {
    int off = 0;
    off = fill_op_range(off, 256, _jp_s);
    off = fill_op_range(off, 256, _retd_l);
    off = fill_op_range(off, 256, _jp_c_s);
    off = fill_op_range(off, 256, _jp_nc_s);
    off = fill_op_range(off, 256, _call_s);
    off = fill_op_range(off, 256, _calz_s);
    off = fill_op_range(off, 256, _jp_z_s);
    off = fill_op_range(off, 256, _jp_nz_s);
    off = fill_op_range(off, 256, _ld_y_y);
    off = fill_op_range(off, 256, _lbpx_mx_l);
    off = fill_op_range(off, 16, _adc_xh_i);
    off = fill_op_range(off, 16, _adc_xl_i);
    off = fill_op_range(off, 16, _adc_yh_i);
    off = fill_op_range(off, 16, _adc_yl_i);
    off = fill_op_range(off, 16, _cp_xh_i);
    off = fill_op_range(off, 16, _cp_xl_i);
    off = fill_op_range(off, 16, _cp_yh_i);
    off = fill_op_range(off, 16, _cp_yl_i);
    off = fill_op_range(off, 16, _add_r_q);
    off = fill_op_range(off, 16, _adc_r_q);
    off = fill_op_range(off, 16, _sub_r_q);
    off = fill_op_range(off, 16, _sbc_r_q);
    off = fill_op_range(off, 16, _and_r_q);
    off = fill_op_range(off, 16, _or_r_q);
    off = fill_op_range(off, 16, _xor_r_q);
    off = fill_op_range(off, 16, _rlc_r);
    off = fill_op_range(off, 256, _ld_x_x);
    off = fill_op_range(off, 64, _add_r_i);
    off = fill_op_range(off, 64, _adc_r_i);
    off = fill_op_range(off, 64, _and_r_i);
    off = fill_op_range(off, 64, _or_r_i);
    off = fill_op_range(off, 64, _xor_r_i);
    off = fill_op_range(off, 64, _sbc_r_i);
    off = fill_op_range(off, 64, _fan_r_i);
    off = fill_op_range(off, 64, _cp_r_i);
    off = fill_op_range(off, 64, _ld_r_i);
    off = fill_op_range(off, 32, _pset_p);
    off = fill_op_range(off, 16, _ldpx_mx_i);
    off = fill_op_range(off, 16, _ldpy_my_i);
    off = fill_op_range(off, 4, _ld_xp_r);
    off = fill_op_range(off, 4, _ld_xh_r);
    off = fill_op_range(off, 4, _ld_xl_r);
    off = fill_op_range(off, 4, _rrc_r);
    off = fill_op_range(off, 4, _ld_yp_r);
    off = fill_op_range(off, 4, _ld_yh_r);
    off = fill_op_range(off, 4, _ld_yl_r);
    off = fill_op_range(off, 4, _dummy);
    off = fill_op_range(off, 4, _ld_r_xp);
    off = fill_op_range(off, 4, _ld_r_xh);
    off = fill_op_range(off, 4, _ld_r_xl);
    off = fill_op_range(off, 4, _dummy);
    off = fill_op_range(off, 4, _ld_r_yp);
    off = fill_op_range(off, 4, _ld_r_yh);
    off = fill_op_range(off, 4, _ld_r_yl);
    off = fill_op_range(off, 4, _dummy);
    off = fill_op_range(off, 16, _ld_r_q);
    off = fill_op_range(off, 16, _dummy);
    off = fill_op_range(off, 16, _ldpx_r_q);
    off = fill_op_range(off, 16, _ldpy_r_q);
    off = fill_op_range(off, 16, _cp_r_q);
    off = fill_op_range(off, 16, _fan_r_q);
    off = fill_op_range(off, 8, _dummy);
    off = fill_op_range(off, 4, _acpx_mx_r);
    off = fill_op_range(off, 4, _acpy_my_r);
    off = fill_op_range(off, 8, _dummy);
    off = fill_op_range(off, 4, _scpx_mx_r);
    off = fill_op_range(off, 4, _scpy_my_r);
    off = fill_op_range(off, 16, _set_f_i);
    off = fill_op_range(off, 16, _rst_f_i);
    off = fill_op_range(off, 16, _inc_mn);
    off = fill_op_range(off, 16, _dec_mn);
    off = fill_op_range(off, 16, _ld_mn_a);
    off = fill_op_range(off, 16, _ld_mn_b);
    off = fill_op_range(off, 16, _ld_a_mn);
    off = fill_op_range(off, 16, _ld_b_mn);
    off = fill_op_range(off, 4, _push_r);
    off = fill_op_range(off, 1, _push_xp);
    off = fill_op_range(off, 1, _push_xh);
    off = fill_op_range(off, 1, _push_xl);
    off = fill_op_range(off, 1, _push_yp);
    off = fill_op_range(off, 1, _push_yh);
    off = fill_op_range(off, 1, _push_yl);
    off = fill_op_range(off, 1, _push_f);
    off = fill_op_range(off, 1, _dec_sp);
    off = fill_op_range(off, 4, _dummy);
    off = fill_op_range(off, 4, _pop_r);
    off = fill_op_range(off, 1, _pop_xp);
    off = fill_op_range(off, 1, _pop_xh);
    off = fill_op_range(off, 1, _pop_xl);
    off = fill_op_range(off, 1, _pop_yp);
    off = fill_op_range(off, 1, _pop_yh);
    off = fill_op_range(off, 1, _pop_yl);
    off = fill_op_range(off, 1, _pop_f);
    off = fill_op_range(off, 1, _inc_sp);
    off = fill_op_range(off, 2, _dummy);
    off = fill_op_range(off, 1, _rets);
    off = fill_op_range(off, 1, _ret);
    off = fill_op_range(off, 4, _ld_sph_r);
    off = fill_op_range(off, 4, _ld_r_sph);
    off = fill_op_range(off, 1, _jpba);
    off = fill_op_range(off, 7, _dummy);
    off = fill_op_range(off, 4, _ld_spl_r);
    off = fill_op_range(off, 4, _ld_r_spl);
    off = fill_op_range(off, 1, _halt);
    off = fill_op_range(off, 2, _dummy);
    off = fill_op_range(off, 1, _nop5);
    off = fill_op_range(off, 3, _dummy);
    fill_op_range(off, 1, _nop7);
}

/* ------------------------------------------------------------------ */
/* IO table construction                                              */
/* ------------------------------------------------------------------ */
static void build_io_tables(void) {
    int i;
    for (i = 0; i < IORAM_SIZE; i++) {
        _io_get[i] = _get_io_dummy;
        _io_set[i] = _set_io_dummy;
    }
    _io_get[0x00] = _get_io_it;      _io_set[0x00] = _set_io_dummy;
    _io_get[0x01] = _get_io_isw;     _io_set[0x01] = _set_io_dummy;
    _io_get[0x02] = _get_io_ipt;     _io_set[0x02] = _set_io_dummy;
    _io_get[0x03] = _get_io_isio;    _io_set[0x03] = _set_io_dummy;
    _io_get[0x04] = _get_io_ik0;     _io_set[0x04] = _set_io_dummy;
    _io_get[0x05] = _get_io_ik1;     _io_set[0x05] = _set_io_dummy;
    _io_get[0x10] = _get_io_eit;     _io_set[0x10] = _set_io_eit;
    _io_get[0x11] = _get_io_eisw;    _io_set[0x11] = _set_io_eisw;
    _io_get[0x12] = _get_io_eipt;    _io_set[0x12] = _set_io_eipt;
    _io_get[0x13] = _get_io_eisio;   _io_set[0x13] = _set_io_eisio;
    _io_get[0x14] = _get_io_eik0;    _io_set[0x14] = _set_io_eik0;
    _io_get[0x15] = _get_io_eik1;    _io_set[0x15] = _set_io_eik1;
    _io_get[0x20] = _get_io_tm30;    _io_set[0x20] = _set_io_dummy;
    _io_get[0x21] = _get_io_tm74;    _io_set[0x21] = _set_io_dummy;
    _io_get[0x22] = _get_io_swl;     _io_set[0x22] = _set_io_dummy;
    _io_get[0x23] = _get_io_swh;     _io_set[0x23] = _set_io_dummy;
    _io_get[0x24] = _get_io_pt30;    _io_set[0x24] = _set_io_dummy;
    _io_get[0x25] = _get_io_pt74;    _io_set[0x25] = _set_io_dummy;
    _io_get[0x26] = _get_io_rd30;    _io_set[0x26] = _set_io_rd30;
    _io_get[0x27] = _get_io_rd74;    _io_set[0x27] = _set_io_rd74;
    _io_get[0x30] = _get_io_sd30;    _io_set[0x30] = _set_io_sd30;
    _io_get[0x31] = _get_io_sd74;    _io_set[0x31] = _set_io_sd74;
    _io_get[0x40] = _get_io_k0;      _io_set[0x40] = _set_io_dummy;
    _io_get[0x41] = _get_io_dfk0;    _io_set[0x41] = _set_io_dfk0;
    _io_get[0x42] = _get_io_k1;      _io_set[0x42] = _set_io_dummy;
    _io_get[0x50] = _get_io_r0;      _io_set[0x50] = _set_io_r0;
    _io_get[0x51] = _get_io_r1;      _io_set[0x51] = _set_io_r1;
    _io_get[0x52] = _get_io_r2;      _io_set[0x52] = _set_io_r2;
    _io_get[0x53] = _get_io_r3;      _io_set[0x53] = _set_io_r3;
    _io_get[0x54] = _get_io_r4;      _io_set[0x54] = _set_io_r4;
    _io_get[0x60] = _get_io_p0;      _io_set[0x60] = _set_io_p0;
    _io_get[0x61] = _get_io_p1;      _io_set[0x61] = _set_io_p1;
    _io_get[0x62] = _get_io_p2;      _io_set[0x62] = _set_io_p2;
    _io_get[0x63] = _get_io_p3;      _io_set[0x63] = _set_io_p3;
    _io_get[0x70] = _get_io_ctrl_osc; _io_set[0x70] = _set_io_ctrl_osc;
    _io_get[0x71] = _get_io_ctrl_lcd; _io_set[0x71] = _set_io_ctrl_lcd;
    _io_get[0x72] = _get_io_lc;       _io_set[0x72] = _set_io_lc;
    _io_get[0x73] = _get_io_ctrl_svd; _io_set[0x73] = _set_io_dummy;
    _io_get[0x74] = _get_io_ctrl_bz1; _io_set[0x74] = _set_io_ctrl_bz1;
    _io_get[0x75] = _get_io_ctrl_bz2; _io_set[0x75] = _set_io_ctrl_bz2;
    _io_get[0x76] = _get_io_dummy;    _io_set[0x76] = _set_io_ctrl_tm;
    _io_get[0x77] = _get_io_ctrl_sw;  _io_set[0x77] = _set_io_ctrl_sw;
    _io_get[0x78] = _get_io_ctrl_pt;  _io_set[0x78] = _set_io_ctrl_pt;
    _io_get[0x79] = _get_io_ptc;      _io_set[0x79] = _set_io_ptc;
    _io_get[0x7d] = _get_io_ioc;      _io_set[0x7d] = _set_io_ioc;
    _io_get[0x7e] = _get_io_pup;      _io_set[0x7e] = _set_io_pup;
    /* 0x7a, 0x7b remain dummy (matches JS). */
}

/* ------------------------------------------------------------------ */
/* Sub-systems advanced by the OSC1 clock                             */
/* ------------------------------------------------------------------ */
static void _process_ptimer(void) {
    _PT = (_PT - 1) & 0xff;
    if (_PT == 0) {
        _PT = _RD;
        _IPT |= IO_IPT;
    }
    if (_PTC & IO_PTCOUT) {
        _R3 ^= IO_R33;
    }
}

static void _process_stopwatch(void) {
    if (_CTRL_SW & IO_SWRUN) {
        _SWL = (_SWL + 1) % 10;
        if (_SWL == 0) {
            _SWH = (_SWH + 1) % 10;
            _ISW |= IO_ISW1;
            if (_SWH == 0) {
                _ISW |= IO_ISW0;
            }
        }
    }
}

static void _process_timer(void) {
    int new_TM = (_TM + 1) & 0xff;
    if ((new_TM & IO_TM2) < (_TM & IO_TM2)) { _IT |= IO_IT32; }
    if (((new_TM >> 4) & IO_TM4) < ((_TM >> 4) & IO_TM4)) { _IT |= IO_IT8; }
    if (((new_TM >> 4) & IO_TM6) < ((_TM >> 4) & IO_TM6)) { _IT |= IO_IT2; }
    if (((new_TM >> 4) & IO_TM7) < ((_TM >> 4) & IO_TM7)) { _IT |= IO_IT1; }
    _TM = new_TM;
}

static double clock_impl(void) {
    double exec_cycles = 7;

    if (_RESET) {
        return exec_cycles;
    }

    if (!_HALT) {
        _if_delay = 0;
        uint16_t opcode;
        if ((unsigned)_PC < (unsigned)_rom_size) {
            opcode = _ROM[_PC] & 0x0fff;   /* 12-bit opcode */
        } else {
            opcode = 0;
        }
        exec_cycles = _execute[opcode](opcode);
    }

    if (_IF && !_if_delay) {
        if (_IPT & _EIPT) {
            exec_cycles += _interrupt(0xc);
        } else if (_ISIO & _EISIO) {
            exec_cycles += _interrupt(0xa);
        } else if (_IK1) {
            exec_cycles += _interrupt(0x8);
        } else if (_IK0) {
            exec_cycles += _interrupt(0x6);
        } else if (_ISW & _EISW) {
            exec_cycles += _interrupt(0x4);
        } else if (_IT & _EIT) {
            exec_cycles += _interrupt(0x2);
        }
    }

    if (!(_CTRL_OSC & IO_CLKCHG)) {
        _clock_OSC1((int)exec_cycles);
        exec_cycles *= _OSC1_clock_div;
    } else {
        /* IO_CLKCHG mode: CPU runs on high-frequency oscillator; OSC1
         * advances fractionally per CPU cycle. Track accumulated ticks. */
        _OSC1_counter -= exec_cycles;
        while (_OSC1_counter <= 0) {
            _OSC1_counter += _OSC1_clock_div;
            _clock_OSC1(1);
        }
    }

    return exec_cycles;
}

/* Advance all OSC1-driven sub-systems by osc1Ticks ticks. Precondition:
 * osc1Ticks < every clock divisor so each counter fires at most once.
 * (1 in IO_CLKCHG mode, else exec_cycles 5..) */
static void _clock_OSC1(int osc1Ticks) {
    sound_clockBatch(osc1Ticks);

    if ((_PTC & IO_PTC) > 1) {
        _ptimer_counter -= (double)osc1Ticks;
        if (_ptimer_counter <= 0) {
            _ptimer_counter += PTIMER_CLOCK_DIV[_PTC & IO_PTC];
            _process_ptimer();
        }
    }

    _stopwatch_counter -= (double)osc1Ticks;
    if (_stopwatch_counter <= 0) {
        _stopwatch_counter += (double)STOPWATCH_CLOCK_DIV;
        _process_stopwatch();
    }

    _timer_counter -= (double)osc1Ticks;
    if (_timer_counter <= 0) {
        _timer_counter += (double)TIMER_CLOCK_DIV;
        _process_timer();
    }
}

static int _interrupt(int vector) {
    set_mem_impl((_SP - 1) & 0xff, (_PC >> 8) & 0x0f);
    set_mem_impl((_SP - 2) & 0xff, (_PC >> 4) & 0x0f);
    _SP = (_SP - 3) & 0xff;
    set_mem_impl(_SP, _PC & 0x0f);
    _IF = 0;
    _HALT = 0;
    _PC = _NPC = (_NPC & 0x1000) | 0x0100 | vector;
    return 13;
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                     */
/* ------------------------------------------------------------------ */
void initCPU(uint16_t *rom, int rom_size, uint32_t clock,
             vpet_tone_cb tone_cb, void *tone_ctx) {
    int i;

    _ROM = rom;
    _rom_size = rom_size;

    _tone_cb = tone_cb;
    _tone_ctx = tone_ctx;
    sound_init(clock);

    _port_pullup_K0 = 15;
    _port_pullup_K1 = 15;
    _p3_dedicated = 0;

    if (!_buffers_initialised) {
        for (i = 0; i < VRAM_SIZE; i++) {
            _EMPTY_VRAM[i] = 0;
            _FULL_VRAM[i] = 1;
        }
        _buffers_initialised = 1;
    }

    _initRegisters();

    _OSC1_clock_div = (double)clock / OSC1_CLOCK;

    _OSC1_counter = 0;
    _timer_counter = 0;
    _ptimer_counter = 0;
    _stopwatch_counter = 0;

    _if_delay = 0;

    _RESET = 0;

    build_io_tables();

    _get_abmxmy[0] = get_A_impl;
    _get_abmxmy[1] = get_B_impl;
    _get_abmxmy[2] = get_MX_impl;
    _get_abmxmy[3] = get_MY_impl;
    _set_abmxmy[0] = set_A_impl;
    _set_abmxmy[1] = set_B_impl;
    _set_abmxmy[2] = set_MX_impl;
    _set_abmxmy[3] = set_MY_impl;

    build_execute_table();
}

void clockBatch(int n) {
    int i;
    for (i = 0; i < n; i++) {
        clock_impl();
    }
}

/* ------------------------------------------------------------------ */
/* Pins                                                               */
/* ------------------------------------------------------------------ */
static int port_is_k0(const char *port) {
    return port[0] == 'K' && port[1] == '0' && port[2] == '\0';
}
static int port_is_k1(const char *port) {
    return port[0] == 'K' && port[1] == '1' && port[2] == '\0';
}

void pin_set(const char *port, int pin, int level) {
    if (port_is_k0(port)) {
        int new_K0 = (~(1 << pin) & _K0) | (level << pin);

        if (_EIK0 && (_DFK0 >> pin) != level && (_K0 >> pin) != level) {
            _IK0 |= IO_IK0;
        }

        if (pin == 3 && (_PTC & IO_PTC) < 2 &&
            (_DFK0 >> pin) != level && (_K0 >> pin) != level) {
            _process_ptimer();
        }

        _K0 = new_K0;
    }
    if (port_is_k1(port)) {
        int new_K1 = (~(1 << pin) & _K1) | (level << pin);
        if (_EIK1 && level == 0 && (_K1 >> pin) != level) {
            _IK1 |= IO_IK1;
        }
        _K1 = new_K1;
    } else if (port[0] == 'P' && port[1] == '0') {
        if (!(_IOC & IO_IOC0)) {
            _P0 = (~(1 << pin) & _P0) | (level << pin);
        }
    } else if (port[0] == 'P' && port[1] == '1') {
        if (!(_IOC & IO_IOC1)) {
            _P1 = (~(1 << pin) & _P1) | (level << pin);
        }
    } else if (port[0] == 'P' && port[1] == '2') {
        if (!(_IOC & IO_IOC2)) {
            _P2 = (~(1 << pin) & _P2) | (level << pin);
        }
    } else if (port[0] == 'P' && port[1] == '3') {
        if (!(_IOC & IO_IOC3) && !_p3_dedicated) {
            _P3 = (~(1 << pin) & _P3) | (level << pin);
        }
    } else if (port[0] == 'R') {   /* "RES" */
        reset();
        _RESET = 1;
    }
}

void pin_release(const char *port, int pin) {
    if (port_is_k0(port)) {
        int level = (_port_pullup_K0 >> pin) & 0x1;
        int new_K0 = (~(1 << pin) & _K0) | (level << pin);

        if (_EIK0 && (_DFK0 >> pin) != level && (_K0 >> pin) != level) {
            _IK0 |= IO_IK0;
        }

        if (pin == 3 && (_PTC & IO_PTC) < 2 &&
            (_DFK0 >> pin) != level && (_K0 >> pin) != level) {
            _process_ptimer();
        }

        _K0 = new_K0;
    }
    if (port_is_k1(port)) {
        int level = (_port_pullup_K1 >> pin) & 0x1;
        int new_K1 = (~(1 << pin) & _K1) | (level << pin);
        if (_EIK1 && level == 0 && (_K1 >> pin) != level) {
            _IK1 |= IO_IK1;
        }
        _K1 = new_K1;
    } else if (port[0] == 'P' && port[1] == '0') {
        if (!(_IOC & IO_IOC0)) {
            _P0 = (~(1 << pin) & _P0) | (_PUP & IO_PUP0);
        }
    } else if (port[0] == 'P' && port[1] == '1') {
        if (!(_IOC & IO_IOC1)) {
            _P1 = (~(1 << pin) & _P1) | (_PUP & IO_PUP1);
        }
    } else if (port[0] == 'P' && port[1] == '2') {
        if (!(_IOC & IO_IOC2)) {
            _P2 = (~(1 << pin) & _P2) | (_PUP & IO_PUP2);
        }
    } else if (port[0] == 'P' && port[1] == '3') {
        if (!(_IOC & IO_IOC3) && !_p3_dedicated) {
            _P3 = (~(1 << pin) & _P3) | (_PUP & IO_PUP3);
        }
    } else if (port[0] == 'R') {   /* "RES" */
        _RESET = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Exported accessors                                                 */
/* ------------------------------------------------------------------ */
uint16_t pc(void) {
    return _PC & 0x1fff;
}

uint16_t get_NPC(void) { return (uint16_t)_NPC; }
uint8_t  get_SP(void)  { return (uint8_t)_SP; }
uint16_t get_IX(void)  { return _IX; }
uint16_t get_IY(void)  { return _IY; }

const uint16_t *get_ROM(void) { return _ROM; }

uint8_t *get_VRAM(void) {
    if (((_CTRL_LCD & IO_ALOFF) | _RESET)) {
        return _EMPTY_VRAM;
    }
    if (_CTRL_LCD & IO_ALON) {
        return _FULL_VRAM;
    }
    return _VRAM;
}

uint16_t *get_VRAM_words(void) {
    if (((_CTRL_LCD & IO_ALOFF) | _RESET)) {
        return (uint16_t *)(void *)_EMPTY_VRAM;
    }
    if (_CTRL_LCD & IO_ALON) {
        return (uint16_t *)(void *)_FULL_VRAM;
    }
    return (uint16_t *)(void *)_VRAM;
}