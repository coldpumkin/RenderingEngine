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

    // (1) 그래픽스 + present. **서피스가 아니라 플랫폼에 묻는다** -
    //     vkGetPhysicalDeviceWin32PresentationSupportKHR은 "이 큐 패밀리가 Win32
    //     데스크톱에 present 할 수 있는가"를 답하고, 창이 없어도 부를 수 있다.
    //     VkBool32를 돌려주는 것에 주목 - 실패할 수 있는 조회가 아니라 성질이다.
    //
    //     (이 함수는 Win32/Wayland/Xcb/Xlib에만 있다. Android/iOS/macOS엔 없어서
    //      언리얼은 "서피스가 생긴 뒤 확인"하는 2단계 초기화를 쓴다.)
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) { continue; }
        if (inst.table.vkGetPhysicalDeviceWin32PresentationSupportKHR(gpu, i) != VK_TRUE) {
            continue;
        }
        out->graphics = i;
        break;
    }
    if (out->graphics == UINT32_MAX) { return false; }

    // (2) 전용 컴퓨트: COMPUTE는 있고 GRAPHICS는 없는 패밀리.
    //     GRAPHICS가 없다는 조건 하나로 "그래픽스 패밀리와 다르다"가 자동 보장된다.
    for (uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) == 0) { continue; }
        if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0) { continue; }
        out->compute = i;
        break;
    }

    // (3) 전용 전송: TRANSFER는 있고 GRAPHICS도 COMPUTE도 없는 패밀리.
    //     **여기서만 TRANSFER 비트를 본다** - "전송을 할 수 있나"가 아니라
    //     "전송만 하는 전용 엔진인가"를 묻는 것이라서다.
    for (uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_TRANSFER_BIT) == 0) { continue; }
        if ((flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0) { continue; }
        out->transfer = i;
        break;
    }

    return true;
}

