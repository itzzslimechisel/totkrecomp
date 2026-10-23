// Vulkan NVN backend: instance, device, submissions, fences, completion, present, and the mtl_* entry points that are
// not replay (see nvn_vk.h for the file map). Ordering rule kept from nvn_mtl.h: every CPU-visible effect of GPU work
// (event values, counter records, read-back pool bytes) happens only after the GPU completed that submission; CPU waits
// look only at the retired fence g_done, which the completion thread advances after applying those effects.
#include "nvn_vk.h"
#include "overlay.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

vk_dev V;
_Atomic uint64_t g_done;
_Atomic uint64_t g_stat_draws, g_stat_passes, g_stat_dispatches, g_stat_uploads, g_stat_upload_bytes, g_stat_readback_bytes;
_Atomic uint64_t g_stat_skip_shader, g_stat_skip_pipe;
_Thread_local vk_sub *g_sub;
static pthread_mutex_t g_big;   // recursive: GPU recording (replay thread for a whole submission; present / texel access otherwise)
static int g_big_init;
void vk_big_lock(void) { pthread_mutex_lock(&g_big); }
void vk_big_unlock(void) { pthread_mutex_unlock(&g_big); }
static uint64_t g_fence;                       // last committed fence (submit thread)
static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;
static vk_sub *g_pend_head, *g_pend_tail, *g_free_subs;   // under g_done_mu
static pthread_t g_comp_thread; static int g_comp_started;
static pthread_cond_t g_pend_cv = PTHREAD_COND_INITIALIZER;
static VkCommandPool g_oneoff_pool; static pthread_mutex_t g_oneoff_mu = PTHREAD_MUTEX_INITIALIZER;
static const char *const *g_win_ext; static uint32_t g_win_next;
static uint64_t g_present_n;

void nvn_vk_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[nvn-vk] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); fflush(stderr);
    va_end(ap);
}
_Noreturn void nvn_vk_die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[nvn-vk] FATAL: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); fflush(stderr);
    va_end(ap); abort();
}
uint64_t vk_now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}
/* Counter timestamps in the Tegra 19.2 MHz tick base of nn::os::GetSystemTick (see nvn_mtl.m gpu_ticks). */
uint64_t vk_gpu_ticks(void) { return vk_now_ns() * 12 / 625; }
uint64_t vk_hash(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p; if (!h) h = 1469598103934665603ull;
    size_t i = 0; for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 1099511628211ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

void nvn_vk_set_window(void *(*surface_fn)(void *), const char *const *inst_ext, uint32_t n) { V.surface_fn = surface_fn; g_win_ext = inst_ext; g_win_next = n; }

static VKAPI_ATTR VkBool32 VKAPI_CALL dbg_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)type; (void)u;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        static _Atomic int n; int k = atomic_fetch_add(&n, 1);
        if (k < 200) VKLOG("validation: %s", d->pMessage);
        if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { static _Atomic int e; atomic_fetch_add(&e, 1); extern _Atomic int g_vk_validation_errors; atomic_fetch_add(&g_vk_validation_errors, 1); }
    }
    return VK_FALSE;
}
_Atomic int g_vk_validation_errors;

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }

