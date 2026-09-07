#include "Vulkan/Descriptors.h"

#include "Vulkan/Shader.h"

#include <iterator>   // std::size
#include <vector>     // one layout handle per set asked for

// Effect: builds a layout shaped exactly like the fragment shader's set 0, and
//         reports how many bindings that turned out to be.
//
// Every value here comes out of the SPIR-V. The type is the shader's (sampler2D
// becomes COMBINED_IMAGE_SAMPLER), and stageFlags is FRAGMENT because that is the
// only stage we reflect for descriptors -- a vertex shader reading a texture would
// need its own pass over that stage.
// How many descriptors of one type a layout asks for.
static uint32_t CountOfType(const DescriptorLayout& layout, VkDescriptorType type) noexcept {
    uint32_t n = 0;
    for (uint32_t i = 0; i < layout.bindingCount; ++i) {
        if (layout.types[i] == type) { ++n; }
    }
    return n;
}

// One layout, count sets in one call.
//
// No ceiling of its own any more. It used to be kFramesInFlight, on the reading that
// a set is per frame -- but that is only true of one of our layouts. A material's is
// counted by materials, and the pool is the only limit both answer to. Overshooting
// fails at vkAllocateDescriptorSets, which names the pool; a number checked here
// would just be a second copy to keep in step.
//
// The array is heap for the same reason: no stack size fits both counts, and this
// runs at init where the heap is free.
bool AllocateSets(const Descriptors& descriptors, const DescriptorLayout& layout,
                  uint32_t count, VkDescriptorSet* out) noexcept {
    if (count == 0) { return true; }

    const std::vector<VkDescriptorSetLayout> layouts(count, layout.handle);

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = descriptors.pool;
    allocInfo.descriptorSetCount = count;
    allocInfo.pSetLayouts = layouts.data();

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

    // A sampler is not an image, it is the rule for reading one -- so it lives apart
    // from any image and one of them serves every layout here.
    //
    // LINEAR: the render resolution and the window size are independent, so something
    // is always being scaled.
    //
    // REPEAT, because **the asset was authored expecting it.** Sponza's uvs run
    // u -27.79..32.29 and v -4.95..7.58, and 45 of its 103 primitives leave 0..1 --
    // that is how one wall texture is tiled across a surface. Under CLAMP those 45
    // would stretch their edge pixel across the whole wall.
    //
    // The cost is paid by a texture atlas, where several pictures share one image and
    // the far side bleeds in. Sponza keeps one file per picture, so it does not.
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    // Between levels as well as within one. NEAREST here snaps from one level to the
    // next, and the seam moves across the floor as the camera does.
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    // Every level a texture has. 0 would clamp sampling to level 0 and undo the chain.
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

    // How many samples along the long axis of the footprint. A level is chosen from the
    // larger of the two screen derivatives, so a surface seen at a grazing angle gets a
    // level that is right for the long direction and too blurry for the short one;
    // taking several samples along that axis is what recovers it.
    //
    // The device's own ceiling, not a number we picked -- 16 is what desktop hardware
    // reports and asking for more is a validation error.
    VkPhysicalDeviceProperties props{};
    dev.inst->table.vkGetPhysicalDeviceProperties(dev.gpu, &props);
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = props.limits.maxSamplerAnisotropy;
    if (dev.table.vkCreateSampler(dev.handle, &samplerInfo, nullptr, &out->sampler)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateSampler failed\n");
        return false;
    }

    // A pool does not grow, so its size is settled here and the per-type counts with
    // it.
    //
    // The two numbers count different things -- how many sets, and how many
    // descriptors those sets hold. Layouts differ in binding count, so the second is a
    // weighted sum rather than a multiple of the first.
    uint32_t maxSets = 0;
    for (uint32_t r = 0; r < requestCount; ++r) { maxSets += requests[r].count; }

    // Counted per type, and a type nobody asked for is left out: the spec forbids a
    // pool size with descriptorCount 0.
    constexpr VkDescriptorType kTypes[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    VkDescriptorPoolSize poolSizes[std::size(kTypes)]{};
    uint32_t sizeCount = 0;
    for (const VkDescriptorType type : kTypes) {
        uint32_t n = 0;
        for (uint32_t r = 0; r < requestCount; ++r) {
            n += requests[r].count * CountOfType(*requests[r].layout, type);
        }
        if (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) { out->imageDescriptors = n; }
        else { out->bufferDescriptors = n; }

        if (n == 0) { continue; }
        poolSizes[sizeCount].type = type;
        poolSizes[sizeCount].descriptorCount = n;
        ++sizeCount;
    }
    out->maxSets = maxSets;

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = sizeCount;
    poolInfo.pPoolSizes = poolSizes;
    // No FREE_DESCRIPTOR_SET, so a set cannot be returned on its own. Every set here
    // is drawn at startup and held until the pool goes, so there is nothing to return
    // -- and without the flag the driver can allocate more simply.
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
    // Fewer than the layout wants is a hole nobody filled. More is the normal case for
    // a shared set: the caller states the whole of it and this program declared only
    // the front of it, exactly as a vertex stage reads two of four attributes. The
    // extra values are not looked at.
    if (count < layout.bindingCount || set == VK_NULL_HANDLE) {
        LOG("[vk] layout wants %u bindings, given %u\n", layout.bindingCount, count);
        return;
    }

    // An allocated set points at nothing, so each binding is filled here. The type
    // decides which of the two infos is read.
    //
    // imageLayout is the layout at read time, not at bind time. Its other half is the
    // barrier that puts the image there before the draw -- the texture upload does it
    // for materials, RecordShadowPass and RecordPostProcessPass for what they wrote.
    VkDescriptorImageInfo imageInfo[kMaxBindingsPerSet]{};
    VkDescriptorBufferInfo bufferInfo[kMaxBindingsPerSet]{};
    VkWriteDescriptorSet write[kMaxBindingsPerSet]{};
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const VkDescriptorType type = layout.types[i];
        if (type == 0) { continue; }   // a hole in the binding numbering

        write[used].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write[used].dstSet = set;
        write[used].dstBinding = i;
        write[used].descriptorCount = 1;
        write[used].descriptorType = type;

        if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            if (values[i].buffer == nullptr) {
                LOG("[vk] binding %u wants a buffer and was given none\n", i);
                return;
            }
            bufferInfo[used].buffer = values[i].buffer->handle;
            bufferInfo[used].range = values[i].buffer->size;
            write[used].pBufferInfo = &bufferInfo[used];
        } else {
            if (values[i].view == nullptr) {
                LOG("[vk] binding %u wants an image and was given none\n", i);
                return;
            }
            // What the stage declared it would read, against what is being written
            // in. The requirement came out of the .spv, so nothing here is a second
            // statement of it -- a caller asserting these again would be copying the
            // shader by hand.
            //
            // The view answers all three: its own type is the shape, and the two
            // fields it keeps of its image answer the rest.
            const ImageRequirement& want = layout.images[i];
            const ImageView& got = *values[i].view;
            if (want.isImage) {
                if (got.desc.type != want.viewType) {
                    LOG("[vk] binding %u: the shader reads a view type %d image and was"
                        " given type %d\n", i, static_cast<int>(want.viewType),
                        static_cast<int>(got.desc.type));
                    return;
                }
                if ((got.imageSamples != VK_SAMPLE_COUNT_1_BIT) != want.multisample) {
                    LOG("[vk] binding %u: the shader reads a %s image and was given a"
                        " %d-sample one\n", i,
                        want.multisample ? "multisample" : "single-sample",
                        static_cast<int>(got.imageSamples));
                    return;
                }
                // A depth/stencil image reaches a shader through one aspect at a
                // time -- VUID-VkDescriptorImageInfo-imageView-01976. The rule is the
                // binding's, so it is asked here and not where the view was made:
                // that same view would be legal, and required, to carry both in a
                // barrier.
                const VkImageAspectFlags both = VK_IMAGE_ASPECT_DEPTH_BIT
                                              | VK_IMAGE_ASPECT_STENCIL_BIT;
                if ((got.aspect & both) == both) {
                    LOG("[vk] binding %u: a view exposing both depth and stencil cannot"
                        " be read by a shader\n", i);
                    return;
                }

                const VkImageUsageFlags needed = want.storage
                                               ? VK_IMAGE_USAGE_STORAGE_BIT
                                               : VK_IMAGE_USAGE_SAMPLED_BIT;
                if ((got.imageUsage & needed) == 0) {
                    LOG("[vk] binding %u: the shader reads it as a %s image and the one"
                        " given was not created for that\n", i,
                        want.storage ? "storage" : "sampled");
                    return;
                }
            }

            imageInfo[used].sampler = descriptors.sampler;
            imageInfo[used].imageView = values[i].view->handle;
            imageInfo[used].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            write[used].pImageInfo = &imageInfo[used];
        }
        ++used;
    }

    // The spec gives this no failure to report. A wrong fill is the validation
    // layer's to catch, not ours.
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
