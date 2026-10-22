// Vulkan NVN backend: shader variants and programs. A variant is the translator's SPIR-V (<base>.spv) plus its
// reflection (<base>.json, totk_xlate --service --target spirv, Service.cs HandleSpirv): Vulkan set / binding /
// descriptor type of each resource and its NVN meaning (kind + slot, the nvn_refl.h kinds the Metal backend uses).
// Vertex and compute stages use descriptor set 0, the fragment stage set 1; the support buffer is set 0 binding 0 in
// every stage (one shared binding).
#include "nvn_vk.h"
#include "nvn_vk_json.h"
#include "nvn_refl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nvn_vk_resolve_fn g_resolve;
void nvn_vk_set_resolver(nvn_vk_resolve_fn fn) { g_resolve = fn; }
static vk_shader *g_shaders;
static pthread_mutex_t g_shmu = PTHREAD_MUTEX_INITIALIZER;

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz + 1); if (!b) { fclose(f); return NULL; }
    *n = fread(b, 1, (size_t)sz, f); b[*n] = 0; fclose(f); return b;
}
static int desc_of(const char *s) {
    if (!strcmp(s, "ubo")) return VKD_UBO; if (!strcmp(s, "ssbo")) return VKD_SSBO; if (!strcmp(s, "combined")) return VKD_COMBINED;
    if (!strcmp(s, "sampled")) return VKD_SAMPLED; if (!strcmp(s, "sampler")) return VKD_SAMPLER; if (!strcmp(s, "storage")) return VKD_STORAGE;
    if (!strcmp(s, "utexel")) return VKD_UTEXEL; if (!strcmp(s, "stexel")) return VKD_STEXEL; return 0;
}
static int kind_of(const char *k, int bank) {
    if (!strcmp(k, "cbuf")) return NVN_RB_CBUF; if (!strcmp(k, "samp")) return NVN_RB_SAMP; if (!strcmp(k, "samp_tex")) return NVN_RB_SAMPT;
    if (!strcmp(k, "tex")) return NVN_RB_TEX; if (!strcmp(k, "img")) return NVN_RB_IMG; if (!strcmp(k, "tbuf")) return NVN_RB_TBUF;
    if (!strcmp(k, "sbuf")) return NVN_RB_SBUFP; if (!strcmp(k, "ubo_ptr")) return NVN_RB_UBOP; if (!strcmp(k, "sbuf_ubo")) return NVN_RB_SBUFU;
    if (!strcmp(k, "support")) return NVN_RB_SUPPORT; if (!strcmp(k, "c1")) return NVN_RB_C1;
    if (!strcmp(k, "drvcb")) return bank == 2 ? NVN_RB_DRV2 : NVN_RB_DRV0;
    return 0;
}
static int dim_of(const char *d) { return !d ? 1 : !strcmp(d, "1d") ? 0 : !strcmp(d, "3d") ? 2 : !strcmp(d, "cube") ? 3 : !strcmp(d, "buffer") ? 4 : 1; }
static int cls_of(const char *c) { return c && !strcmp(c, "uint") ? 1 : c && !strcmp(c, "int") ? 2 : 0; }

static VkBuffer g_zero64k; static VmaAllocation g_zero64k_al;
VkBuffer vk_zero_buffer(void) {
    if (!g_zero64k) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo ai = {0}; ai.usage = VMA_MEMORY_USAGE_AUTO; ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo inf; VKCHECK(vmaCreateBuffer(V.vma, &bi, &ai, &g_zero64k, &g_zero64k_al, &inf)); memset(inf.pMappedData, 0, 65536);
        vmaFlushAllocation(V.vma, g_zero64k_al, 0, VK_WHOLE_SIZE);
    }
    return g_zero64k;
}