int nvn_vk_device_init(void) {
    if (V.inited) return V.ok;
    V.inited = 1;
    if (volkInitialize() != VK_SUCCESS) { VKLOG("no Vulkan loader (vulkan-1)"); return 0; }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) { VKLOG("Vulkan 1.3 loader required (have %#x)", volkGetInstanceVersion()); return 0; }
    const char *ve = getenv("TOTK_VK_VALIDATION"); V.validation = ve && atoi(ve);
    const char *iext[16]; uint32_t ni = 0; const char *layers[1]; uint32_t nl = 0;
    uint32_t nie = 0; vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    VkExtensionProperties *ie = calloc(nie ? nie : 1, sizeof *ie); vkEnumerateInstanceExtensionProperties(NULL, &nie, ie);
    for (uint32_t i = 0; i < g_win_next && ni < 12; i++) iext[ni++] = g_win_ext[i];
    if (V.validation && has_ext(ie, nie, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) iext[ni++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (V.validation) {
        uint32_t nlp = 0; vkEnumerateInstanceLayerProperties(&nlp, NULL);
        VkLayerProperties *lp = calloc(nlp ? nlp : 1, sizeof *lp); vkEnumerateInstanceLayerProperties(&nlp, lp);
        for (uint32_t i = 0; i < nlp; i++) if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) layers[nl++] = "VK_LAYER_KHRONOS_validation";
        if (!nl) VKLOG("TOTK_VK_VALIDATION: VK_LAYER_KHRONOS_validation not found (set VK_ADD_LAYER_PATH)");
        free(lp);
    }
    free(ie);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "TOTK", 1, "nvn-vk", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, nl, layers, ni, iext};
    if (vkCreateInstance(&ici, NULL, &V.inst) != VK_SUCCESS) { VKLOG("vkCreateInstance failed"); return 0; }
    volkLoadInstance(V.inst);
    if (V.validation && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT dci = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, dbg_cb, NULL};
        vkCreateDebugUtilsMessengerEXT(V.inst, &dci, NULL, &V.dbg);
    }
    // physical device: TOTK_VK_DEVICE=<index> or the first discrete GPU, then integrated, with Vulkan 1.3
    uint32_t np = 0; vkEnumeratePhysicalDevices(V.inst, &np, NULL);
    VkPhysicalDevice *pd = calloc(np ? np : 1, sizeof *pd); vkEnumeratePhysicalDevices(V.inst, &np, pd);
    int best = -1, best_score = -1; const char *de = getenv("TOTK_VK_DEVICE");
    for (uint32_t i = 0; i < np; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd[i], &p);
        VKLOG("device %u: %s (api %u.%u.%u, type %d)", i, p.deviceName, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), p.deviceType);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int s = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (de && atoi(de) == (int)i) s = 100;
        if (s > best_score) { best_score = s; best = (int)i; }
    }
    if (best < 0) { VKLOG("no Vulkan 1.3 device"); free(pd); return 0; }
    V.phys = pd[best]; free(pd);
    vkGetPhysicalDeviceProperties(V.phys, &V.props);
    vkGetPhysicalDeviceMemoryProperties(V.phys, &V.memprops);
    V.discrete = V.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(V.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(V.phys, &nq, qp);
    V.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if ((qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { V.qfam = i; break; }
    free(qp);
    if (V.qfam == UINT32_MAX) { VKLOG("no graphics+compute queue"); return 0; }

    uint32_t nde = 0; vkEnumerateDeviceExtensionProperties(V.phys, NULL, &nde, NULL);
    VkExtensionProperties *dex = calloc(nde ? nde : 1, sizeof *dex); vkEnumerateDeviceExtensionProperties(V.phys, NULL, &nde, dex);
    const char *dext[16]; uint32_t nd = 0;
    if (V.surface_fn && has_ext(dex, nde, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) dext[nd++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    int e_interlock = has_ext(dex, nde, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    int e_index8 = has_ext(dex, nde, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
    int e_host = has_ext(dex, nde, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    int e_border = has_ext(dex, nde, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    int e_ballot = has_ext(dex, nde, VK_EXT_SHADER_SUBGROUP_BALLOT_EXTENSION_NAME);
    int e_dclip = has_ext(dex, nde, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    free(dex);

    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT i8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT dc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    void **tail = &f13.pNext;
    if (e_interlock) { *tail = &fi; tail = &fi.pNext; }
    if (e_index8) { *tail = &i8; tail = &i8.pNext; }
    if (e_border) { *tail = &cb; tail = &cb.pNext; }
    if (e_dclip) { *tail = &dc; tail = &dc.pNext; }
    vkGetPhysicalDeviceFeatures2(V.phys, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) { VKLOG("device lacks dynamicRendering / synchronization2 / timelineSemaphore"); return 0; }
    // enable what the replay uses (translator output declares Float64 / DrawParameters / interlock capabilities when used)
    VkPhysicalDeviceFeatures en = {0}; const VkPhysicalDeviceFeatures *have = &f2.features;
#define EN(x) en.x = have->x
    EN(textureCompressionBC); EN(textureCompressionASTC_LDR); EN(shaderFloat64); EN(geometryShader); EN(wideLines); EN(logicOp);
    EN(dualSrcBlend); EN(samplerAnisotropy); EN(shaderStorageImageWriteWithoutFormat); EN(shaderStorageImageReadWithoutFormat);
    EN(fillModeNonSolid); EN(depthClamp); EN(depthBiasClamp); EN(independentBlend); EN(multiViewport); EN(shaderInt64); EN(shaderInt16);
    EN(imageCubeArray); EN(fragmentStoresAndAtomics); EN(vertexPipelineStoresAndAtomics); EN(shaderClipDistance); EN(shaderCullDistance);
    EN(sampleRateShading); EN(shaderImageGatherExtended); EN(largePoints); EN(occlusionQueryPrecise); EN(pipelineStatisticsQuery);
    EN(shaderStorageImageExtendedFormats); EN(alphaToOne); EN(robustBufferAccess);
#undef EN
    VkPhysicalDeviceVulkan11Features e11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    e11.shaderDrawParameters = f11.shaderDrawParameters; e11.storageBuffer16BitAccess = f11.storageBuffer16BitAccess; e11.uniformAndStorageBuffer16BitAccess = f11.uniformAndStorageBuffer16BitAccess;
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.timelineSemaphore = 1; e12.shaderFloat16 = f12.shaderFloat16; e12.shaderInt8 = f12.shaderInt8; e12.storageBuffer8BitAccess = f12.storageBuffer8BitAccess;
    e12.uniformAndStorageBuffer8BitAccess = f12.uniformAndStorageBuffer8BitAccess; e12.shaderOutputLayer = f12.shaderOutputLayer; e12.shaderOutputViewportIndex = f12.shaderOutputViewportIndex;
    e12.samplerFilterMinmax = f12.samplerFilterMinmax; e12.scalarBlockLayout = f12.scalarBlockLayout; e12.uniformBufferStandardLayout = f12.uniformBufferStandardLayout;
    e12.shaderSubgroupExtendedTypes = f12.shaderSubgroupExtendedTypes; e12.vulkanMemoryModel = f12.vulkanMemoryModel; e12.hostQueryReset = f12.hostQueryReset;
    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = 1; e13.synchronization2 = 1; e13.maintenance4 = f13.maintenance4; e13.shaderDemoteToHelperInvocation = f13.shaderDemoteToHelperInvocation;
    e13.subgroupSizeControl = f13.subgroupSizeControl;
    e11.pNext = &e12; e12.pNext = &e13; tail = &e13.pNext;
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT efi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    if (e_interlock && (fi.fragmentShaderPixelInterlock || fi.fragmentShaderSampleInterlock)) {
        efi = fi; efi.pNext = NULL; *tail = &efi; tail = &efi.pNext; dext[nd++] = VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME; V.f_interlock = 1; }
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT ei8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    if (e_index8 && i8.indexTypeUint8) { ei8.indexTypeUint8 = 1; *tail = &ei8; tail = &ei8.pNext; dext[nd++] = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME; V.f_index8 = 1; }
    VkPhysicalDeviceCustomBorderColorFeaturesEXT ecb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    if (e_border && cb.customBorderColors) { ecb.customBorderColors = 1; ecb.customBorderColorWithoutFormat = cb.customBorderColorWithoutFormat; *tail = &ecb; tail = &ecb.pNext; dext[nd++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME; V.f_border_color = cb.customBorderColorWithoutFormat; }
    VkPhysicalDeviceDepthClipEnableFeaturesEXT edc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    if (e_dclip && dc.depthClipEnable) { edc.depthClipEnable = 1; *tail = &edc; tail = &edc.pNext; dext[nd++] = VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME; V.f_depth_clip = 1; }
    if (e_host) { dext[nd++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME; V.f_ext_host = 1; }
    if (e_ballot) { dext[nd++] = VK_EXT_SHADER_SUBGROUP_BALLOT_EXTENSION_NAME; V.f_ballot = 1; }
    V.f_astc = en.textureCompressionASTC_LDR; V.f_bc = en.textureCompressionBC; V.f_float64 = en.shaderFloat64; V.f_geometry = en.geometryShader;
    V.f_wide_lines = en.wideLines; V.f_logic_op = en.logicOp; V.f_dual_src = en.dualSrcBlend; V.f_aniso = en.samplerAnisotropy;
    V.f_storage_wo_fmt = en.shaderStorageImageWriteWithoutFormat; V.f_fill_mode_non_solid = en.fillModeNonSolid; V.f_shader_draw_params = e11.shaderDrawParameters;
    if (getenv("TOTK_VK_NO_ASTC")) V.f_astc = 0;   // tests: force the CPU decode path
    if (getenv("TOTK_VK_NO_BC")) V.f_bc = 0;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, V.qfam, 1, &prio};
    VkPhysicalDeviceFeatures2 ef2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e11, en};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef2, 0, 1, &qci, 0, NULL, nd, dext, NULL};
    VkResult r = vkCreateDevice(V.phys, &dci, NULL, &V.dev);
    if (r != VK_SUCCESS) { VKLOG("vkCreateDevice failed (%d)", r); return 0; }
    volkLoadDevice(V.dev);
    vkGetDeviceQueue(V.dev, V.qfam, 0, &V.q);
    if (V.f_ext_host) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp};
        vkGetPhysicalDeviceProperties2(V.phys, &p2); V.ext_host_align = hp.minImportedHostPointerAlignment;
    }
    // UMA: every device-local heap is host visible (integrated GPUs; Adreno X1)
    V.uma = 1;
    for (uint32_t i = 0; i < V.memprops.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = V.memprops.memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) V.uma = 0;
    }
    VmaVulkanFunctions vf = {0};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr; vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci = {0};
    aci.physicalDevice = V.phys; aci.device = V.dev; aci.instance = V.inst; aci.vulkanApiVersion = VK_API_VERSION_1_3; aci.pVulkanFunctions = &vf;
    VKCHECK(vmaCreateAllocator(&aci, &V.vma));
    VkSemaphoreTypeCreateInfo st = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &st};
    VKCHECK(vkCreateSemaphore(V.dev, &sci, NULL, &V.timeline));
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.qfam};
    VKCHECK(vkCreateCommandPool(V.dev, &pci, NULL, &V.cpool));
    VKCHECK(vkCreateCommandPool(V.dev, &pci, NULL, &g_oneoff_pool));
    pthread_mutex_init(&V.qmu, NULL);
    VKLOG("using %s: %s, BC %d, ASTC %d, fp64 %d, interlock %d, uint8 index %d, host import %d, UMA %d",
          V.props.deviceName, V.discrete ? "discrete" : "integrated", V.f_bc, V.f_astc, V.f_float64, V.f_interlock, V.f_index8, V.f_ext_host, V.uma);
    V.ok = 1;
    return 1;
}

int nvn_vk_available(void) { return V.ok; }
const char *nvn_vk_device_name(void) { return V.ok ? V.props.deviceName : ""; }

const char *nvn_vk_translator_caps(void) {
    static char buf[512];
    snprintf(buf, sizeof buf, "{\"float64\": %s, \"interlock\": %s, \"ballot\": true, \"viewport_mask\": false, \"gs_passthrough\": false, "
             "\"sbuf_align\": %u, \"subgroup_size\": 32, \"image_load_formatted\": %s}",
             V.f_float64 ? "true" : "false", V.f_interlock ? "true" : "false",
             (unsigned)(V.props.limits.minStorageBufferOffsetAlignment > 16 ? V.props.limits.minStorageBufferOffsetAlignment : 16),
             V.f_storage_wo_fmt ? "true" : "false");
    return buf;
}

// ---------- arenas ----------
void *vk_arena_alloc(vk_arena *a, VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    if (align < 16) align = 16;
    vk_block *b = a->head;
    VkDeviceSize at = b ? (b->used + align - 1) & ~(align - 1) : 0;
    if (!b || at + n > b->size) {
        VkDeviceSize sz = n > (4u << 20) ? (n + 65535) & ~(VkDeviceSize)65535 : (4u << 20);
        b = calloc(1, sizeof *b); b->size = sz;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sz,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo ai = {0}; ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo inf; VKCHECK(vmaCreateBuffer(V.vma, &bi, &ai, &b->buf, &b->al, &inf));
        b->map = inf.pMappedData; b->next = a->head; a->head = b; at = 0;
    }
    b->used = at + n; *buf = b->buf; *off = at;
    return b->map + at;
}
void vk_arena_reset(vk_arena *a) {   // keep the newest block, free the rest
    vk_block *b = a->head; if (!b) return;
    vk_block *r = b->next; b->next = NULL; b->used = 0;
    while (r) { vk_block *n = r->next; vmaDestroyBuffer(V.vma, r->buf, r->al); free(r); r = n; }
}
void vk_arena_free(vk_arena *a) { vk_arena_reset(a); if (a->head) { vmaDestroyBuffer(V.vma, a->head->buf, a->head->al); free(a->head); a->head = NULL; } }

// ---------- submissions ----------
static vk_sub *sub_new(void) {
    pthread_mutex_lock(&g_done_mu);
    vk_sub *s = g_free_subs; if (s) g_free_subs = s->next;
    pthread_mutex_unlock(&g_done_mu);
    if (!s) {
        s = calloc(1, sizeof *s);
        VkCommandBuffer cbs[2];
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, V.cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
        VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, cbs)); s->pre = cbs[0]; s->cb = cbs[1];
    }
    static uint64_t seq; s->seq = ++seq;
    s->next = NULL; s->npost = s->nrb = 0; s->dcur = 0; s->defer = NULL;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkResetCommandBuffer(s->pre, 0)); VKCHECK(vkBeginCommandBuffer(s->pre, &bi));
    VKCHECK(vkResetCommandBuffer(s->cb, 0)); VKCHECK(vkBeginCommandBuffer(s->cb, &bi));
    {   // everything submitted before (earlier submissions, presents that blit a rendering) completes its memory accesses first:
        // nothing waits on the CPU for the GPU between submissions any more
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
        vkCmdPipelineBarrier2(s->pre, &di);
    }
    return s;
}
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout l) {
    vk_sub *s = vk_sub_current();
    for (;;) {
        if (s->dcur < s->ndpool) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, s->dpool[s->dcur], 1, &l};
            VkDescriptorSet ds; VkResult r = vkAllocateDescriptorSets(V.dev, &ai, &ds);
            if (r == VK_SUCCESS) return ds;
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) VKDIE("vkAllocateDescriptorSets %d", r);
            s->dcur++; continue;
        }
        if (s->ndpool == 64) VKDIE("descriptor pools exhausted in one submission");
        VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048}, {VK_DESCRIPTOR_TYPE_SAMPLER, 2048}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 512}};
        VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 4096, 8, ps};
        VKCHECK(vkCreateDescriptorPool(V.dev, &pci, NULL, &s->dpool[s->ndpool])); s->ndpool++;
    }
}
void vk_sub_defer(vk_sub *s, void (*fn)(void *), void *arg) { vk_defer *d = malloc(sizeof *d); d->fn = fn; d->arg = arg; d->next = s->defer; s->defer = d; }
typedef struct { void (*fn)(void *); void *arg; uint64_t after; } late_t;
static late_t *g_late; static int g_nlate, g_clate;   // under g_done_mu: run once g_done >= after
void vk_defer_free(void (*fn)(void *), void *arg) {
    if (g_sub) { vk_sub_defer(g_sub, fn, arg); return; }
    pthread_mutex_lock(&g_done_mu);
    if (atomic_load(&g_done) >= g_fence) { pthread_mutex_unlock(&g_done_mu); fn(arg); return; }
    if (g_nlate == g_clate) { g_clate = g_clate ? 2 * g_clate : 64; g_late = realloc(g_late, g_clate * sizeof *g_late); }
    g_late[g_nlate++] = (late_t){fn, arg, g_fence};
    pthread_mutex_unlock(&g_done_mu);
}
static void retire(vk_sub *s) {
    for (int i = 0; i < s->nrb; i++) { vk_guest_write_from_gpu(s->rb[i].guest, s->rb[i].stage, (size_t)s->rb[i].n); atomic_fetch_add(&g_stat_readback_bytes, s->rb[i].n); }
    for (int i = 0; i < s->npost; i++) {
        vk_post *p = &s->post[i];
        if (p->kind == 1) { if (p->mode) __atomic_fetch_add(p->val, p->v, __ATOMIC_SEQ_CST); else __atomic_store_n(p->val, p->v, __ATOMIC_SEQ_CST); }
        else if (p->kind == 2 && p->addr) {   // NVNcounterData {uint64 counter, uint64 timestamp}
            uint64_t rec[2] = {p->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 1 : 0, vk_gpu_ticks()};
            vk_pool *pl = vk_pool_find(p->addr, 16);
            if (p->type == NVN_COUNTER_TYPE_TIMESTAMP || p->type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) rec[0] = 0;
            if (pl) vk_guest_write_from_gpu((uint8_t *)(uintptr_t)p->addr, rec, 16);
        }
    }
    while (s->defer) { vk_defer *d = s->defer; s->defer = d->next; d->fn(d->arg); free(d); }
    for (int i = 0; i < s->ndpool; i++) vkResetDescriptorPool(V.dev, s->dpool[i], 0);
    vk_arena_reset(&s->arena);
}
static void *comp_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_done_mu);
        while (!g_pend_head) pthread_cond_wait(&g_pend_cv, &g_done_mu);
        vk_sub *s = g_pend_head;
        pthread_mutex_unlock(&g_done_mu);
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &V.timeline, &s->fence};
        VkResult r = vkWaitSemaphores(V.dev, &wi, 2000000000ull);
        if (r == VK_TIMEOUT) continue;
        if (r != VK_SUCCESS) VKDIE("vkWaitSemaphores: %d (device lost?)", r);
        retire(s);
        pthread_mutex_lock(&g_done_mu);
        g_pend_head = s->next; if (!g_pend_head) g_pend_tail = NULL;
        atomic_store(&g_done, s->fence);
        s->next = g_free_subs; g_free_subs = s;
        for (int i = 0; i < g_nlate;) if (g_late[i].after <= s->fence) { late_t l = g_late[i]; g_late[i] = g_late[--g_nlate]; pthread_mutex_unlock(&g_done_mu); l.fn(l.arg); pthread_mutex_lock(&g_done_mu); } else i++;
        pthread_cond_broadcast(&g_done_cv);
        pthread_mutex_unlock(&g_done_mu);
    }
    return NULL;
}

