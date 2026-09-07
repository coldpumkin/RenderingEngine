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
// Contract: matches LightUniform in Passes.h, kMaxLights included.
struct LightEntry {
    // xyz = surface toward the light, w = 0 a direction, 1 a position. The homogeneous
    // meaning of w, which is also the only thing that separates the kinds here.
    vec4 direction;
    vec4 color;        // rgb = colour, a = 1 if this light has a shadow map
    vec4 position;     // xyz where it is, w how far it carries
    vec4 cone;         // x inner cosine, y outer -- opened all the way for a point
};

layout(set = 0, binding = 1) uniform Light {
    LightEntry lights[4];

    // rgb = the ambient under all of them, a = how many are live.
    vec4 ambient;
} light;

// Output: xyz toward the light from this point, w how much of it arrives
//
// **The two kinds part here and nowhere else in the shading.** A directional light is
// the same vector at every point and loses nothing on the way; a spot points from the
// surface to where it is, falls off with distance, and stops at its cone. Everything
// after this -- lambert, the highlight, the shadow -- reads one vector and one scalar
// and does not know which kind produced them.
//
// Contract: matches LightUniform in Passes.h.
vec4 LightAt(LightEntry entry, vec3 worldPos) {
    if (entry.direction.w < 0.5) {
        return vec4(normalize(entry.direction.xyz), 1.0);
    }

    const vec3 toLightVec = entry.position.xyz - worldPos;
    const float dist = length(toLightVec);
    const vec3 toLight = toLightVec / max(dist, 0.0001);

    // Inverse square, with the range as the distance where it is called nothing. The
    // window is what stops a light reaching the whole scene faintly, which would cost
    // shadow map area for light nobody can see.
    const float falloff = 1.0 / max(dist * dist, 0.0001);
    const float window = clamp(1.0 - dist / max(entry.position.w, 0.0001), 0.0, 1.0);

    // Full inside the inner cone, nothing outside the outer, smooth between. A point
    // light's outer cosine is -1, so this is 1 in every direction and the same
    // expression covers both positioned kinds without asking which one it is.
    const float aligned = dot(-toLight, normalize(entry.direction.xyz));
    const float cone = smoothstep(entry.cone.y, entry.cone.x, aligned);

    return vec4(toLight, falloff * window * window * cone);
}

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
// Contract: matches ShadowUniform in Passes.h, kMaxLights included.
struct ShadowEntry {
    mat4 lightView;
    mat4 lightProj;
};

layout(set = 0, binding = 2) uniform Shadow {
    ShadowEntry lights[4];
} shadow;

// One layer per light, sampled with the same index the matrices are read with.
layout(set = 0, binding = 3) uniform sampler2DArray shadowMaps;

// What the sky sends a matte surface facing a direction. Replaces a constant ambient:
// a colour that was the same everywhere could not tell a surface looking up from one
// looking into a corner, and this one has that difference built into it.
layout(set = 0, binding = 6) uniform samplerCube irradianceCube;

layout(set = 0, binding = 7) uniform samplerCube prefilteredCube;
layout(set = 0, binding = 8) uniform sampler2D brdfLut;

// One cube per light, addressed by a direction and a slice. What is stored is the
// distance from that light divided by its range, which is why the comparison below
// multiplies it back rather than undoing a projection.
layout(set = 0, binding = 9) uniform samplerCubeArray pointShadowMaps;

const float kPi = 3.14159265359;

// The three terms a microfacet BRDF is made of. A rough surface is taken as a crowd of
// tiny mirrors; the only ones that send this light at this eye are those whose normal is
// the halfway vector, so every term below is about that crowd rather than about the
// surface as a whole.
//
// The same D and G the environment bakes use. prefilter.frag draws its samples from this
// distribution and brdflut.frag integrates this geometry term, so direct light and image
// based light are now two halves of one model rather than two models.

