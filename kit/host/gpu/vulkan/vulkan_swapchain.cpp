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

// The caller has made the queue idle (queue_idle) when `c` has images already.
static bool build_swapchain(VulkanDevice &d, VulkanDevice::Chain &c, int width, int height) {
    for (VkImageView v : c.views)
        vkDestroyImageView(d.device_, v, nullptr);
    c.views.clear();
    c.images.clear();
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.physical_, c.surface, &caps) != VK_SUCCESS)
        return false;
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
    if (vkCreateSwapchainKHR(d.device_, &sci, nullptr, &fresh) != VK_SUCCESS)
        return false;
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

Swapchain VulkanDevice::create_swapchain(void *native_surface, int width, int height) {
    if (failed_ || !native_surface)
        return {};
    auto c = std::make_unique<Chain>();
    c->window = native_surface;
    // Mesa 25.1's headless WSI (SDL's offscreen driver) offers the extension but
    // has no release hook: vkReleaseSwapchainImagesEXT calls a null pointer there.
    const char *driver = SDL_GetCurrentVideoDriver();
    c->release = release_images_ && !(driver && strcmp(driver, "offscreen") == 0);
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
    queue_idle(); // the old images' last presents finish first
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
    if (it->second->needs_recreate) {
        lock.unlock();
        queue_idle(); // as resize: the old images' last presents finish first
        lock.lock();
        it = swapchains_.find(s.id);
        if (it == swapchains_.end() ||
            !build_swapchain(*this, *it->second, it->second->width, it->second->height))
            return {};
    }
    Chain &c = *it->second;
    VkSemaphore sem = c.acquire_semaphores[c.next_acquire % c.acquire_semaphores.size()];
    uint32_t index = 0;
    VkResult r =
        vkAcquireNextImageKHR(device_, c.swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        c.needs_recreate = true;
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

// An acquired image that will not be drawn has to go back to the swapchain:
// an image the application holds is never handed out again, and once every
// image is held vkAcquireNextImageKHR waits forever (lavapipe's headless
// swapchain spins). Dropping the bookkeeping, as this once did, kept the image.
// Where the swapchain has VK_EXT_swapchain_maintenance1 (Chain::release) it is
// released; elsewhere the only way back is to present it, cleared to black. Either way the acquire semaphore is
// consumed first, so its slot can be reused.
void VulkanDevice::release_drawable(Swapchain s, Texture t) {
    VkSemaphore sem = VK_NULL_HANDLE, finished = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    uint32_t index = 0;
    bool release = false;
    {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(s.id);
        if (it == swapchains_.end())
            return;
        Chain &c = *it->second;
        auto a = c.acquired.find(t.id);
        if (a == c.acquired.end())
            return;
        index = a->second;
        release = c.release;
        swapchain = c.swapchain;
        image = c.images[index];
        finished = c.finished_semaphores[index];
        auto w = c.acquired_wait.find(t.id);
        if (w != c.acquired_wait.end()) {
            sem = w->second;
            c.acquired_wait.erase(w);
        }
        c.acquired.erase(a);
        textures_.erase(t.id);
    }
    if (release) {
        if (sem) {
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
        VkReleaseSwapchainImagesInfoEXT ri{VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT};
        ri.swapchain = swapchain;
        ri.imageIndexCount = 1;
        ri.pImageIndices = &index;
        std::lock_guard lock(queue_mutex_); // the swapchain is the queue's to present to
        vkReleaseSwapchainImagesEXT(device_, &ri);
        return;
    }
    VkCommandBuffer cb = one_shot_begin();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    const VkClearColorValue black{{0, 0, 0, 1}};
    vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                         &b.subresourceRange);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (sem) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sem;
        si.pWaitDstStageMask = &stage;
    }
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &finished;
    bool submitted = false;
    {
        std::lock_guard lock(queue_mutex_);
        vkResetFences(device_, 1, &transfer_fence_);
        submitted = vkQueueSubmit(queue_, 1, &si, transfer_fence_) == VK_SUCCESS;
        if (submitted) {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &finished;
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain;
            pi.pImageIndices = &index;
            vkQueuePresentKHR(queue_, &pi);
        } else
            fail("release_drawable: submit failed");
    }
    if (submitted)
        vkWaitForFences(device_, 1, &transfer_fence_, VK_TRUE, UINT64_MAX);
    transfer_mutex_.unlock(); // taken by one_shot_begin
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
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(c.present_swapchain);
        if (it != swapchains_.end())
            it->second->needs_recreate = true;
    }
    c.wait_semaphore = c.signal_semaphore = VK_NULL_HANDLE;
}

double VulkanDevice::refresh_period(Swapchain s) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    return it == swapchains_.end() ? 1.0 / 60 : it->second->refresh;
}

void VulkanDevice::destroy(Swapchain s) {
    queue_idle(); // nothing on the queue may still use its images or semaphores
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    if (it == swapchains_.end())
        return;
    Chain &c = *it->second;
    for (auto &[tid, idx] : c.acquired)
        textures_.erase(tid);
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

void vulkan_test_resize_native_surface(void *surface, int w, int h) {
    SDL_Window *window = static_cast<SDL_Window *>(surface);
    SDL_SetWindowSize(window, w, h);
    SDL_SyncWindow(window);
}

void *vulkan_native_surface_for_window(void *sdl_window) {
    return sdl_window;
}

} // namespace gpu
