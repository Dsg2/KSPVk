#version 450
#extension GL_GOOGLE_include_directive : require
// Uber-shader for opaque objects: KSP/* parts, KSP/Scenery/*, Legacy Shaders/*, Standard (both setups). Maths decoded from the stock DXBC, gamma space.
#define SHADOW_FN
#include "common.glsl"
layout(constant_id = 0) const bool CUTOFF = false;
const int F_BUMP = 1, F_SPEC = 2, F_EMIS = 4, F_MAPPED = 8, F_UNLIT = 16, F_NOVCOL = 32, F_STD = 128, F_STDSPEC = 256, F_RGAG = 512,
          F_LMSHADER = 1024, F_GROUND = 2048, F_MUL = 4096, F_GLOSSA = 8192, F_GLOSSMAP = 16384, F_DETAIL = 32768, F_WRAP = 65536, F_FLAG = 131072, F_INTSPACE = 262144, F_AMIX = 524288;
struct Mat { vec4 color, specColor, emisColor, burn, mainST, bumpST, emisST, specST, p, q; ivec4 tex; ivec4 info; vec4 x0, x1; ivec4 tex2; vec4 pad[9]; };
// p: shininess (Mapped: smoothness), cutoff, rimFalloff, light boost (Lightwrapped)   q: Mapped / InternalSpace ambient multiplier, spec tint, InternalSpace occlusion
// InternalSpace: _LightMap in tex.z (emisST, uv1), _LightColor1 / 2 in color / emisColor
// Diffuse Detail / Flag: detail map tex.z (emisST), detail colour in specColor   x0: Standard metallic, smoothness, gloss map scale, occlusion strength | Ground KSC: near / far grass tiling, far blend distance
// x1: bump scale, Ground KSC Specular mid grass tiling / blend distance   tex2: occlusion map | mid grass
struct Inst { vec4 rim, temp, p, emis, lmST; ivec4 lm; };   // per renderer: rim / temperature / emissive (MPB; p: falloff, opacity, emissive set), lightmap scale-offset; lm: lightmap texture (-1 none), flags (1 = receives no shadows, 2 = IVA: sun scaled to IVASun by ambEq.w), layer bit
layout(std430, set = 0, binding = 1) readonly buffer Mats { Mat m[]; } M;
layout(std430, set = 0, binding = 2) readonly buffer Insts { Inst p[]; } P;
layout(set = 0, binding = 4) uniform sampler2D tex[4096];
layout(set = 0, binding = 6) uniform samplerCube envCube;
layout(location = 0) in vec3 inW; layout(location = 1) in vec3 inN; layout(location = 2) in vec4 inT;
layout(location = 3) in vec2 inUv; layout(location = 4) in vec4 inCol; layout(location = 5) in float inVz;
layout(location = 6) in vec2 inUv1; layout(location = 7) in vec3 inObj;
layout(location = 0) out vec4 oCol; layout(location = 1) out float oDepth;

#ifndef ENH
float pow5(float x) { float x2 = x * x; return x2 * x2 * x; }
#endif
vec3 ambientAt(vec3 n) { return F.env.x < 0.5 ? F.ambient.rgb : n.y > 0 ? mix(F.ambEq.rgb, F.ambient.rgb, n.y) : mix(F.ambEq.rgb, F.ambGround.rgb, -n.y); }

