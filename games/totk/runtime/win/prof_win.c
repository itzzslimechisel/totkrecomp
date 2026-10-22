/* prof_win.c - a sampling profiler for the Windows build (diagnostics, off unless TOTK_PROF is set).
 *
 * TOTK_PROF=<start s>,<duration s>[,<output file>]  (default output: totk-prof.txt in the working directory)
 * A background thread suspends every other thread of the process once per millisecond, reads its instruction pointer and
 * resumes it. The output lists, per thread, the sampled addresses with their counts, the module each address belongs to
 * (module base included, so tools/windows/prof_report.py can symbolize the executable's addresses with llvm-symbolizer)
 * or "[anon]" for code outside any module (the JIT code cache). Nothing is allocated while a thread is suspended. */
#include <windows.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROF_MAX_THREADS 512
#define PROF_HASH (1u << 20)

typedef struct { DWORD tid; DWORD64 rip; unsigned count; } ProfSlot;
static ProfSlot *g_slots;
static unsigned g_used;
static double g_start_s = 60, g_dur_s = 20;
static char g_out[MAX_PATH] = "totk-prof.txt";

static void count(DWORD tid, DWORD64 rip) {
    unsigned h = (unsigned)((rip * 0x9E3779B97F4A7C15ull) >> 40) ^ (tid * 2654435761u);
    for (unsigned i = 0; i < PROF_HASH; i++) {
        ProfSlot *s = &g_slots[(h + i) & (PROF_HASH - 1)];
        if (s->count && s->tid == tid && s->rip == rip) { s->count++; return; }
        if (!s->count) { if (g_used > PROF_HASH / 2) return; s->tid = tid; s->rip = rip; s->count = 1; g_used++; return; }
    }
}

#define PROF_DEPTH 20
#define PROF_STACKS (1u << 16)
typedef struct { DWORD tid; unsigned count; int depth; DWORD64 pc[PROF_DEPTH + 1]; } ProfStack;
static ProfStack *g_stacks;
static unsigned g_nstacks;
static DWORD64 g_exe_lo, g_exe_hi;
static void count_stack(DWORD tid, DWORD64 rip, const DWORD64 *chain, int depth) {
    DWORD64 h = rip * 31 + tid;
    for (int i = 0; i < depth; i++) h = h * 0x100000001B3ull ^ chain[i];
    for (unsigned i = 0; i < PROF_STACKS; i++) {
        ProfStack *s = &g_stacks[(unsigned)(h + i) & (PROF_STACKS - 1)];
        if (s->count && s->tid == tid && s->depth == depth + 1 && s->pc[0] == rip && !memcmp(s->pc + 1, chain, depth * sizeof *chain)) { s->count++; return; }
        if (!s->count) {
            if (g_nstacks > PROF_STACKS / 2) return;
            s->tid = tid; s->depth = depth + 1; s->pc[0] = rip; memcpy(s->pc + 1, chain, depth * sizeof *chain); s->count = 1; g_nstacks++; return;
        }
    }
}

typedef struct { DWORD tid; HANDLE h; unsigned samples; char name[64]; } ProfThread;
static ProfThread g_thr[PROF_MAX_THREADS];
static int g_nthr;

static void refresh_threads(DWORD self) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        int known = 0;
        for (int i = 0; i < g_nthr; i++) if (g_thr[i].tid == te.th32ThreadID) { known = 1; break; }
        if (known || g_nthr == PROF_MAX_THREADS) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        ProfThread *t = &g_thr[g_nthr++];
        t->tid = te.th32ThreadID; t->h = h; t->samples = 0; snprintf(t->name, sizeof t->name, "tid%lu", (unsigned long)t->tid);
        PWSTR desc = NULL;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc && desc[0]) WideCharToMultiByte(CP_UTF8, 0, desc, -1, t->name, sizeof t->name, NULL, NULL);
        if (desc) LocalFree(desc);
    }
    CloseHandle(snap);
}

static int module_of(DWORD64 a, char *name, size_t n, DWORD64 *base) {
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)a, &m) || !m) return 0;
    char path[MAX_PATH]; DWORD k = GetModuleFileNameA(m, path, sizeof path);
    const char *s = k ? strrchr(path, '\\') : NULL;
    snprintf(name, n, "%s", s ? s + 1 : (k ? path : "?"));
    *base = (DWORD64)(uintptr_t)m;
    return 1;
}

#define PROF_MAX_MODULES 512
static struct { DWORD64 lo, hi; } g_mods[PROF_MAX_MODULES];
static int g_nmods;
static void refresh_modules(void) {   /* only while no thread is suspended (EnumProcessModules takes the loader lock) */
    HMODULE hm[PROF_MAX_MODULES]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), hm, sizeof hm, &need)) return;
    int n = (int)(need / sizeof *hm); if (n > PROF_MAX_MODULES) n = PROF_MAX_MODULES;
    for (int i = 0; i < n; i++) { MODULEINFO mi; if (GetModuleInformation(GetCurrentProcess(), hm[i], &mi, sizeof mi)) { g_mods[i].lo = (DWORD64)(uintptr_t)mi.lpBaseOfDll; g_mods[i].hi = g_mods[i].lo + mi.SizeOfImage; } }
    g_nmods = n;
}
static int in_image(DWORD64 a) { for (int i = 0; i < g_nmods; i++) if (a >= g_mods[i].lo && a < g_mods[i].hi) return 1; return 0; }

