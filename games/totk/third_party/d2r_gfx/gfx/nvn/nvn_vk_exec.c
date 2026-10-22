// Vulkan NVN backend: command replay (mtl_exec), the Vulkan counterpart of nvn_mtl.m's mtl_exec / state_op / draw /
// dispatch / copies / clears, without the Metal backend's TBDR optimizations (pass merging, lookahead, argument-buffer
// reuse). Render targets become dynamic-rendering passes, opened lazily at the first draw / clear and closed by a target
// change, a copy, a dispatch, an inline update or the end of the submission. Synchronisation is coarse: a full memory
// barrier after every pass, copy and dispatch (all images stay in VK_IMAGE_LAYOUT_GENERAL).
//
// Coordinate convention: NVN with upper-left window origin (what the translator is told) is replayed as the Metal backend
// does (viewport x, y, w, h as given; clip-space +y up), so the Vulkan viewport is flipped (negative height); front faces
// keep their NVN winding meaning under the flip.
#include "nvn_vk.h"
#include "nvn_fmt_vk.h"
#include "nvn_refl.h"
#include "nvn_astc_dec.h"
#include "nvn_bcdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#endif

VkBuffer vk_zero_buffer(void);
VkDescriptorType vk_desc_type(int d);

// ---------- replay state (as nvn_mtl.m S / R) ----------
static struct {
    const prog_t *prog;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    uint32_t attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16]; uint64_t tex[6][128], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][6]; int has_vp; float dr[2]; float blend_color[4];
    int64_t copy_stride; int depth_clamp, render_disabled, raster_discard;
} S;
static struct { p_set_rt rt; int has_rt; p_scissor sc; int has_sc; } R;
static struct {
    int open; uint32_t w, h, layers, ncolor; VkFormat cfmt[8], dfmt, sfmt; VkSampleCountFlagBits samples;
    VkImageView cview[8], dview; vk_tex *ctex[8], *dtex;
} P;
static int g_inited; static uint32_t g_dispatch_grid[3];
static VkBuffer g_support; static VmaAllocation g_support_al;

static void state_reset(void) {
    memset(&S, 0, sizeof S);
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){(uint64_t)i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
}
static VkCommandBuffer CB(void) { return vk_sub_current()->cb; }
static void barrier_full(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
static void pass_end(void) {
    if (!P.open) return;
    vkCmdEndRendering(CB()); P.open = 0;
    barrier_full(CB());
}
void vk_exec_end_all(void) { pass_end(); }
void vk_exec_sub_begin(void) {
    if (!g_inited) { g_inited = 1; state_reset(); }
    P.open = 0;
}
static VkBuffer support_buf(void) {
    if (!g_support) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo ai = {0}; ai.usage = VMA_MEMORY_USAGE_AUTO; ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo inf; VKCHECK(vmaCreateBuffer(V.vma, &bi, &ai, &g_support, &g_support_al, &inf));
        uint8_t *m = inf.pMappedData; memset(m, 0, 65536);
        for (int i = 0; i < 73; i++) { float one = 1.0f; memcpy(m + 192 + 16 * i, &one, 4); }   // render_scale[i].x (Ryujinx SupportBuffer layout)
        vmaFlushAllocation(V.vma, g_support_al, 0, VK_WHOLE_SIZE);
    }
    return g_support;
}

// ---------- render passes ----------
static uint32_t mipd(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }
static int rt_has_any(void) { if (!R.has_rt) return 0; if (R.rt.depth) return 1; for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) return 1; return 0; }
static void pass_begin(void) {
    if (P.open) return;
    memset(&P, 0, sizeof P);
    VkRenderingAttachmentInfo ca[8]; memset(ca, 0, sizeof ca);
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t w = 16384, h = 16384, layers = 1; int any = 0;
    P.samples = VK_SAMPLE_COUNT_1_BIT;
    int n = R.has_rt ? (R.rt.n > 8 ? 8 : R.rt.n) : 0;
    for (int i = 0; i < n; i++) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!R.rt.color[i]) continue;
        vk_tex *x = vk_tex_get(R.rt.color[i], 1); if (!x || !x->img) continue;
        const view_t *v = R.rt.has_cview[i] ? &R.rt.cview[i] : NULL;
        VkImageView iv = vk_tex_view(x, v, v != NULL, 2); if (!iv) continue;
        unsigned lv = v && v->set_levels ? v->base_level : 0;
        uint32_t aw = mipd(x->w, lv), ah = mipd(x->h, lv); if (aw < w) w = aw; if (ah < h) h = ah;
        uint32_t al = v && v->set_layers && v->nlayers ? v->nlayers : (x->type == VK_IMAGE_TYPE_3D ? mipd(x->d, lv) : x->layers);
        if (!(v && v->set_layers)) al = 1;
        if (al > 1 && (!any || al < layers)) layers = al;
        ca[i].imageView = iv; ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        P.cfmt[i] = v && v->set_format && !x->decoded && vk_format_of(v->format) ? vk_format_of(v->format) : x->fmt;
        P.ctex[i] = x; P.cview[i] = iv; P.samples = x->samples; P.ncolor = (uint32_t)i + 1; any = 1;
    }
    if (R.has_rt && R.rt.depth) {
        vk_tex *x = vk_tex_get(R.rt.depth, 1);
        const view_t *v = R.rt.has_dview ? &R.rt.dview : NULL;
        VkImageView iv = x && x->img ? vk_tex_view(x, v, v != NULL, 2) : VK_NULL_HANDLE;
        if (iv) {
            unsigned lv = v && v->set_levels ? v->base_level : 0;
            uint32_t aw = mipd(x->w, lv), ah = mipd(x->h, lv); if (aw < w) w = aw; if (ah < h) h = ah;
            VkImageAspectFlags asp = x->aspect;
            if (asp & VK_IMAGE_ASPECT_DEPTH_BIT) { da.imageView = iv; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL; da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; P.dfmt = x->fmt; }
            if (asp & VK_IMAGE_ASPECT_STENCIL_BIT) { sa.imageView = iv; sa.imageLayout = VK_IMAGE_LAYOUT_GENERAL; sa.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE; P.sfmt = x->fmt; }
            P.dtex = x; P.dview = iv; P.samples = x->samples; any = 1;
        }
    }
    if (!any) {   // no attachments: the extent of viewport 0, else of the scissor (nvn_mtl.m noatt_dims)
        double x1 = 0, y1 = 0;
        if (S.has_vp) { x1 = ceil(fmax(S.vp[0][0], S.vp[0][0] + S.vp[0][2])); y1 = ceil(fmax(S.vp[0][1], S.vp[0][1] + S.vp[0][3])); }
        if ((x1 < 1 || y1 < 1) && R.has_sc) { x1 = (double)R.sc.x + R.sc.w; y1 = (double)R.sc.y + R.sc.h; }
        w = x1 < 1 ? 1 : x1 > 16384 ? 16384 : (uint32_t)x1; h = y1 < 1 ? 1 : y1 > 16384 ? 16384 : (uint32_t)y1;
    }
    P.w = w; P.h = h; P.layers = layers;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {w, h}}, layers, 0, P.ncolor, ca,
                          da.imageView ? &da : NULL, sa.imageView ? &sa : NULL};
    vkCmdBeginRendering(CB(), &ri);
    P.open = 1;
    atomic_fetch_add(&g_stat_passes, 1);
}

// ---------- pipelines ----------
typedef struct {
    const void *prog; uint32_t cfmt[8], dfmt, sfmt, samples, ncolor;
    blend_st blend[8]; uint32_t blend_en, chmask, logic_op;
    uint8_t poly_mode, raster_discard, depth_clamp, a2c, topo, offset_en;
    ds_st ds; uint32_t vattr[32]; uint32_t vstride[16], vdiv[16];
} pso_key;
typedef struct vk_pipe { pso_key k; VkPipeline p; struct vk_pipe *next; } vk_pipe;

