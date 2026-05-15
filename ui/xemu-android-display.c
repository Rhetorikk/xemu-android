/*
 * xemu Android Vulkan swapchain display (foundation pass)
 *
 * Stands up an independent VkInstance + VkDevice + VkSwapchainKHR backed by
 * the SurfaceView passed from JNI. For the foundation pass this clears the
 * swapchain image to a solid color every frame so we can verify the
 * end-to-end pipeline (JNI -> SDL_Window -> Vulkan surface -> swapchain ->
 * present) on real hardware.
 *
 * Future passes will replace this with a path that composites the nv2a
 * Vulkan renderer's output onto the swapchain and runs Dear ImGui's Vulkan
 * backend for the HUD.
 *
 * Copyright (c) 2026 xemu contributors. SPDX-License-Identifier: GPL-2.0
 */

#include "qemu/osdep.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <android/log.h>
#include <android/native_window.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <volk.h>

#define G_N_ELEMENTS(arr) (sizeof(arr) / sizeof((arr)[0]))

static inline void *malloc_n(size_t n, size_t sz) {
    return calloc(n, sz);
}

#define TAG "xemu-vk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define VK_CHECK(expr) do {                                            \
    VkResult _r = (expr);                                              \
    if (_r != VK_SUCCESS) {                                            \
        LOGE("%s failed: %d", #expr, _r);                              \
        goto fail;                                                     \
    }                                                                  \
} while (0)

#define MAX_FRAMES_IN_FLIGHT 2

typedef struct DisplayState {
    SDL_Window *window;
    VkInstance instance;
    VkPhysicalDevice phys_device;
    uint32_t queue_family;
    VkDevice device;
    VkQueue queue;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_image_count;
    VkImage *sc_images;
    VkCommandPool cmd_pool;
    VkCommandBuffer cmd_bufs[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore img_avail[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore render_done[MAX_FRAMES_IN_FLIGHT];
    VkFence in_flight[MAX_FRAMES_IN_FLIGHT];
    uint32_t frame_idx;
    bool needs_swapchain_recreate;
} DisplayState;

static DisplayState g_ds;
static atomic_intptr_t g_pending_window;
static atomic_int g_should_stop;

void xemu_android_display_set_window(ANativeWindow *win)
{
    atomic_store(&g_pending_window, (intptr_t)win);
    g_ds.needs_swapchain_recreate = true;
}

void xemu_android_display_request_stop(void)
{
    atomic_store(&g_should_stop, 1);
}

static bool pick_physical_device(DisplayState *ds)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(ds->instance, &n, NULL);
    if (n == 0) {
        LOGE("no Vulkan physical devices");
        return false;
    }
    VkPhysicalDevice *devs = malloc_n(n, sizeof(*devs));
    vkEnumeratePhysicalDevices(ds->instance, &n, devs);

    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        LOGI("device[%u]: %s (api=%u.%u.%u)", i, props.deviceName,
             VK_VERSION_MAJOR(props.apiVersion),
             VK_VERSION_MINOR(props.apiVersion),
             VK_VERSION_PATCH(props.apiVersion));

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        VkQueueFamilyProperties *qfp = malloc_n(qn, sizeof(*qfp));
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qfp);
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, ds->surface, &present);
            if ((qfp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                ds->phys_device = devs[i];
                ds->queue_family = q;
                free(qfp);
                free(devs);
                LOGI("Selected physical device: %s, queue family %u",
                     props.deviceName, q);
                return true;
            }
        }
        free(qfp);
    }
    free(devs);
    return false;
}