vk_shader *vk_shader_load(const char *base, int stage) {
    pthread_mutex_lock(&g_shmu);
    for (vk_shader *s = g_shaders; s; s = s->next) if (!strcmp(s->key, base)) { pthread_mutex_unlock(&g_shmu); return s; }
    pthread_mutex_unlock(&g_shmu);
    char path[1024]; size_t ns = 0, nj = 0;
    snprintf(path, sizeof path, "%s.spv", base); char *spv = slurp(path, &ns);
    snprintf(path, sizeof path, "%s.json", base); char *jt = slurp(path, &nj);
    js *j = jt ? js_parse(jt, nj) : NULL;
    if (!spv || !j || ns % 4) { VKLOG("shader %s: missing or unreadable .spv / .json (spv %d, json %d, parsed %d)", base, spv != NULL, jt != NULL, j != NULL); free(spv); free(jt); js_free(j); return NULL; }
    if (strcmp(js_str(js_get(j, "target"), ""), "spirv")) { VKLOG("shader %s: reflection is not from the SPIR-V chain", base); free(spv); free(jt); js_free(j); return NULL; }
    if (js_at(js_get(j, "drv_unfillable"), 0)) VKLOG("shader %s reads driver constants the replay does not fill", base);
    vk_shader *s = calloc(1, sizeof *s);
    snprintf(s->key, sizeof s->key, "%s", base); s->stage = stage;
    snprintf(s->entry, sizeof s->entry, "%s", js_str(js_get(j, "entry"), "main"));
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, ns, (const uint32_t *)spv};
    if (vkCreateShaderModule(V.dev, &mi, NULL, &s->mod) != VK_SUCCESS) { VKLOG("shader %s: vkCreateShaderModule failed", base); free(s); free(spv); free(jt); js_free(j); return NULL; }
    js *bs = js_get(j, "bindings");
    for (js *b = bs ? bs->child : NULL; b && s->nb < 64; b = b->next) {
        vk_bind *o = &s->b[s->nb];
        o->set = (uint8_t)js_int(js_get(b, "set"), 0); o->binding = (uint16_t)js_int(js_get(b, "binding"), 0);
        o->desc = (uint8_t)desc_of(js_str(js_get(b, "desc"), "")); o->count = (uint16_t)js_int(js_get(b, "count"), 1);
        o->bank = (uint16_t)js_int(js_get(b, "bank"), 0);
        o->kind = (uint8_t)kind_of(js_str(js_get(b, "kind"), "?"), o->bank);
        o->reg = (uint16_t)js_int(js_get(b, "reg"), 0); o->rw = (uint8_t)js_bool(js_get(b, "rw"), 0); o->off = (uint16_t)js_int(js_get(b, "off"), 0);
        o->dim = (uint8_t)dim_of(js_str(js_get(b, "dim"), NULL)); o->arrayed = (uint8_t)js_bool(js_get(b, "arrayed"), 0); o->ms = (uint8_t)js_bool(js_get(b, "ms"), 0);
        o->cls = (uint8_t)cls_of(js_str(js_get(b, "class"), NULL)); o->storage_fmt = (uint8_t)js_int(js_get(b, "format"), 0);
        const char *sk = js_str(js_get(b, "samp_kind"), NULL);
        o->samp_kind = (uint8_t)(sk ? kind_of(sk, 0) : 0); o->samp_reg = (uint16_t)js_int(js_get(b, "samp_reg"), 0);
        if (!o->desc || !o->kind) { VKLOG("shader %s: binding %s not understood (kind %s desc %s)", base, js_str(js_get(b, "name"), "?"), js_str(js_get(b, "kind"), "?"), js_str(js_get(b, "desc"), "?")); }
        if (!o->count) o->count = 1;
        s->nb++;
    }
    for (js *v = js_get(j, "vertex_inputs") ? js_get(j, "vertex_inputs")->child : NULL; v && s->nv < 32; v = v->next) {
        s->v[s->nv].loc = (uint8_t)js_int(js_get(v, "location"), 0); s->v[s->nv].cls = (uint8_t)cls_of(js_str(js_get(v, "type"), "float")); s->nv++;
    }
    for (js *f = js_get(j, "frag_outputs") ? js_get(j, "frag_outputs")->child : NULL; f; f = f->next) s->fout_mask |= 1u << (js_int(f, 0) & 31);
    s->writes_depth = js_bool(js_get(j, "writes_depth"), 0);
    js *dr = js_get(j, "drv_reads"); s->drv_read[0] = js_get(dr, "0") != NULL; s->drv_read[2] = js_get(dr, "2") != NULL;
    js *tg = js_get(j, "threadgroup_size"); for (int i = 0; i < 3; i++) s->tg[i] = (uint16_t)js_int(js_at(tg, i), 1);
    const char *c1 = js_str(js_get(j, "c1"), "");
    size_t cl = strlen(c1) / 2; if (cl > sizeof s->c1) cl = sizeof s->c1;
    for (size_t i = 0; i < cl; i++) { unsigned v; sscanf(c1 + 2 * i, "%2x", &v); s->c1[i] = (uint8_t)v; }
    s->c1_len = (uint32_t)cl;
    if (cl) {
        VkDeviceSize sz = (cl + 255) & ~(VkDeviceSize)255;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sz, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo ai = {0}; ai.usage = VMA_MEMORY_USAGE_AUTO; ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo inf; VKCHECK(vmaCreateBuffer(V.vma, &bi, &ai, &s->c1buf, &s->c1al, &inf));
        memset(inf.pMappedData, 0, sz); memcpy(inf.pMappedData, s->c1, cl); vmaFlushAllocation(V.vma, s->c1al, 0, VK_WHOLE_SIZE);
    }
    free(spv); free(jt); js_free(j);
    pthread_mutex_lock(&g_shmu); s->next = g_shaders; g_shaders = s; pthread_mutex_unlock(&g_shmu);
    return s;
}

