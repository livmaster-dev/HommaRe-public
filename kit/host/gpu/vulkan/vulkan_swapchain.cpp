// vulkan_swapchain.cpp - the surface and swapchain over an SDL window:
// acquire, present and the completion-time presented acknowledgement. The
// native surface create_swapchain() takes is always an SDL_Window*.
#include "vulkan_device.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../platform/os.h"

namespace gpu {

static VkPresentModeKHR choose_present_mode(VulkanDevice &d, VkSurfaceKHR surface) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical_, surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical_, surface, &n, modes.data());
    auto has = [&](VkPresentModeKHR m) {
        for (VkPresentModeKHR x : modes)
            if (x == m)
                return true;
        return false;
    };
    VkPresentModeKHR want =
        has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    if (const char *e = recomp_env("VULKAN_PRESENT_MODE"); e && *e) {
        if (strcmp(e, "fifo") == 0)
            want = VK_PRESENT_MODE_FIFO_KHR;
        else if (strcmp(e, "mailbox") == 0 && has(VK_PRESENT_MODE_MAILBOX_KHR))
            want = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (strcmp(e, "immediate") == 0 && has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            want = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    static bool reported = false;
    if (!reported) {
        reported = true;
        fprintf(stderr, "gpu/vulkan: present mode %s\n",
                want == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
                : want == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                        : "fifo");
    }
    return want;
}

static bool build_swapchain(VulkanDevice &d, VulkanDevice::Chain &c, int width, int height) {
    vkDeviceWaitIdle(d.device_);
    for (VkImageView v : c.views)
        vkDestroyImageView(d.device_, v, nullptr);
    c.views.clear();
    c.images.clear();
    VkSurfaceCapabilitiesKHR caps;
    if (const VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.physical_, c.surface, &caps);
        r != VK_SUCCESS) {
        c.surface_lost = r == VK_ERROR_SURFACE_LOST_KHR;
        return false;
    }
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical_, c.surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical_, c.surface, &fn, formats.data());
    c.format = formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
    VkColorSpaceKHR space =
        formats.empty() ? VK_COLOR_SPACE_SRGB_NONLINEAR_KHR : formats[0].colorSpace;
    for (auto &f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            c.format = f.format;
            space = f.colorSpace;
            break;
        }
    // The caller's size, within the surface's bounds: X11 pins min and max to
    // the window so this is the window; MoltenVK and Wayland let the swapchain
    // set the drawable size, which is what a resize on a fixed layer needs.
    VkExtent2D extent = {uint32_t(std::max(1, width)), uint32_t(std::max(1, height))};
    if (width <= 0 || height <= 0)
        extent = caps.currentExtent.width == UINT32_MAX ? VkExtent2D{1, 1} : caps.currentExtent;
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height =
        std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = c.surface;
    sci.minImageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount)
        sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = c.format;
    sci.imageColorSpace = space;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO is vsync and caps the frame rate at the display's refresh; the
    // presenter paces frames itself, so MAILBOX (uncapped, no tearing) is the
    // first choice and FIFO the fallback every driver has. RECOMP_VULKAN_PRESENT_MODE
    // = fifo | mailbox | immediate overrides, for diagnosis.
    sci.presentMode = choose_present_mode(d, c.surface);
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = c.swapchain;
    VkSwapchainKHR fresh;
    if (const VkResult r = vkCreateSwapchainKHR(d.device_, &sci, nullptr, &fresh); r != VK_SUCCESS) {
        c.surface_lost = r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
        return false;
    }
    if (c.swapchain)
        vkDestroySwapchainKHR(d.device_, c.swapchain, nullptr);
    c.swapchain = fresh;
    c.width = int(extent.width);
    c.height = int(extent.height);
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(d.device_, fresh, &n, nullptr);
    c.images.resize(n);
    vkGetSwapchainImagesKHR(d.device_, fresh, &n, c.images.data());
    for (VkImage img : c.images) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = c.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(d.device_, &vci, nullptr, &view);
        c.views.push_back(view);
    }
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    while (c.acquire_semaphores.size() < n + 1) {
        VkSemaphore s;
        vkCreateSemaphore(d.device_, &semci, nullptr, &s);
        c.acquire_semaphores.push_back(s);
    }
    while (c.finished_semaphores.size() < n) {
        VkSemaphore s;
        vkCreateSemaphore(d.device_, &semci, nullptr, &s);
        c.finished_semaphores.push_back(s);
    }
    c.needs_recreate = false;
    return true;
}

