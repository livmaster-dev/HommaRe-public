// vulkan_device.cpp - resources, command buffers, submission and the reaper;
// pipelines, passes and bindings live in vulkan_pipeline.cpp, the swapchain in
// vulkan_swapchain.cpp.
#include "vulkan_device.h"
#include "game_config.h"

#include "../../../platform/os.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace gpu {

VkFormat vk_format(Format f) {
    switch (f) {
    case Format::BGRA8:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::RGBA8:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::R8:
        return VK_FORMAT_R8_UNORM;
    case Format::Depth32F:
        return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_B8G8R8A8_UNORM;
}

Format format_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
        return Format::RGBA8;
    case VK_FORMAT_R8_UNORM:
        return Format::R8;
    case VK_FORMAT_D32_SFLOAT:
        return Format::Depth32F;
    default:
        return Format::BGRA8;
    }
}

static int bytes_per_pixel(Format f) {
    return f == Format::R8 ? 1 : 4;
}

static VkImageAspectFlags aspect_of(Format f) {
    return f == Format::Depth32F ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

void VulkanDevice::fail(const char *what) {
    if (!failed_)
        fprintf(stderr, "gpu/vulkan: %s; the device is now failed\n", what);
    failed_ = true;
}

// ---------------------------------------------------------------- creation

std::unique_ptr<VulkanDevice> VulkanDevice::create() {
    if (!vulkan_load())
        return nullptr;
    std::unique_ptr<VulkanDevice> d(new VulkanDevice());

    uint32_t loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&loader_version);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = RECOMP_APP_NAME;
    app.apiVersion = loader_version >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;

    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> avail(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, avail.data());
    auto has_inst = [&](const char *name) {
        for (auto &e : avail)
            if (strcmp(e.extensionName, name) == 0)
                return true;
        return false;
    };
    std::vector<const char *> inst_ext;
    VkInstanceCreateFlags inst_flags = 0;
    if (has_inst("VK_KHR_portability_enumeration")) {
        inst_ext.push_back("VK_KHR_portability_enumeration");
        inst_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    // SDL's offscreen driver (unattended runs on Linux) makes a headless
    // surface; without the extension the presenter has nothing to draw to.
    for (const char *s :
         {"VK_KHR_surface", "VK_EXT_metal_surface", "VK_KHR_win32_surface", "VK_KHR_xlib_surface",
          "VK_KHR_xcb_surface", "VK_KHR_wayland_surface", "VK_EXT_headless_surface",
          "VK_KHR_get_physical_device_properties2",
          // What VK_EXT_swapchain_maintenance1 needs (release_drawable).
          "VK_KHR_get_surface_capabilities2", "VK_EXT_surface_maintenance1"})
        if (has_inst(s))
            inst_ext.push_back(s);
    std::vector<const char *> layers;
    if (const char *v = recomp_env("GPU_VALIDATE"); v && *v == '1') {
        uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> lp(ln);
        vkEnumerateInstanceLayerProperties(&ln, lp.data());
        for (auto &l : lp)
            if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                layers.push_back("VK_LAYER_KHRONOS_validation");
        if (layers.empty())
            fprintf(stderr, "gpu/vulkan: RECOMP_GPU_VALIDATE set but no validation layer found\n");
    }
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = inst_flags;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(inst_ext.size());
    ici.ppEnabledExtensionNames = inst_ext.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (VkResult r = vkCreateInstance(&ici, nullptr, &d->instance_); r != VK_SUCCESS) {
        fprintf(stderr, "gpu/vulkan: vkCreateInstance failed (VkResult %d, loader %u.%u)\n", int(r),
                VK_API_VERSION_MAJOR(loader_version), VK_API_VERSION_MINOR(loader_version));
        return nullptr;
    }
    volkLoadInstanceOnly(d->instance_);

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(d->instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(d->instance_, &count, devices.data());
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t family = 0;
    for (int pass = 0; pass < 2 && !chosen; ++pass) {
        for (VkPhysicalDevice pd : devices) {
            VkPhysicalDeviceProperties pp;
            vkGetPhysicalDeviceProperties(pd, &pp);
            if (pass == 0 && pp.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> q(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, q.data());
            for (uint32_t i = 0; i < qn; ++i)
                if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                    chosen = pd;
                    family = i;
                    break;
                }
            if (chosen)
                break;
        }
    }
    if (!chosen) {
        fprintf(stderr, "gpu/vulkan: no physical device with a graphics+compute queue\n");
        return nullptr;
    }
    d->physical_ = chosen;
    d->queue_family_ = family;
    vkGetPhysicalDeviceProperties(chosen, &d->props_);
    vkGetPhysicalDeviceMemoryProperties(chosen, &d->memory_props_);

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(chosen, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> dext(en);
    vkEnumerateDeviceExtensionProperties(chosen, nullptr, &en, dext.data());
    auto has_dev = [&](const char *name) {
        for (auto &e : dext)
            if (strcmp(e.extensionName, name) == 0)
                return true;
        return false;
    };
    std::vector<const char *> dev_ext;
    if (has_dev("VK_KHR_swapchain"))
        dev_ext.push_back("VK_KHR_swapchain");
    if (has_dev("VK_KHR_portability_subset"))
        dev_ext.push_back("VK_KHR_portability_subset");
    const bool maintenance1_ext = has_dev("VK_EXT_swapchain_maintenance1") &&
                                  has_inst("VK_EXT_surface_maintenance1") &&
                                  has_inst("VK_KHR_get_surface_capabilities2");
    d->core13_ = d->props_.apiVersion >= VK_API_VERSION_1_3 && app.apiVersion >= VK_API_VERSION_1_3;
    if (!d->core13_) {
        if (!has_dev("VK_KHR_dynamic_rendering")) {
            fprintf(stderr, "gpu/vulkan: device lacks dynamic rendering\n");
            return nullptr;
        }
        dev_ext.push_back("VK_KHR_dynamic_rendering");
    }

    VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sub;
    vkGetPhysicalDeviceProperties2(chosen, &p2);
    d->subgroup_size_ = sub.subgroupSize ? sub.subgroupSize : 32;
    d->subgroup_arithmetic_ = (sub.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                              (sub.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT);
    VkPhysicalDeviceFeatures base{};
    vkGetPhysicalDeviceFeatures(chosen, &base);
    d->anisotropy_ = base.samplerAnisotropy;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.samplerAnisotropy = base.samplerAnisotropy;
    f2.features.largePoints = base.largePoints;
    // What the Direct3D 9 renderer uses when the device has it.
    f2.features.textureCompressionBC = base.textureCompressionBC;
    f2.features.independentBlend = base.independentBlend;
    f2.features.fillModeNonSolid = base.fillModeNonSolid;
    f2.features.occlusionQueryPrecise = base.occlusionQueryPrecise;
    f2.features.depthClamp = base.depthClamp;
    // The vertex fetch check writes what it fetched from the vertex shader.
    f2.features.vertexPipelineStoresAndAtomics = base.vertexPipelineStoresAndAtomics;
    d->enabled_features_ = f2.features;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR fdr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    if (d->core13_) {
        VkPhysicalDeviceVulkan13Features have13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 have2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        have2.pNext = &have13;
        vkGetPhysicalDeviceFeatures2(chosen, &have2);
        f13.dynamicRendering = VK_TRUE;
        if (have13.subgroupSizeControl && have13.computeFullSubgroups) {
            f13.subgroupSizeControl = f13.computeFullSubgroups = VK_TRUE;
            d->full_subgroups_ = true;
        }
        f2.pNext = &f13;
    } else {
        fdr.dynamicRendering = VK_TRUE;
        f2.pNext = &fdr;
    }
    // An image acquired and then not drawn (a frame whose composite could not
    // be allocated) goes back to the swapchain with vkReleaseSwapchainImagesEXT.
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT fsm{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    if (maintenance1_ext) {
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT have{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 have2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        have2.pNext = &have;
        vkGetPhysicalDeviceFeatures2(chosen, &have2);
        if (have.swapchainMaintenance1) {
            dev_ext.push_back("VK_EXT_swapchain_maintenance1");
            fsm.swapchainMaintenance1 = VK_TRUE;
            fsm.pNext = f2.pNext;
            f2.pNext = &fsm;
            d->release_images_ = true;
        }
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.data();
    if (vkCreateDevice(chosen, &dci, nullptr, &d->device_) != VK_SUCCESS) {
        fprintf(stderr, "gpu/vulkan: vkCreateDevice failed\n");
        return nullptr;
    }
    volkLoadDevice(d->device_);
    // A 1.1/1.2 device (CrossOver's winevulkan, older drivers) offers dynamic
    // rendering through the KHR extension only; volk does not alias those
    // entry points onto the core 1.3 names the backend calls.
    if (!vkCmdBeginRendering && vkCmdBeginRenderingKHR) {
        vkCmdBeginRendering = vkCmdBeginRenderingKHR;
        vkCmdEndRendering = vkCmdEndRenderingKHR;
    }
    if (!vkCmdBeginRendering) {
        fprintf(stderr, "gpu/vulkan: no dynamic rendering entry points\n");
        return nullptr;
    }
    d->release_images_ = d->release_images_ && vkReleaseSwapchainImagesEXT;
    // RECOMP_VULKAN_RELEASE_IMAGES=0 takes the path a device without the
    // extension takes (release_drawable presents the image instead), for tests.
    if (const char *e = recomp_env("VULKAN_RELEASE_IMAGES"); e && *e == '0')
        d->release_images_ = false;
    vkGetDeviceQueue(d->device_, family, 0, &d->queue_);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (vkCreateCommandPool(d->device_, &pci, nullptr, &d->transfer_pool_) != VK_SUCCESS)
        return nullptr;
    if (const char *t = recomp_env("GPU_TRACE"); t && *t == '1')
        d->trace_ = true;
    if (!d->init_pipeline_layout())
        return nullptr;
    d->reaper_ = std::thread([p = d.get()] { p->reap_loop(); });
    return d;
}

VulkanDevice::~VulkanDevice() {
    if (!device_) {
        if (instance_)
            vkDestroyInstance(instance_, nullptr);
        return;
    }
    flush_pending();
    wait_all_submitted();
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        reaper_cv_.notify_all();
    }
    if (reaper_.joinable())
        reaper_.join();
    vkDeviceWaitIdle(device_);
    for (auto &[id, c] : swapchains_) {
        for (auto &[tid, idx] : c->acquired)
            textures_.erase(tid);
        for (VkImageView v : c->views)
            vkDestroyImageView(device_, v, nullptr);
        for (VkSemaphore s : c->acquire_semaphores)
            vkDestroySemaphore(device_, s, nullptr);
        for (VkSemaphore s : c->finished_semaphores)
            vkDestroySemaphore(device_, s, nullptr);
        if (c->swapchain)
            vkDestroySwapchainKHR(device_, c->swapchain, nullptr);
        if (c->surface)
            vkDestroySurfaceKHR(instance_, c->surface, nullptr);
    }
    for (auto &[id, t] : textures_)
        destroy_tex(t);
    for (auto &[id, b] : buffers_)
        destroy_buf(b);
    for (auto &g : graves_) {
        destroy_tex(g.tex);
        destroy_buf(g.buf);
    }
    if (dummy_buffer_.buffer) {
        vkDestroyBuffer(device_, dummy_buffer_.buffer, nullptr);
        vkFreeMemory(device_, dummy_buffer_.memory, nullptr);
    }
    auto free_cmd = [&](Cmd &c) {
        if (c.pool)
            vkDestroyDescriptorPool(device_, c.pool, nullptr);
        for (VkDescriptorPool p : c.extra_pools)
            vkDestroyDescriptorPool(device_, p, nullptr);
        if (c.ring)
            vkDestroyBuffer(device_, c.ring, nullptr);
        if (c.ring_memory)
            vkFreeMemory(device_, c.ring_memory, nullptr);
        if (c.queries)
            vkDestroyQueryPool(device_, c.queries, nullptr);
        if (c.fence)
            vkDestroyFence(device_, c.fence, nullptr);
        if (c.pool_of)
            vkDestroyCommandPool(device_, c.pool_of, nullptr);
    };
    for (auto &c : free_cmds_)
        free_cmd(*c);
    for (auto &[id, c] : recording_)
        free_cmd(*c);
    for (auto &[id, f] : families_)
        for (auto &[k, p] : f.variants)
            vkDestroyPipeline(device_, p, nullptr);
    for (auto &[id, cp] : computes_)
        vkDestroyPipeline(device_, cp.pipeline, nullptr);
    for (auto &[name, m] : modules_)
        vkDestroyShaderModule(device_, m, nullptr);
    for (auto &[k, s] : samplers_)
        vkDestroySampler(device_, s, nullptr);
    if (pipeline_layout_)
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (set_layout_)
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (transfer_fence_)
        vkDestroyFence(device_, transfer_fence_, nullptr);
    vkDestroyCommandPool(device_, transfer_pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);
}

// ------------------------------------------------------------------ memory

uint32_t VulkanDevice::memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memory_props_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory_props_.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

bool VulkanDevice::allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want,
                            VkDeviceMemory *out) {
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(req.memoryTypeBits, want);
    if (mai.memoryTypeIndex == UINT32_MAX && want == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        mai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
    if (mai.memoryTypeIndex == UINT32_MAX)
        return false;
    return vkAllocateMemory(device_, &mai, nullptr, out) == VK_SUCCESS;
}

static bool make_host_buffer(VulkanDevice &d, uint64_t bytes, VkBufferUsageFlags usage,
                             VkBuffer *buffer, VkDeviceMemory *memory, void **map) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::max<uint64_t>(bytes, 16);
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d.device_, &bci, nullptr, buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d.device_, *buffer, &req);
    if (!d.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    memory) ||
        vkBindBufferMemory(d.device_, *buffer, *memory, 0) != VK_SUCCESS ||
        vkMapMemory(d.device_, *memory, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) {
        vkDestroyBuffer(d.device_, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// --------------------------------------------------------------- one-shots

void VulkanDevice::full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &m, 0, nullptr, 0, nullptr);
}

VkCommandBuffer VulkanDevice::one_shot_begin() {
    transfer_mutex_.lock(); // released by one_shot_end_wait
    flush_pending_locked(); // queue order puts the pending transfers first
    if (!transfer_cb_) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        ai.commandPool = transfer_pool_; // its own pool, serialised by transfer_mutex_
        vkAllocateCommandBuffers(device_, &ai, &transfer_cb_);
        vkCreateFence(device_, &fci, nullptr, &transfer_fence_);
    } else {
        vkResetCommandBuffer(transfer_cb_, 0);
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(transfer_cb_, &bi);
    return transfer_cb_;
}

void VulkanDevice::one_shot_end_wait(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    {
        std::lock_guard lock(queue_mutex_);
        vkResetFences(device_, 1, &transfer_fence_);
        if (vkQueueSubmit(queue_, 1, &si, transfer_fence_) != VK_SUCCESS)
            fail("one-shot submit failed");
    }
    const double t0 = trace_ ? now_seconds() : 0;
    vkWaitForFences(device_, 1, &transfer_fence_, VK_TRUE, UINT64_MAX);
    ++one_shots_;
    if (trace_) {
        std::lock_guard lock(mutex_);
        ++trace_second_.one_shots;
        trace_second_.one_shot_ms += (now_seconds() - t0) * 1000;
    }
    transfer_mutex_.unlock();
}

// ---------------------------------------------------------------- textures

Texture VulkanDevice::create_texture(const TextureDesc &desc) {
    if (failed_ || desc.width <= 0 || desc.height <= 0)
        return {};
    Tex t;
    t.desc = desc;
    int levels = 1;
    if (desc.mip_levels > 1)
        for (int w = desc.width, h = desc.height; w > 1 || h > 1;
             w = std::max(1, w / 2), h = std::max(1, h / 2))
            ++levels;
    t.desc.mip_levels = levels;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vk_format(desc.format);
    ici.extent = {uint32_t(desc.width), uint32_t(desc.height), 1};
    ici.mipLevels = uint32_t(levels);
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.usage & UsageRenderTarget)
        ici.usage |= desc.format == Format::Depth32F ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &t.image) != VK_SUCCESS)
        return {};
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, t.image, &req);
    t.bytes = req.size;
    if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &t.memory) ||
        vkBindImageMemory(device_, t.image, t.memory, 0) != VK_SUCCESS) {
        vkDestroyImage(device_, t.image, nullptr);
        if (t.memory)
            vkFreeMemory(device_, t.memory, nullptr);
        return {};
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ici.format;
    vci.subresourceRange = {aspect_of(desc.format), 0, uint32_t(levels), 0, 1};
    if (vkCreateImageView(device_, &vci, nullptr, &t.view) != VK_SUCCESS) {
        vkDestroyImage(device_, t.image, nullptr);
        vkFreeMemory(device_, t.memory, nullptr);
        return {};
    }
    if (desc.usage & UsageCpu) {
        t.staging_bytes = uint64_t(desc.width) * desc.height * bytes_per_pixel(desc.format);
        if (!make_host_buffer(*this, t.staging_bytes,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              &t.staging, &t.staging_memory, &t.staging_map)) {
            vkDestroyImageView(device_, t.view, nullptr);
            vkDestroyImage(device_, t.image, nullptr);
            vkFreeMemory(device_, t.memory, nullptr);
            return {};
        }
    }
    // Move to GENERAL once, in the pending transfer buffer: every consumer's
    // submission is preceded by its flush, so the image is never used first.
    {
        std::lock_guard transfer(transfer_mutex_);
        Cmd *p = pending_locked();
        if (!p) {
            destroy_tex(t);
            return {};
        }
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.image;
        b.subresourceRange = vci.subresourceRange;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(p->buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        pending_dirty_ = true;
    }
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    textures_[id] = t;
    return {id};
}

void VulkanDevice::destroy_tex(Tex &t) {
    if (t.external)
        return;
    if (t.view)
        vkDestroyImageView(device_, t.view, nullptr);
    if (t.image)
        vkDestroyImage(device_, t.image, nullptr);
    if (t.memory)
        vkFreeMemory(device_, t.memory, nullptr);
    if (t.staging)
        vkDestroyBuffer(device_, t.staging, nullptr);
    if (t.staging_memory)
        vkFreeMemory(device_, t.staging_memory, nullptr);
    t = Tex{};
}

void VulkanDevice::destroy_buf(Buf &b) {
    if (b.buffer)
        vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory)
        vkFreeMemory(device_, b.memory, nullptr);
    b = Buf{};
}