static VkBlendFactor bfac(int f) {
    switch (f) { case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR; case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA; case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp bop(int e) { switch (e) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; } return VK_BLEND_OP_ADD; }
static VkCompareOp cmpop(int f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static VkStencilOp stop(int o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
static VkPrimitiveTopology topo_of(int prim, int *quads) {
    *quads = 0;
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: VKONCE("line loops drawn as line strips"); return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case NVN_DRAW_PRIMITIVE_QUADS: *quads = 1; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
static VkFormat dummy_attr_fmt(int cls) { return cls == 1 ? VK_FORMAT_R32G32B32A32_UINT : cls == 2 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT; }

/* Pipelines: looked up by a hash of the key, created on worker threads (the draw that first needs one is skipped until it
   exists, so the render thread never waits for the driver's compiler; TOTK_SHADER_SYNC=1 creates them on the spot), all
   through one VkPipelineCache kept on disk next to the shader cache (loaded before the first frame, saved every few
   minutes when it grew and at exit). */
typedef struct {
    VkGraphicsPipelineCreateInfo gci; VkPipelineRenderingCreateInfo rci; VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb[17]; VkVertexInputAttributeDescription va[32];
    VkPipelineVertexInputStateCreateInfo vi; VkPipelineInputAssemblyStateCreateInfo ia; VkPipelineViewportStateCreateInfo vps;
    VkPipelineRasterizationStateCreateInfo rs; VkPipelineRasterizationDepthClipStateCreateInfoEXT dcs; VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds; VkPipelineColorBlendAttachmentState cba[8]; VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[10]; VkPipelineDynamicStateCreateInfo dy; VkFormat cf[8];
    VkComputePipelineCreateInfo cci; int compute;
    struct vk_pipe *dst;
} pipe_job;
static VkPipelineCache g_pcache; static char g_pcache_path[1200]; static _Atomic int g_pcache_new;
static pthread_mutex_t g_pipe_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pipe_cv = PTHREAD_COND_INITIALIZER;
typedef struct pjq { pipe_job *j; struct pjq *next; } pjq;
static pjq *g_pq, *g_pq_tail; static _Atomic int g_pipes_pending; static _Atomic uint64_t g_pipes_made, g_pipe_ns;
static int shader_sync(void) { static int s = -1; if (s < 0) { const char *e = getenv("TOTK_SHADER_SYNC"); s = e && atoi(e); } return s; }
const char *nvn_vk_cache_dir(void);
void vk_pcache_init(void) {
    if (g_pcache) return;
    snprintf(g_pcache_path, sizeof g_pcache_path, "%s/vk-pipelines-%04x-%04x-%08x.bin", nvn_vk_cache_dir(), V.props.vendorID, V.props.deviceID, V.props.driverVersion);
    void *data = NULL; size_t n = 0;
    FILE *f = fopen(g_pcache_path, "rb");
    if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); if (sz > 0 && (data = malloc((size_t)sz))) n = fread(data, 1, (size_t)sz, f); fclose(f); }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, n, data};
    if (vkCreatePipelineCache(V.dev, &ci, NULL, &g_pcache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; vkCreatePipelineCache(V.dev, &ci, NULL, &g_pcache); n = 0; }
    free(data);
    VKLOG("pipeline cache %s: %zu bytes loaded", g_pcache_path, n);
}
void vk_pcache_save(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (!g_pcache || !atomic_exchange(&g_pcache_new, 0)) return;
    pthread_mutex_lock(&mu);
    size_t n = 0; void *data = NULL;
    if (vkGetPipelineCacheData(V.dev, g_pcache, &n, NULL) == VK_SUCCESS && n && (data = malloc(n)) && vkGetPipelineCacheData(V.dev, g_pcache, &n, data) == VK_SUCCESS) {
        char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", g_pcache_path);
        FILE *f = fopen(tmp, "wb");
        if (f) { int ok = fwrite(data, 1, n, f) == n; ok &= fclose(f) == 0; if (ok) { remove(g_pcache_path); rename(tmp, g_pcache_path); } }
    }
    free(data);
    pthread_mutex_unlock(&mu);
}
static void *pcache_save_thread(void *arg) { (void)arg; vk_pcache_save(); return NULL; }
void vk_pcache_tick(uint64_t present) {   // nvn_vk_dev.c present: every 3600 presents, in the background
    if (present % 3600 || !atomic_load(&g_pcache_new)) return;
    pthread_t t; if (!pthread_create(&t, NULL, pcache_save_thread, NULL)) pthread_detach(t);
}
static VkPipeline pipe_create(pipe_job *j) {
    VkPipeline pipe = VK_NULL_HANDLE; uint64_t t0 = vk_now_ns();
    VkResult r = j->compute ? vkCreateComputePipelines(V.dev, g_pcache, 1, &j->cci, NULL, &pipe) : vkCreateGraphicsPipelines(V.dev, g_pcache, 1, &j->gci, NULL, &pipe);
    if (r != VK_SUCCESS) { VKLOG("%s pipeline creation failed: %d", j->compute ? "compute" : "graphics", r); pipe = VK_NULL_HANDLE; }
    atomic_fetch_add(&g_pipe_ns, vk_now_ns() - t0); atomic_fetch_add(&g_pipes_made, 1); atomic_store(&g_pcache_new, 1);
    return pipe;
}
typedef struct vk_pipe2 { pso_key k; uint64_t h; _Atomic(VkPipeline) p; _Atomic int state; struct vk_pipe2 *next; } vk_pipe2;   // state 0 pending, 1 ready, 2 failed
static void *pipe_worker(void *arg) {
    (void)arg;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        pthread_mutex_lock(&g_pipe_mu);
        while (!g_pq) pthread_cond_wait(&g_pipe_cv, &g_pipe_mu);
        pjq *q = g_pq; g_pq = q->next; if (!g_pq) g_pq_tail = NULL;
        pthread_mutex_unlock(&g_pipe_mu);
        vk_pipe2 *dst = (vk_pipe2 *)q->j->dst;
        VkPipeline p = pipe_create(q->j);
        atomic_store(&dst->p, p); atomic_store(&dst->state, p ? 1 : 2);
        atomic_fetch_sub(&g_pipes_pending, 1);
        free(q->j); free(q);
    }
    return NULL;
}
static void fix_pointers(pipe_job *j, uint32_t nst, uint32_t nvb, uint32_t nva, uint32_t ncolor, int dcs) {
    j->vi = (VkPipelineVertexInputStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, nvb, j->vb, nva, j->va};
    j->rs.pNext = dcs ? &j->dcs : NULL;
    j->cb.pAttachments = j->cba; j->cb.attachmentCount = ncolor;
    j->dy = (VkPipelineDynamicStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 10, j->dyn};
    j->rci.pColorAttachmentFormats = j->cf;
    j->gci = (VkGraphicsPipelineCreateInfo){VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &j->rci, 0, nst, j->st, &j->vi, &j->ia, NULL, &j->vps, &j->rs, &j->ms, &j->ds, &j->cb, &j->dy, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
}
static uint64_t key_hash(const pso_key *k) { return vk_hash(k, sizeof *k, 0x51ed270b27f1a3c5ull); }
static vk_pipe2 *pipe_find(vk_prog *pr, const pso_key *k, uint64_t h) {
    for (vk_pipe2 *p = (vk_pipe2 *)__atomic_load_n(&pr->pipes, __ATOMIC_ACQUIRE); p; p = p->next) if (p->h == h && !memcmp(&p->k, k, sizeof *k)) return p;
    return NULL;
}
/* Per-frame budget for creating pipelines on the submitting thread: a pipeline the driver has in its cache takes well
   under a millisecond, so most new ones are made in place and their draw is not skipped. Past the budget (a new area
   with thousands of new pipelines) they go to the workers and their draws are skipped until ready. TOTK_PIPE_BUDGET_MS
   (default 8; 0 = always on the workers). vk_pipe_budget_reset() is called at each present. */
static _Atomic uint64_t g_budget_used_ns;
static uint64_t pipe_budget_ns(void) {
    static int64_t b = -1;
    if (b < 0) { const char *e = getenv("TOTK_PIPE_BUDGET_MS"); double ms = e && *e ? atof(e) : 8.0; b = ms > 0 ? (int64_t)(ms * 1e6) : 0; }
    return (uint64_t)b;
}
static int pipe_budget_take(void) { return atomic_load(&g_budget_used_ns) < pipe_budget_ns(); }
void vk_pipe_budget_reset(void) { atomic_store(&g_budget_used_ns, 0); }
/* Returns the pipeline, or VK_NULL_HANDLE (being created / failed: skip the draw). */
static VkPipeline pipe_submit(vk_prog *pr, const pso_key *k, uint64_t h, pipe_job *j) {
    vk_pipe2 *np = calloc(1, sizeof *np); np->k = *k; np->h = h; j->dst = (struct vk_pipe *)np;
    if (j->compute) j->cci.layout = pr->layout; else j->gci.layout = pr->layout;
    if (shader_sync() || pipe_budget_take()) {
        uint64_t t0 = vk_now_ns();
        VkPipeline p = pipe_create(j); atomic_store(&np->p, p); atomic_store(&np->state, p ? 1 : 2); free(j);
        atomic_fetch_add(&g_budget_used_ns, vk_now_ns() - t0);
    }
    else {
        static int workers; pjq *q = malloc(sizeof *q); q->j = j; q->next = NULL;
        pthread_mutex_lock(&g_pipe_mu);
        if (g_pq_tail) g_pq_tail->next = q; else g_pq = q;
        g_pq_tail = q; atomic_fetch_add(&g_pipes_pending, 1);
        for (; workers < 3; workers++) { pthread_t t; if (pthread_create(&t, NULL, pipe_worker, NULL)) break; pthread_detach(t); }
        pthread_cond_signal(&g_pipe_cv);
        pthread_mutex_unlock(&g_pipe_mu);
    }
    np->next = (vk_pipe2 *)pr->pipes; __atomic_store_n(&pr->pipes, (struct vk_pipe *)np, __ATOMIC_RELEASE);   // only the submit thread adds
    return atomic_load(&np->p);
}
void vk_pipe_stats(uint64_t *made, uint64_t *ns, int *pending) { *made = atomic_load(&g_pipes_made); *ns = atomic_load(&g_pipe_ns); *pending = atomic_load(&g_pipes_pending); }

static VkPipeline pso_get(vk_prog *pr, VkPrimitiveTopology topo) {
    pso_key k; memset(&k, 0, sizeof k);
    k.prog = pr; memcpy(k.cfmt, P.cfmt, sizeof k.cfmt); k.dfmt = P.dfmt; k.sfmt = P.sfmt; k.samples = P.samples; k.ncolor = P.ncolor;
    for (uint32_t i = 0; i < P.ncolor; i++) k.blend[i] = S.blend[i];
    k.blend_en = S.color.blend_en; k.chmask = S.chmask.mask; k.logic_op = S.color.logic_op;
    k.poly_mode = (uint8_t)S.poly.mode; k.raster_discard = (uint8_t)S.raster_discard; k.depth_clamp = (uint8_t)S.depth_clamp; k.a2c = S.ms.a2c;
    k.topo = (uint8_t)topo; k.offset_en = (uint8_t)S.poly.offset_en;
    k.ds = S.ds;
    vk_shader *vs = pr->sh[0];
    uint32_t streams = 0;   /* only the streams this vertex shader reads: the strides of other bound streams must not split the key */
    for (int i = 0; vs && i < vs->nv; i++) {
        int loc = vs->v[i].loc; k.vattr[i] = loc < S.nattr || loc < 16 ? S.attr[loc & 15] : 0;
        va_st a; uint32_t raw = S.attr[loc & 15]; memcpy(&a, &raw, 4);
        if (loc < 16 && a.fmt) streams |= 1u << (a.stream & 15);
    }
    for (int i = 0; i < 16; i++) if (streams & (1u << i)) { k.vstride[i] = S.stream[i].stride; k.vdiv[i] = S.stream[i].divisor; }
    uint64_t h = key_hash(&k);
    vk_pipe2 *found = pipe_find(pr, &k, h);
    if (found) return atomic_load(&found->p);

    pipe_job *j = calloc(1, sizeof *j);
    uint32_t nst = 0;
    for (int s = 0; s < 2; s++) if (pr->sh[s]) j->st[nst++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
        s == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, pr->sh[s]->mod, pr->sh[s]->entry, NULL};
    uint32_t nvb = 0, nva = 0; uint32_t used_streams = 0; int need_dummy = 0;
    for (int i = 0; vs && i < vs->nv; i++) {
        int loc = vs->v[i].loc; va_st a; uint32_t raw = S.attr[loc & 15]; memcpy(&a, &raw, 4);
        VkFormat f = loc < 16 && a.fmt ? (VkFormat)nvn_vertex_fmt_vk((int)a.fmt) : VK_FORMAT_UNDEFINED;
        if (!f) { j->va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)loc, 16, dummy_attr_fmt(vs->v[i].cls), 0}; need_dummy = 1; continue; }
        j->va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)loc, a.stream, f, a.offset};
        used_streams |= 1u << a.stream;
    }
    for (int sidx = 0; sidx < 16; sidx++) if (used_streams & (1u << sidx))
        j->vb[nvb++] = (VkVertexInputBindingDescription){(uint32_t)sidx, S.stream[sidx].stride, S.stream[sidx].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    if (need_dummy) j->vb[nvb++] = (VkVertexInputBindingDescription){16, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    j->ia = (VkPipelineInputAssemblyStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, topo, VK_FALSE};
    j->vps = (VkPipelineViewportStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    int pm = S.poly.mode;
    j->rs = (VkPipelineRasterizationStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, S.depth_clamp ? VK_TRUE : VK_FALSE, S.raster_discard ? VK_TRUE : VK_FALSE,
        !V.f_fill_mode_non_solid || pm == NVN_POLYGON_MODE_FILL ? VK_POLYGON_MODE_FILL : pm == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_POINT,
        VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    j->dcs = (VkPipelineRasterizationDepthClipStateCreateInfoEXT){VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT, NULL, 0, VK_FALSE};
    int dcs = S.depth_clamp && V.f_depth_clip;
    j->ms = (VkPipelineMultisampleStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, P.samples, VK_FALSE, 0, NULL, S.ms.a2c ? VK_TRUE : VK_FALSE, VK_FALSE};
    VkStencilOpState sf = {stop((int)S.ds.fail_f), stop((int)S.ds.zpass_f), stop((int)S.ds.zfail_f), cmpop((int)S.ds.sf_f), 0xff, 0xff, 0};
    VkStencilOpState sb = {stop((int)S.ds.fail_b), stop((int)S.ds.zpass_b), stop((int)S.ds.zfail_b), cmpop((int)S.ds.sf_b), 0xff, 0xff, 0};
    j->ds = (VkPipelineDepthStencilStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0,
        P.dfmt && S.ds.dt ? VK_TRUE : VK_FALSE, P.dfmt && S.ds.dt && S.ds.dw ? VK_TRUE : VK_FALSE, cmpop((int)S.ds.df), VK_FALSE, P.sfmt && S.ds.st ? VK_TRUE : VK_FALSE, sf, sb, 0, 1};
    for (uint32_t i = 0; i < P.ncolor; i++) {
        const blend_st *b = &S.blend[i];
        j->cba[i] = (VkPipelineColorBlendAttachmentState){(S.color.blend_en >> i) & 1 ? VK_TRUE : VK_FALSE, bfac((int)b->src_rgb), bfac((int)b->dst_rgb), bop((int)b->eq_rgb),
            bfac((int)b->src_a), bfac((int)b->dst_a), bop((int)b->eq_a), (S.chmask.mask >> (4 * i)) & 0xf};
        if (!P.cfmt[i]) j->cba[i].colorWriteMask = 0;
        if (j->cba[i].blendEnable && P.cfmt[i]) {   // integer formats cannot blend
            VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(V.phys, (VkFormat)P.cfmt[i], &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) j->cba[i].blendEnable = VK_FALSE;
        }
    }
    int lop = S.color.logic_op != NVN_LOGIC_OP_COPY && V.f_logic_op;
    j->cb = (VkPipelineColorBlendStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, lop ? VK_TRUE : VK_FALSE,
        lop ? (VkLogicOp)(S.color.logic_op & 15) : VK_LOGIC_OP_COPY, P.ncolor, NULL, {0, 0, 0, 0}};
    if (S.color.logic_op != NVN_LOGIC_OP_COPY && !V.f_logic_op) VKONCE("logic op %u not supported by the device", S.color.logic_op);
    static const VkDynamicState dyn[10] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE};
    memcpy(j->dyn, dyn, sizeof dyn);
    for (uint32_t i = 0; i < P.ncolor; i++) j->cf[i] = (VkFormat)P.cfmt[i];
    j->rci = (VkPipelineRenderingCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, P.ncolor, NULL, (VkFormat)P.dfmt, (VkFormat)P.sfmt};
    fix_pointers(j, nst, nvb, nva, P.ncolor, dcs);
    return pipe_submit(pr, &k, h, j);
}
static VkPipeline cps_get(vk_prog *pr) {
    pso_key k; memset(&k, 0, sizeof k); k.prog = pr; k.topo = 255;
    uint64_t h = key_hash(&k);
    vk_pipe2 *found = pipe_find(pr, &k, h);
    if (found) return atomic_load(&found->p);
    pipe_job *j = calloc(1, sizeof *j); j->compute = 1;
    j->cci = (VkComputePipelineCreateInfo){VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, pr->sh[5]->mod, pr->sh[5]->entry, NULL}, VK_NULL_HANDLE, VK_NULL_HANDLE, -1};
    return pipe_submit(pr, &k, h, j);
}
// ---------- descriptors (nvn_mtl.m encode_args, binding kinds of nvn_refl.h) ----------
static void driver_bank_fill(uint8_t *d, int bank, int stage) {
    memset(d, 0, 0xA00);
    if (bank == 0) {
        int base = stage == 0 ? 0x110 : stage == 5 ? 0x310 : stage == 1 ? 0x510 : -1;
        if (base >= 0) for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[stage][r].addr; uint32_t n = (uint32_t)S.ssbo[stage][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
        if (stage == 5) { memcpy(d + 0x200, g_dispatch_grid, sizeof g_dispatch_grid);
            for (int r = 0; r < 16; r++) { uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size; memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4); } }
        else if (stage == 0) { const float ps_max = 2047.9375f; memcpy(d + 0x610, &ps_max, sizeof ps_max); }
    } else {
        for (int r = 0; r < 128; r++) { uint32_t th = (uint32_t)S.tex[stage][r], sh_ = (uint32_t)S.samp[stage][r]; memcpy(d + 0x168 + 8 * r, &th, 4); memcpy(d + 0x568 + 8 * r, &sh_, 4); }
        for (int r = 0; r < 32; r++) { uint32_t th = (uint32_t)S.tex[stage][r]; memcpy(d + 0x20 + 8 * r, &th, 4); }
    }
}
typedef struct { VkWriteDescriptorSet w[128]; VkDescriptorBufferInfo bi[128]; VkDescriptorImageInfo ii[128]; VkBufferView bv[128]; int n; } wset;
static void w_buf(wset *ws, VkDescriptorSet ds, const vk_bind *b, VkBuffer buf, VkDeviceSize off, VkDeviceSize range) {
    if (ws->n >= 128) return;
    int i = ws->n++; ws->bi[i] = (VkDescriptorBufferInfo){buf, off, range};
    ws->w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, b->binding, 0, 1, vk_desc_type(b->desc), NULL, &ws->bi[i], NULL};
}
static void w_img(wset *ws, VkDescriptorSet ds, const vk_bind *b, VkSampler s, VkImageView v) {
    if (ws->n >= 128) return;
    int i = ws->n++; ws->ii[i] = (VkDescriptorImageInfo){s, v, VK_IMAGE_LAYOUT_GENERAL};
    ws->w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, b->binding, 0, 1, vk_desc_type(b->desc), &ws->ii[i], NULL, NULL};
}
static void w_texel(wset *ws, VkDescriptorSet ds, const vk_bind *b, VkBufferView v) {
    if (ws->n >= 128) return;
    int i = ws->n++; ws->bv[i] = v;
    ws->w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, b->binding, 0, 1, vk_desc_type(b->desc), NULL, NULL, &ws->bv[i]};
}
static void bind_ubo_range(wset *ws, VkDescriptorSet ds, const vk_bind *b, uint64_t addr, uint64_t size) {
    VkBuffer buf; VkDeviceSize off;
    uint64_t maxr = V.props.limits.maxUniformBufferRange < 65536 ? V.props.limits.maxUniformBufferRange : 65536;
    if (!size || size > maxr) size = size ? maxr : 16;
    vk_pool *p = addr ? vk_pool_find(addr, 1) : NULL;
    if (!p || !vk_buf_at(addr, 1, &buf, &off)) { VKONCE("uniform buffer at %#llx not in a memory pool", (unsigned long long)addr); w_buf(ws, ds, b, vk_zero_buffer(), 0, 65536); return; }
    if (addr + size > p->hi) size = p->hi - addr;
    if (off % V.props.limits.minUniformBufferOffsetAlignment) {   // misaligned: bind a copy of the current pool contents
        VkBuffer sb; VkDeviceSize so; void *m = vk_arena_alloc(&vk_sub_current()->arena, size, V.props.limits.minUniformBufferOffsetAlignment, &sb, &so);
        memcpy(m, (const void *)(uintptr_t)addr, size); w_buf(ws, ds, b, sb, so, size); return;
    }
    w_buf(ws, ds, b, buf, off, size);
}
static void bind_ssbo_range(wset *ws, VkDescriptorSet ds, const vk_bind *b, uint64_t addr, uint64_t size) {
    VkBuffer buf; VkDeviceSize off;
    uint64_t al = V.props.limits.minStorageBufferOffsetAlignment > 16 ? V.props.limits.minStorageBufferOffsetAlignment : 16;
    uint64_t a0 = addr & ~(al - 1);
    vk_pool *p = addr ? vk_pool_find(a0, 1) : NULL;
    if (!p || !vk_buf_at(a0, 1, &buf, &off)) { w_buf(ws, ds, b, vk_zero_buffer(), 0, 65536); return; }
    uint64_t range = size ? size + (addr - a0) : 16;
    if (a0 + range > p->hi) range = p->hi - a0;
    w_buf(ws, ds, b, buf, off, range);
    if (b->rw) vk_add_readback(vk_sub_current(), a0, range);
}
static VkImageViewType want_vt(const vk_bind *b) {
    switch (b->dim) { case 0: return b->arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D; case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return b->arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE; default: return b->arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; }
}
static void write_stage(wset *ws, VkDescriptorSet sets[2], vk_shader *sh, int st) {
    vk_sub *sub = vk_sub_current();
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i]; VkDescriptorSet ds = sets[b->set > 1 ? 1 : b->set]; int reg = b->reg;
        switch (b->kind) {
            case NVN_RB_CBUF: { const p_bind_buf *u = reg < 16 ? &S.ubo[st][reg] : NULL; bind_ubo_range(ws, ds, b, u ? u->addr : 0, u ? u->size : 0); break; }
            case NVN_RB_SUPPORT: w_buf(ws, ds, b, support_buf(), 0, 65536); break;
            case NVN_RB_C1: if (sh->c1buf) w_buf(ws, ds, b, sh->c1buf, 0, (sh->c1_len + 255) & ~255u); else w_buf(ws, ds, b, vk_zero_buffer(), 0, 65536); break;
            case NVN_RB_DRV0: case NVN_RB_DRV2: {
                int bank = b->kind == NVN_RB_DRV2 ? 2 : 0;
                if (!sh->drv_read[bank]) { w_buf(ws, ds, b, vk_zero_buffer(), 0, 65536); break; }
                VkBuffer sb; VkDeviceSize so; uint8_t *m = vk_arena_alloc(&sub->arena, 0xA00, V.props.limits.minUniformBufferOffsetAlignment, &sb, &so);
                driver_bank_fill(m, bank, st); w_buf(ws, ds, b, sb, so, 0xA00); break; }
            case NVN_RB_SBUFP: { const p_bind_buf *u = reg < 16 ? &S.ssbo[st][reg] : NULL; bind_ssbo_range(ws, ds, b, u ? u->addr : 0, u ? u->size : 0); break; }
            case NVN_RB_UBOP: { const p_bind_buf *u = reg < 16 ? &S.ubo[st][reg] : NULL; bind_ssbo_range(ws, ds, b, u ? u->addr : 0, u ? u->size : 0); break; }
            case NVN_RB_SBUFU: {   // the storage buffer address sits in a user uniform block at byte offset off
                const p_bind_buf *u = reg < 16 ? &S.ubo[st][reg] : NULL; uint64_t va = 0;
                if (u && u->addr && u->size >= (uint64_t)b->off + 8) memcpy(&va, (const void *)(uintptr_t)(u->addr + b->off), 8);
                bind_ssbo_range(ws, ds, b, va, 65536); break; }
            case NVN_RB_TEX: case NVN_RB_SAMP: case NVN_RB_SAMPT: {
                VkImageView iv = VK_NULL_HANDLE; VkSampler sp = VK_NULL_HANDLE;
                if (b->desc == VKD_COMBINED || b->desc == VKD_SAMPLED) {
                    int tid = reg < 128 ? (int)(S.tex[st][reg] & 0xfffff) : 0;
                    const view_t *v = NULL; int hv = 0; vk_tex *x = tid ? vk_tex_by_id(tid, &v, &hv) : NULL;
                    if (x && x->img) iv = vk_tex_view_typed(x, v, hv, want_vt(b), 0);
                    if (!iv) iv = vk_dummy_view(b->dim > 3 ? 1 : b->dim, b->arrayed, b->ms, b->cls, 0);
                }
                if (b->desc == VKD_COMBINED || b->desc == VKD_SAMPLER) {
                    int sk = b->desc == VKD_SAMPLER ? b->kind : b->samp_kind, sr = b->desc == VKD_SAMPLER ? reg : b->samp_reg;
                    int sid = sr < 128 ? (int)((sk == NVN_RB_SAMP ? S.samp[st][sr] : S.tex[st][sr]) >> 20) : 0;
                    sp = sid ? vk_sampler_get(sid) : vk_sampler_default();
                }
                if (b->desc == VKD_UTEXEL) {
                    int tid = reg < 128 ? (int)(S.tex[st][reg] & 0xfffff) : 0; const view_t *v; int hv;
                    vk_tex *x = tid ? vk_tex_by_id(tid, &v, &hv) : NULL; VkBufferView bv = x ? vk_tex_bview(x) : VK_NULL_HANDLE;
                    w_texel(ws, ds, b, bv ? bv : vk_dummy_bview(b->cls, 0)); break;
                }
                w_img(ws, ds, b, sp, iv); break; }
            case NVN_RB_TBUF: {
                int tid = reg < 128 ? (int)(S.tex[st][reg] & 0xfffff) : 0; const view_t *v; int hv;
                vk_tex *x = tid ? vk_tex_by_id(tid, &v, &hv) : NULL; VkBufferView bv = x ? vk_tex_bview(x) : VK_NULL_HANDLE;
                w_texel(ws, ds, b, bv ? bv : vk_dummy_bview(b->cls, b->desc == VKD_STEXEL)); break; }
            case NVN_RB_IMG: {
                int tid = reg < 32 ? (int)(S.img[st][reg] & 0xfffff) : 0; const view_t *v = NULL; int hv = 0;
                vk_tex *x = tid ? vk_tex_by_id(tid, &v, &hv) : NULL;
                if (b->desc == VKD_STEXEL) { VkBufferView bv = x ? vk_tex_bview(x) : VK_NULL_HANDLE; w_texel(ws, ds, b, bv ? bv : vk_dummy_bview(b->cls, 1));
                    if (x && bv) vk_add_readback(sub, (uintptr_t)x->t.mem, x->t.L.size); break; }
                VkImageView iv = x && x->img ? vk_tex_view_typed(x, v, hv, want_vt(b), 1) : VK_NULL_HANDLE;
                if (iv) vk_tex_mark_written(x); else iv = vk_dummy_view(b->dim > 3 ? 1 : b->dim, b->arrayed, 0, b->cls, 1);
                w_img(ws, ds, b, VK_NULL_HANDLE, iv); break; }
            default: VKONCE("binding kind %d not implemented", b->kind); break;
        }
    }
}
static int bind_descriptors(vk_prog *pr, VkPipelineBindPoint bp) {
    VkDescriptorSet sets[2] = {vk_desc_alloc(pr->dsl[0]), vk_desc_alloc(pr->dsl[1])};
    static wset ws; ws.n = 0;
    for (int st = 0; st < 6; st++) if (pr->sh[st]) write_stage(&ws, sets, pr->sh[st], st);
    if (ws.n) vkUpdateDescriptorSets(V.dev, (uint32_t)ws.n, ws.w, 0, NULL);
    vkCmdBindDescriptorSets(CB(), bp, pr->layout, 0, 2, sets, 0, NULL);
    return 1;
}

