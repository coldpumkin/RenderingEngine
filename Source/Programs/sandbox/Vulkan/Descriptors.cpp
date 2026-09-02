#include "Vulkan/Descriptors.h"

#include "Vulkan/Shader.h"

#include <iterator>   // std::size

// Effect: builds a layout shaped exactly like the fragment shader's set 0, and
//         reports how many bindings that turned out to be.
//
// Every value here comes out of the SPIR-V. The type is the shader's (sampler2D
// becomes COMBINED_IMAGE_SAMPLER), and stageFlags is FRAGMENT because that is the
// only stage we reflect for descriptors -- a vertex shader reading a texture would
// need its own pass over that stage.
// 한 layout이 타입별로 descriptor를 몇 개 요구하는지.
static uint32_t CountOfType(const DescriptorLayout& layout, VkDescriptorType type) noexcept {
    uint32_t n = 0;
    for (uint32_t i = 0; i < layout.bindingCount; ++i) {
        if (layout.types[i] == type) { ++n; }
    }
    return n;
}

// One layout, count sets in one call. The ceiling is kFramesInFlight because that is
// what "a set per frame" means -- not a second number to keep in step with the first.
bool AllocateSets(const Descriptors& descriptors, const DescriptorLayout& layout,
                  uint32_t count, VkDescriptorSet* out) noexcept {
    if (count == 0) { return true; }
    if (count > kFramesInFlight) {
        LOG("[vk] %u sets asked for, %u is the ceiling\n", count, kFramesInFlight);
        return false;
    }

    VkDescriptorSetLayout layouts[kFramesInFlight]{};
    for (uint32_t i = 0; i < count; ++i) { layouts[i] = layout.handle; }

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = descriptors.pool;
    allocInfo.descriptorSetCount = count;
    allocInfo.pSetLayouts = layouts;

    const VulkanDevice& dev = *descriptors.dev;
    if (dev.table.vkAllocateDescriptorSets(dev.handle, &allocInfo, out) != VK_SUCCESS) {
        LOG("[vk] vkAllocateDescriptorSets failed (pool may be too small)\n");
        return false;
    }
    return true;
}

bool CreateDescriptors(const VulkanDevice& dev,
                       const SetRequest* requests, uint32_t requestCount,
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

    // Pool은 자라지 않아서 크기를 미리 정한다. 타입별 개수도 같이 말해야 한다.
    //
    // 두 값이 다른 것을 센다 - set의 개수와 descriptor의 개수다. layout마다 binding
    // 수가 달라서 뒤는 가중합이고, 앞의 배수가 아니다.
    uint32_t maxSets = 0;
    for (uint32_t r = 0; r < requestCount; ++r) { maxSets += requests[r].count; }

    // 타입마다 따로 센다. 요구가 0인 타입은 빼야 한다 - 스펙이 descriptorCount 0을
    // 금지한다.
    constexpr VkDescriptorType kTypes[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    VkDescriptorPoolSize poolSizes[std::size(kTypes)]{};
    uint32_t sizeCount = 0;
    for (const VkDescriptorType type : kTypes) {
        uint32_t n = 0;
        for (uint32_t r = 0; r < requestCount; ++r) {
            n += requests[r].count * CountOfType(*requests[r].layout, type);
        }
        if (n == 0) { continue; }
        poolSizes[sizeCount].type = type;
        poolSizes[sizeCount].descriptorCount = n;
        ++sizeCount;
    }

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = sizeCount;
    poolInfo.pPoolSizes = poolSizes;
    // FREE_DESCRIPTOR_SET을 안 주면 set을 개별 반납할 수 없다. 시작할 때 뽑아서
    // 끝까지 쓰므로 반납할 일이 없고, 안 주는 쪽이 driver에게 쉽다.
    if (dev.table.vkCreateDescriptorPool(dev.handle, &poolInfo, nullptr, &out->pool)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateDescriptorPool failed\n");
        return false;
    }

    // The sets are not drawn here. Each pass draws its own, because filling one needs
    // that pass's resources and those do not exist yet.
    return true;
}

void UpdateSet(const Descriptors& descriptors, const DescriptorLayout& layout,
               VkDescriptorSet set,
               const BindingValue* values, uint32_t count) noexcept {
    const VulkanDevice& dev = *descriptors.dev;
    if (count != layout.bindingCount || set == VK_NULL_HANDLE) {
        LOG("[vk] layout wants %u bindings, given %u\n", layout.bindingCount, count);
        return;
    }

    // 뽑은 set은 비어 있어서 binding마다 채운다. type이 어느 info를 쓸지 정한다.
    //
    // imageLayout은 bind 시점이 아니라 읽는 시점의 layout이다. Texture 업로드와
    // RecordPresentStage가 그 전에 SHADER_READ_ONLY_OPTIMAL로 전이시키는 것과 짝이다.
    VkDescriptorImageInfo imageInfo[kMaxBindingsPerSet]{};
    VkDescriptorBufferInfo bufferInfo[kMaxBindingsPerSet]{};
    VkWriteDescriptorSet write[kMaxBindingsPerSet]{};
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const VkDescriptorType type = layout.types[i];
        if (type == 0) { continue; }   // 번호에 구멍이 있는 경우

        write[used].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write[used].dstSet = set;
        write[used].dstBinding = i;
        write[used].descriptorCount = 1;
        write[used].descriptorType = type;

        if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            bufferInfo[used].buffer = values[i].buffer;
            bufferInfo[used].range = values[i].size;
            write[used].pBufferInfo = &bufferInfo[used];
        } else {
            imageInfo[used].sampler = descriptors.sampler;
            imageInfo[used].imageView = values[i].view;
            imageInfo[used].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            write[used].pImageInfo = &imageInfo[used];
        }
        ++used;
    }

    // 스펙: 이 함수는 실패하지 않는다. 잘못 채우면 validation layer가 잡는다.
    dev.table.vkUpdateDescriptorSets(dev.handle, used, write, 0, nullptr);
}

Descriptors::~Descriptors() {
    if (dev == nullptr) { return; }
    const VulkanDevice& d = *dev;
    // Destroying the pool takes the sets drawn from it with it. The layouts belong
    // to the pipelines and are not ours to free.
    d.table.vkDestroyDescriptorPool(d.handle, pool, nullptr);
    d.table.vkDestroySampler(d.handle, sampler, nullptr);
}
