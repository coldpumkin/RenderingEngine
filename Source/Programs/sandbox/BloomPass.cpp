#include "BloomPass.h"

#include "Config.h"
#include "Vulkan/Barrier.h"

TextureDesc MakeBloomTarget(VkExtent2D sceneExtent) noexcept {
    TextureDesc desc{};

    // Half, rounded up, and never zero: a window dragged to one pixel still has to have
    // somewhere to put it.
    desc.extent = VkExtent2D{(sceneExtent.width + 1) / 2 > 0 ? (sceneExtent.width + 1) / 2
                                                             : 1u,
                             (sceneExtent.height + 1) / 2 > 0 ? (sceneExtent.height + 1) / 2
                                                              : 1u};
    desc.format = kRenderColorFormat;
    desc.samples = VK_SAMPLE_COUNT_1_BIT;
    desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return desc;
}

namespace {

// One draw's declaration: it writes target and samples source. Written once because the
// three are the same shape and differ only in which pair of images they name.
bool DeclareStep(const TextureDesc& target, const PassInput& source, const char* what,
                 RenderPassDesc* pass) noexcept {
    pass->attachments[0].resource = &target;

    // DONT_CARE: the draw covers the whole target, so there is nothing under it worth
    // keeping and nothing to clear either.
    pass->attachments[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    pass->attachments[0].store = VK_ATTACHMENT_STORE_OP_STORE;

    if (!ValidatePassDesc(*pass)) { return false; }
    return DeclareRead(source, what, false, pass);
}

}  // namespace

bool CreateBloomPass(const Descriptors& descriptors,
                     const PassInput& source,
                     const TextureDesc& aDesc, const Texture* const a[kFramesInFlight],
                     const TextureDesc& bDesc, const Texture* const b[kFramesInFlight],
                     const Pipeline& extractPipeline, const Pipeline& blurPipeline,
                     BloomPass* out) noexcept {
    if (extractPipeline.program == nullptr || blurPipeline.program == nullptr) {
        LOG("[vk] a pass was given a pipeline that names no program\n");
        return false;
    }
    out->extractPipeline = &extractPipeline;
    out->blurPipeline = &blurPipeline;

    // What each step reads, as the kind of input a read is declared with. A and B are
    // the caller's descs; the frames are the caller's images.
    PassInput fromA{&aDesc, {}};
    PassInput fromB{&bDesc, {}};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        fromA.frames[i] = a[i];
        fromB.frames[i] = b[i];
    }

    if (!DeclareStep(aDesc, source, "bloom extract's source", &out->extract)
            || !DeclareStep(bDesc, fromA, "bloom blur's first axis", &out->blurH)
            || !DeclareStep(aDesc, fromB, "bloom blur's second axis", &out->blurV)) {
        return false;
    }

    if (!SameAttachmentFormats(PassFormats(out->extract), extractPipeline.formats)
            || !SameAttachmentFormats(PassFormats(out->blurH), blurPipeline.formats)) {
        LOG("[vk] a bloom target and its pipeline disagree about the formats\n");
        return false;
    }

    const DescriptorLayout& extractLayout =
        extractPipeline.program->setLayouts[kFrameSet];
    const DescriptorLayout& blurLayout = blurPipeline.program->setLayouts[kFrameSet];

    VkDescriptorSet extractSets[kFramesInFlight]{};
    VkDescriptorSet blurSets[kFramesInFlight * 2]{};
    if (!AllocateSets(descriptors, extractLayout, kFramesInFlight, extractSets)
            || !AllocateSets(descriptors, blurLayout, kFramesInFlight * 2, blurSets)) {
        return false;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        BloomPass::PerFrame& frame = out->frames[i];
        frame.a = a[i];
        frame.b = b[i];
        frame.source = source.frames[i];
        frame.extractSet = extractSets[i];
        frame.blurHSet = blurSets[i * 2];
        frame.blurVSet = blurSets[i * 2 + 1];
    }
    RefreshBloomPass(descriptors, out);
    return true;
}

