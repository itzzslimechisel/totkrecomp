/* overlay.cpp - the in-game menu (overlay.h): Dear ImGui (SDL3 + Vulkan backends, volk) drawn on the swapchain image.
 * The ImGui context lives on the render thread; the SDL main thread only queues events and flips the open flag. */
#ifndef IMGUI_IMPL_VULKAN_USE_VOLK
#define IMGUI_IMPL_VULKAN_USE_VOLK
#endif
#include <volk.h>
#include "overlay.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include <SDL3/SDL.h>
#include <pthread.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <windows.h>

extern "C" {
#include "win/settings_ini.h"
void runtime_input_clear(void);
void runtime_gamepad_clear(void);
int gamepad_sdl_mouse_enabled(void);
void audio_set_volume(int pct);
void audio_set_mute(int on);
int audio_out_list_devices(char (*names)[128], char (*ids)[256], int max);
void audio_out_switch_device(void);
void gamepad_sdl_set_deadzone(int pct);
void gamepad_sdl_set_trigger_threshold(int pct);
void gamepad_sdl_set_mouse(int on, float sens);
void gamepad_feedback_set_gain(float g);
int runtime_hid_feedback_flag(const char *name, int v);
int prof_win_start(double delay_s, double dur_s, const char *out) __attribute__((weak));
void vk_pipe_stats(uint64_t *made, uint64_t *ns, int *pending) __attribute__((weak));
void vk_rt_stats(uint64_t *translated, uint64_t *hits, uint64_t *misses) __attribute__((weak));
uint64_t mtl_present_count(void) __attribute__((weak));
extern _Atomic uint64_t g_stat_skip_shader, g_stat_skip_pipe;
}

namespace {
struct Queued { SDL_Event e; std::string text; };
SDL_Window *g_win;
std::atomic<int> g_open{0};
std::mutex g_qmu;
std::vector<Queued> g_q;
bool g_padBack, g_padStart;

/* render thread state */
bool g_ready;
VkFormat g_fmt;
uint64_t g_gen = ~0ull;
VkImage g_images[8];
VkImageView g_views[8];
VkDevice g_dev;
int g_fpsMode = -1;          /* 0 off, 1 FPS, 2 FPS + frame time */
std::atomic<bool> g_confirmQuit{false};   /* main thread (B) and render thread */
/* Window actions asked for by the render thread (menu items) and done by the SDL main thread (overlay_main_tick). */
enum { REQ_TOGGLE = 1, REQ_FULLSCREEN = 2, REQ_WINSIZE = 4, REQ_MOUSE = 8 };
std::atomic<int> g_req{0}, g_reqFullscreen{0};
std::atomic<bool> g_fullscreen{false};    /* the window's state, kept by the main thread */
std::atomic<bool> g_swapRecreate{false};  /* VSync changed: the backend makes a new swapchain after this present */
void request_main(int what, int value) { if (what == REQ_FULLSCREEN) g_reqFullscreen = value; g_req.fetch_or(what); }
void request_toggle() { request_main(REQ_TOGGLE, 0); }
uint64_t g_lastNs, g_accNs; int g_accFrames; float g_fps, g_ms;

int fps_mode_setting() {
    const char *e = getenv("TOTK_FPS_OVERLAY");
    return !e ? 0 : !strcmp(e, "2") || !strcmp(e, "full") ? 2 : !strcmp(e, "1") || !strcmp(e, "fps") ? 1 : 0;
}
void push(const SDL_Event *e) {
    Queued q; q.e = *e;
    if (e->type == SDL_EVENT_TEXT_INPUT && e->text.text) q.text = e->text.text;
    std::lock_guard<std::mutex> l(g_qmu); g_q.push_back(q);
}
void push_key(SDL_Keycode key, SDL_Scancode sc, bool down) {   /* controller buttons as keyboard navigation */
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP; e.key.key = key; e.key.scancode = sc; e.key.down = down;
    e.key.windowID = SDL_GetWindowID(g_win);
    push(&e);
}
void destroy_views() {
    for (int i = 0; i < 8; i++) { if (g_views[i]) vkDestroyImageView(g_dev, g_views[i], nullptr); g_views[i] = VK_NULL_HANDLE; g_images[i] = VK_NULL_HANDLE; }
}
}

extern "C" void overlay_init(SDL_Window *window) { g_win = window; }
extern "C" int overlay_open(void) { return g_open.load(); }

extern "C" void overlay_toggle(void) {
    int open = !g_open.load();
    if (open) { runtime_input_clear(); runtime_gamepad_clear(); }   /* nothing stays held in the game */
    g_open = open;
    if (g_win) {
        if (gamepad_sdl_mouse_enabled()) SDL_SetWindowRelativeMouseMode(g_win, !open);
        if (open) SDL_ShowCursor(); else if (gamepad_sdl_mouse_enabled()) SDL_HideCursor();
    }
    fprintf(stderr, "[totk-menu] %s\n", open ? "opened (game input held)" : "closed");
}

