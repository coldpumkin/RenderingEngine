#include "Vulkan/Device.h"

#include <cstring>
#include <string>
#include <vector>

// ============================================================================
bool SelectQueueFamilies(const VulkanInstance& inst,
                         VkPhysicalDevice gpu,
                         QueueFamilies* out) noexcept {
    *out = QueueFamilies{};

    uint32_t count = 0;
    inst.table.vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    inst.table.vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());

    // (1) graphics, and present with it. **The question goes to the platform, not to
    //     a surface**: vkGetPhysicalDeviceWin32PresentationSupportKHR answers whether
    //     this queue family can present to the Win32 desktop at all, and needs no
    //     window. It returns VkBool32 rather than VkResult, which says it is a
    //     property and not a query that can fail.
    //
    //     (The function exists only for Win32, Wayland, Xcb and Xlib. Android, iOS and
    //      macOS have none, which is why Unreal checks after a surface exists.)
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) { continue; }
        if (inst.table.vkGetPhysicalDeviceWin32PresentationSupportKHR(gpu, i) != VK_TRUE) {
            continue;
        }
        out->graphics = i;
        break;
    }
    if (out->graphics == UINT32_MAX) { return false; }

    // (2) dedicated compute: COMPUTE set, GRAPHICS clear.
    //     That one condition also guarantees it is not the graphics family.
    for (uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) == 0) { continue; }
        if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0) { continue; }
        out->compute = i;
        break;
    }

    // (3) dedicated transfer: TRANSFER set, neither GRAPHICS nor COMPUTE.
    //     **The only place the TRANSFER bit is read**, because the question is not
    //     "can this transfer" -- everything can -- but "is this an engine that does
    //     nothing else".
    for (uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_TRANSFER_BIT) == 0) { continue; }
        if ((flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0) { continue; }
        out->transfer = i;
        break;
    }

    return true;
}

// The result of choosing. **A value with no lifetime.**
//
// There is no vkDestroyPhysicalDevice: a GPU is enumerated and picked, not created,
// so there is nothing to hand back. That is why this stays a struct.
//
// It is in transit. PickPhysicalDevice produces it, CreateDevice consumes it and
// **absorbs it into VulkanDevice**; nobody holds one afterwards.
PhysicalDeviceSelection PickPhysicalDevice(const VulkanInstance& inst,
                                           VkSurfaceKHR surface) noexcept {
    PhysicalDeviceSelection selection;

    uint32_t gpuCount = 0;
    inst.table.vkEnumeratePhysicalDevices(inst.handle, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOG("[vk] no Vulkan-capable GPU\n");
        return selection;
    }
    // **A failed count leaves gpuCount at 0 and is caught above. A failed fill is
    // different**: the vector stays full of VK_NULL_HANDLE, and the loop below would
    // hand those to queries as though they were GPUs.
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    if (inst.table.vkEnumeratePhysicalDevices(inst.handle, &gpuCount, gpus.data())
            != VK_SUCCESS) {
        LOG("[vk] vkEnumeratePhysicalDevices failed\n");
        return selection;
    }

    VkPhysicalDeviceProperties chosenProps{};
    int bestScore = -1;

    for (VkPhysicalDevice candidate : gpus) {
        VkPhysicalDeviceProperties props{};
        inst.table.vkGetPhysicalDeviceProperties(candidate, &props);

        // (a) API version
        if (props.apiVersion < kRequiredApiVersion) { continue; }

        // (b) whether the 1.3 features are actually available -- supporting the
        //     version does not mean they are
        VkPhysicalDeviceVulkan13Features features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &features13;
        inst.table.vkGetPhysicalDeviceFeatures2(candidate, &features2);
        // (b1) core 1.0. Read out of the same function CreateDevice enables from, so
        //      the check and the enable cannot drift apart.
        const VkPhysicalDeviceFeatures required10 = RequiredFeatures10();
        if (required10.fillModeNonSolid == VK_TRUE
                && features2.features.fillModeNonSolid != VK_TRUE) {
            continue;
        }
        if (required10.samplerAnisotropy == VK_TRUE
                && features2.features.samplerAnisotropy != VK_TRUE) {
            continue;
        }
        if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE) {
            continue;
        }
        // Every core 1.0 feature required is checked above by name. Adding one means
        // editing here and Core.h both.

        // (c) the swapchain extension
        uint32_t extCount = 0;
        inst.table.vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> available(extCount);
        // A failed fill leaves available zeroed, which reads below as "no extensions"
        // and drops the candidate. That is the safe direction, and said out loud so it
        // is a decision rather than an accident.
        if (inst.table.vkEnumerateDeviceExtensionProperties(
                candidate, nullptr, &extCount, available.data()) != VK_SUCCESS) {
            LOG("[vk] vkEnumerateDeviceExtensionProperties failed; skipping this GPU\n");
            continue;
        }

        bool hasAllExtensions = true;
        for (const char* required : kRequiredDeviceExtensions) {
            bool found = false;
            for (const VkExtensionProperties& ext : available) {
                if (std::strcmp(ext.extensionName, required) == 0) { found = true; break; }
            }
            if (!found) { hasAllExtensions = false; break; }
        }
        if (!hasAllExtensions) { continue; }

        // (d) queue families. No graphics-with-present is disqualifying; compute and
        //     transfer are taken if dedicated ones exist and skipped otherwise.
        QueueFamilies families;
        if (!SelectQueueFamilies(inst, candidate, &families)) { continue; }

        const int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 1000 : 0;
        if (score > bestScore) {
            bestScore = score;
            selection.gpu = candidate;
            selection.families = families;
            chosenProps = props;
        }
    }

    if (selection.gpu == VK_NULL_HANDLE) {
        LOG("[vk] no GPU meets requirements (1.3 + dynamicRendering + sync2 + swapchain)\n");
        return selection;
    }

    // Whether the chosen family can present to **this surface**. The Win32 check
    // above was about the platform; this is about the window. Different levels, and
    // neither answers for the other.
    VkBool32 surfaceSupported = VK_FALSE;
    inst.table.vkGetPhysicalDeviceSurfaceSupportKHR(selection.gpu, selection.families.graphics,
                                                    surface, &surfaceSupported);
    if (surfaceSupported != VK_TRUE) {
        selection.gpu = VK_NULL_HANDLE;   // an empty gpu is how failure is spelled
        LOG("[vk] chosen queue family cannot present to this surface\n");
        return selection;
    }

    LOG("[vk] GPU: %s\n", chosenProps.deviceName);
    LOG("[vk] queue families: graphics=%u, compute=%s, transfer=%s\n",
        selection.families.graphics,
        selection.families.HasCompute()  ? std::to_string(selection.families.compute).c_str()  : "(none, use graphics)",
        selection.families.HasTransfer() ? std::to_string(selection.families.transfer).c_str() : "(none, use graphics)");
    return selection;
}

