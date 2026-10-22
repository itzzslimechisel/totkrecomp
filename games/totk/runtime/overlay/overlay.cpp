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
#include <mutex>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "win/settings_ini.h"
void runtime_input_clear(void);
void runtime_gamepad_clear(void);
int gamepad_sdl_mouse_enabled(void);
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
            case SDL_GAMEPAD_BUTTON_EAST: if (down) { if (g_confirmQuit) g_confirmQuit = false; else overlay_toggle(); } break;
            default: break;
            }
        }
        return 0;   /* gamepad_sdl.c still tracks the pad (the game reads nothing while the menu is open) */
    }
    default: return 0;
    }
}

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
    fprintf(stderr, "[totk-menu] ready (F1 / Esc, or Minus + Plus on a controller)\n");
    return true;
}

static void fps_window() {
    ImGui::SetNextWindowPos(ImVec2(12, 12));
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

/* Keyboard / controller navigation of our own (a selected row, drawn highlighted): Up / Down move, Left / Right change a
   value, Enter / A activates. The mouse works through ImGui as usual. */
enum { NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_OK };
static std::vector<int> g_nav;
static int g_sel;
static bool g_mouseMoved; static ImVec2 g_mouseAtOpen(-1, -1);
static bool nav_key(const SDL_Event &e) {   /* render thread: true if the key is ours */
    if (e.type != SDL_EVENT_KEY_DOWN && e.type != SDL_EVENT_KEY_UP) return false;
    int a = -1;
    switch (e.key.scancode) {
    case SDL_SCANCODE_UP: case SDL_SCANCODE_KP_8: a = NAV_UP; break; case SDL_SCANCODE_DOWN: case SDL_SCANCODE_KP_2: a = NAV_DOWN; break;   /* keypad arrows too */
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_KP_4: a = NAV_LEFT; break; case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_KP_6: a = NAV_RIGHT; break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: a = NAV_OK; break;
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
    const int rows = g_confirmQuit ? 2 : 3;
    for (int a : g_nav) {
        if (g_confirmQuit) {
            if (a == NAV_UP || a == NAV_DOWN || a == NAV_LEFT || a == NAV_RIGHT) g_sel = 1 - g_sel;
            else if (a == NAV_OK) { if (g_sel == 0) quit_game(); else g_confirmQuit = false; }
        } else {
            if (a == NAV_UP) g_sel = (g_sel + rows - 1) % rows;
            else if (a == NAV_DOWN) g_sel = (g_sel + 1) % rows;
            else if (g_sel == 1 && (a == NAV_LEFT || a == NAV_RIGHT || a == NAV_OK)) set_fps_mode((g_fpsMode + (a == NAV_LEFT ? 2 : 1)) % 3);
            else if (a == NAV_OK && g_sel == 0) overlay_toggle();
            else if (a == NAV_OK && g_sel == 2) g_confirmQuit = true;
        }
    }
    g_nav.clear();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 140));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.y * 0.62f, 0));
    ImGui::Begin("Tears of the Kingdom", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    float w = ImGui::GetContentRegionAvail().x;
    if (!g_confirmQuit) {
        if (row_button("Resume", 0, w)) overlay_toggle();
        ImGui::Spacing();
        ImGui::SeparatorText("Display");
        static const char *modes[] = {"Off", "FPS", "FPS + frame time"};
        char label[64]; snprintf(label, sizeof label, "FPS counter:  <  %s  >", modes[g_fpsMode]);
        if (row_button(label, 1, w)) set_fps_mode((g_fpsMode + 1) % 3);
        ImGui::Spacing();
        ImGui::Separator();
        if (row_button("Quit game...", 2, w)) g_confirmQuit = true;
        ImGui::Spacing();
        ImGui::TextDisabled("F1 / Esc or Minus + Plus: back to the game");
        ImGui::TextDisabled("Arrows / D-pad: move   Enter / A: choose   B: back");
    } else {
        ImGui::TextWrapped("Quit the game? Progress since the last save is lost. A save that is being written finishes first.");
        ImGui::Spacing();
        if (row_button("Quit", 0, w * 0.48f)) quit_game();
        ImGui::SameLine();
        if (row_button("Cancel", 1, ImGui::GetContentRegionAvail().x)) g_confirmQuit = false;
    }
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
    { static bool was; if (open && !was) { g_sel = 0; g_mouseMoved = false; g_mouseAtOpen = ImVec2(-1, -1); } was = open; }   /* the menu opens on Resume */
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
    return 1;
}
