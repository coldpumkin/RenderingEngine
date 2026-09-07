#version 450

// lighting.frag -- one full-screen draw over what the geometry pass wrote
// ============================================================================
//
//   in                          from                       update
//   -------------------------------------------------------------------------
//   uv                          fullscreen.vert            per fragment
//   camera                      set 0, binding 0           per frame
//   light                       set 0, binding 1           per frame
//   shadow shadowMap            set 0, binding 2 and 3     per frame
//   view                        set 0, binding 4           per frame (the panel)
//   gAlbedo gNormal
//   gMaterial gDepth            set 1, bindings 0..3       per frame in flight
//
//   out
//   -------------------------------------------------------------------------
//   outColor                    location 0
//
// Everything view-dependent lives here and nothing else does. The geometry pass wrote
// what a surface *is*; this reads four images and answers what colour each pixel ends
// up. No mesh, no material set, no vertex buffer -- fullscreen.vert builds its three
// points from gl_VertexIndex.
//
// **Why this is a second pass and not the end of the first.** In the forward stage the
// lighting cost is paid per fragment drawn, including the ones a later triangle covers
// over. Here it is paid once per pixel that survived the depth test. Sponza's
// overdraw is what makes that a real difference; the price is the four images and the
// bandwidth to read them back.
//
// **The position is reconstructed, not stored.** uv and depth give a point in NDC;
// inverse(proj) takes it to view space and inverse(view) to world. That is why
// CameraUniform carries the two matrices apart -- their product cannot be undone one
// step at a time, and the view-space step is what every such pass wants.
//
// **Why the G-buffer is set 1 and the frame is set 0.** Nine bindings do not fit in
// the eight kMaxBindingsPerSet allows, so the split is forced. Where to cut is not:
// set 0 here is binding for binding what scene.frag declares, so the two paths read
// the frame through the same layout and only the second set differs. That is the
// whole of the difference, stated in the set numbers.
//
// **Not shared with scene's allocated set.** The two layouts look alike but this
// stage is the only one that reads binding 0, where scene.vert reads it too -- so the
// stageFlags differ, and whether that breaks "identically defined" is a spec question
// we have not answered. A set of its own costs one allocation and no doubt.

layout(location = 0) in vec2 uv;

// Set 0, per frame. Binding for binding the same as scene.frag's, which is what makes
// the two paths comparable: the forward and the deferred lighting read one frame.
//
// All three fields, unlike scene.frag which truncates to viewPos: the two matrices are
// what the position is rebuilt from here.
//
// Contract: field order and types match CameraUniform in Passes.h.
layout(set = 0, binding = 0) uniform Camera {
    mat4 view;
    mat4 proj;
    vec4 viewPos;
} camera;

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

// The same light as a viewpoint. Binding 2 is the buffer the shadow pass was handed,
// so this matrix is the one that drew the map in binding 3.
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


// The panel. **Only three of these mean anything here** -- useNormalMap, useBaseColor
// and useAlphaMask were decided in the geometry pass and are already written into the
// images below. Which switch belongs to which pass is what deferred moves, and the
// panel greys out the ones this stage cannot answer for.
//
// Contract: field order matches ViewOptionsUniform in Gui.h. scene.frag declares the
//           first six and stops; this one reads the seventh.
layout(set = 0, binding = 4) uniform View {
    float useNormalMap;
    float useBaseColor;
    float useSpecular;
    float useAlphaMask;
    float useShadow;
    float useMetallicRoughness;
    float channel;   // 0 lit, else which G-buffer image to show instead
} view;

// Set 1, the geometry pass's four attachments in its own output order. Counted per
// frame in flight, not per material: this stage has no material.
//
// Contract: bindings 0..2 are geometry.frag's colour outputs 0..2 and binding 3 is the
//           depth it wrote. CreateLightingPass fills them in this order.
layout(set = 1, binding = 0) uniform sampler2D gAlbedo;
layout(set = 1, binding = 1) uniform sampler2D gNormal;
layout(set = 1, binding = 2) uniform sampler2D gMaterial;
layout(set = 1, binding = 3) uniform sampler2D gDepth;

