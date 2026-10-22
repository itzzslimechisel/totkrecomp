// cpu_dynarmic.cpp - cpu_backend.h on dynarmic's A64 JIT (x86-64 hosts). docs/windows-port-plan.md C17-C19.
//
// Memory: identity mapped. fastmem base 0 with 39 address bits, so JIT code accesses host VA == guest VA directly;
// accesses that fault (or lie above 2^39) fall back to the callbacks below, which also dereference host memory.
// Exclusives: one global Dynarmic::ExclusiveMonitor, processor_id per context.
// Host traps: stub = `svc #0xFFFF; ret`. The svc handler looks up the stub by PC-4, marshals AAPCS64 x0..x7/v0..v7
// to the host ABI (Win64: a varargs call with every slot passed as a double, which the Win64 ABI places in both the
// integer and the vector register of that slot), and writes the result to x0/v0.
// Host->guest calls (cpu_call_guest) are re-entrant: dynarmic's Jit::Run is not, so each nesting depth of a
// context has its own Jit ("level"); a nested call runs on the next level using the paused level's SP.
#include "cpu_backend.h"

#include "host_mem.h"
#include "host_time.h"
#include "host_fault.h"

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Dynarmic::HaltReason;
using Vector = Dynarmic::A64::Vector;

namespace {

constexpr uint32_t kTrapSvc = 0xFFFF;
constexpr uint32_t kInsnSvcTrap = 0xD4000001u | (kTrapSvc << 5);
constexpr uint32_t kInsnRet = 0xD65F03C0u;
constexpr uint64_t kAddrLimit = 1ull << 39;
constexpr size_t kTrapAreaSize = 64 * 1024;

constexpr HaltReason kHaltRequest = HaltReason::UserDefined1;
constexpr HaltReason kHaltReturned = HaltReason::UserDefined2;
constexpr HaltReason kHaltException = HaltReason::UserDefined3;

struct Trap {
    bool is_return = false;
    void *fn = nullptr;
    void *const *slot = nullptr; // indirect traps: the function is read from here at each call
    char ret = 'v';
    char args[8] = {};
    int nargs = 0;
};

struct Backend {
    cpu_backend_config cfg{};
    std::unique_ptr<Dynarmic::ExclusiveMonitor> monitor;
    std::shared_mutex trap_mutex;
    std::unordered_map<uint64_t, Trap> traps;
    uint8_t *trap_area = nullptr;
    size_t trap_next = 0; // bytes used
    uint64_t return_addr = 0;
    std::mutex ctx_mutex;
    std::unordered_set<cpu_context *> contexts;
};
Backend *g_backend;
thread_local cpu_context *t_current;

template <typename T> inline void store_raw(uint64_t a, T v) { std::memcpy(reinterpret_cast<void *>(a), &v, sizeof v); }
template <typename T> inline bool cas(uint64_t a, T value, T expected) {
    return __atomic_compare_exchange_n(reinterpret_cast<T *>(a), &expected, value, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

struct Callbacks;

struct Level {
    std::unique_ptr<Callbacks> cb;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
};

} // namespace

struct cpu_context {
    uint32_t processor_id = 0;
    void *user = nullptr;
    uint64_t tpidr = 0;
    uint64_t tpidrro = 0;
    uint64_t abort_addr = 0; // last data abort
    int abort_write = 0, aborted = 0;
    std::mutex level_mutex; // guards `levels` growth vs. invalidate/halt from other threads
    std::vector<std::unique_ptr<Level>> levels;
    int depth = 0; // active Jit::Run calls on this context

    Dynarmic::A64::Jit *active() { return levels[depth > 0 ? depth - 1 : 0]->jit.get(); }
    Level *level(int i);
};

namespace {

struct Callbacks final : Dynarmic::A64::UserCallbacks {
    cpu_context *ctx;
    Dynarmic::A64::Jit *jit = nullptr;
    explicit Callbacks(cpu_context *c) : ctx(c) {}

    // Slow path (fastmem faults, addresses >= 2^39): guarded, so a guest data abort does not crash the host.
    void abort(uint64_t a, int write) {
        ctx->abort_addr = a;
        ctx->abort_write = write;
        ctx->aborted = 1;
        jit->HaltExecution(HaltReason::MemoryAbort);
    }
    template <typename T> T load(uint64_t a) {
        T v{};
        if (host_guarded_read(&v, reinterpret_cast<const void *>(a), sizeof v) != sizeof v) { abort(a, 0); return T{}; }
        return v;
    }
    template <typename T> void store(uint64_t a, T v) {
        if (host_guarded_write(reinterpret_cast<void *>(a), &v, sizeof v) != sizeof v) abort(a, 1);
    }

    std::optional<uint32_t> MemoryReadCode(uint64_t a) override {
        uint32_t w;
        if (a == 0 || a >= kAddrLimit || (a & 3)) return std::nullopt;
        if (host_guarded_read(&w, reinterpret_cast<const void *>(a), 4) != 4) return std::nullopt;
        return w;
    }
    uint8_t MemoryRead8(uint64_t a) override { return load<uint8_t>(a); }
    uint16_t MemoryRead16(uint64_t a) override { return load<uint16_t>(a); }
    uint32_t MemoryRead32(uint64_t a) override { return load<uint32_t>(a); }
    uint64_t MemoryRead64(uint64_t a) override { return load<uint64_t>(a); }
    Vector MemoryRead128(uint64_t a) override { return {load<uint64_t>(a), load<uint64_t>(a + 8)}; }
    void MemoryWrite8(uint64_t a, uint8_t v) override { store(a, v); }
    void MemoryWrite16(uint64_t a, uint16_t v) override { store(a, v); }
    void MemoryWrite32(uint64_t a, uint32_t v) override { store(a, v); }
    void MemoryWrite64(uint64_t a, uint64_t v) override { store(a, v); }
    void MemoryWrite128(uint64_t a, Vector v) override {
        store(a, v[0]);
        store(a + 8, v[1]);
    }
    bool MemoryWriteExclusive8(uint64_t a, uint8_t v, uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(uint64_t a, uint16_t v, uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(uint64_t a, uint32_t v, uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(uint64_t a, uint64_t v, uint64_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive128(uint64_t a, Vector v, Vector e) override {
        unsigned __int128 nv = (unsigned __int128)v[1] << 64 | v[0], ev = (unsigned __int128)e[1] << 64 | e[0];
        return cas(a, nv, ev);
    }

    void InterpreterFallback(uint64_t pc, size_t) override { raise(pc, CPU_EXC_UNDEFINED); }
    void ExceptionRaised(uint64_t pc, Dynarmic::A64::Exception e) override {
        using E = Dynarmic::A64::Exception;
        int kind;
        switch (e) {
        case E::UnallocatedEncoding:
        case E::ReservedValue: kind = CPU_EXC_UNDEFINED; break;
        case E::UnpredictableInstruction: kind = CPU_EXC_UNPREDICTABLE; break;
        case E::Breakpoint: kind = CPU_EXC_BREAKPOINT; break;
        case E::WaitForInterrupt:
        case E::WaitForEvent:
        case E::SendEvent:
        case E::SendEventLocal:
        case E::Yield: kind = CPU_EXC_HINT; break;
        default: kind = CPU_EXC_OTHER; break;
        }
        raise(pc, kind);
    }
    void raise(uint64_t pc, int kind) {
        const cpu_backend_config &c = g_backend->cfg;
        int handled = c.exception ? c.exception(ctx, pc, kind, c.user) : kind == CPU_EXC_HINT;
        if (!handled) jit->HaltExecution(kHaltException);
    }

    void CallSVC(uint32_t imm) override {
        if (imm == kTrapSvc) {
            Trap t;
            bool found;
            {
                std::shared_lock lk(g_backend->trap_mutex);
                auto it = g_backend->traps.find(jit->GetPC() - 4);
                found = it != g_backend->traps.end();
                if (found) t = it->second;
            }
            if (found) {
                if (t.is_return)
                    jit->HaltExecution(kHaltReturned);
                else
                    call_host(t);
                return;
            }
        }
        g_backend->cfg.svc(ctx, imm, g_backend->cfg.user);
    }

    void call_host(Trap t) {
        if (t.slot) t.fn = __atomic_load_n(t.slot, __ATOMIC_ACQUIRE);
        uint64_t slots[8] = {};
        int ni = 0, nf = 0;
        for (int i = 0; i < t.nargs; i++) {
            if (t.args[i] == 'i')
                slots[i] = jit->GetRegister(ni++);
            else {
                Vector v = jit->GetVector(nf++);
                slots[i] = t.args[i] == 'f' ? (v[0] & 0xFFFFFFFFu) : v[0];
            }
        }
#if defined(_WIN64)
        double d[8];
        std::memcpy(d, slots, sizeof d);
        switch (t.ret) {
        case 'v': reinterpret_cast<void (*)(...)>(t.fn)(d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]); break;
        case 'i':
            jit->SetRegister(0, reinterpret_cast<uint64_t (*)(...)>(t.fn)(d[0], d[1], d[2], d[3], d[4], d[5], d[6],
                                                                           d[7]));
            break;
        case 'f': {
            float r = reinterpret_cast<float (*)(...)>(t.fn)(d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
            uint32_t b;
            std::memcpy(&b, &r, 4);
            jit->SetVector(0, Vector{b, 0});
            break;
        }
        case 'd': {
            double r = reinterpret_cast<double (*)(...)>(t.fn)(d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
            uint64_t b;
            std::memcpy(&b, &r, 8);
            jit->SetVector(0, Vector{b, 0});
            break;
        }
        }
#else
        // SysV: integer-only signatures (enforced at registration).
        using F = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        uint64_t r = reinterpret_cast<F>(t.fn)(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5], slots[6],
                                               slots[7]);
        if (t.ret == 'i') jit->SetRegister(0, r);
#endif
    }

    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return ~0ull >> 1; }
    uint64_t GetCNTPCT() override {
        const cpu_backend_config &c = g_backend->cfg;
        if (c.get_cntpct) return c.get_cntpct(c.user);
        uint64_t t = host_counter(), f = host_counter_freq();
        return (uint64_t)((unsigned __int128)t * c.cntfrq / f);
    }
};

bool parse_sig(const char *sig, Trap &t) {
    if (!sig || !std::strchr("vifd", sig[0]) || sig[1] != ':') return false;
    t.ret = sig[0];
    int ni = 0, nf = 0;
    for (const char *p = sig + 2; *p; p++) {
        if (t.nargs == 8) return false;
        if (*p == 'i')
            ni++;
        else if (*p == 'f' || *p == 'd')
            nf++;
        else
            return false;
        t.args[t.nargs++] = *p;
    }
#if !defined(_WIN64)
    if (nf || t.ret == 'f' || t.ret == 'd') return false;
#endif
    return ni <= 8 && nf <= 8;
}

void bind_trap(uint64_t addr, const Trap &t) {
    std::unique_lock lk(g_backend->trap_mutex);
    g_backend->traps[addr] = t;
}

void write_stub(uint64_t addr) {
    uint32_t code[2] = {kInsnSvcTrap, kInsnRet};
    std::memcpy(reinterpret_cast<void *>(addr), code, sizeof code);
    cpu_invalidate(addr, sizeof code);
}

Dynarmic::A64::UserConfig make_config(cpu_context *ctx, Callbacks *cb, bool nested) {
    Dynarmic::A64::UserConfig uc;
    uc.callbacks = cb;
    uc.processor_id = ctx->processor_id;
    uc.global_monitor = g_backend->monitor.get();
    uc.tpidr_el0 = &ctx->tpidr;
    uc.tpidrro_el0 = &ctx->tpidrro;
    uc.cntfrq_el0 = (uint32_t)g_backend->cfg.cntfrq;
    uc.ctr_el0 = 0x8444c004;
    uc.dczid_el0 = 4;
    uc.fastmem_pointer = uintptr_t{0};
    uc.fastmem_address_space_bits = 39;
    uc.silently_mirror_fastmem = false;
    uc.recompile_on_fastmem_failure = true;
    uc.fastmem_exclusive_access = true;
    uc.recompile_on_exclusive_fastmem_failure = true;
    uc.enable_cycle_counting = false;
    uc.define_unpredictable_behaviour = true;   // e.g. stlxr wzr, xzr, [x] (main+0xcd7548): store, discard the status, as the hardware does
    uc.check_halt_on_memory_access = g_backend->cfg.precise_aborts != 0;
    // Floating-point accuracy (cpu_backend_config.fp_accuracy): 0 accurate (all safe optimizations only), 1 the usual
    // emulator "auto" level (NaN payloads and the ASIMD standard-FPCR rounding are not reproduced bit for bit; the C++
    // fallbacks for them were a visible share of JIT time), 2 also approximate FRECPE / FRSQRTE and ignore the global
    // exclusive monitor.
    if (g_backend->cfg.fp_accuracy >= 1) {
        uc.unsafe_optimizations = true;
        uc.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN | Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
        if (g_backend->cfg.fp_accuracy >= 2)
            uc.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP | Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    }
    // Exclusive loads / stores without the global monitor (the store becomes a host compare-and-swap against the loaded
    // value; an ABA change between them goes unseen, as in other emulators' fast modes). On by default except with
    // TOTK_JIT_ACCURACY=accurate: the game's job system issues thousands of them per second, and the global monitor's
    // lock made gameplay ~38 FPS against ~61 without it (1.0.0 prologue, RTX 3070 / 2026-10-23; a 4 min run without
    // errors). TOTK_JIT_GLOBAL_MONITOR=1 keeps the monitor. TOTK_JIT_REDUCED_FP=1 adds the approximate FRECPE / FRSQRTE of
    // "fast" on its own (no measurable gain).
    { const char *e = getenv("TOTK_JIT_GLOBAL_MONITOR");
      if (g_backend->cfg.fp_accuracy >= 1 && !(e && !strcmp(e, "1"))) { uc.unsafe_optimizations = true; uc.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor; } }
    { const char *e = getenv("TOTK_JIT_REDUCED_FP"); if (e && !strcmp(e, "1")) { uc.unsafe_optimizations = true; uc.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP; } }
    // Reserved, committed on demand (dynarmic BlockOfCode). A full cache is flushed and everything recompiled: with 128 MiB
    // the game's main thread spent ~40 % of its time recompiling during gameplay (TOTK_PROF, 1.4.3).
    uc.code_cache_size = nested ? 64u * 1024 * 1024 : 512u * 1024 * 1024;
    return uc;
}

// Run one level until it returns to the sentinel / halts. nested: ignore (but keep pending on the outer levels)
// external halt requests until the guest function returns.
int run_level(cpu_context *ctx, Dynarmic::A64::Jit *jit, bool nested) {
    for (;;) {
        HaltReason hr = jit->Run();
        if (Dynarmic::Has(hr, HaltReason::MemoryAbort)) {
            jit->ClearHalt(HaltReason::MemoryAbort);
            const cpu_backend_config &c = g_backend->cfg;
            if (!c.data_abort || !c.data_abort(ctx, jit->GetPC(), ctx->abort_addr, ctx->abort_write, c.user))
                return CPU_HALT_EXCEPTION;
            continue;
        }
        if (Dynarmic::Has(hr, kHaltException)) {
            jit->ClearHalt(kHaltException);
            return CPU_HALT_EXCEPTION;
        }
        if (Dynarmic::Has(hr, kHaltReturned)) {
            jit->ClearHalt(kHaltReturned);
            return CPU_HALT_RETURNED;
        }
        if (Dynarmic::Has(hr, kHaltRequest)) {
            jit->ClearHalt(kHaltRequest);
            if (!nested) return CPU_HALT_REQUESTED;
        }
        // CacheInvalidation etc.: dynarmic already handled it, keep running.
    }
}

} // namespace

Level *cpu_context::level(int i) {
    std::lock_guard lk(level_mutex);
    while ((int)levels.size() <= i) {
        auto l = std::make_unique<Level>();
        l->cb = std::make_unique<Callbacks>(this);
        l->jit = std::make_unique<Dynarmic::A64::Jit>(make_config(this, l->cb.get(), !levels.empty()));
        l->cb->jit = l->jit.get();
        levels.push_back(std::move(l));
    }
    return levels[i].get();
}

extern "C" {

int cpu_backend_init(const cpu_backend_config *cfg) {
    if (g_backend || !cfg || !cfg->svc) return -1;
    auto *b = new Backend;
    b->cfg = *cfg;
    if (!b->cfg.max_contexts) b->cfg.max_contexts = 64;
    if (!b->cfg.cntfrq) b->cfg.cntfrq = 19200000;
    b->monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(b->cfg.max_contexts);
    void *area = host_mem_reserve_in_range(1ull << 32, kAddrLimit, kTrapAreaSize, 0);
    if (!area || host_mem_commit(area, kTrapAreaSize, HOST_PROT_RW) != 0) {
        if (area) host_mem_release(area, kTrapAreaSize);
        delete b;
        return -1;
    }
    b->trap_area = static_cast<uint8_t *>(area);
    g_backend = b;
    // First stub: the return sentinel used by cpu_call_guest (svc only; the halt happens in the svc handler).
    b->return_addr = (uint64_t)(uintptr_t)area;
    store_raw<uint32_t>(b->return_addr, kInsnSvcTrap);
    b->trap_next = CPU_HOST_TRAP_SIZE;
    Trap ret;
    ret.is_return = true;
    bind_trap(b->return_addr, ret);
    return 0;
}

void cpu_backend_shutdown(void) {
    if (!g_backend) return;
    host_mem_release(g_backend->trap_area, kTrapAreaSize);
    delete g_backend;
    g_backend = nullptr;
}

const char *cpu_backend_name(void) { return "dynarmic-a64"; }

cpu_context *cpu_context_create(uint32_t processor_id, void *user) {
    if (!g_backend || processor_id >= g_backend->cfg.max_contexts) return nullptr;
    auto *ctx = new cpu_context;
    ctx->processor_id = processor_id;
    ctx->user = user;
    ctx->level(0);
    std::lock_guard lk(g_backend->ctx_mutex);
    g_backend->contexts.insert(ctx);
    return ctx;
}

void cpu_context_destroy(cpu_context *ctx) {
    if (!ctx) return;
    {
        std::lock_guard lk(g_backend->ctx_mutex);
        g_backend->contexts.erase(ctx);
    }
    delete ctx;
}

void *cpu_context_user(cpu_context *ctx) { return ctx->user; }
cpu_context *cpu_current(void) { return t_current; }
uint64_t cpu_return_address(void) { return g_backend ? g_backend->return_addr : 0; }

uint64_t cpu_get_reg(cpu_context *ctx, int idx) {
    auto *j = ctx->active();
    if (idx >= 0 && idx <= 30) return j->GetRegister(idx);
    switch (idx) {
    case CPU_REG_SP: return j->GetSP();
    case CPU_REG_PC: return j->GetPC();
    case CPU_REG_PSTATE: return j->GetPstate();
    case CPU_REG_FPCR: return j->GetFpcr();
    case CPU_REG_FPSR: return j->GetFpsr();
    }
    return 0;
}

void cpu_set_reg(cpu_context *ctx, int idx, uint64_t v) {
    auto *j = ctx->active();
    if (idx >= 0 && idx <= 30) return j->SetRegister(idx, v);
    switch (idx) {
    case CPU_REG_SP: j->SetSP(v); break;
    case CPU_REG_PC: j->SetPC(v); break;
    case CPU_REG_PSTATE: j->SetPstate((uint32_t)v); break;
    case CPU_REG_FPCR: j->SetFpcr((uint32_t)v); break;
    case CPU_REG_FPSR: j->SetFpsr((uint32_t)v); break;
    }
}

void cpu_get_vreg(cpu_context *ctx, int idx, uint64_t out[2]) {
    Vector v = ctx->active()->GetVector(idx);
    out[0] = v[0];
    out[1] = v[1];
}
void cpu_set_vreg(cpu_context *ctx, int idx, const uint64_t v[2]) { ctx->active()->SetVector(idx, Vector{v[0], v[1]}); }
uint64_t cpu_get_tpidr(cpu_context *ctx) { return ctx->tpidr; }
void cpu_set_tpidr(cpu_context *ctx, uint64_t v) { ctx->tpidr = v; }
uint64_t cpu_get_tpidrro(cpu_context *ctx) { return ctx->tpidrro; }
void cpu_set_tpidrro(cpu_context *ctx, uint64_t v) { ctx->tpidrro = v; }
void cpu_clear_exclusive(cpu_context *ctx) { ctx->active()->ClearExclusiveState(); }
int cpu_last_abort(cpu_context *ctx, uint64_t *addr, int *is_write) {
    if (!ctx->aborted) return 0;
    if (addr) *addr = ctx->abort_addr;
    if (is_write) *is_write = ctx->abort_write;
    return 1;
}

int cpu_run(cpu_context *ctx) {
    if (ctx->depth != 0) return CPU_HALT_EXCEPTION; // cpu_run is not re-entrant; use cpu_call_guest
    cpu_context *prev = t_current;
    t_current = ctx;
    ctx->depth = 1;
    int r = run_level(ctx, ctx->levels[0]->jit.get(), false);
    ctx->depth = 0;
    t_current = prev;
    return r;
}

void cpu_request_halt(cpu_context *ctx) {
    std::lock_guard lk(ctx->level_mutex);
    for (auto &l : ctx->levels) l->jit->HaltExecution(kHaltRequest);
}

uint64_t cpu_call_guest(cpu_context *ctx, uint64_t fn, const uint64_t *args, int nargs, int *ok) {
    const int d = ctx->depth;
    Dynarmic::A64::Jit *parent = ctx->active();
    const uint64_t sp = parent->GetSP() & ~15ull;
    const uint32_t fpcr = parent->GetFpcr();
    Dynarmic::A64::Jit *j = ctx->level(d)->jit.get();

    // Level 0 is the context's own (idle) state: preserve it.
    std::array<uint64_t, 31> saved_regs{};
    std::array<Vector, 32> saved_vecs{};
    uint64_t saved_sp = 0, saved_pc = 0;
    uint32_t saved_pstate = 0, saved_fpsr = 0;
    if (d == 0) {
        saved_regs = j->GetRegisters();
        saved_vecs = j->GetVectors();
        saved_sp = j->GetSP();
        saved_pc = j->GetPC();
        saved_pstate = j->GetPstate();
        saved_fpsr = j->GetFpsr();
    }

    std::array<uint64_t, 31> regs{};
    for (int i = 0; i < nargs && i < 8; i++) regs[i] = args[i];
    regs[30] = g_backend->return_addr;
    j->SetRegisters(regs);
    j->SetSP(sp);
    j->SetPC(fn);
    j->SetPstate(0);
    j->SetFpcr(fpcr);
    j->ClearExclusiveState();

    cpu_context *prev = t_current;
    t_current = ctx;
    ctx->depth = d + 1;
    int r = run_level(ctx, j, true);
    ctx->depth = d;
    t_current = prev;
    uint64_t result = j->GetRegister(0);
    if (ok) *ok = r == CPU_HALT_RETURNED;

    if (d == 0) {
        j->SetRegisters(saved_regs);
        j->SetVectors(saved_vecs);
        j->SetSP(saved_sp);
        j->SetPC(saved_pc);
        j->SetPstate(saved_pstate);
        j->SetFpsr(saved_fpsr);
    }
    return result;
}

int cpu_register_host_trap(uint64_t addr, void *fn, const char *sig) {
    Trap t;
    if (!g_backend || !fn || (addr & 3) || !addr || addr >= kAddrLimit || !parse_sig(sig, t)) return -1;
    t.fn = fn;
    bind_trap(addr, t);
    write_stub(addr);
    return 0;
}

static uint64_t alloc_trap(Trap t) {
    uint64_t addr;
    {
        std::unique_lock lk(g_backend->trap_mutex);
        if (g_backend->trap_next + CPU_HOST_TRAP_SIZE > kTrapAreaSize) return 0;
        addr = (uint64_t)(uintptr_t)g_backend->trap_area + g_backend->trap_next;
        g_backend->trap_next += CPU_HOST_TRAP_SIZE;
        g_backend->traps[addr] = t;
    }
    write_stub(addr);
    return addr;
}
uint64_t cpu_alloc_host_trap(void *fn, const char *sig) {
    Trap t;
    if (!g_backend || !fn || !parse_sig(sig, t)) return 0;
    t.fn = fn;
    return alloc_trap(t);
}
uint64_t cpu_alloc_host_trap_indirect(void *const *slot, const char *sig) {
    Trap t;
    if (!g_backend || !slot || !parse_sig(sig, t)) return 0;
    t.slot = slot;
    return alloc_trap(t);
}

void cpu_invalidate(uint64_t addr, size_t size) {
    if (!g_backend) return;
    std::lock_guard lk(g_backend->ctx_mutex);
    for (cpu_context *ctx : g_backend->contexts) {
        std::lock_guard lk2(ctx->level_mutex);
        for (auto &l : ctx->levels) l->jit->InvalidateCacheRange(addr, size);
    }
}

} // extern "C"
