#include "Sky.h"

#include "Vulkan/Barrier.h"

#include <iterator>   // std::size

TextureDesc MakeSkyTarget() noexcept {
    TextureDesc desc{};
    desc.extent = VkExtent2D{256, 256};
    desc.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    desc.samples = VK_SAMPLE_COUNT_1_BIT;
    desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    desc.kind = TextureKind::Cube;
    return desc;
}

TextureDesc MakeIrradianceTarget() noexcept {
    TextureDesc desc = MakeSkyTarget();
    desc.extent = VkExtent2D{32, 32};
    return desc;
}

// Effect: draws one triangle into every face of cube, with whatever set is bound
//
// The shape both bakes share. What differs is the program and whether a set is bound,
// and both of those are arguments rather than a second copy of this loop.
static bool BakeCubeFaces(const VulkanDevice& dev, VkCommandBuffer cmd,
                          const Pipeline& pipeline, VkDescriptorSet set,
                          Texture* cube) noexcept {
    const TextureDesc faceDesc = SliceDesc(cube->desc);

    RenderPassDesc desc{};
    desc.attachments[0].resource = &faceDesc;
    desc.attachments[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    desc.attachments[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    if (!ValidatePassDesc(desc)) { return false; }

    const VolkDeviceTable& vk = dev.table;
    const VkRect2D area{{0, 0}, cube->desc.extent};

    for (uint32_t i = 0; i < 6; ++i) {
        ImageViewDesc viewDesc;
        viewDesc.type = VK_IMAGE_VIEW_TYPE_2D;
        viewDesc.baseLayer = i;
        viewDesc.layerCount = 1;

        ImageView faceView;
        if (!CreateImageView(dev, cube->image.handle, cube->desc.format,
                             cube->desc.samples, cube->desc.usage, viewDesc, &faceView)) {
            return false;
        }

        const AttachmentView view{cube->image.handle, &faceView, faceDesc};
        if (!BeginPass(vk, cmd, desc, &view, nullptr, area,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
            return false;
        }

        BindPipeline(vk, cmd, pipeline, area);
        if (set != VK_NULL_HANDLE) {
            vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       pipeline.program->layout, kFrameSet,
                                       1, &set, 0, nullptr);
        }
        const SkyFace face{static_cast<int32_t>(i)};
        vk.vkCmdPushConstants(cmd, pipeline.program->layout,
                              VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(face), &face);
        vk.vkCmdDraw(cmd, 3, 1, 0, 0);
        vk.vkCmdEndRendering(cmd);

        // The view is destroyed when this iteration ends, after the commands that name
        // it are recorded and before they run -- which is legal: a view has to outlive
        // recording, not execution.
    }
    return true;
}

bool BakeIrradianceCube(const VulkanDevice& dev, const Commands& commands,
                        const Descriptors& descriptors, const Pipeline& pipeline,
                        const Texture& environment, Texture* irradiance) noexcept {
    if (pipeline.program == nullptr) { return false; }
    const ShaderProgram& program = *pipeline.program;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!AllocateSets(descriptors, program.setLayouts[kFrameSet], 1, &set)) {
        return false;
    }
    const BindingValue values[] = {{&environment.view}};
    UpdateSet(descriptors, program.setLayouts[kFrameSet], set,
              values, static_cast<uint32_t>(std::size(values)));

    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }
    if (!BakeCubeFaces(dev, cmd, pipeline, set, irradiance)) { return false; }
    RecordSampledHandover(dev.table, cmd, *irradiance, AttachmentRole::Color);
    if (!EndOneShotAndWait(dev, commands, cmd, "irradiance bake")) { return false; }

    LOG("[render] irradiance cube baked (%ux%u, 6 faces)\n",
        irradiance->desc.extent.width, irradiance->desc.extent.height);
    return true;
}

TextureDesc MakePrefilterTarget() noexcept {
    TextureDesc desc = MakeSkyTarget();
    desc.extent = VkExtent2D{128, 128};
    desc.mipLevels = kPrefilterMips;
    return desc;
}

