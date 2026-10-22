// Vulkan NVN backend: memory pools. NVN GPU addresses are CPU addresses (MemoryPoolGetBufferAddress), so a pool is the
// guest range [mem, mem+size) expanded to pages, and every buffer binding is "pool buffer + (addr - lo)".
// Modes (TOTK_VK_POOL=mirror|host|auto, default auto):
//   0 mirror  device-local VkBuffer per pool. At each submission the bytes the CPU changed since the last upload are
//             copied in (diffed against a shadow copy at 256-byte granularity, so bytes the GPU wrote and the CPU did not
//             touch are never overwritten); GPU-written ranges are read back at completion (vk_add_readback).
//             With dirty hooks (nvn_dirty.c write tracking) only chunks written since their last sync are diffed.
//   1 host    VK_EXT_external_memory_host: the guest pages themselves are the buffer (zero copy, Metal's model); the GPU
//             reads over PCIe on a discrete card.
//   2 UMA     integrated GPUs (all device memory host visible): host-visible mirror, uploads by memcpy.
#include "nvn_vk.h"
#include <stdlib.h>
#include <string.h>

static vk_pool **g_pools; static int g_npools, g_cpools;       // sorted by lo
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
static nvn_vk_dirty_hooks g_hooks;
static int g_mode = -1;

void nvn_vk_set_dirty_hooks(const nvn_vk_dirty_hooks *h) { if (h) g_hooks = *h; else memset(&g_hooks, 0, sizeof g_hooks); }
static int pool_mode(void) {
    if (g_mode >= 0) return g_mode;
    const char *e = getenv("TOTK_VK_POOL");
    if (e && !strcmp(e, "host") && V.f_ext_host) g_mode = 1;
    else if (e && !strcmp(e, "mirror")) g_mode = 0;
    else g_mode = V.uma ? 2 : 0;
    VKLOG("memory pools: %s", g_mode == 0 ? "device mirror + dirty uploads + readback" : g_mode == 1 ? "host memory import (zero copy)" : "host-visible mirror (UMA)");
    return g_mode;
}
int nvn_vk_pool_mode(void) { return pool_mode(); }

static int find_idx(uintptr_t a) {   // last pool with lo <= a, or -1
    int lo = 0, hi = g_npools - 1, r = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_pools[m]->lo <= a) { r = m; lo = m + 1; } else hi = m - 1; }
    return r;
}
vk_pool *vk_pool_find(uint64_t addr, uint64_t len) {
    pthread_mutex_lock(&g_pmu);
    int i = find_idx((uintptr_t)addr);
    vk_pool *p = NULL;
    /* pools may nest or overlap (a pool inside a larger pool's range): search back for one that covers the whole range */
    for (int k = i; k >= 0 && k > i - 8; k--) if (g_pools[k]->lo <= addr && addr + len <= g_pools[k]->hi) { p = g_pools[k]; break; }
    pthread_mutex_unlock(&g_pmu);
    return p;
}
int vk_buf_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    vk_pool *p = vk_pool_find(addr, len ? len : 1);
    if (!p) return 0;
    *buf = p->buf; *off = addr - p->lo; return 1;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    VkBuffer b; VkDeviceSize o;
    if (!vk_buf_at(addr, len, &b, &o)) return 0;
    if (buf) *buf = (void *)(uintptr_t)b;
    if (off) *off = o;
    return 1;
}

static const VkBufferUsageFlags POOL_USAGE = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

static int import_host(vk_pool *p) {
    VkDeviceSize al = V.ext_host_align ? V.ext_host_align : 4096;
    uintptr_t lo = p->lo & ~(uintptr_t)(al - 1), hi = (p->hi + al - 1) & ~(uintptr_t)(al - 1);
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (vkGetMemoryHostPointerPropertiesEXT(V.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)lo, &hp) != VK_SUCCESS || !hp.memoryTypeBits) return 0;
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, hi - lo, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(V.dev, &bi, NULL, &p->buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, p->buf, &mr);
    uint32_t bits = hp.memoryTypeBits & mr.memoryTypeBits, type = UINT32_MAX;
    for (uint32_t i = 0; i < V.memprops.memoryTypeCount; i++) if (bits & (1u << i)) { type = i; if (V.memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break; }
    if (type == UINT32_MAX) { vkDestroyBuffer(V.dev, p->buf, NULL); return 0; }
    VkImportMemoryHostPointerInfoEXT ih = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)lo};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ih, hi - lo, type};
    if (vkAllocateMemory(V.dev, &ai, NULL, &p->imported) != VK_SUCCESS) { vkDestroyBuffer(V.dev, p->buf, NULL); return 0; }
    vkBindBufferMemory(V.dev, p->buf, p->imported, 0);
    p->lo = lo; p->hi = hi;
    return 1;
}