// ---------- draws ----------
static void draw(const p_draw *dw, const p_draw_idx *di) {
    if (S.render_disabled || !S.prog) return;
    int count = dw ? dw->count : di->count;
    if (count <= 0) return;
    vk_prog *pr = vk_prog_get(S.prog);
    if (!pr || !pr->sh[0]) { VKONCE("draw skipped: program without a vertex shader variant"); atomic_fetch_add(&g_stat_skip_shader, 1); return; }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) return;
    if (!rt_has_any() && !S.has_vp && !R.has_sc) { VKONCE("draw without render targets or viewport skipped"); return; }
    pass_begin();
    VkCommandBuffer cb = CB();
    int quads; VkPrimitiveTopology topo = topo_of(dw ? dw->prim : di->prim, &quads);
    VkPipeline pipe = pso_get(pr, topo); if (!pipe) { atomic_fetch_add(&g_stat_skip_pipe, 1); return; }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    bind_descriptors(pr, VK_PIPELINE_BIND_POINT_GRAPHICS);
    // dynamic state
    float vx = S.has_vp ? S.vp[0][0] : 0, vy = S.has_vp ? S.vp[0][1] : 0, vw = S.has_vp ? S.vp[0][2] : (float)P.w, vh = S.has_vp ? S.vp[0][3] : (float)P.h;
    float n = S.dr[0], f = S.dr[1]; if (n < 0) n = 0; if (n > 1) n = 1; if (f < 0) f = 0; if (f > 1) f = 1;
    VkViewport vp = {vx, vy + vh, vw, -vh, n, f};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc = {{0, 0}, {P.w, P.h}};
    if (R.has_sc) {
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)P.w) x1 = P.w; if (y1 > (long)P.h) y1 = P.h;
        if (x1 <= x0 || y1 <= y0) return;
        sc = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    }
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetCullMode(cb, (VkCullModeFlags)S.poly.cull);
    vkCmdSetFrontFace(cb, S.poly.front == NVN_FRONT_FACE_CCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    int pm = S.poly.mode, bias_on = pm == NVN_POLYGON_MODE_POINT ? (S.poly.offset_en & 1) : pm == NVN_POLYGON_MODE_LINE ? (S.poly.offset_en & 2) : (S.poly.offset_en & 4);
    vkCmdSetDepthBiasEnable(cb, bias_on ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthBias(cb, S.po.units, V.f_wide_lines || 1 ? S.po.clamp : 0, S.po.factor);
    vkCmdSetBlendConstants(cb, S.blend_color);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_vmask[0]); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, S.st_vmask[1]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_wmask[0]); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, S.st_wmask[1]);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_ref[0]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, S.st_ref[1]);
    // vertex buffers
    vk_shader *vs = pr->sh[0]; uint32_t used = 0; int need_dummy = 0;
    for (int i = 0; i < vs->nv; i++) { va_st a; uint32_t raw = S.attr[vs->v[i].loc & 15]; memcpy(&a, &raw, 4);
        if (a.fmt && nvn_vertex_fmt_vk((int)a.fmt)) used |= 1u << a.stream; else need_dummy = 1; }
    for (int s = 0; s < 16; s++) if (used & (1u << s)) {
        VkBuffer b; VkDeviceSize off;
        if (S.vbuf[s].address && vk_buf_at(S.vbuf[s].address, 1, &b, &off)) vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &b, &off);
        else { VkBuffer z = vk_zero_buffer(); VkDeviceSize zo = 0; vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &z, &zo); }
    }
    if (need_dummy) { VkBuffer z = vk_zero_buffer(); VkDeviceSize zo = 0; vkCmdBindVertexBuffers(cb, 16, 1, &z, &zo); }
    uint32_t inst = dw ? (dw->instances > 0 ? (uint32_t)dw->instances : 1) : (di->instances > 0 ? (uint32_t)di->instances : 1);
    vk_sub *sub = vk_sub_current();
    if (dw && !quads) vkCmdDraw(cb, (uint32_t)dw->count, inst, (uint32_t)dw->first, (uint32_t)dw->base_instance);
    else if (dw) {   // quads: generated triangle indices 0,1,2 0,2,3 per quad
        uint32_t nq = (uint32_t)dw->count / 4; if (!nq) return;
        VkBuffer ib; VkDeviceSize io; uint32_t *ix = vk_arena_alloc(&sub->arena, (VkDeviceSize)nq * 24, 16, &ib, &io);
        for (uint32_t q = 0; q < nq; q++) { uint32_t b0 = 4 * q; uint32_t t[6] = {b0, b0 + 1, b0 + 2, b0, b0 + 2, b0 + 3}; memcpy(ix + 6 * q, t, 24); }
        vkCmdBindIndexBuffer(cb, ib, io, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, nq * 6, inst, 0, dw->first, (uint32_t)dw->base_instance);
    } else {
        int isz = di->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE ? 1 : di->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4;
        VkBuffer ib; VkDeviceSize io;
        if (quads || (isz == 1 && !V.f_index8)) {   // expand / widen on the CPU from the guest index data
            const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf;
            uint32_t nout = quads ? (uint32_t)(di->count / 4) * 6 : (uint32_t)di->count;
            uint32_t *ix = vk_arena_alloc(&sub->arena, (VkDeviceSize)nout * 4 + 4, 16, &ib, &io);
            for (uint32_t o = 0; o < nout; o++) {
                uint32_t k = quads ? (o / 6) * 4 + (uint32_t[]){0, 1, 2, 0, 2, 3}[o % 6] : o, v;
                if (isz == 1) v = src[k]; else if (isz == 2) { uint16_t h; memcpy(&h, src + 2 * k, 2); v = h; } else memcpy(&v, src + 4 * k, 4);
                ix[o] = v;
            }
            vkCmdBindIndexBuffer(cb, ib, io, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, nout, inst, 0, di->base_vertex, (uint32_t)di->base_instance);
        } else {
            if (!vk_buf_at(di->ibuf, (uint64_t)di->count * isz, &ib, &io)) { VKONCE("index buffer not in a memory pool"); return; }
            vkCmdBindIndexBuffer(cb, ib, io, isz == 1 ? VK_INDEX_TYPE_UINT8_EXT : isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, (uint32_t)di->count, inst, 0, di->base_vertex, (uint32_t)di->base_instance);
        }
    }
    atomic_fetch_add(&g_stat_draws, 1);
}
static void dispatch(const p_dispatch *dp) {
    if (!S.prog) return;
    vk_prog *pr = vk_prog_get(S.prog);
    if (!pr || !pr->sh[5]) { VKONCE("dispatch skipped: program without a compute variant"); return; }
    pass_end();
    VkPipeline pipe = cps_get(pr); if (!pipe) return;
    g_dispatch_grid[0] = (uint32_t)dp->x; g_dispatch_grid[1] = (uint32_t)dp->y; g_dispatch_grid[2] = (uint32_t)dp->z;
    vkCmdBindPipeline(CB(), VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    bind_descriptors(pr, VK_PIPELINE_BIND_POINT_COMPUTE);
    vkCmdDispatch(CB(), (uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z);
    barrier_full(CB());
    atomic_fetch_add(&g_stat_dispatches, 1);
}

// ---------- clears ----------
static VkRect2D clear_rect(void) {
    VkRect2D r = {{0, 0}, {P.w, P.h}};
    if (R.has_sc) {
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)P.w) x1 = P.w; if (y1 > (long)P.h) y1 = P.h;
        if (x1 <= x0 || y1 <= y0) { r.extent.width = r.extent.height = 0; return r; }
        r = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    }
    return r;
}
static void clear_color(const p_clear_color *c) {
    if (S.render_disabled || c->index < 0 || c->index >= 8 || !R.has_rt || c->index >= R.rt.n || !R.rt.color[c->index]) return;
    if (!(c->mask & 0xf)) return;
    if ((c->mask & 0xf) != 0xf) VKONCE("colour clear with a partial channel mask clears all channels");
    pass_begin();
    if (!P.cview[c->index]) return;
    VkRect2D r = clear_rect(); if (!r.extent.width) return;
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, {{{c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]}}}};
    VkFormat f = P.cfmt[c->index];
    int isint = (f >= VK_FORMAT_R8_UINT && f <= VK_FORMAT_R8_SINT && (f == VK_FORMAT_R8_UINT || f == VK_FORMAT_R8_SINT));
    (void)isint;   // integer targets receive the float bits converted by the driver as NVN does (ClearColor takes floats)
    VkClearRect cr = {r, 0, P.layers};
    vkCmdClearAttachments(CB(), 1, &a, 1, &cr);
}
static void clear_ds(const p_clear_ds *c) {
    if (S.render_disabled || !R.has_rt || !R.rt.depth) return;
    pass_begin();
    VkImageAspectFlags asp = 0;
    if (c->depth_mask && P.dfmt) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((c->stencil_mask & 255) && P.sfmt) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!asp) return;
    if ((c->stencil_mask & 255) && (c->stencil_mask & 255) != 255) VKONCE("stencil clear with a partial mask clears all bits");
    VkRect2D r = clear_rect(); if (!r.extent.width) return;
    VkClearAttachment a = {asp, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect cr = {r, 0, P.layers};
    vkCmdClearAttachments(CB(), 1, &a, 1, &cr);
}
static void clear_tex(const p_clear_tex *c) {
    vk_tex *x = vk_tex_get(c->tex, 1); if (!x || !x->img) return;
    pass_end();
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, ly = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    VkImageSubresourceRange rr = {x->aspect, lv, 1, ly + (x->type == VK_IMAGE_TYPE_3D ? 0 : (uint32_t)c->r.zoffset), x->type == VK_IMAGE_TYPE_3D ? 1 : (uint32_t)(c->r.depth > 0 ? c->r.depth : 1)};
    if (c->r.xoffset || c->r.yoffset || (uint32_t)c->r.width < mipd(x->w, lv) || (uint32_t)c->r.height < mipd(x->h, lv)) VKONCE("ClearTexture of a sub-rectangle clears the whole level");
    if (x->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue v; memcpy(&v, &c->c, 16);
        vkCmdClearColorImage(CB(), x->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rr);
    } else {
        VkClearDepthStencilValue v = {c->c.f[0], c->c.u[1]};
        vkCmdClearDepthStencilImage(CB(), x->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rr);
    }
    barrier_full(CB());
}

