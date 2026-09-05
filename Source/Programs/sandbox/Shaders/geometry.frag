#version 450

// The G-buffer stage: what a surface is, written down instead of lit
// ============================================================================
//
// scene.frag reads a surface and answers "what colour is this pixel". This one reads
// the same surface and answers "what is here", leaving the question to a later pass.
// The vertex stage is the same file for both -- where a surface sits does not depend
// on when it is shaded, which is why scene.vert is reused unchanged.
//
// Three colour attachments and a depth one:
//
//   0  albedo      rgb the surface colour, a unused
//   1  normal      rgb the world-space normal, mapped from -1..1 into 0..1
//   2  material    r metallic, g roughness, ba unused
//   depth          written by the fixed-function test, read back by the lighting pass
//
// **The position is not among them.** It is recovered from depth and the inverse
// matrices, which costs two multiplies and saves a whole RGBA32F attachment -- the
// standard trade, and the reason view and proj travel separately.
//
// Nothing here is view-dependent. Where the eye is, where the light is and whether
// anything shadows this point are all the lighting pass's, which is the whole of what
// deferred moves.

// The same four the forward stage reads, from the same vertex stage.
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec4 fragTangent;
layout(location = 3) in vec2 fragUV;

// The panel, and the only thing this stage reads from set 0 -- nothing here asks
// where the camera or the light is. **Three of the six are answered here and the
// other three in the lighting pass**, and which is which is what deferred moves: a
// switch that changes what a surface *is* belongs to this stage, and one that changes
// how it is lit belongs to the other. In the forward path all six sit in one file
// and the line is invisible.
//
// Bindings 1..3 of set 0 are a hole in this program's layout, and BuildSetLayout
// leaves them out. That makes this set 0 a different layout from scene.frag's, so
// this pass fills a set of its own.
//
// Contract: field order matches ViewOptionsUniform in Gui.h, truncated after the
//           fourth field -- this stage reads no further.
layout(set = 0, binding = 4) uniform View {
    float useNormalMap;
    float useBaseColor;
    float useSpecular;    // the lighting pass's, named to keep the offsets
    float useAlphaMask;
} view;

// Set 1 is the material's, and this stage reads all of it.
//
// Contract: three images and a block, in this order, matching the material layout.
layout(set = 1, binding = 0) uniform sampler2D baseColor;
layout(set = 1, binding = 1) uniform sampler2D normalMap;

// Contract: field order and std140 padding match MaterialParams in Passes.h.
layout(set = 1, binding = 2) uniform MaterialBlock {
    vec4 baseColorFactor;
    float alphaCutoff;
    float metallic;
    float roughness;
} mtl;

layout(set = 1, binding = 3) uniform sampler2D metallicRoughnessMap;

// Contract: 112 is offsetof(PushConstants, alpha) in Passes.h. The vertex stage
//           declares the front of the same block; a stage may name part of one, and
//           the offsets are what the two owe each other.
layout(push_constant) uniform Push {
    layout(offset = 112) float alpha;
} pc;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;

void main() {
    // Before anything else, for the reason the forward stage discards first: a
    // thrown-away fragment should cost nothing after this point.
    //
    // **This switch is heavier here than in the forward path.** There it decides one
    // pixel's colour. Here it decides what goes in the depth buffer, and the lighting
    // pass reads that depth to rebuild a position -- so turning the mask off does not
    // just fill the leaves in, it moves where the lighting pass thinks the surface is.
    const vec4 sampled = texture(baseColor, fragUV);
    const float coverage = sampled.a * mtl.baseColorFactor.a;
    if (view.useAlphaMask > 0.5 && coverage < mtl.alphaCutoff) { discard; }

    // Normalized here because interpolation across the triangle shortens it.
    const vec3 geometric = normalize(fragNormal);

    // Gram-Schmidt, the same as the forward stage: interpolation leaves the tangent
    // slightly off perpendicular, and the TBN has to be orthonormal or the bent normal
    // comes out skewed.
    const vec3 tangent =
        normalize(fragTangent.xyz - geometric * dot(geometric, fragTangent.xyz));
    const vec3 bitangent = cross(geometric, tangent) * fragTangent.w;
    const mat3 tbn = mat3(tangent, bitangent, geometric);

    // Stored 0..1, used -1..1. A flat texel is (0.5, 0.5, 1.0), which comes back as
    // +z -- the geometric normal, unchanged.
    const vec3 tangentNormal = texture(normalMap, fragUV).xyz * 2.0 - 1.0;
    const vec3 normal = view.useNormalMap > 0.5 ? normalize(tbn * tangentNormal)
                                                : geometric;

    // Texture times factor is what glTF means by base colour -- neither alone is it.
    // A flat grey when it is off, the same value the forward stage uses.
    const vec3 albedo = view.useBaseColor > 0.5
                      ? sampled.rgb * mtl.baseColorFactor.rgb
                      : vec3(0.8);
    outAlbedo = vec4(albedo, pc.alpha);

    // -1..1 folded into 0..1 because the attachment is UNORM. The lighting pass folds
    // it back. A signed format would carry it directly and is what this becomes if the
    // banding on a smooth surface ever shows.
    outNormal = vec4(normal * 0.5 + 0.5, 0.0);

    // glTF packs occlusion in r, roughness in g and metallic in b. Factor times
    // texture, the same rule base colour follows.
    const vec2 mr = texture(metallicRoughnessMap, fragUV).gb;
    outMaterial = vec4(mr.x * mtl.roughness, mr.y * mtl.metallic, 0.0, 0.0);
}
