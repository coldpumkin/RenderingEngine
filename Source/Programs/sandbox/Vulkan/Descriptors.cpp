#include "Vulkan/Descriptors.h"

bool CreateDescriptors(const VulkanDevice& dev, uint32_t maxSets, Descriptors* out) noexcept {
    out->dev = &dev;

    // Sampler는 image가 아니라 읽는 규칙이다. 그래서 image와 따로 살고 하나로
    // 여러 image를 읽는다.
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

    // binding 0은 shader의 layout(set=0, binding=0)과 짝이다.
    // COMBINED_IMAGE_SAMPLER: image와 sampler를 한 자리에 묶는다(GLSL의 sampler2D).
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;   // 읽는 곳이 fragment뿐이다

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (dev.table.vkCreateDescriptorSetLayout(dev.handle, &layoutInfo, nullptr, &out->setLayout)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateDescriptorSetLayout failed\n");
        return false;
    }

    // Pool은 자라지 않아서 크기를 미리 정한다. 타입별 개수도 같이 말해야 한다.
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = maxSets;

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = 1;
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

VkDescriptorSet AllocateImageSet(const Descriptors& descriptors, VkImageView view) noexcept {
    const VulkanDevice& dev = *descriptors.dev;

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = descriptors.pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptors.setLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (dev.table.vkAllocateDescriptorSets(dev.handle, &allocInfo, &set) != VK_SUCCESS) {
        LOG("[vk] vkAllocateDescriptorSets failed (풀이 모자랄 수 있다)\n");
        return VK_NULL_HANDLE;
    }

    // 뽑은 set은 비어 있어서 "0번 자리 = 이 view + 이 sampler"를 채운다.
    //
    // imageLayout은 bind 시점이 아니라 읽는 시점의 layout이다. RecordPresentPass가
    // 그 직전에 SHADER_READ_ONLY_OPTIMAL로 전이시키는 것과 짝이다.
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = descriptors.sampler;
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;

    // 스펙: 이 함수는 실패하지 않는다. 잘못 채우면 validation layer가 잡는다.
    dev.table.vkUpdateDescriptorSets(dev.handle, 1, &write, 0, nullptr);
    return set;
}

Descriptors::~Descriptors() {
    if (dev == nullptr) { return; }
    const VulkanDevice& d = *dev;
    // Pool을 지우면 거기서 뽑은 set도 같이 사라진다.
    d.table.vkDestroyDescriptorPool(d.handle, pool, nullptr);
    d.table.vkDestroyDescriptorSetLayout(d.handle, setLayout, nullptr);
    d.table.vkDestroySampler(d.handle, sampler, nullptr);
}