// ---------- copies ----------
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (!V.ok || !n) return;
    VkBuffer sb, db; VkDeviceSize so, doff;
    if (!vk_buf_at(src, n, &sb, &so) || !vk_buf_at(dst, n, &db, &doff)) { VKONCE("buffer copy outside the memory pools (%#llx -> %#llx, %llu bytes)", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n); return; }
    pass_end();
    VkCommandBuffer cb = CB();
    if (sb == db && so < doff + n && doff < so + n) {   // overlapping in one buffer: through the arena
        VkBuffer tb; VkDeviceSize to; vk_arena_alloc(&vk_sub_current()->arena, n, 16, &tb, &to);
        VkBufferCopy c1 = {so, to, n}; vkCmdCopyBuffer(cb, sb, tb, 1, &c1); barrier_full(cb);
        VkBufferCopy c2 = {to, doff, n}; vkCmdCopyBuffer(cb, tb, db, 1, &c2);
    } else { VkBufferCopy c = {so, doff, n}; vkCmdCopyBuffer(cb, sb, db, 1, &c); }
    barrier_full(cb);
    vk_add_readback(vk_sub_current(), dst, n);
}
static void update_ub(const void *data, uint64_t dst, uint64_t n) {
    VkBuffer db; VkDeviceSize doff;
    if (!vk_buf_at(dst, n, &db, &doff)) { VKONCE("uniform update outside the memory pools"); return; }
    pass_end();
    VkBuffer sb; VkDeviceSize so; void *m = vk_arena_alloc(&vk_sub_current()->arena, n, 16, &sb, &so); memcpy(m, data, n);
    VkBufferCopy c = {so, doff, n}; vkCmdCopyBuffer(CB(), sb, db, 1, &c);
    barrier_full(CB());
}
static void tex_region(const vk_tex *x, const view_t *v, int hv, const NVNcopyRegion *r, VkImageSubresourceLayers *sl, VkOffset3D *o, VkExtent3D *e) {
    unsigned lv = hv && v->set_levels ? v->base_level : 0, ly = hv && v->set_layers ? v->min_layer : 0;
    int is3d = x->type == VK_IMAGE_TYPE_3D;
    VkImageAspectFlags asp = x->aspect; if (asp == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) asp = VK_IMAGE_ASPECT_DEPTH_BIT;
    *sl = (VkImageSubresourceLayers){asp, lv, is3d ? 0 : ly + (uint32_t)r->zoffset, is3d ? 1 : (uint32_t)(r->depth > 0 ? r->depth : 1)};
    *o = (VkOffset3D){r->xoffset, r->yoffset, is3d ? r->zoffset : 0};
    *e = (VkExtent3D){(uint32_t)(r->width > 0 ? r->width : 1), (uint32_t)(r->height > 0 ? r->height : 1), is3d ? (uint32_t)(r->depth > 0 ? r->depth : 1) : 1};
    if (x->type == VK_IMAGE_TYPE_1D) { o->y = 0; e->height = 1; }
}
static void copy_b2t(const p_copy_b2t *c) {
    vk_tex *x = vk_tex_get(c->tex, 1); if (!x || !x->img) return;
    int bw, bh, bpb = nvn_fmt_bpb(x->t.d.format, &bw, &bh); if (!bpb) return;
    VkImageSubresourceLayers sl; VkOffset3D o; VkExtent3D e; tex_region(x, &c->view, c->has_view, &c->r, &sl, &o, &e);
    uint32_t row_len = S.copy_stride > 0 ? (uint32_t)(S.copy_stride / bpb) * (uint32_t)bw : 0;
    pass_end();
    VkBuffer sb; VkDeviceSize so;
    if (x->decoded) {   // ASTC / BC on a device without them: decode the source bytes on the CPU
        int abw, abh, srgb; const uint8_t *src = (const uint8_t *)(uintptr_t)c->src;
        size_t pitch = S.copy_stride > 0 ? (size_t)S.copy_stride : (size_t)((e.width + bw - 1) / bw) * bpb;
        uint8_t *m = vk_arena_alloc(&vk_sub_current()->arena, (VkDeviceSize)e.width * e.height * 8 * sl.layerCount, 16, &sb, &so);
        if (nvn_fmt_is_astc(x->t.d.format, &abw, &abh, &srgb)) nvn_astc_decode_image(src, pitch, abw, abh, e.width, e.height, srgb, m, (size_t)e.width * 4);
        else { extern int nvn_bc_decode(uint32_t, const uint8_t *, size_t, uint32_t, uint32_t, uint8_t *, size_t); uint32_t pf; int obpp; extern int nvn_bc_unpacked(uint32_t, uint32_t *, int *);
               nvn_bc_unpacked((uint32_t)x->t.d.format, &pf, &obpp); nvn_bc_decode((uint32_t)x->t.d.format, src, pitch, e.width, e.height, m, (size_t)e.width * obpp); }
        row_len = 0;
    } else if (!vk_buf_at(c->src, 1, &sb, &so)) { VKONCE("CopyBufferToTexture source not in a memory pool"); return; }
    VkBufferImageCopy bc = {so, row_len, 0, sl, o, e};
    vkCmdCopyBufferToImage(CB(), sb, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    barrier_full(CB());
}
static void copy_t2b(const p_copy_b2t *c) {   // p_copy_b2t layout reused: src = destination buffer address
    vk_tex *x = vk_tex_get(c->tex, 0); if (!x || !x->img) return;
    int bw, bh, bpb = nvn_fmt_bpb(x->t.d.format, &bw, &bh); if (!bpb) return;
    if (x->decoded) { VKONCE("CopyTextureToBuffer from a CPU-decoded compressed texture: skipped"); return; }
    VkImageSubresourceLayers sl; VkOffset3D o; VkExtent3D e; tex_region(x, &c->view, c->has_view, &c->r, &sl, &o, &e);
    uint32_t row_len = S.copy_stride > 0 ? (uint32_t)(S.copy_stride / bpb) * (uint32_t)bw : 0;
    uint64_t bytes = (uint64_t)(row_len ? row_len / bw * bpb : (e.width + bw - 1) / bw * bpb) * ((e.height + bh - 1) / bh) * e.depth * sl.layerCount;
    VkBuffer db; VkDeviceSize doff;
    if (!vk_buf_at(c->src, bytes, &db, &doff)) { VKONCE("CopyTextureToBuffer destination not in a memory pool"); return; }
    pass_end();
    VkBufferImageCopy bc = {doff, row_len, 0, sl, o, e};
    vkCmdCopyImageToBuffer(CB(), x->img, VK_IMAGE_LAYOUT_GENERAL, db, 1, &bc);
    barrier_full(CB());
    vk_add_readback(vk_sub_current(), c->src, bytes);
}
static void copy_t2t(const p_copy_t2t *c) {
    vk_tex *s = vk_tex_get(c->src, 0), *d = vk_tex_get(c->dst, 1);
    if (!s || !d || !s->img || !d->img) return;
    pass_end();
    VkImageSubresourceLayers ss, ds; VkOffset3D so, dof; VkExtent3D se, de;
    tex_region(s, &c->sv, c->has_sv, &c->sr, &ss, &so, &se); tex_region(d, &c->dv, c->has_dv, &c->dr, &ds, &dof, &de);
    if (se.width == de.width && se.height == de.height && se.depth == de.depth && ss.layerCount == ds.layerCount &&
        (s->fmt == d->fmt || (!(s->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) && !(d->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)))) {
        VkImageCopy ic = {ss, so, ds, dof, se};
        vkCmdCopyImage(CB(), s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    } else {
        VkImageBlit bl = {ss, {so, {so.x + (int32_t)se.width, so.y + (int32_t)se.height, so.z + (int32_t)se.depth}},
                          ds, {dof, {dof.x + (int32_t)de.width, dof.y + (int32_t)de.height, dof.z + (int32_t)de.depth}}};
        int depth = (s->aspect | d->aspect) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        vkCmdBlitImage(CB(), s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, (c->flags & 1) && !depth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    barrier_full(CB());
}

// ---------- state ops ----------
static int cond_eval(const p_render_cond *c) {
    if (!vk_pool_find(c->addr, 32)) return 1;
    uint64_t x, y; memcpy(&x, (const void *)(uintptr_t)c->addr, 8); memcpy(&y, (const void *)(uintptr_t)(c->addr + 16), 8);
    return c->mode == 0 ? x == y : x != y;
}
static void state_op(uint32_t op, const void *a) {
    switch (op) {
        case OP_SCISSOR: R.sc = *(const p_scissor *)a; R.has_sc = 1; break;
        case OP_BIND_PROGRAM: S.prog = ((const p_bind_program *)a)->prog; break;
        case OP_BIND_STATE: {
            const p_bind_state *b = a; blend_st bs;
            switch (b->kind) {
                case 0: memcpy(&bs, b->bytes, sizeof bs); if (bs.target < 8) S.blend[bs.target] = bs; break;
                case 1: memcpy(&S.color, b->bytes, sizeof S.color); break;
                case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
                case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break;
                case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break;
                case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break;
            }
            break; }
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) S.attr[b->first + i] = b->st[i]; if (b->first + b->n > S.nattr) S.nattr = b->first + b->n; break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) S.stream[b->first + i] = b->st[i]; if (b->first + b->n > S.nstream) S.nstream = b->first + b->n; break; }
        case OP_BIND_VBUF: { const p_bind_vbuf *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) S.vbuf[b->first + i] = b->r[i]; break; }
        case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16) S.ubo[b->stage][b->index] = *b; break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16) S.ssbo[b->stage][b->index] = *b; break; }
        case OP_BIND_TEX: { const p_bind_handles *b = a; if (b->stage < 6 && b->first + b->n <= 128) memcpy(&S.tex[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage < 6 && b->first + b->n <= 128) memcpy(&S.samp[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) memcpy(&S.img[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
        case OP_STENCIL: { const p_stencil *b = a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = (uint32_t)b->value_mask; else if (b->which == 1) S.st_wmask[f] = (uint32_t)b->mask; else S.st_ref[f] = (uint32_t)b->ref; } break; }
        case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
        case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(S.vp[b->first + i], b->v[i], 16); S.has_vp = 1; break; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
        case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_COPY_STRIDE: S.copy_stride = ((const p_copy_stride *)a)->stride; break;
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_flag *)a)->value; break;
        case OP_RENDER_ENABLE: S.render_disabled = !((const p_flag *)a)->value; break;
        case OP_RENDER_COND: S.render_disabled = !cond_eval(a); break;
        case OP_RASTER_DISCARD: S.raster_discard = ((const p_flag *)a)->value; break;
    }
}
static int rt_same(const p_set_rt *a, const p_set_rt *b) {
    if (a->n != b->n || a->depth != b->depth || a->has_dview != b->has_dview) return 0;
    if ((a->depth && b->depth && a->depth->serial != b->depth->serial)) return 0;
    for (int i = 0; i < a->n && i < 8; i++) {
        if (!a->color[i] != !b->color[i] || a->has_cview[i] != b->has_cview[i]) return 0;
        if (a->color[i] && a->color[i]->serial != b->color[i]->serial) return 0;
        if (a->has_cview[i] && memcmp(&a->cview[i], &b->cview[i], sizeof a->cview[i])) return 0;
    }
    if (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview)) return 0;
    return 1;
}

