#pragma once

#include "Vulkan/Attachments.h"
#include "Vulkan/Device.h"
#include "Vulkan/Shader.h"

#include <glm/glm.hpp>   // PushConstants holds a mat4

// ViewportY - one sign that decides two things
// ============================================================================
//
// A negative viewport height makes the shader side y-up, and the same negation
// flips the winding test. So the sign also chooses frontFace; written by hand in
// two places they disagree silently until culling is turned on.
//
// Contract: both shaders emit triangles with positive shoelace area in clip space.
//           Measured, not derived - both pipelines run CULL_MODE_BACK.
enum class ViewportY {
    Down,   // Vulkan default, positive height
    Up,     // negative height - our world is y-up
};

// Output: the frontFace that makes the Contract triangles front-facing
constexpr VkFrontFace FrontFaceFor(ViewportY y) noexcept {
    return y == ViewportY::Up ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                              : VK_FRONT_FACE_CLOCKWISE;
}

// Output: a viewport with the sign applied. The caller never writes the sign, so it
//         cannot disagree with the frontFace baked into the pipeline.
VkViewport MakeViewport(VkExtent2D extent, ViewportY y) noexcept;

// Blending - one value, because blend and depth write cannot disagree
// ============================================================================
//
// A translucent surface with depth write on hides what is drawn behind it later.
// Two flags would make that state expressible. The cost: ordering moves to the
// recording side. Depth test stays on either way.
enum class Blending {
    Opaque,        // blend off - depth write on
    Translucent,   // blend on  - depth write off
};

//
// Appending never moves an earlier offset, so a new field cannot disturb a shader.
//
// Contract: a field is not an attribute. VertexInput() declares only what
//           the shader reads -- the layer warns about any extra. tangent has none.
struct Vertex {
    float position[3];   // location 0
    float normal[3];     // location 1
    float uv[2];         // location 2
    float tangent[4];    // no attribute yet
};

// One pipeline's worth of decisions. Everything not here is the same in both of
// ours and lives in CreateGraphicsPipeline. The rows are where each value comes
// from, which is the whole reason this is a struct and not five arguments:
//
//   the shader requires   vertPath . fragPath . vertexInput
//   the pass decides      viewportY . cullMode
//   the caller chooses    polygonMode . blending
//   passed through        colorFormat . depthFormat . samples
//
// The last row decides nothing: it carries values from Attachments so both sides of
// a baked-in contract read the same one.
struct GraphicsPipelineDesc {
    const char* vertPath = nullptr;
    const char* fragPath = nullptr;

    // nullptr means no vertex buffer - the shader builds its points from
    // gl_VertexIndex.
    const VkPipelineVertexInputStateCreateInfo* vertexInput = nullptr;

    // The same values the attachments were made from -- dynamic rendering bakes them
    // in, so a mismatch is caught at vkCmdBeginRendering. depth UNDEFINED = no depth.
    AttachmentFormats formats;

    ViewportY viewportY = ViewportY::Down;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;

    // FILL is the only value anything passes right now. LINE needs the device's
    // fillModeNonSolid, which we stopped requesting -- switching to it means adding
    // that back in Core.h and Device.cpp's candidate check.
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;

    Blending blending = Blending::Opaque;
};

// Dynamic rendering bakes the attachment formats in. Size is not baked - viewport
// and scissor are dynamic state, so a resize rebuilds nothing.
struct Pipeline {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline handle = VK_NULL_HANDLE;

    // Read out of the shaders, like the push range. It outlives a rebuild: the sets
    // already allocated from it stay valid only while it does.
    DescriptorLayout setLayout;

    // What it was built from. Recording reads viewportY out of it, and a rebuild
    // needs the rest -- without this the caller would have to keep the desc alive.
    GraphicsPipelineDesc desc;

    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};

// Effect: destroys pipeline and layout, leaves the struct empty. setLayout is not
//         touched -- only the destructor frees that. The destructor calls this; main
//         calls it directly to rebuild in place when the surface format changes.
//
// Contract: every command buffer using this pipeline must have finished, so the
//           caller calls vkDeviceWaitIdle - one frame's fence is not enough.
void DestroyPipeline(const VulkanDevice& dev, Pipeline* pipeline) noexcept;


// Rides inside the command buffer: no pool, no set, no lifetime. At least 128 bytes
// are guaranteed, which is why the three matrices are multiplied on the CPU - sent
// apart they would be 192. Lighting that wants world space splits model back out.
//
// Contract: field order and types match the shader's push_constant block. The layer
//           checks the size, not the order.
// Contract: every stage that reads it must be in pushRange.stageFlags - fragment
//           reads alpha, so VERTEX alone is not enough.
// Contract: mesh.vert / mesh.frag의 Scene 블록과 필드가 같아야 한다. 프레임마다 한 번
//           쓰고 모든 draw가 같은 값을 읽는다 - draw마다 다른 것은 push로 간다.
//
// vec3가 아니라 vec4인 이유: std140에서 vec3도 16바이트로 정렬되므로, 남는 자리를
// 숨기는 것보다 이름을 붙이는 쪽이 낫다.
struct SceneUniform {
    glm::mat4 viewProj;
    glm::vec4 lightDir;     // xyz = 표면에서 광원을 향하는 방향, w 미사용
    glm::vec4 lightColor;   // rgb = 색, a = ambient
    glm::vec4 viewPos;      // xyz = 카메라 위치, w = specular 지수
};

struct PushConstants {
    glm::mat4 mvp;   // model -> world -> view -> clip
    float alpha;     // 1.0 is opaque. Opaque pipelines ignore it: blending is off
};

// Vertex - stride 48, all float so no padding. Offsets leave via offsetof.
//
//   position  12   world space
//   normal    12   +z for a z=0 face wound CCW in y-up
//   uv         8   (0,0) top-left, y down
//   tangent   16   xyz, w = bitangent sign (glTF TANGENT)

// The vertex layout for Vertex. Callers hand this to desc.vertexInput; a shader that
// builds its own points (fullscreen) leaves that null.
const VkPipelineVertexInputStateCreateInfo& VertexInput() noexcept;

// Effect: builds one pipeline from desc. The push range, the set layout and the
//         shader stages come out of the .spv; everything else is desc. An out that
//         already carries a set layout keeps it, which is what a rebuild needs.
//
// Contract: colorFormat, depthFormat and samples must be what the attachments
//           actually are.
//           LINE polygonMode needs fillModeNonSolid, which is no longer requested.
bool CreateGraphicsPipeline(const VulkanDevice& dev,
                            const GraphicsPipelineDesc& desc,
                            Pipeline* out) noexcept;
