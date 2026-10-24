// Vulkan replay backend of the NVN layer (docs/windows-port-plan.md C11 / C20): internal shared header.
// The backend implements the mtl_* seam of nvn_mtl.h / nvn_obj.h (the same record format hle_nvn.c writes), so hle_nvn.c
// links against either nvn_mtl.m (macOS / iOS) or nvn_vk*.c (Windows / Vulkan 1.3). Desktop GPUs are the target (NVIDIA,
// AMD, Intel; Adreno kept in mind): no unified memory, so memory pools are mirrored into device buffers with dirty-range
// uploads and GPU-written ranges read back (or, TOTK_VK_POOL=host, imported zero-copy with VK_EXT_external_memory_host).
//
// Files: nvn_vk_dev.c   instance / device / swapchain / submissions / fences / completion thread / present / stubs
//        nvn_vk_mem.c   memory pools (mirror or import), dirty tracking, arenas, readback
//        nvn_vk_tex.c   textures, views, samplers, descriptor-pool registry, uploads (deswizzle, BC / ASTC fallback)
//        nvn_vk_shader.c shader variants (SPIR-V + reflection json from totk_xlate --target spirv), layouts, pipelines
//        nvn_vk_exec.c  command replay (mtl_exec): render passes (dynamic rendering), draws, compute, copies, clears
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>
#include "volk.h"
#include "vk_mem_alloc.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"

#define VKLOG(...) nvn_vk_log(__VA_ARGS__)
#define VKDIE(...) nvn_vk_die(__VA_ARGS__)
#define VKONCE(...) do { static int once_; if (!once_++) VKLOG(__VA_ARGS__); } while (0)
#define VKCHECK(x) do { VkResult r_ = (x); if (r_ < 0) VKDIE("%s failed: %d (%s:%d)", #x, (int)r_, __FILE__, __LINE__); } while (0)
void nvn_vk_log(const char *fmt, ...);
_Noreturn void nvn_vk_die(const char *fmt, ...);

// ---------- device ----------
typedef struct {
    int inited, ok;
    VkInstance inst; VkPhysicalDevice phys; VkDevice dev; VkQueue q; uint32_t qfam;
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memprops;
    VkDebugUtilsMessengerEXT dbg;
    int validation, uma, discrete;
    // features / extensions actually enabled
    int f_astc, f_bc, f_float64, f_interlock, f_index8, f_ext_host, f_depth_clip, f_geometry, f_wide_lines, f_logic_op,
        f_dual_src, f_aniso, f_storage_wo_fmt, f_border_color, f_fill_mode_non_solid, f_shader_draw_params, f_ballot;
    VkDeviceSize ext_host_align;
    VkSemaphore timeline;
    VkCommandPool cpool;   // used only on the submit thread (and under V.mu for one-off command buffers)
    pthread_mutex_t qmu;   // queue access (submit thread, present, one-off)
    // swapchain
    VkSurfaceKHR surface; VkSwapchainKHR swap; VkFormat sfmt; VkExtent2D sext;
    uint32_t nsimg; VkImage simg[8]; VkSemaphore s_acq[8], s_done[8]; uint32_t s_frame;
    void *(*surface_fn)(void *inst);   // returns VkSurfaceKHR (as uint64) via frontend (front_vk_surface)
} vk_dev;
extern vk_dev V;
int nvn_vk_device_init(void);           // 1 = usable device
VkCommandBuffer vk_oneoff_begin(void);
void vk_oneoff_end(VkCommandBuffer cb); // submits and waits
uint64_t vk_gpu_ticks(void);
uint64_t vk_now_ns(void);

// ---------- arenas: host-visible scratch memory of one submission (driver banks, uniform updates, staging) ----------
typedef struct vk_block { VkBuffer buf; VmaAllocation al; uint8_t *map; VkDeviceSize size, used; struct vk_block *next; } vk_block;
typedef struct { vk_block *head; } vk_arena;
// Allocates n bytes (aligned) from the arena; returns mapped pointer, *buf / *off for binding.
void *vk_arena_alloc(vk_arena *a, VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off);
void vk_arena_reset(vk_arena *a);
void vk_arena_free(vk_arena *a);

