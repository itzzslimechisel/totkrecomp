// Vulkan NVN backend: textures, views, samplers, the texture / sampler descriptor registry, uploads.
// Images live in VK_IMAGE_LAYOUT_GENERAL for their whole life (one layout, no per-subresource tracking; barriers are
// memory barriers). Textures in CPU-visible pools ("mirrored", as nvn_mtl.m tex_mirrored) are uploaded from the pool
// bytes (block-linear deswizzle, nvn_deswizzle.c) and uploaded again when those bytes change, unless the GPU rendered
// into them since. Formats the device cannot sample are converted on upload: BC (no textureCompressionBC: Adreno) via
// bcdec, ASTC (no textureCompressionASTC_LDR: NVIDIA / AMD / Intel desktop) via nvn_astc_dec.c, 3-component formats
// expanded to 4 components.
#include "nvn_vk.h"
#include "nvn_fmt_vk.h"
#include "nvn_deswizzle.h"
#include "nvn_bcdec.h"
#include "nvn_astc_dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------- registry (texture id / sampler id -> object snapshot), written by game threads ----------
typedef struct { tex_t t; view_t v; uint8_t has_v, ok; } treg_t;
static treg_t *g_treg; static int g_ntreg;
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static int g_nsreg;
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id > (1 << 20)) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_ntreg) { int n = g_ntreg ? g_ntreg : 1024; while (n <= id) n *= 2; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (size_t)(n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    g_treg[id].t = *t; g_treg[id].has_v = v != NULL; if (v) g_treg[id].v = *v; g_treg[id].ok = 1;
    pthread_mutex_unlock(&g_rmu);
}
void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0 || id > (1 << 12)) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_nsreg) { int n = g_nsreg ? g_nsreg : 256; while (n <= id) n *= 2; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n);
        memset(g_sreg_ok + g_nsreg, 0, (size_t)(n - g_nsreg)); g_nsreg = n; }
    g_sreg[id] = *p; g_sreg_ok[id] = 1;
    pthread_mutex_unlock(&g_rmu);
}

/* target / format of a registered texture id (translator answers), 0 if unregistered */
int vk_reg_info(int tid, int *target, int *format) {
    int ok = 0;
    pthread_mutex_lock(&g_rmu);
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) {
        const treg_t *r = &g_treg[tid]; ok = 1;
        *target = r->has_v && r->v.set_target ? r->v.target : r->t.d.target;
        *format = r->has_v && r->v.set_format ? r->v.format : r->t.d.format;
    }
    pthread_mutex_unlock(&g_rmu);
    return ok;
}
// ---------- texture table by serial ----------
#define TB 4096
static vk_tex *g_tab[TB]; static uint64_t g_live;
static pthread_mutex_t g_tmu = PTHREAD_MUTEX_INITIALIZER;
uint64_t vk_tex_live(void) { return g_live; }
static vk_tex *tab_get(uint64_t s) { for (vk_tex *x = g_tab[s % TB]; x; x = x->hnext) if (x->serial == s) return x; return NULL; }

VkFormat vk_format_of(int f) { nvn_vkfmt m; return nvn_fmt_vk(f, &m) ? (VkFormat)m.vk : VK_FORMAT_UNDEFINED; }
static VkImageAspectFlags aspect_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_D16_UNORM_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
static int fmt_compressed(VkFormat f) { return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK); }

/* nvn_mtl.m tex_mirrored: CPU-visible pools hold texture contents written by the CPU (streamed assets) */
static int tex_mirrored(const tex_t *t) {
    if (!t || !t->mem) return 0;
    if (t->has_layout) return 1;
    const pool_t *p = t->pool;
    if (!p || p->host || !(p->flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED))) return 0;
    if (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL |
                      NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL)) return 0;
    if (t->d.samples > 1 || t->d.target == 10) return 0;
    if (nvn_fmt_is_depth(t->d.format)) return 0;
    return 1;
}

static void image_barrier_general(VkCommandBuffer cb, VkImage img, VkImageAspectFlags asp, VkImageLayout from) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, from, VK_IMAGE_LAYOUT_GENERAL, 0, 0, img,
        {asp, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
}

