/* Windows x86-64 module loader (docs/windows-port-plan.md C4, C14, C18). The ORIGINAL NSOs of the player's ExeFS run
   under the dynarmic CPU backend (jit_glue.c); the rewriter is not used on x64. Same contract as loader.c on the Mac:
   - rtld, main, subsdk0 and sdk are mapped in Horizon order at fixed addresses below 2^39 (guest VA == host VA);
   - the modules are linked on the host (rtld is mapped but never runs), chosen imports are replaced by host functions,
     bound as host traps (runtime_override / runtime_observer, as loader.c's relocate);
   - the game patches (game_patches*.c) are applied to main before its text is sealed;
   - nn::init::Start runs on the main guest thread with the same host callbacks as loader.c's boot.
   Usage: totk-win <exefs dir> boot   (TOTK_ROMFS etc. as on the Mac; TOTK_HEADLESS: no window). */
#include "runtime.h"
#include "jit_glue.h"
#include "game_patches.h"
#include "game_version.h"
#include "host_mem.h"
#include "host_fault.h"
#include "host_time.h"
#include <CommonCrypto/CommonDigest.h>
#include <windows.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODULES_AT 0x800000000ULL   /* 32 GiB: above the low stack/heap search window of services.c, below the alias region */
#define MAIN_STACK_SIZE 0x100000    /* main.npdm: main thread stack size of 1.4.3 */
#define NM 4                        /* main, subsdk0, sdk (symbol search order, as loader.c) + rtld (mapped, no symbols) */
#define NSEARCH 3

pthread_key_t runtime_keys[8];
__thread sigjmp_buf *runtime_fault_guard;
static Module modules[NM];
static const char *const module_names[NM] = {"main", "subsdk0", "sdk", "rtld"};
static const int map_order[NM] = {3, 0, 1, 2};   /* Horizon: rtld, main, subsdk*, sdk */
static uint64_t main_stack_lo;

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "[totk] FAIL: %s\n", message); fflush(stderr); _exit(1); }
static void need(bool c, const char *m) { if (!c) runtime_fail(m); }
static uint64_t page4k(uint64_t x) { return (x + 0xfff) & ~0xfffULL; }
static uint64_t page16k(uint64_t x) { return (x + 0x3fff) & ~0x3fffULL; }
uint64_t runtime_now_ns(void) { return host_now_ns(); }
void runtime_main_stack(uint64_t *lo, uint64_t *size) { *lo = main_stack_lo; *size = MAIN_STACK_SIZE; }

/* ---------- NSO ---------- */
static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[totk] cannot open %s\n", path); runtime_fail("open NSO"); }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    need(size > 0x100, "NSO size");
    uint8_t *p = malloc((size_t)size);
    need(p && fread(p, 1, (size_t)size, f) == (size_t)size, "read NSO");
    fclose(f); *n = (size_t)size;
    return p;
}
/* LZ4 block format (no frame). Returns the decoded size or -1. */
static long lz4_block(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    const uint8_t *se = s + n; size_t o = 0;
    while (s < se) {
        unsigned token = *s++; size_t lit = token >> 4;
        if (lit == 15) for (unsigned b = 255; b == 255 && s < se;) { b = *s++; lit += b; }
        if (lit > (size_t)(se - s) || lit > cap - o) return -1;
        memcpy(d + o, s, lit); s += lit; o += lit;
        if (s >= se) break;
        if (se - s < 2) return -1;
        size_t off = s[0] | (size_t)s[1] << 8; s += 2;
        size_t len = (token & 15) + 4;
        if ((token & 15) == 15) for (unsigned b = 255; b == 255 && s < se;) { b = *s++; len += b; }
        if (!off || off > o || len > cap - o) return -1;
        for (size_t i = 0; i < len; i++, o++) d[o] = d[o - off];
    }
    return (long)o;
}
static void hex32(const uint8_t d[32], char out[65]) { for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]); }

