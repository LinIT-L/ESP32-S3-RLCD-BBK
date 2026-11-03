/* lav_bench.c — LavaX VM 自动测试与性能基准（测试专用，不参与正式固件）
 *
 * 设计原则：
 *  1. **不复制 VM**：直接链接 components/engines/lavax 的真实 lava.c/main.c/lcd.c/
 *     hardware.c/platform.c，测的就是跑在设备上的那套代码。
 *  2. **不改 VM**：只通过全局符号（pLAVA / lavReset / main_loop）驱动；
 *     不新增任何 VM 侧测试钩子。
 *  3. **结束条件可控**：测试程序末尾放 0xFF（无效系统 opcode），
 *     main_loop 会在这里报错并返回 —— 这是受控退出，不是崩溃。
 *  4. **指令数精确**：程序是线性展开的，条数由 lav_asm 计数，
 *     不依赖计时推算，所以吞吐量可直接算。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lav_asm.h"

/* ---- 直接引用真实 VM 的全局符号（类型用 void* 规避内部头依赖） ---- */
extern void  *pLAVA;                 /* byte *pLAVA */
extern void   lavReset(void);
extern void   main_loop(void);
extern size_t g_vm_ram_size;
extern long   result;                /* pop_val 的结果 */
extern int    RamBits;
extern void  *lRam;
extern void  *VRam;

/* 生命周期入口（lavax 组件 main.c 提供） */
extern size_t lavaxvm_vm_ram_size(void);
extern size_t lavaxvm_local_ram_size(void);
extern bool   lavaxvm_init(void *vm_ram, size_t vm_ram_size,
                           void *local_ram, size_t local_ram_size);
extern void   lavaxvm_shutdown(void);

/* 测试程序最大字节数 */
#define CODE_CAP   (64 * 1024)

static uint8_t *s_vm_ram;
static uint8_t *s_code;
static bool     s_vm_ready;
static int      s_pass, s_fail;

/* ---------------- 基础设施 ---------------- */

static bool vm_setup(void)
{
    if (s_vm_ready) return true;

    size_t vm_sz = lavaxvm_vm_ram_size();
    size_t lo_sz = lavaxvm_local_ram_size();

    s_vm_ram = (uint8_t *)heap_caps_malloc(vm_sz + lo_sz, MALLOC_CAP_SPIRAM);
    if (!s_vm_ram) { printf("[BENCH] PSRAM 分配失败 (%u B)\n", (unsigned)(vm_sz + lo_sz)); return false; }

    if (!lavaxvm_init(s_vm_ram, vm_sz, s_vm_ram + vm_sz, lo_sz)) {
        printf("[BENCH] lavaxvm_init 失败\n");
        return false;
    }
    s_code = (uint8_t *)heap_caps_malloc(CODE_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_code) { printf("[BENCH] 代码缓冲分配失败\n"); return false; }

    s_vm_ready = true;
    printf("[BENCH] VM ready: vm=%u local=%u code_cap=%u\n",
           (unsigned)vm_sz, (unsigned)lo_sz, (unsigned)CODE_CAP);
    return true;
}

/* 跑一个已构造好的程序：返回耗时(us)，*ops_out 得到执行指令数 */
static int64_t vm_exec(const lav_asm_t *a, unsigned *ops_out)
{
    int64_t t0, t1;

    pLAVA = a->buf;
    lavReset();

    t0 = esp_timer_get_time();
    main_loop();                 /* 直到 0xFF 或退出请求 */
    t1 = esp_timer_get_time();

    if (ops_out) *ops_out = a->ops;
    return t1 - t0;
}

/* ---------------- 用例 ---------------- */

/* 每个用例都构造「计算 → pop_val → 0xFF 结束」的线性程序 */

static void case_arith(void)
{
    lav_asm_t a;
    unsigned ops;
    int64_t us;

    lav_asm_init(&a, s_code, CODE_CAP, 16, 8, 256, 192);
    lav_push_int(&a, 100);
    lav_push_int(&a, 7);
    lav_op(&a, LOP_CAL_MUL);     /* 700 */
    lav_push_int(&a, 3);
    lav_op(&a, LOP_CAL_ADD);     /* 703 */
    lav_push_int(&a, 4);
    lav_op(&a, LOP_CAL_SUB);     /* 699 */
    lav_push_int(&a, 100);
    lav_op(&a, LOP_CAL_MOD);     /* 99 */
    lav_op(&a, LOP_POP_VAL);
    lav_op(&a, 0xFF);

    if (a.overflow) { printf("[FAIL] arith: 代码溢出\n"); s_fail++; return; }
    us = vm_exec(&a, &ops);
    /* 700+3=703, 703-4=699, 699%100=99 */
    bool ok = (result == 99);
    printf("[%s] arith result=%ld expect=99 ops=%u us=%lld\n",
           ok ? "PASS" : "FAIL", result, ops, (long long)us);
    ok ? s_pass++ : s_fail++;
}

