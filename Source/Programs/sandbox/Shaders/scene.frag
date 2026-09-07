#version 450

// scene.frag -- one fragment, to one colour
// ============================================================================
//
//   in                          from                   update
//   -------------------------------------------------------------------------
//   fragWorldPos fragNormal
//   fragTangent fragUV          scene.vert, interpolated   per fragment
//   camera.viewPos              set 0, binding 0           per frame
//   light                       set 0, binding 1           per frame
//   shadow shadowMap            set 0, binding 2 and 3     per frame
//   view                        set 0, binding 4           per frame (the panel)
//   baseColor normalMap
//   mtl metallicRoughnessMap    set 1, bindings 0..3       per material
//   pc.alpha                    push constant              per draw
//
//   out
//   -------------------------------------------------------------------------
//   outColor                    location 0
//
// Every update frequency in the program meets here. That is what a fragment stage is:
// the place a per-vertex value, a per-material texture and a per-frame light become
// one number.
//
// Set 0 and set 1 are two sets because their contents are counted differently -- one
// per frame in flight, one per material. In one set the count would be their product,
// each copy carrying the same camera.

// All world space but the last, which is a coordinate on the surface. The order is
// scene.vert's, and the two files agree by location.
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec4 fragTangent;
layout(location = 3) in vec2 fragUV;

// Where the eye is. Only this field, at the offset it sits at: the two matrices in
// front of it are the vertex stage's.
//
// Contract: 128 is offsetof(CameraUniform, viewPos) in Passes.h -- view then proj.
layout(set = 0, binding = 0) uniform Camera {
    layout(offset = 128) vec4 viewPos;
} camera;

// What arrives at a surface. No matrix here: a fragment needs a direction and a
// colour, and where the light looks *from* is the shadow's business below.
//
// Contract: same fields as LightUniform in Passes.h.
layout(set = 0, binding = 1) uniform Light {
    vec4 direction;   // xyz = surface toward the light
    vec4 color;       // rgb = colour, a = ambient
} light;

// The same light as a viewpoint -- one fact in two halves. The matrix puts a fragment
// where the map was drawn from, and the map says what was nearest there.
//
// Apart from the block above because neither is used outside ShadowFactor: being a
// viewpoint is something shadow mapping needs, not something a light has.
//
// The two cannot disagree by construction: binding 2 is the buffer the shadow pass was
// handed, so this matrix is the one that drew the map beside it.
//
// A plain sampler2D, so the comparison happens here. A comparison sampler would do it
// in hardware with free 2x2 filtering, which is what a soft edge would ask for.
layout(set = 0, binding = 2) uniform Shadow {
    mat4 lightView;
    mat4 lightProj;
} shadow;

layout(set = 0, binding = 3) uniform sampler2D shadowMap;

// What the sky sends a matte surface facing a direction. Replaces a constant ambient:
// a colour that was the same everywhere could not tell a surface looking up from one
// looking into a corner, and this one has that difference built into it.
layout(set = 0, binding = 6) uniform samplerCube irradianceCube;

layout(set = 0, binding = 7) uniform samplerCube prefilteredCube;
layout(set = 0, binding = 8) uniform sampler2D brdfLut;

// Output: what the environment reflects off this surface
//
// The split-sum approximation put back together: the prefiltered environment in the
// mirror direction, at the level that stands for this roughness, multiplied by what the
// BRDF does to it. Neither half means anything alone -- one is the light and the other
// is the surface, and the whole point of splitting them is that each fits in a texture.
//
// Contract: kPrefilterMips in Sky.h. The level is where a roughness was stored, so
//           reading it back is that mapping run the other way.
vec3 EnvironmentSpecular(vec3 normal, vec3 toEye, float roughness, vec3 f0) {
    const float kPrefilterMips = 5.0;
    const vec3 reflected = reflect(-toEye, normal);
    const float ndotv = max(dot(normal, toEye), 0.0);

    const vec3 prefiltered =
        textureLod(prefilteredCube, reflected, roughness * (kPrefilterMips - 1.0)).rgb;
    const vec2 scaleBias = texture(brdfLut, vec2(ndotv, roughness)).rg;
    return prefiltered * (f0 * scaleBias.x + scaleBias.y);
}