void mtl_exec(const uint8_t *p) {
    if (!V.ok) return;
    if (!g_inited) { g_inited = 1; state_reset(); }
    vk_sub_current();
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) VKDIE("corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size);
        const void *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_BEGIN: break;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); mtl_exec((const uint8_t *)(uintptr_t)t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
            case OP_RESET_COUNTER: break;
            case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8); mtl_sub_copy(s_, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8); update_ub((const uint8_t *)a + 16, d, n); break; }
            case OP_SET_RT: { const p_set_rt *r = a; if (P.open && R.has_rt && rt_same(&R.rt, r)) { R.rt = *r; break; } pass_end(); R.rt = *r; R.has_rt = 1; break; }
            case OP_CLEAR_COLOR: clear_color(a); break;
            case OP_CLEAR_DS: clear_ds(a); break;
            case OP_DRAW: draw(a, NULL); break;
            case OP_DRAW_IDX: draw(NULL, a); break;
            case OP_COPY_B2T: copy_b2t(a); break;
            case OP_COPY_T2B: copy_t2b(a); break;
            case OP_COPY_T2T: copy_t2t(a); break;
            case OP_CLEAR_TEX: clear_tex(a); break;
            case OP_DISPATCH: dispatch(a); break;
            case OP_BARRIER: if (P.open) { /* a barrier inside a pass: end it so later reads see the writes */ pass_end(); } else barrier_full(CB()); break;
            case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
            default: state_op(h.op, a); break;
        }
        p += h.size;
    }
}
/* translator answers from the bound state (nvn_mtl.m rt_answers): texture slot types (depth formats with _depth), image
   formats, vertex attribute / colour output classes */
