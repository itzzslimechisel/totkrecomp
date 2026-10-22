// Vulkan NVN backend: shader variants for the SPIR-V chain (the Windows counterpart of totk_shader_rt.m).
//
// A shader is identified as in the shared shader list (docs/shader-list.md): key = SHA-256 of its control section
// (ctl_key_len bytes) and code area. A variant is that shader translated for one bind state; what it depends on (the
// translator's "deps": the texture slot types, image formats, vertex attribute and colour output classes it actually
// reads) is recorded per shader in <cache>/<key>.variants.tsv, one line "<variant>\t<depth_minus_w>\t<deps JSON>". The
// files of a variant are <cache>/<variant>_v.{spv,json}. A draw uses any variant whose deps match the bound state.
//
// Where variants come from:
//   - tools/windows/prepare_shaders.py ("Prepare shaders", after the game import or from the setup screen): every
//     shader of the shared list found in the player's own game files, translated before play;
//   - a miss during play: translated in the background by the translator service (TOTK_XLATE_CMD, totk_xlate --service
//     --target spirv), started once. The render thread never waits: the draw is skipped until the variant is ready
//     (TOTK_SHADER_SYNC=1 waits instead, for comparisons). Each miss is appended to <profile>/shader-list-contribution.txt
//     as a "V <key> <deps>" row, the format the shared list takes contributions in.
// All index files are read when the device starts, before the first frame.
#include "nvn_vk.h"
#include "nvn_vk_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

int totk_depth_near_minus_w(void) __attribute__((weak));
static pthread_mutex_t g_rt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rt_cv = PTHREAD_COND_INITIALIZER;
static char g_dir[1024];
#ifdef _WIN32
static HANDLE g_in_w, g_out_r; static int g_svc = -1;
#endif
static uint64_t g_translated, g_hits, g_misses;

static const char *cache_dir(void) {
    if (g_dir[0]) return g_dir;
    const char *e = getenv("TOTK_SHADER_CACHE");
    if (e && *e) snprintf(g_dir, sizeof g_dir, "%s", e);
    else { const char *la = getenv("LOCALAPPDATA"); snprintf(g_dir, sizeof g_dir, "%s/MacNativeGames/TOTK/shader-cache-vk", la ? la : "."); }
#ifdef _WIN32
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", g_dir);
    for (char *p = tmp + 3; *p; p++) if (*p == '/' || *p == '\\') { char c = *p; *p = 0; CreateDirectoryA(tmp, NULL); *p = c; }
    CreateDirectoryA(tmp, NULL);
#endif
    return g_dir;
}
const char *nvn_vk_cache_dir(void) { return cache_dir(); }
static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }
static void hex(char *o, const uint8_t *b, size_t n) { static const char H[] = "0123456789abcdef"; for (size_t i = 0; i < n; i++) { o[2 * i] = H[b[i] >> 4]; o[2 * i + 1] = H[b[i] & 15]; } o[2 * n] = 0; }

// ---------- SHA-256 (the shared list's shader key) ----------
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; size_t k; } sha_ctx;
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(sha_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, d, e, f, g, h, cc;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) { uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10); w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void sha_init(sha_ctx *c) { static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; memcpy(c->h, iv, sizeof iv); c->n = 0; c->k = 0; }
static void sha_update(sha_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data; c->n += n;
    while (n) { size_t t = 64 - c->k < n ? 64 - c->k : n; memcpy(c->buf + c->k, p, t); c->k += t; p += t; n -= t; if (c->k == 64) { sha_block(c, c->buf); c->k = 0; } }
}
static void sha_final(sha_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->n * 8; uint8_t pad = 0x80, z = 0; sha_update(c, &pad, 1);
    while (c->k != 56) sha_update(c, &z, 1);
    uint8_t len[8]; for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i)); sha_update(c, len, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16); out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i]; }
}

