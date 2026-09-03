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

void BuildGui(ViewOptions* options, float frameSeconds,
              uint32_t drawCount, uint32_t materialCount) noexcept {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("View")) {
        ImGui::Checkbox("normal map", &options->normalMap);
        ImGui::Checkbox("base colour", &options->baseColor);
        ImGui::Checkbox("specular", &options->specular);
        ImGui::Checkbox("alpha mask", &options->alphaMask);

        ImGui::Separator();
        // Both numbers, because they answer different questions: the rate is what a
        // person reads, the milliseconds are what a change moves. A guard on the
        // first frame, where the gap is zero.
        const float fps = frameSeconds > 0.0f ? 1.0f / frameSeconds : 0.0f;
        ImGui::Text("fps    %.0f  (%.2f ms)", fps, frameSeconds * 1000.0f);
        ImGui::Text("draws  %u", drawCount);
        ImGui::Text("mats   %u", materialCount);
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