// The window's system surface went away and came back (Android destroys an
// app's surface while it is in the background or another activity covers
// it, and makes a new one when it returns): the swapchain and the
// VkSurfaceKHR over the old one go, and a new pair is made over the window
// as it is now. False while the window has no surface yet; the next acquire
// tries again.
static bool rebuild_surface(VulkanDevice &d, VulkanDevice::Chain &c) {
    vkDeviceWaitIdle(d.device_);
    for (VkImageView v : c.views)
        vkDestroyImageView(d.device_, v, nullptr);
    c.views.clear();
    c.images.clear();
    if (c.swapchain)
        vkDestroySwapchainKHR(d.device_, c.swapchain, nullptr);
    c.swapchain = VK_NULL_HANDLE;
    if (c.surface)
        vkDestroySurfaceKHR(d.instance_, c.surface, nullptr);
    c.surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window *>(c.window), d.instance_, nullptr, &c.surface)) {
        c.surface = VK_NULL_HANDLE;
        return false;
    }
    if (!build_swapchain(d, c, c.width, c.height))
        return false;
    c.surface_lost = false;
    fprintf(stderr, "gpu/vulkan: the window's surface came back: swapchain %dx%d\n", c.width, c.height);
    return true;
}

Swapchain VulkanDevice::create_swapchain(void *native_surface, int width, int height) {
    if (failed_ || !native_surface)
        return {};
    auto c = std::make_unique<Chain>();
    c->window = native_surface;
    SDL_Window *window = static_cast<SDL_Window *>(native_surface);
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &c->surface)) {
        fprintf(stderr, "gpu/vulkan: SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return {};
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queue_family_, c->surface, &supported);
    if (!supported || !build_swapchain(*this, *c, width, height)) {
        fprintf(stderr, "gpu/vulkan: %s\n",
                supported ? "no swapchain on the window's surface"
                          : "the device cannot present to the window's surface");
        vkDestroySurfaceKHR(instance_, c->surface, nullptr);
        return {};
    }
    float reported_hz = 0;
    if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) {
        reported_hz = mode->refresh_rate;
        if (mode->refresh_rate > 1.0f)
            c->refresh = 1.0 / mode->refresh_rate;
    }
    // The presenter paces one frame per refresh from this number, so a display
    // SDL reports at 60 Hz caps the game at 60 whatever the panel can do.
    fprintf(stderr,
            "gpu/vulkan: swapchain %dx%d, display reports %.2f Hz (pacing at %.1f), %u images\n",
            c->width, c->height, reported_hz, 1.0 / c->refresh, unsigned(c->images.size()));
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    swapchains_[id] = std::move(c);
    return {id};
}

void VulkanDevice::resize(Swapchain s, int width, int height) {
    // build_swapchain waits for the device to go idle itself.
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    if (it != swapchains_.end())
        build_swapchain(*this, *it->second, width, height);
}

Format VulkanDevice::swapchain_format(Swapchain s) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    return it == swapchains_.end() ? Format::BGRA8 : format_of(it->second->format);
}

