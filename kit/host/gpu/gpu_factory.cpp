#include "gpu_factory.h"

#include <stdlib.h>
#include <string.h>
#include "../../platform/os.h"

namespace gpu {

#ifdef __EMSCRIPTEN__
// The web has one backend: the browser's WebGPU.
std::unique_ptr<Device> webgpu_create_device();

std::unique_ptr<Device> create_default_device() {
    return webgpu_create_device();
}
const char *default_backend_name() {
    return "webgpu";
}
void *test_native_surface(int, int) {
    return (void *)"#canvas";
}
void test_resize_native_surface(void *, int, int) {}
void *native_surface_for_window(void *) {
    return (void *)"#canvas"; // the canvas selector create_swapchain takes
}
void release_window_surface(void *) {}
const char *vulkan_loader_path() {
    return nullptr;
}
#else

std::unique_ptr<Device> vulkan_create_device();
void *vulkan_test_native_surface(int w, int h);
void vulkan_test_resize_native_surface(void *surface, int w, int h);
void *vulkan_native_surface_for_window(void *sdl_window);
const char *vulkan_loader_path_impl();
bool vulkan_available();
#ifdef __APPLE__
std::unique_ptr<Device> metal_create_device();
void *metal_test_native_surface(int w, int h);
void *metal_native_surface_for_window(void *sdl_window);
void metal_release_window_surface(void *surface);
#endif

static const char *chosen_backend() {
    const char *want = recomp_env("GPU_BACKEND");
    if (want && strcmp(want, "vulkan") == 0)
        return vulkan_available() ? "vulkan" : "none";
#ifdef __APPLE__
    if (want && strcmp(want, "metal") == 0)
        return "metal";
    if (!want || !*want)
        return "metal";
    return "none";
#else
    if (!want || !*want)
        return vulkan_available() ? "vulkan" : "none";
    return "none";
#endif
}

std::unique_ptr<Device> create_default_device() {
    const char *b = chosen_backend();
    if (strcmp(b, "vulkan") == 0)
        return vulkan_create_device();
#ifdef __APPLE__
    if (strcmp(b, "metal") == 0)
        return metal_create_device();
#endif
    return nullptr;
}

const char *default_backend_name() {
    return chosen_backend();
}

void *test_native_surface(int w, int h) {
    const char *b = chosen_backend();
    if (strcmp(b, "vulkan") == 0)
        return vulkan_test_native_surface(w, h);
#ifdef __APPLE__
    if (strcmp(b, "metal") == 0)
        return metal_test_native_surface(w, h);
#endif
    (void)w;
    (void)h;
    return nullptr;
}

void test_resize_native_surface(void *surface, int w, int h) {
    if (strcmp(chosen_backend(), "vulkan") == 0)
        vulkan_test_resize_native_surface(surface, w, h);
}

void *native_surface_for_window(void *sdl_window) {
    const char *b = chosen_backend();
    if (strcmp(b, "vulkan") == 0)
        return vulkan_native_surface_for_window(sdl_window);
#ifdef __APPLE__
    if (strcmp(b, "metal") == 0)
        return metal_native_surface_for_window(sdl_window);
#endif
    return nullptr;
}

void release_window_surface(void *surface) {
#ifdef __APPLE__
    if (strcmp(chosen_backend(), "metal") == 0)
        metal_release_window_surface(surface);
#endif
    (void)surface; // Vulkan: the SDL window is the surface; the host destroys it
}

const char *vulkan_loader_path() {
    return vulkan_loader_path_impl();
}

#endif // __EMSCRIPTEN__

} // namespace gpu
