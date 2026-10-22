/* jit_glue.c - the runtime on the dynarmic CPU backend (x86-64 hosts, TOTK_CPU_JIT). docs/windows-port-plan.md C17-C19.
 *
 * - One cpu_context per host thread that runs guest code (guest threads are 1:1 host threads, threads.c); TPIDRRO_EL0 is
 *   the thread's TLR, TPIDR_EL0 its TLS block, as the rewriter's KTLR / KTLS slots are on the Mac.
 * - svc #n -> runtime_svc with x0..x30 in an array, written back afterwards (the rewritten gates do the same).
 * - Host functions handed to guest code (overrides, observers, callbacks) are host traps (cpu_backend.h); their AAPCS64
 *   arguments are marshalled by signature (jit_host_sig: integer slots unless listed).
 * - Host->guest calls: runtime_guest_call on the calling thread's context (re-entrant). */
#include "runtime.h"
#include "jit_glue.h"
#include "host_mem.h"
#include "host_thread.h"
#include "host_fault.h"
#include "game_version.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONTEXTS 256
#define HOST_STACK_SIZE (512u << 10)   /* guest stack of a host thread that calls guest code */
#define POOL_AT 0x600000000ULL         /* host-owned guest memory: below the modules, inside the guest's ASLR range */
#define POOL_SIZE (1ULL << 30)

int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);   /* loader_win.c */

/* ---------- host-owned guest memory ---------- */
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *pool;
static size_t pool_used;
typedef struct Freed { struct Freed *next; size_t size; } Freed;
static Freed *pool_free;
void *jit_guest_alloc(size_t size) {
    size = (size + 0xffff) & ~(size_t)0xffff;
    pthread_mutex_lock(&pool_lock);
    if (!pool && !(pool = host_mem_reserve((void *)(uintptr_t)POOL_AT, POOL_SIZE, HOST_RESERVE_FIXED))) runtime_fail("reserve host-owned guest memory");
    void *p = NULL;
    for (Freed **f = &pool_free; *f; f = &(*f)->next) if ((*f)->size == size) { p = *f; *f = (*f)->next; break; }
    if (!p) {
        if (pool_used + size > POOL_SIZE) runtime_fail("host-owned guest memory exhausted");
        p = pool + pool_used; pool_used += size;
        if (host_mem_commit(p, size, HOST_PROT_RW)) runtime_fail("commit host-owned guest memory");
    }
    pthread_mutex_unlock(&pool_lock);
    memset(p, 0, size);
    return p;
}
void jit_guest_free(void *p, size_t size) {
    if (!p) return;
    size = (size + 0xffff) & ~(size_t)0xffff;
    Freed *f = p; f->size = size;
    pthread_mutex_lock(&pool_lock); f->next = pool_free; pool_free = f; pthread_mutex_unlock(&pool_lock);
}

/* ---------- contexts ---------- */
typedef struct {
    cpu_context *ctx;
    uint32_t id;
    int exit_requested;
    uint64_t tls, tlr;
    void *stack;   /* host threads only */
} JitThread;
static __thread JitThread *jt;
static pthread_mutex_t ids_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t ids_used[MAX_CONTEXTS];

static JitThread *self(void) {
    if (!jt && !(jt = calloc(1, sizeof *jt))) runtime_fail("JIT thread state");
    return jt;
}
void jit_thread_set_env(uint64_t tls, uint64_t tlr) {
    JitThread *t = self();
    t->tls = tls; t->tlr = tlr;
    if (t->ctx) { cpu_set_tpidr(t->ctx, tls); cpu_set_tpidrro(t->ctx, tlr); }
}
cpu_context *jit_thread_context(void) {
    JitThread *t = self();
    if (t->ctx) return t->ctx;
    pthread_mutex_lock(&ids_lock);
    uint32_t id = 0;
    while (id < MAX_CONTEXTS && ids_used[id]) id++;
    if (id == MAX_CONTEXTS) runtime_fail("too many guest CPU contexts");
    ids_used[id] = 1;
    pthread_mutex_unlock(&ids_lock);
    t->id = id;
    if (!(t->ctx = cpu_context_create(id, t))) runtime_fail("create guest CPU context");
    cpu_set_tpidr(t->ctx, t->tls); cpu_set_tpidrro(t->ctx, t->tlr);
    if (!t->tlr) {   /* a host thread calling guest code: it gets a guest stack and a thread environment of its own */
        t->stack = jit_guest_alloc(HOST_STACK_SIZE);
        cpu_set_reg(t->ctx, CPU_REG_SP, (uint64_t)(uintptr_t)t->stack + HOST_STACK_SIZE - 64);
        runtime_thread_init();
    }
    return t->ctx;
}
void jit_thread_release(void) {
    JitThread *t = jt;
    if (!t) return;
    jt = NULL;
    if (t->ctx) {
        cpu_context_destroy(t->ctx);
        pthread_mutex_lock(&ids_lock); ids_used[t->id] = 0; pthread_mutex_unlock(&ids_lock);
    }
    jit_guest_free(t->stack, HOST_STACK_SIZE);
    free(t);
}
void jit_request_exit(void) {
    JitThread *t = self();
    t->exit_requested = 1;
    if (t->ctx) cpu_request_halt(t->ctx);
}