static void case_logic(void)
{
    lav_asm_t a;
    unsigned ops;
    int64_t us;

    lav_asm_init(&a, s_code, CODE_CAP, 16, 8, 256, 192);
    lav_push_int(&a, 0xF0);
    lav_push_int(&a, 0x3C);
    lav_op(&a, LOP_CAL_AND);      /* 0x30 */
    lav_push_int(&a, 0x0F);
    lav_op(&a, LOP_CAL_OR);       /* 0x3F */
    lav_push_int(&a, 0xFF);
    lav_op(&a, LOP_CAL_XOR);      /* 0xC0 */
    lav_push_int(&a, 2);
    lav_op(&a, LOP_CAL_LSHIFT);   /* 0x300 */
    lav_op(&a, LOP_POP_VAL);
    lav_op(&a, 0xFF);

    us = vm_exec(&a, &ops);
    bool ok = (result == 0x300);
    printf("[%s] logic result=%ld expect=768 ops=%u us=%lld\n",
           ok ? "PASS" : "FAIL", result, ops, (long long)us);
    ok ? s_pass++ : s_fail++;
}

static void case_compare(void)
{
    lav_asm_t a;
    unsigned ops;
    int64_t us;

    lav_asm_init(&a, s_code, CODE_CAP, 16, 8, 256, 192);
    lav_push_int(&a, 5);
    lav_push_int(&a, 5);
    lav_op(&a, LOP_CAL_EQU);      /* LTRUE = 0xFFFFFFFF */
    lav_push_int(&a, 3);
    lav_push_int(&a, 9);
    lav_op(&a, LOP_CAL_LESS);     /* LTRUE */
    lav_op(&a, LOP_CAL_LAND);     /* LTRUE && LTRUE = LTRUE */
    lav_op(&a, LOP_POP_VAL);
    lav_op(&a, 0xFF);

    us = vm_exec(&a, &ops);
    bool ok = ((unsigned long)result == 0xFFFFFFFFul);
    printf("[%s] compare result=0x%lx expect=0xffffffff ops=%u us=%lld\n",
           ok ? "PASS" : "FAIL", (unsigned long)result, ops, (long long)us);
    ok ? s_pass++ : s_fail++;
}

/* 内存：写 short 再读回（对应 c_let + c_ptr） */
static void case_memory(void)
{
    lav_asm_t a;
    unsigned ops;
    int64_t us;
    const uint32_t ADDR = 0x2100;

    lav_asm_init(&a, s_code, CODE_CAP, 16, 8, 256, 192);
    /* push_aint 会先 pop 一个偏移 → lav_push_addr_const 已自动补 0 */
    lav_push_addr_const(&a, ADDR, 2);   /* 目标地址常量（2 字节宽） */
    lav_push_int(&a, 0x1234);
    lav_op(&a, LOP_C_LET);              /* 写 2 字节 */
    /* 读回低字节：push_aint 压地址，c_ptr 读 byte */
    lav_push_addr_const(&a, ADDR, 2);
    lav_op(&a, LOP_C_PTR);              /* 读 byte → 0x34 */
    lav_op(&a, LOP_POP_VAL);
    lav_op(&a, 0xFF);

    us = vm_exec(&a, &ops);
    bool ok = (result == 0x34);
    printf("[%s] memory result=0x%lx expect=0x34 ops=%u us=%lld\n",
           ok ? "PASS" : "FAIL", (unsigned long)result, ops, (long long)us);
    ok ? s_pass++ : s_fail++;
}

/* 字符串：push_string + strlen */
static void case_string(void)
{
    lav_asm_t a;
    unsigned ops;
    int64_t us;

    lav_asm_init(&a, s_code, CODE_CAP, 16, 8, 256, 192);
    lav_push_string(&a, "HELLO");
    lav_op(&a, LSC(LSC_STRLEN));
    lav_op(&a, LOP_POP_VAL);
    lav_op(&a, 0xFF);

    us = vm_exec(&a, &ops);
    bool ok = (result == 5);
    printf("[%s] string result=%ld expect=5 ops=%u us=%lld\n",
           ok ? "PASS" : "FAIL", result, ops, (long long)us);
    ok ? s_pass++ : s_fail++;
}

/* 边界：读远超 VM RAM 的地址，应返回 0 且不崩 */
static void case_border(void)
{
    lav_asm_t a;
    unsigned ops;
    int64_t us;

    /* 32 位地址模式才能构造 >1MB 的地址 */
    lav_asm_init(&a, s_code, CODE_CAP, 32, 8, 256, 192);
    lav_push_addr_const(&a, 0x800000, 2);  /* 8MB 处，超出 1MB VM RAM */
    lav_op(&a, LOP_C_PTR);
    lav_op(&a, LOP_POP_VAL);
    lav_op(&a, 0xFF);

    us = vm_exec(&a, &ops);
    bool ok = (result == 0);               /* 越界读按 0 处理 */
    printf("[%s] border_oob result=%ld expect=0 ops=%u us=%lld\n",
           ok ? "PASS" : "FAIL", result, ops, (long long)us);
    ok ? s_pass++ : s_fail++;
}