void vk_exec_end_all(void);       // nvn_vk_exec.c: closes an open render pass, records pending barriers
void vk_exec_sub_begin(void);
static _Thread_local int g_implicit;
void nvn_vk_frontend_hook(void) __attribute__((weak));   // nvn_vk_game.c: window surface of the game frontend
void mtl_init(void) {
    if (V.inited) return;
    if (!g_big_init) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&g_big, &a); g_big_init = 1; }
    if (nvn_vk_frontend_hook) nvn_vk_frontend_hook();
    if (!nvn_vk_device_init()) { VKLOG("Vulkan backend unavailable: GPU work is skipped"); return; }
    {   // before the first frame: the shader variant index, the pipeline cache; the device's translator caps for prepare_shaders.py
        void nvn_vk_shader_index_load(void); void vk_pcache_init(void); void vk_pcache_save(void); const char *nvn_vk_cache_dir(void);
        nvn_vk_shader_index_load(); vk_pcache_init(); atexit(vk_pcache_save);
        char p[1200]; snprintf(p, sizeof p, "%s/vk-caps.json", nvn_vk_cache_dir());
        FILE *f = fopen(p, "wb"); if (f) { fputs(nvn_vk_translator_caps(), f); fclose(f); }
    }
    if (!g_comp_started) { g_comp_started = 1; pthread_create(&g_comp_thread, NULL, comp_main, NULL); }
}
void mtl_sub_begin(void) {
    mtl_init();
    if (!V.ok) return;
    if (g_sub) { if (g_implicit) { g_implicit = 0; return; } VKDIE("mtl_sub_begin inside a submission"); }
    vk_big_lock();
    g_sub = sub_new();
    vk_pools_sync(g_sub);
    vk_exec_sub_begin();
}
vk_sub *vk_sub_current(void) {
    if (!g_sub) { mtl_sub_begin(); g_implicit = 1; }
    return g_sub;
}
void vk_add_readback(vk_sub *s, uint64_t addr, uint64_t n) {
    VkBuffer b; VkDeviceSize off;
    if (!n || !vk_buf_at(addr, n, &b, &off)) return;
    vk_pool *p = vk_pool_find(addr, n); if (!p || p->mode == 1) return;   // imported host memory: the GPU wrote guest memory itself
    for (int i = 0; i < s->nrb; i++) if (s->rb[i].guest == (uint8_t *)(uintptr_t)addr && s->rb[i].n == n) return;
    if (s->nrb == s->crb) { s->crb = s->crb ? 2 * s->crb : 32; s->rb = realloc(s->rb, s->crb * sizeof *s->rb); }
    s->rb[s->nrb++] = (vk_readback){(uint8_t *)(uintptr_t)addr, b, off, n, NULL};
}
uint64_t mtl_sub_commit(void) {
    if (!V.ok || !g_sub) return g_fence;
    vk_sub *s = g_sub;
    vk_exec_end_all();
    if (s->nrb) {   // GPU-written pool ranges: copy into host-visible staging after everything else in this submission
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
        vkCmdPipelineBarrier2(s->cb, &di);
        for (int i = 0; i < s->nrb; i++) {
            VkBuffer sb; VkDeviceSize so; s->rb[i].stage = vk_arena_alloc(&s->arena, s->rb[i].n, 16, &sb, &so);
            VkBufferCopy c = {s->rb[i].src_off, so, s->rb[i].n}; vkCmdCopyBuffer(s->cb, s->rb[i].src, sb, 1, &c);
        }
        VkMemoryBarrier2 hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
        di.pMemoryBarriers = &hb; vkCmdPipelineBarrier2(s->cb, &di);
    }
    VKCHECK(vkEndCommandBuffer(s->pre)); VKCHECK(vkEndCommandBuffer(s->cb));
    s->fence = ++g_fence;
    VkCommandBufferSubmitInfo cbi[2] = {{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, s->pre}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, s->cb}};
    VkSemaphoreSubmitInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, V.timeline, s->fence, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 2, cbi, 1, &ssi};
    pthread_mutex_lock(&V.qmu);
    VkResult r = vkQueueSubmit2(V.q, 1, &si, VK_NULL_HANDLE);
    pthread_mutex_unlock(&V.qmu);
    if (r != VK_SUCCESS) VKDIE("vkQueueSubmit2: %d", r);
    pthread_mutex_lock(&g_done_mu);
    if (g_pend_tail) g_pend_tail->next = s; else g_pend_head = s;
    g_pend_tail = s;
    pthread_cond_signal(&g_pend_cv);
    pthread_mutex_unlock(&g_done_mu);
    g_sub = NULL; g_implicit = 0;
    vk_big_unlock();
    return s->fence;
}
static void post_push(vk_post p) {
    vk_sub *s = vk_sub_current();
    if (s->npost == s->cpost) { s->cpost = s->cpost ? 2 * s->cpost : 32; s->post = realloc(s->post, s->cpost * sizeof *s->post); }
    s->post[s->npost++] = p;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { if (V.ok) post_push((vk_post){1, val, mode, v, 0, 0}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { if (V.ok) post_push((vk_post){2, NULL, 0, 0, addr, type}); }
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (!V.ok || atomic_load(&g_done) >= fence) return 1;
    if (!timeout_ns) return 0;
    uint64_t end = vk_now_ns() + timeout_ns;
    pthread_mutex_lock(&g_done_mu);
    while (atomic_load(&g_done) < fence) {
        uint64_t now = vk_now_ns(); if (now >= end) break;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t left = end - now; if (left > 50000000ull) left = 50000000ull;
        ts.tv_nsec += (long)(left % 1000000000ull); ts.tv_sec += (time_t)(left / 1000000000ull + ts.tv_nsec / 1000000000L); ts.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&g_done_cv, &g_done_mu, &ts);
    }
    int ok = atomic_load(&g_done) >= fence;
    pthread_mutex_unlock(&g_done_mu);
    return ok;
}
void mtl_finish(void) {
    if (!V.ok) return;
    if (g_sub && g_implicit) mtl_sub_commit();
    while (!mtl_wait(g_fence, 1000000000ull)) VKLOG("waiting for the GPU (fence %llu, done %llu)", (unsigned long long)g_fence, (unsigned long long)atomic_load(&g_done));
}
void mtl_drain(void) { mtl_finish(); }

VkCommandBuffer vk_oneoff_begin(void) {
    pthread_mutex_lock(&g_oneoff_mu);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_oneoff_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}
void vk_oneoff_end(VkCommandBuffer cb) {   // ordered after all earlier submissions (same queue; full barrier at the start)
    VKCHECK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; VKCHECK(vkCreateFence(V.dev, &fci, NULL, &f));
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &cbi, 0, NULL};
    pthread_mutex_lock(&V.qmu); VKCHECK(vkQueueSubmit2(V.q, 1, &si, f)); pthread_mutex_unlock(&V.qmu);
    VKCHECK(vkWaitForFences(V.dev, 1, &f, VK_TRUE, UINT64_MAX));
    vkDestroyFence(V.dev, f, NULL);
    vkFreeCommandBuffers(V.dev, g_oneoff_pool, 1, &cb);
    pthread_mutex_unlock(&g_oneoff_mu);
}