void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init();
    if (!V.ok || !mem || !size) return;
    vk_pool *p = calloc(1, sizeof *p);
    p->lo = (uintptr_t)mem & ~(uintptr_t)4095; p->hi = ((uintptr_t)mem + size + 4095) & ~(uintptr_t)4095;
    p->mode = pool_mode();
    if (p->mode == 1 && !import_host(p)) { VKONCE("host memory import failed: falling back to a mirror for some pools"); p->mode = 0; }
    if (p->mode != 1) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, p->hi - p->lo, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo ai = {0}; ai.usage = VMA_MEMORY_USAGE_AUTO;
        if (p->mode == 2) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        else ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VmaAllocationInfo inf;
        VkResult r = vmaCreateBuffer(V.vma, &bi, &ai, &p->buf, &p->al, &inf);
        if (r != VK_SUCCESS) { VKLOG("pool of %llu bytes: allocation failed (%d)", (unsigned long long)(p->hi - p->lo), r); free(p); return; }
        p->map = inf.pMappedData;
        p->nchunk = (uint32_t)((p->hi - p->lo + VK_CHUNK - 1) / VK_CHUNK);
        p->gen = calloc(p->nchunk, sizeof *p->gen); p->stamp = calloc(p->nchunk, sizeof *p->stamp);
        p->changed = calloc(p->nchunk, sizeof *p->changed); p->cold = malloc(p->nchunk);
        if (p->cold) memset(p->cold, 2, p->nchunk);
        if (p->mode == 0) {
            p->shadow = malloc(p->hi - p->lo);
            if (!p->shadow) VKDIE("pool shadow of %llu bytes", (unsigned long long)(p->hi - p->lo));
            // the first sync uploads everything: shadow = complement of the first byte of each 256 B run is not needed;
            // mark all stale by filling with the inverse of the current contents
            const uint8_t *g = (const uint8_t *)p->lo;
            for (uintptr_t i = 0; i < p->hi - p->lo; i++) p->shadow[i] = (uint8_t)~g[i];
            for (uint32_t c = 0; c < p->nchunk; c++) p->stamp[c] = 0;
        } else if (p->mode == 2) memcpy(p->map, (const void *)p->lo, p->hi - p->lo);
    }
    pthread_mutex_lock(&g_pmu);
    if (g_npools == g_cpools) { g_cpools = g_cpools ? 2 * g_cpools : 64; g_pools = realloc(g_pools, g_cpools * sizeof *g_pools); }
    int i = find_idx(p->lo) + 1;
    memmove(&g_pools[i + 1], &g_pools[i], (size_t)(g_npools - i) * sizeof *g_pools);
    g_pools[i] = p; g_npools++;
    pthread_mutex_unlock(&g_pmu);
}
static void pool_free(void *arg) {
    vk_pool *p = arg;
    if (p->mode == 1) { vkDestroyBuffer(V.dev, p->buf, NULL); vkFreeMemory(V.dev, p->imported, NULL); }
    else vmaDestroyBuffer(V.vma, p->buf, p->al);
    free(p->shadow); free(p->gen); free(p->stamp); free(p->changed); free(p->cold); free(p);
}
void mtl_pool_del(const void *mem) {
    if (!V.ok) return;
    uintptr_t lo = (uintptr_t)mem & ~(uintptr_t)4095;
    pthread_mutex_lock(&g_pmu);
    vk_pool *p = NULL;
    for (int i = 0; i < g_npools; i++) if (g_pools[i]->lo == lo || (g_pools[i]->lo <= (uintptr_t)mem && (uintptr_t)mem < g_pools[i]->hi && g_pools[i]->lo == lo)) {
        p = g_pools[i]; memmove(&g_pools[i], &g_pools[i + 1], (size_t)(g_npools - i - 1) * sizeof *g_pools); g_npools--; break; }
    pthread_mutex_unlock(&g_pmu);
    if (p) vk_defer_free(pool_free, p);
}
void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) { (void)p; (void)off; (void)n; mtl_finish(); }

/* Copies guest bytes the GPU copy does not have yet. Diff granularity: 256 bytes (runs merged when closer than 1 KiB).
   Returns 1 when something changed. */