static uint32_t mipd(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }
/* Uploads every level / layer of x from the pool bytes into the image, through the arena of submission s (pre cb). */
static int tex_upload(vk_tex *x, vk_sub *s) {
    const tex_t *t = &x->t; const nvn_texdesc *d = &t->d;
    nvn_vkfmt m; nvn_fmt_vk(d->format, &m);
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb) return 0;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    int astc_bw = 0, astc_bh = 0, astc_srgb = 0; int is_astc = nvn_fmt_is_astc(d->format, &astc_bw, &astc_bh, &astc_srgb);
    VkBufferImageCopy *rg = calloc((size_t)x->levels * x->layers, sizeof *rg); int nrg = 0;
    VkBuffer sbuf = VK_NULL_HANDLE; uint64_t bytes = 0;
    for (uint32_t lv = 0; lv < x->levels; lv++) {
        uint32_t w = mipd(d->width, lv), h = (d->target == 0 || d->target == 3) ? 1 : mipd(d->height, lv), dz = d->target == 2 ? mipd(d->depth ? d->depth : 1, lv) : 1;
        uint32_t rows = (h + bh - 1) / bh, row_bytes = (w + bw - 1) / bw * (uint32_t)bpb;
        size_t lin_size = (size_t)row_bytes * rows * dz;
        uint8_t *lin = malloc(lin_size ? lin_size : 1);
        for (uint32_t ly = 0; ly < x->layers; ly++) {
            if (nvn_deswizzle(d, lv, ly, t->mem, lin, row_bytes)) { VKONCE("texture upload: deswizzle failed (format %#x target %d)", d->format, d->target); free(lin); free(rg); return 0; }
            uint8_t *src = lin; size_t sz = lin_size; uint32_t out_row = row_bytes; uint8_t *tmp = NULL;
            if (x->decoded && is_astc) {
                out_row = w * 4; sz = (size_t)out_row * h * dz; tmp = malloc(sz);
                for (uint32_t z = 0; z < dz; z++) nvn_astc_decode_image(lin + (size_t)z * row_bytes * rows, row_bytes, astc_bw, astc_bh, w, h, astc_srgb, tmp + (size_t)z * out_row * h, out_row);
                src = tmp;
            } else if (x->decoded && m.conv == NVN_VKCONV_BC) {
                uint32_t pf; int obpp; nvn_bc_unpacked((uint32_t)d->format, &pf, &obpp);
                out_row = w * (uint32_t)obpp; sz = (size_t)out_row * h * dz; tmp = malloc(sz);
                for (uint32_t z = 0; z < dz; z++) nvn_bc_decode((uint32_t)d->format, lin + (size_t)z * row_bytes * rows, row_bytes, w, h, tmp + (size_t)z * out_row * h, out_row);
                src = tmp;
            } else if (m.conv == NVN_VKCONV_RGB_EXPAND) {
                int comp = bpb / 3;   // bytes per component (1, 2, 4)
                out_row = w * (uint32_t)comp * 4; sz = (size_t)out_row * h * dz; tmp = malloc(sz);
                int isint = (d->format >= 0x1B && d->format <= 0x1C) || (d->format >= 0x20 && d->format <= 0x21) || d->format == 0x23 || d->format == 0x24 || (d->format >= 0x5D && d->format <= 0x62);
                for (size_t px = 0; px < (size_t)w * h * dz; px++) {
                    uint8_t *o = tmp + px * comp * 4; const uint8_t *i = lin + (px / w) * row_bytes + (px % w) * (size_t)bpb;
                    memcpy(o, i, (size_t)bpb);
                    if (comp == 1) o[3] = isint ? 1 : 0xFF;
                    else if (comp == 2) { uint16_t one = isint ? 1 : (d->format == 0x1D ? 0x3C00 : 0xFFFF); memcpy(o + 6, &one, 2); }
                    else { uint32_t one = isint ? 1 : 0x3F800000; memcpy(o + 12, &one, 4); }
                }
                src = tmp;
            } else if (m.conv == NVN_VKCONV_D24) {
                VKONCE("24-bit depth texture contents uploaded as float depth: converted");
                sz = (size_t)w * h * dz * (x->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? 4 : 4); tmp = malloc(sz); out_row = w * 4;
                for (size_t px = 0; px < (size_t)w * h * dz; px++) { uint32_t v; memcpy(&v, lin + px * 4, 4); float f = (float)(v & 0xFFFFFF) / 16777215.0f; memcpy(tmp + px * 4, &f, 4); }
                src = tmp;
            }
            VkDeviceSize so; uint8_t *dst = vk_arena_alloc(&s->arena, sz, 16, &sbuf, &so);
            memcpy(dst, src, sz); free(tmp); bytes += sz;
            if (nrg && rg[0].bufferOffset == 0 && 0) {}
            VkBufferImageCopy c = {so, 0, 0, {x->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT ? x->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT : x->aspect, lv, x->type == VK_IMAGE_TYPE_3D ? 0 : ly, 1}, {0, 0, 0}, {w, h, dz}};
            vkCmdCopyBufferToImage(s->pre, sbuf, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            nrg++;
        }
        free(lin);
    }
    free(rg);
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
    vkCmdPipelineBarrier2(s->pre, &di);
    atomic_fetch_add(&g_stat_uploads, 1); atomic_fetch_add(&g_stat_upload_bytes, bytes);
    return 1;
}

