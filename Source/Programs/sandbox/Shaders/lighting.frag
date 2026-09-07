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
layout(set = 0, binding = 1) uniform Light {
    vec4 direction;   // xyz = surface toward the light
    vec4 color;       // rgb = colour, a = ambient
} light;

// The same light as a viewpoint. Binding 2 is the buffer the shadow pass was handed,
// so this matrix is the one that drew the map in binding 3.
layout(set = 0, binding = 2) uniform Shadow {
    mat4 lightView;
    mat4 lightProj;
} shadow;

layout(set = 0, binding = 3) uniform sampler2D shadowMap;
layout(set = 0, binding = 6) uniform samplerCube irradianceCube;

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

// Output: 1 where the light reaches this point, 0 where something else got there first
//
// The same function as scene.frag's, reading the same map through the same binding.
// Two copies rather than one, because the two paths are meant to be compared and a
// shared include would hide which of them a change touched.
//
// ndotl steers the bias. A surface edge-on to the light spans many depths inside one
// shadow texel and needs more slack than one facing it.
float ShadowFactor(vec3 worldPos, float ndotl) {
    const vec4 clip = shadow.lightProj * (shadow.lightView * vec4(worldPos, 1.0));
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
    return texture(shadowMap, uvShadow).r + bias < ndc.z ? 0.0 : 1.0;
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
    const vec3 toLight = normalize(light.direction.xyz);
    const vec3 toEye = normalize(camera.viewPos.xyz - worldPos);
    const float lambert = max(dot(normal, toLight), 0.0);

    // Blinn-Phong, carried over from scene.frag unchanged. **This is not PBR** -- no
    // GGX, no Fresnel, no energy conservation. What roughness buys is that it comes
    // from the asset rather than one constant for the whole scene.
    const vec3 halfway = normalize(toLight + toEye);
    const float shininess = mix(256.0, 4.0, roughness);
    const float highlight = pow(max(dot(normal, halfway), 0.0), shininess);
    const float specular = view.useSpecular > 0.5
                         ? highlight * step(0.0001, lambert) : 0.0;

    const float lit = view.useShadow > 0.5 ? ShadowFactor(worldPos, lambert) : 1.0;

    // A metal reflects its own colour and has no diffuse; a dielectric reflects the
    // light's colour and keeps its paint. 0.04 is where most non-metals sit.
    const vec3 specularColor = mix(vec3(0.04), albedo, metallic);
    const vec3 diffuseColor = albedo * (1.0 - metallic);

    // Ambient reaches the reflected term too, standing in for an environment map --
    // without it every metal not facing the light goes black, which Sponza's curtain
    // rods did. Ambient is outside the shadow term: a shadowed surface still sits in
    // the room.
    // The same ambient the forward path uses, from the same cube.
    const vec3 ambient = texture(irradianceCube, normal).rgb + vec3(light.color.a);

    const vec3 colour = (light.color.rgb * lambert * lit + ambient) * diffuseColor
                      + (light.color.rgb * specular * lit + ambient) * specularColor;

    outColor = vec4(colour, 1.0);
}