// ---------- translator service ----------
#ifdef _WIN32
static int svc_start(void) {
    if (g_svc >= 0) return g_svc;
    g_svc = 0;
    const char *cmd = getenv("TOTK_XLATE_CMD");
    if (!cmd || !*cmd) { VKLOG("TOTK_XLATE_CMD not set: shaders not in the cache cannot be translated (draws using them are skipped)"); return 0; }
    char line[4096]; snprintf(line, sizeof line, "%s --service --target spirv --out \"%s\" --chain vk1", cmd, cache_dir());
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r, out_w;
    if (!CreatePipe(&in_r, &g_in_w, &sa, 0) || !CreatePipe(&g_out_r, &out_w, &sa, 0)) return 0;
    SetHandleInformation(g_in_w, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(g_out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {sizeof si}; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in_r; si.hStdOutput = out_w; si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) { VKLOG("cannot start the shader translator: %s", line); return 0; }
    CloseHandle(in_r); CloseHandle(out_w); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    VKLOG("shader translator: %s", line);
    return g_svc = 1;
}
static int svc_call(const char *req, char *reply, size_t n) {
    if (!svc_start()) return 0;
    DWORD w; size_t len = strlen(req);
    if (!WriteFile(g_in_w, req, (DWORD)len, &w, NULL) || !WriteFile(g_in_w, "\n", 1, &w, NULL)) return 0;
    size_t k = 0; char c; DWORD r;
    while (k + 1 < n && ReadFile(g_out_r, &c, 1, &r, NULL) && r == 1) { if (c == '\n') break; reply[k++] = c; }
    reply[k] = 0;
    return k > 0;
}
#else
static int svc_call(const char *req, char *reply, size_t n) { (void)req; (void)reply; (void)n; return 0; }
#endif

static inline uint32_t ctl_key_len(const uint8_t *control) { uint32_t n; memcpy(&n, control + 0x20, 4); return n >= 0x7cc && n < 0x880 ? n : 0x880; }

// ---------- canonical JSON (Python json.dumps(sort_keys=True, separators=(',', ':')): the shared list's deps form) ----------
static int key_cmp(const void *a, const void *b) { return strcmp((*(js *const *)a)->key, (*(js *const *)b)->key); }
static size_t canon(const js *v, char *o, size_t n, size_t at) {
#define PUT(c) do { if (at + 1 < n) o[at] = (c); at++; } while (0)
    if (!v) { const char *s = "null"; while (*s) PUT(*s++); return at; }
    switch (v->t) {
        case JS_STR: { PUT('"'); for (const char *s = v->str; *s; s++) { if (*s == '"' || *s == '\\') PUT('\\'); PUT(*s); } PUT('"'); return at; }
        case JS_TRUE: case JS_FALSE: case JS_NULL: { const char *s = v->t == JS_TRUE ? "true" : v->t == JS_FALSE ? "false" : "null"; while (*s) PUT(*s++); return at; }
        case JS_NUM: { char b[32]; snprintf(b, sizeof b, "%.17g", v->num); for (char *s = b; *s; s++) PUT(*s); return at; }
        case JS_ARR: { PUT('['); int i = 0; for (js *c = v->child; c; c = c->next) { if (i++) PUT(','); at = canon(c, o, n, at); } PUT(']'); return at; }
        case JS_OBJ: {
            js *m[256]; int k = 0; for (js *c = v->child; c && k < 256; c = c->next) m[k++] = c;
            qsort(m, (size_t)k, sizeof *m, key_cmp);
            PUT('{'); for (int i = 0; i < k; i++) { if (i) PUT(','); PUT('"'); for (const char *s = m[i]->key; *s; s++) PUT(*s); PUT('"'); PUT(':'); at = canon(m[i], o, n, at); } PUT('}'); return at; }
    }
    return at;
#undef PUT
}

// ---------- the per-shader variant index ----------
typedef struct vrec { char variant[96]; int dmw; js *deps; char *deps_text; struct vrec *next; } vrec;
typedef struct kent { char key[65]; vrec *v; struct kent *next; } kent;
#define KHASH 65536
static kent *g_keys[KHASH];
static unsigned khash(const char *k) { unsigned h = 2166136261u; for (int i = 0; i < 16; i++) h = (h ^ (unsigned char)k[i]) * 16777619u; return h & (KHASH - 1); }
static kent *kfind(const char *key, int create) {
    unsigned h = khash(key);
    for (kent *e = g_keys[h]; e; e = e->next) if (!strcmp(e->key, key)) return e;
    if (!create) return NULL;
    kent *e = calloc(1, sizeof *e); snprintf(e->key, sizeof e->key, "%s", key); e->next = g_keys[h]; g_keys[h] = e; return e;
}
static int g_index_entries;
static void index_add(const char *key, const char *variant, int dmw, const char *deps_text) {   // under g_rt_mu
    kent *e = kfind(key, 1);
    for (vrec *r = e->v; r; r = r->next) if (!strcmp(r->variant, variant)) return;
    vrec *r = calloc(1, sizeof *r); snprintf(r->variant, sizeof r->variant, "%s", variant); r->dmw = dmw;
    r->deps_text = strdup(deps_text); r->deps = js_parse(r->deps_text, strlen(r->deps_text));
    if (!r->deps) { free(r->deps_text); free(r); return; }
    r->next = e->v; e->v = r; g_index_entries++;
}
static void index_load_file(const char *path, const char *key) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t'); if (!t1) continue; *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue; *t2 = 0;
        index_add(key, line, atoi(t1 + 1), t2 + 1);
    }
    fclose(f);
}
/* All index files, before the first frame (mtl_init). */
void nvn_vk_shader_index_load(void) {
    static int done; if (done) return; done = 1;
    uint64_t t0 = vk_now_ns(); int files = 0;
#ifdef _WIN32
    char pat[1100]; snprintf(pat, sizeof pat, "%s/*.variants.tsv", cache_dir());
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    pthread_mutex_lock(&g_rt_mu);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strlen(fd.cFileName) != 64 + 13) continue;
            char key[65]; memcpy(key, fd.cFileName, 64); key[64] = 0;
            char p[1200]; snprintf(p, sizeof p, "%s/%s", cache_dir(), fd.cFileName);
            index_load_file(p, key); files++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    pthread_mutex_unlock(&g_rt_mu);