// ---------- present ----------
static uint64_t g_swap_gen;   /* bumped when the swapchain is recreated (the overlay remakes its image views) */
int overlay_vk_draw(const overlay_vk_frame *f) __attribute__((weak));
static int swap_create(void) {
    if (!V.surface_fn) return 0;
    if (!V.surface) { V.surface = (VkSurfaceKHR)(uintptr_t)V.surface_fn(V.inst); if (!V.surface) return 0; }
    VkBool32 sup = 0; vkGetPhysicalDeviceSurfaceSupportKHR(V.phys, V.qfam, V.surface, &sup); if (!sup) { VKONCE("queue cannot present to the window"); return 0; }
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.phys, V.surface, &caps);
    if (!caps.currentExtent.width || !caps.currentExtent.height) return 0;   // minimized
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, V.surface, &nf, NULL);
    VkSurfaceFormatKHR fm[64]; if (nf > 64) nf = 64; vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, V.surface, &nf, fm);
    VkSurfaceFormatKHR pick = fm[0];
    for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) { pick = fm[i]; break; }
    uint32_t n = caps.minImageCount + 1; if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount; if (n > 8) n = 8;
    VkSwapchainKHR old = V.swap;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, V.surface, n, pick.format, pick.colorSpace, caps.currentExtent, 1,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT),   /* SRC: "oshot" */
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, caps.currentTransform,
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, getenv("TOTK_VK_VSYNC_OFF") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, old};
    if (ci.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
        uint32_t npm = 0; VkPresentModeKHR pm[16]; vkGetPhysicalDeviceSurfacePresentModesKHR(V.phys, V.surface, &npm, NULL); if (npm > 16) npm = 16;
        vkGetPhysicalDeviceSurfacePresentModesKHR(V.phys, V.surface, &npm, pm); int ok = 0; for (uint32_t i = 0; i < npm; i++) ok |= pm[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
        if (!ok) ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    }
    if (vkCreateSwapchainKHR(V.dev, &ci, NULL, &V.swap) != VK_SUCCESS) { V.swap = VK_NULL_HANDLE; return 0; }
    if (old) { vkDeviceWaitIdle(V.dev); vkDestroySwapchainKHR(V.dev, old, NULL); }
    V.sfmt = pick.format; V.sext = caps.currentExtent;
    g_swap_gen++;
    V.nsimg = 8; vkGetSwapchainImagesKHR(V.dev, V.swap, &V.nsimg, V.simg);
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < 8; i++) { if (!V.s_acq[i]) vkCreateSemaphore(V.dev, &sci, NULL, &V.s_acq[i]); if (!V.s_done[i]) vkCreateSemaphore(V.dev, &sci, NULL, &V.s_done[i]); }
    VKLOG("swapchain %ux%u format %d, %u images", V.sext.width, V.sext.height, V.sfmt, V.nsimg);
    return 1;
}
int mtl_has_window(void) { return V.surface_fn != NULL; }
/* A texture's level 0 / layer 0 converted to RGBA8 by a blit (any colour format), read back synchronously. */
static uint8_t *read_rgba8_(const tex_t *t, uint32_t *w, uint32_t *h);
uint8_t *nvn_vk_read_rgba8(const tex_t *t, uint32_t *w, uint32_t *h) { if (!V.ok) return NULL; vk_big_lock(); uint8_t *r = read_rgba8_(t, w, h); vk_big_unlock(); return r; }
static uint8_t *read_rgba8_(const tex_t *t, uint32_t *w, uint32_t *h) {
    if (g_sub && g_implicit) mtl_sub_commit();
    vk_tex *x = vk_tex_get(t, 0);
    if (g_sub && g_implicit) mtl_sub_commit();
    if (!x || !x->img || !(x->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) return NULL;
    mtl_finish();
    *w = x->w; *h = x->h;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {x->w, x->h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {0}; ai.usage = VMA_MEMORY_USAGE_AUTO;
    VkImage tmp; VmaAllocation tal; if (vmaCreateImage(V.vma, &ci, &ai, &tmp, &tal, NULL) != VK_SUCCESS) return NULL;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)x->w * x->h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo bai = {0}; bai.usage = VMA_MEMORY_USAGE_AUTO; bai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buf; VmaAllocation bal; VmaAllocationInfo inf; VKCHECK(vmaCreateBuffer(V.vma, &bi, &bai, &buf, &bal, &inf));
    VkCommandBuffer cb = vk_oneoff_begin();
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 0, tmp, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)x->w, (int32_t)x->h, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)x->w, (int32_t)x->h, 1}}};
    vkCmdBlitImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    VkDependencyInfo d2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
    vkCmdPipelineBarrier2(cb, &d2);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {x->w, x->h, 1}};
    vkCmdCopyImageToBuffer(cb, tmp, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    vk_oneoff_end(cb);
    uint8_t *out = malloc((size_t)x->w * x->h * 4); memcpy(out, inf.pMappedData, (size_t)x->w * x->h * 4);
    for (size_t i = 0; i < (size_t)x->w * x->h; i++) out[4 * i + 3] = 255;
    vmaDestroyBuffer(V.vma, buf, bal); vmaDestroyImage(V.vma, tmp, tal);
    return out;
}
int nvn_vk_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) __attribute__((weak));
void nvn_vk_present_hook(void) __attribute__((weak));
/* Screenshot of the n-th next presented picture (TOTK_INPUT_FILE "shot <path>" / "wshot <path>": runtime/input.c). */
static pthread_mutex_t g_shot_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_shot_path[1024]; static long g_shot_in;
void dm_gfx_shot_at(long frames, const char *path) {
    pthread_mutex_lock(&g_shot_mu); snprintf(g_shot_path, sizeof g_shot_path, "%s", path); g_shot_in = frames > 0 ? frames : 1; pthread_mutex_unlock(&g_shot_mu);
}
void dm_gfx_window_shot(const char *path) { dm_gfx_shot_at(1, path); }
static void present_shot(const tex_t *t, uint64_t n) {
    static const char *dir; static long every = -1;
    {
        char p[1024]; int take = 0;
        pthread_mutex_lock(&g_shot_mu); if (g_shot_in > 0 && --g_shot_in == 0) { snprintf(p, sizeof p, "%s", g_shot_path); take = 1; } pthread_mutex_unlock(&g_shot_mu);
        if (take && nvn_vk_write_png) {
            uint32_t w, h; uint8_t *px = nvn_vk_read_rgba8(t, &w, &h);
            if (px && nvn_vk_write_png(p, px, w, h)) VKLOG("screenshot %ux%u -> %s", w, h, p); else VKLOG("screenshot %s failed", p);
            free(px);
        }
    }
    if (every < 0) { dir = getenv("TOTK_VK_SHOT"); const char *e = getenv("TOTK_VK_SHOT_EVERY"); every = e ? atol(e) : 60; if (every < 1) every = 60; }
    if (!dir || !nvn_vk_write_png || n % (uint64_t)every) return;
    uint32_t w, h; uint8_t *px = nvn_vk_read_rgba8(t, &w, &h); if (!px) return;
    char p[1024]; snprintf(p, sizeof p, "%s/present_%05llu.png", dir, (unsigned long long)n);
    if (nvn_vk_write_png(p, px, w, h)) VKLOG("screenshot %ux%u -> %s (draws %llu)", w, h, p, (unsigned long long)atomic_load(&g_stat_draws));
    free(px);
}
static uint64_t present_(const tex_t *t, const int crop[4], uint64_t ticket);
/* TOTK_INPUT_FILE "oshot <path>": the next presented swapchain image (the game picture with the in-game menu / FPS counter
   drawn over it), copied out after the present's submission and written as a PNG. */
