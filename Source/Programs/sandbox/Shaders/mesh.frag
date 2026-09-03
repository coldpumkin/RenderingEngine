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

    // The same world, seen from the light. Here rather than in the push block for the
    // reason the camera is: one light for every draw in the pass.
    mat4 lightViewProj;

    vec4 lightDir;
    vec4 lightColor;
    vec4 viewPos;
    float useNormalMap;
    float useBaseColor;
    float useSpecular;
    float useAlphaMask;
    float useShadow;
} scene;

// Binding 1 of the frame's set: the depth the shadow pass wrote, counted the same way
// the uniform above is -- one per frame in flight, because each frame draws its own.
//
// A plain sampler2D, so this reads the stored depth and compares it here. A
// comparison sampler would do the test in hardware and give free 2x2 filtering, and
// that is what the first soft edge will ask for.
layout(set = 0, binding = 1) uniform sampler2D shadowMap;

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

// Output: 1 where the light reaches this point, 0 where something else got there first
//
// ndotl steers the bias: a surface edge-on to the light spans many depths inside one
// shadow texel, so it needs more slack than one facing the light does. Without it the
// choice is between acne on the flat surfaces and a gap under every object.
float ShadowFactor(vec3 worldPos, float ndotl) {
    const vec4 clip = scene.lightViewProj * vec4(worldPos, 1.0);

    // The light is directional, so its projection is orthographic and w is 1. Divided
    // anyway -- this line is what would have to change for a spot light, and it should
    // be visible rather than assumed.
    const vec3 ndc = clip.xyz / clip.w;
    const vec2 uv = ndc.xy * 0.5 + 0.5;

    // Outside the map is not "in shadow": the light's ortho box covers the scene we
    // chose, and anything past it has no depth to compare against. The sampler wraps,
    // so without this the far end of the atrium would be shaded by the near end.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) {
        return 1.0;
    }

    const float bias = max(0.0025 * (1.0 - ndotl), 0.0004);
    return texture(shadowMap, uv).r + bias < ndc.z ? 0.0 : 1.0;
}

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

    // Skipped where the surface already faces away: it is unlit either way, and the
    // bias is meaningless at a grazing angle.
    const float shade = (scene.useShadow > 0.5 && lambert > 0.0)
                      ? ShadowFactor(fragWorldPos, lambert) : 1.0;

    // Diffuse takes the surface colour, specular does not -- a highlight is the light
    // itself reflected, not the paint.
    // A flat grey when it is off, so the shape and the lighting stay readable.
    // Texture times factor is what glTF means by base colour -- neither alone is it.
    const vec3 albedo = scene.useBaseColor > 0.5
                      ? sampled.rgb * mtl.baseColorFactor.rgb
                      : vec3(0.8);
    // Ambient is outside the shade: a shadowed surface is still lit by the room.
    const vec3 lit = (scene.lightColor.rgb * lambert * shade + scene.lightColor.a) * albedo
                   + scene.lightColor.rgb * specular * shade;

    outColor = vec4(lit, pc.alpha);
}
