#version 450
#extension GL_GOOGLE_include_directive : require
// PQS Ocean Surface Quad. v[0] color | v[1] colorFromSpace | v[2] specColor | v[3] fogColor
// v[4] displacement, texDisplacement, tiling, dispFreq | v[5] shininess, gloss, oceanOpacity, planetOpacity
// v[6] falloffPower, falloffExp, fadeStart, fadeEnd | v[7] globalDensity, heightDensityAtViewer | v[8] sunDir | v[9] ids (waterTex, waterTex1, fogRamp)
#define SHADOW_FN
#include "world.glsl"
#ifdef ENH
// Enhanced ocean (8d): wave normals from the FFT field (High; two scales) or 4 Gerstner waves (Medium), triplanar on the planet-relative
// position; Fresnel IBL reflection, GGX sun glint, water-column absorption over the depth to the sea floor (depth copy), shoreline and crest foam.
vec3 waves(vec3 q, float t, out float foam) {   // slope (x, z) in a plane, divergence-based foam
    if (ef(EF_FFT)) {
        // four scales of the one field: ripples near, swell-sized patterns that stay visible from altitude
        vec4 a = texture(oceanNrm, q.xy / 64), b = texture(oceanNrm, q.xy / 17.3), c = texture(oceanNrm, q.yx / 263), e = texture(oceanNrm, q.xy / 1130);
        foam = smoothstep(0.25, 0.6, a.z + 0.5 * b.z);
        return vec3(a.xy + 0.5 * b.xy + 0.7 * c.xy + 0.5 * e.xy, 0);
    }
    const vec4 W[4] = vec4[](vec4(0.96, 0.28, 21, 0.11), vec4(0.6, 0.8, 9.3, 0.09), vec4(-0.31, 0.95, 5.1, 0.07), vec4(0.85, -0.53, 2.7, 0.05));   // dir, wavelength m, max slope
    vec2 g = vec2(0); foam = 0;
    for (int i = 0; i < 4; i++) { float k = 6.2831853 / W[i].z; g += W[i].xy * W[i].w * cos(k * dot(W[i].xy, q.xy) - sqrt(9.81 * k) * t); }
    return vec3(g, 0);
}
void ocean() {
    float d = inVz;
    if (d > F.misc.x) discard;
    vec4 sh = MV(5);
    vec3 N0 = normalize(inN), V = normalize(F.camPos.xyz - inW), L = F.lightDir.xyz;
    vec3 q = inW - (atmOn() ? F.atmC.xyz : vec3(0)), w = pow(abs(N0), vec3(8)); w /= w.x + w.y + w.z;
    float f0, f1, f2, t = F.misc.y;
    vec3 g = waves(q.zyx, t, f0).xyz * w.x, gy = waves(q.xzy, t, f1), gz = waves(q.xyz, t, f2);
    g = vec3(0, g.y, g.x) + vec3(gy.x, 0, gy.y) * w.y + vec3(gz.x, gz.y, 0) * w.z;   // plane slopes back to world axes (yz, xz, xy planes)
    float foam = f0 * w.x + f1 * w.y + f2 * w.z, fade = 1 - ss(d, 20000, 80000), nf = 1 - ss(d, 1500, 15000);
    vec3 N = normalize(N0 - (g - N0 * dot(g, N0)) * fade);
    float nv = clamp(dot(N, V), 1e-3, 1), fr = 0.02 + 0.98 * pow5(1 - nv), pr = mix(0.25, 0.08, nf), rr = pr * pr;
    bool ibl = (int(F.ibl.x) & 2) != 0;
    vec3 R = reflect(-V, N); R += N0 * max(0, -dot(R, N0)) * 2;
    vec3 irr = toLin(F.ambient.rgb) * (ibl ? F.ibl.y : 1) + (ibl ? shIrr(N0) * F.ibl.w : vec3(0));
    vec3 env = ibl ? textureLod(iblCube, R, pr * F.ibl.z).rgb * F.ibl.w : irr;
    vec3 lc = toLin(F.lightCol.rgb) * sunAtm(inW) * sunShadow(inW, d, N0);
    vec3 spec = pbrLit(L, lc, N, V, vec3(0), vec3(0.02), pr, rr), refl = env * fr + spec;
    gRefl = env * fr; gReflW = fr;
    float path = 1e4;   // water traversed to the sea floor
    if (F.enh.z != 0 && pc.ids.z < 3) { float zb = texelFetch(zCopy, ivec2(gl_FragCoord.xy), 0).r; if (zb > d && zb < 1e30) path = (zb - d) * length(inW - F.camPos.xyz) / d; }
    vec3 Tc = exp(-vec3(0.45, 0.09, 0.06) * path), Lw = toLin(MV(1).rgb) * (lc * max(dot(N0, L), 0) + irr);   // colorFromSpace: the deep-water body colour
    float Tm = dot(Tc, vec3(1.0 / 3)), al = 1 - (1 - fr) * Tm;
    vec3 src = refl + (1 - fr) * (1 - Tm) * Lw * mix(vec3(1), 1 - Tc, 0.5);
    foam = max(foam * nf, (1 - smoothstep(0.2, 2.5, path)) * (0.55 + 0.45 * sin(path * 6 - t * 1.5)));
    vec3 fl = 0.75 * (lc * max(dot(N0, L), 0) + irr);
    src = mix(src, fl, foam); al = mix(al, 1, foam);
    GBUF(N, pr, 0);
    oCol = vec4(finish(src / max(al, 1e-3), spec * (1 - foam) / max(al, 1e-3), vec3(0), inW), al * (1 - sh.w));
    gRM.zw = vec2(0);
    oDepth = d;
}
#endif
void main() {
#ifdef ENH
    if (ef(EF_OCEAN) && !underwater()) { ocean(); return; }
#endif
    float d = inVz;
    if (d > F.misc.x) discard;
    vec4 wv = MV(4), sh = MV(5), fo = MV(6), dn = MV(7), id = MV(9);
    vec3 dir = normalize(inUv.xyz), w = abs(dir), V = normalize(F.camPos.xyz - inW), N = normalize(inN);
    float fadeW = 1 - ss(d, fo.z, fo.w), fres = pow(max(0, (1 - dot(N, V)) * fo.x), fo.y);
    float fog = 1 - exp(d * dn.x * dn.y), fogA = 1 - exp(dir.x * dn.x * dn.y);
    float t = F.misc.y, df = wv.w, disp = wv.x * F.misc.z;
    vec3 p0 = dir * wv.z + disp, n = tri(tid(id.x), p0, w).rgb;
    n = mix(n, n.yzx, disp) * (1 + 39 * sat(3 * fog));
    vec3 q = p0 + n * wv.y + F.misc.w * df + vec3(0.3 * cos(t * df + 0.2), 0.2 * sin(-df * t), 0.4 * cos(-df * t));
    int t1 = tid(id.y);
    vec3 c = (T(t1, -q.xy) * w.z + T(t1, q.zy) * w.x + T(t1, q.zx) * w.y).rgb;
    c = mix(MV(1).rgb, c * MV(0).rgb, fadeW);
    c = mix(c, MV(3).rgb, fogA);
    int ramp = tid(id.z);
    if (ramp >= 0) c = mix(c, texB(tex[ramp], vec2(dot(MV(8).xyz, dir), 0.5)).rgb, fog);
    GBUF(N, 0.1, 0);
    vec3 L = F.lightDir.xyz, lc = F.lightCol.rgb * shadowAt(inW, d, N);
    float a = (1 + fadeW * (mix(sh.z, 1, fres) - 1)) * (1 - sh.w);
    oCol = vec4(lc * c * max(0, dot(N, L)) + lc * MV(2).rgb * pow(max(0, dot(N, normalize(L + V))), sh.x * 128) * (1 - sh.w) * sh.y, a);
    oDepth = d;
}