#endif
    VKLOG("shader cache %s: %d shaders, %d variants indexed in %.0f ms", cache_dir(), files, g_index_entries, (vk_now_ns() - t0) / 1e6);
}

/* deps match the bound state: every slot the shader depends on has the value it was translated for (absent answers
   are the translator's defaults: "2d" textures, "float" attributes and outputs, an empty image description). */
static int str_eq(const char *a, const char *b) { return !strcmp(a ? a : "", b ? b : ""); }
static int deps_match(const js *deps, const js *ans) {
    static const char *cats[4] = {"tex", "attr", "fout", "img"};
    for (int c = 0; c < 4; c++) {
        const js *d = js_get(deps, cats[c]), *a = js_get(ans, cats[c]);
        for (const js *m = d ? d->child : NULL; m; m = m->next) {
            const js *cur = a ? js_get(a, m->key) : NULL;
            if (c < 3) { if (!str_eq(js_str(m, ""), cur ? js_str(cur, "") : (c == 0 ? "2d" : "float"))) return 0; }
            else if (!str_eq(js_str(js_get(m, "type"), ""), js_str(js_get(cur, "type"), "")) || !str_eq(js_str(js_get(m, "fmt"), ""), js_str(js_get(cur, "fmt"), ""))) return 0;
        }
    }
    return 1;
}

