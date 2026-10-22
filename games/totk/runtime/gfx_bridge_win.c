/* The graphics interface on the Windows JIT build: the counterpart of runtime/gfx_bridge.c (host hooks the NVN layer
   registers, its forced imports in the "totkgfx" registration section, nv::InitializeGraphics). The Mac-only write
   watchpoint diagnostics (TOTK_WATCH_ADDR) are not here. */
#include "runtime.h"
#include "host_fault.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/d2r_gfx/gfx/nvn/nvn_mtl.h"
#include "../third_party/d2r_gfx/host/dm_section.h"
typedef struct { const char *name; void *fn; const char *why; } GfxEntry;
uint64_t g_t0_ns;

uint64_t now_ns(void) { return runtime_now_ns(); }
uint64_t guest_ns(void) { return runtime_now_ns(); }
void *module_of(const void *p) { (void)p; return NULL; }
static int (*fault_hook)(void *, int, void *);
static void (*prepare_hook)(void *, size_t);
/* Write faults on write-protected pool pages (dirty-page tracking) are resolved in the first-chance vectored handler,
   whatever thread and code they come from: the JIT's fastmem stores re-execute natively (no detour through dynarmic's
   slow memory path and the data-abort halt), and host memcpy into guest memory works the same way. */
static int veh_dirty(host_fault_info *info) {
    if (info->kind != HOST_FAULT_ACCESS || info->access != HOST_ACCESS_WRITE || !fault_hook) return HOST_FAULT_CONTINUE;
    return fault_hook((void *)(uintptr_t)info->addr, 1, NULL) ? HOST_FAULT_HANDLED : HOST_FAULT_CONTINUE;
}
void dm_fault_hook_register(int (*fn)(void *, int, void *)) {
    if (fault_hook) runtime_fail("second gfx fault hook");
    fault_hook = fn;
    if (host_fault_add_handler(veh_dirty)) runtime_fail("dirty-page fault handler");
}
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { if (prepare_hook) runtime_fail("second gfx prepare hook"); prepare_hook = fn; }
void dm_res_report_register(void (*fn)(uint32_t, const char *)) { (void)fn; }
extern uint64_t dm_host_write_pc;   /* nvn_dirty.c */
void dm_guest_write_prepare(void *addr, size_t n) { dm_host_write_pc = (uint64_t)(uintptr_t)__builtin_return_address(0); if (prepare_hook) prepare_hook(addr, n); }
/* A guest write fault (jit_glue.c data abort, exact state): dirty-page tracking (nvn_dirty.c) unprotects and the write
   re-executes. context is NULL under the JIT. */
int runtime_gfx_fault(void *addr, int is_write, void *context) { return fault_hook ? fault_hook(addr, is_write, context) : 0; }
void crash_set_wait_dump(void (*fn)(void)) { (void)fn; }
void runtime_watch_set(uintptr_t addr, size_t len) { (void)addr; (void)len; fprintf(stderr, "[watch] not available on Windows\n"); }
void runtime_watch_tick(uint64_t frame) { (void)frame; }
void *runtime_gfx_override(const char *name) {
    unsigned long size = 0;
    const GfxEntry *entries = getsectiondata(&_mh_execute_header, "__DATA", "totkgfx", &size);
    for (unsigned i = 0; entries && i < size / sizeof *entries; i++) if (!strcmp(entries[i].name, name)) return entries[i].fn;
    return NULL;
}
/* The game's graphics allocator callbacks are guest functions: the NVN layer calls them through runtime_guest_call. */
typedef void *(*Alloc)(unsigned long, unsigned long, void *);
typedef void (*Free)(void *, void *);
typedef void *(*Realloc)(void *, unsigned long, void *);
struct Allocator { Alloc alloc; Free free; Realloc realloc; void *user; };
struct Allocator g_nv_alloc, g_nv_devtools_alloc;
static void graphics_allocator(Alloc a, Free f, Realloc r, void *u) { g_nv_alloc = (struct Allocator){a, f, r, u}; }
static void graphics_dev_allocator(Alloc a, Free f, Realloc r, void *u) { g_nv_devtools_alloc = (struct Allocator){a, f, r, u}; }
static void graphics_initialize(void *mem, unsigned long size) {
    if (!mem || !size) runtime_fail("invalid graphics work memory");
    g_t0_ns = runtime_now_ns(); mtl_init();
    fprintf(stderr, "[totk] graphics context initialized (%#lx bytes of work memory)\n", size);
}
__attribute__((used, section(DM_SECT("totkgfx")))) static const GfxEntry init_entry = {"_ZN2nv18InitializeGraphicsEPvm", graphics_initialize, "host graphics context"};
__attribute__((used, section(DM_SECT("totkgfx")))) static const GfxEntry alloc_entry = {"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", graphics_allocator, "game allocator callbacks"};
__attribute__((used, section(DM_SECT("totkgfx")))) static const GfxEntry dev_alloc_entry = {"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", graphics_dev_allocator, "game diagnostic allocator callbacks"};
