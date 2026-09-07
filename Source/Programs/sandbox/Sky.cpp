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

bool BakeSkyCube(const VulkanDevice& dev, const Commands& commands,
                 const Pipeline& pipeline, Texture* cube) noexcept {
    const TextureDesc faceDesc = SliceDesc(cube->desc);

    // One 2D view per layer. The kind says 2D and not Cube because that is what one
    // layer is -- a cube is how the six are addressed together, and nothing addresses
    // them together while they are being drawn.
    ImageView faces[6];
    for (uint32_t i = 0; i < std::size(faces); ++i) {
        ImageViewDesc viewDesc;
        viewDesc.type = VK_IMAGE_VIEW_TYPE_2D;
        viewDesc.baseLayer = i;
        viewDesc.layerCount = 1;
        if (!CreateImageView(dev, cube->image.handle, cube->desc.format,
                             cube->desc.samples, cube->desc.usage, viewDesc, &faces[i])) {
            return false;
        }
    }

    // One attachment, cleared by being written: the shader covers the face, so what was
    // there is dead. Declared against the face's desc rather than the cube's, because a
    // 2D view of one layer is what this pass draws into.
    RenderPassDesc desc{};
    desc.attachments[0].resource = &faceDesc;
    desc.attachments[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    desc.attachments[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    if (!ValidatePassDesc(desc)) { return false; }

    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }

    const VolkDeviceTable& vk = dev.table;
    const VkRect2D area{{0, 0}, cube->desc.extent};

    for (uint32_t i = 0; i < std::size(faces); ++i) {
        const AttachmentView view{cube->image.handle, &faces[i], faceDesc};
        if (!BeginPass(vk, cmd, desc, &view, nullptr, area,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
            return false;
        }

        BindPipeline(vk, cmd, pipeline, area);
        const SkyFace face{static_cast<int32_t>(i)};
        vk.vkCmdPushConstants(cmd, pipeline.program->layout,
                              VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(face), &face);
        vk.vkCmdDraw(cmd, 3, 1, 0, 0);
        vk.vkCmdEndRendering(cmd);
    }

    // Every face at once, because from here the cube is read as one thing.
    RecordSampledHandover(vk, cmd, *cube, AttachmentRole::Color);

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