void VulkanDevice::bury(Tex tex, Buf buf) {
    Grave g;
    g.tex = tex;
    g.buf = buf;
    for (auto &[id, c] : recording_)
        g.waits.push_back(id);
    for (auto &s : submitted_)
        g.waits.push_back(s.id);
    if (g.waits.empty()) {
        destroy_tex(g.tex);
        destroy_buf(g.buf);
        return;
    }
    graves_.push_back(std::move(g));
}

void VulkanDevice::retire_graves(uint64_t cmd_id) {
    for (size_t i = 0; i < graves_.size();) {
        auto &w = graves_[i].waits;
        w.erase(std::remove(w.begin(), w.end(), cmd_id), w.end());
        if (w.empty()) {
            destroy_tex(graves_[i].tex);
            destroy_buf(graves_[i].buf);
            graves_[i] = std::move(graves_.back());
            graves_.pop_back();
        } else {
            ++i;
        }
    }
}

static bool region_in_level(const TextureDesc &d, Region r, int level) {
    if (level < 0 || level >= d.mip_levels)
        return false;
    int w = std::max(1, d.width >> level), h = std::max(1, d.height >> level);
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= w && r.y + r.h <= h;
}

bool VulkanDevice::upload(Texture tex, Region region, const void *bytes, int pitch, int level) {
    Tex t;
    {
        std::lock_guard lock(mutex_);
        auto it = textures_.find(tex.id);
        if (it == textures_.end() || it->second.external)
            return false;
        t = it->second;
    }
    if (!bytes || !region_in_level(t.desc, region, level))
        return false;
    const int bpp = bytes_per_pixel(t.desc.format);
    const uint64_t row = uint64_t(region.w) * bpp, total = row * region.h;
    // A transient staging buffer per upload: the copy runs later, in the
    // pending transfer buffer, so the shared staging cannot be reused yet.
    // The graveyard frees it once that buffer has retired.
    Buf staging;
    staging.bytes = total;
    if (!make_host_buffer(*this, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging.buffer,
                          &staging.memory, &staging.map))
        return false;
    for (int y = 0; y < region.h; ++y)
        memcpy(static_cast<uint8_t *>(staging.map) + y * row,
               static_cast<const uint8_t *>(bytes) + uint64_t(y) * pitch, size_t(row));
    std::lock_guard transfer(transfer_mutex_);
    Cmd *p = pending_locked();
    if (!p) {
        destroy_buf(staging);
        return false;
    }
    full_barrier(p->buffer); // orders this copy after earlier copies to the same image
    VkBufferImageCopy c{};
    c.bufferRowLength = uint32_t(region.w);
    c.bufferImageHeight = uint32_t(region.h);
    c.imageSubresource = {aspect_of(t.desc.format), uint32_t(level), 0, 1};
    c.imageOffset = {region.x, region.y, 0};
    c.imageExtent = {uint32_t(region.w), uint32_t(region.h), 1};
    vkCmdCopyBufferToImage(p->buffer, staging.buffer, t.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    pending_dirty_ = true;
    std::lock_guard lock(mutex_);
    bury(Tex{}, staging);
    return true;
}

bool VulkanDevice::readback(Texture tex, Region region, void *bytes, int pitch) {
    Tex t;
    {
        std::lock_guard lock(mutex_);
        auto it = textures_.find(tex.id);
        if (it == textures_.end() || it->second.external)
            return false;
        t = it->second;
    }
    if (!bytes || !region_in_level(t.desc, region, 0))
        return false;
    const int bpp = bytes_per_pixel(t.desc.format);
    const uint64_t row = uint64_t(region.w) * bpp, total = row * region.h;
    VkBuffer dst = t.staging;
    VkDeviceMemory dst_mem = VK_NULL_HANDLE;
    void *map = t.staging_map;
    const bool transient = !dst || total > t.staging_bytes;
    if (transient &&
        !make_host_buffer(*this, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst, &dst_mem, &map))
        return false;
    VkCommandBuffer cb = one_shot_begin();
    full_barrier(cb);
    VkBufferImageCopy c{};
    c.bufferRowLength = uint32_t(region.w);
    c.bufferImageHeight = uint32_t(region.h);
    c.imageSubresource = {aspect_of(t.desc.format), 0, 0, 1};
    c.imageOffset = {region.x, region.y, 0};
    c.imageExtent = {uint32_t(region.w), uint32_t(region.h), 1};
    vkCmdCopyImageToBuffer(cb, t.image, VK_IMAGE_LAYOUT_GENERAL, dst, 1, &c);
    full_barrier(cb);
    one_shot_end_wait(cb);
    for (int y = 0; y < region.h; ++y)
        memcpy(static_cast<uint8_t *>(bytes) + uint64_t(y) * pitch,
               static_cast<const uint8_t *>(map) + y * row, size_t(row));
    if (transient) {
        vkDestroyBuffer(device_, dst, nullptr);
        vkFreeMemory(device_, dst_mem, nullptr);
    }
    return true;
}

void VulkanDevice::destroy(Texture tex) {
    std::lock_guard lock(mutex_);
    auto it = textures_.find(tex.id);
    if (it == textures_.end())
        return;
    Tex t = it->second;
    textures_.erase(it);
    if (!t.external)
        bury(t, Buf{});
}

TextureDesc VulkanDevice::describe(Texture tex) {
    std::lock_guard lock(mutex_);
    auto it = textures_.find(tex.id);
    return it == textures_.end() ? TextureDesc{} : it->second.desc;
}

uint64_t VulkanDevice::allocated_bytes(Texture tex) {
    std::lock_guard lock(mutex_);
    auto it = textures_.find(tex.id);
    return it == textures_.end() ? 0 : it->second.bytes;
}

Texture VulkanDevice::register_external_image(VkImage image, VkImageView view,
                                              const TextureDesc &desc, uint64_t swapchain,
                                              uint32_t index) {
    Tex t;
    t.image = image;
    t.view = view;
    t.desc = desc;
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t.external = true;
    t.swapchain = swapchain;
    t.image_index = index;
    uint64_t id = next_id_++;
    textures_[id] = t;
    return {id};
}

void VulkanDevice::transition_if_external(Cmd &c, Tex &t, VkImageLayout to) {
    if (!t.external || t.layout == to)
        return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = t.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(c.buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    t.layout = to;
}

Texture VulkanDevice::import_image(VkImage image, VkImageView view, const TextureDesc &desc) {
    Tex t;
    t.image = image;
    t.view = view;
    t.desc = desc;
    t.layout = VK_IMAGE_LAYOUT_GENERAL;
    t.external = true;
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    textures_[id] = t;
    return {id};
}

bool VulkanDevice::submit_native(VkCommandBuffer cb, VkFence fence) {
    if (failed_)
        return false;
    flush_pending();
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    std::lock_guard lock(queue_mutex_);
    if (vkQueueSubmit(queue_, 1, &si, fence) != VK_SUCCESS) {
        fail("native submit failed");
        return false;
    }
    return true;
}

// ----------------------------------------------------------------- buffers

Buffer VulkanDevice::create_buffer(uint64_t bytes, const void *contents) {
    if (failed_)
        return {};
    Buf b;
    b.bytes = bytes;
    if (!make_host_buffer(*this, bytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          &b.buffer, &b.memory, &b.map))
        return {};
    if (contents && bytes)
        memcpy(b.map, contents, size_t(bytes));
    else if (bytes)
        memset(b.map, 0, size_t(bytes));
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    buffers_[id] = b;
    return {id};
}

void VulkanDevice::update(Buffer buf, uint64_t offset, const void *bytes, uint64_t count) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(buf.id);
    if (it == buffers_.end() || offset + count > it->second.bytes)
        return;
    memcpy(static_cast<uint8_t *>(it->second.map) + offset, bytes, size_t(count));
}

const void *VulkanDevice::map_read(Buffer buf) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(buf.id);
    return it == buffers_.end() ? nullptr : it->second.map;
}

