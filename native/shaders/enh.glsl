// Enhanced scene-shader helpers (fragment): IBL (8b), soft sun shadows and shadowed spot lights (8c), atmosphere transmittance and aerial
// perspective (8d). Bindings mirror initScene in native/core/scene.inl.
const int EF_SOFTSH = 256, EF_CONTACT = 512, EF_GTAO = 1024, EF_SSGI = 2048, EF_SPOTSH = 4096, EF_ATMO = 8192, EF_OCEAN = 16384, EF_FFT = 32768,
          EF_TERRAIN = 65536, EF_POM = 131072, EF_SSR = 262144;
layout(set = 0, binding = 8) uniform samplerCube iblCube;
layout(std430, set = 0, binding = 9) readonly buffer IblSH { vec4 c[9]; } SH;   // cosine-convolved, / pi
layout(set = 0, binding = 10) uniform sampler2DArray shadowRaw;
layout(set = 0, binding = 11) uniform sampler2D zCopy;
layout(set = 0, binding = 12) uniform sampler3D apVol;
layout(set = 0, binding = 13) uniform sampler2D skyLut;
layout(set = 0, binding = 14) uniform sampler2D transLut;
layout(set = 0, binding = 15) uniform sampler2DArrayShadow spotMap;
layout(set = 0, binding = 16) uniform sampler2D oceanNrm;
layout(set = 0, binding = 17) uniform sampler2D multiLut;
#include "atmo.glsl"
bool ef(int b) { return (F.enh.x & b) != 0; }
vec3 toLin(vec3 c) { return pow(max(c, vec3(0)), vec3(2.2)); }
float lumi(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float pow5(float x) { float x2 = x * x; return x2 * x2 * x; }
float ign(vec2 p) { p += float(F.enh.y & 63) * 5.588238; return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
vec3 shIrr(vec3 d) {
    vec3 r = SH.c[0].rgb * 0.282095 + 0.488603 * (SH.c[1].rgb * d.y + SH.c[2].rgb * d.z + SH.c[3].rgb * d.x)
           + 1.092548 * (SH.c[4].rgb * d.x * d.y + SH.c[5].rgb * d.y * d.z + SH.c[7].rgb * d.x * d.z)
           + 0.315392 * SH.c[6].rgb * (3 * d.z * d.z - 1) + 0.546274 * SH.c[8].rgb * (d.x * d.x - d.y * d.y);
    return max(r, vec3(0));
}
vec3 pbrLit(vec3 L, vec3 lc, vec3 N, vec3 V, vec3 dc, vec3 f0, float pr, float r) {   // GGX (Smith-joint) + Burley; lights carry Unity's pi
    vec3 H = normalize(L + V);
    float nl = clamp(dot(N, L), 0, 1), nh = clamp(dot(N, H), 0, 1), lh = clamp(dot(L, H), 0, 1), nv = abs(dot(N, V)) + 1e-5;
    float fd90 = 0.5 + 2 * lh * lh * pr, dif = (1 + (fd90 - 1) * pow5(1 - nl)) * (1 + (fd90 - 1) * pow5(1 - nv)) * nl;
    float vis = 0.5 / (nl * (nv * (1 - r) + r) + nv * (nl * (1 - r) + r) + 1e-5), a2 = r * r, d = (nh * a2 - nh) * nh + 1;
    return (dc * dif + min(vis * a2 / (d * d + 1e-7), 100.0) * nl * (f0 + (1 - f0) * pow5(1 - lh))) * lc;
}
vec2 envBrdf(float pr, float nv) {   // Karis' analytic split-sum terms: f0 scale, bias
    vec4 k = pr * vec4(-1, -0.0275, -0.572, 0.022) + vec4(1, 0.0425, 1.04, -0.04);
    float a = min(k.x * k.x, exp2(-9.28 * nv)) * k.x + k.y;
    return vec2(-1.04 * a + k.z, 1.04 * a + k.w);
}

#ifdef SHADOW_FN
const vec2 PD[12] = vec2[](vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457), vec2(-0.203, 0.621), vec2(0.962, -0.195), vec2(0.473, -0.480),
                           vec2(0.519, 0.767), vec2(0.185, -0.893), vec2(0.507, 0.064), vec2(0.896, 0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));
// PCSS: blocker search, then a penumbra from the sun's angular size (F.sfx.x, softness included) and the receiver-blocker distance
float sunShadow(vec3 w, float vz, vec3 n) {
    if (!ef(EF_SOFTSH)) return shadowAt(w, vz, n);
    if (F.shadowP.w == 0 || vz >= F.splits.w) return 1;
    int c = vz < F.splits.x ? 0 : vz < F.splits.y ? 1 : vz < F.splits.z ? 2 : 3;
    vec4 sp = F.shadowVP[c] * vec4(w + n * (F.shadowP.y * F.cascTexel[c]), 1);
    vec2 uv = sp.xy * 0.5 + 0.5; float z = sp.z - F.cascBias[c];
    float depthM = F.cascTexel[c] / F.cascBias[c], uvPerM = F.shadowP.z / F.cascTexel[c], a = ign(gl_FragCoord.xy) * 6.2831853;
    mat2 R = mat2(cos(a), sin(a), -sin(a), cos(a));
    float sr = clamp(F.sfx.x * 40 * uvPerM, 1.5 * F.shadowP.z, 12 * F.shadowP.z), bs = 0, bn = 0;
    for (int i = 0; i < 8; i++) { float d = textureLod(shadowRaw, vec3(uv + R * PD[i] * sr, c), 0).r; if (d < z) { bs += d; bn++; } }
    if (bn == 0) return 1;
    float pr = clamp((z - bs / bn) * depthM * F.sfx.x * uvPerM, 0.7 * F.shadowP.z, sr), s = 0;
    for (int i = 0; i < 12; i++) s += texture(shadowMap, vec4(uv + R * PD[i] * pr, c, z));
    return max(s / 12, 1 - F.shadowP.x);
}
#endif
float spotShadow(int i, vec3 w, vec3 n) {   // light i: its shadow layer, if it has one
    int l = F.spotIdx[i >> 2][i & 3];
    if (l < 0 || !ef(EF_SPOTSH)) return 1;
    vec4 sp = F.spotVP[l] * vec4(w, 1);
    if (sp.w <= 0) return 1;
    sp = F.spotVP[l] * vec4(w + n * (0.004 * sp.w), 1);
    vec3 q = sp.xyz / sp.w; vec2 uv = q.xy * 0.5 + 0.5, o = vec2(0.5 / 1024);
    if (any(greaterThan(abs(q.xy), vec2(1)))) return 1;
    return 0.25 * (texture(spotMap, vec4(uv + vec2(-o.x, -o.y), l, q.z)) + texture(spotMap, vec4(uv + vec2(o.x, -o.y), l, q.z))
                 + texture(spotMap, vec4(uv + vec2(-o.x, o.y), l, q.z)) + texture(spotMap, vec4(uv + o, l, q.z)));
}
bool atmOn() { return ef(EF_ATMO) && F.atmX.z > 0; }
vec3 sunAtm(vec3 w) { return atmOn() ? sunTrans(w - F.atmC.xyz) : vec3(1); }
vec4 apAt(vec3 w) {   // aerial perspective between the camera and w (local view): in-scattered light, mean transmittance
    if (!atmOn() || pc.ids.z >= 3) return vec4(0, 0, 0, 1);
    return textureLod(apVol, vec3(gl_FragCoord.xy * F.jitter.zw, sqrt(clamp(length(w - F.camPos.xyz) / F.atmP.w, 0, 1))), 0);
}
bool underwater() { return atmOn() && length(F.camPos.xyz - F.atmC.xyz) < F.atmC.w; }
// Linear colour -> output: aerial perspective, G-buffer fractions (direct sun, ambient) for the AO / contact-shadow resolve, gamma
vec3 finish(vec3 col, vec3 sun, vec3 amb, vec3 w) {
    vec4 ap = apAt(w); vec3 o = col * ap.a + ap.rgb; float l = max(lumi(o), 1e-6);
    gRM.zw = clamp(vec2(lumi(sun), lumi(amb)) * ap.a / l, 0, 1);
    gSp = clamp(vec2(lumi(gRefl) / l, gReflW) * ap.a, 0, 1);
    return pow(max(o, vec3(0)), vec3(1 / 2.2));
}