typedef struct { uint64_t tag[38]; uint64_t gnu_hash; } Dyn;
static void dynamic_of(Module *m, Dyn *dy) {
    memset(dy, 0, sizeof *dy);
    uint32_t mod0; memcpy(&mod0, m->base + 4, 4);
    need(mod0 + 8 < m->key_off && !memcmp(m->base + mod0, "MOD0", 4), "MOD0 header");
    int32_t rel; memcpy(&rel, m->base + mod0 + 4, 4);
    const uint8_t *p = m->base + mod0 + rel;
    for (unsigned i = 0; i < 4096; i++, p += 16) {
        int64_t tag; uint64_t value; memcpy(&tag, p, 8); memcpy(&value, p + 8, 8);
        if (tag == 0) return;
        if (tag >= 0 && tag < 38) dy->tag[tag] = value;
        else if (tag == 0x6ffffef5) dy->gnu_hash = value;
    }
    runtime_fail("unterminated dynamic table");
}

static unsigned hash_name(const char *s) { unsigned h = 2166136261u; for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }
static void make_hash(Module *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->hash = calloc(cap, sizeof(unsigned)); need(m->hash != NULL, "symbol hash allocation"); m->hash_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) if (m->symbols[i].defined) {
        const char *s = m->symbols[i].name; unsigned p = hash_name(s) & m->hash_mask;
        while (m->hash[p] && strcmp(s, m->symbols[m->hash[p]].name)) p = (p + 1) & m->hash_mask;
        if (!m->hash[p]) m->hash[p] = i;
    }
}
static void *module_symbol(Module *m, const char *name) {
    if (!m->hash) return NULL;
    unsigned p = hash_name(name) & m->hash_mask;
    while (m->hash[p]) {
        const Symbol *s = &m->symbols[m->hash[p]];
        if (!strcmp(name, s->name)) return s->value < m->key_off ? m->base + s->value : NULL;
        p = (p + 1) & m->hash_mask;
    }
    return NULL;
}
void *runtime_symbol(const char *name) {
    for (unsigned i = 0; i < NSEARCH; i++) { void *p = module_symbol(&modules[i], name); if (p) return p; }
    return NULL;
}
uint8_t *runtime_module_base(const char *name) { for (unsigned i = 0; i < NM; i++) if (!strcmp(modules[i].name, name)) return modules[i].base; return NULL; }

/* The DT_INIT constructor-state word, decoded from DT_INIT's own load of it (scripts/prepare_runtime.py). */
static uint64_t ctor_flag_of(const Module *m) {
#if defined(TOTK_GAME_V143) || defined(GAME_BOTW160)
    const unsigned reg = 19, load = 20;
#else
    const unsigned reg = 20, load = 24;
#endif
    uint32_t a, w; memcpy(&a, m->base + m->init + 16, 4); memcpy(&w, m->base + m->init + load, 4);
    need((a & 0x9f00001fu) == (0x90000000u | reg) && (w & 0xffc003ffu) == (0xb9400008u | (reg << 5)), "DT_INIT constructor-state load");
    int64_t imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2);
    imm = (imm ^ (1 << 20)) - (1 << 20);
    uint64_t flag = ((m->init + 16) & ~4095ULL) + (uint64_t)(imm << 12) + ((w >> 10) & 0xfff) * 4;
    need(flag >= m->data_off && flag < m->key_off, "DT_INIT constructor state outside data");
    return flag;
}