uint64_t VulkanDevice::buffer_bytes(Buffer buf) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(buf.id);
    return it == buffers_.end() ? 0 : it->second.bytes;
}

void VulkanDevice::destroy(Buffer buf) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(buf.id);
    if (it == buffers_.end())
        return;
    Buf b = it->second;
    buffers_.erase(it);
    bury(Tex{}, b);
}

// --------------------------------------------------------- command buffers

CommandBuffer VulkanDevice::begin() {
    if (failed_)
        return {};
    std::unique_ptr<Cmd> c;
    {
        std::lock_guard lock(mutex_);
        if (!free_cmds_.empty()) {
            c = std::move(free_cmds_.back());
            free_cmds_.pop_back();
        }
    }
    if (!c) {
        c = std::make_unique<Cmd>();
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = queue_family_;
        if (vkCreateCommandPool(device_, &pci, nullptr, &c->pool_of) != VK_SUCCESS) {
            fail("command pool creation failed");
            return {};
        }
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = c->pool_of;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_, &ai, &c->buffer);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(device_, &fci, nullptr, &c->fence);
        if (props_.limits.timestampComputeAndGraphics) {
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = 2;
            vkCreateQueryPool(device_, &qci, nullptr, &c->queries);
        }
        c->ring_bytes = 4u << 20;
        if (!make_host_buffer(*this, c->ring_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &c->ring,
                              &c->ring_memory, reinterpret_cast<void **>(&c->ring_map))) {
            fail("bytes ring allocation failed");
            return {};
        }
        create_descriptor_pool(*c);
    }
    c->ring_used = 0;
    c->callbacks.clear();
    c->bindings_dirty = true;
    c->bound = VK_NULL_HANDLE;
    c->pipeline = 0;
    c->in_render = c->in_compute = false;
    c->presents = false;
    c->presented = nullptr;
    c->wait_semaphore = c->signal_semaphore = VK_NULL_HANDLE;
    for (auto &b : c->buffers)
        b = {};
    for (auto &t : c->textures)
        t = {};
    {
        const double t0 = trace_ ? now_seconds() : 0;
        vkResetCommandPool(device_, c->pool_of, 0); // the Cmd's own pool: no lock needed
        if (trace_ && now_seconds() - t0 > 0.02)
            fprintf(stderr,
                    "gpu/vulkan: slow reset %.1f ms (previous draws %u dispatches %u binds %u)\n",
                    (now_seconds() - t0) * 1000, c->draws, c->dispatches, c->binds);
        c->draws = c->dispatches = c->binds = 0;
        if (c->pool)
            vkResetDescriptorPool(device_, c->pool, 0);
        for (VkDescriptorPool p : c->extra_pools)
            vkDestroyDescriptorPool(device_, p, nullptr);
        c->extra_pools.clear();
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c->buffer, &bi);
    if (c->queries) {
        vkCmdResetQueryPool(c->buffer, c->queries, 0, 2);
        vkCmdWriteTimestamp(c->buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c->queries, 0);
    }
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    recording_[id] = std::move(c);
    return {id};
}

