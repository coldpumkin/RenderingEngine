#include "PostProcessPass.h"

#include "Vulkan/Barrier.h"

void RefreshPostProcessPass(const Descriptors& descriptors,
                            PostProcessPass* post) noexcept {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        const BindingValue values[] = {{&post->source[i]->view}};
        UpdateSet(descriptors, post->pipeline->program->setLayouts[kFrameSet],
                  post->sets[i],
                  values, 1);
    }
}

bool CreatePostProcessPass(const Descriptors& descriptors,
                           const Texture* const source[kFramesInFlight],
                           const TextureDesc& target,
                           const Pipeline& pipeline, PostProcessPass* out) noexcept {
    // The program is the pipeline's, not a second argument beside it. A pipeline
    // records what it was built from, and taking both let a caller hand over a pair
    // that never met -- which is what the check below used to be for.
    if (pipeline.program == nullptr) {
        LOG("[vk] a pass was given a pipeline that names no program\n");
        return false;
    }
    const ShaderProgram& program = *pipeline.program;

    out->pipeline = &pipeline;

    out->pass.attachments[0].resource = &target;
    out->pass.attachments[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    out->pass.attachments[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    out->pass.attachments[0].clear.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

    // Everything the declaration can be wrong about on its own, asked once here. What
    // needs a frame's images is asked every frame by BeginPass.
    if (!ValidatePassDesc(out->pass)) { return false; }
    out->target = &target;

    // What it writes, against what the pipeline baked -- the same comparison the other
    // two passes make. It could not be made here until this pass was told what it
    // writes; the pipeline was the only one holding an answer.
    if (!SameAttachmentFormats(PassFormats(out->pass), pipeline.formats)) {
        LOG("[vk] the post pass's target and its pipeline disagree about the formats\n");
        return false;
    }

    // What it reads, asked the way every reader asks it now. A sampler cannot take a
    // multisample image, which is the whole reason the scene pass resolves -- and this
    // used to be that one question alone, written here.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CheckSampledInput(source[i]->desc, "post pass's source",
                               false)) {
            return false;
        }
    }

    if (!AllocateSets(descriptors, program.setLayouts[kFrameSet], kFramesInFlight,
                      out->sets)) {
        return false;
    }

    // The pointer and the set that names it are written in the same step, so the two
    // cannot come to disagree about which image frame i reads.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        out->source[i] = source[i];
        const BindingValue values[] = {{&source[i]->view}};
        UpdateSet(descriptors, program.setLayouts[kFrameSet], out->sets[i], values, 1);
    }
    return true;
}

// Output: the largest rect inside dest that has source's aspect, centred
//
// The 2D -> 2D contract in Pipeline.h, held for the one pass that owes it. Fitting by
// whichever side runs out first is what "largest that still fits" means, and the
// leftover split in two is what centres it.
//
// Equal aspects give back {{0, 0}, dest} exactly, which is what every window at the
// render target's own shape gets -- the case this has to leave alone.
//
// Integer throughout, and the comparison is cross-multiplied rather than two
// divisions: same answer, and no float to round the wrong way at the boundary.
static VkRect2D LetterboxInto(VkExtent2D source, VkExtent2D dest) noexcept {
    const uint64_t sourceIsWider = uint64_t{source.width} * dest.height;
    const uint64_t destIsWider = uint64_t{dest.width} * source.height;

    VkExtent2D fitted = dest;
    if (sourceIsWider > destIsWider) {
        // Width fills the target and the bars are above and below.
        fitted.height = static_cast<uint32_t>(uint64_t{dest.width} * source.height
                                              / source.width);
    } else if (sourceIsWider < destIsWider) {
        fitted.width = static_cast<uint32_t>(uint64_t{dest.height} * source.width
                                             / source.height);
    }

    VkRect2D area{};
    area.offset.x = static_cast<int32_t>((dest.width - fitted.width) / 2);
    area.offset.y = static_cast<int32_t>((dest.height - fitted.height) / 2);
    area.extent = fitted;
    return area;
}

void RecordPostProcessPass(const FrameSlot& slot, const PostProcessPass& post,
                           const Texture& target) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const Pipeline& pipeline = *post.pipeline;
    // From the pipeline about to be bound, not from a program the pass holds: what a
    // draw receives is the pipeline's fact.
    const VkPipelineLayout layout = pipeline.program->layout;

    // Handed in rather than found: this pass does not know what drew it. The set
    // bound below names this same image, both picked by slot.index.
    const Texture& source = *post.source[slot.index];
    const Texture& dest = target;
    const VkExtent2D destExtent = dest.desc.extent;

    // The image this pass reads. Written as an attachment by the pass before, read as
    // a texture here -- and the layout must equal the one recorded into the set.
    //
    // The three values passed are the scene pass's, not this one's: a reader cannot
    // work out where its input stopped being written. That they are spelled out here
    // is what a reader transitioning someone else's product costs.
    RecordSampledTransition(vk, cmd, source.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // The image this pass draws into. Window sized, unlike the scene pass -- the
    // sampler's LINEAR filter scales.
    // The waited stage is the one thing here that cannot be derived. The presentation
    // engine owns this image until the acquire, and SubmitFrame's semaphore waits at
    // COLOR_ATTACHMENT_OUTPUT for it -- a transition scheduled ahead of that would run
    // before the acquire. Every other attachment in this program is TOP_OF_PIPE because
    // nothing outside the command buffer holds it.
    const Texture* const views[] = {&dest};
    if (!BeginPass(vk, cmd, post.pass, views, nullptr,
                   VkRect2D{{0, 0}, destExtent},
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)) {
        return;
    }

    // The area is the whole point. fullscreen.vert's uv runs 0..1 over the source no
    // matter what, so the shape of the picture is decided here and nowhere else: hand
    // in the whole target and it stretches. This is the only call of the four that
    // passes anything but the target it draws on, and the reason area is a parameter
    // rather than the pipeline's like the rest of its raster state.
    BindPipeline(vk, cmd, pipeline, LetterboxInto(source.desc.extent, destExtent));

    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                               0, 1, &post.sets[slot.index], 0, nullptr);

    // 3 vertices, no buffer. The shader builds them from gl_VertexIndex.
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    vk.vkCmdEndRendering(cmd);

    // The target is left COLOR_ATTACHMENT_OPTIMAL, which is this pass's whole output
    // contract. What happens to it next -- another pass on top, or the screen -- is
    // not this function's to know, and the transition that used to be here said
    // otherwise.
}