static VkDescriptorType vkdt(int d) {
    switch (d) { case VKD_UBO: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; case VKD_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case VKD_COMBINED: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; case VKD_SAMPLED: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case VKD_SAMPLER: return VK_DESCRIPTOR_TYPE_SAMPLER; case VKD_STORAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case VKD_UTEXEL: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; default: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER; }
}
VkDescriptorType vk_desc_type(int d) { return vkdt(d); }
static VkShaderStageFlags stage_bit(int st) { return st == 0 ? VK_SHADER_STAGE_VERTEX_BIT : st == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT; }

static void prog_layout(vk_prog *p) {
    VkDescriptorSetLayoutBinding lb[2][128]; int nlb[2] = {0, 0};
    for (int st = 0; st < 6; st++) {
        vk_shader *s = p->sh[st]; if (!s) continue;
        for (int i = 0; i < s->nb; i++) {
            const vk_bind *b = &s->b[i]; int set = b->set > 1 ? 1 : b->set; int k;
            for (k = 0; k < nlb[set]; k++) if (lb[set][k].binding == b->binding) break;
            if (k < nlb[set]) {
                if (lb[set][k].descriptorType != vkdt(b->desc)) VKLOG("program: set %d binding %d declared with two descriptor types", set, b->binding);
                lb[set][k].stageFlags |= stage_bit(st); continue;
            }
            if (nlb[set] == 128) continue;
            lb[set][nlb[set]++] = (VkDescriptorSetLayoutBinding){b->binding, vkdt(b->desc), b->count, stage_bit(st), NULL};
        }
    }
    p->nsets = 2;
    for (int set = 0; set < 2; set++) {
        VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)nlb[set], lb[set]};
        VKCHECK(vkCreateDescriptorSetLayout(V.dev, &ci, NULL, &p->dsl[set]));
    }
    VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, p->dsl, 0, NULL};
    VKCHECK(vkCreatePipelineLayout(V.dev, &pci, NULL, &p->layout));
}

int vk_rt_variant(int stage, const uint8_t *control, const uint8_t *code, const char *answers, char *path_base, size_t n);
int vk_exec_answers(int stage, char *buf, size_t n);   // nvn_vk_exec.c: translator answers from the bound state
uint64_t vk_exec_answer_key(int stage, uint64_t h);   // nvn_vk_exec.c: the same answers, hashed
/* Programs by (serial, answer key): an open hash table (the replay asks at every draw). */
#define PROG_HASH 16384
static vk_prog *g_prog_tab[PROG_HASH];
static unsigned prog_slot(uint64_t serial, uint64_t akey) { uint64_t h = (serial * 0x9E3779B97F4A7C15ull) ^ akey; return (unsigned)(h ^ (h >> 29)) & (PROG_HASH - 1); }
vk_prog *vk_prog_get(const prog_t *cp) {
    if (!cp) return NULL;
    char ans[6][2048]; uint64_t akey = 0; int stages = 0;
    if (!g_resolve) for (unsigned i = 0; i < cp->nstage && i < 6; i++) {
        const uint8_t *control = cp->sd[i].control; if (!control) continue;
        uint32_t st; memcpy(&st, control + 0x714, 4); if (st >= 6) continue;
        akey = vk_exec_answer_key((int)st, akey + st + 1); stages |= 1 << st;
    }
    unsigned slot = prog_slot(cp->serial, akey);
    pthread_mutex_lock(&g_shmu);
    if (cp->serial) for (vk_prog *p = g_prog_tab[slot]; p; p = p->next) if (p->serial == cp->serial && p->akey == akey) { pthread_mutex_unlock(&g_shmu); return p->failed ? NULL : p; }
    pthread_mutex_unlock(&g_shmu);
    for (int st = 0; st < 6; st++) if (stages & (1 << st)) vk_exec_answers(st, ans[st], sizeof ans[st]);
    vk_prog *p = calloc(1, sizeof *p); p->serial = cp->serial; p->akey = akey;
    for (unsigned i = 0; i < cp->nstage && i < 6; i++) {
        const uint8_t *control = cp->sd[i].control; const uint8_t *code = (const uint8_t *)(uintptr_t)cp->sd[i].data;
        if (!control || !code) continue;
        uint32_t st; memcpy(&st, control + 0x714, 4);
        if (st != 0 && st != 1 && st != 5) { VKONCE("shader stage %u not supported", st); continue; }
        char base[1024];
        int ok = g_resolve ? g_resolve((int)st, control, code, base, sizeof base) : vk_rt_variant((int)st, control, code, ans[st], base, sizeof base);
        if (ok == 2) { free(p); return NULL; }   // being translated in the background: skip the draw, ask again next time
        if (ok) p->sh[st] = vk_shader_load(base, (int)st);
        if (!ok || !p->sh[st]) { p->failed = 1; break; }
    }
    if (!p->failed) prog_layout(p);
    pthread_mutex_lock(&g_shmu); p->next = g_prog_tab[slot]; g_prog_tab[slot] = p; pthread_mutex_unlock(&g_shmu);
    return p->failed ? NULL : p;
}
void mtl_prog_release(prog_t *p) { (void)p; /* programs are keyed by serial; variants are shared and kept */ }