static int sync_range(vk_sub *s, vk_pool *p, uintptr_t a, uintptr_t b, VkBufferCopy **cp, int *ncp, int *ccp, VkBuffer *stage_buf) {
    const uint8_t *g = (const uint8_t *)a; uint8_t *sh = p->shadow + (a - p->lo);
    size_t n = b - a, i = 0; int changed = 0;
    while (i < n) {
        size_t k = i; const size_t G = 256;
        while (k < n && !memcmp(g + k, sh + k, (n - k) < G ? n - k : G)) k += G;
        if (k >= n) break;
        size_t e = k, gap = 0;
        while (e < n && gap < 1024) { size_t len = (n - e) < G ? n - e : G; if (memcmp(g + e, sh + e, len)) gap = 0; else gap += len; e += len; }
        e -= gap; if (e > n) e = n;
        VkDeviceSize so; VkBuffer sb; uint8_t *dst = vk_arena_alloc(&s->arena, e - k, 16, &sb, &so);
        memcpy(dst, g + k, e - k); memcpy(sh + k, dst, e - k);
        if (*stage_buf != sb && *ncp) { vkCmdCopyBuffer(s->pre, *stage_buf, p->buf, (uint32_t)*ncp, *cp); *ncp = 0; }
        *stage_buf = sb;
        if (*ncp == *ccp) { *ccp = *ccp ? 2 * *ccp : 64; *cp = realloc(*cp, *ccp * sizeof **cp); }
        (*cp)[(*ncp)++] = (VkBufferCopy){so, a + k - p->lo, e - k};
        for (uintptr_t c = (a + k - p->lo) / VK_CHUNK; c <= (a + e - 1 - p->lo) / VK_CHUNK; c++) p->gen[c]++;
        atomic_fetch_add(&g_stat_upload_bytes, e - k);
        i = e; changed = 1;
    }
    return changed;
}
/* Mirror mode: comparing every pool byte with its shadow at each submission costs tens of milliseconds per frame (TOTK keeps
   hundreds of MiB in memory pools). With the dirty tracker, a chunk the CPU has left alone for a while (1 << cold[c]
   presents) is write-protected before its diff; later submissions skip it until a write is recorded (fault or host write
   preparation), then it is diffed again, and protected later the next time (cold[c] grows) so that chunks written every
   few frames do not fault all the time. Order: stamp, protect, diff: a write before the protection is in the diff, a
   write after it is recorded with a newer generation than the stamp. */
uint64_t mtl_present_count(void);
static uint64_t g_skip_chunks, g_diff_chunks;
void vk_pools_sync(vk_sub *s) {
    static VkBufferCopy *cp; static int ccp;
    int any = 0;
    const int track = g_hooks.now && g_hooks.since && g_hooks.protect;
    const uint64_t frame = mtl_present_count();
    pthread_mutex_lock(&g_pmu);
    for (int pi = 0; pi < g_npools; pi++) {
        vk_pool *p = g_pools[pi];
        if (p->mode == 1) continue;
        if (p->mode == 2) {   // UMA mirror: changed bytes by memcmp at chunk granularity
            for (uint32_t c = 0; c < p->nchunk; c++) {
                uintptr_t a = p->lo + (uintptr_t)c * VK_CHUNK, b = a + VK_CHUNK > p->hi ? p->hi : a + VK_CHUNK;
                if (memcmp(p->map + (a - p->lo), (const void *)a, b - a)) { memcpy(p->map + (a - p->lo), (const void *)a, b - a); p->gen[c]++; }
            }
            continue;
        }
        int ncp = 0; VkBuffer sb = VK_NULL_HANDLE;
        for (uint32_t c = 0; c < p->nchunk; c++) {
            uintptr_t a = p->lo + (uintptr_t)c * VK_CHUNK, b = a + VK_CHUNK > p->hi ? p->hi : a + VK_CHUNK;
            if (track) {
                if (p->stamp[c]) {
                    if (!g_hooks.since((const void *)a, b - a, p->stamp[c])) { g_skip_chunks++; continue; }
                    p->stamp[c] = 0;
                    if (p->cold[c] < 10) p->cold[c]++;
                } else if (frame - p->changed[c] >= (1ull << p->cold[c])) {
                    uint64_t st = g_hooks.now(); g_hooks.protect((const void *)a, b - a); p->stamp[c] = st ? st : 1;
                }
            }
            g_diff_chunks++;
            if (sync_range(s, p, a, b, &cp, &ncp, &ccp, &sb)) p->changed[c] = frame;
        }
        if (ncp) { vkCmdCopyBuffer(s->pre, sb, p->buf, (uint32_t)ncp, cp); any = 1; }
    }
    pthread_mutex_unlock(&g_pmu);
    if (track && frame && frame % 600 == 0) {
        static uint64_t logged; if (logged != frame) { logged = frame;
            VKLOG("pool sync: %llu chunks diffed, %llu skipped (write-protected and unwritten) since the last report", (unsigned long long)g_diff_chunks, (unsigned long long)g_skip_chunks);
            g_diff_chunks = g_skip_chunks = 0; }
    }
    if (any) {
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
        vkCmdPipelineBarrier2(s->pre, &di);
    }
}
uint32_t vk_range_gen(uint64_t addr, uint64_t len) {
    vk_pool *p = vk_pool_find(addr, len ? len : 1);
    if (!p || !p->gen) return 0;
    uint32_t g = 0;
    for (uint64_t c = (addr - p->lo) / VK_CHUNK; c <= (addr + (len ? len : 1) - 1 - p->lo) / VK_CHUNK && c < p->nchunk; c++) g += p->gen[c] * 2654435761u + (uint32_t)c;
    return g;
}
void vk_guest_write_from_gpu(uint8_t *dst, const void *src, size_t n) {
    memcpy(dst, src, n);
    vk_pool *p = vk_pool_find((uintptr_t)dst, n);
    if (p && p->shadow) memcpy(p->shadow + ((uintptr_t)dst - p->lo), src, n);
    else if (p && p->map) memcpy(p->map + ((uintptr_t)dst - p->lo), src, n);
}