TextureDesc MakeBrdfLutTarget() noexcept {
    TextureDesc desc{};
    desc.extent = VkExtent2D{256, 256};
    desc.format = VK_FORMAT_R16G16_SFLOAT;
    desc.samples = VK_SAMPLE_COUNT_1_BIT;
    desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return desc;
}

bool BakePrefilterCube(const VulkanDevice& dev, const Commands& commands,
                       const Descriptors& descriptors, const Pipeline& pipeline,
                       const Texture& environment, Texture* prefiltered) noexcept {
    if (pipeline.program == nullptr) { return false; }
    const ShaderProgram& program = *pipeline.program;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!AllocateSets(descriptors, program.setLayouts[kFrameSet], 1, &set)) {
        return false;
    }
    const BindingValue values[] = {{&environment.view}};
    UpdateSet(descriptors, program.setLayouts[kFrameSet], set,
              values, static_cast<uint32_t>(std::size(values)));

    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }
    const VolkDeviceTable& vk = dev.table;

    // A level's target is a 2D image of that level's size, which is what a pass drawing
    // into it declares -- not the cube, and not the cube's extent.
    TextureDesc levelDesc = SliceDesc(prefiltered->desc);

    RenderPassDesc desc{};
    desc.attachments[0].resource = &levelDesc;
    desc.attachments[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    desc.attachments[0].store = VK_ATTACHMENT_STORE_OP_STORE;

    for (uint32_t mip = 0; mip < prefiltered->desc.mipLevels; ++mip) {
        const uint32_t size = prefiltered->desc.extent.width >> mip;
        levelDesc.extent = VkExtent2D{size, size};
        if (!ValidatePassDesc(desc)) { return false; }

        // Level 0 is the environment itself and the last is the roughest, spread evenly
        // between -- which is the mapping a shader inverts to pick a level.
        const float roughness =
            static_cast<float>(mip) / static_cast<float>(prefiltered->desc.mipLevels - 1);
        const VkRect2D area{{0, 0}, levelDesc.extent};

        for (uint32_t layer = 0; layer < 6; ++layer) {
            ImageViewDesc viewDesc;
            viewDesc.type = VK_IMAGE_VIEW_TYPE_2D;
            viewDesc.baseMip = mip;
            viewDesc.mipCount = 1;
            viewDesc.baseLayer = layer;
            viewDesc.layerCount = 1;

            ImageView faceView;
            if (!CreateImageView(dev, prefiltered->image.handle, prefiltered->desc.format,
                                 prefiltered->desc.samples, prefiltered->desc.usage,
                                 viewDesc, &faceView)) {
                return false;
            }

            const AttachmentView view{prefiltered->image.handle, &faceView, levelDesc};
            if (!BeginPass(vk, cmd, desc, &view, nullptr, area,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
                return false;
            }

            BindPipeline(vk, cmd, pipeline, area);
            vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       program.layout, kFrameSet, 1, &set, 0, nullptr);
            const PrefilterFace face{static_cast<int32_t>(layer), roughness};
            vk.vkCmdPushConstants(cmd, program.layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                  0, sizeof(face), &face);
            vk.vkCmdDraw(cmd, 3, 1, 0, 0);
            vk.vkCmdEndRendering(cmd);
        }
    }

    RecordSampledHandover(vk, cmd, *prefiltered, AttachmentRole::Color);
    if (!EndOneShotAndWait(dev, commands, cmd, "prefilter bake")) { return false; }

    LOG("[render] prefiltered cube baked (%ux%u, %u levels)\n",
        prefiltered->desc.extent.width, prefiltered->desc.extent.height,
        prefiltered->desc.mipLevels);
    return true;
}