VulkanDevice::Cmd *VulkanDevice::cmd(CommandBuffer cb) {
    auto it = recording_.find(cb.id);
    return it == recording_.end() ? nullptr : it->second.get();
}

VulkanDevice::Binding VulkanDevice::ring_alloc(Cmd &c, const void *bytes, uint64_t count) {
    const uint64_t align = std::max<uint64_t>(256, props_.limits.minStorageBufferOffsetAlignment);
    const uint64_t start = (c.ring_used + align - 1) & ~(align - 1);
    if (start + count > c.ring_bytes) {
        fail("bytes ring exhausted in one command buffer");
        return {};
    }
    memcpy(c.ring_map + start, bytes, size_t(count));
    c.ring_used = start + count;
    return {c.ring, start, std::max<uint64_t>(count, 16)};
}

void VulkanDevice::on_complete(CommandBuffer cb, std::function<void(CommandStatus, double)> fn) {
    std::lock_guard lock(mutex_);
    if (Cmd *c = cmd(cb))
        c->callbacks.push_back(std::move(fn));
}

VulkanDevice::Cmd *VulkanDevice::pending_locked() {
    if (!pending_id_) {
        CommandBuffer cb = begin();
        if (!cb)
            return nullptr;
        pending_id_ = cb.id;
    }
    std::lock_guard lock(mutex_);
    return cmd({pending_id_});
}

