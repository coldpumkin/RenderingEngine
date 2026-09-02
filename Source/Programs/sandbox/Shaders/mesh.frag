#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragWorldPos;

// Contract: one sampler2D here, one binding in the scene layout. Nothing reads both.
layout(set = 0, binding = 0) uniform sampler2D tex;

// Contract: same fields as SceneUniform in Pipeline.h.
layout(set = 0, binding = 1) uniform Scene {
    mat4 viewProj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 viewPos;
} scene;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 model;
    float alpha;
} pc;

void main() {
    // Normalized here because interpolation across the triangle shortens it.
    const vec3 normal = normalize(fragNormal);
    const vec3 toLight = normalize(scene.lightDir.xyz);
    const float lambert = max(dot(normal, toLight), 0.0);

    // Blinn-Phong: the halfway vector stands in for the mirror direction, and lines up
    // with the normal exactly when the surface reflects the light at the eye.
    const vec3 toEye = normalize(scene.viewPos.xyz - fragWorldPos);
    const vec3 halfway = normalize(toLight + toEye);
    const float highlight = pow(max(dot(normal, halfway), 0.0), scene.viewPos.w);

    // Gated on lambert: a surface facing away from the light cannot shine.
    const float specular = highlight * step(0.0001, lambert);

    // Diffuse takes the surface colour, specular does not -- a highlight is the light
    // itself reflected, not the paint.
    const vec3 albedo = texture(tex, fragUV).rgb;
    const vec3 lit = (scene.lightColor.rgb * lambert + scene.lightColor.a) * albedo
                   + scene.lightColor.rgb * specular;

    outColor = vec4(lit, pc.alpha);
}