int vk_reg_info(int tid, int *target, int *format);
static const char *target_name(int t) {
    switch (t) { case 0: return "1d"; case 2: return "3d"; case 3: return "1darray"; case 4: return "2darray"; case 5: return "2dms";
                 case 6: return "2dmsarray"; case 8: return "cube"; case 9: return "cubearray"; case 10: return "buffer"; default: return "2d"; }
}
static const char *fmt_class(int f) {
    switch (f) {
        case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23: case 0x27: case 0x2C: case 0x2F: return "uint";
        case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24: case 0x28: case 0x2D: case 0x30: return "sint";
        default: return "float";
    }
}
static const char *image_format(int f) {
    switch (f) {
        case 0x01: return "R8Unorm"; case 0x0D: return "R8G8Unorm"; case 0x25: return "R8G8B8A8Unorm"; case 0x27: return "R8G8B8A8Uint";
        case 0x05: return "R16Float"; case 0x11: return "R16G16Float"; case 0x29: return "R16G16B16A16Float"; case 0x08: return "R16Uint"; case 0x2C: return "R16G16B16A16Uint";
        case 0x0A: return "R32Float"; case 0x16: return "R32G32Float"; case 0x2E: return "R32G32B32A32Float"; case 0x0B: return "R32Uint"; case 0x0C: return "R32Sint";
        case 0x17: return "R32G32Uint"; case 0x2F: return "R32G32B32A32Uint"; case 0x3D: return "R10G10B10A2Unorm"; case 0x3F: return "R11G11B10Float";
        default: return NULL;
    }
}
int vk_exec_answers(int st, char *buf, size_t n) {
    if (!g_inited) { g_inited = 1; state_reset(); }
    size_t o = 0; int first;
#define AP(...) do { int k_ = snprintf(buf + o, o < n ? n - o : 0, __VA_ARGS__); if (k_ > 0) o += (size_t)k_; } while (0)
    AP("{\"tex\": {"); first = 1;
    for (int r = 0; r < 128 && st < 6; r++) if (S.tex[st][r]) { int tg, f; if (!vk_reg_info((int)(S.tex[st][r] & 0xfffff), &tg, &f)) continue;
        AP("%s\"%d\": \"%s%s\"", first ? "" : ", ", r, target_name(tg), f >= 0x32 && f <= 0x36 ? "_depth" : ""); first = 0; }
    AP("}, \"img\": {"); first = 1;
    for (int r = 0; r < 32 && st < 6; r++) if (S.img[st][r]) { int tg, f; if (!vk_reg_info((int)(S.img[st][r] & 0xfffff), &tg, &f)) continue;
        const char *fn = image_format(f);
        if (fn) AP("%s\"%d\": {\"type\": \"%s\", \"fmt\": \"%s\"}", first ? "" : ", ", r, target_name(tg), fn); else AP("%s\"%d\": {\"type\": \"%s\"}", first ? "" : ", ", r, target_name(tg));
        first = 0; }
    AP("}, \"attr\": {"); first = 1;
    if (st == 0) for (int i = 0; i < S.nattr && i < 16; i++) { va_st a; memcpy(&a, &S.attr[i], 4); AP("%s\"%d\": \"%s\"", first ? "" : ", ", i, fmt_class((int)a.fmt)); first = 0; }
    AP("}, \"fout\": {"); first = 1;
    if (st == 1 && R.has_rt) for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) {
        AP("%s\"%d\": \"%s\"", first ? "" : ", ", i, fmt_class(R.rt.has_cview[i] && R.rt.cview[i].set_format ? R.rt.cview[i].format : R.rt.color[i]->d.format)); first = 0; }
    AP("}}");
