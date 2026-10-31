// gpu_factory.h - the one place that knows which backends this platform has.
#pragma once
#include "gpu.h"
#include <memory>

namespace gpu {

// Metal on Apple platforms, Vulkan elsewhere. RECOMP_GPU_BACKEND=metal|vulkan
// selects one explicitly; an unavailable choice yields nullptr.
std::unique_ptr<Device> create_default_device();
// The backend create_default_device() would build: "metal", "vulkan" or "none".
const char *default_backend_name();

// A native surface create_swapchain() accepts, for tests that need one without
// a window: a CAMetalLayer for Metal, a hidden SDL window for Vulkan, nullptr
// when the platform cannot provide one (no display).
void *test_native_surface(int w, int h);
// What a window host does before it resizes the swapchain: the test surface
// takes the new size. An X11 swapchain is pinned to its window's size; a
// CAMetalLayer takes whatever resize() asks, so there is nothing to do.
void test_resize_native_surface(void *surface, int w, int h);

// For the SDL host: the surface create_swapchain() wants for this backend,
// given an SDL_Window* (a CAMetalLayer for Metal, the window itself for
// Vulkan). `sdl_window` is an SDL_Window*; the header stays SDL-free.
void *native_surface_for_window(void *sdl_window);
void release_window_surface(void *surface);

// The Vulkan loader the backend dlopen'ed, or nullptr for the default search;
// the SDL host passes it to SDL_Vulkan_LoadLibrary before creating a window.
const char *vulkan_loader_path();

} // namespace gpu
