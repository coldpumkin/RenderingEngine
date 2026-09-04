#include "Vulkan/Swapchain.h"

#include "Config.h"
#include "Vulkan/Window.h"

#include <memory>
#include <utility>
#include <vector>

// ============================================================================
// UNDEFINED means none was usable. Any SRGB format will do -- the shader writes and
// reads (r,g,b,a) whatever the byte order is, so only the colour space matters.
static VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) noexcept {
    for (const VkSurfaceFormatKHR& f : available) {
        const bool srgb = f.format == VK_FORMAT_B8G8R8A8_SRGB
                       || f.format == VK_FORMAT_R8G8B8A8_SRGB
                       || f.format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
        if (srgb && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { return f; }
    }
    return VkSurfaceFormatKHR{};
}

// OPAQUE composites without reading the alpha, which is what we want -- nothing here
// uses window transparency.
//
// Unlike FIFO, the spec guarantees no particular mode is supported, so one is picked
// from what the surface reports. Writing an unsupported value in makes
// vkCreateSwapchainKHR fail with a code that does not say why.
static VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) noexcept {
    constexpr VkCompositeAlphaFlagBitsKHR kPreferred[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (VkCompositeAlphaFlagBitsKHR candidate : kPreferred) {
        if ((supported & candidate) != 0) { return candidate; }
    }
    return static_cast<VkCompositeAlphaFlagBitsKHR>(0);   // a driver breaking the spec
}

Swapchain::~Swapchain() {
    if (handle == VK_NULL_HANDLE || dev == nullptr) { return; }
    const VulkanDevice& d = *dev;

    // **No wait here.** A destructor that stalls the device would weld "let go" and
    // "destroy" into one moment, and every resize would stop the GPU.
    //
    // Contract: one of two paths has already made this safe --
    //   at run time  the RetiredSwapchain count ran out (AdvanceRetiredSwapchains)
    //   at exit      main called vkDeviceWaitIdle first
    // Out of step, the validation layer reports destroying an object still in use.

    for (SwapchainImage& img : images) {
        d.table.vkDestroySemaphore(d.handle, img.renderFinished, nullptr);
    }
    // ~Image destroys the view. The image itself has no allocation and is left alone:
    // it was queried, and vkDestroySwapchainKHR takes it.
    images.clear();

    d.table.vkDestroySwapchainKHR(d.handle, handle, nullptr);
}

bool QuerySurfaceExtent(const VulkanInstance& inst,
                        VkPhysicalDevice gpu,
                        Window* window) noexcept {
    VkSurfaceCapabilitiesKHR caps{};
    if (inst.table.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, window->surface, &caps)
            != VK_SUCCESS) {
        LOG("[vk] vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed\n");
        window->surfaceExtent = VkExtent2D{};
        return false;
    }

    // Contract: currentExtent is the window's size. On platforms that answer
    //           0xFFFFFFFF the swapchain would pick instead, and this asserts it
    //           never happens here rather than carrying a path nothing runs.
    window->surfaceExtent = caps.currentExtent;

    // Minimized. Not logged: it lasts until the window comes back, and it would be
    // hundreds of lines a second.
    return caps.currentExtent.width != 0 && caps.currentExtent.height != 0;
}

AttachmentFormats SwapchainAttachmentFormats(const Window& window) noexcept {
    AttachmentFormats formats{};
    formats.color = window.surfaceFormat.format;
    return formats;   // depth UNDEFINED and one sample: the engine gives neither
}

bool SelectSurfaceFormat(const VulkanInstance& inst,
                         VkPhysicalDevice gpu,
                         Window* window) noexcept {
    uint32_t count = 0;
    inst.table.vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, window->surface, &count, nullptr);
    if (count == 0) {
        LOG("[vk] surface reports no formats\n");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(count);
    inst.table.vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, window->surface, &count,
                                                    formats.data());

    // A non-SRGB surface would store what the shader wrote without encoding it, and
    // nothing catches that - the picture is simply wrong. No fallback path, as with 1x.
    const VkSurfaceFormatKHR chosen = ChooseSurfaceFormat(formats);
    if (chosen.format == VK_FORMAT_UNDEFINED) {
        LOG("[vk] surface offers no SRGB format\n");
        return false;
    }
    window->surfaceFormat = chosen;
    return true;
}