static bool create_swapchain(DisplayState *ds)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ds->phys_device, ds->surface, &caps));

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ds->phys_device, ds->surface, &fmt_count, NULL);
    if (fmt_count == 0) {
        LOGE("no surface formats");
        return false;
    }
    VkSurfaceFormatKHR *fmts = malloc_n(fmt_count, sizeof(*fmts));
    vkGetPhysicalDeviceSurfaceFormatsKHR(ds->phys_device, ds->surface, &fmt_count, fmts);
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < fmt_count; i++) {
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
             fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            pick = fmts[i];
            break;
        }
    }
    free(fmts);

    ds->sc_format = pick.format;
    ds->sc_extent = caps.currentExtent.width != UINT32_MAX
                        ? caps.currentExtent
                        : (VkExtent2D){1280, 720};
    LOGI("Swapchain extent: %ux%u, format=%d", ds->sc_extent.width,
         ds->sc_extent.height, ds->sc_format);

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && want > caps.maxImageCount) {
        want = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR sci = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = ds->surface,
        .minImageCount    = want,
        .imageFormat      = pick.format,
        .imageColorSpace  = pick.colorSpace,
        .imageExtent      = ds->sc_extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = VK_PRESENT_MODE_FIFO_KHR,
        .clipped          = VK_TRUE,
    };

    VK_CHECK(vkCreateSwapchainKHR(ds->device, &sci, NULL, &ds->swapchain));

    vkGetSwapchainImagesKHR(ds->device, ds->swapchain, &ds->sc_image_count, NULL);
    ds->sc_images = malloc_n(ds->sc_image_count, sizeof(*ds->sc_images));
    vkGetSwapchainImagesKHR(ds->device, ds->swapchain, &ds->sc_image_count, ds->sc_images);
    LOGI("Swapchain has %u images", ds->sc_image_count);
    return true;

fail:
    return false;
}

static void destroy_swapchain(DisplayState *ds)
{
    if (ds->swapchain) {
        vkDestroySwapchainKHR(ds->device, ds->swapchain, NULL);
        ds->swapchain = VK_NULL_HANDLE;
    }
    if (ds->sc_images) {
        free(ds->sc_images);
        ds->sc_images = NULL;
    }
    ds->sc_image_count = 0;
}

static bool create_sync_and_cmd(DisplayState *ds)
{
    VkCommandPoolCreateInfo pci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ds->queue_family,
    };
    VK_CHECK(vkCreateCommandPool(ds->device, &pci, NULL, &ds->cmd_pool));

    VkCommandBufferAllocateInfo cbai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = ds->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MAX_FRAMES_IN_FLIGHT,
    };
    VK_CHECK(vkAllocateCommandBuffers(ds->device, &cbai, ds->cmd_bufs));

    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                  .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VK_CHECK(vkCreateSemaphore(ds->device, &sci, NULL, &ds->img_avail[i]));
        VK_CHECK(vkCreateSemaphore(ds->device, &sci, NULL, &ds->render_done[i]));
        VK_CHECK(vkCreateFence(ds->device, &fci, NULL, &ds->in_flight[i]));
    }
    return true;
fail:
    return false;
}

static bool init_vulkan(DisplayState *ds)
{
    if (volkInitialize() != VK_SUCCESS) {
        LOGE("volkInitialize failed");
        return false;
    }

    uint32_t ext_count = 0;
    const char *const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    LOGI("SDL requested %u Vulkan instance extensions", ext_count);
    for (uint32_t i = 0; i < ext_count; i++) {
        LOGI("  ext[%u]: %s", i, sdl_exts[i]);
    }

    VkApplicationInfo app = {
        .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xemu",
        .apiVersion       = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &app,
        .enabledExtensionCount   = ext_count,
        .ppEnabledExtensionNames = sdl_exts,
    };
    VK_CHECK(vkCreateInstance(&ici, NULL, &ds->instance));
    volkLoadInstance(ds->instance);

    if (!SDL_Vulkan_CreateSurface(ds->window, ds->instance, NULL, &ds->surface)) {
        LOGE("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }

    if (!pick_physical_device(ds)) {
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ds->queue_family,
        .queueCount       = 1,
        .pQueuePriorities = &prio,
    };
    const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &dqci,
        .enabledExtensionCount   = G_N_ELEMENTS(dev_exts),
        .ppEnabledExtensionNames = dev_exts,
    };
    VK_CHECK(vkCreateDevice(ds->phys_device, &dci, NULL, &ds->device));
    volkLoadDevice(ds->device);
    vkGetDeviceQueue(ds->device, ds->queue_family, 0, &ds->queue);

    if (!create_swapchain(ds)) {
        return false;
    }
    if (!create_sync_and_cmd(ds)) {
        return false;
    }
    return true;
fail:
    return false;
}

static void render_clear_frame(DisplayState *ds, uint32_t img_idx,
                                VkCommandBuffer cmd)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier to_dst = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = ds->sc_images[img_idx],
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_dst);

    VkClearColorValue clear = { .float32 = { 0.08f, 0.20f, 0.45f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, ds->sc_images[img_idx],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1, &range);

    VkImageMemoryBarrier to_present = to_dst;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_present);

    vkEndCommandBuffer(cmd);
}

