// Enhanced post-processing resources (layout mirrors gP in native/core/enhanced.inl). Colours are gamma-space like the stock shaders: linearised for exposure, bloom, tonemap.
// S: 0 hdr, 1 / 2 TAA history, 3 motion, 4 linear depth (nearest), 5 bloom chain, 6 normal, 7 Hi-Z, 8 rough / metal / sun / ambient fractions, 9 albedo,
//    10 AO + contact (half-res), 11 GI, 12 / 13 AO history, 14 / 15 GI history
// Wc: 0 / 1 TAA history, 2..6 bloom mips, 7 hdr, 8 AO, 9 GI, 10 / 11 AO history, 12 / 13 GI history, 14 SSR (half-res)   Wz: Hi-Z mips, 15 depth copy (ocean)
layout(set = 0, binding = 0) uniform sampler2D S[16];
layout(set = 0, binding = 1, rgba16f) uniform image2D Wc[16];
layout(set = 0, binding = 2, r32f) uniform image2D Wz[16];
layout(std430, set = 0, binding = 3) buffer Expo { vec4 v; uint hist[256]; } E;   // v: exposure, mean log2 luminance, -, -
layout(push_constant) uniform PP { ivec4 a; vec4 b; vec4 c; } pc;                  // per pass; c.w > 0: fixed exposure (auto-exposure off)
float expo() { return pc.c.w > 0 ? pc.c.w : E.v.x; }
vec3 lin(vec3 c) { return pow(max(c, vec3(0)), vec3(2.2)); }
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float dec(float d) { return d >= 1e19 && d < 1e38 ? d * 1e-20 : d; }   // IVA depth is tagged x1e20
#ifdef FRAME   // screen-space passes: camera from the Frame UBO
#define ENH
#define FRAME_BINDING 4
#include "frame.glsl"
float dz(ivec2 q) { float d = dec(texelFetch(S[4], q, 0).r); return d > 0 && d < 1e30 ? d : 0; }   // local-view depth, 0 elsewhere
vec3 vpos(vec2 uv, float d) {   // camera-relative world position
    vec2 n = uv * 2 - 1 - F.jitter.xy;
    return F.camR.xyz * (d * (n.x + F.proj.z) / F.proj.x) + F.camU.xyz * (d * (F.proj.w - n.y) / F.proj.y) + F.camF.xyz * d;
}
vec2 suv(vec3 v) {   // camera-relative world position -> uv
    float d = dot(v, F.camF.xyz);
    return (vec2(dot(v, F.camR.xyz) * F.proj.x / d - F.proj.z, F.proj.w - dot(v, F.camU.xyz) * F.proj.y / d) + F.jitter.xy) * 0.5 + 0.5;
}
vec3 oct(vec2 e) { vec3 v = vec3(e, 1 - abs(e.x) - abs(e.y)); if (v.z < 0) v.xy = (1 - abs(v.yx)) * vec2(v.x >= 0 ? 1 : -1, v.y >= 0 ? 1 : -1); return normalize(v); }
float ign(vec2 p) { p += float(F.enh.y & 63) * 5.588238; return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
#endif