// ---------- submissions ----------
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; } vk_post;   // kind 1 event, 2 counter
typedef struct { uint8_t *guest; VkBuffer src; VkDeviceSize src_off; VkDeviceSize n; void *stage; } vk_readback;
typedef struct vk_defer { void (*fn)(void *); void *arg; struct vk_defer *next; } vk_defer;
typedef struct vk_sub {
    uint64_t fence, seq;            // seq: submission number from mtl_sub_begin (fence is known only at commit)
    VkCommandBuffer pre, cb;         // pre: pool / texture uploads (before cb in the same queue submission)
    VkDescriptorPool dpool[64]; int ndpool, dcur;
    vk_arena arena;
    vk_post *post; int npost, cpost;
    vk_readback *rb; int nrb, crb;
    vk_defer *defer;
    struct vk_sub *next;
} vk_sub;
extern _Thread_local vk_sub *g_sub;               // the submission being recorded (submit thread), NULL outside begin/commit
vk_sub *vk_sub_current(void);       // g_sub, or an implicit one (begun on demand)
void vk_sub_defer(vk_sub *s, void (*fn)(void *), void *arg);   // run after the GPU finished this submission
void vk_defer_free(void (*fn)(void *), void *arg);             // after everything submitted so far completes
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout l);
void vk_add_readback(vk_sub *s, uint64_t addr, uint64_t n);   // copy GPU pool contents back to guest memory at completion
extern _Atomic uint64_t g_done;     // completed (retired) fence

// ---------- memory pools ----------
typedef struct vk_pool {
    uintptr_t lo, hi;                // page-expanded guest range
    VkBuffer buf; VmaAllocation al;  // device mirror, or imported host memory
    VkDeviceMemory imported;
    int mode;                        // 0 mirror + uploads, 1 imported host memory (zero copy), 2 host-visible mirror (UMA)
    uint8_t *shadow;                 // mode 0: what the GPU copy holds (for byte-exact dirty diffs)
    uint8_t *map;                    // mode 2: mapped mirror
    uint32_t *gen;                   // per 64 KiB chunk: bumped when an upload changed it (texture re-upload checks)
    uint64_t *stamp;                 // per 64 KiB chunk: dirty-tracker stamp taken when the chunk was write-protected (0: not protected)
    uint64_t *changed;               // per chunk: present count of the last sync that found CPU changes
    uint8_t *cold;                   // per chunk: log2 of the quiet presents needed before it is write-protected again
    uint32_t nchunk;
} vk_pool;
#define VK_CHUNK 65536u
vk_pool *vk_pool_find(uint64_t addr, uint64_t len);   // pool containing [addr, addr+len)
int vk_buf_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);
void vk_pools_sync(vk_sub *s);       // uploads the CPU changes of every mirrored pool (recorded into s->pre)
uint32_t vk_range_gen(uint64_t addr, uint64_t len);   // sum of chunk generations over a range (changes when uploaded data changed)
void vk_guest_write_from_gpu(uint8_t *dst, const void *src, size_t n);   // readback write: keeps the shadow in step
// Optional dirty tracker (nvn_dirty.c): now() = generation, since(addr,len,stamp) = written after stamp, protect(addr,len) =
// write-protect so that the next write is recorded.
typedef struct { uint64_t (*now)(void); int (*since)(const void *, size_t, uint64_t); void (*protect)(const void *, size_t); } nvn_vk_dirty_hooks;
void nvn_vk_set_dirty_hooks(const nvn_vk_dirty_hooks *h);