// The spec allows one swapchain per surface at a time, which is why recreation hands
// the old one in as oldSwapchain. That handing over retires it -- destroying it is
// still ours to do.
//
// Two tables, because a swapchain straddles the levels: the surface queries are
// instance level and the creation is device level.
bool CreateSwapchain(const VulkanInstance& inst,
                     const VulkanDevice& dev,
                     VkSurfaceKHR surface,
                     VkSurfaceFormatKHR surfaceFormat,
                     VkExtent2D extent,
                     VkSwapchainKHR oldSwapchain,
                     Swapchain* out) noexcept {
    Swapchain& sc = *out;
    sc.dev = &dev;

    VkSurfaceCapabilitiesKHR caps{};
    if (inst.table.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.gpu, surface, &caps) != VK_SUCCESS) {
        LOG("[vk] vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed\n");
        return false;
    }

    // The size is handed in rather than read out of caps here. QuerySurfaceExtent
    // asked the same pair, and one answer used twice cannot disagree with itself.
    if (extent.width == 0 || extent.height == 0) {
        return false;   // minimized. Not logged: it lasts until the window comes back
    }

    const VkCompositeAlphaFlagBitsKHR compositeAlpha =
        ChooseCompositeAlpha(caps.supportedCompositeAlpha);
    if (compositeAlpha == 0) {
        LOG("[vk] no usable composite alpha (0x%x)\n", caps.supportedCompositeAlpha);
        return false;
    }

    // Clamped to what the surface allows. maxImageCount == 0 means no upper bound, so
    // it is left out of the clamp rather than treated as zero.
    uint32_t imageCount = kDesiredSwapchainImages;
    if (imageCount < caps.minImageCount) { imageCount = caps.minImageCount; }
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = surfaceFormat.format;
    info.imageColorSpace = surfaceFormat.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    // The post-process pass draws straight into these, so COLOR_ATTACHMENT is all
    // that is needed -- and it is the one usage the spec always puts in
    // supportedUsageFlags, so there is nothing to check.
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Only the graphics queue touches a swapchain image. Compute writing here
    // directly would mean CONCURRENT or a queue family ownership transfer, both of
    // which cost something -- picked when there is a reason to.
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = compositeAlpha;
    // FIFO is the one present mode the spec guarantees. It is vsync, so no tearing.
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;

    const VkResult created = dev.table.vkCreateSwapchainKHR(dev.handle, &info, nullptr, &sc.handle);
    if (created != VK_SUCCESS) {
        LOG("[vk] vkCreateSwapchainKHR failed (%d)\n", created);
        sc.handle = VK_NULL_HANDLE;
        return false;
    }

    sc.extent = extent;

    // The count is a result, not a request: minImageCount is a floor and the driver
    // may hand back more, so it is asked again after creation.
    uint32_t actualCount = 0;
    if (dev.table.vkGetSwapchainImagesKHR(dev.handle, sc.handle, &actualCount, nullptr)
            != VK_SUCCESS || actualCount == 0) {
        LOG("[vk] vkGetSwapchainImagesKHR returned no images\n");
        return false;
    }
    std::vector<VkImage> rawImages(actualCount);
    if (dev.table.vkGetSwapchainImagesKHR(dev.handle, sc.handle, &actualCount, rawImages.data())
            != VK_SUCCESS) {
        LOG("[vk] vkGetSwapchainImagesKHR failed\n");
        return false;
    }

    // A failure partway through throws the whole thing away. Returning a half-built
    // swapchain as success would leave the caller looking at a valid handle and
    // rendering through null views.
    //
    // The destructor does the unwinding: vkDestroy* on VK_NULL_HANDLE is a no-op by
    // spec, so a half-filled array cleans up the same as a full one.
    sc.images.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        // A queried image with a view of ours on it. The desc says what the image is,
        // and the post-process pipeline has to have been built for that same format.
        Texture& texture = sc.images[i].texture;
        texture.desc = {sc.extent, surfaceFormat.format, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        texture.image.dev = &dev;
        texture.image.handle = rawImages[i];   // no allocation: it is not ours

        // The one place ownership splits: the image is the swapchain's, the view is
        // ours. Two types now say that, where an empty allocation used to.
        if (!CreateImageView(dev, rawImages[i], surfaceFormat.format, {}, &texture.view)) {
            LOG("[vk] image view failed on swapchain image %u\n", i);
            return false;
        }

        // One per image, and the reason is that present waits on this semaphore while
        // giving nothing back -- vkQueuePresentKHR hands over no fence. The only
        // evidence that it is safe to signal again is that acquire handed the same
        // image back, and that arrives as an image index.
        //
        // imageAvailable is the opposite: before the acquire there is no index, so it
        // cannot be per image. That asymmetry is the line between what belongs to the
        // swapchain and what belongs to a frame.
        //
        // The complete answer is VkSwapchainPresentFenceInfoKHR in
        // VK_KHR_swapchain_maintenance1, which puts a fence on present. That the
        // extension had to be written is the evidence there was no signal before.
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        const VkResult semResult =
            dev.table.vkCreateSemaphore(dev.handle, &semInfo, nullptr,
                                        &sc.images[i].renderFinished);
        if (semResult != VK_SUCCESS) {
            LOG("[vk] vkCreateSemaphore failed on image %u (%d)\n", i, semResult);
            return false;
        }

    }

    LOG("[vk] swapchain %ux%u, %u images (min %u), format %d, FIFO\n",
        sc.extent.width, sc.extent.height, actualCount, caps.minImageCount,
        static_cast<int>(surfaceFormat.format));
    return true;
}

