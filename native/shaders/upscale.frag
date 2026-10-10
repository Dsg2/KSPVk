#version 450
// Render scale: the scene-size colour (Catmull-Rom; alpha bilinear) and linear depth (nearest) into the full-size shared images. a.xy: 1 / output size
layout(set = 0, binding = 0) uniform sampler2D C;
layout(set = 0, binding = 1) uniform sampler2D Z;
layout(push_constant) uniform PP { vec4 a; } pc;
layout(location = 0) out vec4 o;
layout(location = 1) out vec4 z;
void main() {
    vec2 uv = gl_FragCoord.xy * pc.a.xy, sz = vec2(textureSize(C, 0)), p = uv * sz, t1 = floor(p - 0.5) + 0.5, f = p - t1;
    vec2 w0 = f * (-0.5 + f * (1 - 0.5 * f)), w1 = 1 + f * f * (-2.5 + 1.5 * f), w2 = f * (0.5 + f * (2 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2, t0 = (t1 - 1) / sz, t3 = (t1 + 2) / sz, t12 = (t1 + w2 / w12) / sz;
    vec3 c = textureLod(C, vec2(t12.x, t0.y), 0).rgb * w12.x * w0.y + textureLod(C, vec2(t0.x, t12.y), 0).rgb * w0.x * w12.y
           + textureLod(C, t12, 0).rgb * w12.x * w12.y + textureLod(C, vec2(t3.x, t12.y), 0).rgb * w3.x * w12.y
           + textureLod(C, vec2(t12.x, t3.y), 0).rgb * w12.x * w3.y;
    float w = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    o = vec4(clamp(c / w, 0, 1), textureLod(C, uv, 0).a);
    z = vec4(texelFetch(Z, ivec2(uv * vec2(textureSize(Z, 0))), 0).r);
}