Texture VulkanDevice::acquire(Swapchain s) {
    std::unique_lock lock(mutex_);
    auto it = swapchains_.find(s.id);
    if (it == swapchains_.end() || failed_)
        return {};
    Chain &c = *it->second;
    if (c.surface_lost && !rebuild_surface(*this, c))
        return {};
    if (c.needs_recreate && !build_swapchain(*this, c, c.width, c.height))
        return {}; // build_swapchain waits for the device to go idle
    VkSemaphore sem = c.acquire_semaphores[c.next_acquire % c.acquire_semaphores.size()];
    uint32_t index = 0;
    VkResult r =
        vkAcquireNextImageKHR(device_, c.swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        c.needs_recreate = true;
        return {};
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        c.surface_lost = true;
        return {};
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return {};
    if (r == VK_SUBOPTIMAL_KHR)
        c.needs_recreate = true;
    ++c.next_acquire;
    TextureDesc desc{c.width, c.height, format_of(c.format), UsageRenderTarget, 1};
    Texture t = register_external_image(c.images[index], c.views[index], desc, s.id, index);
    c.acquired[t.id] = index;
    c.acquired_wait[t.id] = sem;
    return t;
}

void VulkanDevice::release_drawable(Swapchain s, Texture t) {
    // The acquire semaphore is signalled and nobody waits on it: consume it
    // with an empty fenced submit so the slot can be reused.
    VkSemaphore sem = VK_NULL_HANDLE;
    {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(s.id);
        if (it == swapchains_.end())
            return;
        Chain &c = *it->second;
        auto w = c.acquired_wait.find(t.id);
        if (w != c.acquired_wait.end()) {
            sem = w->second;
            c.acquired_wait.erase(w);
        }
        c.acquired.erase(t.id);
        textures_.erase(t.id);
    }
    if (!sem)
        return;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sem;
    si.pWaitDstStageMask = &stage;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(device_, &fci, nullptr, &fence);
    {
        std::lock_guard lock(queue_mutex_);
        vkQueueSubmit(queue_, 1, &si, fence);
    }
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device_, fence, nullptr);
}

void VulkanDevice::present(CommandBuffer cb, Swapchain s, Texture t, double,
                           std::function<void(double)> presented) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    auto it = swapchains_.find(s.id);
    if (!c || it == swapchains_.end())
        return;
    Chain &chain = *it->second;
    auto a = chain.acquired.find(t.id);
    auto tex = textures_.find(t.id);
    if (a == chain.acquired.end() || tex == textures_.end())
        return;
    end_passes(*c);
    transition_if_external(*c, tex->second, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    c->presents = true;
    c->present_swapchain = s.id;
    c->present_image = a->second;
    c->wait_semaphore = chain.acquired_wait[t.id];
    c->signal_semaphore = chain.finished_semaphores[a->second];
    c->presented = std::move(presented);
    chain.acquired.erase(a);
    chain.acquired_wait.erase(t.id);
    textures_.erase(tex);
}

void VulkanDevice::queue_present(Cmd &c) { // queue_mutex_ held; called by commit after submit
    VkSwapchainKHR sc;
    {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(c.present_swapchain);
        if (it == swapchains_.end())
            return;
        sc = it->second->swapchain;
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &c.signal_semaphore;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &c.present_image;
    VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(c.present_swapchain);
        if (it != swapchains_.end()) {
            it->second->needs_recreate = true;
            it->second->surface_lost |= r == VK_ERROR_SURFACE_LOST_KHR;
        }
    }
    c.wait_semaphore = c.signal_semaphore = VK_NULL_HANDLE;
}

double VulkanDevice::refresh_period(Swapchain s) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    return it == swapchains_.end() ? 1.0 / 60 : it->second->refresh;
}

void VulkanDevice::destroy(Swapchain s) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    if (it == swapchains_.end())
        return;
    Chain &c = *it->second;
    for (auto &[tid, idx] : c.acquired)
        textures_.erase(tid);
    vkDeviceWaitIdle(device_);
    for (VkImageView v : c.views)
        vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore sem : c.acquire_semaphores)
        vkDestroySemaphore(device_, sem, nullptr);
    for (VkSemaphore sem : c.finished_semaphores)
        vkDestroySemaphore(device_, sem, nullptr);
    vkDestroySwapchainKHR(device_, c.swapchain, nullptr);
    vkDestroySurfaceKHR(instance_, c.surface, nullptr);
    swapchains_.erase(it);
}

void *vulkan_test_native_surface(int w, int h) {
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO))
        return nullptr;
    SDL_Vulkan_LoadLibrary(vulkan_loader_path_impl());
    return SDL_CreateWindow("gpu test", w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
}

void *vulkan_native_surface_for_window(void *sdl_window) {
    return sdl_window;
}

} // namespace gpu