// ---------- background translation ----------
typedef struct job { char key[65], variant[96]; int stage, dmw; uint8_t *code; size_t code_len; uint32_t c1o, c1t, local[3]; char *answers; struct job *next; } job;
static job *g_jobs, *g_jobs_tail; static int g_worker_started;
typedef struct pend { char variant[96]; int state; struct pend *next; } pend;   // state 1 queued / running, 2 failed, 3 done
static pend *g_pend[4096];
static pend *pend_find(const char *variant, int create) {
    unsigned h = 2166136261u; for (const char *s = variant; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; h &= 4095;
    for (pend *p = g_pend[h]; p; p = p->next) if (!strcmp(p->variant, variant)) return p;
    if (!create) return NULL;
    pend *p = calloc(1, sizeof *p); snprintf(p->variant, sizeof p->variant, "%s", variant); p->next = g_pend[h]; g_pend[h] = p; return p;
}
static void contribute(const char *key, const char *deps) {
    const char *prof = getenv("TOTK_PROFILE_DIR"); char p[1100];
    snprintf(p, sizeof p, "%s/shader-list-contribution.txt", prof && *prof ? prof : "user-data");
    FILE *f = fopen(p, "ab"); if (!f) return;
    fprintf(f, "V\t%s\t%s\n", key, deps); fclose(f);
}
static int translate(job *j) {   // worker thread, g_rt_mu not held
    const char *sn = j->stage == 0 ? "vs" : j->stage == 1 ? "ps" : "cs";
    char code_path[1200]; snprintf(code_path, sizeof code_path, "%s/%s.code", cache_dir(), j->variant);
    FILE *f = fopen(code_path, "wb"); if (!f) return 0;
    size_t skip = j->stage == 5 ? 0 : 0x30;
    fwrite(j->code + skip, 1, j->c1t - skip, f); fclose(f);
    for (char *p = code_path; *p; p++) if (*p == '\\') *p = '/';
    size_t c1n = j->c1t - j->c1o; char *c1h = malloc(2 * c1n + 1); hex(c1h, j->code + j->c1o, c1n);
    char local[64] = "";
    if (j->stage == 5) snprintf(local, sizeof local, ", \"local\": [%u, %u, %u]", j->local[0], j->local[1], j->local[2]);
    size_t rl = strlen(j->answers) + 2 * c1n + 2048; char *req = malloc(rl);
    const char *ab = strchr(j->answers, '{'); const char *ae = strrchr(j->answers, '}');
    snprintf(req, rl, "{\"variant\": \"%s\", \"stage\": \"%s\", \"code\": \"%s\", \"c1\": \"%s\"%s, \"depth_minus_w\": %s, \"caps\": %s, %.*s}",
             j->variant, sn, code_path, c1h, local, j->dmw ? "true" : "false", nvn_vk_translator_caps(), ab && ae > ab ? (int)(ae - ab - 1) : 0, ab ? ab + 1 : "");
    free(c1h);
    char reply[2048]; int ok = svc_call(req, reply, sizeof reply);
    free(req); remove(code_path);
    if (!ok || !strstr(reply, "\"ok\": true")) { VKLOG("[shader] translation failed for %s %s: %s", sn, j->variant, ok ? reply : "no translator"); return 0; }
    // its deps, from the reflection the translator wrote
    char jp[1200]; snprintf(jp, sizeof jp, "%s/%s_v.json", cache_dir(), j->variant);
    FILE *jf = fopen(jp, "rb"); if (!jf) return 0;
    fseek(jf, 0, SEEK_END); long sz = ftell(jf); fseek(jf, 0, SEEK_SET);
    char *txt = malloc((size_t)sz + 1); size_t got = fread(txt, 1, (size_t)sz, jf); txt[got] = 0; fclose(jf);
    js *refl = js_parse(txt, got);
    char deps[8192]; size_t dl = refl ? canon(js_get(refl, "deps"), deps, sizeof deps, 0) : 0;
    js_free(refl); free(txt);
    if (!dl || dl >= sizeof deps) return 0;
    deps[dl] = 0;
    char ip[1200]; snprintf(ip, sizeof ip, "%s/%s.variants.tsv", cache_dir(), j->key);
    FILE *idx = fopen(ip, "ab"); if (idx) { fprintf(idx, "%s\t%d\t%s\n", j->variant, j->dmw, deps); fclose(idx); }
    pthread_mutex_lock(&g_rt_mu); index_add(j->key, j->variant, j->dmw, deps); g_translated++; pthread_mutex_unlock(&g_rt_mu);
    contribute(j->key, deps);
    if (g_translated <= 8 || g_translated % 100 == 0) VKLOG("[shader] translated %llu during play: %s %s", (unsigned long long)g_translated, sn, j->variant);
    return 1;
}
static void *worker_main(void *arg) {
    (void)arg;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        pthread_mutex_lock(&g_rt_mu);
        while (!g_jobs) pthread_cond_wait(&g_rt_cv, &g_rt_mu);
        job *j = g_jobs; g_jobs = j->next; if (!g_jobs) g_jobs_tail = NULL;
        pthread_mutex_unlock(&g_rt_mu);
        int ok = translate(j);
        pthread_mutex_lock(&g_rt_mu);
        pend *p = pend_find(j->variant, 1); p->state = ok ? 3 : 2;
        pthread_cond_broadcast(&g_rt_cv);
        pthread_mutex_unlock(&g_rt_mu);
        free(j->code); free(j->answers); free(j);
    }
    return NULL;
}