void VulkanDevice::flush_pending_locked() {
    if (!pending_id_)
        return;
    if (!pending_dirty_)
        return; // keep the open, empty buffer for the next transfer
    std::unique_ptr<Cmd> c;
    const uint64_t id = pending_id_;
    {
        std::lock_guard lock(mutex_);
        auto it = recording_.find(id);
        if (it != recording_.end()) {
            c = std::move(it->second);
            recording_.erase(it);
        }
    }
    pending_id_ = 0;
    pending_dirty_ = false;
    if (c)
        submit_cmd(id, std::move(c));
}

void VulkanDevice::flush_pending() {
    std::lock_guard transfer(transfer_mutex_);
    flush_pending_locked();
}

void VulkanDevice::commit(CommandBuffer cb) {
    flush_pending(); // the transfers this work may read go first, in queue order
    std::unique_ptr<Cmd> c;
    {
        std::lock_guard lock(mutex_);
        auto it = recording_.find(cb.id);
        if (it == recording_.end())
            return;
        c = std::move(it->second);
        recording_.erase(it);
        end_passes(*c);
    }
    submit_cmd(cb.id, std::move(c));
}

void VulkanDevice::submit_cmd(uint64_t id, std::unique_ptr<Cmd> c) {
    const CommandBuffer cb{id};
    if (c->queries)
        vkCmdWriteTimestamp(c->buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->queries, 1);
    vkEndCommandBuffer(c->buffer);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c->buffer;
    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (c->wait_semaphore) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &c->wait_semaphore;
        si.pWaitDstStageMask = &wait_stage;
    }
    if (c->signal_semaphore) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &c->signal_semaphore;
    }
    VkResult r;
    const double t0 = trace_ ? now_seconds() : 0;
    {
        std::lock_guard lock(queue_mutex_);
        vkResetFences(device_, 1, &c->fence);
        r = vkQueueSubmit(queue_, 1, &si, c->fence);
        if (r == VK_SUCCESS && c->presents)
            queue_present(*c);
    }
    std::unique_lock lock(mutex_);
    if (trace_) {
        trace_second_.submit_ms += (now_seconds() - t0) * 1000;
        ++trace_second_.commits;
        trace_second_.draws += c->draws;
        trace_tick();
    }
    if (r != VK_SUCCESS) {
        fail("queue submit failed");
        auto callbacks = std::move(c->callbacks);
        auto presented = std::move(c->presented);
        c->presented = nullptr;
        retired_[cb.id] = CommandStatus::Error;
        retired_order_.push_back(cb.id);
        retire_graves(cb.id);
        free_cmds_.push_back(std::move(c));
        completing_.push_back(cb.id);
        retired_cv_.notify_all();
        // As in reap_loop: callbacks run, and what they hold is released, unlocked.
        lock.unlock();
        for (auto &fn : callbacks)
            fn(CommandStatus::Error, 0);
        callbacks.clear();
        presented = nullptr;
        lock.lock();
        end_completing(cb.id);
        return;
    }
    if (trace_)
        fprintf(stderr,
                "gpu/vulkan: commit %llu draws %u dispatches %u binds %u ring %llu KB textures %zu "
                "buffers %zu graves %zu recording %zu submitted %zu oneshots %u\n",
                (unsigned long long)cb.id, c->draws, c->dispatches, c->binds,
                (unsigned long long)(c->ring_used >> 10), textures_.size(), buffers_.size(),
                graves_.size(), recording_.size(), submitted_.size(), one_shots_);
    one_shots_ = 0;
    submitted_.push_back({cb.id, std::move(c)});
    reaper_cv_.notify_one();
}