static vk_tex *tex_create(const tex_t *t) {
    const nvn_texdesc *d = &t->d;
    nvn_vkfmt m;
    if (!nvn_fmt_vk(d->format, &m) || !m.vk) { VKONCE("texture format %#x has no Vulkan counterpart", d->format); return NULL; }
    vk_tex *x = calloc(1, sizeof *x);
    x->serial = t->serial; x->t = *t; memcpy(x->cswz, m.swz, 4);
    x->fmt = (VkFormat)m.vk;
    if ((m.conv == NVN_VKCONV_BC && !V.f_bc) || (m.conv == NVN_VKCONV_ASTC && !V.f_astc)) { x->fmt = (VkFormat)m.vk_decoded; x->decoded = 1; }
    if (d->target == 10) {   // buffer texture: a texel buffer view on the pool buffer (created at bind time)
        x->type = VK_IMAGE_TYPE_1D; x->w = d->width; x->levels = x->layers = 1;
        return x;
    }
    x->aspect = aspect_of(x->fmt);
    x->levels = d->levels ? d->levels : 1;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    x->w = d->width ? d->width : 1; x->h = d->height ? d->height : 1; x->d = 1; x->layers = 1;
    VkImageCreateFlags fl = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    switch (d->target) {
        case 0: x->type = VK_IMAGE_TYPE_1D; x->h = 1; break;
        case 3: x->type = VK_IMAGE_TYPE_1D; x->h = 1; x->layers = d->height ? d->height : 1; break;
        case 2: x->type = VK_IMAGE_TYPE_3D; x->d = d->depth ? d->depth : 1; break;
        case 4: case 6: x->type = VK_IMAGE_TYPE_2D; x->layers = d->depth ? d->depth : 1; break;
        case 8: x->type = VK_IMAGE_TYPE_2D; x->layers = 6; fl |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
        case 9: x->type = VK_IMAGE_TYPE_2D; x->layers = d->depth ? d->depth : 6; fl |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
        default: x->type = VK_IMAGE_TYPE_2D; break;
    }
    if (x->type == VK_IMAGE_TYPE_2D && x->w == x->h && x->layers >= 6 && !(fl & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && d->samples <= 1) fl |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    x->samples = d->samples > 1 ? (VkSampleCountFlagBits)d->samples : VK_SAMPLE_COUNT_1_BIT;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(V.phys, x->fmt, &fp);
    VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    if (!(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !(ff & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        VKLOG("texture format %#x (VkFormat %d) not supported by the device", d->format, x->fmt); free(x); return NULL; }
    VkImageUsageFlags us = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) us |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!fmt_compressed(x->fmt)) {
        if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) us |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) us |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ((d->flags & NVN_TEXTURE_FLAGS_IMAGE) && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && x->samples == VK_SAMPLE_COUNT_1_BIT) us |= VK_IMAGE_USAGE_STORAGE_BIT;
    } else fl |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    if (x->type == VK_IMAGE_TYPE_3D) fl &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    /* A 3D render target (the colour-grading LUT behind the menus) is drawn slice by slice through 2D-array views */
    if (x->type == VK_IMAGE_TYPE_3D && (us & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) fl |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    uint32_t maxdim = x->w > x->h ? x->w : x->h; if (x->d > maxdim) maxdim = x->d;
    uint32_t maxlv = 1; while ((maxdim >> maxlv) > 0) maxlv++;
    if (x->levels > maxlv) x->levels = maxlv;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, fl, x->type, x->fmt, {x->w, x->h, x->d}, x->levels, x->layers, x->samples,
        VK_IMAGE_TILING_OPTIMAL, us, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {0}; ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkResult r = vmaCreateImage(V.vma, &ci, &ai, &x->img, &x->al, NULL);
    if (r != VK_SUCCESS) { VKLOG("vkCreateImage %ux%ux%u fmt %d levels %u layers %u failed: %d", x->w, x->h, x->d, x->fmt, x->levels, x->layers, r); free(x); return NULL; }
    vk_sub *s = vk_sub_current();
    image_barrier_general(s->pre, x->img, x->aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    x->mirrored = tex_mirrored(t);
    g_live++;
    return x;
}

static uint64_t tex_hash(const tex_t *t) { return vk_hash(t->mem, (size_t)t->L.size, 0); }
vk_tex *vk_tex_get(const tex_t *t, int for_render) {
    if (!t || !t->serial || !V.ok) return NULL;
    pthread_mutex_lock(&g_tmu);
    vk_tex *x = tab_get(t->serial);
    if (!x) {
        x = tex_create(t);
        if (!x) { pthread_mutex_unlock(&g_tmu); return NULL; }
        x->hnext = g_tab[t->serial % TB]; g_tab[t->serial % TB] = x;
    }
    pthread_mutex_unlock(&g_tmu);
    if (for_render) { x->gpu_written = 1; return x; }
    if (x->mirrored && !x->gpu_written && x->img) {
        vk_sub *s = vk_sub_current();
        if (x->up_sub != s->seq || !x->uploaded) {   // once per submission (contents as of the submit call)
            vk_pool *p = vk_pool_find((uintptr_t)t->mem, t->L.size);
            int changed;
            if (p && p->gen) { uint32_t g = vk_range_gen((uintptr_t)t->mem, t->L.size); changed = !x->uploaded || g != x->up_gen; x->up_gen = g; }
            else { uint64_t h = tex_hash(&x->t); changed = !x->uploaded || h != x->up_hash; x->up_hash = h; }
            if (changed && tex_upload(x, s)) x->uploaded = 1;
            x->up_sub = s->seq;
        }
    }
    return x;
}
void vk_tex_mark_written(vk_tex *x) { if (x) x->gpu_written = 1; }

vk_tex *vk_tex_by_id(int tid, const view_t **v, int *has_v) {
    static view_t vcopy[64]; static int vi;
    tex_t t; int ok = 0;
    pthread_mutex_lock(&g_rmu);
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) { t = g_treg[tid].t; ok = 1; *has_v = g_treg[tid].has_v; if (*has_v) { vi = (vi + 1) & 63; vcopy[vi] = g_treg[tid].v; *v = &vcopy[vi]; } }
    pthread_mutex_unlock(&g_rmu);
    if (!ok) return NULL;
    return vk_tex_get(&t, 0);
}