// How much of the crowd faces exactly the halfway vector. GGX rather than a power of the
// cosine: its tail falls off slowly, which is what gives a real highlight a bright core
// and a wide skirt at once.
float DistributionGGX(float ndoth, float roughness) {
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1e-7);
}

// How much of that crowd is hidden behind its neighbours, from the eye and from the
// light. k is the direct-light one; the image based case halves it, which is the
// difference brdflut.frag notes.
float GeometrySmith(float ndotv, float ndotl, float roughness) {
    const float r = roughness + 1.0;
    const float k = (r * r) / 8.0;
    const float gv = ndotv / (ndotv * (1.0 - k) + k);
    const float gl = ndotl / (ndotl * (1.0 - k) + k);
    return gv * gl;
}

// What fraction of the light a mirror reflects rather than lets in. It rises to 1 at
// grazing angles, which is why every surface has a bright rim, and f0 is what it reads
// head on -- 0.04 for a dielectric, the metal's own colour for a metal.
vec3 FresnelSchlick(float vdoth, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - vdoth, 0.0, 1.0), 5.0);
}


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

// Output: 1 where this point light reaches the fragment, 0 where its own geometry is in
//         the way
//
// The direction from the light is the whole address: a cube map is a texture addressed
// by a direction, so which of the six faces this lands on is the sampler's business and
// not this function's.
//
// The bias grows as the surface turns away from the light, for the reason a 2D map's
// does -- at a grazing angle one texel of the map covers a long stretch of surface, and
// a fixed offset is either too small there or too large everywhere else.
float PointShadowFactor(int index, vec3 worldPos, float ndotl) {
    const vec3 fromLight = worldPos - light.lights[index].position.xyz;
    const float range = max(light.lights[index].position.w, 0.0001);

    const float stored = texture(pointShadowMaps, vec4(fromLight, float(index))).r * range;
    const float bias = mix(0.35, 0.05, clamp(ndotl, 0.0, 1.0));
    return length(fromLight) - bias > stored ? 0.0 : 1.0;
}


