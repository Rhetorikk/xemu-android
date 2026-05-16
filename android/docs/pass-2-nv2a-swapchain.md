# Pass 2 design: nv2a Vulkan renderer → Android swapchain

> Status: not yet implemented. This document is a design draft for the
> follow-up pass after the foundation in PR #1 lands.

## Problem

The foundation pass stands up two **independent** Vulkan worlds:

1. `hw/xbox/nv2a/pgraph/vk/` — the nv2a GPU emulator. Creates its own
   `VkInstance` + `VkDevice`, renders the Xbox framebuffer to an
   offscreen `VkImage`, exports that image's memory as an `OPAQUE_FD`,
   and (on desktop) imports it into an OpenGL texture via
   `glImportMemoryFdEXT` for the GL display path.
2. `ui/xemu-android-display.c` — the Android presenter. Creates *another*
   `VkInstance` + `VkDevice`, owns the `ANativeWindow`-backed
   `VkSurfaceKHR` + `VkSwapchainKHR`, and currently just clears the
   swapchain image to a solid color every frame.

These two don't see each other. To boot a game we need the nv2a's
framebuffer image to land on the Android swapchain. Three architectural
options:

### Option A — share VkInstance + VkDevice (recommended)

Build a single `VkInstance` + `VkDevice` and pass it to both nv2a and
the Android presenter. Then the presenter can directly `vkCmdBlitImage`
(or run a fullscreen blit shader for scaling) from nv2a's framebuffer
image to the acquired swapchain image. No external-memory dance, no
FD export, no cross-context synchronization.

**Implementation sketch:**

1. New file `ui/xemu-vk-context.{c,h}` owning the shared instance/device.
2. `xemu-android-display.c` switches from creating its own
   `VkInstance`/`VkDevice` to calling `xemu_vk_context_get()`.
3. `hw/xbox/nv2a/pgraph/vk/instance.c::pgraph_vk_init_instance` does the
   same when `CONFIG_ANDROID` is defined — instead of building a private
   instance, retrieves the shared one.
4. nv2a's framebuffer image gets created with usage including
   `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` so the presenter can blit it.
5. `pgraph/vk/display.c::pgraph_vk_process_pending` already produces a
   complete framebuffer image at `display_state.image`. Expose its
   `VkImage` + current layout through a small API
   (`pgraph_vk_get_present_image()`) so the presenter can pick it up.
6. The presenter's render loop becomes:
   - acquire next swapchain image
   - transition swapchain image → `TRANSFER_DST_OPTIMAL`
   - transition nv2a framebuffer → `TRANSFER_SRC_OPTIMAL`
   - `vkCmdBlitImage` (or sampled-image scaling pass for non-1:1)
   - transition swapchain image → `PRESENT_SRC_KHR`
   - `vkQueuePresentKHR`
7. Synchronization: nv2a's `flip_stall` already drains the frame. Add a
   shared `VkSemaphore` signaled by nv2a's command submission and waited
   on by the presenter.

### Option B — render directly into the swapchain image

Have nv2a render its final composition pass directly into the acquired
swapchain image. More surgery on `pgraph/vk/display.c` (the renderer
needs to know about the swapchain's image format and extent, which can
change on rotation). Saves one blit per frame. Probably not worth it
for pass 2.

### Option C — keep two instances, share via VK_KHR_external_memory_android

Use `VK_ANDROID_external_memory_android_hardware_buffer` to share an
`AHardwareBuffer`-backed image between the two instances. Closest to
the current desktop GL-interop architecture. Adds a dependency on a
device extension the foundation already deliberately removed, and
needs duplicate command buffers across the two devices. Not
recommended.

## ImGui Vulkan backend

Once the presenter owns the active swapchain image, swap the HUD
backend:

- Replace `ImGui_ImplOpenGL3_*` with `ImGui_ImplVulkan_*`
  (`subprojects/imgui` already builds the Vulkan backend when we pass
  `vulkan=enabled` — done in the foundation pass).
- The HUD becomes a final render pass against the swapchain image in
  the same command buffer that blits nv2a output.

## File-level changes

- New: `ui/xemu-vk-context.c`, `ui/xemu-vk-context.h`
- Edit: `hw/xbox/nv2a/pgraph/vk/instance.c` — when CONFIG_ANDROID, use
  the shared instance.
- Edit: `hw/xbox/nv2a/pgraph/vk/display.c` — expose the present-ready
  image via a getter; ensure `TRANSFER_SRC_BIT` is on its usage flags.
- Edit: `hw/xbox/nv2a/pgraph/vk/renderer.h` — drop `HAVE_EXTERNAL_MEMORY`
  guard around `extern` decls; both paths now use the same image
  internally.
- Edit: `ui/xemu-android-display.c` — switch to shared context; replace
  the clear-frame body with the blit-from-nv2a pipeline.
- Edit: `ui/meson.build` — add `xemu-vk-context.c` to the Android source
  set; also add the ImGui Vulkan backend file(s) once we re-enable the
  HUD.

## Synchronization details

The TRICKY part is making sure nv2a finishes its frame before the
presenter blits it. Today nv2a uses `flip_stall` to block the guest
GPU thread on the host. We can layer on top:

- nv2a's command submission signals a binary semaphore on its queue.
- Presenter's blit-and-present command buffer waits on that semaphore.
- After present, presenter signals a fence; nv2a waits on it before
  reusing the framebuffer image.

This costs one frame of latency in the worst case. On Adreno 830 at
60 fps that's ~16 ms — within Xbox-era acceptability.

## Out of scope for pass 2

- Audio (pass 3).
- Input routing (pass 4).
- Performance tuning (pass 6).
- Multi-frame-in-flight tuning. Start with double-buffered presenter
  and single-frame nv2a; tune after profiling on real hardware.

## Verification

| Step | How | Pass criterion |
|------|-----|---|
| V2.1 | `meson compile` succeeds | `libxemu.so` builds without HAVE_EXTERNAL_MEMORY references on Android |
| V2.2 | Launch APK with valid MCPX + flash | logcat shows nv2a renderer initialized via shared `VkDevice` |
| V2.3 | Frame counter advances | `vkQueuePresentKHR` returns success ≥30/s |
| V2.4 | Boot animation visible | Xbox boot logo appears on SurfaceView |
| V2.5 | Dashboard reachable | Sounds-and-Music skin renders without crashes |