static VkComponentSwizzle cs_of(int nvn) {
    switch (nvn) { case NVN_SWZ_ZERO: return VK_COMPONENT_SWIZZLE_ZERO; case NVN_SWZ_ONE: return VK_COMPONENT_SWIZZLE_ONE; case NVN_SWZ_R: return VK_COMPONENT_SWIZZLE_R;
                   case NVN_SWZ_G: return VK_COMPONENT_SWIZZLE_G; case NVN_SWZ_B: return VK_COMPONENT_SWIZZLE_B; default: return VK_COMPONENT_SWIZZLE_A; }
}
/* NVN swizzle selectors (texture's own, then the view's) composed with the format substitution's mapping */
static void compose_swz(const vk_tex *x, const view_t *v, int has_v, uint8_t out[4]) {
    uint8_t ts[4] = {x->t.d.swizzle_r, x->t.d.swizzle_g, x->t.d.swizzle_b, x->t.d.swizzle_a};
    if (!ts[0] && !ts[1] && !ts[2] && !ts[3]) { ts[0] = 2; ts[1] = 3; ts[2] = 4; ts[3] = 5; }
    if (has_v && v->set_swizzle) memcpy(ts, v->swz, 4);
    for (int i = 0; i < 4; i++) {
        int s = ts[i];
        out[i] = s >= NVN_SWZ_R && s <= NVN_SWZ_A ? x->cswz[s - NVN_SWZ_R] : (uint8_t)s;
    }
}
static VkImageViewType default_view_type(const vk_tex *x, int target) {
    switch (target) {
        case 0: return VK_IMAGE_VIEW_TYPE_1D; case 3: return VK_IMAGE_VIEW_TYPE_1D_ARRAY; case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 4: case 6: return VK_IMAGE_VIEW_TYPE_2D_ARRAY; case 8: return VK_IMAGE_VIEW_TYPE_CUBE; case 9: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        default: return x->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}
VkImageView vk_tex_view_typed(vk_tex *x, const view_t *v, int has_v, VkImageViewType want, int usage) {
    if (!x || !x->img) return VK_NULL_HANDLE;
    uint32_t base = has_v && v->set_levels ? v->base_level : 0, nl = has_v && v->set_levels && v->nlevels ? v->nlevels : x->levels - base;
    uint32_t l0 = has_v && v->set_layers ? v->min_layer : 0, nly = has_v && v->set_layers && v->nlayers ? v->nlayers : x->layers - l0;
    if (base >= x->levels) base = x->levels - 1;
    if (base + nl > x->levels) nl = x->levels - base;
    /* A 3D image attached as a render target: its slices are the layers of the 2D-array view (x->layers is 1) */
    uint32_t nslice = x->layers;
    if (x->type == VK_IMAGE_TYPE_3D && usage == 2) {
        nslice = x->d >> base ? x->d >> base : 1;
        if (!(has_v && v->set_layers && v->nlayers)) nly = nslice - (l0 < nslice ? l0 : nslice - 1);
    }
    if (l0 >= nslice) l0 = nslice - 1;
    if (l0 + nly > nslice) nly = nslice - l0;
    VkFormat f = x->fmt;
    if (has_v && v->set_format && !x->decoded) { VkFormat vf = vk_format_of(v->format); if (vf) f = vf; }
    int target = has_v && v->set_target ? v->target : x->t.d.target;
    VkImageViewType vt = want != VK_IMAGE_VIEW_TYPE_MAX_ENUM ? want : default_view_type(x, target);
    if (usage == 2) { vt = x->type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : (nly > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D); nl = 1; }
    if (x->type == VK_IMAGE_TYPE_3D && usage != 2) vt = VK_IMAGE_VIEW_TYPE_3D;
    if ((vt == VK_IMAGE_VIEW_TYPE_CUBE && nly < 6) || (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && nly % 6)) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
    if (vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_1D) nly = 1;
    if (x->type == VK_IMAGE_TYPE_1D && (vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY)) vt = nly > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    if (x->type == VK_IMAGE_TYPE_2D && (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY)) vt = nly > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    uint8_t sw[4] = {NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B, NVN_SWZ_A};
    if (usage == 0) compose_swz(x, v, has_v, sw);
    VkImageAspectFlags asp = aspect_of(f);
    if (usage != 2 && (asp & VK_IMAGE_ASPECT_DEPTH_BIT)) asp = VK_IMAGE_ASPECT_DEPTH_BIT;   // sampling a depth / stencil texture reads depth
    uint64_t key = (uint64_t)f | (uint64_t)base << 32 | (uint64_t)nl << 37 | (uint64_t)vt << 43 | (uint64_t)usage << 47;
    key = vk_hash(&l0, 4, key); key = vk_hash(&nly, 4, key); key = vk_hash(sw, 4, key);
    for (vk_view *w = x->views; w; w = w->next) if (w->key == key) return w->v;
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
        usage == 1 ? VK_IMAGE_USAGE_STORAGE_BIT : usage == 2 ? (asp & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) : VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &uci, 0, x->img, vt, f,
        {cs_of(sw[0]), cs_of(sw[1]), cs_of(sw[2]), cs_of(sw[3])}, {asp, base, nl, l0, nly}};
    if (usage != 0) ci.components = (VkComponentMapping){0, 0, 0, 0};
    VkImageView iv;
    if (vkCreateImageView(V.dev, &ci, NULL, &iv) != VK_SUCCESS) { VKONCE("vkCreateImageView failed (fmt %d type %d)", f, vt); return VK_NULL_HANDLE; }
    vk_view *w = malloc(sizeof *w); w->key = key; w->v = iv; w->next = x->views; x->views = w;
    return iv;
}
VkImageView vk_tex_view(vk_tex *x, const view_t *v, int has_v, int usage) { return vk_tex_view_typed(x, v, has_v, VK_IMAGE_VIEW_TYPE_MAX_ENUM, usage); }