/* ---------- guest entry points ---------- */
static void describe(uint64_t a, char *out, size_t n) {
    const char *name; uint64_t off;
    if (runtime_module_offset(a, &name, &off)) snprintf(out, n, "%s+%#llx", name, (unsigned long long)off);
    else snprintf(out, n, "%#llx", (unsigned long long)a);
}
void jit_report(cpu_context *ctx, const char *why) {
    char pc[64], lr[64];
    describe(cpu_get_reg(ctx, CPU_REG_PC), pc, sizeof pc);
    describe(cpu_get_reg(ctx, CPU_REG_LR), lr, sizeof lr);
    fprintf(stderr, "[totk-jit] %s: pc=%s lr=%s sp=%#llx thread=%#x\n", why, pc, lr, (unsigned long long)cpu_get_reg(ctx, CPU_REG_SP),
            runtime_thread_current_handle());
    for (int i = 0; i < 31; i += 4) {
        fprintf(stderr, "[totk-jit]  ");
        for (int k = i; k < i + 4 && k < 31; k++) fprintf(stderr, " x%-2d=%016llx", k, (unsigned long long)cpu_get_reg(ctx, k));
        fputc('\n', stderr);
    }
    runtime_trace_guest(cpu_get_reg(ctx, 29), cpu_get_reg(ctx, CPU_REG_LR));
}
/* threads.c: run a new guest thread's entry on this host thread until it leaves through svc ExitThread. */
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp, uint64_t *host_context) {
    (void)host_context;
    cpu_context *ctx = jit_thread_context();
    for (int i = 0; i < 31; i++) cpu_set_reg(ctx, i, 0);
    cpu_set_reg(ctx, 0, arg);
    cpu_set_reg(ctx, CPU_REG_LR, cpu_return_address());
    cpu_set_reg(ctx, CPU_REG_SP, sp);
    cpu_set_reg(ctx, CPU_REG_PC, entry);
    cpu_set_reg(ctx, CPU_REG_PSTATE, 0);
    for (;;) {
        int r = cpu_run(ctx);
        if (jt->exit_requested) return;
        if (r & CPU_HALT_RETURNED) runtime_fail("guest thread entry returned (Horizon requires ExitThread)");
        if (r & CPU_HALT_EXCEPTION) { jit_report(ctx, "guest exception"); runtime_fail("guest exception"); }
    }
}
uint64_t runtime_guest_call(const void *fn, unsigned n, ...) {
    uint64_t args[8] = {0};
    va_list ap;
    va_start(ap, n);
    if (n > 8) runtime_fail("guest call with more than 8 arguments");
    for (unsigned i = 0; i < n; i++) args[i] = va_arg(ap, uint64_t);
    va_end(ap);
    cpu_context *ctx = cpu_current();
    if (!ctx) ctx = jit_thread_context();
    int ok = 0;
    uint64_t r = cpu_call_guest(ctx, (uint64_t)(uintptr_t)fn, args, (int)n, &ok);
    if (!ok) {
        if (jt && jt->exit_requested) runtime_fail("guest thread exited inside a host->guest call");
        jit_report(ctx, "guest exception in a host->guest call");
        runtime_fail("guest exception");
    }
    return r;
}