void VulkanDevice::reap_loop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (submitted_.empty()) {
            reaper_cv_.wait(lock);
            continue;
        }
        VkFence fence = submitted_.front().cmd->fence;
        lock.unlock();
        VkResult r = vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
        lock.lock();
        if (submitted_.empty() || submitted_.front().cmd->fence != fence)
            continue;
        Submission s = std::move(submitted_.front());
        submitted_.pop_front();
        const double done_at = now_seconds();
        double ms = 0;
        uint64_t ts[2] = {0, 0};
        if (s.cmd->queries &&
            vkGetQueryPoolResults(device_, s.cmd->queries, 0, 2, sizeof ts, ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            ts[1] >= ts[0])
            ms = double(ts[1] - ts[0]) * props_.limits.timestampPeriod / 1e6;
        const CommandStatus status =
            r == VK_SUCCESS ? CommandStatus::Completed : CommandStatus::Error;
        if (r != VK_SUCCESS)
            fail("fence wait failed (device lost?)");
        auto callbacks = std::move(s.cmd->callbacks);
        auto presented = std::move(s.cmd->presented);
        s.cmd->callbacks.clear();
        s.cmd->presented = nullptr;
        retired_[s.id] = status;
        retired_order_.push_back(s.id);
        retire_graves(s.id);
        while (retired_order_.size() > 1024) {
            retired_.erase(retired_order_.front());
            retired_order_.pop_front();
        }
        free_cmds_.push_back(std::move(s.cmd));
        completing_.push_back(s.id);
        retired_cv_.notify_all();
        lock.unlock();
        for (auto &fn : callbacks)
            fn(status, ms);
        if (presented)
            presented(done_at);
        // What the callbacks captured is released before the lock is taken
        // again. A presenter frame's last reference can be the one its
        // completion handler holds (a frame dropped while in flight), and its
        // destructor destroys textures, which takes mutex_: released under the
        // lock, the reaper waited on itself and every completion stopped.
        callbacks.clear();
        presented = nullptr;
        lock.lock();
        end_completing(s.id);
    }
}