// What to leave out, so a feature can be compared against its own absence without
// rebuilding. Owned by the panel -- nothing the scene computes decides any of it.
//
// Contract: field order matches ViewOptionsUniform in Gui.h.
layout(set = 0, binding = 4) uniform View {
    float useNormalMap;
    float useBaseColor;
    float useSpecular;
    float useAlphaMask;
    float useShadow;
    float useMetallicRoughness;
} view;

// Set 1, per material. Four bindings and one set: all four are counted the same way.
//
// Contract: this order is MaterialSet() in Passes.h, which every program that draws a
//           surface is checked against.
layout(set = 1, binding = 0) uniform sampler2D baseColor;

// glTF stores this in tangent space, so it needs the TBN built in main.
//
// Contract: this image must be UNORM. It is a direction, not a colour -- read as SRGB
//           every normal bends toward the flat one.
layout(set = 1, binding = 1) uniform sampler2D normalMap;

// What the material is apart from its images. In the set and not the push block
// because it is counted per material; the push block goes out once per draw.
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
// Contract: this image must be UNORM, for the same reason as the normal map. These are
//           numbers the shader multiplies, not light the eye sees.
layout(set = 1, binding = 3) uniform sampler2D metallicRoughnessMap;

// Per draw. One field, at the offset it actually sits at -- a stage declares what it
// reads, and a field's offset comes from every field in front of it.
//
// Contract: 112 is offsetof(PushConstants, alpha) in Passes.h. Nothing checks it: the
//           .spv reports the block's extent and the layer compares that, and a field
//           inside it is past what either side can see.
layout(push_constant) uniform Push {
    layout(offset = 112) float alpha;
} pc;

layout(location = 0) out vec4 outColor;

// Output: 1 where the light reaches this point, 0 where something else got there first
//
// ndotl steers the bias. A surface edge-on to the light spans many depths inside one
// shadow texel and needs more slack than one facing it; without that the choice is
// between acne on the flat surfaces and a gap under every object.
float ShadowFactor(vec3 worldPos, float ndotl) {
    const vec4 clip = shadow.lightProj * (shadow.lightView * vec4(worldPos, 1.0));

    // The light is directional, so its projection is orthographic and w is 1. Divided
    // anyway: this is the line a spot light would change, and it should be visible.
    const vec3 ndc = clip.xyz / clip.w;

    // Contract: this maps ndc to uv the way the shadow pipeline's viewport lays the map
    //           out -- v grows downward, which is ViewportY::Down. Flip one and the
    //           shadows land mirrored; nothing reports it, both sides being legal alone.
    const vec2 uv = ndc.xy * 0.5 + 0.5;

    // Outside the map is not "in shadow": the light's ortho box covers the scene we
    // chose, and past it there is no depth to compare against. The sampler wraps, so
    // without this the far end of the atrium would be shaded by the near end.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) {
        return 1.0;
    }

    const float bias = max(0.0025 * (1.0 - ndotl), 0.0004);
    return texture(shadowMap, uv).r + bias < ndc.z ? 0.0 : 1.0;
}

