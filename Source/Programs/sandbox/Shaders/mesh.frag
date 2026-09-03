#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec4 fragTangent;

// Set 1 is the material's. Separate from set 0 because the two are counted
// differently -- one set per frame, one per material -- and in one set the sets
// needed would be their product, each carrying a copy of the same camera.
//
// Contract: one sampler2D here, one binding in the material layout.
layout(set = 1, binding = 0) uniform sampler2D baseColor;

// A second binding in the same set, not a second set: it is counted the same way --
// one per material. glTF stores it tangent space, so it needs the TBN below.
//
// Contract: this image must be UNORM. It is a direction, not a colour, and reading it
//           as SRGB would bend every normal toward the flat one.
layout(set = 1, binding = 1) uniform sampler2D normalMap;

// Contract: same fields as SceneUniform in Passes.h.
layout(set = 0, binding = 0) uniform Scene {
    mat4 viewProj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 viewPos;
    float useNormalMap;
    float useBaseColor;
    float useSpecular;
    float useAlphaMask;
} scene;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 model;
    float alpha;
} pc;

// What the material is, apart from its images. One per material, like the samplers
// above -- alphaCutoff used to ride the push constant, which sent a per-material value
// once per draw.
//
// Contract: field order and std140 padding match MaterialParams in Passes.h.
layout(set = 1, binding = 2) uniform MaterialBlock {
    vec4 baseColorFactor;   // rgb multiplies the texture. a unused
    float alphaCutoff;      // 0 keeps every texel. glTF MASK sets it, OPAQUE does not
} mtl;

void main() {
    // Before the lighting: a thrown-away fragment should cost nothing after this
    // point, and discard is what glTF alphaMode MASK means.
    //
    // The alpha is the base colour texture's, not the push constant's -- one says
    // which texels exist, the other how see-through the whole surface is.
    const vec4 sampled = texture(baseColor, fragUV);
    if (scene.useAlphaMask > 0.5 && sampled.a < mtl.alphaCutoff) { discard; }

    // Normalized here because interpolation across the triangle shortens it.
    const vec3 geometric = normalize(fragNormal);

    // Gram-Schmidt: interpolation leaves the tangent slightly off perpendicular, and
    // the TBN has to be orthonormal or the bent normal comes out skewed.
    const vec3 tangent = normalize(fragTangent.xyz - geometric * dot(geometric, fragTangent.xyz));
    const vec3 bitangent = cross(geometric, tangent) * fragTangent.w;
    const mat3 tbn = mat3(tangent, bitangent, geometric);

    // Stored 0..1, used -1..1. A flat texel is (0.5, 0.5, 1.0), which comes back as
    // +z -- the geometric normal, unchanged.
    const vec3 tangentNormal = texture(normalMap, fragUV).xyz * 2.0 - 1.0;
    const vec3 normal = scene.useNormalMap > 0.5 ? normalize(tbn * tangentNormal)
                                                 : geometric;
    const vec3 toLight = normalize(scene.lightDir.xyz);
    const float lambert = max(dot(normal, toLight), 0.0);

    // Blinn-Phong: the halfway vector stands in for the mirror direction, and lines up
    // with the normal exactly when the surface reflects the light at the eye.
    const vec3 toEye = normalize(scene.viewPos.xyz - fragWorldPos);
    const vec3 halfway = normalize(toLight + toEye);
    const float highlight = pow(max(dot(normal, halfway), 0.0), scene.viewPos.w);

    // Gated on lambert: a surface facing away from the light cannot shine.
    const float specular = highlight * step(0.0001, lambert) * scene.useSpecular;

    // Diffuse takes the surface colour, specular does not -- a highlight is the light
    // itself reflected, not the paint.
    // A flat grey when it is off, so the shape and the lighting stay readable.
    // The factor is the material's, not the texture's: glTF multiplies one by the
    // other, and every one of Sponza's 25 materials sets it (0.588 grey). Ignoring it
    // was why this looked brighter than the asset asks for.
    const vec3 albedo = scene.useBaseColor > 0.5
                      ? sampled.rgb * mtl.baseColorFactor.rgb
                      : vec3(0.8);
    const vec3 lit = (scene.lightColor.rgb * lambert + scene.lightColor.a) * albedo
                   + scene.lightColor.rgb * specular;

    outColor = vec4(lit, pc.alpha);
}
