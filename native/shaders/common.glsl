// Shared by all part shaders. Layouts mirror FrameU / MatIn / InstIn in native/core/scene.inl.
#include "frame.glsl"
layout(push_constant) uniform PC { vec4 m0, m1, m2; ivec4 ids; } pc;          // model rows; ids: material, instance, cascade, bone base (vec4 index)
#ifdef SHADOW_FN
layout(set = 0, binding = 3) uniform sampler2DArrayShadow shadowMap;
float shadowAt(vec3 w, float vz, vec3 n) {
    if (F.shadowP.w == 0 || vz >= F.splits.w) return 1;
    int c = vz < F.splits.x ? 0 : vz < F.splits.y ? 1 : vz < F.splits.z ? 2 : 3;
    vec4 sp = F.shadowVP[c] * vec4(w + n * (F.shadowP.y * F.cascTexel[c]), 1);
    vec2 uv = sp.xy * 0.5 + 0.5, o = vec2(0.5 * F.shadowP.z);
    float z = sp.z - F.cascBias[c];
    float s = texture(shadowMap, vec4(uv + vec2(-o.x, -o.y), c, z)) + texture(shadowMap, vec4(uv + vec2(o.x, -o.y), c, z))
            + texture(shadowMap, vec4(uv + vec2(-o.x, o.y), c, z)) + texture(shadowMap, vec4(uv + vec2(o.x, o.y), c, z));
    return max(s * 0.25, 1 - F.shadowP.x);
}
#endif
#ifdef VERT
#ifdef SKIN
layout(std430, set = 0, binding = 5) readonly buffer Bones { vec4 b[]; } BN;   // 3 world-space rows per bone
layout(location = 5) in vec4 inW8; layout(location = 6) in uvec4 inJ;
#endif
vec3 xform(vec4 p, out vec4 m0, out vec4 m1, out vec4 m2) {
#ifdef SKIN
    m0 = vec4(0); m1 = vec4(0); m2 = vec4(0);
    for (int i = 0; i < 4; i++) { int b = pc.ids.w + int(inJ[i]) * 3; m0 += inW8[i] * BN.b[b]; m1 += inW8[i] * BN.b[b + 1]; m2 += inW8[i] * BN.b[b + 2]; }
#else
    m0 = pc.m0; m1 = pc.m1; m2 = pc.m2;
#endif
    return vec3(dot(m0, p), dot(m1, p), dot(m2, p));
}
#endif
#if defined(ENH) && defined(VERT)
// Enhanced: motion vectors from the previous frame's rows (3 per draw, indexed by firstInstance; skinned: row 0 = bounds-centre delta), then TAA jitter
layout(std430, set = 0, binding = 7) readonly buffer Prev { vec4 r[]; } PV;
layout(location = 8) out vec4 oCur; layout(location = 9) out vec4 oPrev;
void motion(vec3 obj, vec3 w, int v) {
    int b = gl_InstanceIndex * 3;
#ifdef SKIN
    vec3 pw = w - PV.r[b].xyz;
#else
    vec4 p = vec4(obj, 1); vec3 pw = vec3(dot(PV.r[b], p), dot(PV.r[b + 1], p), dot(PV.r[b + 2], p));
#endif
    oCur = gl_Position; oPrev = F.pvp[v % 3] * vec4(pw, 1);
    if (v < 3) gl_Position.xy += F.jitter.xy * gl_Position.w;
}
#define MOTION(obj, w, v) motion(obj, w, v)
#elif defined(VERT)
#define MOTION(obj, w, v)
#endif
#if defined(ENH) && !defined(VERT)
// Enhanced G-buffer extras: octahedral normal, roughness / metalness / direct-sun and ambient fractions of the output (AO and contact
// shadows scale these), motion (uv units), albedo (sqrt of linear diffuse colour, a = 1: receives screen-space GI); normal .zw: the IBL reflection's
// share of the output and its weight (SSR replaces it). The shader's own main runs as body().
layout(location = 2) out vec4 oNrm; layout(location = 3) out vec4 oRM; layout(location = 4) out vec2 oMv; layout(location = 5) out vec4 oAlb;
layout(location = 8) in vec4 inCur; layout(location = 9) in vec4 inPrev;
vec3 gN = vec3(0, 0, 1), gRefl = vec3(0); vec4 gRM = vec4(1, 0, 0, 0), gAlb = vec4(0); vec2 gSp = vec2(0); float gReflW = 0;   // gRefl: IBL reflection (linear), gReflW: its weight
#include "enh.glsl"
void body();
void main() {
    body();
    vec3 n = gN / (abs(gN.x) + abs(gN.y) + abs(gN.z) + 1e-20);
    oNrm = vec4(n.z >= 0 ? n.xy : (1 - abs(n.yx)) * vec2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1), gSp);
    oRM = gRM; oAlb = gAlb; oMv = inPrev.w > 0 ? (inCur.xy / inCur.w - inPrev.xy / inPrev.w) * 0.5 : vec2(0);
}
#define main body
#define GBUF(n, r, m) { gN = n; gRM.xy = vec2(r, m); }
#elif !defined(VERT)
#define GBUF(n, r, m)
#endif
