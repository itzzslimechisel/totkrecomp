/* cpu_backend.h - guest CPU backend interface (docs/windows-port-plan.md C17-C19).
 *
 * Abstracts how AArch64 guest code runs: natively (macOS / Windows ARM64, rewritten NSOs) or under a JIT
 * (x86-64, dynarmic: cpu_dynarmic.cpp). Guest memory is identity mapped: guest VA == host VA, below 2^39, so host
 * code keeps dereferencing guest pointers directly.
 *
 * Model:
 *  - One cpu_context per guest execution context (a guest thread, or a core when threads are multiplexed on cores).
 *    A context is used by one host thread at a time.
 *  - cpu_run() executes at the context's PC until cpu_request_halt() or an unhandled exception.
 *  - `svc #imm` calls cfg.svc (on the running host thread); the handler reads/writes registers with cpu_get_reg /
 *    cpu_set_reg and may call cpu_request_halt.
 *  - Host traps: guest->host calls. A trap address holds a backend-specific stub (8 bytes, CPU_HOST_TRAP_SIZE); a
 *    guest `blr` to it calls a host C function with the guest's AAPCS64 arguments marshalled to the host ABI and
 *    returns to the guest's LR with the result in x0 / v0.
 *  - cpu_call_guest(): host->guest call, re-entrant (may be called from inside an svc handler or a host trap
 *    function running on the same context).
 *  - TPIDR_EL0 / TPIDRRO_EL0 are per context. CNTPCT_EL0 reads call cfg.get_cntpct (default: host counter scaled
 *    to cfg.cntfrq). LDXR/STXR use a global exclusive monitor shared by all contexts.
 *
 * All functions are C ABI. Return 0 on success / -1 on failure unless stated otherwise. */
#ifndef CPU_BACKEND_H
#define CPU_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cpu_context cpu_context;

/* Register indices for cpu_get_reg / cpu_set_reg. 0..30 are x0..x30. */
enum { CPU_REG_LR = 30, CPU_REG_SP = 31, CPU_REG_PC = 32, CPU_REG_PSTATE = 33, CPU_REG_FPCR = 34, CPU_REG_FPSR = 35 };

/* cpu_run results (bit set). */
enum {
    CPU_HALT_REQUESTED = 1, /* cpu_request_halt() */
    CPU_HALT_EXCEPTION = 2, /* undefined instruction / breakpoint etc. not consumed by cfg.exception */
    CPU_HALT_RETURNED = 4,  /* guest branched to cpu_return_address() (e.g. a thread entry returned) */
};

/* Exception kinds passed to cfg.exception. */
enum {
    CPU_EXC_UNDEFINED = 0,
    CPU_EXC_UNPREDICTABLE = 1,
    CPU_EXC_BREAKPOINT = 2,
    CPU_EXC_HINT = 3, /* WFI/WFE/SEV/YIELD: default handler ignores them */
    CPU_EXC_OTHER = 4,
};

typedef struct cpu_backend_config {
    uint32_t max_contexts;  /* exclusive monitor size: max simultaneously existing contexts (0 -> 64) */
    uint64_t cntfrq;        /* CNTFRQ_EL0 (0 -> 19200000, Horizon) */
    void *user;             /* passed back to the callbacks below */
    /* svc #imm from guest code. Required. */
    void (*svc)(cpu_context *ctx, uint32_t imm, void *user);
    /* CNTPCT_EL0. NULL -> host_counter() scaled to cntfrq. */
    uint64_t (*get_cntpct)(void *user);
    /* Guest exception at pc. Return nonzero if handled (execution continues at the context's PC, which the
     * handler may change); 0 halts cpu_run with CPU_HALT_EXCEPTION. NULL -> hints ignored, others halt. */
    int (*exception)(cpu_context *ctx, uint64_t pc, int kind, void *user);
    /* Guest data abort: a load/store hit unmapped or protected memory (the access did nothing; a load read 0).
     * Return nonzero after fixing the context (registers / PC) to continue; 0 halts cpu_run with CPU_HALT_EXCEPTION.
     * pc and the registers are exact only with precise_aborts (costs speed: every access checks for an abort). */
    int (*data_abort)(cpu_context *ctx, uint64_t pc, uint64_t addr, int is_write, void *user);
    int precise_aborts;
    /* Floating-point accuracy: 0 exact, 1 "auto" (NaN payloads / ASIMD standard-FPCR rounding not bit-exact), 2 fast
     * (also approximate reciprocal estimates, no global exclusive monitor). */
    int fp_accuracy;
} cpu_backend_config;