static bool draw_one_frame(DisplayState *ds)
{
    uint32_t fi = ds->frame_idx % MAX_FRAMES_IN_FLIGHT;
    vkWaitForFences(ds->device, 1, &ds->in_flight[fi], VK_TRUE, UINT64_MAX);

    uint32_t img_idx = 0;
    VkResult r = vkAcquireNextImageKHR(ds->device, ds->swapchain, UINT64_MAX,
                                       ds->img_avail[fi], VK_NULL_HANDLE,
                                       &img_idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        ds->needs_swapchain_recreate = true;
        return true;
    }
    if (r != VK_SUCCESS) {
        LOGE("vkAcquireNextImageKHR: %d", r);
        return false;
    }

    vkResetFences(ds->device, 1, &ds->in_flight[fi]);
    vkResetCommandBuffer(ds->cmd_bufs[fi], 0);
    render_clear_frame(ds, img_idx, ds->cmd_bufs[fi]);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1,
        .pWaitSemaphores      = &ds->img_avail[fi],
        .pWaitDstStageMask    = &wait_stage,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &ds->cmd_bufs[fi],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &ds->render_done[fi],
    };
    vkQueueSubmit(ds->queue, 1, &si, ds->in_flight[fi]);

    VkPresentInfoKHR pi = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &ds->render_done[fi],
        .swapchainCount     = 1,
        .pSwapchains        = &ds->swapchain,
        .pImageIndices      = &img_idx,
    };
    r = vkQueuePresentKHR(ds->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        ds->needs_swapchain_recreate = true;
    } else if (r != VK_SUCCESS) {
        LOGE("vkQueuePresentKHR: %d", r);
        return false;
    }

    ds->frame_idx++;
    return true;
}

static void destroy_display(DisplayState *ds)
{
    if (ds->device) {
        vkDeviceWaitIdle(ds->device);
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (ds->in_flight[i])   vkDestroyFence(ds->device, ds->in_flight[i], NULL);
            if (ds->img_avail[i])   vkDestroySemaphore(ds->device, ds->img_avail[i], NULL);
            if (ds->render_done[i]) vkDestroySemaphore(ds->device, ds->render_done[i], NULL);
        }
        if (ds->cmd_pool) vkDestroyCommandPool(ds->device, ds->cmd_pool, NULL);
        destroy_swapchain(ds);
        vkDestroyDevice(ds->device, NULL);
    }
    if (ds->surface && ds->instance) {
        vkDestroySurfaceKHR(ds->instance, ds->surface, NULL);
    }
    if (ds->instance) {
        vkDestroyInstance(ds->instance, NULL);
    }
    if (ds->window) {
        SDL_DestroyWindow(ds->window);
    }
    memset(ds, 0, sizeof *ds);
}

int xemu_android_display_run(void)
{
    DisplayState *ds = &g_ds;
    memset(ds, 0, sizeof *ds);

    ds->window = SDL_CreateWindow("xemu", 0, 0,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
    if (!ds->window) {
        LOGE("SDL_CreateWindow failed: %s", SDL_GetError());
        return -1;
    }

    if (!init_vulkan(ds)) {
        destroy_display(ds);
        return -2;
    }

    LOGI("display loop entering");
    while (!atomic_load(&g_should_stop)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                atomic_store(&g_should_stop, 1);
            }
        }

        if (ds->needs_swapchain_recreate) {
            vkDeviceWaitIdle(ds->device);
            destroy_swapchain(ds);
            if (!create_swapchain(ds)) {
                LOGE("swapchain recreate failed; exiting loop");
                break;
            }
            ds->needs_swapchain_recreate = false;
        }

        if (!draw_one_frame(ds)) {
            break;
        }
    }
    LOGI("display loop exited");

    destroy_display(ds);
    return 0;
}
