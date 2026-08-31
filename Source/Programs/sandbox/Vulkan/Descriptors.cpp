#include "Vulkan/Descriptors.h"

// 각 layout이 요구하는 binding 개수. 출처는 shader이고 여기는 받아적는 쪽이다.
//
//   scene    triangle.frag   sampler2D tex(0) · detail(1)
//   present  fullscreen.frag sampler2D sceneColor(0)
//
// Contract: shader의 sampler2D 선언 개수와 같아야 한다. 어느 컴파일러도 양쪽을
//           같이 안 보고, 어긋나면 validation layer가 draw에서 잡는다.
constexpr uint32_t kSceneBindingCount = 2;
constexpr uint32_t kPresentBindingCount = 1;

// 두 layout이 같은 모양의 binding을 쓴다 - 개수만 다르다.
// Effect: bindingCount개를 0번부터 채운 layout을 만든다
static bool CreateImageSetLayout(const VulkanDevice& dev, uint32_t bindingCount,
                                 VkDescriptorSetLayout* out) noexcept {
    // COMBINED_IMAGE_SAMPLER: image와 sampler를 한 자리에 묶는다(GLSL의 sampler2D).
    VkDescriptorSetLayoutBinding bindings[kSceneBindingCount]{};
    for (uint32_t i = 0; i < bindingCount; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;   // 읽는 곳이 fragment뿐이다
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = bindingCount;
    layoutInfo.pBindings = bindings;
    if (dev.table.vkCreateDescriptorSetLayout(dev.handle, &layoutInfo, nullptr, out)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateDescriptorSetLayout failed (%u bindings)\n", bindingCount);
        return false;
    }
    return true;
}

bool CreateDescriptors(const VulkanDevice& dev,
                       uint32_t sceneSets, uint32_t presentSets,
                       Descriptors* out) noexcept {
    out->dev = &dev;

    // Sampler는 image가 아니라 읽는 규칙이다. 그래서 image와 따로 살고 하나로
    // 여러 image를 읽는다. 두 layout이 이것 하나를 같이 쓴다.
    //
    // LINEAR: 렌더 해상도와 창 크기가 다를 수 있어 확대·축소가 일어난다
    // CLAMP_TO_EDGE: 0~1 밖은 가장자리 색. REPEAT면 반대편이 말려 들어온다
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;   // mipmap이 없다
    if (dev.table.vkCreateSampler(dev.handle, &samplerInfo, nullptr, &out->sampler)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateSampler failed\n");
        return false;
    }

    if (!CreateImageSetLayout(dev, kSceneBindingCount, &out->sceneLayout)) { return false; }
    if (!CreateImageSetLayout(dev, kPresentBindingCount, &out->presentLayout)) { return false; }

    // Pool은 자라지 않아서 크기를 미리 정한다. 타입별 개수도 같이 말해야 한다.
    //
    // 두 값이 다른 것을 센다 - set의 개수와 descriptor의 개수다. layout마다 binding
    // 수가 달라서 뒤는 가중합이고, 앞의 배수가 아니다.
    const uint32_t maxSets = sceneSets + presentSets;

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = sceneSets * kSceneBindingCount
                             + presentSets * kPresentBindingCount;

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = 1;   // 타입이 한 종류다. UNIFORM_BUFFER가 오면 둘이 된다
    poolInfo.pPoolSizes = &poolSize;
    // FREE_DESCRIPTOR_SET을 안 주면 set을 개별 반납할 수 없다. 시작할 때 뽑아서
    // 끝까지 쓰므로 반납할 일이 없고, 안 주는 쪽이 driver에게 쉽다.
    if (dev.table.vkCreateDescriptorPool(dev.handle, &poolInfo, nullptr, &out->pool)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateDescriptorPool failed\n");
        return false;
    }
    return true;
}

// 두 Allocate의 공통부. 실제로 다른 것은 **layout과 view 개수**뿐이다.
//
// Contract: viewCount는 layout이 요구하는 binding 개수와 같아야 한다. 모자라면
//           안 채운 자리를 shader가 읽다가 draw에서 잡힌다.
static VkDescriptorSet AllocateImageSet(const Descriptors& descriptors,
                                        VkDescriptorSetLayout layout,
                                        const VkImageView* views, uint32_t viewCount) noexcept {
    const VulkanDevice& dev = *descriptors.dev;

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = descriptors.pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (dev.table.vkAllocateDescriptorSets(dev.handle, &allocInfo, &set) != VK_SUCCESS) {
        LOG("[vk] vkAllocateDescriptorSets failed (pool may be too small)\n");
        return VK_NULL_HANDLE;
    }

    // 뽑은 set은 비어 있어서 binding마다 "이 view + 이 sampler"를 채운다.
    //
    // imageLayout은 bind 시점이 아니라 읽는 시점의 layout이다. Texture 업로드와
    // RecordPresentPass가 그 전에 SHADER_READ_ONLY_OPTIMAL로 전이시키는 것과 짝이다.
    VkDescriptorImageInfo imageInfo[kSceneBindingCount]{};
    VkWriteDescriptorSet write[kSceneBindingCount]{};
    for (uint32_t i = 0; i < viewCount; ++i) {
        imageInfo[i].sampler = descriptors.sampler;
        imageInfo[i].imageView = views[i];
        imageInfo[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        write[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write[i].dstSet = set;
        write[i].dstBinding = i;
        write[i].descriptorCount = 1;
        write[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write[i].pImageInfo = &imageInfo[i];
    }

    // 스펙: 이 함수는 실패하지 않는다. 잘못 채우면 validation layer가 잡는다.
    dev.table.vkUpdateDescriptorSets(dev.handle, viewCount, write, 0, nullptr);
    return set;
}

VkDescriptorSet AllocateSceneSet(const Descriptors& descriptors,
                                 VkImageView view0, VkImageView view1) noexcept {
    const VkImageView views[kSceneBindingCount] = {view0, view1};
    return AllocateImageSet(descriptors, descriptors.sceneLayout, views, kSceneBindingCount);
}

VkDescriptorSet AllocatePresentSet(const Descriptors& descriptors, VkImageView view) noexcept {
    const VkImageView views[kPresentBindingCount] = {view};
    return AllocateImageSet(descriptors, descriptors.presentLayout, views, kPresentBindingCount);
}

Descriptors::~Descriptors() {
    if (dev == nullptr) { return; }
    const VulkanDevice& d = *dev;
    // Pool을 지우면 거기서 뽑은 set도 같이 사라진다.
    d.table.vkDestroyDescriptorPool(d.handle, pool, nullptr);
    d.table.vkDestroyDescriptorSetLayout(d.handle, presentLayout, nullptr);
    d.table.vkDestroyDescriptorSetLayout(d.handle, sceneLayout, nullptr);
    d.table.vkDestroySampler(d.handle, sampler, nullptr);
}
