#pragma once

// Image, ImageView - the allocation, and how one looks at it
// ============================================================================
//
// Two types because Vulkan has two objects, and because they are owned differently:
//
//   Image      the bytes and their shape. A swapchain's is queried, so it may not be
//              ours to free -- allocation says which
//   ImageView  which part of that, read as what. Always ours, even over a queried
//              image, so its destructor has no condition to check
//
// Everything that draws or reads takes a view. Only a barrier takes the image,
// because a layout transition is a fact about the memory:
//
//   VkRenderingAttachmentInfo.imageView   drawing into it
//   VkDescriptorImageInfo.imageView       reading from it
//   VkImageMemoryBarrier2.image           moving it between those two
//
// One view per image today, held by Texture beside its Image. Nothing here says one:
// mips, cube faces or a depth/stencil split each make it several, and no caller that
// takes a view changes when they do.

#include "Vulkan/Device.h"

struct Image {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;   // null = queried, not ours to free

    Image() = default;
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    // Movable because a swapchain keeps its images in a vector. The source is left
    // empty, so its destructor frees nothing.
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;
};

// Which part of an image, seen as what. Every default means "all of it, the way the
// image already is", so a caller with nothing to say passes {}.
//
// Kept by the view because Vulkan cannot be asked what a view sees.
struct ImageViewDesc {
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED;   // UNDEFINED = the image's own format

    // Which aspects this view exposes. 0 means "the format has only one, use it" --
    // not "nobody said". A format with two is refused unless this names one or both,
    // because there the answer is a decision and CreateImageView is not where it is
    // made. Left as a derivation, that decision would be silently taken here and then
    // taken again, differently, by everything downstream.
    VkImageAspectFlags aspect = 0;

    uint32_t baseMip = 0;
    uint32_t mipCount = VK_REMAINING_MIP_LEVELS;
    uint32_t baseLayer = 0;
    uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS;
};

// No pointer back to its Image. A view is used through the caller that already holds
// both, and a Texture moves inside a vector -- a pointer at a sibling member would
// survive the move pointing at the old one, with nothing to catch it.
struct ImageView {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkImageView handle = VK_NULL_HANDLE;
    ImageViewDesc desc;

    // Two facts of the image behind this view, and not because a view owns them
    //
    // A descriptor is written with an image view, so the view is what travels to a
    // binding -- and what a shader requires of that binding is partly the view's
    // (which shape) and partly the image's (how many samples, what it may be used
    // for). Without these the check at the binding could only ask half its question.
    //
    // Both are fixed when the image is created and never move, so a copy here cannot
    // go stale. **Only what a binding check needs belongs here.** Extent, mip count
    // and the rest stay where they are; this is not a second TextureDesc.
    VkSampleCountFlagBits imageSamples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags imageUsage = 0;

    // What this view actually exposes, after desc.aspect's 0 is resolved. The view's
    // own fact, unlike the two above.
    //
    // Kept where desc.format's resolution is not, because nothing downstream needs the
    // resolved format -- a TextureDesc already carries it -- while the aspect has no
    // other home. **Read it to check against, never to answer with**: what a barrier
    // or a copy must name is its own question with its own rule, and the two differ on
    // exactly the format where it matters.
    VkImageAspectFlags aspect = 0;

    ImageView() = default;
    ~ImageView();
    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;
    ImageView(ImageView&& other) noexcept;
    ImageView& operator=(ImageView&& other) noexcept;
};

// Output: the optimal-tiling format features an image with this usage needs
//
// One usage bit to one feature bit, which is Vulkan's own mapping. It exists so the
// question "can this GPU do that" is asked from the usage a caller already wrote
// down. It used to be one hardcoded pair of bits in QueryTargetCapabilities, which is
// a guess made away from the image: it missed the shadow map's SAMPLED and the
// resolve's TRANSFER_SRC, and demanded SAMPLED of a multisample colour image that
// cannot have it.
VkFormatFeatureFlags RequiredFormatFeatures(VkImageUsageFlags usage) noexcept;
// What one texture is. usage is the only field a caller really chooses -- the rest
// comes from AttachmentFormats or from the file the pixels came out of.
// What kind of thing this is, which is what the layer count and the view type follow
// from rather than being written beside it
//
// A cube is six layers addressed by a direction, and saying so here makes a cube of
// five faces unwriteable. Nothing here is 3D or an array yet; both are one more value
// and a depth field on the day something wants them.
enum class TextureKind { Texture2D, Cube };

struct TextureDesc {
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = 0;

    TextureKind kind = TextureKind::Texture2D;

