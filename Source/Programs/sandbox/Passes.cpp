#include "Passes.h"

#include "Gui.h"
#include "GeometryPass.h"
#include "LightingPass.h"
#include "BloomPass.h"
#include "PointShadowPass.h"
#include "ShadowPass.h"
#include "Sky.h"
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

glm::mat4 ProjectionFor(float fovDegrees, VkExtent2D target) noexcept {
    // An extent and not the target's desc: a format or a sample count changing does
    // not change this answer, so taking one would claim a dependency there is not.
    const float aspect = static_cast<float>(target.width)
                       / static_cast<float>(target.height);

    // No proj[1][1] *= -1: the viewport height is already negative.
    // Depth lands in [0,1] thanks to GLM_FORCE_DEPTH_ZERO_TO_ONE on the CMake target.
    return glm::perspective(glm::radians(fovDegrees), aspect, kNearPlane, kFarPlane);
}

LightEntry EntryFor(const LightState& light) noexcept {
    LightEntry entry{};
    const bool positioned = light.kind != LightKind::Directional;

    // A point light has no axis, so nothing sets one -- and the shader normalizes this
    // vector for the cone term below. Normalizing a zero vector is not a number, and
    // that carries through the whole contribution: until this line existed, a point
    // light lit nothing at all and its shadow had nothing to fall on. The value is
    // arbitrary because the cone is open; what matters is that it is a direction.
    const glm::vec3 axis = light.kind == LightKind::Point ? glm::vec3{0.0f, -1.0f, 0.0f}
                                                         : light.direction;
    entry.direction = glm::vec4{axis, positioned ? 1.0f : 0.0f};
    // color.a says which kind of map this light has, not merely whether it has one:
    // 0 none, 1 a layer of the 2D array, 2 a cube. The two are sampled differently and
    // the shader has no other way to tell -- kind is not in this struct on purpose.
    const float mapKind = !light.castsShadow ? 0.0f
                        : (light.kind == LightKind::Point ? 2.0f : 1.0f);
    entry.color = glm::vec4{light.color, mapKind};
    entry.position = glm::vec4{light.position, light.range};

    // A point light accepts every direction, and that is an open cone rather than a flag
    // saying it has none -- so one expression serves both positioned kinds without
    // asking which it is.
    //
    // The outer edge is below -1 rather than equal to the inner one. smoothstep is
    // undefined when its two edges are the same, and an alignment is never below -1, so
    // putting the outer edge under that makes the window 1 everywhere by arithmetic
    // rather than by a special case.
    entry.cone = light.kind == LightKind::Point
               ? glm::vec4{-1.0f, -2.0f, 0.0f, 0.0f}
               : glm::vec4{light.innerCos, light.outerCos, 0.0f, 0.0f};
    return entry;
}

bool FillLights(const LightState lights[], uint32_t count, const glm::vec3& ambient,
                LightUniform* out) noexcept {
    if (count > kMaxLights) {
        LOG("[render] %u lights, and a frame carries %u\n", count, kMaxLights);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) { out->lights[i] = EntryFor(lights[i]); }
    out->ambient = glm::vec4{ambient, static_cast<float>(count)};
    return true;
}

glm::mat4 ShadowView(const LightState& light, const glm::vec3& sceneCenter) noexcept {
    // Chosen and not assumed: lookAt builds a basis by crossing the forward with the
    // up, and that collapses when the two are parallel -- a sun directly overhead.
    const glm::vec3 up = glm::abs(light.direction.y) > 0.99f
                       ? glm::vec3{0.0f, 0.0f, 1.0f} : kWorldUp;

    // A spot is somewhere, and looks along its own direction from there.
    //
    // A directional light is not anywhere. The eye put back along the direction is a
    // choice about how big a piece of the scene the map covers, not a fact about the
    // light -- which is the whole difference between the two kinds in one place.
    if (light.kind == LightKind::Spot) {
        return glm::lookAt(light.position, light.position - light.direction, up);
    }
    return glm::lookAt(sceneCenter + light.direction * kShadowDistance, sceneCenter, up);
}