/* ---------- callbacks ---------- */
static void on_svc(cpu_context *ctx, uint32_t imm, void *user) {
    (void)user;
    uint64_t r[31], before[31];
    for (int i = 0; i < 31; i++) before[i] = r[i] = cpu_get_reg(ctx, i);
    uint64_t pc = cpu_get_reg(ctx, CPU_REG_PC) - 4, off = pc;
    const char *module = "guest";
    runtime_module_offset(pc, &module, &off);
    runtime_svc(imm, r, module, off);
    if (cpu_current() != ctx) return;   /* not expected: the svc left this context */
    for (int i = 0; i < 31; i++) if (r[i] != before[i]) cpu_set_reg(ctx, i, r[i]);
}
static int on_exception(cpu_context *ctx, uint64_t pc, int kind, void *user) {
    (void)user;
    if (kind == CPU_EXC_HINT) return 1;   /* WFE / YIELD / SEV: spin loops keep polling */
    char where[64];
    describe(pc, where, sizeof where);
    uint32_t insn = 0;
    if (pc && pc < (1ULL << 39)) memcpy(&insn, (const void *)(uintptr_t)pc, 4);
    fprintf(stderr, "[totk-jit] guest exception kind %d at %s (instruction %08x)\n", kind, where, insn);
    (void)ctx;
    return 0;
}
/* ---------- fault recovery (loader.c crash(): the same TOTK cases, on the JIT's exact abort state) ---------- */
uint8_t *runtime_module_base(const char *name);
static uint64_t rd64(uint64_t a, int *ok) { uint64_t v = 0; if (host_guarded_read(&v, (const void *)(uintptr_t)a, 8) != 8) *ok = 0; return v; }
#ifndef GAME_BOTW
/* #57: a stale optional target reference (object+0x6b0 -> holder -> target): the callee faults reading the target's id
   at +0x10. Pop the callee's frame (decoded from its prologue) and resume the caller at its "no target" branch. */
