#version 450

// The lighting stage: one full-screen draw over what the geometry pass wrote
// ============================================================================
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

layout(location = 0) in vec2 uv;

// Set 0 is this pass's own, and every binding in it is something the geometry pass
// left or something the frame decided. Four images and two blocks.
//
// Contract: bindings 0..3 are the geometry pass's attachments in its own output order,
//           and 4..5 are the frame's. UpdateSet fills them by position.
layout(set = 0, binding = 0) uniform sampler2D gAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gNormal;
layout(set = 0, binding = 2) uniform sampler2D gMaterial;
layout(set = 0, binding = 3) uniform sampler2D gDepth;

// Contract: field order and types match CameraUniform in Passes.h. All three fields,
//           unlike the vertex stage which truncates after two: this one needs viewPos
//           and both inverses.
layout(set = 0, binding = 4) uniform Camera {
    mat4 view;
    mat4 proj;
    vec4 viewPos;
} camera;

// Contract: same fields as LightUniform in Passes.h.
layout(set = 0, binding = 5) uniform Light {
    vec4 direction;   // xyz toward the light
    vec4 color;       // rgb colour, a ambient
} light;

layout(location = 0) out vec4 outColor;

// Output: where this pixel's surface is, in world space
//
// depth is what the geometry pass's fixed-function test wrote, in 0..1 --
// GLM_FORCE_DEPTH_ZERO_TO_ONE is on the CMake target, so no remapping of z. uv is
// 0..1 across the screen and NDC x and y run -1..1, which is the one line below.
//
// The w divide is what undoes the perspective: inverse(proj) gives a homogeneous
// point, and the position is that over its own w.
vec3 WorldFromDepth(vec2 screenUv, float depth) {
    const vec4 ndc = vec4(screenUv * 2.0 - 1.0, depth, 1.0);
    const vec4 viewSpace = inverse(camera.proj) * ndc;
    const vec4 world = inverse(camera.view) * (viewSpace / viewSpace.w);
    return world.xyz;
}

void main() {
    const float depth = texture(gDepth, uv).r;

    // Nothing was drawn here: the depth test left the clear value, which is farthest.
    // The background is the ambient alone rather than a lit surface that is not there.
    if (depth >= 1.0) {
        outColor = vec4(light.color.a * light.color.rgb, 1.0);
        return;
    }

    const vec3 albedo = texture(gAlbedo, uv).rgb;

    // Folded back out of 0..1. The geometry pass folded it in because the attachment
    // is UNORM.
    const vec3 normal = normalize(texture(gNormal, uv).xyz * 2.0 - 1.0);

    const vec2 material = texture(gMaterial, uv).rg;
    const float roughness = clamp(material.x, 0.04, 1.0);
    const float metallic = material.y;

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
    const float specular = highlight * step(0.0001, lambert);

    // A metal reflects its own colour and has no diffuse; a dielectric reflects the
    // light's colour and keeps its paint. 0.04 is where most non-metals sit.
    const vec3 specularColor = mix(vec3(0.04), albedo, metallic);
    const vec3 diffuseColor = albedo * (1.0 - metallic);

    // Ambient reaches the reflected term too, standing in for an environment map --
    // without it every metal not facing the light goes black, which Sponza's curtain
    // rods did.
    const vec3 lit = (light.color.rgb * lambert + light.color.a) * diffuseColor
                   + (light.color.rgb * specular + light.color.a) * specularColor;

    outColor = vec4(lit, 1.0);
}