void RefreshBloomPass(const Descriptors& descriptors, BloomPass* bloom) noexcept {
    const DescriptorLayout& extractLayout =
        bloom->extractPipeline->program->setLayouts[kFrameSet];
    const DescriptorLayout& blurLayout =
        bloom->blurPipeline->program->setLayouts[kFrameSet];

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        BloomPass::PerFrame& frame = bloom->frames[i];

        const BindingValue fromScene[] = {{&frame.source->view}};
        const BindingValue fromA[] = {{&frame.a->view}};
        const BindingValue fromB[] = {{&frame.b->view}};
        UpdateSet(descriptors, extractLayout, frame.extractSet, fromScene, 1);
        UpdateSet(descriptors, blurLayout, frame.blurHSet, fromA, 1);
        UpdateSet(descriptors, blurLayout, frame.blurVSet, fromB, 1);
    }
}

void RecordBloomPass(const FrameSlot& slot, const BloomPass& bloom) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const BloomPass::PerFrame& frame = bloom.frames[slot.index];
    const VkRect2D area{{0, 0}, frame.a->desc.extent};

    // One texel of the half-size image, along each axis.
    const BloomStep across{1.0f / static_cast<float>(frame.a->desc.extent.width), 0.0f};
    const BloomStep down{0.0f, 1.0f / static_cast<float>(frame.a->desc.extent.height)};

    // The middle's output, from an attachment into something a sampler can read. This
    // pass is the first reader of it, so the move is here -- the post pass reads the
    // same image afterwards and finds it already where it needs it.
    //
    // Which of the two middles wrote it is not asked: both draw into it as a colour
    // attachment, and that role is the whole of what the source half follows from.
    RecordSampledHandover(vk, cmd, *frame.source, AttachmentRole::Color,
                          WholeImage(VK_IMAGE_ASPECT_COLOR_BIT));

    // Extract: the scene into A.
    const AttachmentView toA[] = {TargetOf(*frame.a)};
    if (!BeginPass(vk, cmd, bloom.extract, toA, nullptr, area,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
        return;
    }
    BindPipeline(vk, cmd, *bloom.extractPipeline, area);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               bloom.extractPipeline->program->layout, kFrameSet, 1,
                               &frame.extractSet, 0, nullptr);
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    vk.vkCmdEndRendering(cmd);
    RecordSampledHandover(vk, cmd, *frame.a, AttachmentRole::Color,
                          WholeImage(VK_IMAGE_ASPECT_COLOR_BIT));

    // Horizontal: A into B.
    const AttachmentView toB[] = {TargetOf(*frame.b)};
    if (!BeginPass(vk, cmd, bloom.blurH, toB, nullptr, area,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
        return;
    }
    BindPipeline(vk, cmd, *bloom.blurPipeline, area);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               bloom.blurPipeline->program->layout, kFrameSet, 1,
                               &frame.blurHSet, 0, nullptr);
    vk.vkCmdPushConstants(cmd, bloom.blurPipeline->program->layout,
                          VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(across), &across);
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    vk.vkCmdEndRendering(cmd);
    RecordSampledHandover(vk, cmd, *frame.b, AttachmentRole::Color,
                          WholeImage(VK_IMAGE_ASPECT_COLOR_BIT));

    // Vertical: B back into A, which is where the post pass reads it.
    if (!BeginPass(vk, cmd, bloom.blurV, toA, nullptr, area,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
        return;
    }
    BindPipeline(vk, cmd, *bloom.blurPipeline, area);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               bloom.blurPipeline->program->layout, kFrameSet, 1,
                               &frame.blurVSet, 0, nullptr);
    vk.vkCmdPushConstants(cmd, bloom.blurPipeline->program->layout,
                          VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(down), &down);
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    vk.vkCmdEndRendering(cmd);
    RecordSampledHandover(vk, cmd, *frame.a, AttachmentRole::Color,
                          WholeImage(VK_IMAGE_ASPECT_COLOR_BIT));
}