// 고르기의 결과. **수명이 없는 값 타입이다.**
//
// vkDestroyPhysicalDevice 같은 함수는 존재하지 않는다 - GPU는 우리가 만든 게 아니라
// 열거해서 고른 것이라 반납할 게 없다. 그래서 이건 영원히 struct고 클래스가 될 일이 없다.
//
// 지나가는 값이다: PickPhysicalDevice가 만들고, CreateDevice가 소비해서
// **VulkanDevice 안으로 흡수된다.** 그 뒤로 따로 들고 있지 않는다.
PhysicalDeviceSelection PickPhysicalDevice(const VulkanInstance& inst,
                                           VkSurfaceKHR surface) noexcept {
    PhysicalDeviceSelection selection;

    uint32_t gpuCount = 0;
    inst.table.vkEnumeratePhysicalDevices(inst.handle, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOG("[vk] no Vulkan-capable GPU\n");
        return selection;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    inst.table.vkEnumeratePhysicalDevices(inst.handle, &gpuCount, gpus.data());

    VkPhysicalDeviceProperties chosenProps{};
    int bestScore = -1;

    for (VkPhysicalDevice candidate : gpus) {
        VkPhysicalDeviceProperties props{};
        inst.table.vkGetPhysicalDeviceProperties(candidate, &props);

        // (a) API 버전
        if (props.apiVersion < kRequiredApiVersion) { continue; }

        // (b) 1.3 기능이 실제로 켜져 있는가 (버전을 지원해도 꺼져 있을 수 있다)
        VkPhysicalDeviceVulkan13Features features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &features13;
        inst.table.vkGetPhysicalDeviceFeatures2(candidate, &features2);
        if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE) {
            continue;
        }

        // (c) 스왑체인 확장
        uint32_t extCount = 0;
        inst.table.vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> available(extCount);
        inst.table.vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, available.data());

        bool hasAllExtensions = true;
        for (const char* required : kRequiredDeviceExtensions) {
            bool found = false;
            for (const VkExtensionProperties& ext : available) {
                if (std::strcmp(ext.extensionName, required) == 0) { found = true; break; }
            }
            if (!found) { hasAllExtensions = false; break; }
        }
        if (!hasAllExtensions) { continue; }

        // (d) 큐 패밀리 - 그래픽스+present가 없으면 탈락. 컴퓨트/전송은 있으면 좋고 없어도 된다.
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

    // 고른 큐 패밀리가 **이 서피스**에도 present 되는지 확인한다.
    // 위의 Win32 확인은 "플랫폼에 대해"이고 이건 "이 창에 대해"다.
    // 층이 다르고 서로를 대신하지 않는다.
    VkBool32 surfaceSupported = VK_FALSE;
    inst.table.vkGetPhysicalDeviceSurfaceSupportKHR(selection.gpu, selection.families.graphics,
                                                    surface, &surfaceSupported);
    if (surfaceSupported != VK_TRUE) {
        selection.gpu = VK_NULL_HANDLE;   // 실패는 gpu가 비어 있는 것으로 표현한다
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
    dev.gpu = selection.gpu;
    dev.families = selection.families;
    const QueueFamilies& families = dev.families;

    // 스펙: pQueueCreateInfos 안의 queueFamilyIndex는 **서로 달라야 한다.**
    // SelectQueueFamilies가 "GRAPHICS 없는 것만 compute", "GRAPHICS/COMPUTE 없는 것만
    // transfer"로 골랐으므로 셋은 자동으로 서로 다르다. 중복 제거가 필요 없다.
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfos[3]{};
    uint32_t queueInfoCount = 0;

    auto addQueue = [&](uint32_t family) {
        VkDeviceQueueCreateInfo& q = queueInfos[queueInfoCount++];
        q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q.queueFamilyIndex = family;
        q.queueCount = 1;              // 패밀리당 하나면 지금은 충분하다
        q.pQueuePriorities = &priority;
    };

    addQueue(families.graphics);
    if (families.HasCompute())  { addQueue(families.compute); }
    if (families.HasTransfer()) { addQueue(families.transfer); }

    // 지원 여부를 확인만 하는 게 아니라 **켜달라고 요청**해야 쓸 수 있다.
    VkPhysicalDeviceVulkan13Features enable13 = RequiredFeatures13();

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &enable13;
    info.queueCreateInfoCount = queueInfoCount;
    info.pQueueCreateInfos = queueInfos;
    info.enabledExtensionCount = static_cast<uint32_t>(std::size(kRequiredDeviceExtensions));
    info.ppEnabledExtensionNames = kRequiredDeviceExtensions;

    // **inst.table을 거친다.** 여기가 한동안 전역 vkCreateDevice를 부르고 있었다 -
    // 동작은 했지만(volkLoadInstanceOnly가 전역을 채워둬서) 테이블 규약 위반이었다.
    if (inst.table.vkCreateDevice(dev.gpu, &info, nullptr, &dev.handle) != VK_SUCCESS) {
        LOG("[vk] vkCreateDevice failed\n");
        return false;
    }

    // **전역이 아니라 테이블로 받는다.** 전역(volkLoadDevice)은 마지막으로 로드한
    // 디바이스로 덮인다. 디바이스가 둘이 되는 순간 조용히 틀린 디바이스를 부르게 되고,
    // 조용해서 안 잡힌다. 지금 디바이스는 하나지만 **테이블을 쓰면 그 버그가 아예
    // 표현 불가능해진다.**
    volkLoadDeviceTable(&dev.table, dev.handle);

    // vkGetDeviceQueue는 **조회**다. vkCreateDevice가 이미 만들었고, 파괴 함수도 없다.
    dev.table.vkGetDeviceQueue(dev.handle, families.graphics, 0, &dev.queues.graphics);
    if (families.HasCompute()) {
        dev.table.vkGetDeviceQueue(dev.handle, families.compute, 0, &dev.queues.compute);
    }
    if (families.HasTransfer()) {
        dev.table.vkGetDeviceQueue(dev.handle, families.transfer, 0, &dev.queues.transfer);
    }

    // present는 새로 만드는 게 아니라 위에서 만든 것 중 하나를 가리킨다.
    // 그래픽스 패밀리가 present를 지원하는 것은 PickPhysicalDevice가 이미 확인했다.
    dev.queues.present = dev.queues.graphics;

    // 메모리 타입 목록을 여기서 한 번 물어 담는다 (인스턴스 레벨 조회다).
    inst.table.vkGetPhysicalDeviceMemoryProperties(dev.gpu, &dev.memoryProperties);

    // ---- VMA 할당자 ----
    //
    // **함수 포인터를 손으로 채운다.** volk 때문이다: VK_NO_PROTOTYPES라 전역 vk* 심볼이
    // 아예 없어서 VMA가 스스로 찾을 수가 없다. 우리가 쓰는 것과 **같은 테이블**을
    // 넘기는 것이 목적이기도 하다 - VMA가 따로 로드하면 멀티 디바이스에서 어긋난다.
    //
    // 이게 volk + VMA 조합의 유일한 대가다. 한 번 적으면 끝이다.
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
    // 1.1+ 코어로 올라온 것들. VMA가 있으면 더 나은 경로를 쓴다.
    functions.vkGetBufferMemoryRequirements2KHR = dev.table.vkGetBufferMemoryRequirements2;
    functions.vkGetImageMemoryRequirements2KHR = dev.table.vkGetImageMemoryRequirements2;
    functions.vkBindBufferMemory2KHR = dev.table.vkBindBufferMemory2;
    functions.vkBindImageMemory2KHR = dev.table.vkBindImageMemory2;
    functions.vkGetPhysicalDeviceMemoryProperties2KHR =
        inst.table.vkGetPhysicalDeviceMemoryProperties2;
    // 1.3 코어. maintenance4의 "버퍼를 안 만들고도 요구사항을 묻는" 경로.
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

// 인스턴스와 같다 - 자기 테이블로 자기를 지운다.
// **파괴 전에 GPU를 기다린다**: 스펙상 vkDestroyDevice 전에 이 디바이스의 모든 큐
// 작업이 끝나 있어야 한다.
VulkanDevice::~VulkanDevice() {
    if (handle == VK_NULL_HANDLE) { return; }
    table.vkDeviceWaitIdle(handle);

    // **디바이스보다 먼저.** 할당자가 이 디바이스로 잡은 메모리를 들고 있다.
    // (버퍼들은 이보다도 먼저 죽는다 - main()에서 dev보다 뒤에 선언했다.)
    if (allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator);
    }
    table.vkDestroyDevice(handle, nullptr);
}