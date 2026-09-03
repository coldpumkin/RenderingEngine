#include "Gui.h"

#include "Vulkan/Window.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>

#include <GLFW/glfw3.h>

#include <cstring>    // memcpy
#include <iterator>   // std::size

namespace {

// Room for one frame of widgets. Fixed, because growing it would be a heap
// allocation inside the frame loop.
//
// The panel we have is a few hundred vertices with every section open. These are two
// orders above that and still under a quarter megabyte for the pair -- the cost of
// being wrong in the cheap direction.
constexpr VkDeviceSize kMaxGuiVertexBytes = 192u * 1024u;
constexpr VkDeviceSize kMaxGuiIndexBytes = 64u * 1024u;

// What a descriptor type is, short enough to sit in a table.
//
// Only the two we declare. A third would be an unnamed number here, which is louder
// than a wrong name -- the panel exists to show what is actually there.
const char* TypeName(VkDescriptorType type) noexcept {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "sampler2D";
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:         return "uniform";
    default: return "?";
    }
}

// The formats we actually ask for. Anything else prints its number, which is a
// louder way of saying "we did not expect this" than a wrong name would be.
const char* FormatName(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:     return "RGBA8 srgb";
    case VK_FORMAT_R8G8B8A8_UNORM:    return "RGBA8 unorm";
    case VK_FORMAT_B8G8R8A8_SRGB:     return "BGRA8 srgb";
    case VK_FORMAT_B8G8R8A8_UNORM:    return "BGRA8 unorm";
    case VK_FORMAT_D32_SFLOAT:        return "D32 float";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case VK_FORMAT_UNDEFINED:         return "-";
    default:                          return "?";
    }
}

// One line about an image: what it is, how big, how many samples.
//
// The sample count is the interesting column. The ones inside the scene pass are 4x
// and the ones that leave it are 1x, which is the whole shape of the resolve.
void ShowTexture(const char* name, const Texture* texture) noexcept {
    if (texture == nullptr) {
        ImGui::Text("%-9s -", name);
        return;
    }
    const TextureDesc& d = texture->desc;
    ImGui::Text("%-9s %-12s %4ux%-4u  %ux",
                name, FormatName(d.format), d.extent.width, d.extent.height,
                static_cast<uint32_t>(d.samples));
}

// One row per set a pipeline declares, and one line per binding in it.
//
// An empty set is printed too. Vulkan numbers sets by position, so set 1 cannot exist
// without a set 0 in front of it, and a layout with no bindings is how that is said.
void ShowSetLayouts(const char* name, const ShaderProgram* program) noexcept {
    if (program == nullptr) { return; }
    for (uint32_t set = 0; set < kMaxSets; ++set) {
        const DescriptorLayout& layout = program->setLayouts[set];
        if (layout.bindingCount == 0) {
            ImGui::Text("%-8s set %u   (empty)", set == 0 ? name : "", set);
            continue;
        }
        ImGui::Text("%-8s set %u", set == 0 ? name : "", set);
        for (uint32_t b = 0; b < layout.bindingCount; ++b) {
            if (layout.types[b] == 0) { continue; }   // a hole in the numbering
            ImGui::Text("             [%u] %s", b, TypeName(layout.types[b]));
        }
    }
}

// The values a pipeline was built from. Everything here is baked in at creation --
// that is what a Vulkan pipeline is -- so this is the only place to see what was
// baked without reading the call that made it.
void ShowPipeline(const char* name, const Pipeline* pipeline) noexcept {
    if (pipeline == nullptr) { return; }
    const GraphicsPipelineDesc& d = pipeline->desc;
    ImGui::Text("%-8s %-11s  %ux  %s",
                name,
                d.blending == Blending::Opaque ? "opaque" : "translucent",
                static_cast<uint32_t>(d.formats.samples),
                d.viewportY == ViewportY::Up ? "y-up" : "y-down");
    const char* frag = pipeline->program != nullptr ? pipeline->program->fragPath : nullptr;
    ImGui::Text("         %s", frag != nullptr ? frag : "-");
}

}   // namespace

const VkPipelineVertexInputStateCreateInfo& GuiVertexInput() noexcept {
    // ImDrawVert is {ImVec2 pos, ImVec2 uv, ImU32 col} -- 20 bytes. Its offsets come
    // from offsetof for the same reason the scene's do: a field moving must not need
    // a second edit here.
    static constexpr VkVertexInputBindingDescription binding{
        0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};

    static constexpr VkVertexInputAttributeDescription attributes[]{
        {0, 0, VK_FORMAT_R32G32_SFLOAT,  offsetof(ImDrawVert, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT,  offsetof(ImDrawVert, uv)},
        // Four bytes, not four floats. UNORM is what turns 0..255 into the 0..1 the
        // shader reads -- the conversion belongs to the format, not the shader.
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col)},
    };

    static const VkPipelineVertexInputStateCreateInfo info{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
        1, &binding,
        static_cast<uint32_t>(std::size(attributes)), attributes};
    return info;
}

