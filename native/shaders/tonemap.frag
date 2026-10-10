#version 450
#extension GL_GOOGLE_include_directive : require
// Enhanced output: exposure + bloom in linear, tonemap, back to gamma into the shared RGBA8 image (alpha = coverage).
// a.x: source S index, a.y: operator (0 neutral, 1 AgX, 2 ACES fit, 3 clip), a.z: bloom on; b.x: bloom intensity
#include "post.glsl"
layout(location = 0) out vec4 o;
vec3 neutral(vec3 c) {   // Khronos PBR Neutral shoulder without the toe: identity below 0.76, so stock colours pass unchanged
    const float s = 0.76, d = 0.24;
    float pk = max(c.r, max(c.g, c.b));
    if (pk < s) return c;
    float np = 1 - d * d / (pk + d - s);
    c *= np / pk;
    return mix(c, vec3(np), 1 - 1 / (0.15 * (pk - np) + 1));
}
vec3 agx(vec3 v) {   // AgX base contrast, polynomial fit; returns display-encoded
    const mat3 m = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992, 0.878468636469772, 0.0784336, 0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 mi = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368, 1.15190312990417, -0.0980434501171241, -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    v = (clamp(log2(max(m * v, vec3(1e-10))), -12.47393, 4.026069) + 12.47393) / 16.5;
    vec3 x2 = v * v, x4 = x2 * x2;
    v = 15.5 * x4 * x2 - 40.14 * x4 * v + 31.96 * x4 - 6.868 * x2 * v + 0.4298 * x2 + 0.1191 * v - 0.00232;
    return clamp(mi * v, 0, 1);
}
vec3 aces(vec3 x) { x *= 0.8; return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0, 1); }
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 c = texelFetch(S[pc.a.x], p, 0);
    vec3 l = lin(c.rgb) * expo();
    if (pc.a.z != 0) l += textureLod(S[5], gl_FragCoord.xy / vec2(textureSize(S[pc.a.x], 0)), 0).rgb * pc.b.x;
    vec3 g = pc.a.y == 1 ? agx(l) : pow(pc.a.y == 0 ? neutral(l) : pc.a.y == 2 ? aces(l) : clamp(l, 0, 1), vec3(1 / 2.2));
    g += (fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) / 255;   // dither: no banding from the 16-bit source
    o = vec4(g, c.a);
}