glm::mat4 ShadowProjectionFor(const LightState& light, VkExtent2D map) noexcept {
    // The aspect comes from the map the way the camera's comes from its target.
    const float aspect = static_cast<float>(map.width)
                       / static_cast<float>(map.height);

    // **Orthographic against perspective is the same distinction one level down**:
    // parallel rays against rays that leave a point. The cone is the field of view,
    // doubled because outerCos is a half-angle, and clamped below pi so a very wide
    // spot does not ask for a projection with no far plane.
    if (light.kind == LightKind::Spot) {
        const float half = glm::acos(glm::clamp(light.outerCos, -1.0f, 1.0f));
        const float fov = glm::min(2.0f * half, 3.0f);
        return glm::perspective(fov, aspect, 0.1f, glm::max(light.range, 1.0f));
    }
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
    *gbuffer = MakeGBufferTargets(extent, kGBufferAlbedoFormat, caps);
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

glm::mat4 PointShadowFaceView(const glm::vec3& position, uint32_t face) noexcept {
    // Vulkan's cube order, +X -X +Y -Y +Z -Z, with the up vectors the convention asks
    // for rather than the world's. skybake.frag reads the same order at the other end.
    static const glm::vec3 kForward[6] = {
        { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
        { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
        { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
    };
    static const glm::vec3 kUp[6] = {
        { 0.0f, -1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
        { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
        { 0.0f, -1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
    };
    const uint32_t i = face < 6 ? face : 0;
    return glm::lookAt(position, position + kForward[i], kUp[i]);
}

glm::mat4 PointShadowProjection(float range) noexcept {
    // Square and 90 degrees, which is what makes six faces meet without a gap or an
    // overlap. far is the range because past it the light adds nothing.
    return glm::perspective(glm::radians(90.0f), 1.0f, 0.05f,
                            range > 0.05f ? range : 1.0f);
}

void FillPointShadows(const LightState lights[], uint32_t count,
                      PointShadowUniform* out) noexcept {
    *out = PointShadowUniform{};
    const uint32_t live = count < kMaxLights ? count : kMaxLights;
    for (uint32_t i = 0; i < live; ++i) {
        if (lights[i].kind != LightKind::Point) { continue; }

        const glm::mat4 proj = PointShadowProjection(lights[i].range);
        for (uint32_t face = 0; face < 6; ++face) {
            out->faceViewProj[i * 6 + face] =
                proj * PointShadowFaceView(lights[i].position, face);
        }
        out->lightPosRange[i] = glm::vec4{lights[i].position, lights[i].range};
    }
}

bool CreateFramePointShadows(const VulkanDevice& dev, FramePointShadow* out) noexcept {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateBuffer(dev, sizeof(PointShadowUniform),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &out[i].buffer)) {
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

Frustum FrustumFrom(const glm::mat4& viewProj) noexcept {
    // Rows of the matrix, which is what the planes are built from. GLM is column major,
    // so a row is one component of each column.
    const glm::vec4 rowX{viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]};
    const glm::vec4 rowY{viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]};
    const glm::vec4 rowZ{viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]};
    const glm::vec4 rowW{viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]};

    Frustum out;
    out.planes[0] = rowW + rowX;   // left
    out.planes[1] = rowW - rowX;   // right
    out.planes[2] = rowW + rowY;   // bottom
    out.planes[3] = rowW - rowY;   // top

    // 0 <= z <= w here, not -w <= z <= w: GLM_FORCE_DEPTH_ZERO_TO_ONE is on the target,
    // so the near plane is the z row itself.
    out.planes[4] = rowZ;          // near
    out.planes[5] = rowW - rowZ;   // far

    // Normalized so the distance a plane reports is in world units. Nothing here reads
    // that distance, but a plane of arbitrary length makes every later use wrong in a
    // way that looks like a tuning problem.
    for (glm::vec4& plane : out.planes) {
        const float length = glm::length(glm::vec3{plane});
        if (length > 0.0f) { plane /= length; }
    }
    return out;
}

bool IsVisible(const Frustum& frustum, const DrawItem& item) noexcept {
    // An asset that stated no bounds is drawn. The alternative is treating an empty box
    // as a point, which culls the whole primitive and looks like missing geometry.
    if (item.boundsMin.x > item.boundsMax.x) { return true; }

    // The object box, moved into world space as a box again. Transforming the centre and
    // then taking the absolute value of the matrix against the half-extent is the
    // standard shortcut: it is the same answer as transforming eight corners and taking
    // their bounds, at a fraction of the work.
    const glm::vec3 centre = (item.boundsMin + item.boundsMax) * 0.5f;
    const glm::vec3 half   = (item.boundsMax - item.boundsMin) * 0.5f;

    const glm::vec3 worldCentre = glm::vec3{item.model * glm::vec4{centre, 1.0f}};
    const glm::mat3 basis = glm::mat3(item.model);
    const glm::vec3 worldHalf{
        glm::abs(basis[0].x) * half.x + glm::abs(basis[1].x) * half.y
            + glm::abs(basis[2].x) * half.z,
        glm::abs(basis[0].y) * half.x + glm::abs(basis[1].y) * half.y
            + glm::abs(basis[2].y) * half.z,
        glm::abs(basis[0].z) * half.x + glm::abs(basis[1].z) * half.y
            + glm::abs(basis[2].z) * half.z};

    for (const glm::vec4& plane : frustum.planes) {
        const glm::vec3 normal{plane};

        // The corner furthest along the plane's normal. If even that one is behind the
        // plane, every corner is, and the box cannot be seen.
        const float reach = glm::abs(normal.x) * worldHalf.x
                          + glm::abs(normal.y) * worldHalf.y
                          + glm::abs(normal.z) * worldHalf.z;
        if (glm::dot(normal, worldCentre) + plane.w + reach < 0.0f) { return false; }
    }
    return true;
}

bool DeclareRead(const PassInput& input, const char* what, bool wantDepth,
                 RenderPassDesc* desc) noexcept {
    if (input.resource == nullptr) {
        LOG("[vk] the %s was declared as a read with no resource\n", what);
        return false;
    }

    // Every frame's image against the one thing they are all supposed to be. Nothing
    // else would notice a frame wired to the wrong texture: the descriptor write takes
    // whatever view it is handed, and the graph would then be about a resource one of
    // the frames is not.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (input.frames[i] == nullptr) {
            LOG("[vk] the %s has no image for frame %u\n", what, i);
            return false;
        }
        const TextureDesc& got = input.frames[i]->desc;
        if (!SameTextureDesc(got, *input.resource)) {
            LOG("[vk] the %s's frame %u describes something else than the resource it"
                " is declared as (format %d/%d)\n", what, i,
                static_cast<int>(got.format), static_cast<int>(input.resource->format));
            return false;
        }
    }

    if (!CheckSampledInput(*input.resource, what, wantDepth)) { return false; }

    // Once. A pass may read one image through two bindings, which is legal and would
    // otherwise be two edges where there is one.
    uint32_t count = 0;
    while (count < kMaxReads && desc->reads[count] != nullptr) {
        if (desc->reads[count] == input.resource) { return true; }
        count += 1;
    }
    if (count == kMaxReads) {
        LOG("[vk] a pass reads more than %u resources\n", kMaxReads);
        return false;
    }
    desc->reads[count] = input.resource;
    return true;
}

bool FillFrameSet(const Descriptors& descriptors, const ShaderProgram& program,
                  VkDescriptorSet set, const FrameSetSources& sources, uint32_t frame,
                  RenderPassDesc* desc) noexcept {
    const DescriptorLayout& layout = program.setLayouts[kFrameSet];

    // Every binding of the set, whether or not this program reads it. UpdateSet skips
    // the ones reflection left as holes, so a program that reads two of the five is
    // written the same way as one that reads all of them.
    const BindingValue values[] = {
        {nullptr, &sources.cameras[frame].buffer},
        {nullptr, &sources.lights[frame].buffer},
        {nullptr, &sources.shadows[frame].buffer},
        {&sources.shadowMap.frames[frame]->view},
        {nullptr, &sources.views[frame].buffer},
        {&sources.skyCube->view},
        {&sources.irradianceCube->view},
        {&sources.prefilteredCube->view},
        {&sources.brdfLut->view},
        {&sources.pointShadowMap.frames[frame]->view},
    };
    UpdateSet(descriptors, layout, set, values, static_cast<uint32_t>(std::size(values)));

    // The one binding here that another pass produces. Declared only when this program
    // reads it -- a hole means the shader never mentioned it, which is the shader's
    // answer to the question rather than one made here.
    constexpr uint32_t kShadowMapBinding = 3;
    if (kShadowMapBinding < layout.bindingCount
            && layout.types[kShadowMapBinding] != 0) {
        if (!DeclareRead(sources.shadowMap, "shadow map", /*wantDepth*/ true, desc)) {
            return false;
        }
    }

    // The same question for the cubes. Colour rather than depth: what is stored there is
    // a distance this renderer computed, not something the depth test produced.
    constexpr uint32_t kPointShadowBinding = 9;
    if (kPointShadowBinding < layout.bindingCount
            && layout.types[kPointShadowBinding] != 0) {
        return DeclareRead(sources.pointShadowMap, "point shadow cubes",
                           /*wantDepth*/ false, desc);
    }
    return true;
}

void UploadFrameValues(const FrameSlot& slot,
                       const FrameCamera* cameras, const FrameLight* lights,
                       const FrameShadow* shadows, const FramePointShadow* pointShadows,
                       FrameViewOptions* views, const Gui& gui) noexcept {
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

    const FramePointShadow& point = pointShadows[slot.index];
    std::memcpy(point.buffer.mapped, &point.value, sizeof(point.value));
}

// Output: whether the order these passes are recorded in agrees with what they declared
//
// **The order stays where it is -- in the lines below -- and this reads it.** Nothing
// here schedules anything; it walks the chain the recorder is about to walk and asks
// whether each pass can have what it says it needs by the time it runs.
//
// Three questions, and all three were unanswerable until a pass declared its reads:
//
//   a read with no earlier producer          nothing wrote what this samples
//   loadOp LOAD with no earlier producer     loading what nobody put there
//   a stored output nothing reads            written for no one
//
// The last is exempt for the final pass, whose output is the frame itself.
//
// Frame-independent: every input is a declaration, and none of it changes between
// frames. It lives here because which passes make up a path is this function's branch
// and nowhere else -- writing the chain again outside would be a second copy of the
// one thing RecordFrame owns. The day that membership becomes data, this moves out.
static bool CheckPassOrder(const RenderPassDesc* const passes[],
                           uint32_t count) noexcept {
    const TextureDesc* produced[kMaxAttachments * 8]{};
    uint32_t producedCount = 0;
    bool ok = true;

    const auto wasProduced = [&](const TextureDesc* r) {
        for (uint32_t k = 0; k < producedCount; ++k) {
            if (produced[k] == r) { return true; }
        }
        return false;
    };
    const auto produce = [&](const TextureDesc* r) {
        if (r == nullptr || wasProduced(r)) { return; }
        if (producedCount < static_cast<uint32_t>(std::size(produced))) {
            produced[producedCount++] = r;
        }
    };

    for (uint32_t p = 0; p < count; ++p) {
        const RenderPassDesc& pass = *passes[p];

        for (uint32_t i = 0; i < kMaxReads && pass.reads[i] != nullptr; ++i) {
            if (!wasProduced(pass.reads[i])) {
                LOG("[render] pass %u reads something no earlier pass produced\n", p);
                ok = false;
            }
        }
        for (uint32_t i = 0; i < kMaxAttachments
                             && pass.attachments[i].resource != nullptr; ++i) {
            const Attachment& a = pass.attachments[i];
            const TextureDesc* loaded = a.whole != nullptr ? a.whole : a.resource;
            if (a.load == VK_ATTACHMENT_LOAD_OP_LOAD && !wasProduced(loaded)) {
                LOG("[render] pass %u loads attachment %u, which no earlier pass"
                    " produced\n", p, i);
                ok = false;
            }
        }

        for (uint32_t i = 0; i < kMaxAttachments
                             && pass.attachments[i].resource != nullptr; ++i) {
            const Attachment& a = pass.attachments[i];
            // What a later pass could read, which for a slice is the thing it is a
            // slice of: nothing samples one layer of a shadow array, it samples the
            // array and picks the layer.
            if (a.store == VK_ATTACHMENT_STORE_OP_STORE) {
                produce(a.whole != nullptr ? a.whole : a.resource);
            }
            produce(a.resolve.target);
        }
    }

    // What the last pass leaves is the frame, so only what earlier passes stored has
    // to find a reader.
    for (uint32_t p = 0; p + 1 < count; ++p) {
        const RenderPassDesc& pass = *passes[p];
        for (uint32_t i = 0; i < kMaxAttachments
                             && pass.attachments[i].resource != nullptr; ++i) {
            const Attachment& a = pass.attachments[i];
            const TextureDesc* stored = a.whole != nullptr ? a.whole : a.resource;
            const TextureDesc* out = a.resolve.target != nullptr
                                   ? a.resolve.target
                                   : (a.store == VK_ATTACHMENT_STORE_OP_STORE
                                          ? stored : nullptr);
            if (out == nullptr) { continue; }

            bool read = false;
            for (uint32_t q = p + 1; q < count && !read; ++q) {
                for (uint32_t r = 0; r < kMaxReads && passes[q]->reads[r] != nullptr; ++r) {
                    if (passes[q]->reads[r] == out) { read = true; break; }
                }
                for (uint32_t a2 = 0; a2 < kMaxAttachments && !read
                                      && passes[q]->attachments[a2].resource != nullptr;
                     ++a2) {
                    if (passes[q]->attachments[a2].resource == out
                            && passes[q]->attachments[a2].load
                                   == VK_ATTACHMENT_LOAD_OP_LOAD) {
                        read = true;
                    }
                }
            }
            if (!read) {
                LOG("[render] pass %u stores attachment %u and nothing after it reads"
                    " that\n", p, i);
                ok = false;
            }
        }
    }
    return ok;
}

const char* TimedPassName(TimedPass pass) noexcept {
    switch (pass) {
        case TimedPass::Shadow:   return "shadow";
        case TimedPass::PointShadow: return "point shadow";
        case TimedPass::Sky:      return "sky";
        case TimedPass::Scene:    return "scene";
        case TimedPass::Geometry: return "geometry";
        case TimedPass::Lighting: return "lighting";
        case TimedPass::Bloom:    return "bloom";
        case TimedPass::Post:     return "post";
        case TimedPass::Gui:      return "gui";
        default:                  return "?";
    }
}

void ReadPassTimings(const GpuTimer& timer, PassTimings* out) noexcept {
    *out = PassTimings{};
    for (uint32_t i = 0; i < kTimedPassCount; ++i) {
        uint64_t begin = 0;
        uint64_t end = 0;

        // Both or neither. A pair with one half missing is a pass that was interrupted
        // between its two writes, which nothing here does, so it is treated as absent
        // rather than reported as a number from two different frames.
        if (!ReadGpuTimestamp(timer, i * 2, &begin)
                || !ReadGpuTimestamp(timer, i * 2 + 1, &end)) {
            continue;
        }
        out->ran[i] = true;
        out->ms[i] = GpuMillis(timer, begin, end);
        out->totalMs += out->ms[i];
    }
}

bool RecordFrame(const FrameSlot& slot,
                 const ShadowPass& shadow, uint32_t shadowCasters,
                 const PointShadowPass& pointShadow,
                 const uint32_t* pointLights, uint32_t pointLightCount,
                 const SkyPass& skyForward, const SkyPass& skyDeferred,
                 const ScenePass& scene,
                 const GeometryPass& geometry, const LightingPass& lighting,
                 const BloomPass& bloom,
                 const PostProcessPass& post, Gui& gui, const Texture& target,
                 const DrawList& draws, const DrawList& shadowDraws,
                 const GpuTimer& timer, DrawStats* stats) noexcept {
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

    // Every query, and only once the buffer is open. The pool belongs to this slot and
    // its fence was waited on, so nothing is reading the old values any more.
    ResetGpuTimer(timer, cmd);

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

    // The chain, named once and used twice: walked below, and checked once per path
    // against what the passes declared.
    const bool deferred = GuiDeferred(gui);
    const RenderPassDesc* const forwardChain[] = {
        &shadow.pass, &pointShadow.pass, &skyForward.pass, &scene.pass,
        &bloom.extract, &bloom.blurH, &bloom.blurV, &post.pass, &gui.pass};
    const RenderPassDesc* const deferredChain[] = {
        &shadow.pass, &pointShadow.pass, &skyDeferred.pass, &geometry.pass,
        &lighting.pass, &bloom.extract, &bloom.blurH, &bloom.blurV, &post.pass,
        &gui.pass};

    static bool checkedForward = false;
    static bool checkedDeferred = false;
    if (deferred && !checkedDeferred) {
        checkedDeferred = true;
        if (!CheckPassOrder(deferredChain,
                            static_cast<uint32_t>(std::size(deferredChain)))) {
            return false;
        }
    } else if (!deferred && !checkedForward) {
        checkedForward = true;
        if (!CheckPassOrder(forwardChain,
                            static_cast<uint32_t>(std::size(forwardChain)))) {
            return false;
        }
    }

    // A pair per pass, at fixed indices, both written at ALL_COMMANDS.
    //
    // TOP_OF_PIPE for the opening stamp reads as the moment the command was reached,
    // which on a queue that is still working through earlier passes is well before this
    // pass starts. The interval then includes the tail of whatever came before it, and
    // measured that way the deferred post pass reported 0.666 ms of work it did not do.
    // Waiting for everything before it makes the two stamps bracket this pass alone.
    const auto timeBegin = [&](TimedPass pass) {
        WriteGpuTimestamp(timer, cmd, static_cast<uint32_t>(pass) * 2,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    };
    const auto timeEnd = [&](TimedPass pass) {
        WriteGpuTimestamp(timer, cmd, static_cast<uint32_t>(pass) * 2 + 1,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    };

    timeBegin(TimedPass::Shadow);
    RecordShadowPass(slot, shadow, shadowDraws, shadowCasters);
    timeEnd(TimedPass::Shadow);

    timeBegin(TimedPass::PointShadow);
    RecordPointShadowPass(slot, pointShadow, shadowDraws, pointLights, pointLightCount);
    timeEnd(TimedPass::PointShadow);

    // The one branch in a frame. Everything either side of it is the same call with
    // the same arguments, which is the point: what deferred changes is here and
    // nowhere else in this function.
    if (deferred) {
        timeBegin(TimedPass::Sky);
        RecordSkyPass(slot, skyDeferred);
        timeEnd(TimedPass::Sky);

        timeBegin(TimedPass::Geometry);
        RecordGeometryPass(slot, geometry, draws, raster, stats);
        timeEnd(TimedPass::Geometry);

        timeBegin(TimedPass::Lighting);
        RecordLightingPass(slot, lighting);
        timeEnd(TimedPass::Lighting);
    } else {
        timeBegin(TimedPass::Sky);
        RecordSkyPass(slot, skyForward);
        timeEnd(TimedPass::Sky);

        timeBegin(TimedPass::Scene);
        RecordScenePass(slot, scene, draws, raster, stats);
        timeEnd(TimedPass::Scene);
    }

    timeBegin(TimedPass::Bloom);
    RecordBloomPass(slot, bloom);
    timeEnd(TimedPass::Bloom);

    timeBegin(TimedPass::Post);
    RecordPostProcessPass(slot, post, target);
    timeEnd(TimedPass::Post);

    // The third edge on this image, and the only one between two passes that both
    // write it. Here for the same reason the present transition below is: it is about
    // what runs either side of it, and neither side is allowed to know the other.
    //
    // The edge from the post pass to the gui pass, which is the one BeginPass leaves
    // to the writer because a loadOp of LOAD reads something that call cannot see. The
    // role is what it takes: both passes draw into this as a colour attachment, and
    // ordering and visibility follow from that.
    //
    // Issued whether or not the panel draws. A pass that returns early leaves a barrier
    // between two writes that no longer collide, which costs nothing.
    RecordLoadHandover(vk, cmd, target, AttachmentRole::Color);

    timeBegin(TimedPass::Gui);
    RecordGuiPass(slot, gui, target);
    timeEnd(TimedPass::Gui);

    // The frame leaves for the presentation engine here, after everything that draws
    // into it. This used to sit at the end of the post-process pass, which made that
    // pass assume its target was a swapchain image -- adding a second pass on top is
    // what forced it out.
    //
    // dstAccess is 0, unlike every other barrier here: present is not a queue
    // operation and reads nothing through the memory model, so there is no access to
    // make visible. The semaphore SubmitFrame signals is what present actually waits
    // on -- this barrier only has to leave the image in the right layout.
    RecordLayoutTransition(vk, cmd, target.image.handle, WholeImage(VK_IMAGE_ASPECT_COLOR_BIT),
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
