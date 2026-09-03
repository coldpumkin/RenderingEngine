#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec4 fragTangent;

// Declared in the order the values are decided: the frame's, then the material's,
// then this draw's. A set is a set because of how its contents are counted, so that
// order is also the reason there are two of them.

// Set 0 is the frame's: one camera and one light for every draw in the pass.
//
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

// Set 1 is the material's -- three bindings, not three sets, because all three are
// counted the same way: one per material. Separate from set 0 because that one is
// counted per frame in flight, and putting both in one set would need their product,
// each copy carrying the same camera.
//
// Contract: three bindings here, three in the material layout, in this order.
layout(set = 1, binding = 0) uniform sampler2D baseColor;

// glTF stores it tangent space, so it needs the TBN below.
//
// Contract: this image must be UNORM. It is a direction, not a colour, and reading it
//           as SRGB would bend every normal toward the flat one.
layout(set = 1, binding = 1) uniform sampler2D normalMap;

// What the material is apart from its images. In the set rather than the push block
// because it is counted by materials: the push block goes out once per draw, so a
// value that is one per material would ride along four times too often.
//
// Contract: field order and std140 padding match MaterialParams in Passes.h.
layout(set = 1, binding = 2) uniform MaterialBlock {
    vec4 baseColorFactor;   // rgb multiplies the texture, a multiplies its alpha
    float alphaCutoff;      // 0 keeps every texel. glTF MASK sets it, OPAQUE does not
} mtl;

// This draw's, and nothing else: the push block is the one thing sent for every draw
// whatever the order.
layout(push_constant) uniform Push {
    mat4 model;
    float alpha;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    // Before the lighting: a thrown-away fragment should cost nothing after this
    // point, and discard is what glTF alphaMode MASK means.
    //
    // The alpha is the material's, not the push constant's -- one says which texels
    // exist, the other how see-through the whole surface is. glTF multiplies the
    // texture's by the factor's, which is why both are here.
    const vec4 sampled = texture(baseColor, fragUV);
    const float coverage = sampled.a * mtl.baseColorFactor.a;
    if (scene.useAlphaMask > 0.5 && coverage < mtl.alphaCutoff) { discard; }

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
    // Texture times factor is what glTF means by base colour -- neither alone is it.
    const vec3 albedo = scene.useBaseColor > 0.5
                      ? sampled.rgb * mtl.baseColorFactor.rgb
                      : vec3(0.8);
    const vec3 lit = (scene.lightColor.rgb * lambert + scene.lightColor.a) * albedo
                   + scene.lightColor.rgb * specular;

    outColor = vec4(lit, pc.alpha);
}