    // 1 is a texture with no mip chain, which is every one of ours so far. IBL's
    // prefilter is the first thing that wants more, and it draws into them one at a
    // time -- which is what ImageViewDesc::baseMip has been waiting for.
    uint32_t mipLevels = 1;
};

// Output: how many array layers a texture of this kind has
inline uint32_t LayersOf(TextureKind kind) noexcept {
    return kind == TextureKind::Cube ? 6u : 1u;
}

// Output: the desc of one 2D slice of a texture -- one cube face, one array layer
//
// What a view over a single layer exposes is a 2D image of the same extent, and that is
// what a pass drawing into it is drawing into. The whole thing's kind is the cube's
// business and not that pass's.
inline TextureDesc SliceDesc(const TextureDesc& whole) noexcept {
    TextureDesc slice = whole;
    slice.kind = TextureKind::Texture2D;
    slice.mipLevels = 1;
    return slice;
}

// Output: whether two descs describe the same kind of image
//
// Every field but the extent, which is deliberate and was measured: a resize changes it
// on the image while the declaration a pass was created with is not re-derived, so
// comparing it refuses a frame that is perfectly correct. main's swapchainTarget is
// built once before the loop and keeps the size the window had then -- nothing reads
// that extent, and this is why nothing may.
//
// Size is checked, by coverage rather than by equality: BeginPass asks that every
// attachment reaches the render area (VUID-VkRenderingInfo-pNext-06079), which is what
// the spec asks and allows an attachment larger than what is drawn.
//
// Written out at the two places that ask "is this the resource that was declared" --
// BeginPass and DeclareRead -- and each grew a field behind the other until it was
// worth one call.
inline bool SameTextureDesc(const TextureDesc& a, const TextureDesc& b) noexcept {
    return a.format == b.format && a.samples == b.samples && a.usage == b.usage
        && a.kind == b.kind && a.mipLevels == b.mipLevels;
}

// Output: the view type that reaches the whole of a texture of this kind
inline VkImageViewType ViewTypeOf(TextureKind kind) noexcept {
    return kind == TextureKind::Cube ? VK_IMAGE_VIEW_TYPE_CUBE
                                     : VK_IMAGE_VIEW_TYPE_2D;
}


// Input:  samples is the MSAA sample count (1_BIT means no MSAA)
//         usage is what this image is for (attachment / sampled / copy destination)
// Output: an Image with no view. CreateImageView makes those. false also means the
//         format cannot do what usage asks of it, checked here because this is where
//         both are known.
//
// Contract: samples must equal the rasterizationSamples of every pipeline that draws
//           into this. The validation layer says so at vkCmdBeginRendering.
//
// No default for samples: 1_BIT as one would compile at a call site that meant to
// make a multisample image. There are three callers, so being explicit is cheap.
bool CreateImage(const VulkanDevice& dev, const TextureDesc& desc, Image* out) noexcept;

// Output: which aspect a format is read through -- COLOR or DEPTH
//
// **Derived and not chosen.** Whether an image holds a colour or a depth is a fact
// about its format, so nothing has to say it twice; CreateImageView takes this when a
// caller leaves desc.aspect at 0, and a pass checking what it reads asks the same
// question of the same function.
//
// **Every aspect it has, which is a fact -- not the one to use, which is not.**
//
// A combined format answers DEPTH | STENCIL. That makes "is there anything to decide"
// a question this answers (one bit set, or more), and it is the only thing any caller
// should read it for. Which aspect an operation names is that operation's rule:
// a barrier over a combined image must name both
// (VUID-VkImageMemoryBarrier2-image-03320, with separateDepthStencilLayouts off) while
// a view sampled from one must name exactly one
// (VUID-VkDescriptorImageInfo-imageView-01976). One answer cannot serve both.
VkImageAspectFlags FormatAspects(VkFormat format) noexcept;


// Output: whether this format is a depth one rather than a colour one
//
// A different question from the one above, and it used to be asked of it by comparing
// its answer to DEPTH_BIT. Two of that function's three callers were asking this.
bool IsDepthFormat(VkFormat format) noexcept;

// Input:  imageFormat is what the image was created with -- desc.format UNDEFINED
//         means that one, and desc.aspect 0 is derived from it.
//
// The format is passed rather than read back because Image does not keep it: the
// caller that made the image has it, and a queried swapchain image was told it. The
// samples and the usage arrive for the same reason and are kept for the reason the
// fields above give.
//
// Contract: image must outlive the view. Vulkan destroys neither for the other.
bool CreateImageView(const VulkanDevice& dev,
                     VkImage image,
                     VkFormat imageFormat,
                     VkSampleCountFlagBits imageSamples,
                     VkImageUsageFlags imageUsage,
                     const ImageViewDesc& desc,
                     ImageView* out) noexcept;