VkBufferView vk_tex_bview(vk_tex *x) {
    if (!x) return VK_NULL_HANDLE;
    uint64_t addr = (uintptr_t)x->t.mem;
    if (x->bview && x->bview_addr == addr) return x->bview;
    int bw, bh, bpb = nvn_fmt_bpb(x->t.d.format, &bw, &bh);
    VkBuffer b; VkDeviceSize off;
    if (!bpb || !vk_buf_at(addr, (uint64_t)x->t.d.width * bpb, &b, &off)) { VKONCE("buffer texture storage not in a memory pool"); return VK_NULL_HANDLE; }
    if (off % V.props.limits.minTexelBufferOffsetAlignment) { VKONCE("buffer texture offset %#llx misaligned", (unsigned long long)off); return VK_NULL_HANDLE; }
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, b, x->fmt, off, (VkDeviceSize)x->t.d.width * bpb};
    if (vkCreateBufferView(V.dev, &ci, NULL, &x->bview) != VK_SUCCESS) return VK_NULL_HANDLE;
    x->bview_addr = addr;
    return x->bview;
}

static void tex_destroy(void *arg) {
    vk_tex *x = arg;
    for (vk_view *w = x->views; w;) { vk_view *n = w->next; vkDestroyImageView(V.dev, w->v, NULL); free(w); w = n; }
    if (x->bview) vkDestroyBufferView(V.dev, x->bview, NULL);
    if (x->img) vmaDestroyImage(V.vma, x->img, x->al);
    free(x);
}
void mtl_tex_release(tex_t *t) {
    if (!V.ok || !t || !t->serial) return;
    pthread_mutex_lock(&g_tmu);
    vk_tex **pp = &g_tab[t->serial % TB], *x = NULL;
    for (; *pp; pp = &(*pp)->hnext) if ((*pp)->serial == t->serial) { x = *pp; *pp = x->hnext; break; }
    pthread_mutex_unlock(&g_tmu);
    if (x) { if (x->img) g_live--; vk_defer_free(tex_destroy, x); }
}