static char g_oshot_path[1024]; static _Atomic int g_oshot_pending;
void dm_gfx_overlay_shot(const char *path) {
    pthread_mutex_lock(&g_shot_mu); snprintf(g_oshot_path, sizeof g_oshot_path, "%s", path); pthread_mutex_unlock(&g_shot_mu);
    atomic_store(&g_oshot_pending, 1);
}
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    uint64_t ticket = ++g_present_n;
    if (nvn_vk_present_hook) nvn_vk_present_hook();
    if (!V.ok) return ticket;
    vk_big_lock(); uint64_t r = present_(t, crop, ticket); vk_big_unlock(); return r;
}
static uint64_t present_(const tex_t *t, const int crop[4], uint64_t ticket) {
    present_shot(t, ticket);
    { void vk_rt_dump_flush(void); vk_rt_dump_flush(); }
    { void vk_pcache_tick(uint64_t); vk_pcache_tick(ticket); }
    { void vk_pipe_budget_reset(void); vk_pipe_budget_reset(); }
    if (ticket % 600 == 0) { void vk_pipe_stats(uint64_t *, uint64_t *, int *); uint64_t made, ns; int pend; vk_pipe_stats(&made, &ns, &pend);
        VKLOG("pipelines: %llu created (%.1f ms average, on worker threads), %d being created", (unsigned long long)made, made ? ns / 1e6 / made : 0.0, pend); }
    {   /* draws skipped because a shader variant or pipeline was not ready: per 60 presents, when there were any */
        static uint64_t last_s, last_p, prev_s, prev_p, frames_with;
        uint64_t s = atomic_load(&g_stat_skip_shader), p = atomic_load(&g_stat_skip_pipe);
        if (s != prev_s || p != prev_p) frames_with++;
        prev_s = s; prev_p = p;
        if (ticket % 60 == 0 && frames_with) {
            VKLOG("draws skipped in the last 60 presents (%llu of them): shader not ready %llu, pipeline not ready %llu (total %llu / %llu)", (unsigned long long)frames_with,
                  (unsigned long long)(s - last_s), (unsigned long long)(p - last_p), (unsigned long long)s, (unsigned long long)p);
            frames_with = 0;
        }
        if (ticket % 60 == 0) { last_s = s; last_p = p; }
    }
    if (ticket == 1 || ticket % 600 == 0) VKLOG("present %llu: draws %llu passes %llu dispatches %llu", (unsigned long long)ticket, (unsigned long long)atomic_load(&g_stat_draws),
                                             (unsigned long long)atomic_load(&g_stat_passes), (unsigned long long)atomic_load(&g_stat_dispatches));
    if (!V.ok || !V.surface_fn) return ticket;
    if (g_sub) { if (g_implicit) mtl_sub_commit(); }
    if (!V.swap && !swap_create()) return ticket;
    vk_tex *x = vk_tex_get(t, 0);
    if (g_sub && g_implicit) mtl_sub_commit();   // vk_tex_get may have uploaded through an implicit submission
    if (!x) return ticket;
    /* Presents do not wait for the GPU: a ring of 8 command buffers / fences, slot i waited for only when it comes round
       again (its acquire semaphore is reused then too). Waiting for every present serialized the CPU behind the GPU. */
    static VkCommandPool ppool; static VkCommandBuffer pcb[8]; static VkFence pfence[8];
    if (!ppool) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.qfam};
        VKCHECK(vkCreateCommandPool(V.dev, &pci, NULL, &ppool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, ppool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 8};
        VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, pcb));
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        for (int i = 0; i < 8; i++) VKCHECK(vkCreateFence(V.dev, &fci, NULL, &pfence[i]));
    }
    uint32_t fi = V.s_frame++ % 8, idx = 0;
    vkWaitForFences(V.dev, 1, &pfence[fi], VK_TRUE, UINT64_MAX);
    VkResult r = vkAcquireNextImageKHR(V.dev, V.swap, 1000000000ull, V.s_acq[fi], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { swap_create(); return ticket; }
    if (r < 0) { VKONCE("vkAcquireNextImageKHR: %d", r); return ticket; }
    vkResetFences(V.dev, 1, &pfence[fi]);
    VkCommandBuffer cb = pcb[fi]; int swap_reset = 0;
    VKCHECK(vkResetCommandBuffer(cb, 0));
    { VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; VKCHECK(vkBeginCommandBuffer(cb, &bi)); }
    VkImageMemoryBarrier2 b[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0, 0, x->img, {x->aspect, 0, 1, 0, 1}},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0, V.simg[idx], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 2, b};
    vkCmdPipelineBarrier2(cb, &di);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, V.simg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    int cx = crop ? crop[0] : 0, cy = crop ? crop[1] : 0, cw = crop && crop[2] > 0 ? crop[2] : (int)x->w, ch = crop && crop[3] > 0 ? crop[3] : (int)x->h;
    double s = (double)V.sext.width / cw < (double)V.sext.height / ch ? (double)V.sext.width / cw : (double)V.sext.height / ch;
    int dw = (int)(cw * s), dh = (int)(ch * s), dx = ((int)V.sext.width - dw) / 2, dy = ((int)V.sext.height - dh) / 2;
    VkImageBlit bl = {{x->aspect, 0, 0, 1}, {{cx, cy, 0}, {cx + cw, cy + ch, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo d2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
    vkCmdPipelineBarrier2(cb, &d2);
    vkCmdBlitImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, V.simg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    if (overlay_vk_draw) {   /* the in-game menu / FPS counter (runtime/overlay), drawn last; the image is TRANSFER_DST again after it */
        overlay_vk_frame of = {V.inst, V.phys, V.dev, V.q, &V.qmu, V.qfam, V.nsimg, idx, cb, V.simg[idx], (uint32_t)V.sfmt, V.sext.width, V.sext.height, g_swap_gen};
        if (overlay_vk_draw(&of) & 2) swap_reset = 1;
    }
    VkBuffer obuf = VK_NULL_HANDLE; VmaAllocation oal = NULL; VmaAllocationInfo oinf = {0};
    if (atomic_exchange(&g_oshot_pending, 0)) {   /* "oshot": copy the finished swapchain image out (read after the fence below) */
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)V.sext.width * V.sext.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo bai = {0}; bai.usage = VMA_MEMORY_USAGE_AUTO; bai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(V.vma, &bi, &bai, &obuf, &oal, &oinf) == VK_SUCCESS) {
            VkImageMemoryBarrier2 ob = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0, V.simg[idx], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VkDependencyInfo od = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &ob};
            vkCmdPipelineBarrier2(cb, &od);
            VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {V.sext.width, V.sext.height, 1}};
            vkCmdCopyImageToBuffer(cb, V.simg[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, obuf, 1, &c);
            ob.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; ob.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT; ob.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT; ob.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            ob.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; ob.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            vkCmdPipelineBarrier2(cb, &od);
        } else obuf = VK_NULL_HANDLE;
    }
    VkImageMemoryBarrier2 pb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0, V.simg[idx], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d3 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &pb};
    vkCmdPipelineBarrier2(cb, &d3);
    VKCHECK(vkEndCommandBuffer(cb));
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb};
    VkSemaphoreSubmitInfo wsi = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, V.s_acq[fi], 0, VK_PIPELINE_STAGE_2_BLIT_BIT};
    VkSemaphoreSubmitInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, V.s_done[idx], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 1, &wsi, 1, &cbi, 1, &ssi};
    pthread_mutex_lock(&V.qmu);
    VKCHECK(vkQueueSubmit2(V.q, 1, &si, pfence[fi]));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &V.s_done[idx], 1, &V.swap, &idx, NULL};
    r = vkQueuePresentKHR(V.q, &pi);
    pthread_mutex_unlock(&V.qmu);
    { extern void front_frame_presented(void) __attribute__((weak)); if (front_frame_presented) front_frame_presented(); }   // clears the boot status in the title
    if (obuf) {
        vkWaitForFences(V.dev, 1, &pfence[fi], VK_TRUE, UINT64_MAX);
        uint8_t *px = oinf.pMappedData; size_t n = (size_t)V.sext.width * V.sext.height;
        int bgr = V.sfmt == VK_FORMAT_B8G8R8A8_UNORM || V.sfmt == VK_FORMAT_B8G8R8A8_SRGB;
        for (size_t i = 0; i < n; i++) { if (bgr) { uint8_t t0 = px[4 * i]; px[4 * i] = px[4 * i + 2]; px[4 * i + 2] = t0; } px[4 * i + 3] = 255; }
        char p[1024]; pthread_mutex_lock(&g_shot_mu); snprintf(p, sizeof p, "%s", g_oshot_path); pthread_mutex_unlock(&g_shot_mu);
        if (nvn_vk_write_png && nvn_vk_write_png(p, px, V.sext.width, V.sext.height)) VKLOG("screenshot with overlay %ux%u -> %s", V.sext.width, V.sext.height, p);
        else VKLOG("screenshot with overlay %s failed", p);
        vmaDestroyBuffer(V.vma, obuf, oal);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || swap_reset) swap_create();   /* swap_reset: VSync changed in the menu */
    return ticket;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