extern "C" void overlay_main_tick(void) {   /* SDL main thread, each loop */
    if (!g_win) return;
    g_fullscreen = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) != 0;
    int r = g_req.exchange(0);
    if (r & REQ_TOGGLE) overlay_toggle();
    if (r & REQ_FULLSCREEN) { SDL_SetWindowFullscreen(g_win, g_reqFullscreen.load() != 0); g_fullscreen = g_reqFullscreen.load() != 0; }
    if (r & REQ_MOUSE) {   /* the relative mouse mode follows when the menu closes (overlay_toggle) */
        const char *m = getenv("TOTK_MOUSE"), *s = getenv("TOTK_MOUSE_SENS");
        gamepad_sdl_set_mouse(m && !strcmp(m, "1"), s && *s ? (float)atof(s) : 1200.0f);
    }
    if ((r & REQ_WINSIZE) && !g_fullscreen) {
        const char *e = getenv("TOTK_WINDOW_SIZE"); int w = 0, h = 0;
        if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 180) { SDL_SetWindowSize(g_win, w, h); SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED); }
    }
}

extern "C" int overlay_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 || e->key.scancode == SDL_SCANCODE_ESCAPE)) { overlay_toggle(); return 1; }
        /* fall through */
    case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
        if (!g_open) return 0;
        if (e->type == SDL_EVENT_KEY_UP && (e->key.scancode == SDL_SCANCODE_F1 || e->key.scancode == SDL_SCANCODE_ESCAPE)) return 1;
        push(e); return 1;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        int b = e->gbutton.button;
        bool before = g_padBack && g_padStart;
        if (b == SDL_GAMEPAD_BUTTON_BACK) g_padBack = down;
        if (b == SDL_GAMEPAD_BUTTON_START) g_padStart = down;
        if (!before && g_padBack && g_padStart) { overlay_toggle(); return 0; }   /* Minus + Plus */
        if (g_open) {   /* D-pad moves, A (south) activates, B (east) goes back / closes */
            switch (b) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: push_key(SDLK_UP, SDL_SCANCODE_UP, down); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: push_key(SDLK_DOWN, SDL_SCANCODE_DOWN, down); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: push_key(SDLK_LEFT, SDL_SCANCODE_LEFT, down); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: push_key(SDLK_RIGHT, SDL_SCANCODE_RIGHT, down); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: push_key(SDLK_RETURN, SDL_SCANCODE_RETURN, down); break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: push_key(SDLK_PAGEUP, SDL_SCANCODE_PAGEUP, down); break;      /* LB / RB: tabs */
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: push_key(SDLK_PAGEDOWN, SDL_SCANCODE_PAGEDOWN, down); break;
            case SDL_GAMEPAD_BUTTON_EAST: if (down) { if (g_confirmQuit) g_confirmQuit = false; else overlay_toggle(); } break;
            default: break;
            }
        }
        return 0;   /* gamepad_sdl.c still tracks the pad (the game reads nothing while the menu is open) */
    }
    default: return 0;
    }
}

static void apply_menu_scale();
static bool init_imgui(const overlay_vk_frame *f) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 8; st.FrameRounding = 5; st.FramePadding = ImVec2(12, 7); st.ItemSpacing = ImVec2(10, 9); st.WindowPadding = ImVec2(18, 16);
    float scale = f->height / 1080.0f; if (scale < 0.75f) scale = 0.75f;
    st.ScaleAllSizes(scale);
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 22.0f * scale)) io.Fonts->AddFontDefault();
    if (!ImGui_ImplSDL3_InitForVulkan(g_win)) return false;
    g_dev = (VkDevice)f->device;
    g_fmt = (VkFormat)f->format;
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = (VkInstance)f->instance; ii.PhysicalDevice = (VkPhysicalDevice)f->physical_device; ii.Device = g_dev;
    ii.QueueFamily = f->queue_family; ii.Queue = (VkQueue)f->queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2; ii.ImageCount = f->image_count < 2 ? 2 : f->image_count;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1; ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return false;
    apply_menu_scale();
    fprintf(stderr, "[totk-menu] ready (F1 / Esc, or Minus + Plus on a controller)\n");
    return true;
}