bool CreateGui(const VulkanDevice& dev, const Commands& commands,
               Window& window, Gui* out) noexcept {
    out->dev = &dev;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    out->started = true;
    ImGui::StyleColorsDark();

    // No .ini file. It would remember window positions across runs, which makes two
    // runs of the same build differ -- the opposite of what the capture tool needs.
    ImGui::GetIO().IniFilename = nullptr;

    // The default font is 13px, a third of the height of the text in the console
    // beside it. Scaling the built-in atlas is blurry at large factors but costs no
    // font file; a real one goes in when the panel has to be read rather than glanced
    // at. ScaleAllSizes too, or the boxes stay 13px-sized around 20px text.
    constexpr float kUiScale = 1.6f;
    ImGui::GetIO().FontGlobalScale = kUiScale;
    ImGui::GetStyle().ScaleAllSizes(kUiScale);

    // Input only. The Vulkan half of ImGui's backends is what this file replaces.
    if (!ImGui_ImplGlfw_InitForVulkan(window.handle, true)) {
        LOG("[gui] ImGui_ImplGlfw_InitForVulkan failed\n");
        return false;
    }

    // The atlas, as one of our textures. RGBA8 rather than the single channel ImGui
    // can also give, because one shared sampler reads every image the same way and a
    // megabyte at init is cheaper than a second sampler.
    //
    // UNORM, not SRGB: these bytes are coverage, and encoding them would thin the
    // text. The same distinction the normal maps needed.
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const TextureDesc fontDesc{{static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                               VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    const size_t fontBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (!CreateTextureFromPixels(dev, commands, fontDesc, pixels, fontBytes, &out->font)) {
        return false;
    }
    LOG("[gui] font atlas %dx%d\n", width, height);

    // HOST_VISIBLE and mapped, unlike a mesh: the CPU rewrites these every frame, so
    // a staging copy would be a round trip per frame for data that is already small.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateBuffer(dev, kMaxGuiVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &out->frames[i].vertices)) {
            return false;
        }
        if (!CreateBuffer(dev, kMaxGuiIndexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &out->frames[i].indices)) {
            return false;
        }
        if (out->frames[i].vertices.mapped == nullptr
                || out->frames[i].indices.mapped == nullptr) {
            LOG("[gui] gui buffers are not mapped\n");
            return false;
        }
    }
    return true;
}

bool CreateGuiSet(const Descriptors& descriptors, const ShaderProgram& program,
                  const Pipeline& pipeline, Gui* out) noexcept {
    out->program = &program;
    out->pipeline = &pipeline;
    if (!AllocateSets(descriptors, program.setLayouts[0], 1, &out->set)) {
        return false;
    }
    const BindingValue values[] = {{out->font.view.handle}};
    UpdateSet(descriptors, program.setLayouts[0], out->set, values, 1);
    return true;
}

Gui::~Gui() {
    if (started) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    // The texture and the buffers are members and free themselves. The set goes with
    // the pool, which outlives this because it is declared before it.
}