static int cmp_slot(const void *a, const void *b) {
    const ProfSlot *x = a, *y = b;
    if (x->tid != y->tid) return x->tid < y->tid ? -1 : 1;
    return x->count > y->count ? -1 : x->count < y->count;
}

static DWORD WINAPI prof_main(LPVOID arg) {
    (void)arg;
    DWORD self = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);
    Sleep((DWORD)(g_start_s * 1000));
    fprintf(stderr, "[totk-prof] sampling for %.0f s\n", g_dur_s);
    ULONGLONG t0 = GetTickCount64(), last_refresh = 0;
    unsigned rounds = 0;
    while (GetTickCount64() - t0 < (ULONGLONG)(g_dur_s * 1000)) {
        if (GetTickCount64() - last_refresh > 250) { refresh_threads(self); refresh_modules(); last_refresh = GetTickCount64(); }
        for (int i = 0; i < g_nthr; i++) {
            ProfThread *t = &g_thr[i];
            if (!t->h) continue;
            if (SuspendThread(t->h) == (DWORD)-1) { CloseHandle(t->h); t->h = NULL; continue; }
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
            BOOL ok = GetThreadContext(t->h, &c);
            DWORD64 chain[PROF_DEPTH]; int depth = 0;
            if (ok && in_image(c.Rip)) {
                /* call chain through loaded images (their static unwind tables; the walk stops at JIT code, whose dynamic
                   unwind table lookup could wait for a lock the suspended thread holds) */
                CONTEXT u = c;
                while (depth < PROF_DEPTH) {
                    DWORD64 ib = 0; PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(u.Rip, &ib, NULL);
                    if (!fe) break;
                    PVOID hd; DWORD64 est;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, u.Rip, fe, &u, &hd, &est, NULL);
                    if (!u.Rip) break;
                    chain[depth++] = u.Rip;
                    if (!in_image(u.Rip)) break;
                }
            }
            ResumeThread(t->h);
            if (!ok) continue;
            /* a thread blocked in the kernel sits on a syscall return in ntdll / win32u: counted (as "waiting") like any other */
            count(t->tid, c.Rip); t->samples++;
            if (depth) count_stack(t->tid, c.Rip, chain, depth);
        }
        rounds++;
        Sleep(1);
    }
    timeEndPeriod(1);
    FILE *f = fopen(g_out, "w");
    if (!f) { fprintf(stderr, "[totk-prof] cannot write %s\n", g_out); return 0; }
    fprintf(f, "# totk-prof rounds %u duration %.1f s\n", rounds, (GetTickCount64() - t0) / 1000.0);
    for (int i = 0; i < g_nthr; i++) fprintf(f, "T\t%lu\t%u\t%s\n", (unsigned long)g_thr[i].tid, g_thr[i].samples, g_thr[i].name);
    ProfSlot *all = malloc(sizeof *all * (g_used ? g_used : 1)); unsigned n = 0;
    for (unsigned i = 0; i < PROF_HASH; i++) if (g_slots[i].count) all[n++] = g_slots[i];
    qsort(all, n, sizeof *all, cmp_slot);
    for (unsigned i = 0; i < n; i++) {
        char mod[128] = "[anon]"; DWORD64 base = 0;
        module_of(all[i].rip, mod, sizeof mod, &base);
        fprintf(f, "S\t%lu\t%llx\t%u\t%s\t%llx\n", (unsigned long)all[i].tid, (unsigned long long)all[i].rip, all[i].count, mod, (unsigned long long)base);
    }
    for (int i = 0; i < g_nmods; i++) { char mod[128] = "?"; DWORD64 base = 0; module_of(g_mods[i].lo, mod, sizeof mod, &base); fprintf(f, "M\t%s\t%llx\t%llx\n", mod, (unsigned long long)g_mods[i].lo, (unsigned long long)(g_mods[i].hi - g_mods[i].lo)); }
    for (unsigned i = 0; i < PROF_STACKS; i++) if (g_stacks[i].count) {
        fprintf(f, "K\t%lu\t%u\t", (unsigned long)g_stacks[i].tid, g_stacks[i].count);
        for (int k = 0; k < g_stacks[i].depth; k++) fprintf(f, k ? ",%llx" : "%llx", (unsigned long long)g_stacks[i].pc[k]);
        fputc('\n', f);
    }
    fclose(f); free(all);
    fprintf(stderr, "[totk-prof] wrote %s (%u rounds, %u distinct addresses)\n", g_out, rounds, n);
    return 0;
}

__attribute__((constructor)) static void prof_start(void) {
    const char *e = getenv("TOTK_PROF");
    if (!e || !*e) return;
    char buf[MAX_PATH + 64]; snprintf(buf, sizeof buf, "%s", e);
    char *p = buf, *q;
    g_start_s = strtod(p, &q); if (*q == ',') { p = q + 1; g_dur_s = strtod(p, &q); if (*q == ',') snprintf(g_out, sizeof g_out, "%s", q + 1); }
    if (g_dur_s <= 0) g_dur_s = 20;
    g_slots = calloc(PROF_HASH, sizeof *g_slots); g_stacks = calloc(PROF_STACKS, sizeof *g_stacks);
    if (!g_slots || !g_stacks) return;
    { HMODULE m = GetModuleHandleA(NULL); const IMAGE_DOS_HEADER *d = (const void *)m; const IMAGE_NT_HEADERS *nt = (const void *)((const char *)m + d->e_lfanew);
      g_exe_lo = (DWORD64)(uintptr_t)m; g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage; }
    HANDLE h = CreateThread(NULL, 0, prof_main, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