static void map_module(Module *m, const char *dir, uint64_t at) {
    char path[4096];
    need(snprintf(path, sizeof path, "%s/%s", dir, m->name) < (int)sizeof path, "NSO path");
    size_t n; uint8_t *nso = read_all(path, &n);
    need(!memcmp(nso, "NSO0", 4), "NSO magic");
    uint32_t flags, seg[3][4], comp[3];
    memcpy(&flags, nso + 0xc, 4); memcpy(seg, nso + 0x10, sizeof seg); memcpy(comp, nso + 0x60, sizeof comp);
    uint8_t file_digest[32]; char hex[65];
    CC_SHA256(nso, (CC_LONG)n, file_digest); hex32(file_digest, hex);
    m->digest = strdup(hex);
#ifdef TOTK_GAME_V143
    if (!strcmp(m->name, "main")) need(!strcmp(hex, "e84a90d551fed0824b4b74933b9c6d043c458521363a7cea58b2faca1822dc9e"), "unsupported 1.4.3 main image (ExeFS main SHA-256)");
#endif
    uint64_t image_end = page4k((uint64_t)seg[2][1] + seg[2][2] + seg[2][3]);   /* data + bss */
    m->text_end = seg[0][2]; m->ro_off = seg[1][1]; m->data_off = seg[2][1];
    m->file_size = image_end; m->key_off = m->patch_off = image_end;
    m->size = strcmp(m->name, "main") ? image_end : page16k(image_end) + 0x4000;   /* main: the real-time frame-step page */
    size_t reserve = (m->size + 0xffff) & ~(size_t)0xffff;
    m->base = host_mem_reserve((void *)(uintptr_t)at, reserve, HOST_RESERVE_FIXED);
    need(m->base == (uint8_t *)(uintptr_t)at, "reserve module range below 2^39");
    need(host_mem_commit(m->base, m->size, HOST_PROT_RW) == 0, "commit module");
    for (int i = 0; i < 3; i++) {
        uint32_t off = seg[i][0], mem = seg[i][1], size = seg[i][2];
        need(mem + (uint64_t)size <= image_end, "NSO segment layout");
        if (flags & (1u << i)) {
            need(off + (uint64_t)comp[i] <= n, "NSO compressed segment bounds");
            need(lz4_block(nso + off, comp[i], m->base + mem, size) == (long)size, "NSO segment decompression");
        } else {
            need(off + (uint64_t)size <= n, "NSO segment bounds");
            memcpy(m->base + mem, nso + off, size);
        }
        if (flags & (8u << i)) {
            uint8_t d[32]; CC_SHA256(m->base + mem, size, d);
            need(!memcmp(d, nso + 0xa0 + 32 * i, 32), "NSO segment SHA-256 mismatch");
        }
    }
    Dyn dy; dynamic_of(m, &dy);
    m->init = dy.tag[12]; m->init_array = dy.tag[25]; m->init_size = dy.tag[27];
    if (strcmp(m->name, "rtld")) {
        unsigned nsym;
        if (dy.tag[4]) memcpy(&nsym, m->base + dy.tag[4] + 4, 4);
        else { uint32_t so, ss; memcpy(&so, nso + 0x98, 4); memcpy(&ss, nso + 0x9c, 4); need(m->ro_off + so == dy.tag[6] && ss % 24 == 0, "dynsym extent"); nsym = ss / 24; }
        need(dy.tag[11] == 24 && nsym < 1000000, "dynsym layout");
        Symbol *syms = calloc(nsym, sizeof *syms); need(syms != NULL, "symbol table");
        for (unsigned i = 0; i < nsym; i++) {
            const uint8_t *e = m->base + dy.tag[6] + 24 * (uint64_t)i;
            uint32_t name; uint16_t section; memcpy(&name, e, 4); memcpy(&section, e + 6, 2);
            need(name < dy.tag[10], "symbol name bounds");
            syms[i].name = (const char *)m->base + dy.tag[5] + name;
            memcpy(&syms[i].value, e + 8, 8); memcpy(&syms[i].size, e + 16, 8);
            syms[i].defined = section != 0; syms[i].binding = e[4] >> 4; syms[i].type = e[4] & 15;
        }
        m->symbols = syms; m->nsym = nsym;
        make_hash(m);
        m->ctor_flag = ctor_flag_of(m);
    }
    free(nso);
    fprintf(stderr, "[totk] mapped %s at %p size=%#llx text=%#llx symbols=%u sha256=%.16s\n", m->name, (void *)m->base,
            (unsigned long long)m->size, (unsigned long long)m->text_end, m->nsym, m->digest);
}