// ---------- textures ----------
typedef struct vk_view { uint64_t key; VkImageView v; struct vk_view *next; } vk_view;
typedef struct vk_tex {
    uint64_t serial; tex_t t;        // snapshot of the NVN object at creation
    VkImage img; VmaAllocation al; VkFormat fmt, view_fmt0; VkImageAspectFlags aspect;
    VkImageType type; uint32_t w, h, d, levels, layers; VkSampleCountFlagBits samples;
    VkBufferView bview; uint64_t bview_addr;   // TARGET_BUFFER
    int mirrored, decoded;           // mirrored: uploaded from pool bytes; decoded: CPU-transcoded (BC / ASTC fallback)
    uint32_t up_gen; uint64_t up_hash; uint64_t up_sub; int uploaded, gpu_written;
    uint64_t alias_serial, alias_sub;   // a GPU-written texture over the same memory that sampling uses instead (vk_tex_get)
    uint8_t cswz[4];                 // component mapping of the format substitution
    vk_view *views;
    struct vk_tex *hnext;
} vk_tex;
vk_tex *vk_tex_get(const tex_t *t, int for_render);   // creates / uploads on demand (submit thread)
VkImageView vk_tex_view(vk_tex *x, const view_t *v, int has_v, int usage);   // usage 0 sampled, 1 storage, 2 attachment
VkImageView vk_tex_view_typed(vk_tex *x, const view_t *v, int has_v, VkImageViewType want, int usage);
VkSampler vk_sampler_get(int sid);
VkSampler vk_sampler_default(void);
vk_tex *vk_tex_by_id(int tid, const view_t **v, int *has_v);
VkFormat vk_format_of(int nvn_fmt);
void vk_tex_mark_written(vk_tex *x);
VkImageView vk_dummy_view(int dim, int arrayed, int ms, int cls, int storage);
VkBufferView vk_dummy_bview(int cls, int storage);
VkBufferView vk_tex_bview(vk_tex *x);
uint64_t vk_tex_live(void);

// ---------- shaders / programs ----------
enum { VKD_UBO = 1, VKD_SSBO, VKD_COMBINED, VKD_SAMPLED, VKD_SAMPLER, VKD_STORAGE, VKD_UTEXEL, VKD_STEXEL };
typedef struct {
    uint8_t set, desc, kind, rw, dim, arrayed, ms, cls, samp_kind, storage_fmt;
    uint16_t binding, count, reg, samp_reg, off, bank;
} vk_bind;
typedef struct vk_shader {
    char key[160]; int stage;        // 0 vertex, 1 fragment, 5 compute
    VkShaderModule mod; char entry[32];
    int nb; vk_bind b[64];
    int nv; struct { uint8_t loc, cls; } v[32];
    uint32_t fout_mask; int writes_depth;
    uint8_t drv_read[3]; uint16_t tg[3];
    uint8_t c1[4096]; uint32_t c1_len; VkBuffer c1buf; VmaAllocation c1al;
    struct vk_shader *next;
} vk_shader;
typedef struct vk_prog {
    uint64_t serial, akey; int failed; vk_shader *sh[6];
    VkDescriptorSetLayout dsl[2]; VkPipelineLayout layout; int nsets;
    struct vk_pipe *pipes;
    struct vk_prog *next;
} vk_prog;
vk_shader *vk_shader_load(const char *path_base, int stage);   // <path_base>.spv + .json
vk_prog *vk_prog_get(const prog_t *p);
// The shader variant resolver (shader runtime, totk_shader_rt): maps a program stage to "<dir>/<variant>_v" (no extension).
// Gets the stage (0 / 1 / 5), the control section and the code; answers about the bound state may be queried through
// nvn_vk_state_answers. Returns 1 when the files exist (translating first if needed), 0 when the stage is not available yet.
typedef int (*nvn_vk_resolve_fn)(int stage, const uint8_t *control, const uint8_t *code, char *path_base, size_t n);
void nvn_vk_set_resolver(nvn_vk_resolve_fn fn);
// Host capabilities to hand to the translator ("caps" of the SPIR-V chain request); JSON text.
const char *nvn_vk_translator_caps(void);

// ---------- misc ----------
uint64_t vk_hash(const void *p, size_t n, uint64_t h);
extern _Atomic uint64_t g_stat_draws, g_stat_passes, g_stat_dispatches, g_stat_uploads, g_stat_upload_bytes, g_stat_readback_bytes;
extern _Atomic uint64_t g_stat_skip_shader, g_stat_skip_pipe;   /* draws skipped: shader variant / pipeline not ready yet */

// Test / diagnostics API (also used by tests/gfx_vk)
int nvn_vk_available(void);        // device initialized (mtl_init called) and usable
const char *nvn_vk_device_name(void);
int nvn_vk_pool_mode(void);        // 0 mirror, 1 host import, 2 UMA
void nvn_vk_set_window(void *(*surface_fn)(void *vk_instance), const char *const *inst_ext, uint32_t n_inst_ext);
