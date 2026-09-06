#include "Passes.h"

#include "Gui.h"
#include "GeometryPass.h"
#include "LightingPass.h"
#include "ShadowPass.h"
#include "PostProcessPass.h"
#include "ScenePass.h"
#include "Vulkan/Barrier.h"
#include "Vulkan/Mesh.h"

#include <cstring>    // memcpy
#include <vector>     // one handle per material, counted at load time
#include <iterator>   // std::size

#include <glm/common.hpp>                   // abs
#include <glm/matrix.hpp>                   // inverse, transpose
#include <glm/ext/matrix_clip_space.hpp>    // perspective, ortho
#include <glm/ext/matrix_transform.hpp>     // lookAt
#include <glm/trigonometric.hpp>            // radians

bool CreateMaterials(const VulkanDevice& dev,
                     const Descriptors& descriptors, const DescriptorLayout& layout,
                     const MaterialDesc* sources, uint32_t count,
                     Material* out) noexcept {
    if (count == 0) { return true; }

    // One call, because the pool hands sets out in batches and a per-material call
    // would ask it 25 times for the same layout.
    std::vector<VkDescriptorSet> sets(count);
    if (!AllocateSets(descriptors, layout, count, sets.data())) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        out[i].set = sets[i];
        out[i].cullMode = sources[i].cullMode;   // copied, not bound: it is not a binding

        // 32 bytes, written once and never again -- but HOST_VISIBLE like the scene's
        // uniform rather than a staging copy, because a device-local upload for 32
        // bytes costs a command buffer and a queue wait each.
        if (!CreateBuffer(dev, sizeof(MaterialParams),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &out[i].params)) {
            return false;
        }
        if (out[i].params.mapped == nullptr) {
            LOG("[vk] material uniform buffer is not mapped\n");
            return false;
        }
        std::memcpy(out[i].params.mapped, &sources[i].params, sizeof(MaterialParams));

        // Binding order, and the order is MaterialSet()'s -- the same declaration every
        // program that reads a material is checked against. The static_assert is what
        // keeps the two from drifting: a binding added there without a value here is a
        // set with a hole in it, which UpdateSet would fill from the wrong slot.
        const BindingValue values[] = {
            {&sources[i].baseColor->view},           // 0 baseColor
            {&sources[i].normal->view},              // 1 normalMap
            {nullptr, &out[i].params},               // 2 mtl
            {&sources[i].metallicRoughness->view},   // 3 metallicRoughnessMap
        };
        static_assert(std::size(values) == 4,
                      "one value per binding MaterialSet() declares");
        UpdateSet(descriptors, layout, out[i].set,
                  values, static_cast<uint32_t>(std::size(values)));
    }
    return true;
}

glm::mat4 ViewFromPose(const Pose& pose) noexcept {
    // The inverse of T(p) * R(q), which is R(q^-1) * T(-p). True because the pose is
    // rigid; a scale in here and this line would be wrong rather than incomplete.
    const glm::quat back = glm::conjugate(pose.orientation);

    glm::mat4 out = glm::mat4_cast(back);
    out[3] = glm::vec4{back * -pose.position, 1.0f};
    return out;
}

glm::mat4 ModelFromTransform(const Transform& transform) noexcept {
    // T * R * S expanded: R's columns carry the scale, and the translation is written
    // into the fourth. Three matrix multiplies would land on the same numbers.
    glm::mat4 out = glm::mat4_cast(transform.orientation);
    out[0] *= transform.scale.x;
    out[1] *= transform.scale.y;
    out[2] *= transform.scale.z;
    out[3] = glm::vec4{transform.position, 1.0f};
    return out;
}

glm::mat4 ProjectionFor(float fovDegrees, const TextureDesc& target) noexcept {
    // The aspect is the target's, which is the whole reason this takes one: a
    // projection answers to the shape of the image it lands on.
    const float aspect = static_cast<float>(target.extent.width)
                       / static_cast<float>(target.extent.height);

    // No proj[1][1] *= -1: the viewport height is already negative.
    // Depth lands in [0,1] thanks to GLM_FORCE_DEPTH_ZERO_TO_ONE on the CMake target.
    return glm::perspective(glm::radians(fovDegrees), aspect, kNearPlane, kFarPlane);
}

glm::mat4 ShadowView(const glm::vec3& direction, const glm::vec3& sceneCenter) noexcept {
    // direction runs from a surface toward the light, so the eye is the centre plus it.
    //
    // Chosen and not assumed: lookAt builds a basis by crossing the forward with the
    // up, and that collapses when the two are parallel -- a sun directly overhead.
    const glm::vec3 up = glm::abs(direction.y) > 0.99f
                       ? glm::vec3{0.0f, 0.0f, 1.0f} : kWorldUp;

    return glm::lookAt(sceneCenter + direction * kShadowDistance, sceneCenter, up);
}