void mtl_present_flush(void) {}
void mtl_present_test_stall(uint64_t ns) { (void)ns; }
int mtl_display_hz(void) { return V.surface_fn ? 60 : 0; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
uint64_t mtl_present_count(void) { return g_present_n; }

// ---------- statistics / probes / features the Metal backend has and this one does not (yet) ----------
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = atomic_load(&g_stat_passes); *draws = atomic_load(&g_stat_draws); }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = 0; *reuse = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = 0; *skip = 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
void mtl_bw(uint64_t *l, uint64_t *s, uint64_t *f) { *l = *s = *f = 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(uint64_t timeout_ns, unsigned frame_ticks) { (void)timeout_ns; (void)frame_ticks; return 0; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
double mtl_gpu_ms(uint64_t f) { (void)f; return -1; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; (void)t0; (void)t1; return 0; }
void mtl_pkg_stats(uint64_t *u, uint64_t *r) { *u = atomic_load(&g_stat_uploads); *r = 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
uint64_t mtl_upload_bytes(void) { return atomic_load(&g_stat_upload_bytes); }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *n) { *a = *b = 0; *n = 0; }
void mtl_upload_wait_stats(uint64_t *n, uint64_t *ns) { *n = *ns = 0; }
uint64_t mtl_chunk_commits(void) { return 0; }
void mtl_blit_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_pool_res_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
void mtl_keepalive_stats(uint64_t v[3]) { memset(v, 0, 3 * sizeof *v); }
void mtl_layer_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
void mtl_ren_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_cube_merge_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
uint32_t mtl_present_qos(void) { return 0; }
void mtl_note_world(void) {}
void mtl_app_active(int active) { (void)active; }
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb) { (void)frame; (void)rss_mb; }
void mtl_shot_at_present(const tex_t *t) { (void)t; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void xv_test_counts(int *dc, int *viol) { if (dc) *dc = 0; if (viol) *viol = 0; }
void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy) { (void)on; (void)px; (void)py; (void)grid; (void)cx; (void)cy; }
int mtl_ui_ghost_rect(float r[4]) { (void)r; return 0; }
void bf_want_note(const char *key) { (void)key; }
void mtl_pin_refs(void (*mark)(const void *)) { (void)mark; }
void mtl_dead_forget(const uint64_t *serials, size_t n) { (void)serials; (void)n; }
void mtl_probe_pin_rt(const tex_t *t) { (void)t; }
void mtl_res_snapshot(mtl_res_count *out) { memset(out, 0, sizeof *out); out->live_tex = (uint32_t)vk_tex_live(); }
void mtl_res_counts(uint32_t tick, const char *why) { VKLOG("resource counts tick %u %s: textures %llu", tick, why, (unsigned long long)vk_tex_live()); }
void mtl_set_tex_pool_n(int n) { (void)n; }
/* probes of the Metal backend's GPU paths: not ported (return 0 checks) */
#define PROBE(n) int mtl_probe_##n(void) { VKLOG("probe " #n " is Metal-only"); return 0; }
PROBE(vertex_streams) PROBE(arg_epoch) PROBE(tex_slots) PROBE(pool_retire) PROBE(anomaly) PROBE(widen) PROBE(blockrt) PROBE(midclear)
PROBE(srgb_view) PROBE(storage_view) PROBE(compressed_alias) PROBE(linear_rt_upload) PROBE(texture_retire) PROBE(ds_clear) PROBE(ub_rename)
PROBE(ubo_snapshot) PROBE(drv_c0) PROBE(dispatch_chain) PROBE(copy_alias) PROBE(layer_merge) PROBE(cube_merge) PROBE(winding)
PROBE(depth_output) PROBE(tile) PROBE(upload_batch)
#undef PROBE