// ---------- samplers ----------
typedef struct samp_ent { uint64_t key; VkSampler s; struct samp_ent *next; } samp_ent;
static samp_ent *g_samps[256]; static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static VkSamplerAddressMode wrapm(int w) {
    switch (w) { case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                 case NVN_WRAP_MODE_CLAMP_TO_BORDER: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                 case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
                 default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; }
}
static VkSampler samp_make(const samp_par *p) {
    uint64_t key = vk_hash(p, sizeof *p, 0);
    pthread_mutex_lock(&g_smu);
    for (samp_ent *e = g_samps[key & 255]; e; e = e->next) if (e->key == key) { pthread_mutex_unlock(&g_smu); return e->s; }
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = p->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (p->minf == NVN_MIN_FILTER_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = (p->minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrapm(p->ws); ci.addressModeV = wrapm(p->wt); ci.addressModeW = wrapm(p->wr);
    ci.mipLodBias = p->lod_bias; ci.minLod = p->lod_min;
    ci.maxLod = (p->minf == NVN_MIN_FILTER_NEAREST || p->minf == NVN_MIN_FILTER_LINEAR) ? 0.25f : (p->lod_max > p->lod_min ? p->lod_max : p->lod_min);
    if (p->aniso > 1 && V.f_aniso) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = p->aniso > V.props.limits.maxSamplerAnisotropy ? V.props.limits.maxSamplerAnisotropy : p->aniso; }
    if (p->cmp_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE) { ci.compareEnable = VK_TRUE; ci.compareOp = (VkCompareOp)(p->cmp_func >= 1 && p->cmp_func <= 8 ? p->cmp_func - 1 : 7); }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    int border = ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    if (border) {
        const float *c = p->border;
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (V.f_border_color) { ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT; memcpy(cb.customBorderColor.float32, c, 16); ci.pNext = &cb; }
        else ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    }
    if (p->reduction == 1 || p->reduction == 2) VKONCE("sampler min / max reduction not implemented");
    VkSampler s; VKCHECK(vkCreateSampler(V.dev, &ci, NULL, &s));
    samp_ent *e = malloc(sizeof *e); e->key = key; e->s = s; e->next = g_samps[key & 255]; g_samps[key & 255] = e;
    pthread_mutex_unlock(&g_smu);
    return s;
}
VkSampler vk_sampler_get(int sid) {
    samp_par p; int ok = 0;
    pthread_mutex_lock(&g_rmu);
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) { p = g_sreg[sid]; ok = 1; }
    pthread_mutex_unlock(&g_rmu);
    return ok ? samp_make(&p) : vk_sampler_default();
}
/* "min <f> mag <f> wrap <s>/<t>/<r>" of a registered sampler (diagnostics); "unregistered" otherwise. */
const char *vk_sampler_desc(int sid) {
    static char buf[96]; samp_par p; int ok = 0;
    pthread_mutex_lock(&g_rmu);
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) { p = g_sreg[sid]; ok = 1; }
    pthread_mutex_unlock(&g_rmu);
    if (ok) snprintf(buf, sizeof buf, "min %d mag %d wrap %d/%d/%d", p.minf, p.magf, p.ws, p.wt, p.wr); else snprintf(buf, sizeof buf, "unregistered (default linear)");
    return buf;
}
VkSampler vk_sampler_default(void) {
    samp_par p = {NVN_MIN_FILTER_LINEAR, NVN_MAG_FILTER_LINEAR, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, 0, 0, 0, 0, 1000, 0, 1, {0, 0, 0, 0}, 0};
    return samp_make(&p);
}