glm::mat4 ShadowProjectionFor(const TextureDesc& map) noexcept {
    // Orthographic because the light is directional: parallel rays have no eye point to
    // project from, only a box, and the box decides how much world one texel covers.
    //
    // The aspect comes from the map the way the camera's comes from its target, which
    // is what lets the map stop being square without anything else knowing.
    const float aspect = static_cast<float>(map.extent.width)
                       / static_cast<float>(map.extent.height);

    // 0.1 rather than 0: an ortho box with a zero near plane is legal and wastes half
    // its depth range on space behind the light.
    return glm::ortho(-kShadowRadius * aspect, kShadowRadius * aspect,
                      -kShadowRadius, kShadowRadius, 0.1f, kShadowDistance * 2.0f);
}

bool RenderTargetCapabilities(const VulkanInstance& inst, VkPhysicalDevice gpu,
                              TargetCapabilities* out) noexcept {
    return QueryTargetCapabilities(inst, gpu, DepthTargetUsage(), kDesiredSampleCount,
                                   out);
}

void DescribeSizedTargets(VkExtent2D windowExtent, const TargetCapabilities& caps,
                          SceneTargetDescs* scene, GBufferTargetDescs* gbuffer) noexcept {
    const VkExtent2D extent = RenderExtentFor(windowExtent);
    *scene = MakeSceneTargets(extent, kRenderColorFormat, caps);
    *gbuffer = MakeGBufferTargets(extent, kRenderColorFormat, caps);
}

VkExtent2D RenderExtentFor(VkExtent2D windowExtent) noexcept {
    // The policy decides whether to look at the argument at all. Following the window
    // is what an editor does; a fixed size is what a client usually ships, and the
    // panel switches between the two.
    return kRenderFollowsWindow ? windowExtent
                                : VkExtent2D{kRenderWidth, kRenderHeight};
}

// HOST_VISIBLE + MAPPED, like every uniform here: one memcpy a frame, so a staging
// buffer and a copy command would buy nothing.
//
// Three functions and not one taking a size: the only thing they share is the four
// flags, and those are the same for every uniform in this program.
bool CreateFrameCameras(const VulkanDevice& dev, FrameCamera* out) noexcept {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateBuffer(dev, sizeof(CameraUniform),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &out[i].buffer)) {
            return false;
        }
        if (out[i].buffer.mapped == nullptr) {
            LOG("[vk] camera uniform buffer is not mapped\n");
            return false;
        }
    }
    return true;
}

bool CreateFrameViewOptions(const VulkanDevice& dev, FrameViewOptions* out) noexcept {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateBuffer(dev, sizeof(ViewOptionsUniform),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &out[i].buffer)) {
            return false;
        }
        if (out[i].buffer.mapped == nullptr) {
            LOG("[vk] view options uniform buffer is not mapped\n");
            return false;
        }
    }
    return true;
}

bool CreateFrameLights(const VulkanDevice& dev, FrameLight* out) noexcept {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateBuffer(dev, sizeof(LightUniform),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &out[i].buffer)) {
            return false;
        }
        if (out[i].buffer.mapped == nullptr) {
            LOG("[vk] light uniform buffer is not mapped\n");
            return false;
        }
    }
    return true;
}

bool CreateFrameShadows(const VulkanDevice& dev, FrameShadow* out) noexcept {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateBuffer(dev, sizeof(ShadowUniform),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &out[i].buffer)) {
            return false;
        }
        if (out[i].buffer.mapped == nullptr) {
            LOG("[vk] shadow uniform buffer is not mapped\n");
            return false;
        }
    }
    return true;
}

VkImageUsageFlags DepthTargetUsage() noexcept {
    // The arguments do not reach usage, so any values will do. Calling the real
    // functions is the point -- a hand-written union here would drift the day one of
    // them changes what it asks for.
    return MakeSceneTargets(VkExtent2D{}, VK_FORMAT_UNDEFINED,
                            TargetCapabilities{}).depth.usage
         | MakeShadowTarget(TargetCapabilities{}).usage;
}

void SetDrawTransform(DrawItem* item, const Transform& transform) noexcept {
    item->model = ModelFromTransform(transform);

    // The inverse-transpose of the upper 3x3. For a rotation it is the same matrix,
    // and for a uniform scale it differs only in length -- which the fragment stage
    // normalizes away. It earns its place the moment a scale is not uniform, and
    // nothing here would have said so.
    const glm::mat3 normal = glm::transpose(glm::inverse(glm::mat3(item->model)));
    item->normal[0] = glm::vec4{normal[0], 0.0f};
    item->normal[1] = glm::vec4{normal[1], 0.0f};
    item->normal[2] = glm::vec4{normal[2], 0.0f};
}

