#version 450

// All world space but the last, which is on the surface rather than in the scene.
// The order is scene.vert's, and the two files agree on it by location.
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec4 fragTangent;
layout(location = 3) in vec2 fragUV;

// Declared in the order the values are decided: the frame's, then the material's,
// then this draw's. A set is a set because of how its contents are counted, so that
// order is also the reason there are two of them.

// Set 0 is the frame's, and it holds two subjects rather than one. They were a single
// block called Scene until it was read field by field: the camera's two come from the
// keyboard, the light's three from the clock, and the light's matrix is wanted by the
// shadow pass as well while nothing of the camera's is. Different reasons to change
// and a different number of readers, which is two of the three grounds for splitting.
//
// Binding 0, the camera. Only viewPos is read here, so it is the only field declared
// -- 64 is where it starts, and stating that beats declaring a matrix this stage never
// touches. The same thing the push block below does, for the same reason.
//
// Contract: 64 is offsetof(CameraUniform, viewPos) in Passes.h.
layout(set = 0, binding = 0) uniform Camera {
    layout(offset = 64) vec4 viewPos;
} camera;

// Binding 1, the light. All three read here, so the block is spelled out in order.
//
// Contract: same fields as LightUniform in Passes.h.
layout(set = 0, binding = 1) uniform Light {
    // The same world, seen from the light. Here rather than in the push block for the
    // reason the camera is: one light for every draw in the pass.
    mat4 lightViewProj;

    vec4 direction;   // xyz = surface toward the light
    vec4 color;       // rgb = colour, a = ambient
} light;

// Binding 2: the depth the shadow pass wrote, counted the same way the two above are
// -- one per frame in flight, because each frame draws its own.
//
// Next to the light on purpose. lightViewProj and this map are one fact in two halves:
// the matrix has to be the one that drew the map, or every shadow lands somewhere
// else. Nothing checks it -- main writes both from one local.
//
// A plain sampler2D, so this reads the stored depth and compares it here. A
// comparison sampler would do the test in hardware and give free 2x2 filtering, and
// that is what the first soft edge will ask for.
layout(set = 0, binding = 2) uniform sampler2D shadowMap;

// Binding 3: what to leave out, so a feature can be compared against its own absence
// without rebuilding. Counted per frame in flight like the three above, and owned by
// the panel -- nothing the scene computes decides any of it.
//
// Contract: field order matches ViewOptionsUniform in Gui.h.
layout(set = 0, binding = 3) uniform View {
    float useNormalMap;
    float useBaseColor;
    float useSpecular;
    float useAlphaMask;
    float useShadow;
    float useMetallicRoughness;
} view;

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
    float metallic;         // both multiply the texture below, and glTF defaults
    float roughness;        // both to 1
} mtl;

// glTF packs two numbers into one image: green is roughness, blue is metallic. Red is
// unused here -- some tools write occlusion into it, which we do not read.
//
// Contract: this image must be UNORM. These are numbers the shader multiplies, not
//           light the eye sees, and SRGB would bend every one of them.
layout(set = 1, binding = 3) uniform sampler2D metallicRoughnessMap;

// This draw's, and nothing else: the push block is the one thing sent for every draw
// whatever the order.
//
// One field, at the offset it actually sits at. A stage declares what it reads, not
// what the block contains -- but a field's offset comes from every field in front of
// it, so naming a subset means saying where the subset starts.
//
// Written out as mat4 + three vec4 before it, this stage would have to carry a normal
// matrix it never touches only to put alpha in the right place. It said
// "mat4 model; float alpha;" instead, which put alpha at 64 and read the first column
// of the normal matrix -- about 125 under our uniform scale. Nothing showed, because
// blending is off and the alpha channel is discarded.
//
// Contract: 112 is offsetof(PushConstants, alpha) in Passes.h. Nothing checks it --
//           the .spv reports the block's size and the layer compares that, and a field
//           inside it is past what either side can see.
layout(push_constant) uniform Push {
    layout(offset = 112) float alpha;
} pc;

layout(location = 0) out vec4 outColor;

