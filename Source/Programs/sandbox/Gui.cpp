#include "Gui.h"

#include "Config.h"
#include "Vulkan/Window.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <GLFW/glfw3.h>

namespace {

// Where ImGui's Vulkan calls come from.
//
// Not volk's globals. We fill the instance-level ones (volkLoadInstanceOnly) and
// deliberately leave the device-level ones empty -- everything of ours goes through
// VolkDeviceTable, so a second device cannot be overwritten by the last one loaded.
// IMGUI_IMPL_VULKAN_USE_VOLK would have had the backend call those empty globals.
//
// vkGetInstanceProcAddr resolves device-level functions too. The loader puts a
// trampoline in front of them, which costs a dispatch we are not in a position to
// measure and keeps the rule intact.
struct VulkanLoader {
    VkInstance instance = VK_NULL_HANDLE;
};

PFN_vkVoidFunction LoadVulkanFunction(const char* name, void* userData) {
    const VulkanLoader* loader = static_cast<const VulkanLoader*>(userData);
    return vkGetInstanceProcAddr(loader->instance, name);
}

// Lives as long as ImGui does: the backend keeps the pointer it was handed.
VulkanLoader g_loader;

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
    case VK_FORMAT_R8G8B8A8_SRGB:  return "RGBA8 srgb";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8 unorm";
    case VK_FORMAT_B8G8R8A8_SRGB:  return "BGRA8 srgb";
    case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8 unorm";
    case VK_FORMAT_D32_SFLOAT:     return "D32 float";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case VK_FORMAT_UNDEFINED:      return "-";
    default:                       return "?";
    }
}

// One line about an image: what it is, how big, how many samples.
//
// The sample count is the interesting column. Three of these are 4x and the two that
// leave the frame are 1x, which is the whole shape of the resolve.
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
    ImGui::Text("         %s", d.fragPath != nullptr ? d.fragPath : "-");
}

// One row per set a pipeline declares, and one line per binding in it.
//
// An empty set is printed too. Vulkan numbers sets by position, so set 1 cannot exist
// without a set 0 in front of it, and a layout with no bindings is how that is said.
void ShowSetLayouts(const char* name, const Pipeline& pipeline) noexcept {
    for (uint32_t set = 0; set < kMaxSets; ++set) {
        const DescriptorLayout& layout = pipeline.setLayouts[set];
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

// ImGui reports failures through a callback rather than a return value, because most
// of its calls are inside its own recording. Ours only says so -- there is nothing to
// unwind from a panel.
void OnVulkanResult(VkResult result) noexcept {
    if (result != VK_SUCCESS) { LOG("[gui] vulkan call failed (%d)\n", result); }
}

// One texture at a time is all the panel ever binds: its font atlas. The pool is sized
// for a handful anyway, because ImGui_ImplVulkan_AddTexture exists and someone will
// want to look at a render target through it.
constexpr uint32_t kPoolSets = 8;

}   // namespace

bool CreateGui(const VulkanInstance& inst, const VulkanDevice& dev,
               Window& window, VkFormat targetFormat, Gui* out) noexcept {
    out->dev = &dev;

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPoolSets};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    // The one flag our own pool refuses. ImGui frees sets when a texture goes away.
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = kPoolSets;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (dev.table.vkCreateDescriptorPool(dev.handle, &poolInfo, nullptr, &out->pool)
            != VK_SUCCESS) {
        LOG("[gui] vkCreateDescriptorPool failed\n");
        return false;
    }

    // Before Init, and before anything else the backend does: it has no prototypes
    // to fall back on.
    g_loader.instance = inst.handle;
    if (!ImGui_ImplVulkan_LoadFunctions(LoadVulkanFunction, &g_loader)) {
        LOG("[gui] ImGui_ImplVulkan_LoadFunctions failed" "\n");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // The default font is 13px, which is a third of the height of the text in the
    // console beside it. Scaling the built-in atlas is blurry at large factors but
    // costs no font file; a real one goes in when the panel needs to be read rather
    // than glanced at.
    //
    // ScaleAllSizes too, or the boxes and padding stay 13px-sized around 20px text.
    constexpr float kUiScale = 1.6f;
    ImGui::GetIO().FontGlobalScale = kUiScale;
    ImGui::GetStyle().ScaleAllSizes(kUiScale);

    // No .ini file. It would remember window positions across runs, which makes two
    // runs of the same build differ -- the opposite of what the capture tool needs.
    ImGui::GetIO().IniFilename = nullptr;

    if (!ImGui_ImplGlfw_InitForVulkan(window.handle, true)) {
        LOG("[gui] ImGui_ImplGlfw_InitForVulkan failed\n");
        return false;
    }
    out->started = true;

    // Dynamic rendering, so no VkRenderPass: the format is handed over directly and
    // has to be the one the pass below actually begins with.
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &targetFormat;

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = inst.handle;
    info.PhysicalDevice = dev.gpu;
    info.Device = dev.handle;
    info.QueueFamily = dev.families.graphics;
    info.Queue = dev.queues.graphics;
    info.DescriptorPool = out->pool;
    // Buffering counts, not a promise about our swapchain: the backend uses them to
    // size its own vertex buffers. The real image count is not known here -- the
    // swapchain is not built until the first frame.
    info.MinImageCount = 2;
    info.ImageCount = kDesiredSwapchainImages;
    // 1, not the scene's 4x. The panel is drawn after the resolve, onto the swapchain.
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = rendering;
    info.CheckVkResultFn = OnVulkanResult;

    if (!ImGui_ImplVulkan_Init(&info)) {
        LOG("[gui] ImGui_ImplVulkan_Init failed\n");
        return false;
    }
    return true;
}

Gui::~Gui() {
    if (dev == nullptr) { return; }
    // Backends first, then the context, then what we made. ImGui destroys its own
    // pipeline and font image in the Vulkan shutdown, and both read the device -- so
    // main's vkDeviceWaitIdle has to have run, and it has (it is above every
    // destructor).
    if (started) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    if (pool != VK_NULL_HANDLE) {
        dev->table.vkDestroyDescriptorPool(dev->handle, pool, nullptr);
    }
}

void BuildGui(ViewOptions* options, const GuiFrameInfo& info) noexcept {
    ImGui_ImplVulkan_NewFrame();
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

    // What the shader interface actually is
    //
    // Every number here was decided somewhere else and then became unreadable: a set
    // layout is opaque once created, a pool forgets its sizes, and a push range lives
    // in the .spv. This is the only place they are all visible at once.
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

        if (info.scenePipeline != nullptr) { ShowSetLayouts("scene", *info.scenePipeline); }
        if (info.presentPipeline != nullptr) { ShowSetLayouts("present", *info.presentPipeline); }
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

void RecordGuiPass(const FrameSlot& slot, const Texture& target) noexcept {
    ImDrawData* draws = ImGui::GetDrawData();
    if (draws == nullptr) { return; }

    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;

    // LOAD, unlike the two passes before it: this one draws on top of a finished
    // picture rather than replacing it. No barrier either -- the post-process pass
    // left the image COLOR_ATTACHMENT_OPTIMAL, which is what this needs.
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.image.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = target.desc.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vk.vkCmdBeginRendering(cmd, &rendering);
    // No viewport or scissor set here: the backend sets its own from the draw data,
    // which is in the window's pixels and would not survive our sign convention.
    ImGui_ImplVulkan_RenderDrawData(draws, cmd);
    vk.vkCmdEndRendering(cmd);
}