// Legacy shaders: Lambert + Blinn-Phong (spec gloss = albedo alpha). Mapped: approximation of Unity's Standard-specular BRDF.
vec3 gWrap; float gBoost;   // KSP/Lightwrapped/Specular: per-channel wrap (_Color), _LightBoost
vec3 lit(vec3 L, vec3 lc, vec3 N, vec3 V, vec4 alb, vec3 diff, vec3 sc, float n, int fl, vec3 specColor, float shin) {
    float ndl = max(0, dot(N, L)), ndh = max(0, dot(N, normalize(L + V)));
    if ((fl & F_WRAP) != 0) { float g = shin * alb.a; return (alb.rgb * (dot(N, L) * (1 - gWrap) + gWrap) + specColor * pow(ndh, g * 128) * g) * lc * gBoost; }
    if ((fl & F_MAPPED) != 0) return (diff + sc * ((n + 8) / 8) * pow(ndh, n)) * lc * ndl;
    vec3 c = alb.rgb * lc * ndl;
    if ((fl & F_SPEC) != 0) c += lc * specColor * pow(ndh, shin * 128) * alb.a;
    return c;
}
// Unity BRDF1 (Disney diffuse, Smith-joint GGX), gamma-space variant
vec3 stdLit(vec3 L, vec3 lc, vec3 N, vec3 V, vec3 dc, vec3 sc, float pr, float r) {
    vec3 H = normalize(L + V);
    float nl = clamp(dot(N, L), 0, 1), nh = clamp(dot(N, H), 0, 1), lh = clamp(dot(L, H), 0, 1), nv = abs(dot(N, V));
    float fd90 = 0.5 + 2 * lh * lh * pr, dif = (1 + (fd90 - 1) * pow5(1 - nl)) * (1 + (fd90 - 1) * pow5(1 - nv)) * nl;
    float vis = 0.5 / (nl * (nv * (1 - r) + r) + nv * (nl * (1 - r) + r) + 1e-5);
    float a2 = r * r, d = (nh * a2 - nh) * nh + 1, s = sqrt(max(1e-4, vis * 0.31831 * a2 / (d * d + 1e-7) * 3.14159)) * nl;
    if (dot(sc, sc) == 0) s = 0;
    return (dc * dif + s * (sc + (1 - sc) * pow5(1 - lh))) * lc;
}
// col.w = type (0 point, 1 spot, 2 directional) + 4 * culling mask over the 8 local layers; m: this renderer's layer bit
vec3 lightDir(Light l, int m, out float a) {
    int ty = int(l.col.w) & 3;
    a = (int(l.col.w) >> 2 & m) != 0 ? 1 : 0;
    if (ty == 2 || a == 0) return l.dir.xyz;
    vec3 d = l.pos.xyz - inW;
    float x = dot(d, d) / (l.pos.w * l.pos.w);
    if (x >= 1) { a = 0; return vec3(0, 1, 0); }
    vec3 Ll = d * inversesqrt(max(dot(d, d), 1e-8));
    a = clamp((1 - x) * 5, 0, 1) / (1 + 25 * x);                 // Unity's built-in point attenuation LUT, approximately
    if (ty == 1) a *= smoothstep(l.dir.w, mix(l.dir.w, 1, 0.4), dot(-Ll, l.dir.xyz));
    return Ll;
}