static int g_fpsPos = -1;   /* 0 top left, 1 top right, 2 bottom left, 3 bottom right (TOTK_FPS_POS) */
static void fps_window() {
    if (g_fpsPos < 0) { const char *e = getenv("TOTK_FPS_POS"); g_fpsPos = e ? atoi(e) & 3 : 0; }
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(g_fpsPos & 1 ? ds.x - 12 : 12, g_fpsPos & 2 ? ds.y - 12 : 12), ImGuiCond_Always, ImVec2(g_fpsPos & 1 ? 1.0f : 0.0f, g_fpsPos & 2 ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
    if (g_fpsMode == 2) ImGui::Text("%.0f FPS  %.1f ms", g_fps, g_ms); else ImGui::Text("%.0f FPS", g_fps);
    ImGui::End();
}

static void set_fps_mode(int m) {
    g_fpsMode = m;
    settings_ini_set("TOTK_FPS_OVERLAY", m == 2 ? "full" : m == 1 ? "fps" : "0");
}

/* ---------- settings rows ---------- */
static std::string env(const char *k) { const char *v = getenv(k); return v ? v : ""; }
struct Choice { const char *label, *value; };
static int choice_index(const Choice *c, int n, const std::string &cur) { for (int i = 0; i < n; i++) if (cur == c[i].value) return i; return -1; }

/* A row of a tab: a button (activate) or a value cycled with Left / Right / Enter (change(+1 / -1)). */
struct Row {
    std::string label, value;
    bool next_start = false;              /* applied the next time the game starts */
    std::function<void(int)> change;      /* value rows */
    std::function<void()> activate;       /* button rows */
};

static const Choice kSizes[] = {{"1280 x 720", "1280x720"}, {"1600 x 900", "1600x900"}, {"1920 x 1080", "1920x1080"}, {"2560 x 1440", "2560x1440"}, {"3840 x 2160", "3840x2160"}};
static const Choice kRender[] = {{"Game default", ""}, {"1280 x 720", "1280x720"}, {"1600 x 900", "1600x900"}, {"1920 x 1080", "1920x1080"}, {"2560 x 1440", "2560x1440"}};
static const char *kFpsModes[] = {"Off", "FPS", "FPS + frame time"};

/* FPS target: 30 = the game's original pacing (TOTK_FPS60=0); otherwise the 60 FPS patch with a cap (TOTK_UNCAPPED_HZ). */
static int fps_target_index() {
    if (env("TOTK_FPS60") == "0") return 0;
    std::string hz = env("TOTK_UNCAPPED_HZ");
    return hz == "60" ? 1 : hz == "120" ? 2 : 3;
}
static void set_fps_target(int i) {
    settings_ini_set("TOTK_FPS60", i == 0 ? "0" : nullptr);
    settings_ini_set("TOTK_UNCAPPED_HZ", i == 1 ? "60" : i == 2 ? "120" : nullptr);
}

/* Output devices, listed when the Audio tab is shown (index 0 = the system default). */
static char g_devNames[16][128], g_devIds[16][256];
static int g_ndev = -1;
static void refresh_devices() { g_ndev = audio_out_list_devices(g_devNames, g_devIds, 16); if (g_ndev < 0) g_ndev = 0; }
static int device_index() {   /* 0 = default, i + 1 = g_devIds[i]; -1 = a saved device that is not present */
    std::string cur = env("TOTK_AUDIO_DEVICE");
    if (cur.empty()) return 0;
    for (int i = 0; i < g_ndev; i++) if (cur == g_devIds[i]) return i + 1;
    return -1;
}

static int env_int(const char *k, int def) { const char *v = getenv(k); return v && *v ? atoi(v) : def; }
static void apply_menu_scale() { ImGui::GetIO().FontGlobalScale = env_int("TOTK_MENU_SCALE", 100) / 100.0f; }
static std::string g_advNote;   /* result line of the last Advanced action */
static void save_diagnostics() {
    char dir[MAX_PATH]; time_t t = time(NULL); struct tm lt; localtime_s(&lt, &t);
    snprintf(dir, sizeof dir, "logs\\diagnostics-%04d%02d%02d-%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    CreateDirectoryA("logs", NULL); CreateDirectoryA(dir, NULL);
    char p[MAX_PATH + 32];
    snprintf(p, sizeof p, "%s\\totk.log", dir); CopyFileA("logs\\totk.log", p, FALSE);
    const char *ini = settings_ini_path(); snprintf(p, sizeof p, "%s\\settings.ini", dir); if (ini) CopyFileA(ini, p, FALSE);
    snprintf(p, sizeof p, "%s\\stats.txt", dir);
    FILE *f = fopen(p, "w");
    if (f) {
        uint64_t made = 0, ns = 0, tr = 0, hits = 0, miss = 0; int pend = 0;
        if (vk_pipe_stats) vk_pipe_stats(&made, &ns, &pend);
        if (vk_rt_stats) vk_rt_stats(&tr, &hits, &miss);
        fprintf(f, "fps %.1f frame_ms %.2f presents %llu\npipelines %llu (%.2f ms average) pending %d\ndraws skipped: shader %llu pipeline %llu\nshaders: %llu found in the cache, %llu missing, %llu translated during play\n",
                g_fps, g_ms, (unsigned long long)(mtl_present_count ? mtl_present_count() : 0), (unsigned long long)made, made ? ns / 1e6 / made : 0.0, pend,
                (unsigned long long)g_stat_skip_shader, (unsigned long long)g_stat_skip_pipe, (unsigned long long)hits, (unsigned long long)miss, (unsigned long long)tr);
        fclose(f);
    }
    g_advNote = std::string("Saved to ") + dir;
    fprintf(stderr, "[totk-menu] diagnostics saved to %s\n", dir);
}

static std::vector<Row> tab_rows(int tab) {
    std::vector<Row> r;
    if (tab == 0) {   /* Game */
        Row resume; resume.label = "Resume"; resume.activate = [] { request_toggle(); }; r.push_back(resume);
        Row quit; quit.label = "Quit game..."; quit.activate = [] { g_confirmQuit = true; }; r.push_back(quit);
    } else if (tab == 1) {   /* Video */
        Row fs; fs.label = "Fullscreen"; fs.value = g_fullscreen ? "On" : "Off";
        fs.change = [](int) { bool on = !g_fullscreen; settings_ini_set("TOTK_FULLSCREEN", on ? "1" : nullptr); request_main(REQ_FULLSCREEN, on); };
        r.push_back(fs);
        Row ws; ws.label = "Window size"; { std::string cur = env("TOTK_WINDOW_SIZE"); if (cur.empty()) cur = "1920x1080"; int i = choice_index(kSizes, 5, cur); ws.value = i >= 0 ? kSizes[i].label : cur; }
        ws.change = [](int d) { int i = choice_index(kSizes, 5, env("TOTK_WINDOW_SIZE").empty() ? "1920x1080" : env("TOTK_WINDOW_SIZE")); i = ((i < 0 ? 2 : i) + d + 5) % 5;
                                settings_ini_set("TOTK_WINDOW_SIZE", kSizes[i].value); request_main(REQ_WINSIZE, 1); };
        r.push_back(ws);
        Row vs; vs.label = "VSync"; vs.value = env("TOTK_VK_VSYNC_OFF") == "1" ? "Off" : "On";
        vs.change = [](int) { bool off = env("TOTK_VK_VSYNC_OFF") != "1"; settings_ini_set("TOTK_VK_VSYNC_OFF", off ? "1" : nullptr); g_swapRecreate = true; };
        r.push_back(vs);
        Row rr; rr.label = "Render resolution"; rr.next_start = true; { int i = choice_index(kRender, 5, env("TOTK_RENDER_RES")); rr.value = i >= 0 ? kRender[i].label : env("TOTK_RENDER_RES"); }
        rr.change = [](int d) { int i = choice_index(kRender, 5, env("TOTK_RENDER_RES")); i = ((i < 0 ? 0 : i) + d + 5) % 5; settings_ini_set("TOTK_RENDER_RES", kRender[i].value[0] ? kRender[i].value : nullptr); };
        r.push_back(rr);
        static const char *targets[] = {"30 (original)", "60", "120", "Display refresh"};
        Row ft; ft.label = "FPS target"; ft.next_start = true; ft.value = targets[fps_target_index()];
        ft.change = [](int d) { set_fps_target((fps_target_index() + d + 4) % 4); };
        r.push_back(ft);
        Row fc; fc.label = "FPS counter"; fc.value = kFpsModes[g_fpsMode < 0 ? 0 : g_fpsMode];
        fc.change = [](int d) { set_fps_mode((g_fpsMode + d + 3) % 3); };
        r.push_back(fc);
    } else if (tab == 2) {   /* Audio */
        if (g_ndev < 0) refresh_devices();
        Row vol; vol.label = "Volume"; { int v = env("TOTK_VOLUME").empty() ? 100 : atoi(env("TOTK_VOLUME").c_str()); char b[16]; snprintf(b, sizeof b, "%d %%", v); vol.value = b; }
        vol.change = [](int d) {
            int v = env("TOTK_VOLUME").empty() ? 100 : atoi(env("TOTK_VOLUME").c_str());
            v = v + 5 * d; v = v < 0 ? 0 : v > 100 ? 100 : v;
            char b[16]; snprintf(b, sizeof b, "%d", v);
            settings_ini_set("TOTK_VOLUME", v >= 100 ? nullptr : b); audio_set_volume(v);
        };
        r.push_back(vol);
        Row mute; mute.label = "Mute"; mute.value = env("TOTK_MUTE") == "1" ? "On" : "Off";
        mute.change = [](int) { bool on = env("TOTK_MUTE") != "1"; settings_ini_set("TOTK_MUTE", on ? "1" : nullptr); audio_set_mute(on); };
        r.push_back(mute);
        Row dev; dev.label = "Output device"; { int i = device_index(); dev.value = i == 0 ? "System default" : i > 0 ? g_devNames[i - 1] : "(saved device not connected)"; }
        dev.change = [](int d) {
            int n = g_ndev + 1, i = device_index(); i = ((i < 0 ? 0 : i) + d + n) % n;
            settings_ini_set("TOTK_AUDIO_DEVICE", i ? g_devIds[i - 1] : nullptr);
            audio_out_switch_device();
        };
        r.push_back(dev);
    } else if (tab == 3) {   /* Controls */
        auto pct = [](const char *k, int def) { std::string v = env(k); return v.empty() ? def : atoi(v.c_str()); };
        char b[32];
        Row dz; dz.label = "Stick deadzone"; snprintf(b, sizeof b, "%d %%", pct("TOTK_STICK_DEADZONE", 12)); dz.value = b;
        dz.change = [pct](int d) { int v = pct("TOTK_STICK_DEADZONE", 12) + 2 * d; v = v < 0 ? 0 : v > 30 ? 30 : v; char s[8]; snprintf(s, sizeof s, "%d", v);
                                   settings_ini_set("TOTK_STICK_DEADZONE", v == 12 ? nullptr : s); gamepad_sdl_set_deadzone(v); };
        r.push_back(dz);
        Row tr; tr.label = "Trigger press point"; snprintf(b, sizeof b, "%d %%", pct("TOTK_TRIGGER_THRESHOLD", 25)); tr.value = b;
        tr.change = [pct](int d) { int v = pct("TOTK_TRIGGER_THRESHOLD", 25) + 5 * d; v = v < 5 ? 5 : v > 90 ? 90 : v; char s[8]; snprintf(s, sizeof s, "%d", v);
                                   settings_ini_set("TOTK_TRIGGER_THRESHOLD", v == 25 ? nullptr : s); gamepad_sdl_set_trigger_threshold(v); };
        r.push_back(tr);
        Row ru; ru.label = "Rumble"; ru.value = env("TOTK_RUMBLE") == "0" ? "Off" : "On";
        ru.change = [](int) { bool on = env("TOTK_RUMBLE") == "0"; runtime_hid_feedback_flag("rumble", on); settings_ini_set("TOTK_RUMBLE", on ? nullptr : "0");   /* the flag reads the old setting */ };
        r.push_back(ru);
        Row rg; rg.label = "Rumble strength"; { double g = env("TOTK_RUMBLE_GAIN").empty() ? 1.0 : atof(env("TOTK_RUMBLE_GAIN").c_str()); snprintf(b, sizeof b, "%d %%", (int)(g * 100 + 0.5)); rg.value = b; }
        rg.change = [](int d) { double g = env("TOTK_RUMBLE_GAIN").empty() ? 1.0 : atof(env("TOTK_RUMBLE_GAIN").c_str()); int p = (int)(g * 100 + 0.5) + 25 * d; p = p < 25 ? 25 : p > 200 ? 200 : p;
                                char s[16]; snprintf(s, sizeof s, "%.2f", p / 100.0); settings_ini_set("TOTK_RUMBLE_GAIN", p == 100 ? nullptr : s); gamepad_feedback_set_gain(p / 100.0f); };
        r.push_back(rg);
        Row gy; gy.label = "Motion controls (gyro)"; gy.value = env("TOTK_GYRO") == "0" ? "Off" : "On";
        gy.change = [](int) { bool on = env("TOTK_GYRO") == "0"; runtime_hid_feedback_flag("gyro", on); settings_ini_set("TOTK_GYRO", on ? nullptr : "0");   /* the flag reads the old setting */ };
        r.push_back(gy);
        Row mo; mo.label = "Mouse aims (right stick)"; mo.value = env("TOTK_MOUSE") == "1" ? "On" : "Off";
        mo.change = [](int) { bool on = env("TOTK_MOUSE") != "1"; settings_ini_set("TOTK_MOUSE", on ? "1" : nullptr); request_main(REQ_MOUSE, 1); };
        r.push_back(mo);
        Row ms; ms.label = "Mouse sensitivity"; { int s = env("TOTK_MOUSE_SENS").empty() ? 1200 : atoi(env("TOTK_MOUSE_SENS").c_str()); snprintf(b, sizeof b, "%d", s / 12); ms.value = b; }
        ms.change = [](int d) { int s = env("TOTK_MOUSE_SENS").empty() ? 1200 : atoi(env("TOTK_MOUSE_SENS").c_str()); s += 120 * d; s = s < 240 ? 240 : s > 4800 ? 4800 : s;
                                char t[16]; snprintf(t, sizeof t, "%d", s); settings_ini_set("TOTK_MOUSE_SENS", s == 1200 ? nullptr : t); request_main(REQ_MOUSE, 1); };
        r.push_back(ms);
        static const Choice faces[] = {{"Automatic", ""}, {"By label (A is A)", "label"}, {"By position (Nintendo)", "position"}};
        Row fb; fb.label = "Face buttons"; fb.next_start = true; { int i = choice_index(faces, 3, env("TOTK_FACE_BUTTONS")); fb.value = faces[i < 0 ? 0 : i].label; }
        fb.change = [](int d) { int i = choice_index(faces, 3, env("TOTK_FACE_BUTTONS")); i = ((i < 0 ? 0 : i) + d + 3) % 3; settings_ini_set("TOTK_FACE_BUTTONS", faces[i].value[0] ? faces[i].value : nullptr); };
        r.push_back(fb);
    } else if (tab == 4) {   /* Interface */
        static const int scales[] = {80, 90, 100, 115, 130, 150};
        Row sc; sc.label = "Menu size"; { char b[16]; snprintf(b, sizeof b, "%d %%", env_int("TOTK_MENU_SCALE", 100)); sc.value = b; }
        sc.change = [](int d) { int cur = env_int("TOTK_MENU_SCALE", 100), i = 2; for (int k = 0; k < 6; k++) if (scales[k] == cur) i = k; i = (i + d + 6) % 6;
                                char b[8]; snprintf(b, sizeof b, "%d", scales[i]); settings_ini_set("TOTK_MENU_SCALE", scales[i] == 100 ? nullptr : b); apply_menu_scale(); };
        r.push_back(sc);
        static const char *corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        Row fp; fp.label = "FPS counter position"; fp.value = corners[g_fpsPos < 0 ? 0 : g_fpsPos];
        fp.change = [](int d) { g_fpsPos = ((g_fpsPos < 0 ? 0 : g_fpsPos) + d + 4) % 4; char b[4]; snprintf(b, sizeof b, "%d", g_fpsPos); settings_ini_set("TOTK_FPS_POS", g_fpsPos ? b : nullptr); };
        r.push_back(fp);
        static const int dims[] = {0, 25, 55, 75};
        Row dm; dm.label = "Background dim"; { char b[16]; snprintf(b, sizeof b, "%d %%", env_int("TOTK_MENU_DIM", 55)); dm.value = b; }
        dm.change = [](int d) { int cur = env_int("TOTK_MENU_DIM", 55), i = 2; for (int k = 0; k < 4; k++) if (dims[k] == cur) i = k; i = (i + d + 4) % 4;
                                char b[8]; snprintf(b, sizeof b, "%d", dims[i]); settings_ini_set("TOTK_MENU_DIM", dims[i] == 55 ? nullptr : b); };
        r.push_back(dm);
        Row ad; ad.label = "Advanced tab"; ad.value = env_int("TOTK_MENU_ADVANCED", 0) ? "Shown" : "Hidden";
        ad.change = [](int) { settings_ini_set("TOTK_MENU_ADVANCED", env_int("TOTK_MENU_ADVANCED", 0) ? nullptr : "1"); };
        r.push_back(ad);
    } else if (tab == 5) {   /* Advanced */
        Row pr; pr.label = "Profile CPU for 20 s"; pr.activate = [] {
            char p[MAX_PATH]; time_t t = time(NULL); struct tm lt; localtime_s(&lt, &t);
            CreateDirectoryA("logs", NULL);
            snprintf(p, sizeof p, "logs\\totk-prof-%02d%02d%02d.txt", lt.tm_hour, lt.tm_min, lt.tm_sec);
            g_advNote = prof_win_start && prof_win_start(0, 20, p) ? std::string("Profiling 20 s (close the menu to profile play) -> ") + p : "A profile already ran in this session";
        };
        r.push_back(pr);
        Row dg; dg.label = "Save diagnostics"; dg.activate = [] { save_diagnostics(); };
        r.push_back(dg);
    }
    return r;
}

/* ---------- navigation ---------- */
/* Keyboard / controller navigation of our own (a selected row, drawn highlighted): Up / Down move, Left / Right change a
   value (on the top row: the tab), Enter / A activates, Q / E or LB / RB switch tabs. The mouse works through ImGui. */
enum { NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_OK, NAV_TAB_PREV, NAV_TAB_NEXT };
static std::vector<int> g_nav;
static int g_sel, g_tab;
static bool g_mouseMoved; static ImVec2 g_mouseAtOpen(-1, -1);
static const char *kTabs[] = {"Game", "Video", "Audio", "Controls", "Interface", "Advanced"};
#define kNumTabs (env_int("TOTK_MENU_ADVANCED", 0) ? 6 : 5)
static bool nav_key(const SDL_Event &e) {   /* render thread: true if the key is ours */
    if (e.type != SDL_EVENT_KEY_DOWN && e.type != SDL_EVENT_KEY_UP) return false;
    int a = -1;
    switch (e.key.scancode) {
    case SDL_SCANCODE_UP: case SDL_SCANCODE_KP_8: a = NAV_UP; break; case SDL_SCANCODE_DOWN: case SDL_SCANCODE_KP_2: a = NAV_DOWN; break;   /* keypad arrows too */
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_KP_4: a = NAV_LEFT; break; case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_KP_6: a = NAV_RIGHT; break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: a = NAV_OK; break;
    case SDL_SCANCODE_Q: case SDL_SCANCODE_PAGEUP: a = NAV_TAB_PREV; break; case SDL_SCANCODE_E: case SDL_SCANCODE_PAGEDOWN: a = NAV_TAB_NEXT; break;
    default: return false;
    }
    if (e.type == SDL_EVENT_KEY_DOWN) g_nav.push_back(a);
    return true;
}
static bool row_button(const char *label, int row, float w) {   /* a button that is also selectable by keyboard / controller */
    bool sel = g_sel == row;
    if (sel) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.52f, 0.95f, 1)); ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.9f)); ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2); }
    bool hit = ImGui::Button(label, ImVec2(w, 0));
    if (sel) { ImGui::PopStyleVar(); ImGui::PopStyleColor(2); }
    if (g_mouseMoved && ImGui::IsItemHovered()) g_sel = row;   /* the pointer selects only once it was moved in the menu */
    return hit;
}
static void quit_game() {
    SDL_Event q; memset(&q, 0, sizeof q); q.type = SDL_EVENT_QUIT; SDL_PushEvent(&q);   /* frontend_win.c close_window: waits for a save */
    g_confirmQuit = false;
}