// ---------- dummies for unbound slots (read 0, as on NVN) ----------
static VkImage g_dimg[16][3][2]; static VkImageView g_dview[16][3][2]; static VmaAllocation g_dal[16][3][2];
VkImageView vk_dummy_view(int dim, int arrayed, int ms, int cls, int storage) {
    // dim: 0 1d, 1 2d, 2 3d, 3 cube
    int k = (dim & 3) | (arrayed ? 4 : 0) | (ms ? 8 : 0); if (cls < 0 || cls > 2) cls = 0; storage = !!storage;
    if (g_dview[k][cls][storage]) return g_dview[k][cls][storage];
    static const VkFormat F[3][2] = {{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT}, {VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT}, {VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SINT}};
    VkImageType it = dim == 0 ? VK_IMAGE_TYPE_1D : dim == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    uint32_t layers = dim == 3 ? 6 : 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, dim == 3 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, it, F[cls][storage], {1, 1, 1}, 1, layers,
        ms ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0) | (ms ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {0}; ai.usage = VMA_MEMORY_USAGE_AUTO;
    VKCHECK(vmaCreateImage(V.vma, &ci, &ai, &g_dimg[k][cls][storage], &g_dal[k][cls][storage], NULL));
    VkImageViewType vt = dim == 0 ? (arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) : dim == 2 ? VK_IMAGE_VIEW_TYPE_3D :
                         dim == 3 ? (arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) : (arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_dimg[k][cls][storage], vt, F[cls][storage], {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VKCHECK(vkCreateImageView(V.dev, &vi, NULL, &g_dview[k][cls][storage]));
    vk_sub *s = vk_sub_current();
    image_barrier_general(s->pre, g_dimg[k][cls][storage], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (!ms) { VkClearColorValue z = {{0}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        vkCmdClearColorImage(s->pre, g_dimg[k][cls][storage], VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
        image_barrier_general(s->pre, g_dimg[k][cls][storage], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL); }
    return g_dview[k][cls][storage];
}
VkBufferView vk_dummy_bview(int cls, int storage) {
    static VkBufferView bv[3][2]; static VkBuffer b; static VmaAllocation al;
    if (cls < 0 || cls > 2) cls = 0; storage = !!storage;
    if (bv[cls][storage]) return bv[cls][storage];
    if (!b) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo ai = {0}; ai.usage = VMA_MEMORY_USAGE_AUTO;
        VKCHECK(vmaCreateBuffer(V.vma, &bi, &ai, &b, &al, NULL));
        vkCmdFillBuffer(vk_sub_current()->pre, b, 0, VK_WHOLE_SIZE, 0);
    }
    static const VkFormat F[3] = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT};
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, b, F[cls], 0, 65536};
    VKCHECK(vkCreateBufferView(V.dev, &ci, NULL, &bv[cls][storage]));
    return bv[cls][storage];
}

// ---------- CPU texel access (ReadTexels / WriteTexels): synchronous ----------
static int texel_bytes(const vk_tex *x, uint32_t *bpb, uint32_t *bw, uint32_t *bh) {
    int w, h, b = nvn_fmt_bpb(x->t.d.format, &w, &h);
    if (x->decoded) { *bpb = 4; *bw = *bh = 1; return 1; }
    *bpb = (uint32_t)b; *bw = (uint32_t)w; *bh = (uint32_t)h; return b > 0;
}
void vk_big_lock(void); void vk_big_unlock(void);
static void texel_xfer_(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *mem, size_t row, size_t img, int write);
static void texel_xfer(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *mem, size_t row, size_t img, int write) {
    if (!V.ok) return;
    vk_big_lock(); texel_xfer_(t, level, layer0, r, mem, row, img, write); vk_big_unlock();
}
static void texel_xfer_(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *mem, size_t row, size_t img, int write) {
    if (g_sub) VKDIE("texel access inside a submission");
    vk_tex *x = vk_tex_get(t, write);
    if (g_sub) mtl_sub_commit();   // creation / upload went into an implicit submission
    if (!x || !x->img) return;
    mtl_finish();
    uint32_t bpb, bw, bh; if (!texel_bytes(x, &bpb, &bw, &bh)) return;
    uint32_t w = (uint32_t)r->width, h = (uint32_t)(r->height ? r->height : 1), d = (uint32_t)(r->depth ? r->depth : 1);
    uint32_t brow = (w + bw - 1) / bw * bpb, brows = (h + bh - 1) / bh;
    size_t tight = (size_t)brow * brows, total = tight * d;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, total ? total : 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {0}; ai.usage = VMA_MEMORY_USAGE_AUTO; ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer sb; VmaAllocation sa; VmaAllocationInfo inf; VKCHECK(vmaCreateBuffer(V.vma, &bi, &ai, &sb, &sa, &inf));
    uint8_t *map = inf.pMappedData;
    int is3d = x->type == VK_IMAGE_TYPE_3D;
    VkBufferImageCopy c = {0, 0, 0, {x->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, level, is3d ? 0 : layer0 + (uint32_t)r->zoffset, is3d ? 1 : d},
                           {r->xoffset, r->yoffset, is3d ? r->zoffset : 0}, {w, h, is3d ? d : 1}};
    if (write) for (uint32_t z = 0; z < d; z++) for (uint32_t y = 0; y < brows; y++) memcpy(map + z * tight + (size_t)y * brow, mem + z * img + y * row, brow);
    VkCommandBuffer cb = vk_oneoff_begin();
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
    vkCmdPipelineBarrier2(cb, &di);
    if (write) vkCmdCopyBufferToImage(cb, sb, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    else vkCmdCopyImageToBuffer(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &c);
    vkCmdPipelineBarrier2(cb, &di);
    vk_oneoff_end(cb);
    if (!write) for (uint32_t z = 0; z < d; z++) for (uint32_t y = 0; y < brows; y++) memcpy(mem + z * img + y * row, map + z * tight + (size_t)y * brow, brow);
    vmaDestroyBuffer(V.vma, sb, sa);
}
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) { texel_xfer(t, level, layer0, r, dst, row, img, 0); }
void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) { texel_xfer(t, level, layer0, r, (uint8_t *)src, row, img, 1); }