bool BakeBrdfLut(const VulkanDevice& dev, const Commands& commands,
                 const Pipeline& pipeline, Texture* lut) noexcept {
    RenderPassDesc desc{};
    desc.attachments[0].resource = &lut->desc;
    desc.attachments[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    desc.attachments[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    if (!ValidatePassDesc(desc)) { return false; }

    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }
    const VolkDeviceTable& vk = dev.table;

    const VkRect2D area{{0, 0}, lut->desc.extent};
    const AttachmentView view = TargetOf(*lut);
    if (!BeginPass(vk, cmd, desc, &view, nullptr, area,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
        return false;
    }
    BindPipeline(vk, cmd, pipeline, area);
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    vk.vkCmdEndRendering(cmd);

    RecordSampledHandover(vk, cmd, *lut, AttachmentRole::Color);
    if (!EndOneShotAndWait(dev, commands, cmd, "brdf lut bake")) { return false; }

    LOG("[render] brdf table baked (%ux%u)\n", lut->desc.extent.width,
        lut->desc.extent.height);
    return true;
}

bool BakeSkyCube(const VulkanDevice& dev, const Commands& commands,
                 const Pipeline& pipeline, Texture* cube) noexcept {
    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }

    // No set: this program reads nothing. The face index is the whole of its input.
    if (!BakeCubeFaces(dev, cmd, pipeline, VK_NULL_HANDLE, cube)) { return false; }

    // Every face at once, because from here the cube is read as one thing.
    RecordSampledHandover(dev.table, cmd, *cube, AttachmentRole::Color);

    if (!EndOneShotAndWait(dev, commands, cmd, "sky bake")) { return false; }

    LOG("[render] sky cube baked (%ux%u, 6 faces)\n",
        cube->desc.extent.width, cube->desc.extent.height);
    return true;
}

bool CreateSkyPass(const Descriptors& descriptors,
                   const TextureDesc& targetDesc,
                   const Texture* const targets[kFramesInFlight],
                   const Pipeline& pipeline,
                   const FrameSetSources& frameSet,
                   SkyPass* out) noexcept {
    if (pipeline.program == nullptr) {
        LOG("[vk] a pass was given a pipeline that names no program\n");
        return false;
    }
    const ShaderProgram& program = *pipeline.program;
    out->pipeline = &pipeline;

    // DONT_CARE and not CLEAR: the sky covers every pixel of the area, so clearing
    // first would be writing twice. STORE because the pass after this loads it.
    out->pass.attachments[0].resource = &targetDesc;
    out->pass.attachments[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    out->pass.attachments[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    if (!ValidatePassDesc(out->pass)) { return false; }

    if (!SameAttachmentFormats(PassFormats(out->pass), pipeline.formats)) {
        LOG("[vk] the sky pass's target and its pipeline disagree about the formats\n");
        return false;
    }

    VkDescriptorSet sets[kFramesInFlight]{};
    if (!AllocateSets(descriptors, program.setLayouts[kFrameSet], kFramesInFlight, sets)) {
        return false;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        out->frames[i].set = sets[i];
        out->frames[i].target = targets[i];
        if (!FillFrameSet(descriptors, program, sets[i], frameSet, i, &out->pass)) {
            return false;
        }
    }
    return true;
}

void RecordSkyPass(const FrameSlot& slot, const SkyPass& sky) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const SkyPass::PerFrame& frame = sky.frames[slot.index];
    const Texture& target = *frame.target;
    const VkRect2D area{{0, 0}, target.desc.extent};

    const AttachmentView views[] = {TargetOf(target)};
    if (!BeginPass(vk, cmd, sky.pass, views, nullptr, area,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
        return;
    }

    BindPipeline(vk, cmd, *sky.pipeline, area);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               sky.pipeline->program->layout, kFrameSet,
                               1, &frame.set, 0, nullptr);
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    vk.vkCmdEndRendering(cmd);

    // The pass after this loads what was just written, and BeginPass issues no barrier
    // for a loadOp of LOAD -- what wrote the image is not in that call. This is that.
    RecordLoadHandover(vk, cmd, target, AttachmentRole::Color);
}