/* 性能：N 组「push/push/add/pop」线性展开，测纯指令吞吐 */
#define PERF_GROUPS  4000
#define PERF_REPS    5

static void case_perf(void)
{
    lav_asm_t a;
    unsigned ops = 0;
    int64_t best = 0;
    int i, r;

    lav_asm_init(&a, s_code, CODE_CAP, 16, 8, 256, 192);
    for (i = 0; i < PERF_GROUPS; i++) {
        lav_push_int(&a, i & 0xff);
        lav_push_int(&a, 1);
        lav_op(&a, LOP_CAL_ADD);
        lav_op(&a, LOP_POP_VAL);
    }
    lav_op(&a, 0xFF);

    if (a.overflow) { printf("[FAIL] perf: 代码溢出 (%u B), 调小 PERF_GROUPS\n", (unsigned)a.len); s_fail++; return; }

    /* 首次执行有 cache 预热/首次 lavReset 的一次性开销，取多轮最小值，
     * 让数字代表稳态吞吐（这也是优化前后唯一可比的量）。 */
    for (r = 0; r < PERF_REPS; r++) {
        int64_t us = vm_exec(&a, &ops);
        if (r == 0 || us < best) best = us;
    }
    double ips = (best > 0) ? (double)ops * 1e6 / (double)best : 0.0;
    printf("[PERF] linear ops=%u us=%lld ips=%.0f bytes=%u reps=%d\n",
           ops, (long long)best, ips, (unsigned)a.len, PERF_REPS);
}

/* 性能：内存访问密集（写 short + 读 byte），用来单独看 lRamRead/Write 路径 */
#define MEM_GROUPS   2000

static void case_perf_mem(void)
{
    lav_asm_t a;
    unsigned ops = 0;
    int64_t best = 0;
    int i, r;

    lav_asm_init(&a, s_code, CODE_CAP, 16, 8, 256, 192);
    for (i = 0; i < MEM_GROUPS; i++) {
        /* 每个循环体必须**严格平衡**：c_let 会把赋值结果留在求值栈上，
         * 不 pop 掉就会每轮净增 4 字节，2000 轮后 eval_top(byte) 回绕、
         * 地址错乱 —— 这是测试程序自身的错误，不是 VM 的。 */
        lav_push_addr_const(&a, 0x2100, 2);      /* push_int 0 + push_aint */
        lav_push_int(&a, i & 0xff);
        lav_op(&a, LOP_C_LET);                   /* 写 2 字节，栈上留结果 */
        lav_op(&a, LOP_POP_VAL);                 /* 清掉它 */
        lav_push_addr_const(&a, 0x2100, 2);
        lav_op(&a, LOP_C_PTR);                   /* 读 1 字节 */
        lav_op(&a, LOP_POP_VAL);
    }
    lav_op(&a, 0xFF);

    if (a.overflow) { printf("[FAIL] perf_mem: 代码溢出\n"); s_fail++; return; }

    for (r = 0; r < PERF_REPS; r++) {
        int64_t us = vm_exec(&a, &ops);
        if (r == 0 || us < best) best = us;
    }
    double ips = (best > 0) ? (double)ops * 1e6 / (double)best : 0.0;
    printf("[PERF] memops ops=%u us=%lld ips=%.0f bytes=%u reps=%d\n",
           ops, (long long)best, ips, (unsigned)a.len, PERF_REPS);
}

/* 性能：控制流（每条 c_jmp 跳到紧邻的下一条，等价 no-op 跳转） */
#define JMP_GROUPS   4000

static void case_perf_jump(void)
{
    lav_asm_t a;
    unsigned ops = 0;
    int64_t best = 0;
    int i, r;

    lav_asm_init(&a, s_code, CODE_CAP, 16, 8, 256, 192);
    for (i = 0; i < JMP_GROUPS; i++) {
        size_t slot = lav_jmp_slot(&a, LOP_C_JMP);
        lav_patch(&a, slot, lav_pos(&a));        /* 目标 = 下一条指令 */
    }
    lav_op(&a, 0xFF);

    if (a.overflow) { printf("[FAIL] perf_jump: 代码溢出\n"); s_fail++; return; }

    for (r = 0; r < PERF_REPS; r++) {
        int64_t us = vm_exec(&a, &ops);
        if (r == 0 || us < best) best = us;
    }
    double ips = (best > 0) ? (double)ops * 1e6 / (double)best : 0.0;
    printf("[PERF] jump ops=%u us=%lld ips=%.0f bytes=%u reps=%d\n",
           ops, (long long)best, ips, (unsigned)a.len, PERF_REPS);
}

/* ---------------- 入口 ---------------- */

void lavax_bench_run(void)
{
    printf("\n===== LavaX VM BENCH START =====\n");
    if (!vm_setup()) { printf("===== BENCH ABORT =====\n"); return; }

    case_arith();
    case_logic();
    case_compare();
    case_memory();
    case_string();
    case_border();
    case_perf();
    case_perf_mem();
    case_perf_jump();

    printf("===== BENCH DONE pass=%d fail=%d =====\n", s_pass, s_fail);
}