int cpu_backend_init(const cpu_backend_config *cfg);
void cpu_backend_shutdown(void); /* all contexts must be destroyed first */
const char *cpu_backend_name(void);

/* processor_id: 0..max_contexts-1, unique among live contexts (exclusive monitor slot). */
cpu_context *cpu_context_create(uint32_t processor_id, void *user);
void cpu_context_destroy(cpu_context *ctx);
void *cpu_context_user(cpu_context *ctx);
cpu_context *cpu_current(void); /* context currently executing guest code on this host thread, or NULL */

uint64_t cpu_get_reg(cpu_context *ctx, int idx);
void cpu_set_reg(cpu_context *ctx, int idx, uint64_t v);
void cpu_get_vreg(cpu_context *ctx, int idx, uint64_t out[2]); /* q0..q31: out[0] = low 64 bits */
void cpu_set_vreg(cpu_context *ctx, int idx, const uint64_t v[2]);
uint64_t cpu_get_tpidr(cpu_context *ctx);
void cpu_set_tpidr(cpu_context *ctx, uint64_t v);
uint64_t cpu_get_tpidrro(cpu_context *ctx);
void cpu_set_tpidrro(cpu_context *ctx, uint64_t v);
void cpu_clear_exclusive(cpu_context *ctx);
/* The last data abort of ctx: returns 1 and its address / direction, 0 if none happened. */
int cpu_last_abort(cpu_context *ctx, uint64_t *addr, int *is_write);

/* Run at the current PC. Returns CPU_HALT_* bits. */
int cpu_run(cpu_context *ctx);
/* Ask a running (or the next) cpu_run of ctx to return CPU_HALT_REQUESTED. Thread safe. */
void cpu_request_halt(cpu_context *ctx);

/* Call guest function `fn` with up to 8 integer/pointer args in x0..x7; returns x0. Uses the context's current
 * SP (below any active frame). Re-entrant. The caller's guest register state is preserved. *ok (optional) is
 * set to 0 if the call ended with an exception instead of returning. */
uint64_t cpu_call_guest(cpu_context *ctx, uint64_t fn, const uint64_t *args, int nargs, int *ok);

/* Like cpu_call_guest with the whole integer state: runs guest code at pc with x0..x30 (x30 is replaced by the return
 * sentinel), SP and NZCV (pstate bits) from the arguments and the caller's vector / FP state, until it returns; then
 * writes x0..x29, SP and NZCV back and gives the vector / FP state to the caller. Only from inside guest execution
 * (a host trap, an svc handler). Returns 1 if it returned normally. Used by the static recompiler (cpu_static). */
int cpu_call_guest_full(cpu_context *ctx, uint64_t pc, uint64_t regs[31], uint64_t *sp, uint64_t *nzcv);

/* Guest address that ends cpu_call_guest / makes cpu_run return CPU_HALT_RETURNED when branched to (use as LR). */
uint64_t cpu_return_address(void);

/* Host traps. `sig` describes the host function: return type char, ':' then one char per argument.
 *   'v' void (return only), 'i' integer or pointer (any width <= 64 bits), 'f' float, 'd' double.
 * Up to 8 integer and 8 floating-point args (taken from x0..x7 / v0..v7 per AAPCS64), at most 8 args total.
 * Examples: "i:ii" = uint64_t f(uint64_t, uint64_t); "v:" = void f(void); "d:id" = double f(uint64_t, double).
 * Not supported: stack-passed guest args, by-value structs, varargs (wrap them by hand).
 * The host function may call cpu_current() / cpu_call_guest(). */
#define CPU_HOST_TRAP_SIZE 8
/* Write a trap stub at guest address addr (CPU_HOST_TRAP_SIZE bytes, 4-aligned, must be writable now) and bind it.
 * Re-registering an address rebinds it. */
int cpu_register_host_trap(uint64_t addr, void *fn, const char *sig);
/* Allocate a stub in the backend's own trap area and bind it; returns its guest address or 0. */
uint64_t cpu_alloc_host_trap(void *fn, const char *sig);

/* Like cpu_alloc_host_trap, but each call goes to the function *slot holds at that moment (a dispatch table the host
 * may update, as the NVN thunks' slot table). The trap's guest address is cpu_get_reg(ctx, CPU_REG_PC) - 4 inside it. */
uint64_t cpu_alloc_host_trap_indirect(void *const *slot, const char *sig);

/* Guest code at [addr, addr+size) changed (patches, loader): drop translations in every context. Thread safe. */
void cpu_invalidate(uint64_t addr, size_t size);

#ifdef __cplusplus
}
#endif
#endif