#ifdef ENH
// Enhanced physically based path (8b-8d): linear light, GGX (Smith-joint) + Burley diffuse, prefiltered IBL cube + SH9 irradiance,
// soft sun shadows, shadowed spot lights, sunlight through the atmosphere, aerial perspective.
// Stock materials converted per pixel: Blinn-Phong exponent -> GGX roughness, gloss mask -> roughness blend, a bright neutral highlight over a
// dark diffuse (texture average from its last mip) -> metal. Per-part overrides (KSPVK_PBR config) in pt.lm.w.
vec3 pbr(Mat mt, Inst pt, int fl, vec4 alb, vec3 N, vec3 N0, vec3 V, vec3 emis) {
    vec3 b0 = toLin(alb.rgb), mf0 = b0, f0 = vec3(-1);
    float pr, met = 0, occ = 1;
    if ((fl & F_STD) != 0) {
        vec4 g = texB(tex[mt.tex.w], inUv * mt.mainST.xy + mt.mainST.zw);
        pr = 1 - ((fl & F_GLOSSA) != 0 ? alb.a * ((fl & F_GROUND) != 0 ? 1 : mt.x0.z) : (fl & F_GLOSSMAP) != 0 ? g.a * mt.x0.z : mt.x0.y);
        if ((fl & F_STDSPEC) != 0) f0 = toLin((fl & F_GLOSSMAP) != 0 ? g.rgb : mt.specColor.rgb);
        else met = (fl & F_GLOSSMAP) != 0 ? g.r : mt.x0.x;
        if ((fl & F_GROUND) == 0) occ = mix(1, texB(tex[mt.tex2.x], inUv * mt.mainST.xy + mt.mainST.zw).g, mt.x0.w);
    } else {
        bool spec = (fl & (F_SPEC | F_WRAP)) != 0, mapped = (fl & F_MAPPED) != 0;
        vec3 sc = toLin(mt.specColor.rgb);
        float gloss = spec ? alb.a : 0;
        if (mapped) { sc = toLin(texB(tex[mt.tex.w], inUv * mt.specST.xy + mt.specST.zw).rgb); sc = mix(sc, sc * b0, mt.q.y); gloss = 1; }
        pr = mapped ? 1 - mt.p.x : spec ? mix(0.85, pow(2 / (mt.p.x * 128 + 2), 0.25), clamp(gloss * 2, 0, 1)) : 0.8;
        if (spec || mapped) met = smoothstep(0.1, 0.4, lumi(sc) * gloss - lumi(toLin(textureLod(tex[mt.tex.x], vec2(0.5), 16).rgb * mt.color.rgb)));
        mf0 = max(b0, sc * gloss);
    }
    int ov = pt.lm.w;
    if ((ov & 1) != 0) pr = float(ov >> 8 & 255) / 255;
    if ((ov & 2) != 0) met = float(ov >> 16 & 255) / 255;
    vec3 dc;
    if (f0.r < 0) { f0 = mix(vec3(0.04), mf0, met); dc = b0 * (1 - met) * 0.96; }
    else dc = b0 * (1 - max(f0.r, max(f0.g, f0.b)));
    pr = clamp(pr, 0.02, 1);
    float r = max(pr * pr, 0.002), nv = clamp(abs(dot(N, V)), 1e-4, 1);
    GBUF(N, pr, met);
    gAlb = vec4(sqrt(dc), 1);

    bool lm = pt.lm.x >= 0, ibl = (int(F.ibl.x) & 2) != 0;
    vec3 sun = vec3(0), col = vec3(0);
    if (!lm || (fl & F_LMSHADER) != 0)
        sun = pbrLit(F.lightDir.xyz, toLin(F.lightCol.rgb) * sunAtm(inW) * ((pt.lm.y & 1) != 0 ? 1 : sunShadow(inW, inVz, N0)), N, V, dc, f0, pr, r);
    for (int i = 0; i < F.nLights.x; i++) {
        float a; vec3 L = lightDir(F.lights[i], pt.lm.z, a);
        if (a > 0) col += pbrLit(L, toLin(F.lights[i].col.rgb) * a * spotShadow(i, inW, N0), N, V, dc, f0, pr, r);
    }
    vec3 irr = toLin(ambientAt(N)) * (ibl ? F.ibl.y : 1);
    if (ibl) irr += shIrr(N) * F.ibl.w;
    if (lm) { vec4 l = texB(tex[pt.lm.x], inUv1 * pt.lmST.xy + pt.lmST.zw); irr = toLin(l.rgb * l.a * F.env.z); }
    vec3 R = reflect(-V, N);
    vec3 pre = ibl ? textureLod(iblCube, R, pr * F.ibl.z).rgb * F.ibl.w : toLin(textureLod(envCube, R, pr * (1.7 - 0.7 * pr) * min(6, F.env.w)).rgb * F.env.y);
    vec2 eb = envBrdf(pr, nv); float hz = clamp(1 + dot(R, N0), 0, 1);   // hz: reflections below the geometric surface fade
    vec3 sw = (f0 * eb.x + eb.y) * hz * hz * occ, amb = dc * irr * occ + pre * sw;
    gRefl = pre * sw; gReflW = lumi(sw);
    return finish(col + sun + amb + toLin((fl & F_STD) != 0 ? emis : emis * pt.p.y), sun, amb, inW);
}
#endif