// ============================================================================
bool CreateDevice(const VulkanInstance& inst,
                  const PhysicalDeviceSelection& selection,
                  VulkanDevice* out) noexcept {
    VulkanDevice& dev = *out;
    dev.inst = &inst;
    dev.gpu = selection.gpu;
    dev.families = selection.families;
    const QueueFamilies& families = dev.families;

    // The spec requires the queueFamilyIndex values in pQueueCreateInfos to be
    // **distinct**. SelectQueueFamilies took compute only where GRAPHICS was clear and
    // transfer only where both were, so the three cannot collide and nothing here has
    // to deduplicate.
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfos[3]{};
    uint32_t queueInfoCount = 0;

    auto addQueue = [&](uint32_t family) {
        VkDeviceQueueCreateInfo& q = queueInfos[queueInfoCount++];
        q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q.queueFamilyIndex = family;
        q.queueCount = 1;              // one per family is all anything here submits to
        q.pQueuePriorities = &priority;
    };

    addQueue(families.graphics);
    if (families.HasCompute())  { addQueue(families.compute); }
    if (families.HasTransfer()) { addQueue(families.transfer); }

    // Checking support is not enough -- a feature has to be **asked for** to be usable.
    //
    // Two structures because the features are from two versions. VkPhysicalDeviceFeatures2
    // carries the core 1.0 set and chains the 1.3 one behind it, which is also how they
    // were queried above.
    VkPhysicalDeviceVulkan13Features enable13 = RequiredFeatures13();
    VkPhysicalDeviceFeatures2 enable2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enable2.pNext = &enable13;
    enable2.features = RequiredFeatures10();

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &enable2;
    // pEnabledFeatures stays null, and has to: the spec allows either that field or a
    // VkPhysicalDeviceFeatures2 in pNext, never both.
    info.queueCreateInfoCount = queueInfoCount;
    info.pQueueCreateInfos = queueInfos;
    info.enabledExtensionCount = static_cast<uint32_t>(std::size(kRequiredDeviceExtensions));
    info.ppEnabledExtensionNames = kRequiredDeviceExtensions;

    // **Through inst.table**, because vkCreateDevice is an instance-level call.
    // Calling the global works -- volkLoadInstanceOnly filled it in -- and that is
    // exactly what makes the convention worth stating.
    if (inst.table.vkCreateDevice(dev.gpu, &info, nullptr, &dev.handle) != VK_SUCCESS) {
        LOG("[vk] vkCreateDevice failed\n");
        return false;
    }

    // **A table, not the globals.** volkLoadDevice overwrites the globals with
    // whichever device was loaded last, so a second device means calls silently going
    // to the wrong one. There is one device today; the table is what makes that bug
    // impossible to write rather than merely absent.
    volkLoadDeviceTable(&dev.table, dev.handle);

    // vkGetDeviceQueue is a **query**: vkCreateDevice already made these, and nothing
    // destroys one.
    dev.table.vkGetDeviceQueue(dev.handle, families.graphics, 0, &dev.queues.graphics);
    if (families.HasCompute()) {
        dev.table.vkGetDeviceQueue(dev.handle, families.compute, 0, &dev.queues.compute);
    }
    if (families.HasTransfer()) {
        dev.table.vkGetDeviceQueue(dev.handle, families.transfer, 0, &dev.queues.transfer);
    }

    // present is an alias, not another queue. That the graphics family can present was
    // already established in PickPhysicalDevice.
    dev.queues.present = dev.queues.graphics;

    // The memory types, asked once and kept. The query is instance level.
    inst.table.vkGetPhysicalDeviceMemoryProperties(dev.gpu, &dev.memoryProperties);

    // ---- the VMA allocator ----
    //
    // **The function pointers are filled in by hand**, because of volk: under
    // VK_NO_PROTOTYPES there are no global vk* symbols for VMA to find. It also hands
    // VMA **the same table** everything else here calls through -- loading its own
    // would drift apart the moment there are two devices.
    //
    // This is the whole cost of volk plus VMA, and it is paid once.
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    functions.vkGetPhysicalDeviceProperties = inst.table.vkGetPhysicalDeviceProperties;
    functions.vkGetPhysicalDeviceMemoryProperties = inst.table.vkGetPhysicalDeviceMemoryProperties;
    functions.vkAllocateMemory = dev.table.vkAllocateMemory;
    functions.vkFreeMemory = dev.table.vkFreeMemory;
    functions.vkMapMemory = dev.table.vkMapMemory;
    functions.vkUnmapMemory = dev.table.vkUnmapMemory;
    functions.vkFlushMappedMemoryRanges = dev.table.vkFlushMappedMemoryRanges;
    functions.vkInvalidateMappedMemoryRanges = dev.table.vkInvalidateMappedMemoryRanges;
    functions.vkBindBufferMemory = dev.table.vkBindBufferMemory;
    functions.vkBindImageMemory = dev.table.vkBindImageMemory;
    functions.vkGetBufferMemoryRequirements = dev.table.vkGetBufferMemoryRequirements;
    functions.vkGetImageMemoryRequirements = dev.table.vkGetImageMemoryRequirements;
    functions.vkCreateBuffer = dev.table.vkCreateBuffer;
    functions.vkDestroyBuffer = dev.table.vkDestroyBuffer;
    functions.vkCreateImage = dev.table.vkCreateImage;
    functions.vkDestroyImage = dev.table.vkDestroyImage;
    functions.vkCmdCopyBuffer = dev.table.vkCmdCopyBuffer;
    // Promoted to core in 1.1. VMA takes a better path when they are present.
    functions.vkGetBufferMemoryRequirements2KHR = dev.table.vkGetBufferMemoryRequirements2;
    functions.vkGetImageMemoryRequirements2KHR = dev.table.vkGetImageMemoryRequirements2;
    functions.vkBindBufferMemory2KHR = dev.table.vkBindBufferMemory2;
    functions.vkBindImageMemory2KHR = dev.table.vkBindImageMemory2;
    functions.vkGetPhysicalDeviceMemoryProperties2KHR =
        inst.table.vkGetPhysicalDeviceMemoryProperties2;
    // Core in 1.3: maintenance4's way of asking a buffer's requirements without
    // creating one.
    functions.vkGetDeviceBufferMemoryRequirements = dev.table.vkGetDeviceBufferMemoryRequirements;
    functions.vkGetDeviceImageMemoryRequirements = dev.table.vkGetDeviceImageMemoryRequirements;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.vulkanApiVersion = kRequiredApiVersion;
    allocatorInfo.physicalDevice = dev.gpu;
    allocatorInfo.device = dev.handle;
    allocatorInfo.instance = inst.handle;
    allocatorInfo.pVulkanFunctions = &functions;

    if (vmaCreateAllocator(&allocatorInfo, &dev.allocator) != VK_SUCCESS) {
        LOG("[vk] vmaCreateAllocator failed\n");
        return false;
    }

    return true;
}

// Like the instance: it destroys itself with its own table.
// **Waits for the GPU first** -- the spec requires every queue on this device to be
// idle before vkDestroyDevice.
VulkanDevice::~VulkanDevice() {
    if (handle == VK_NULL_HANDLE) { return; }
    table.vkDeviceWaitIdle(handle);

    // **Before the device.** The allocator holds memory taken from it. The buffers go
    // even earlier -- main declares them after dev, so they are destroyed first.
    if (allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator);
    }
    table.vkDestroyDevice(handle, nullptr);
}