layout(location = 0) out vec4 outColor;

// Output: where this pixel's surface is, in world space
//
// depth is what the geometry pass's fixed-function test wrote, in 0..1 --
// GLM_FORCE_DEPTH_ZERO_TO_ONE is on the CMake target, so no remapping of z. uv is
// 0..1 across the screen and NDC x and y run -1..1, which is the one line below.
//
// The w divide is what undoes the perspective: inverse(proj) gives a homogeneous
// point, and the position is that over its own w.
// Output: the clip-space xy a fullscreen uv stands for
//
// **Not uv * 2 - 1.** The passes that draw geometry use a negative viewport height
// (ViewportY::Up), which puts clip y = +1 at the top of the framebuffer; a fullscreen
// triangle is drawn without that flip, so its uv.y is 0 at the top. Anything undoing
// the camera's projection has to read the first convention out of a uv written in the
// second, and the y sign is the whole of the difference.
//
// Getting it wrong is quiet: the picture still fills the screen, and what moves is
// where the shader thinks each pixel is looking.
vec2 ClipFromUv(vec2 uv) {
    return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

vec3 WorldFromDepth(vec2 screenUv, float depth) {
    const vec4 ndc = vec4(ClipFromUv(screenUv), depth, 1.0);
    const vec4 viewSpace = inverse(camera.proj) * ndc;
    const vec4 world = inverse(camera.view) * (viewSpace / viewSpace.w);
    return world.xyz;
}

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
// The same function as scene.frag's, reading the same map through the same binding.
// Two copies rather than one, because the two paths are meant to be compared and a
// shared include would hide which of them a change touched.
//
// ndotl steers the bias. A surface edge-on to the light spans many depths inside one
// shadow texel and needs more slack than one facing it.
float ShadowFactor(int light, vec3 worldPos, float ndotl) {
    const ShadowEntry entry = shadow.lights[light];
    const vec4 clip = entry.lightProj * (entry.lightView * vec4(worldPos, 1.0));
    const vec3 ndc = clip.xyz / clip.w;

    // Contract: this maps ndc to uv the way the shadow pipeline's viewport lays the map
    //           out -- v grows downward, which is ViewportY::Down.
    const vec2 uvShadow = ndc.xy * 0.5 + 0.5;

    // Outside the map is not "in shadow": past the light's ortho box there is no depth
    // to compare against, and the sampler wraps.
    if (uvShadow.x < 0.0 || uvShadow.x > 1.0 || uvShadow.y < 0.0 || uvShadow.y > 1.0
            || ndc.z > 1.0) {
        return 1.0;
    }

    const float bias = max(0.0025 * (1.0 - ndotl), 0.0004);
    return texture(shadowMaps, vec3(uvShadow, float(light))).r + bias < ndc.z ? 0.0 : 1.0;
}

void main() {
    const float depth = texture(gDepth, uv).r;

    // What the G-buffer holds, shown instead of what it is for. The forward path has
    // no such mode and cannot: these images do not exist there, which is the point.
    //
    // Before the coverage test, so the background is part of what is shown -- an
    // empty albedo and a depth of 1 are facts about the buffer.
    if (view.channel > 0.5) {
        if (view.channel < 1.5) { outColor = vec4(texture(gAlbedo, uv).rgb, 1.0); }
        else if (view.channel < 2.5) { outColor = vec4(texture(gNormal, uv).rgb, 1.0); }
        else if (view.channel < 3.5) { outColor = vec4(texture(gMaterial, uv).rgb, 1.0); }
        else {
            // Depth is nearly 1 over most of a perspective frame, so the raw value is
            // a white screen. Stretched against the near end of the range it is a
            // picture; the number is chosen to make Sponza legible and nothing else.
            const float shown = pow(depth, 64.0);
            outColor = vec4(vec3(1.0 - shown), 1.0);
        }
        return;
    }

    // Nothing was drawn here: the depth test left the clear value, which is farthest.
    // The sky pass already put a picture in this pixel, so this one keeps out of it --
    // discard rather than a colour, which is why the attachment loads instead of being
    // written over.
    if (depth >= 1.0) {
        discard;
    }

    const vec3 albedo = texture(gAlbedo, uv).rgb;

    // Folded back out of 0..1. The geometry pass folded it in because the attachment
    // is UNORM.
    const vec3 normal = normalize(texture(gNormal, uv).xyz * 2.0 - 1.0);

    const vec2 material = texture(gMaterial, uv).rg;
    const float roughness = view.useMetallicRoughness > 0.5
                          ? clamp(material.x, 0.04, 1.0) : 1.0;
    const float metallic = view.useMetallicRoughness > 0.5 ? material.y : 0.0;

    const vec3 worldPos = WorldFromDepth(uv, depth);
    const vec3 toEye = normalize(camera.viewPos.xyz - worldPos);

    // What metalness means, in the two places it means anything: a metal reflects its
    // own colour and has no diffuse, a dielectric reflects the light's and keeps its
    // paint.
    const vec3 specularColor = mix(vec3(0.04), albedo, metallic);
    const vec3 diffuseColor = albedo * (1.0 - metallic);

    // **The loop is the whole of what having several lights costs here.** One shadow
    // map is drawn, for the first light, so only that one is shaded; the rest light
    // what they reach and cast nothing.
    const int liveLights = int(light.ambient.a);

    vec3 direct = vec3(0.0);
    for (int i = 0; i < liveLights; ++i) {
        // After this call nothing in the loop knows which kind of light it is.
        const vec4 incoming = LightAt(light.lights[i], worldPos);
        const vec3 toLight = incoming.xyz;
        const float lambert = max(dot(normal, toLight), 0.0) * incoming.w;

        // Cook-Torrance, the same three terms scene.frag uses and the same the
        // environment bakes were built from.
        const vec3 halfway = normalize(toLight + toEye);
        const float ndotv = max(dot(normal, toEye), 1e-4);
        const float ndotl = max(dot(normal, toLight), 0.0);
        const float ndoth = max(dot(normal, halfway), 0.0);
        const float vdoth = max(dot(toEye, halfway), 0.0);

        const float d = DistributionGGX(ndoth, roughness);
        const float g = GeometrySmith(ndotv, ndotl, roughness);
        const vec3 f = FresnelSchlick(vdoth, specularColor);

        const vec3 specular = view.useSpecular > 0.5
                            ? (d * g * f) / max(4.0 * ndotv * ndotl, 1e-4)
                                  * step(1e-4, ndotl)
                            : vec3(0.0);
        const vec3 kd = (vec3(1.0) - f) * (1.0 - metallic);

        // Only the lights a map was drawn for.
        // color.a says which kind of map this light has: 1 a layer of the 2D array,
        // 2 a cube.
        float lit = 1.0;
        if (view.useShadow > 0.5) {
            if (light.lights[i].color.a > 1.5) {
                lit = PointShadowFactor(i, worldPos, lambert);
            } else if (light.lights[i].color.a > 0.5) {
                lit = ShadowFactor(i, worldPos, lambert);
            }
        }

        direct += (kd * albedo / kPi + specular)
                * light.lights[i].color.rgb * lambert * lit;
    }

    // Once, outside the loop and outside the shade: a shadowed surface is still lit by
    // the room, and a second light does not add a second sky.
    const vec3 ambient = texture(irradianceCube, normal).rgb + light.ambient.rgb;
    const vec3 envSpecular =
        EnvironmentSpecular(normal, toEye, roughness, specularColor);

    // No division by pi here, unlike the direct diffuse above, and the integral says
    // why. irradiance.frag sums L cos sin over a grid and multiplies by pi, which comes
    // to E / pi rather than E -- the true irradiance carries pi squared over the sample
    // count. So the factor a Lambertian surface needs is already in the texture, and
    // dividing again would darken the sky's contribution by pi.
    const vec3 colour = direct + ambient * diffuseColor + envSpecular;

    outColor = vec4(colour, 1.0);
}