/* ---------- linking ---------- */
static void *bind_import(Module *m, const Symbol *s, unsigned kind) {
    void *p = kind == 1026 ? runtime_observer(s->name) : NULL;
    if (!p) p = runtime_override(s->name);
    if (p) return (void *)(uintptr_t)jit_host_trap(p, s->name);   /* host replacements are host traps */
    if (s->defined) return s->value < m->key_off ? m->base + s->value : NULL;
    return runtime_symbol(s->name);
}
static void put64(Module *m, uint64_t where, uint64_t v) {
    need(where >= m->data_off && where + 8 <= m->key_off, "relocation outside the writable image");
    memcpy(m->base + where, &v, 8);
}
static void relocate(Module *m) {
    Dyn dy; dynamic_of(m, &dy);
    const uint64_t tables[2][2] = {{dy.tag[7], dy.tag[8]}, {dy.tag[23], dy.tag[2]}};
    for (int t = 0; t < 2; t++) {
        need(tables[t][1] % 24 == 0, "RELA table size");
        for (uint64_t off = tables[t][0]; off < tables[t][0] + tables[t][1]; off += 24) {
            uint64_t where, info; int64_t addend;
            memcpy(&where, m->base + off, 8); memcpy(&info, m->base + off + 8, 8); memcpy(&addend, m->base + off + 16, 8);
            unsigned kind = (unsigned)info, si = (unsigned)(info >> 32);
            if (kind == 1027) { need(addend >= 0 && (uint64_t)addend < m->key_off, "relative addend"); put64(m, where, (uintptr_t)m->base + (uint64_t)addend); m->relative++; continue; }
            need(kind == 257 || kind == 1025 || kind == 1026, "relocation kind");
            need(m->symbols && si < m->nsym, "relocation symbol");
            const Symbol *s = &m->symbols[si];
            void *p = bind_import(m, s, kind);
            if (!p && s->binding != 2) { fprintf(stderr, "%s unresolved %s\n", m->name, s->name); runtime_fail("strong import unresolved"); }
            if (!p) m->weak++;
            if (p && !s->defined) m->cross++;
            put64(m, where, p ? (uintptr_t)p + (kind == 1026 ? 0 : (uint64_t)addend) : 0);
            m->symbolic++;
        }
    }
    if (dy.tag[36]) {   /* DT_RELR */
        need(dy.tag[37] == 8 && dy.tag[35] % 8 == 0, "RELR tags");
        uint64_t cursor = 0; int have = 0;
        for (uint64_t off = dy.tag[36]; off < dy.tag[36] + dy.tag[35]; off += 8) {
            uint64_t e; memcpy(&e, m->base + off, 8);
            if (!(e & 1)) { uint64_t v; memcpy(&v, m->base + e, 8); put64(m, e, (uintptr_t)m->base + v); cursor = e + 8; have = 1; m->relative++; continue; }
            need(have, "RELR bitmap before an address");
            for (int bit = 1; bit < 64; bit++) if (e >> bit & 1) {
                uint64_t w = cursor + (uint64_t)(bit - 1) * 8, v; memcpy(&v, m->base + w, 8); put64(m, w, (uintptr_t)m->base + v); m->relative++;
            }
            cursor += 63 * 8;
        }
    }
}
static void seal(Module *m) {
    uint64_t text = page4k(m->text_end);
    need(host_mem_protect(m->base, text, HOST_PROT_R) == 0, "text R");   /* read by the JIT, never executed by the host */
    if (m->data_off > text) need(host_mem_protect(m->base + text, m->data_off - text, HOST_PROT_R) == 0, "rodata R");
    cpu_invalidate((uint64_t)(uintptr_t)m->base, m->size);
}