// Output: 1 where the light reaches this point, 0 where something else got there first
//
// ndotl steers the bias. A surface edge-on to the light spans many depths inside one
// shadow texel and needs more slack than one facing it; without that the choice is
// between acne on the flat surfaces and a gap under every object.
float ShadowFactor(int light, vec3 worldPos, float ndotl) {
    const ShadowEntry entry = shadow.lights[light];
    const vec4 clip = entry.lightProj * (entry.lightView * vec4(worldPos, 1.0));

    // A spot's projection is a perspective and w is not 1, a directional light's is an
    // orthographic and it is. Divided either way, which is what lets one function serve
    // both -- the line that used to say a spot would change it.
    const vec3 ndc = clip.xyz / clip.w;

    // Contract: this maps ndc to uv the way the shadow pipeline's viewport lays the map
    //           out -- v grows downward, which is ViewportY::Down. Flip one and the
    //           shadows land mirrored; nothing reports it, both sides being legal alone.
    const vec2 uv = ndc.xy * 0.5 + 0.5;

    // Outside the map is not "in shadow": what the map covers is the box or the cone
    // that was chosen, and past it there is no depth to compare against. The sampler
    // wraps, so without this the far end of the atrium would be shaded by the near end.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) {
        return 1.0;
    }

    const float bias = max(0.0025 * (1.0 - ndotl), 0.0004);
    return texture(shadowMaps, vec3(uv, float(light))).r + bias < ndc.z ? 0.0 : 1.0;
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

    // --- how much light arrives, from each of them ---------------------------
    //
    // **The loop is the whole of what having several lights costs here.** One shadow
    // map is drawn, for the first light, so only that one is shaded -- the others light
    // what they can reach and cast nothing. That is a choice about how many maps a
    // frame draws, not about what a light is.
    const vec3 toEye = normalize(camera.viewPos.xyz - fragWorldPos);
    const int liveLights = int(light.ambient.a);

    vec3 direct = vec3(0.0);
    for (int i = 0; i < liveLights; ++i) {
        // After this call nothing in the loop knows which kind of light it is: xyz is
        // the direction and w is how much arrives, which a directional light answers
        // with 1.
        const vec4 incoming = LightAt(light.lights[i], fragWorldPos);
        const vec3 toLight = incoming.xyz;
        const float lambert = max(dot(normal, toLight), 0.0) * incoming.w;

        // Cook-Torrance. The halfway vector is the normal a microfacet would need to
        // send this light at this eye, so the three terms are all about the facets that
        // have it: how many there are, how many are not hidden, and how reflective they
        // are.
        const vec3 halfway = normalize(toLight + toEye);
        const float ndotv = max(dot(normal, toEye), 1e-4);
        const float ndotl = max(dot(normal, toLight), 0.0);
        const float ndoth = max(dot(normal, halfway), 0.0);
        const float vdoth = max(dot(toEye, halfway), 0.0);

        const float d = DistributionGGX(ndoth, roughness);
        const float g = GeometrySmith(ndotv, ndotl, roughness);
        const vec3 f = FresnelSchlick(vdoth, specularColor);

        // The denominator turns the facets' own area into the surface's. Gated on ndotl
        // for the reason the old exponent was: a surface facing away cannot shine.
        const vec3 specular = (d * g * f) / max(4.0 * ndotv * ndotl, 1e-4)
                            * step(1e-4, ndotl) * view.useSpecular;

        // What Fresnel did not reflect is what goes in and comes back as diffuse, and a
        // metal keeps none of it. This is the link Blinn-Phong has no way to express:
        // there the two halves are added without either knowing the other.
        const vec3 kd = (vec3(1.0) - f) * (1.0 - metallic);

        // Only the first light has a map. Skipped where the surface already faces
        // away -- unlit either way, and the bias is meaningless at a grazing angle.
        // Only the lights a map was drawn for. color.a says which, because the pass
        // that drew them is the only thing that knows.
        // color.a says which kind of map this light has: 1 a layer of the 2D array,
        // 2 a cube. The two are read differently and nothing else here could tell them
        // apart -- the kind of light is not in this block on purpose.
        float shade = 1.0;
        if (view.useShadow > 0.5 && lambert > 0.0) {
            if (light.lights[i].color.a > 1.5) {
                shade = PointShadowFactor(i, fragWorldPos, lambert);
            } else if (light.lights[i].color.a > 0.5) {
                shade = ShadowFactor(i, fragWorldPos, lambert);
            }
        }

        // One radiance, one cosine, one shadow, and the BRDF decides how the two halves
        // split it. albedo / pi is what a Lambertian surface is; the colour of a
        // highlight is already inside f.
        direct += (kd * albedo / kPi + specular)
                * light.lights[i].color.rgb * lambert * shade;
    }

    // --- and what the sky sends, once --------------------------------------
    //
    // Outside the loop and outside the shade, because a shadowed surface is still lit
    // by the room and a second light does not add a second sky. ambient.a is the count
    // and ambient.rgb is the floor the panel's slider sets under the irradiance.
    const vec3 ambient = texture(irradianceCube, normal).rgb + light.ambient.rgb;
    const vec3 envSpecular =
        EnvironmentSpecular(normal, toEye, roughness, specularColor);

    // No division by pi here, unlike the direct diffuse above, and the integral says
    // why. irradiance.frag sums L cos sin over a grid and multiplies by pi, which comes
    // to E / pi rather than E -- the true irradiance carries pi squared over the sample
    // count. So the factor a Lambertian surface needs is already in the texture, and
    // dividing again would darken the sky's contribution by pi.
    const vec3 lit = direct + ambient * diffuseColor + envSpecular;

    outColor = vec4(lit, pc.alpha);
}