void main() {
    Mat mt = M.m[pc.ids.x]; Inst pt = P.p[pc.ids.y];
    int fl = mt.info.x;
    oDepth = (pt.lm.y & 2) != 0 ? inVz * 1e20 : inVz;   // IVA: tagged x1e20 so the InternalCamera composite (mode 4) can find it
    vec3 V = normalize(F.camPos.xyz - inW);
    vec4 alb;
    if ((fl & F_GROUND) != 0) {   // KSP/Scenery/Diffuse Ground KSC (+ Specular: mid layer, smoothness in alpha): grass on object xz, distance-blended, under a masked tarmac layer
        float d = length(F.camPos.xyz - inW);
        vec4 grass = texB(tex[mt.tex.z], inObj.xz * mt.x0.x);
        if ((fl & F_STD) != 0) {
            grass = mix(grass, texB(tex[mt.tex2.x], inObj.xz * mt.x1.y), smoothstep(0, 1, d / mt.x1.z));
            grass = mix(grass, texB(tex[mt.tex.w], inObj.xz * mt.x0.y), smoothstep(0, 1, (d - mt.x1.z) / max(1e-3, mt.x0.z - mt.x1.z)));
        } else grass = mix(grass, texB(tex[mt.tex.w], inObj.xz * mt.x0.y), smoothstep(0, 1, d / mt.x0.z));
        alb = mix(grass * mt.color, texB(tex[mt.tex.x], inUv * mt.mainST.xy + mt.mainST.zw), texB(tex[mt.tex.y], inUv1 * mt.bumpST.xy + mt.bumpST.zw).r);
        if ((fl & F_STD) == 0) alb.a = 1;
    } else {
        vec4 t = texB(tex[mt.tex.x], inUv * mt.mainST.xy + mt.mainST.zw);
        alb = (fl & F_AMIX) != 0 ? vec4(mix(mt.color.rgb, t.rgb * mt.color.rgb, t.a), t.a) : (fl & F_MUL) != 0 ? vec4(mix(mt.color.rgb, t.rgb, mt.color.a), 1) : (fl & (F_WRAP | F_INTSPACE)) != 0 ? t : t * mt.burn * mt.color * ((fl & F_NOVCOL) != 0 ? vec4(1) : inCol);
        gWrap = mt.color.rgb * 0.5; gBoost = mt.p.w;
        if ((fl & (F_DETAIL | F_FLAG)) != 0) { vec4 d = texB(tex[mt.tex.z], inUv * mt.emisST.xy + mt.emisST.zw) * mt.specColor; alb.rgb = mix(alb.rgb, alb.rgb * d.rgb, d.a); }   // KSP/Scenery/Diffuse Detail
    }
    if (CUTOFF && alb.a < mt.p.y) discard;
    vec3 N0 = normalize(inN), N = N0;
    if ((fl & F_BUMP) != 0) {
        vec4 nt = texB(tex[mt.tex.y], inUv * mt.bumpST.xy + mt.bumpST.zw);
        vec2 xy = ((fl & F_RGAG) != 0 ? vec2(nt.x * nt.w, nt.y) : nt.wy) * 2 - 1;
        if ((fl & F_STD) != 0) xy *= mt.x1.x;
        vec3 tn = vec3(xy, sqrt(1 - min(1, dot(xy, xy))));
        vec3 T = normalize(inT.xyz - N * dot(N, inT.xyz)), B = cross(N, T) * inT.w;
        N = normalize(T * tn.x + B * tn.y + N * tn.z);
    }
    GBUF(N, (fl & (F_SPEC | F_WRAP | F_INTSPACE)) != 0 ? sqrt(2 / (mt.p.x * 128 + 2)) : (fl & F_MAPPED) != 0 ? 1 - mt.p.x : 1, 0);
    float rimF = pt.p.x > 0 ? pt.p.x : mt.p.z;
    vec3 emis = pow(1 - clamp(dot(V, N), 0, 1), rimF) * pt.rim.rgb * pt.rim.a + pt.temp.rgb * pt.temp.a;
    if ((fl & F_EMIS) != 0) emis += texB(tex[mt.tex.z], inUv * mt.emisST.xy + mt.emisST.zw).rgb * (pt.p.z != 0 ? pt.emis.rgb * pt.emis.a : mt.emisColor.rgb * mt.emisColor.a);
    if ((fl & F_UNLIT) != 0) { oCol = vec4(alb.rgb + emis * pt.p.y, 1); return; }
#ifdef ENH
    if ((int(F.ibl.x) & 1) != 0 && (pt.lm.y & 2) == 0 && (fl & F_INTSPACE) == 0) { oCol = vec4(pbr(mt, pt, fl, alb, N, N0, V, emis), 1); return; }   // IVA keeps its baked look
#endif

    float sh = shadowAt(inW, inVz, N0);
    if ((pt.lm.y & 1) != 0) sh = 1;
    vec3 sun = F.lightCol.rgb * sh * ((pt.lm.y & 2) != 0 ? F.ambEq.w : 1), amb = ambientAt(N), col;
    if ((fl & F_INTSPACE) != 0) {   // KSP/InternalSpace: baked two-colour light map with occlusion, plus the IVA sun / internal lights
        vec4 L = texB(tex[mt.tex.z], inUv1 * mt.emisST.xy + mt.emisST.zw);
        float occ = pow(L.w, mt.q.z), g = 2 * alb.a * mt.specColor.r;
        vec3 lc = L.x * mt.color.rgb + L.y * mt.emisColor.rgb, k = lc * (1 - occ) + occ, a2 = alb.rgb * inCol.rgb * k;
        col = alb.rgb * (lc + F.ambient.rgb * L.z * mt.q.x) * k;   // glstate_lightmodel_ambient is half the ambient; the shader doubles it
        col += (a2 * max(0, dot(N, F.lightDir.xyz)) + mt.specColor.rgb * pow(max(0, dot(N, normalize(F.lightDir.xyz + V))), mt.p.x * 128) * g) * sun;
        for (int i = 0; i < F.nLights.x; i++) { float a; vec3 Ll = lightDir(F.lights[i], pt.lm.z, a); if (a > 0) col += (a2 * max(0, dot(N, Ll)) + mt.specColor.rgb * pow(max(0, dot(N, normalize(Ll + V))), mt.p.x * 128) * g) * F.lights[i].col.rgb * a; }
        oCol = vec4(col + emis * pt.p.y, 1);
        return;
    }
    if ((fl & F_STD) != 0) {
        vec4 g = texB(tex[mt.tex.w], inUv * mt.mainST.xy + mt.mainST.zw);   // metallic-gloss / spec-gloss map
        float sm = (fl & F_GLOSSA) != 0 ? alb.a * ((fl & F_GROUND) != 0 ? 1 : mt.x0.z) : (fl & F_GLOSSMAP) != 0 ? g.a * mt.x0.z : mt.x0.y, omr;
        vec3 sc;
        if ((fl & F_STDSPEC) != 0) { sc = (fl & F_GLOSSMAP) != 0 ? g.rgb : mt.specColor.rgb; omr = 1 - max(sc.r, max(sc.g, sc.b)); }
        else { float met = (fl & F_GLOSSMAP) != 0 ? g.r : mt.x0.x; sc = mix(vec3(0.220916301), alb.rgb, met); omr = 0.779083699 * (1 - met); }
        vec3 dc = alb.rgb * omr;
        float occ = (fl & F_GROUND) != 0 ? 1 : mix(1, texB(tex[mt.tex2.x], inUv * mt.mainST.xy + mt.mainST.zw).g, mt.x0.w);
        float pr = 1 - sm, r = max(pr * pr, 0.002), nv = abs(dot(N, V));
        GBUF(N, pr, (fl & F_STDSPEC) != 0 ? 0 : (fl & F_GLOSSMAP) != 0 ? g.r : mt.x0.x);
        vec3 env = textureLod(envCube, reflect(-V, N), pr * (1.7 - 0.7 * pr) * min(6, F.env.w)).rgb * F.env.y * occ;
        col = dc * amb * occ + (1 - 0.28 * r * pr) * env * mix(sc, vec3(clamp(sm + 1 - omr, 0, 1)), pow5(1 - nv));
        col += stdLit(F.lightDir.xyz, sun, N, V, dc, sc, pr, r);
        for (int i = 0; i < F.nLights.x; i++) { float a; vec3 L = lightDir(F.lights[i], pt.lm.z, a); if (a > 0) col += stdLit(L, F.lights[i].col.rgb * a, N, V, dc, sc, pr, r); }
        oCol = vec4(col + emis, 1);
        return;
    }
    // lightmapped: the lightmap replaces ambient and (except the Lightmapper shaders) the sun; Lightmapper / Multiply have no ambient
    bool lm = pt.lm.x >= 0, direct = !lm || (fl & F_LMSHADER) != 0;
    vec3 base = amb;
    if (lm) { vec4 l = texB(tex[pt.lm.x], inUv1 * pt.lmST.xy + pt.lmST.zw); base = l.rgb * l.a * F.env.z; }
    else if ((fl & (F_LMSHADER | F_MUL | F_FLAG | F_AMIX)) != 0) base = vec3(0);
    vec3 sc = vec3(0), diff = alb.rgb;
    float n = 1;
    if ((fl & F_MAPPED) != 0) {
        sc = texB(tex[mt.tex.w], inUv * mt.specST.xy + mt.specST.zw).rgb;
        sc = mix(sc, sc * alb.rgb, mt.q.y);
        float r = (1 - mt.p.x) * (1 - mt.p.x);
        n = max(2 / max(r * r, 1e-4) - 2, 1);
        diff = alb.rgb * (1 - max(sc.r, max(sc.g, sc.b)));
        col = alb.rgb * base + alb.rgb * mt.q.x + sc * base * mt.p.x;
    } else col = alb.rgb * base;
    if (direct) col += lit(F.lightDir.xyz, sun, N, V, alb, diff, sc, n, fl, mt.specColor.rgb, mt.p.x);
    {   // point / spot lights reach lightmapped surfaces too (Unity ForwardAdd)
        for (int i = 0; i < F.nLights.x; i++) { float a; vec3 L = lightDir(F.lights[i], pt.lm.z, a); if (a > 0) col += lit(L, F.lights[i].col.rgb * a, N, V, alb, diff, sc, n, fl, mt.specColor.rgb, mt.p.x); }
    }
    oCol = vec4(col + emis * pt.p.y, 1);
}
