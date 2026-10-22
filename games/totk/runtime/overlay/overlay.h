/* overlay.h - the in-game menu (Windows, Vulkan): Dear ImGui drawn by the Vulkan backend as the last pass before present.
 *
 * Threads: the SDL main thread (frontend_win.c) feeds events with overlay_event() and opens / closes the menu (F1 or Esc,
 * Minus + Plus on a controller; fixed keys); the render thread draws it inside the present (nvn_vk_dev.c ->
 * overlay_vk_draw). While the menu is open the game's input is held (frontend_win.c does not pass keys, gamepad_sdl.c
 * reports nothing pressed). Settings changed in the menu are written to settings.ini (runtime/win/settings_ini.h). */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;
typedef struct SDL_Window SDL_Window;

/* main thread */
void overlay_init(SDL_Window *window);      /* after the window exists */
int overlay_event(const SDL_Event *e);      /* 1 = consumed by the menu (do not pass it to the game) */
void overlay_toggle(void);
int overlay_open(void);                     /* any thread */

/* render thread: one call per present, recording into cb after the game picture was blitted to the swapchain image
   (layout TRANSFER_DST_OPTIMAL on entry and on return). The weak reference in nvn_vk_dev.c keeps tests that do not
   link the overlay working. */
typedef struct {
    void *instance, *physical_device, *device, *queue;   /* VkInstance, VkPhysicalDevice, VkDevice, VkQueue */
    void *queue_mutex;                                   /* pthread_mutex_t * guarding vkQueueSubmit on that queue */
    uint32_t queue_family, image_count, image_index;
    void *command_buffer, *image;                        /* VkCommandBuffer (recording), VkImage (swapchain image) */
    uint32_t format, width, height;                      /* VkFormat of the swapchain, its extent */
    uint64_t swapchain_generation;                       /* changes when the swapchain is recreated */
} overlay_vk_frame;
int overlay_vk_draw(const overlay_vk_frame *f);         /* 1 if it recorded anything */

#ifdef __cplusplus
}
#endif