// ---------------------------------------------------------------------------
// Window-level operations. They are here rather than in Window.cpp because none of
// them means anything without a swapchain.
// ---------------------------------------------------------------------------

// Guarantees somewhere to draw, remaking it when out of date or absent.
// **false is not a failure**; it means there is nowhere to draw right now (minimized).
//
// Deciding whether to recreate, handing over oldSwapchain and letting go of the
// previous one are one piece of work, so they are one function.
bool EnsureSwapchain(const VulkanDevice& dev, Window* window) noexcept {
    if (!window->swapchainOutOfDate && window->swapchain != nullptr) {
        return true;
    }

    // **The format is not asked again.** window->surfaceFormat is settled once at
    // startup and fed back in here, the way Unreal's FVulkanViewport carries
    // PixelFormat into RecreateSwapchainFromRT.
    //
    // Re-querying makes "this can change at any time" the premise, and then something
    // has to watch for it in the rendering path.
    //
    // Being wrong about that is not silent: calling vkCreateSwapchainKHR with an
    // unsupported format is a VUID violation and the validation layer reports it. A
    // real change (HDR) would be a request rather than a detection.

    // The old handle goes in as oldSwapchain, which retires it, and is released after
    // the new one exists. Per spec the retirement happens even if creation fails, so
    // the old one has to be let go either way.
    const VkSwapchainKHR retiring =
        window->swapchain != nullptr ? window->swapchain->handle : VK_NULL_HANDLE;

    // The extent is not asked again either. QuerySurfaceExtent runs at the top of the
    // frame, so what a swapchain is made at is the size that was already read.
    auto fresh = std::make_unique<Swapchain>();
    const bool created = CreateSwapchain(*window->inst, dev, window->surface,
                                         window->surfaceFormat, window->surfaceExtent,
                                         retiring, fresh.get());

    // Released **after** the new one is made, and not destroyed here: present may
    // still be reading the old images, so it waits in retired until the count runs out.
    //
    // The count is the old swapchain's image count -- once that many more presents
    // have happened, everything it left on screen has been replaced. It goes to
    // retired even when creation failed, because the spec retired it regardless.
    if (window->swapchain != nullptr) {
        const uint32_t frames =
            static_cast<uint32_t>(window->swapchain->images.size()) + 1;
        window->retired.push_back(RetiredSwapchain{std::move(window->swapchain), frames});
    }

    // On failure fresh is destroyed here, half-built or not, which is why
    // CreateSwapchain's failure paths contain no unwinding of their own.
    if (created) {
        window->swapchain = std::move(fresh);
    }
    window->swapchainOutOfDate = false;

    return window->swapchain != nullptr;
}

void AdvanceRetiredSwapchains(Window* window) noexcept {
    // Walked backwards: erasing from the front shifts what is left and the index
    // would skip an entry.
    for (size_t i = window->retired.size(); i > 0; --i) {
        RetiredSwapchain& item = window->retired[i - 1];
        if (item.framesLeft > 0) {
            --item.framesLeft;
            continue;
        }
        window->retired.erase(window->retired.begin() + static_cast<ptrdiff_t>(i - 1));
    }
}