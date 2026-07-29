/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_VULKAN_H
#define WLR_RENDER_VULKAN_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/wlr_renderer.h>

struct wlr_vk_image_attribs {
	VkImage image;
	VkImageLayout layout;
	VkFormat format;
};

/* waylib: dmabuf imported with COLOR_ATTACHMENT usage. Caller owns resources. */
struct waylib_vk_imported_image {
	VkImage image;
	VkDeviceMemory memories[WLR_DMABUF_MAX_PLANES];
	uint32_t n_memories;
	VkFormat format;
	VkImageLayout layout;
};

struct wlr_renderer *wlr_vk_renderer_create_with_drm_fd(int drm_fd);

VkInstance wlr_vk_renderer_get_instance(struct wlr_renderer *renderer);
VkPhysicalDevice wlr_vk_renderer_get_physical_device(struct wlr_renderer *renderer);
VkDevice wlr_vk_renderer_get_device(struct wlr_renderer *renderer);
uint32_t wlr_vk_renderer_get_queue_family(struct wlr_renderer *renderer);

bool wlr_renderer_is_vk(struct wlr_renderer *wlr_renderer);
bool wlr_texture_is_vk(struct wlr_texture *texture);

void wlr_vk_texture_get_image_attribs(struct wlr_texture *texture,
	struct wlr_vk_image_attribs *attribs);
bool wlr_vk_texture_has_alpha(struct wlr_texture *texture);

/* waylib extensions (not upstream) */
bool waylib_vk_renderer_import_dmabuf(struct wlr_renderer *renderer,
	const struct wlr_dmabuf_attributes *attribs,
	struct waylib_vk_imported_image *out);
void waylib_vk_imported_image_finish(struct wlr_renderer *renderer,
	struct waylib_vk_imported_image *image);
bool waylib_vk_renderer_flush_stage(struct wlr_renderer *renderer);

#endif