/* ---------- Horizon memory queries on modules (services.c) ---------- */
static pthread_mutex_t module_permission_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *module_permissions[NM];
static unsigned module_page_permission(unsigned i, uint64_t off) {
    Module *m = &modules[i];
    if (off < m->data_off) return off < page4k(m->text_end) ? 5 : 1;
    if (off >= m->key_off) return 3;
    return module_permissions[i] ? module_permissions[i][(off - m->data_off) / 4096] : 3;
}
static unsigned char *permission_table(unsigned i) {
    Module *m = &modules[i];
    if (!module_permissions[i]) {
        size_t count = (m->key_off - m->data_off + 4095) / 4096;
        module_permissions[i] = malloc(count); need(module_permissions[i] != NULL, "module permission table");
        memset(module_permissions[i], 3, count);
    }
    return module_permissions[i];
}
uint64_t runtime_module_permission(uint64_t address, uint64_t size, unsigned permission) {
    if (address & 4095) return 0xcc01;
    if (!size || (size & 4095)) return 0xca01;
    if (permission != 0 && permission != 1 && permission != 3) return 0xd801;
    for (unsigned i = 0; i < NM; i++) {
        Module *m = &modules[i]; uint64_t base = (uintptr_t)m->base;
        if (address < base || address - base < m->data_off || address - base >= m->key_off || size > m->key_off - (address - base)) continue;
        uint64_t off = address - base;
        pthread_mutex_lock(&module_permission_lock);
        unsigned char *t = permission_table(i);
        for (uint64_t p = 0; p < size / 4096; p++) if (t[(off - m->data_off) / 4096 + p] & 16) { pthread_mutex_unlock(&module_permission_lock); return 0xd401; }
        memset(t + (off - m->data_off) / 4096, (int)permission, size / 4096);
        int prot = permission == 3 ? HOST_PROT_RW : permission == 1 ? HOST_PROT_R : HOST_PROT_NONE;
        need(host_mem_protect(m->base + off, size, prot) == 0, "module data permission");
        pthread_mutex_unlock(&module_permission_lock);
        fprintf(stderr, "[totk] %s data permission +%#llx size=%#llx perm=%u\n", m->name, (unsigned long long)off, (unsigned long long)size, permission);
        return 0;
    }
    return 0xd401;
}
uint64_t runtime_module_lock_permission(uint64_t address, uint64_t size, unsigned mask, unsigned attribute) {
    if (address & 4095) return 0xcc01;
    if (!size || (size & 4095)) return 0xca01;
    if (mask != 16 || attribute != 16) return 0xd801;   /* the SDK's only use: the irreversible RELRO lock */
    for (unsigned i = 0; i < NM; i++) {
        Module *m = &modules[i]; uint64_t start = (uintptr_t)m->base;
        if (address < start || address - start < m->data_off || address - start >= m->key_off || size > m->key_off - (address - start)) continue;
        pthread_mutex_lock(&module_permission_lock);
        unsigned char *t = permission_table(i);
        size_t first = (address - start - m->data_off) / 4096;
        for (uint64_t p = 0; p < size / 4096; p++) t[first + p] |= 16;
        pthread_mutex_unlock(&module_permission_lock);
        return 0;
    }
    return 0xd401;
}
int runtime_module_query(uint64_t address, uint64_t *base, uint64_t *size, unsigned *state, unsigned *permission, unsigned *attribute) {
    for (unsigned i = 0; i < NM; i++) {
        Module *m = &modules[i]; uint64_t start = (uintptr_t)m->base;
        if (!m->base || address < start || address - start >= m->key_off) continue;
        uint64_t off = (address - start) & ~4095ULL, lo = off, hi = off + 4096;
        pthread_mutex_lock(&module_permission_lock);
        unsigned perm = module_page_permission(i, off); int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->key_off : m->data_off;
        while (lo > limit && module_page_permission(i, lo - 4096) == perm) lo -= 4096;
        while (hi < end && module_page_permission(i, hi) == perm) hi += 4096;
        pthread_mutex_unlock(&module_permission_lock);
        *base = start + lo; *size = hi - lo; *state = data ? 4 : 3; *permission = perm & 7; *attribute = perm & 16;
        return 1;
    }
    return 0;
}
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off) {
    for (unsigned i = 0; i < NM; i++)
        if (modules[i].base && pc >= (uintptr_t)modules[i].base && pc < (uintptr_t)modules[i].base + modules[i].size) { *name = modules[i].name; *off = pc - (uintptr_t)modules[i].base; return 1; }
    return 0;
}
void runtime_describe_address(uint64_t a, char *out, size_t n) {
    const char *name; uint64_t off;
    if (runtime_module_offset(a, &name, &off)) snprintf(out, n, "%s+0x%llx", name, (unsigned long long)off);
    else snprintf(out, n, "0x%llx", (unsigned long long)a);
}
/* Guest frame-pointer chain; frames read with guarded reads (a bad pointer ends the walk). */
void runtime_trace_guest(uint64_t fp, uint64_t lr) {
    for (unsigned n = 0; n < 16; n++) {
        const char *name; uint64_t off;
        if (runtime_module_offset(lr, &name, &off)) fprintf(stderr, "[totk] caller %s+0x%llx\n", name, (unsigned long long)off);
        if (!fp || fp % 8) break;
        uint64_t f[2];
        if (host_guarded_read(f, (const void *)(uintptr_t)fp, 16) != 16) break;
        lr = f[1]; if (f[0] <= fp || f[0] - fp > (64u << 20)) break; fp = f[0];
    }
}
uint64_t runtime_image_bytes(void) { uint64_t total = 0; for (unsigned i = 0; i < NM; i++) total += modules[i].key_off; return page16k(total); }
uintptr_t runtime_frame_controller_return_pc(void) { return 0; }   /* world survey: Mac diagnostics only */
void runtime_startup_checkpoint(uintptr_t caller) { (void)caller; }   /* TOTK_VERIFY_STARTUP: Mac probe only */