// Output: 1 where the light reaches this point, 0 where something else got there first
//
// ndotl steers the bias: a surface edge-on to the light spans many depths inside one
// shadow texel, so it needs more slack than one facing the light does. Without it the
// choice is between acne on the flat surfaces and a gap under every object.
float ShadowFactor(vec3 worldPos, float ndotl) {
    const vec4 clip = light.lightViewProj * vec4(worldPos, 1.0);

    // The light is directional, so its projection is orthographic and w is 1. Divided
    // anyway -- this line is what would have to change for a spot light, and it should
    // be visible rather than assumed.
    const vec3 ndc = clip.xyz / clip.w;

    // Contract: this maps ndc to uv the way the shadow pipeline's viewport lays the
    //           map out -- v grows downward, which is ViewportY::Down and the default
    //           it is left at. Flip one and the shadows land mirrored about the
    //           horizontal; nothing reports it, because both sides are legal alone.
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
    if (view.useAlphaMask > 0.5 && coverage < mtl.alphaCutoff) { discard; }

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
    const vec3 normal = view.useNormalMap > 0.5 ? normalize(tbn * tangentNormal)
                                                 : geometric;
    const vec3 toLight = normalize(light.direction.xyz);
    const float lambert = max(dot(normal, toLight), 0.0);

    // Blinn-Phong: the halfway vector stands in for the mirror direction, and lines up
    // with the normal exactly when the surface reflects the light at the eye.
    // What the surface is made of. Texture times factor, the same rule base colour
    // follows -- glTF means neither alone.
    //
    // Off, both take the value a material with no answer would have: fully rough and
    // not metal, which is the flat plastic everything looked like before this was
    // read.
    const vec2 mr = texture(metallicRoughnessMap, fragUV).gb;
    const float roughness = view.useMetallicRoughness > 0.5
                          ? clamp(mr.x * mtl.roughness, 0.04, 1.0) : 1.0;
    const float metallic = view.useMetallicRoughness > 0.5 ? mr.y * mtl.metallic : 0.0;

    const vec3 toEye = normalize(camera.viewPos.xyz - fragWorldPos);
    const vec3 halfway = normalize(toLight + toEye);

    // Roughness as a Blinn-Phong exponent. **This is not PBR** -- there is no GGX
    // distribution, no Fresnel and no energy conservation here. What it buys is that
    // roughness now comes from the asset instead of one constant for the whole scene,
    // so marble and cloth stop having the same highlight.
    //
    // The mapping is the usual one: a smooth surface concentrates the highlight, a
    // rough one spreads it. 2/a^4 - 2 with a = roughness^2 is the standard
    // correspondence; this is the same curve without the arithmetic.
    const float shininess = mix(256.0, 4.0, roughness);
    const float highlight = pow(max(dot(normal, halfway), 0.0), shininess);

    // Gated on lambert: a surface facing away from the light cannot shine.
    const float specular = highlight * step(0.0001, lambert) * view.useSpecular;

    // Skipped where the surface already faces away: it is unlit either way, and the
    // bias is meaningless at a grazing angle.
    const float shade = (view.useShadow > 0.5 && lambert > 0.0)
                      ? ShadowFactor(fragWorldPos, lambert) : 1.0;

    // Diffuse takes the surface colour, specular does not -- a highlight is the light
    // itself reflected, not the paint.
    // A flat grey when it is off, so the shape and the lighting stay readable.
    // Texture times factor is what glTF means by base colour -- neither alone is it.
    const vec3 albedo = view.useBaseColor > 0.5
                      ? sampled.rgb * mtl.baseColorFactor.rgb
                      : vec3(0.8);

    // What metalness means, in the two places it means anything. A metal reflects its
    // own colour and has no diffuse at all; a dielectric reflects the light's colour
    // and keeps its paint. 0.04 is the reflectance most non-metals sit near.
    const vec3 specularColor = mix(vec3(0.04), albedo, metallic);
    const vec3 diffuseColor = albedo * (1.0 - metallic);

    // Ambient is outside the shade: a shadowed surface is still lit by the room. It
    // reaches the reflected term too, which is not what a real renderer does -- there
    // an environment map is what a metal reflects.
    //
    // Without one, a metal has no diffuse and only a highlight, so every metal surface
    // not facing the light goes black. Sponza has several (the curtain rods, the
    // planters) and they did exactly that. The ambient standing in for a reflection is
    // the cheapest thing that is not a black hole, and it is the line that changes the
    // day an environment map arrives.
    const vec3 lit =
        (light.color.rgb * lambert * shade + light.color.a) * diffuseColor
        + (light.color.rgb * specular * shade + light.color.a) * specularColor;

    outColor = vec4(lit, pc.alpha);
}
