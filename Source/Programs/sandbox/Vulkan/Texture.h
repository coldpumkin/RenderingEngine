#pragma once

// Texture - one GPU image resource
// ============================================================================
//
// One Texture is one image plus the view onto it. Being an attachment or a sampled
// input is not a property of the type: desc.usage and the current layout decide that.
// Unreal's FRHITexture is the same shape, and an attachment there (FColorEntry) points
// at two of them rather than nesting one inside the other.
//
// So a resolve target is its own Texture, and the pass that owns both names them.
// Holding it in here gave every sampled texture a field it never used, and turned
// "which view do I read" into a question about a value (is resolve.view null?)
// instead of a question the caller already knows the answer to.
//
// No descriptor set here: a set belongs to the pass that binds it, not to one of the
// things it names -- it can name several.
//
// Two ways to fill one: draw into it (a pass does that) or upload pixels.

#include "Vulkan/Image.h"

#include <cstdint>
#include <vector>   // ReadTexturePixels hands back a whole image

struct Commands;


struct Texture {
    TextureDesc desc;
    Image image;

    // The one view anything else actually takes. Declared last so it is destroyed
    // first: Vulkan frees neither for the other, and a view over a dead image is a
    // dangling child.
    //
    // One because this image has one mip and one layer. Mips, cube faces or a
    // depth/stencil split make it several, and only this struct changes -- everyone
    // else already takes a view rather than a Texture.
    ImageView view;
};

// One thing a pass can draw into: a view, the image behind it, and what that view is
//
// **Not a Texture, and that is the point.** A Texture owns its image, so requiring one
// here would mean a pass can only draw into whole images allocated for it -- one face
// of a cube and one level of a mip chain are views onto an image something else owns,
// and neither could be an attachment.
//
// desc says what this view exposes rather than what the whole image is: a cube face is
// a 2D target of the cube's extent, which is what the pass drawing into it declares.
//
// The three members are what BeginPass actually reads. The image is for the barrier,
// which covers an image; the view is what an attachment is written with; the desc is
// what the declaration is checked against and what the render area is covered by.
struct AttachmentView {
    VkImage image = VK_NULL_HANDLE;
    const ImageView* view = nullptr;   // nullptr means there is none
    TextureDesc desc{};
};

// Output: the whole of a texture, as something a pass can draw into
inline AttachmentView TargetOf(const Texture& texture) noexcept {
    return AttachmentView{texture.image.handle, &texture.view, texture.desc};
}

// Output: an empty texture. Something has to draw into it before it is worth reading.
// What a pass can state about an image it reads, and the whole of what a desc answers
// ----------------------------------------------------------------------------
//
// A pass draws into some images and reads others. What it draws into is checked
// against its pipeline; what it **reads** was checked by nobody until 09-06, and two
// of the four passes had grown their own version of one third of this.
//
// **One question, and it is the one a shader cannot ask.** This began as three. The
// other two -- one sample, and SAMPLED rather than STORAGE -- are in the .spv: a stage
// writes sampler2D or sampler2DMS, and image2D or not, and UpdateSet holds the view it
// is handed to what the stage declared. Asserting them here as well was writing the
// shader down a second time.
//
//   the right kind COLOR or DEPTH, from the format by IsDepthFormat. GLSL's
//                  sampler2D takes either, so no reflection reports this -- a shadow
//                  map read as a colour is legal Vulkan and a wrong picture. The
//                  meaning is the pass's, which is why it is still an argument
//
// **Everything past this is not in a desc.** That it is *the* shadow map rather than
// some other depth image, and that it was drawn this frame, are the next two rungs and
// neither is written down anywhere -- the first is a loop index at pass creation, the
// second the order of two lines in RecordFrame.
//
// Input:  what names the image in the message, in the reader's words
// wantDepth rather than an aspect mask: what a pass can say about an image it reads is
// which kind it is, and a mask would invite the answer to be confused with the aspect a
// view exposes, which is a different question with different rules.
bool CheckSampledInput(const TextureDesc& desc, const char* what,
                       bool wantDepth) noexcept;

bool CreateTexture(const VulkanDevice& dev, const TextureDesc& desc,
                   Texture* out) noexcept;

// Effect: releases the view and then the image, in that order, leaving an empty
//         Texture that CreateTexture can fill again.
//
// **Assigning a fresh Texture over an old one does not do this.** Member-wise
// assignment runs in declaration order, so the image would be destroyed while a view
// made from it is still alive -- the child-before-parent rule, and the reason view is
// declared last in the first place. Remaking a render target at a new size is the
// path that needs this.
//
// Contract: nothing on the GPU may still be using it. The caller waits.
void ResetTexture(Texture* texture) noexcept;

// Effect: uploads pixels through a staging buffer and leaves the image
//         SHADER_READ_ONLY_OPTIMAL, which is what a set records.
//
// Contract: usage must include TRANSFER_DST and SAMPLED, and size must match
//           extent x format. Neither is checked here.
bool CreateTextureFromPixels(const VulkanDevice& dev, const Commands& commands,
                             const TextureDesc& desc,
                             const void* pixels, VkDeviceSize size,
                             Texture* out) noexcept;

// The other direction: what the GPU drew, back where the CPU can look at it.
//
// Input:  current is the layout the image is in when this runs. It is put back that
//         way, so reading does not disturb the frame after it.
// Output: out holds width * height * 4 bytes, tightly packed, in the image's own
//         channel order.
//
// Contract: usage must include TRANSFER_SRC, samples must be 1 (a multisample image
//           cannot be copied to a buffer), and the format must be four 8-bit channels.
//           All three are checked -- this runs off a switch, so a wrong call should
//           say so rather than trip the validation layer.
//
// Blocks on the GPU, like the upload above. Not a per-frame path.
bool ReadTexturePixels(const VulkanDevice& dev, const Commands& commands,
                       const Texture& texture, VkImageLayout current,
                       std::vector<uint8_t>* out) noexcept;