static int stale_target_reference(cpu_context *c, uint64_t pc, uint64_t addr) {
    uint8_t *main = runtime_module_base("main");
    const uint32_t *site = (const uint32_t *)(uintptr_t)(main + GAME_ADDR(0x15ecb0c));   /* ldr x21,[x8] */
    uint32_t insn = 0;
    if (site[-2] != 0xf9435a68u || site[0] != 0xf9400115u || (site[1] & 0xff00001fu) != 0xb4000015u || (site[6] & 0xfc000000u) != 0x94000000u) return 0;
    if (cpu_get_reg(c, CPU_REG_LR) != (uintptr_t)(site + 7) || host_guarded_read(&insn, (const void *)(uintptr_t)pc, 4) != 4 || insn != 0xb94012c8u ||
        cpu_get_reg(c, 1) != cpu_get_reg(c, 22) || addr != cpu_get_reg(c, 22) + 0x10) return 0;
    int32_t bl = (int32_t)(site[6] << 6) >> 6, skip = (int32_t)((site[1] >> 5) << 13) >> 13;
    const uint32_t *callee = site + 6 + bl, *resume = site + 1 + skip;
    int64_t frame = 0; unsigned saved[32] = {0}; int nsaved = 0; uint64_t at[32];
    for (const uint32_t *i = callee; i < callee + 12 && (uintptr_t)i < pc; i++) {
        uint32_t w = *i; unsigned rt = w & 31, rt2 = (w >> 10) & 31, rn = (w >> 5) & 31;
        int64_t imm7 = ((int64_t)(int32_t)(((w >> 15) & 0x7f) << 25) >> 25) * 8;
        if ((w & 0xffc00000u) == 0xa9800000u && rn == 31 && rt == 29 && rt2 == 30 && imm7 < 0 && !frame) { frame = -imm7; saved[nsaved] = 29; at[nsaved++] = 0; saved[nsaved] = 30; at[nsaved++] = 8; }
        else if ((w & 0xffc00000u) == 0xa9000000u && rn == 31 && frame && nsaved < 30) { saved[nsaved] = rt; at[nsaved++] = (uint64_t)imm7; saved[nsaved] = rt2; at[nsaved++] = (uint64_t)imm7 + 8; }
        else if ((w & 0xffc00000u) == 0xf9000000u && rn == 31 && frame && nsaved < 31) { saved[nsaved] = rt; at[nsaved++] = ((w >> 10) & 0xfff) * 8; }
    }
    if (!frame || frame > 0x100) return 0;
    uint64_t sp = cpu_get_reg(c, CPU_REG_SP), stack[32];
    if (host_guarded_read(stack, (const void *)(uintptr_t)sp, (size_t)frame) != (size_t)frame) return 0;
    for (int i = 0; i < nsaved; i++) { if (at[i] + 8 > (uint64_t)frame) return 0; cpu_set_reg(c, (int)saved[i], stack[at[i] / 8]); }
    cpu_set_reg(c, CPU_REG_SP, sp + (uint64_t)frame); cpu_set_reg(c, CPU_REG_PC, (uintptr_t)resume);
    uint64_t stale = cpu_get_reg(c, 21), field = cpu_get_reg(c, 19) + 0x6b0;   /* the caller's x19 = the object, x21 = the stale word */
    int ok = 1; uint64_t holder = rd64(field, &ok), word = holder ? rd64(holder, &ok) : 0;
    if (ok && holder && word == stale) __atomic_compare_exchange_n((uint64_t *)(uintptr_t)field, &holder, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    fprintf(stderr, "[totk] ignored a stale optional target reference (#57)\n");
    return 1;
}
/* During save load this optional game object can hold a tagged invalid reference: treat it as absent. */
static int invalid_menu_reference(cpu_context *c, uint64_t pc, uint64_t addr) {
    uint8_t *main = runtime_module_base("main");
    if (pc != (uintptr_t)main + GAME_ADDR(0x15ecb0c) || addr != 0xffff0000ffffffffULL || cpu_get_reg(c, 8) != 0xffff0000ffffffffULL) return 0;
    uint64_t field = cpu_get_reg(c, 19) + 0x6b0, current = 0;
    if (host_guarded_read(&current, (const void *)(uintptr_t)field, 8) != 8 || current != 0xffff0000ffffffffULL) return 0;
    if (!__atomic_compare_exchange_n((uint64_t *)(uintptr_t)field, &current, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 0;
    fprintf(stderr, "[totk] ignored invalid optional menu reference\n");
    cpu_set_reg(c, 8, 0); cpu_set_reg(c, CPU_REG_PC, pc - 4);
    return 1;
}
#endif
static int on_data_abort(cpu_context *ctx, uint64_t pc, uint64_t addr, int is_write, void *user) {
    (void)user;
#ifndef GAME_BOTW
    if (!is_write && (stale_target_reference(ctx, pc, addr) || invalid_menu_reference(ctx, pc, addr))) return 1;
#endif
    if (runtime_gfx_fault((void *)(uintptr_t)addr, is_write, NULL)) return 1;   /* dirty-page tracking: the access re-executes */
    char where[64]; describe(pc, where, sizeof where);
    fprintf(stderr, "[totk-jit] guest data abort: %s %#llx at %s\n", is_write ? "write" : "read", (unsigned long long)addr, where);
    return 0;
}
void jit_init(void) {
    cpu_backend_config cfg = {0};
    cfg.max_contexts = MAX_CONTEXTS;
    cfg.cntfrq = 19200000;
    cfg.svc = on_svc;
    cfg.exception = on_exception;
    cfg.data_abort = on_data_abort;
    { const char *e = getenv("TOTK_JIT_PRECISE_FAULTS"); cfg.precise_aborts = !(e && !strcmp(e, "0")); }   /* exact abort state (fault recovery) */
    { const char *e = getenv("TOTK_JIT_ACCURACY");   /* accurate | auto (default) | fast */
      cfg.fp_accuracy = !e || !strcmp(e, "auto") ? 1 : !strcmp(e, "accurate") ? 0 : !strcmp(e, "fast") ? 2 : 1; }
    if (cpu_backend_init(&cfg)) runtime_fail("CPU backend initialization");
    fprintf(stderr, "[totk] CPU backend: %s\n", cpu_backend_name());
}

/* ---------- host traps ---------- */
/* Marshalling per host function: integer slots x0..x7 -> the host's first 8 arguments, x0 <- the result. Listed here:
   the overrides whose host functions take floating-point arguments (cpu_backend.h signature strings). */
static const struct { const char *name, *sig; } sigs[] = {
    {"_ZN2nn2oe24SetExpectedVolumeBalanceEff", "v:ff"},
};
const char *jit_host_sig(const char *name) {
    if (name) for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) if (!strcmp(name, sigs[i].name)) return sigs[i].sig;
    return "i:iiiiiiii";
}
static pthread_mutex_t trap_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { void *fn; uint64_t addr; } traps[4096];
static unsigned ntraps;
uint64_t jit_host_trap(void *fn, const char *name) {
    if (!fn) return 0;
    pthread_mutex_lock(&trap_lock);
    for (unsigned i = 0; i < ntraps; i++) if (traps[i].fn == fn) { uint64_t a = traps[i].addr; pthread_mutex_unlock(&trap_lock); return a; }
    if (ntraps == sizeof traps / sizeof *traps) runtime_fail("host trap table full");
    uint64_t a = cpu_alloc_host_trap(fn, jit_host_sig(name));
    if (!a) runtime_fail("host trap allocation");
    traps[ntraps].fn = fn; traps[ntraps++].addr = a;
    pthread_mutex_unlock(&trap_lock);
    return a;
}