/* ---------- threads ---------- */
typedef struct { void *tls, *tlr, *dummy; } ThreadEnvironment;
static pthread_key_t environment_key;
#define ENV_BYTES 0x4000   /* TLS 8 KiB, TLR 0x200, a dummy ThreadType 0x400 */
static void environment_destroy(void *p) { if (p) jit_guest_free(((ThreadEnvironment *)p)->tls, ENV_BYTES), free(p); }
void runtime_thread_init(void) {
    need(!pthread_getspecific(environment_key), "duplicate guest thread environment");
    ThreadEnvironment *e = calloc(1, sizeof *e); need(e != NULL, "thread environment");
    uint8_t *block = jit_guest_alloc(ENV_BYTES);   /* guest-visible: below 2^39 */
    e->tls = block; e->tlr = block + 0x2000; e->dummy = block + 0x2400;
    memcpy((uint8_t *)e->tlr + 0x1f8, &e->dummy, 8);
    need(pthread_setspecific(environment_key, e) == 0 && pthread_setspecific(runtime_keys[0], NULL) == 0, "thread ownership");
    need(pthread_setspecific(runtime_keys[1], e->tls) == 0 && pthread_setspecific(runtime_keys[2], e->tlr) == 0, "thread slots");
    jit_thread_set_env((uintptr_t)e->tls, (uintptr_t)e->tlr);
}

/* ---------- crashes ---------- */
/* Crash report with no CRT locks (the faulting thread may hold stderr's): snprintf into a stack buffer, WriteFile. */
static void raw_err(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    DWORD w; if (n > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, (DWORD)(n < (int)sizeof b ? n : (int)sizeof b - 1), &w, NULL);
}
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) { Sleep(INFINITE); }   /* one report; other crashing threads wait for the exit */
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    raw_err("[totk] host exception %#lx at rip=%p thread=%lu", (unsigned long)er->ExceptionCode, er->ExceptionAddress, GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        raw_err(" (%s %#llx)", er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "execute" : "read",
                (unsigned long long)er->ExceptionInformation[1]);
    raw_err(" exe=%p\n", (void *)GetModuleHandleW(NULL));
    cpu_context *ctx = cpu_current();
    if (ctx) {   /* pc is the start of the current block unless the fault came through a precise abort */
        char pc[64], lr[64];
        runtime_describe_address(cpu_get_reg(ctx, CPU_REG_PC), pc, sizeof pc); runtime_describe_address(cpu_get_reg(ctx, CPU_REG_LR), lr, sizeof lr);
        raw_err("[totk] guest pc=%s lr=%s sp=%#llx guest thread=%#x\n", pc, lr, (unsigned long long)cpu_get_reg(ctx, CPU_REG_SP), runtime_thread_current_handle());
        for (int i = 0; i < 31; i += 4)
            raw_err("[totk]   x%d..: %016llx %016llx %016llx %016llx\n", i, (unsigned long long)cpu_get_reg(ctx, i), (unsigned long long)cpu_get_reg(ctx, i + 1 < 31 ? i + 1 : 30),
                    (unsigned long long)cpu_get_reg(ctx, i + 2 < 31 ? i + 2 : 30), (unsigned long long)cpu_get_reg(ctx, i + 3 < 31 ? i + 3 : 30));
        uint64_t fp = cpu_get_reg(ctx, 29);
        for (int d = 0; d < 12 && fp && !(fp & 7); d++) {
            uint64_t f[2]; if (host_guarded_read(f, (const void *)(uintptr_t)fp, 16) != 16) break;
            char w[64]; runtime_describe_address(f[1], w, sizeof w); raw_err("[totk]   frame %s\n", w);
            if (f[0] <= fp) break; fp = f[0];
        }
    }
    _exit(128 + 11);
}

/* First-chance view of exceptions the unhandled filter may never see (fail-fast, faults in host code that a later handler
   turns fatal): logged once each kind, then left to the normal dispatch. JIT fastmem faults (rip outside the executable)
   are dynarmic's. */