// The buffer's on_complete callbacks have run (and what they held is released):
// wait() on it may return now.
void VulkanDevice::end_completing(uint64_t id) {
    for (auto it = completing_.begin(); it != completing_.end(); ++it)
        if (*it == id) {
            completing_.erase(it);
            break;
        }
    retired_cv_.notify_all();
}

void VulkanDevice::wait(CommandBuffer cb) {
    std::unique_lock lock(mutex_);
    const double t0 = trace_ ? now_seconds() : 0;
    if (trace_)
        ++trace_second_.waits;
    struct Done {
        VulkanDevice *d;
        double t0;
        ~Done() {
            if (d->trace_)
                d->trace_second_.wait_ms += (d->now_seconds() - t0) * 1000;
        }
    } done{this, t0};
    retired_cv_.wait(lock, [&] {
        for (uint64_t id : completing_)
            if (id == cb.id)
                return false; // retired, its callbacks still running
        if (retired_.count(cb.id))
            return true;
        for (auto &s : submitted_)
            if (s.id == cb.id)
                return false;
        return true; // never submitted, or long retired
    });
}

CommandStatus VulkanDevice::status(CommandBuffer cb) {
    std::lock_guard lock(mutex_);
    auto it = retired_.find(cb.id);
    if (it != retired_.end())
        return it->second;
    for (auto &s : submitted_)
        if (s.id == cb.id)
            return CommandStatus::Pending;
    return recording_.count(cb.id) ? CommandStatus::Pending : CommandStatus::Completed;
}

