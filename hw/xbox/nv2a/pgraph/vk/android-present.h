/*
 * Android: bridge between the nv2a Vulkan renderer and the SurfaceView
 * presenter (ui/xemu-android-display.c).
 *
 * The nv2a renderer owns the VkInstance/VkDevice/queue and renders the
 * Xbox display into an RGBA8 VkImage. The presenter needs those handles
 * plus the current display image so it can create an Android swapchain
 * on the same device and blit the image to it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NV2A_VK_ANDROID_PRESENT_H
#define NV2A_VK_ANDROID_PRESENT_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NV2AVkHandles {
    VkInstance       instance;
    VkPhysicalDevice physical_device;
    VkDevice         device;
    VkQueue          queue;
    uint32_t         queue_family;
} NV2AVkHandles;

typedef struct NV2AVkDisplayImage {
    VkImage  image;
    uint32_t width;
    uint32_t height;
} NV2AVkDisplayImage;

/*
 * Returns true and fills *out when the nv2a Vulkan renderer has been
 * initialized (i.e. the xbox machine is up). Until then, returns false;
 * the presenter should keep clearing the surface and retry.
 *
 * NOTE: The returned VkQueue is shared with the nv2a render thread.
 * Vulkan queues are not internally synchronized, so all submissions
 * must be serialized by the caller (see nv2a_vk_present_lock/unlock).
 */
bool nv2a_vk_get_handles(NV2AVkHandles *out);

/*
 * Returns true and fills *out with the current display image if one
 * exists. The image contents are only well-defined right after an nv2a
 * flip; callers must serialize against the render thread.
 */
bool nv2a_vk_get_display_image(NV2AVkDisplayImage *out);

/*
 * Serialize access to the shared VkQueue. The presenter must hold this
 * lock around its vkQueueSubmit + vkQueuePresentKHR; the nv2a renderer
 * takes the same lock around its own submissions. No-op if nv2a isn't up.
 */
void nv2a_vk_present_lock(void);
void nv2a_vk_present_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_VK_ANDROID_PRESENT_H */