/* answers: JSON object text {"tex": {...}, "img": {...}, "attr": {...}, "fout": {...}}. Returns 1 with path_base set
   (the variant is ready), 2 when it is being translated (skip the draw, ask again later), 0 when it cannot be had. */
int vk_rt_variant(int stage, const uint8_t *control, const uint8_t *code, const char *answers, char *path_base, size_t n) {
    uint32_t c1o, c1t; memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4);
    if (!c1t || c1t > 16u << 20 || c1o > c1t || (stage != 5 && c1o < 0x30)) { VKONCE("shader with an implausible code area (%#x / %#x)", c1t, c1o); return 0; }
    uint8_t d[32]; char key[65];
    sha_ctx c; sha_init(&c); sha_update(&c, control, ctl_key_len(control)); sha_update(&c, code, c1t); sha_final(&c, d); hex(key, d, 32);
    int dmw = totk_depth_near_minus_w ? totk_depth_near_minus_w() : 0;
    js *ans = js_parse(answers, strlen(answers));
    if (!ans) return 0;
    char variant[96];
    pthread_mutex_lock(&g_rt_mu);
    kent *e = kfind(key, 0);
    for (vrec *r = e ? e->v : NULL; r; r = r->next) {
        if (r->dmw != dmw || !deps_match(r->deps, ans)) continue;
        snprintf(path_base, n, "%s/%s_v", cache_dir(), r->variant);
        g_hits++;
        pthread_mutex_unlock(&g_rt_mu); js_free(ans); return 1;
    }
    // not translated for this state yet
    uint64_t ha = vk_hash(answers, strlen(answers), 0x9e3779b97f4a7c15ull ^ (uint64_t)dmw);
    snprintf(variant, sizeof variant, "%s-r%012llx", key, (unsigned long long)(ha & 0xffffffffffffull));
    pend *p = pend_find(variant, 1);
    if (p->state == 2) { pthread_mutex_unlock(&g_rt_mu); js_free(ans); return 0; }
    if (p->state == 3) { VKONCE("[shader] %s: the translated variant's deps do not match the state it was made for", variant); pthread_mutex_unlock(&g_rt_mu); js_free(ans); return 0; }
    if (p->state == 0) {   // new miss: queue it
        p->state = 1; g_misses++;
        job *j = calloc(1, sizeof *j);
        snprintf(j->key, sizeof j->key, "%s", key); snprintf(j->variant, sizeof j->variant, "%s", variant);
        j->stage = stage; j->dmw = dmw; j->c1o = c1o; j->c1t = c1t; j->code = malloc(c1t); memcpy(j->code, code, c1t); j->code_len = c1t;
        if (stage == 5) memcpy(j->local, control + 0x748, 12);
        j->answers = strdup(answers);
        if (g_jobs_tail) g_jobs_tail->next = j; else g_jobs = j;
        g_jobs_tail = j;
        if (!g_worker_started) { g_worker_started = 1; pthread_t t; pthread_create(&t, NULL, worker_main, NULL); pthread_detach(t); }
        pthread_cond_broadcast(&g_rt_cv);
        if (g_misses <= 4 || g_misses % 100 == 0) VKLOG("[shader] %llu variants missing from the prepared cache so far (%llu found); translating in the background", (unsigned long long)g_misses, (unsigned long long)g_hits);
    }
    static int sync = -1; if (sync < 0) { const char *s = getenv("TOTK_SHADER_SYNC"); sync = s && atoi(s); }
    if (sync) {   // comparisons: wait for it
        while (p->state == 1) pthread_cond_wait(&g_rt_cv, &g_rt_mu);
        pthread_mutex_unlock(&g_rt_mu); js_free(ans);
        if (p->state == 2) return 0;
        return vk_rt_variant(stage, control, code, answers, path_base, n);
    }
    pthread_mutex_unlock(&g_rt_mu); js_free(ans);
    return 2;
}