void main() {
    // --- coverage: does this fragment exist at all -------------------------
    //
    // Before the lighting, so a thrown-away fragment costs nothing after this point.
    // discard is what glTF alphaMode MASK means.
    //
    // The alpha here is the material's, not the push constant's: one says which texels
    // exist, the other how see-through the whole surface is. glTF multiplies the
    // texture's by the factor's, which is why both appear.
    const vec4 sampled = texture(baseColor, fragUV);
    const float coverage = sampled.a * mtl.baseColorFactor.a;
    if (view.useAlphaMask > 0.5 && coverage < mtl.alphaCutoff) { discard; }

    // --- the surface frame: which way does this point face ------------------
    //
    // Normalized because interpolation across the triangle shortens it.
    const vec3 geometric = normalize(fragNormal);

    // Gram-Schmidt. Interpolation leaves the tangent slightly off perpendicular, and
    // the TBN has to be orthonormal or the bent normal comes out skewed.
    const vec3 tangent = normalize(fragTangent.xyz - geometric * dot(geometric, fragTangent.xyz));
    const vec3 bitangent = cross(geometric, tangent) * fragTangent.w;
    const mat3 tbn = mat3(tangent, bitangent, geometric);

    // Stored 0..1, used -1..1. A flat texel is (0.5, 0.5, 1.0), which comes back as +z
    // -- the geometric normal, unchanged.
    const vec3 tangentNormal = texture(normalMap, fragUV).xyz * 2.0 - 1.0;
    const vec3 normal = view.useNormalMap > 0.5 ? normalize(tbn * tangentNormal)
                                                 : geometric;

    // --- what the material is ----------------------------------------------
    //
    // Texture times factor, which is what glTF means by either -- neither alone is it.
    //
    // Switched off, both take the value a material with no answer would have: fully
    // rough and not metal, which is the flat plastic everything looked like before
    // this was read.
    const vec2 mr = texture(metallicRoughnessMap, fragUV).gb;
    const float roughness = view.useMetallicRoughness > 0.5
                          ? clamp(mr.x * mtl.roughness, 0.04, 1.0) : 1.0;
    const float metallic = view.useMetallicRoughness > 0.5 ? mr.y * mtl.metallic : 0.0;

    // A flat grey when base colour is off, so the shape and the lighting stay readable.
    const vec3 albedo = view.useBaseColor > 0.5
                      ? sampled.rgb * mtl.baseColorFactor.rgb
                      : vec3(0.8);

    // What metalness means, in the two places it means anything. A metal reflects its
    // own colour and has no diffuse at all; a dielectric reflects the light's colour
    // and keeps its paint. 0.04 is the reflectance most non-metals sit near.
    const vec3 specularColor = mix(vec3(0.04), albedo, metallic);
    const vec3 diffuseColor = albedo * (1.0 - metallic);

    // --- how much light arrives ---------------------------------------------
    const vec3 toLight = normalize(light.direction.xyz);
    const float lambert = max(dot(normal, toLight), 0.0);

    // Blinn-Phong. The halfway vector stands in for the mirror direction and lines up
    // with the normal exactly when the surface reflects the light at the eye.
    const vec3 toEye = normalize(camera.viewPos.xyz - fragWorldPos);
    const vec3 halfway = normalize(toLight + toEye);

    // Roughness as a Blinn-Phong exponent. **This is not PBR** -- no GGX distribution,
    // no Fresnel, no energy conservation. What it buys is roughness coming from the
    // asset instead of one constant for the scene, so marble and cloth stop having the
    // same highlight.
    //
    // Smooth concentrates the highlight, rough spreads it. 2/a^4 - 2 with a =
    // roughness^2 is the standard correspondence; this is that curve without the
    // arithmetic.
    const float shininess = mix(256.0, 4.0, roughness);
    const float highlight = pow(max(dot(normal, halfway), 0.0), shininess);

    // Gated on lambert: a surface facing away from the light cannot shine.
    const float specular = highlight * step(0.0001, lambert) * view.useSpecular;

    // Skipped where the surface already faces away -- unlit either way, and the bias is
    // meaningless at a grazing angle.
    const float shade = (view.useShadow > 0.5 && lambert > 0.0)
                      ? ShadowFactor(fragWorldPos, lambert) : 1.0;

    // --- put it together -----------------------------------------------------
    //
    // Diffuse takes the surface colour, specular takes the light's: a highlight is the
    // light itself reflected, not the paint.
    //
    // Ambient sits outside the shade, because a shadowed surface is still lit by the
    // room. It reaches the reflected term too, which a real renderer would not do --
    // there an environment map is what a metal reflects. Without one a metal has no
    // diffuse and only a highlight, so every metal surface facing away goes black.
    // Sponza has several and they did exactly that.
    // Ambient is what the sky sends this normal, not a number that was the same
    // everywhere. light.color.a stays as a floor under it, so the panel's ambient
    // slider still means something and a scene with no sky is not black.
    const vec3 ambient = texture(irradianceCube, normal).rgb + vec3(light.color.a);

    // The environment's share of the specular, which is what a metal facing away from
    // the sun now reflects instead of going black. The direct highlight stays: one is a
    // light source and the other is everything else, and they add.
    const vec3 envSpecular =
        EnvironmentSpecular(normal, toEye, roughness, specularColor);

    const vec3 lit =
        (light.color.rgb * lambert * shade + ambient) * diffuseColor
        + (light.color.rgb * specular * shade) * specularColor + envSpecular;

    outColor = vec4(lit, pc.alpha);
}