static void menu_window() {
    ImGuiIO &io = ImGui::GetIO();
    static bool wasConfirm;
    if (wasConfirm != g_confirmQuit) { g_sel = g_confirmQuit ? 1 : 0; wasConfirm = g_confirmQuit; }   /* the dialog opens on Cancel */
    if (g_mouseAtOpen.x < 0 && ImGui::IsMousePosValid()) g_mouseAtOpen = io.MousePos;
    if (!g_mouseMoved && g_mouseAtOpen.x >= 0 && (fabsf(io.MousePos.x - g_mouseAtOpen.x) + fabsf(io.MousePos.y - g_mouseAtOpen.y)) > 6) g_mouseMoved = true;
    std::vector<Row> rows = tab_rows(g_tab);
    const int n = g_confirmQuit ? 2 : (int)rows.size() + 1;   /* row 0 = the tab selector */
    for (int a : g_nav) {
        if (g_confirmQuit) {
            if (a == NAV_UP || a == NAV_DOWN || a == NAV_LEFT || a == NAV_RIGHT) g_sel = 1 - g_sel;
            else if (a == NAV_OK) { if (g_sel == 0) quit_game(); else g_confirmQuit = false; }
            continue;
        }
        if (a == NAV_TAB_PREV || a == NAV_TAB_NEXT || (g_sel == 0 && (a == NAV_LEFT || a == NAV_RIGHT))) {
            g_tab = (g_tab + (a == NAV_TAB_PREV || a == NAV_LEFT ? kNumTabs - 1 : 1)) % kNumTabs; g_sel = 0; rows = tab_rows(g_tab); continue;
        }
        if (a == NAV_UP) g_sel = (g_sel + n - 1) % n;
        else if (a == NAV_DOWN) g_sel = (g_sel + 1) % n;
        else if (g_sel > 0 && g_sel <= (int)rows.size()) {
            Row &r = rows[g_sel - 1];
            if (r.change && (a == NAV_LEFT || a == NAV_RIGHT || a == NAV_OK)) r.change(a == NAV_LEFT ? -1 : 1);
            else if (r.activate && a == NAV_OK) r.activate();
            rows = tab_rows(g_tab);
        }
    }
    g_nav.clear();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, env_int("TOTK_MENU_DIM", 55) * 255 / 100));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (g_tab >= kNumTabs) g_tab = 0;
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.y * 0.85f * io.FontGlobalScale, 0));
    ImGui::Begin("Tears of the Kingdom", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    float w = ImGui::GetContentRegionAvail().x;
    if (g_confirmQuit) {
        ImGui::TextWrapped("Quit the game? Progress since the last save is lost. A save that is being written finishes first.");
        ImGui::Spacing();
        if (row_button("Quit", 0, w * 0.48f)) quit_game();
        ImGui::SameLine();
        if (row_button("Cancel", 1, ImGui::GetContentRegionAvail().x)) g_confirmQuit = false;
        ImGui::End();
        return;
    }
    /* tab selector (row 0) */
    float tw = (w - ImGui::GetStyle().ItemSpacing.x * (kNumTabs - 1)) / kNumTabs;
    for (int t = 0; t < kNumTabs; t++) {
        if (t) ImGui::SameLine();
        bool cur = t == g_tab;
        if (cur) ImGui::PushStyleColor(ImGuiCol_Button, g_sel == 0 ? ImVec4(0.30f, 0.52f, 0.95f, 1) : ImVec4(0.24f, 0.36f, 0.62f, 1));
        if (cur && g_sel == 0) { ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.9f)); ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2); }
        if (ImGui::Button(kTabs[t], ImVec2(tw, 0))) { g_tab = t; g_sel = 0; rows = tab_rows(g_tab); }
        if (cur && g_sel == 0) { ImGui::PopStyleVar(); ImGui::PopStyleColor(); }
        if (cur) ImGui::PopStyleColor();
    }
    ImGui::Separator();
    bool anyNext = false;
    for (int i = 0; i < (int)rows.size(); i++) {
        Row &r = rows[i];
        char label[160];
        if (r.change) snprintf(label, sizeof label, "%s:   <  %s  >%s", r.label.c_str(), r.value.c_str(), r.next_start ? "  *" : "");
        else snprintf(label, sizeof label, "%s", r.label.c_str());
        if (row_button(label, i + 1, w)) { if (r.change) r.change(+1); else if (r.activate) r.activate(); }
        anyNext |= r.next_start;
    }
    if (g_tab == 5) {
        uint64_t made = 0, ns = 0, tr = 0, hits = 0, miss = 0; int pend = 0;
        if (vk_pipe_stats) vk_pipe_stats(&made, &ns, &pend);
        if (vk_rt_stats) vk_rt_stats(&tr, &hits, &miss);
        ImGui::SeparatorText("Statistics");
        ImGui::Text("%.1f FPS, %.1f ms per frame, %llu frames shown", g_fps, g_ms, (unsigned long long)(mtl_present_count ? mtl_present_count() : 0));
        ImGui::Text("Pipelines: %llu made (%.2f ms average), %d being made", (unsigned long long)made, made ? ns / 1e6 / made : 0.0, pend);
        ImGui::Text("Shaders: %llu from the prepared cache, %llu missing, %llu translated while playing", (unsigned long long)hits, (unsigned long long)miss, (unsigned long long)tr);
        ImGui::Text("Draws skipped (not ready): shader %llu, pipeline %llu", (unsigned long long)g_stat_skip_shader, (unsigned long long)g_stat_skip_pipe);
        if (!g_advNote.empty()) ImGui::TextWrapped("%s", g_advNote.c_str());
    }
    ImGui::Spacing();
    if (anyNext) ImGui::TextDisabled("* applies the next time the game starts");
    ImGui::TextDisabled("F1 / Esc or Minus + Plus: back to the game");
    ImGui::TextDisabled("Arrows / D-pad: move   Left / Right: change   Enter / A: choose   B: back   Q / E or LB / RB: tabs");
    ImGui::End();
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 s0, VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, s0, a0, s1, a1, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                               {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

extern "C" int overlay_vk_draw(const overlay_vk_frame *f) {
    uint64_t now = SDL_GetTicksNS();
    if (g_lastNs) { g_accNs += now - g_lastNs; g_accFrames++; }
    g_lastNs = now;
    if (g_accNs >= 500000000ull) { g_fps = g_accFrames * 1e9f / g_accNs; g_ms = g_accNs / 1e6f / g_accFrames; g_accNs = 0; g_accFrames = 0; }
    if (g_fpsMode < 0) g_fpsMode = fps_mode_setting();
    bool open = g_open.load();
    { static bool was; if (open && !was) { g_sel = 1; g_tab = 0; g_ndev = -1; g_mouseMoved = false; g_mouseAtOpen = ImVec2(-1, -1); } was = open; }   /* the menu opens on Resume */
    if (!open) { std::lock_guard<std::mutex> l(g_qmu); g_q.clear(); g_confirmQuit = false; }
    if (!g_win || (!open && !g_fpsMode) || f->image_index >= 8) return 0;
    if (!g_ready) { if (!init_imgui(f)) { g_win = nullptr; fprintf(stderr, "[totk-menu] initialization failed: menu off\n"); return 0; } g_ready = true; }
    if (f->swapchain_generation != g_gen || (VkFormat)f->format != g_fmt) {
        destroy_views(); g_gen = f->swapchain_generation;
        if ((VkFormat)f->format != g_fmt) { fprintf(stderr, "[totk-menu] swapchain format changed: menu off\n"); g_win = nullptr; return 0; }
    }
    VkImage img = (VkImage)f->image;
    if (g_images[f->image_index] != img) {
        if (g_views[f->image_index]) vkDestroyImageView(g_dev, g_views[f->image_index], nullptr);
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, img, VK_IMAGE_VIEW_TYPE_2D, g_fmt, {},
                                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(g_dev, &vi, nullptr, &g_views[f->image_index]) != VK_SUCCESS) { g_views[f->image_index] = VK_NULL_HANDLE; return 0; }
        g_images[f->image_index] = img;
    }
    {
        std::vector<Queued> q; { std::lock_guard<std::mutex> l(g_qmu); q.swap(g_q); }
        for (auto &e : q) {
            if (e.e.type == SDL_EVENT_TEXT_INPUT) e.e.text.text = e.text.c_str();
            bool used = nav_key(e.e) || ImGui_ImplSDL3_ProcessEvent(&e.e);
            if (getenv("TOTK_MENU_DEBUG") && (e.e.type == SDL_EVENT_KEY_DOWN || e.e.type == SDL_EVENT_KEY_UP))
                fprintf(stderr, "[totk-menu] key %s %d (%s)\n", e.e.type == SDL_EVENT_KEY_DOWN ? "down" : "up", (int)e.e.key.scancode, used ? "used" : "ignored");
        }
    }
    {   /* the first frame uploads the font texture with its own queue submission */
        pthread_mutex_t *qm = (pthread_mutex_t *)f->queue_mutex;
        if (qm) pthread_mutex_lock(qm);
        ImGui_ImplVulkan_NewFrame();
        if (qm) pthread_mutex_unlock(qm);
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2((float)f->width, (float)f->height);
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(1, 1);
    ImGui::NewFrame();
    if (g_fpsMode) fps_window();
    if (open) menu_window();
    ImGui::Render();
    VkCommandBuffer cb = (VkCommandBuffer)f->command_buffer;
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = g_views[f->image_index]; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {f->width, f->height}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    vkCmdEndRendering(cb);
    barrier(cb, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    return g_swapRecreate.exchange(false) ? 3 : 1;
}