#undef AP
    return (int)o;
}
/* The same facts as vk_exec_answers, hashed instead of written as text: the per-draw program lookup key (formatting the
   answers with snprintf at every draw took about half of the submission CPU time). */
static inline uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0x100000001B3ull; }
uint64_t vk_exec_answer_key(int st, uint64_t h) {
    if (!g_inited) { g_inited = 1; state_reset(); }
    if (st >= 6) return h;
    h = mix(h, 0x7e5 + (uint64_t)st);
    for (int r = 0; r < 128; r++) if (S.tex[st][r]) { int tg, f; if (!vk_reg_info((int)(S.tex[st][r] & 0xfffff), &tg, &f)) continue;
        h = mix(h, (uint64_t)r << 32 | (uint64_t)(tg & 0xff) << 8 | (f >= 0x32 && f <= 0x36)); }
    h = mix(h, 0x1a6);
    for (int r = 0; r < 32; r++) if (S.img[st][r]) { int tg, f; if (!vk_reg_info((int)(S.img[st][r] & 0xfffff), &tg, &f)) continue;
        h = mix(h, (uint64_t)r << 32 | (uint64_t)(tg & 0xff) << 16 | (uint64_t)(image_format(f) ? (unsigned)f & 0xffff : 0xffff)); }
    h = mix(h, 0xa77);
    if (st == 0) for (int i = 0; i < S.nattr && i < 16; i++) { va_st a; memcpy(&a, &S.attr[i], 4); h = mix(h, (uint64_t)i << 8 | (uint64_t)(uintptr_t)fmt_class((int)a.fmt)[0]); }
    h = mix(h, 0xf0a);
    if (st == 1 && R.has_rt) for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i])
        h = mix(h, (uint64_t)i << 8 | (uint64_t)(uintptr_t)fmt_class(R.rt.has_cview[i] && R.rt.cview[i].set_format ? R.rt.cview[i].format : R.rt.color[i]->d.format)[0]);
    return h;
}
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
/* Uniform contents as of the submit call: the pools are synchronized when the replay of the submission begins
   (mtl_sub_begin), which is where this backend takes the CPU's memory contents. */
void *mtl_sub_capture_ubos(const uint64_t *h, int n) { (void)h; (void)n; return NULL; }
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *cap) { (void)h; (void)n; (void)cap; }