void BuildGui(ViewOptions* options, const GuiFrameInfo& info) noexcept {
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    // AlwaysAutoResize, not a size: every one of these is a list whose length is a
    // fact about the program, and a scrollbar would hide the part that changed.
    if (ImGui::Begin("View", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Checkbox("normal map", &options->normalMap);
        ImGui::Checkbox("base colour", &options->baseColor);
        ImGui::Checkbox("specular", &options->specular);
        ImGui::Checkbox("alpha mask", &options->alphaMask);

        ImGui::Separator();
        // Both numbers, because they answer different questions: the rate is what a
        // person reads, the milliseconds are what a change moves. A guard on the
        // first frame, where the gap is zero.
        const float fps = info.frameSeconds > 0.0f ? 1.0f / info.frameSeconds : 0.0f;
        ImGui::Text("fps    %.0f  (%.2f ms)", fps, info.frameSeconds * 1000.0f);
        ImGui::Text("draws  %u", info.drawCount);
        ImGui::Text("mats   %u", info.materialCount);
    }
    ImGui::End();

    // Four sections, one window, collapsed by default. Five windows did not fit at
    // 1280x720 and the one you wanted was always the one off screen.
    ImGui::SetNextWindowPos(ImVec2(12.0f, 300.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspect", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        ImGui::Render();
        return;
    }

    if (ImGui::CollapsingHeader("descriptors") && info.descriptors != nullptr) {
        const Descriptors& d = *info.descriptors;

        // Two numbers because the pool takes two: how many sets may be drawn, and how
        // many descriptors those sets hold. A layout with two bindings spends one of
        // the first and two of the second.
        ImGui::Text("pool   %u sets", d.maxSets);
        ImGui::Text("       %u sampler2D  +  %u uniform",
                    d.imageDescriptors, d.bufferDescriptors);
        ImGui::Separator();

        ShowSetLayouts("scene", info.sceneProgram);
        ShowSetLayouts("present", info.presentProgram);
        ShowSetLayouts("gui", info.guiProgram);
    }

    if (ImGui::CollapsingHeader("shader data")) {
        // Three ways to get bytes to a shader, and the reason there are three is how
        // often each changes and how big it is allowed to be.
        ImGui::Text("uniform  %3u B   x%u    per frame, host visible + mapped",
                    info.uniformBytes, info.framesInFlight);
        ImGui::Text("push     %3u B         per draw, inside the command buffer",
                    info.pushBytes);
        ImGui::Text("vertex   %3u B   x%u attrs   per vertex, in the buffer",
                    info.vertexStride, info.vertexAttributes);

        if (info.mesh != nullptr) {
            ImGui::Separator();
            const MeshDesc& m = info.mesh->desc;
            // One buffer each for the whole scene. Every DrawItem is a span inside
            // these, which is why 103 draws need no rebinding between them.
            ImGui::Text("mesh     %u verts  %u indices  %s",
                        m.vertexCount, m.indexCount,
                        m.indexType == VK_INDEX_TYPE_UINT16 ? "uint16" : "uint32");
            ImGui::Text("         %u KB + %u KB in two buffers",
                        (m.vertexCount * m.vertexStride) / 1024,
                        (m.indexCount * (m.indexType == VK_INDEX_TYPE_UINT16 ? 2u : 4u)) / 1024);
        }

        // The panel's own, and the only vertices here that are rewritten per frame.
        ImGui::Separator();
        ImGui::Text("gui vtx  %3u B   x3 attrs   rewritten every frame",
                    static_cast<uint32_t>(sizeof(ImDrawVert)));
    }

    // What this frame draws through, in the order it happens. The sample counts say
    // where MSAA starts and stops, and the two extents say why the render resolution
    // is not the window's.
    if (ImGui::CollapsingHeader("frame")) {
        ImGui::Text("slot %u of %u", info.slotIndex, info.framesInFlight);
        ImGui::Separator();
        ShowTexture("color", info.sceneColor);
        ShowTexture("resolve", info.sceneResolve);
        ShowTexture("depth", info.sceneDepth);
        ShowTexture("target", info.frameTarget);
        ImGui::Separator();
        // The order is three lines in RecordFrame and nothing else enforces it.
        ImGui::TextUnformatted("scene  -> color+depth, resolving into resolve");
        ImGui::TextUnformatted("post   -> samples resolve, draws into target");
        ImGui::TextUnformatted("gui    -> draws on target, loadOp LOAD");
        ImGui::TextUnformatted("then   target -> PRESENT_SRC");
    }

    if (ImGui::CollapsingHeader("pipelines")) {
        ShowPipeline("scene", info.scenePipeline);
        ShowPipeline("present", info.presentPipeline);
        ShowPipeline("gui", info.guiPipeline);
        ImGui::Separator();
        // The three the pipelines do not bake. Named here because the panel lists
        // what was baked, and the absence is the interesting half.
        ImGui::TextUnformatted("dynamic  viewport  scissor  cullMode");
    }

    ImGui::End();

    // Here, not in the pass: this is the last point where the panel is data. After it
    // the draw list is fixed and recording only reads it.
    ImGui::Render();
}

void RecordGuiPass(const FrameSlot& slot, Gui& gui, const Texture& target) noexcept {
    const ImDrawData* draws = ImGui::GetDrawData();
    if (draws == nullptr || draws->TotalVtxCount == 0 || gui.pipeline == nullptr
            || gui.program == nullptr) {
        return;
    }
    const Pipeline& pipeline = *gui.pipeline;
    const VkPipelineLayout layout = gui.program->layout;

    const VkDeviceSize vertexBytes =
        static_cast<VkDeviceSize>(draws->TotalVtxCount) * sizeof(ImDrawVert);
    const VkDeviceSize indexBytes =
        static_cast<VkDeviceSize>(draws->TotalIdxCount) * sizeof(ImDrawIdx);

    Gui::PerFrame& buffers = gui.frames[slot.index];
    if (vertexBytes > buffers.vertices.size || indexBytes > buffers.indices.size) {
        // Skipping the panel is the right failure: the picture underneath is still
        // correct, and half a panel would be worse than none.
        if (!gui.warnedTooBig) {
            gui.warnedTooBig = true;
            LOG("[gui] a frame wanted %llu vertex bytes, the buffer holds %llu\n",
                static_cast<unsigned long long>(vertexBytes),
                static_cast<unsigned long long>(buffers.vertices.size));
        }
        return;
    }

    // ImGui keeps one list per window; the buffers here are one each, so the lists go
    // in end to end and every draw below carries an offset into them.
    //
    // Safe to write now because BeginFrame waited on this slot's fence, so the GPU has
    // finished with what this slot wrote last time round.
    auto* vertexOut = static_cast<ImDrawVert*>(buffers.vertices.mapped);
    auto* indexOut = static_cast<ImDrawIdx*>(buffers.indices.mapped);
    for (int i = 0; i < draws->CmdListsCount; ++i) {
        const ImDrawList* list = draws->CmdLists[i];
        std::memcpy(vertexOut, list->VtxBuffer.Data,
                    static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
        std::memcpy(indexOut, list->IdxBuffer.Data,
                    static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
        vertexOut += list->VtxBuffer.Size;
        indexOut += list->IdxBuffer.Size;
    }

    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;

    // LOAD, unlike the two passes before it: this one draws on top of a finished
    // picture rather than replacing it. No barrier either -- the post-process pass
    // left the image COLOR_ATTACHMENT_OPTIMAL, which is what this needs.
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.view.handle;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = target.desc.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vk.vkCmdBeginRendering(cmd, &rendering);

    const VkViewport viewport = MakeViewport(target.desc.extent, pipeline.desc.viewportY);
    vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
    // Nothing is culled: the panel's triangles have no consistent winding, and a
    // rectangle has no back to hide.
    vk.vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                               0, 1, &gui.set, 0, nullptr);

    const VkDeviceSize offset = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &buffers.vertices.handle, &offset);
    // Contract: this type must match ImDrawIdx, which is 16 bits unless imconfig.h
    //           says otherwise.
    vk.vkCmdBindIndexBuffer(cmd, buffers.indices.handle, 0, VK_INDEX_TYPE_UINT16);

    // Pixels to clip space. DisplayPos is not always zero -- it is the top-left of
    // the area ImGui was told to draw into.
    GuiPushConstants push{};
    push.scale[0] = 2.0f / draws->DisplaySize.x;
    push.scale[1] = 2.0f / draws->DisplaySize.y;
    push.translate[0] = -1.0f - draws->DisplayPos.x * push.scale[0];
    push.translate[1] = -1.0f - draws->DisplayPos.y * push.scale[1];
    vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                          0, sizeof(push), &push);

    // One draw per command, and a scissor with it: clipping is how ImGui keeps a
    // widget inside its window, and it changes far more often than anything in the
    // scene pass does.
    int vertexOffset = 0;
    uint32_t indexOffset = 0;
    for (int i = 0; i < draws->CmdListsCount; ++i) {
        const ImDrawList* list = draws->CmdLists[i];
        for (int c = 0; c < list->CmdBuffer.Size; ++c) {
            const ImDrawCmd& command = list->CmdBuffer[c];

            // A command can carry a callback instead of geometry. We register none,
            // so one here would mean something else wrote into our draw list.
            if (command.UserCallback != nullptr) { continue; }

            // ClipRect is in ImGui's coordinates; subtracting DisplayPos makes it the
            // framebuffer's. The clamp keeps a negative left edge -- a window dragged
            // off screen -- from becoming a huge unsigned number.
            const float left = command.ClipRect.x - draws->DisplayPos.x;
            const float top = command.ClipRect.y - draws->DisplayPos.y;
            const float right = command.ClipRect.z - draws->DisplayPos.x;
            const float bottom = command.ClipRect.w - draws->DisplayPos.y;
            if (right <= left || bottom <= top) { continue; }

            VkRect2D scissor{};
            scissor.offset.x = left > 0.0f ? static_cast<int32_t>(left) : 0;
            scissor.offset.y = top > 0.0f ? static_cast<int32_t>(top) : 0;
            scissor.extent.width =
                static_cast<uint32_t>(right) - static_cast<uint32_t>(scissor.offset.x);
            scissor.extent.height =
                static_cast<uint32_t>(bottom) - static_cast<uint32_t>(scissor.offset.y);
            vk.vkCmdSetScissor(cmd, 0, 1, &scissor);

            vk.vkCmdDrawIndexed(cmd, command.ElemCount, 1,
                                command.IdxOffset + indexOffset,
                                static_cast<int32_t>(command.VtxOffset) + vertexOffset,
                                0);
        }
        indexOffset += static_cast<uint32_t>(list->IdxBuffer.Size);
        vertexOffset += list->VtxBuffer.Size;
    }

    vk.vkCmdEndRendering(cmd);
}
