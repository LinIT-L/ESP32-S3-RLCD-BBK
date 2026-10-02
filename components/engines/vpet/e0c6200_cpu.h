/*
 * e0c6200_cpu.h — E0C6200 4-bit CPU core.
 *
 * 1:1 C translation of
 *   third_party/vpet-emu-zepp/utils/cpu.js
 * (which itself is a software emulator of the Epson E0C6200 4-bit
 *  microcontroller used by first-generation virtual pet devices such as
 *  the Tamagotchi P1 / Digimon).
 *
 * ROM format: the caller supplies the program as an array of big-endian
 * 16-bit opcode words (`uint16_t *rom`). `rom[pc]` is the 12-bit opcode
 * fetched at address `pc` (row opcodes are masked to 12 bits internally,
 * matching the JS ROM wrapper which packs bytes as (data[2i]<<8)|data[2i+1]).
 *
 * This is a faithful port: byte order, bit manipulation, timer dividers,
 * carry/boolean flags and the 0xF0F0 VRAM mapping are preserved exactly.
 */
#ifndef E0C6200_CPU_H
#define E0C6200_CPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory map constants (mirror cpu.js). */
#define E0C6200_RAM_SIZE         0x300
#define E0C6200_VRAM_SIZE        0x0a0
#define E0C6200_VRAM_PART_SIZE   0x050
#define E0C6200_VRAM_PART1_OFFSET 0xe00
#define E0C6200_VRAM_PART2_OFFSET 0xe80
#define E0C6200_IORAM_OFFSET     0xf00
#define E0C6200_IORAM_SIZE       0x07f

/* Fixed-size save-state buffer size (SAVE_STATE_SIZE in cpu.js). */
#define E0C6200_SAVE_STATE_SIZE  1013

/*
 * Tone generator call-back. Replaces the JS `sound` module and the
 * `toneGenerator` object. `enable != 0` means "turn the tone on at
 * `freq` Hz"; `enable == 0` means "stop the tone". The `ctx` pointer is
 * an opaque caller context supplied to initCPU().
 */
typedef void (*vpet_tone_cb)(void *ctx, int freq, int enable);

/*
 * Initialise the CPU.
 *   rom      : array of big-endian 16-bit opcode words (as produced by the
 *              JS ROM wrapper).
 *   rom_size : number of uint16_t opcode words in `rom`.
 *   clock    : oscillator frequency (SAR+SRAM single-chip clock) which the
 *              OSC1 sub-systems are derived from (32768 Hz typical).
 *   tone_cb  : tone call-back (may be NULL to disable sound).
 *   tone_ctx : opaque context handed back to `tone_cb`.
 */
void initCPU(uint16_t *rom, int rom_size, uint32_t clock,
             vpet_tone_cb tone_cb, void *tone_ctx);

/* Reset all registers / memory / timers to the power-on state. */
void reset(void);

/* Advance the machine by `n` CPU clock steps. */
void clockBatch(int n);

/* Drive a pin low/high. `port` is "K0"/"K1"/"P0".."P3"/"RES", pin 0..3,
 * level 0 (low) or 1 (high). */
void pin_set(const char *port, int pin, int level);
void pin_release(const char *port, int pin);

/* Current display VRAM (affected by LCD control + reset state). */
uint8_t *get_VRAM(void);
uint16_t *get_VRAM_words(void);

/* Pointer to the ROM array passed to initCPU(). */
const uint16_t *get_ROM(void);

/* Program counter. */
uint16_t pc(void);

/* Raw memory read/write (nibble). 0..0xfff address space. */
int  get_mem(int addr);
void set_mem(int addr, int value);

/* 4-bit register accessors (A/B) and memory-indirect accessors (MX/MY). */
int  get_A(void);
void set_A(int value);
int  get_B(void);
void set_B(int value);
int  get_MX(void);
void set_MX(int value);
int  get_MY(void);
void set_MY(int value);

/* Register peekers. */
uint16_t get_NPC(void);
uint8_t  get_SP(void);
uint16_t get_IX(void);
uint16_t get_IY(void);

/* Save/restore state into a caller-provided fixed-size buffer. */
uint32_t vpet_save_state_size(void); /* returns E0C6200_SAVE_STATE_SIZE */
void     saveState(uint8_t *out);
/* Returns 0 on success; -1 too short, -2 bad magic, -3 bad version. */
int      loadState(const uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* E0C6200_CPU_H */