// Before a swapchain is rebuilt or destroyed nothing on the queue may still use
// it. vkDeviceWaitIdle did that, but it requires every queue to be externally
// synchronised, and other threads (the game's renderer, transfers) submit
// meanwhile: the wait now holds the queue as a submit does. Not with mutex_
// held: queue_present takes mutex_ inside queue_mutex_.
void VulkanDevice::queue_idle() {
    std::lock_guard lock(queue_mutex_);
    vkQueueWaitIdle(queue_);
}

void VulkanDevice::wait_all_submitted() {
    std::unique_lock lock(mutex_);
    retired_cv_.wait(lock, [&] { return submitted_.empty() && completing_.empty(); });
}

uint64_t VulkanDevice::submission_watermark() {
    std::lock_guard lock(mutex_);
    return next_id_;
}

// Submissions retire in order, so "nothing older than `id` remains" is a check
// on the queue's front. Unlike wait_all_submitted this cannot starve while the
// presenter keeps new frames in flight.
void VulkanDevice::wait_submitted_before(uint64_t id) {
    std::unique_lock lock(mutex_);
    retired_cv_.wait(lock, [&] { return submitted_.empty() || submitted_.front().id >= id; });
}

double VulkanDevice::now_seconds() {
    return double(os_monotonic_ns()) / 1e9;
}

void VulkanDevice::trace_tick() {
    const double now = now_seconds();
    TraceSecond &t = trace_second_;
    if (t.started == 0)
        t.started = now;
    if (now - t.started < 1.0)
        return;
    fprintf(stderr,
            "gpu/vulkan: %.1fs: commits %u draws %u sets %u | blocked: one-shots %u (%.1f ms) "
            "wait() %u (%.1f ms) acquire %u (%.1f ms) submit %.1f ms\n",
            now - t.started, t.commits, t.draws, t.sets, t.one_shots, t.one_shot_ms, t.waits,
            t.wait_ms, t.acquires, t.acquire_ms, t.submit_ms);
    t = TraceSecond{};
    t.started = now;
}

std::unique_ptr<Device> vulkan_create_device() {
    return VulkanDevice::create();
}

} // namespace gpu