static LONG CALLBACK first_chance(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    static volatile LONG logged;
    uintptr_t rip = (uintptr_t)er->ExceptionAddress, exe = (uintptr_t)GetModuleHandleW(NULL);
    int host = rip >= exe && rip < exe + (64u << 20);
    if (er->ExceptionCode == 0xe06d7363 || er->ExceptionCode == 0x406d1388 || er->ExceptionCode < 0x80000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !host) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&logged) <= 8)
        raw_err("[totk] first-chance exception %#lx at rip=%p (exe+%#llx) addr=%#llx thread=%lu\n", (unsigned long)er->ExceptionCode, er->ExceptionAddress,
                (unsigned long long)(rip - exe), er->NumberParameters >= 2 ? (unsigned long long)er->ExceptionInformation[1] : 0ull, GetCurrentThreadId());
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------- boot ---------- */
static void loader_ready(void) { fprintf(stderr, "[totk] rtld callback: modules linked\n"); }
static void module_inits(void) {
    fprintf(stderr, "[totk] SDK startup returned; running module constructors\n");
    for (unsigned i = 0; i < NSEARCH; i++) {
        fprintf(stderr, "[totk] ENTER %s DT_INIT\n", modules[i].name);
        runtime_guest_call(modules[i].base + modules[i].init, 0);
        uint32_t state; memcpy(&state, modules[i].base + modules[i].ctor_flag, 4);
        need(state == 2, "DT_INIT deferred or skipped constructors");
        fprintf(stderr, "[totk] RETURN %s DT_INIT state=%u init_array_count=%llu\n", modules[i].name, state, (unsigned long long)(modules[i].init_size / 8));
    }
}
static void module_finis(void) { runtime_fail("guest exited: module finalizers not yet implemented"); }
static void boot(void) {
    fprintf(stderr, "[totk] game version %s\n", TOTK_GAME_VERSION);
    runtime_thread_init();
    uint32_t main_handle = svc_thread_main_init();
    cpu_context *ctx = jit_thread_context();
    main_stack_lo = (uintptr_t)jit_guest_alloc(MAIN_STACK_SIZE);
    cpu_set_reg(ctx, CPU_REG_SP, main_stack_lo + MAIN_STACK_SIZE);
    void *start = runtime_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    need(start != NULL, "nn::init::Start missing");
    fprintf(stderr, "[totk] ENTER original nn::init::Start sdk+%#llx\n", (unsigned long long)((uint8_t *)start - modules[2].base));
    runtime_guest_call(start, 5, (uint64_t)main_handle, (uint64_t)0, jit_host_trap((void *)loader_ready, NULL),
                       jit_host_trap((void *)module_inits, NULL), jit_host_trap((void *)module_finis, NULL));
    runtime_fail("SDK Start unexpectedly returned");
}

int main(int argc, char **argv) {
    _set_fmode(_O_BINARY);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "--vk-caps")) {   /* tools/windows/prepare_shaders.py: <cache>/vk-caps.json, the GPU's translator caps */
        extern void mtl_init(void);
        setenv("TOTK_VK_NO_WINDOW", "1", 1); setenv("TOTK_SHADER_CACHE", argv[2], 1);
        mtl_init();
        char p[1200]; snprintf(p, sizeof p, "%s/vk-caps.json", argv[2]);
        FILE *f = fopen(p, "rb"); if (f) fclose(f);
        _exit(f ? 0 : 1);
    }
    need(argc == 3, "usage: totk-win <exefs dir> boot");
    SetUnhandledExceptionFilter(crash_filter);
    AddVectoredExceptionHandler(0, first_chance);
    for (unsigned i = 0; i < 8; i++) need(pthread_key_create(&runtime_keys[i], NULL) == 0, "allocate thread key");
    need(pthread_key_create(&environment_key, environment_destroy) == 0, "thread environment key");
    jit_init();
    uint64_t at = MODULES_AT;
    for (unsigned k = 0; k < NM; k++) {
        Module *m = &modules[map_order[k]];
        m->name = module_names[map_order[k]];
        map_module(m, argv[1], at);
        at = (at + m->size + 0x1fffff) & ~0x1fffffULL;
    }
    for (unsigned i = 0; i < NSEARCH; i++) relocate(&modules[i]);
    for (unsigned i = 0; i < NSEARCH; i++)
        fprintf(stderr, "[totk] linked %s: relative=%u symbolic=%u cross=%u weak_zero=%u\n", modules[i].name, modules[i].relative, modules[i].symbolic, modules[i].cross, modules[i].weak);
    runtime_game_patches(&modules[0]);
    for (unsigned i = 0; i < NM; i++) seal(&modules[i]);
    if (!strcmp(argv[2], "boot")) {
#ifdef TOTK_WIN_FRONTEND
        runtime_front_run(boot);   /* SDL window + input on this thread, the game on its own */
#else
        boot();
#endif
    }
    else runtime_fail("unknown mode");
    return 0;
}