void UploadFrameValues(const FrameSlot& slot,
                       const FrameCamera* cameras, const FrameLight* lights,
                       const FrameShadow* shadows, FrameViewOptions* views,
                       const Gui& gui) noexcept {
    // Every uniform a frame writes, in the order the passes read them. Four memcpys
    // and one shape -- the panel's switches used to be a call into Gui here, which is
    // what having the buffer on the other side of that boundary cost.
    //
    // The panel's are among them even though the gui pass runs last: what reads them
    // is the scene pass, two passes earlier in the same submission. That edge runs
    // backwards through the frame and is the reason this value is asked for rather
    // than assigned from outside like the other three.
    FrameViewOptions& view = views[slot.index];
    view.value = GuiViewUniform(gui);
    std::memcpy(view.buffer.mapped, &view.value, sizeof(view.value));

    const FrameCamera& camera = cameras[slot.index];
    std::memcpy(camera.buffer.mapped, &camera.value, sizeof(camera.value));

    const FrameLight& light = lights[slot.index];
    std::memcpy(light.buffer.mapped, &light.value, sizeof(light.value));

    const FrameShadow& shadow = shadows[slot.index];
    std::memcpy(shadow.buffer.mapped, &shadow.value, sizeof(shadow.value));
}

bool RecordFrame(const FrameSlot& slot,
                 const ShadowPass& shadow, const ScenePass& scene,
                 const GeometryPass& geometry, const LightingPass& lighting,
                 const PostProcessPass& post, Gui& gui, const Texture& target,
                 const DrawList& draws, DrawStats* stats) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    // The pool has RESET_COMMAND_BUFFER_BIT, so one buffer can rewind on its own.
    if (vk.vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) {
        LOG("[vk] vkResetCommandBuffer failed\n");
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;   // recorded once
    if (vk.vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        LOG("[vk] vkBeginCommandBuffer failed\n");
        return false;
    }

    // The order is here, in these lines, and nowhere else. Both dependencies are
    // written somewhere -- the scene's set names the shadow map, post.source names the
    // images the scene resolves into -- and neither says anything about when. A set
    // naming a map cannot say the map was drawn this frame; that is what these lines
    // say, by being in this order.
    // Read once and handed to whichever middle runs. The six are the panel's answers
    // about drawing surfaces, and both middles draw the same surfaces -- so a switch
    // meaning one thing on one path and nothing on the other would be a hole in the
    // comparison rather than a saving.
    const RasterOptions raster{GuiPolygonMode(gui), GuiDepthTest(gui),
                               GuiDepthWrite(gui), GuiRasterizerDiscard(gui),
                               GuiCullMode(gui), GuiDepthCompare(gui)};

    RecordShadowPass(slot, shadow, draws);

    // The one branch in a frame. Everything either side of it is the same call with
    // the same arguments, which is the point: what deferred changes is here and
    // nowhere else in this function.
    if (GuiDeferred(gui)) {
        RecordGeometryPass(slot, geometry, draws, raster, stats);
        RecordLightingPass(slot, lighting);
    } else {
        RecordScenePass(slot, scene, draws, raster, stats);
    }

    RecordPostProcessPass(slot, post, target);

    // The third edge on this image, and the only one between two passes that both
    // write it. Here for the same reason the present transition below is: it is about
    // what runs either side of it, and neither side is allowed to know the other.
    //
    // Nothing moves -- both sides want COLOR_ATTACHMENT_OPTIMAL. What is missing
    // without it is the other two halves of a barrier, ordering and visibility: the
    // gui pass's loadOp LOAD reads what the post pass's storeOp wrote.
    //
    // dstAccess is both ways round because loadOp LOAD reads the image and the panel
    // then blends over it.
    //
    // Issued whether or not the panel draws. A pass that returns early leaves a
    // barrier that moves nothing between two writes that no longer collide.
    RecordLayoutTransition(vk, cmd, target.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT
                               | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    RecordGuiPass(slot, gui, target);

    // The frame leaves for the presentation engine here, after everything that draws
    // into it. This used to sit at the end of the post-process pass, which made that
    // pass assume its target was a swapchain image -- adding a second pass on top is
    // what forced it out.
    //
    // dstAccess is 0, unlike every other barrier here: present is not a queue
    // operation and reads nothing through the memory model, so there is no access to
    // make visible. The semaphore SubmitFrame signals is what present actually waits
    // on -- this barrier only has to leave the image in the right layout.
    RecordLayoutTransition(vk, cmd, target.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    if (vk.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        LOG("[vk] vkEndCommandBuffer failed\n");
        return false;
    }
    return true;
}
