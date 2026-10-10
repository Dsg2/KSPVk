#version 450
#extension GL_GOOGLE_include_directive : require
// Data-driven PQS terrain (PQS Main / Main Optimised / Sphere Projection [AP]); maths from the stock DXBC.
// v[0..3] band (lo.xy, hi.zw) | v[4..7] tilings (near, far, bumpNear, bumpFar) | v[8..11] ids (same order)
// v[12] steep tilings (near, far), single-blend flag | v[13] steep ids (tex, bump) | v[14] groundStart, groundEnd, steepStart, steepEnd
// v[15] powerNear, powerFar, steepPower, planetOpacity | v[16] contrast, saturation, fogOn, nBands | v[17] tint
// v[18] globalDensity, heightDensityAtViewer, fogRamp id, ramp v | v[19] sunDir | v[20] floating origin
// v[21] global bump id, tiling, oceanFogDistance, cameraAltitude (< 0: camera underwater) | v[22] / v[23] Enhanced detail albedo / normal ids per band (KSPVK_TERRAIN)
#define SHADOW_FN
#include "world.glsl"
#ifdef ENH
// Enhanced terrain (8e): height-blended bands, sharpened triplanar weights, detail normals (procedural from the near texture's luminance, or
// optional CC0 textures), macro variation, parallax occlusion within 50 m (High), linear-light PBR with soft shadows and aerial perspective.
float hash3(vec3 p) { p = fract(p * 0.1031); p += dot(p, p.zyx + 31.32); return fract((p.x + p.y) * p.z); }
float vnoise(vec3 x) {
    vec3 i = floor(x), f = x - i; f = f * f * (3 - 2 * f);
    return mix(mix(mix(hash3(i), hash3(i + vec3(1, 0, 0)), f.x), mix(hash3(i + vec3(0, 1, 0)), hash3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0, 0, 1)), hash3(i + vec3(1, 0, 1)), f.x), mix(hash3(i + vec3(0, 1, 1)), hash3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
vec3 tq(float s) { return fract(MV(20).xyz * s) + inW * s; }   // world position in tiles of 1/s m, precise near the camera
vec2 pl(vec3 q, int k) { return k == 0 ? q.zy : k == 1 ? q.zx : q.xy; }
float hgt(int t, vec2 uv) { return lumi(texB(tex[t], uv).rgb); }
void terrain() {
    float d = inVz;
    if (d > F.misc.x) discard;
    vec4 a = MV(14), pw = MV(15), cs = MV(16), tint = MV(17), fg = MV(18);
    vec3 v6 = cs.x * (mix(vec3(dot(inCol.rgb, vec3(0.299, 0.587, 0.114))), inCol.rgb, cs.y) - tint.rgb * tint.a) + tint.rgb * tint.a;
    vec3 p = (inW + MV(20).xyz) * 1e-5, N0 = normalize(inN), V = normalize(F.camPos.xyz - inW), w = pow(abs(N0), vec3(4));
    w /= w.x + w.y + w.z;
    float h = inCol.a, sD = ss(d, a.x, a.y), nk = (1 - sD) * pw.x, fk = sD * pw.y;
    int nb = int(cs.w), gb = tid(MV(21).x);
    vec3 cb[4], nbv[4]; float hb[4], hm = -1;
    for (int i = 0; i < 4; i++) { cb[i] = nbv[i] = vec3(0); hb[i] = -1; }
    for (int i = 0; i < nb; i++) {   // height blending: a band's own texture brightness decides the transition
        vec4 r = MV(i), tl = MV(4 + i), id = MV(8 + i);
        float bw = ss(h, r.x, r.y) - ss(h, r.z, r.w);
        if (bw <= 0) continue;
        vec3 cn = tri(tid(id.x), p * tl.x, w).rgb;
        cb[i] = cn * nk + tri(tid(id.y), p * tl.y, w).rgb * fk;
        if (gb < 0) {
            if (tid(id.z) >= 0) nbv[i] += triN(tid(id.z), p * tl.z, w) * nk;
            if (tid(id.w) >= 0) nbv[i] += triN(tid(id.w), p * tl.w, w) * fk;
        }
        hb[i] = bw + 0.6 * lumi(cn); hm = max(hm, hb[i]);
    }
    vec3 c = vec3(0), n = vec3(0); float ws = 0, wd = 0; int db = 0;
    for (int i = 0; i < nb; i++) {
        float wi = hb[i] < 0 ? 0 : max(hb[i] - hm + 0.25, 0);
        c += cb[i] * wi; n += nbv[i] * wi; ws += wi;
        if (wi > wd) { wd = wi; db = i; }
    }
    c /= max(ws, 1e-5); n /= max(ws, 1e-5);
    if (gb >= 0) n = triN(gb, p * MV(21).y, w);
    c = mix(v6, v6 * c, nk + fk);
    vec4 st = MV(12); int stT = tid(MV(13).x), stB = tid(MV(13).y);
    float sw = (1 - ss(d, a.z, a.w)) * sat(inUv.w * pw.z), sn = st.z > 0 ? sw : (1 - sD) * sw, sf = st.z > 0 ? 0 : sD * sw;
    if (stT >= 0) { c = mix(c, tri(stT, p * st.x, w).rgb, sn); c = mix(c, tri(stT, p * st.y, w).rgb, sf); }
    if (stB >= 0) { n = mix(n, triN(stB, p * st.x, w), sn); n = mix(n, triN(stB, p * st.y, w), sf); }
    n.z += 1e-4;
    vec3 Tg = normalize(inT.xyz - N0 * dot(N0, inT.xyz)), B = cross(N0, Tg) * inT.w;
    vec3 N = normalize(Tg * n.x + B * n.y + N0 * n.z);

    // detail layer on the dominant triplanar plane: CC0 textures (2 m tiles) or the dominant band's near texture at 6x its tiling
    float dk = (1 - ss(d, 60, 200)) * smoothstep(0.45, 0.8, max(w.x, max(w.y, w.z)));
    if (dk > 0 && nb > 0) {
        int k = w.x > w.y && w.x > w.z ? 0 : w.y > w.z ? 1 : 2, alb = tid(MV(22)[db]), dnm = tid(MV(23)[db]), ht = alb > 3 ? alb : tid(MV(8 + db).x);
        if (ht >= 0) {
            float s = alb > 3 ? 0.5 : 6e-5 * MV(4 + db).x;
            vec3 Au = k == 2 ? vec3(1, 0, 0) : vec3(0, 0, 1), Av = k == 1 ? vec3(1, 0, 0) : vec3(0, 1, 0), An = k == 0 ? vec3(1, 0, 0) : k == 1 ? vec3(0, 1, 0) : vec3(0, 0, 1);
            vec2 uv = pl(tq(s), k);
            if (ef(EF_POM) && d < 50) {   // parallax occlusion: 8 linear steps, 4 cm relief
                vec3 Vt = vec3(dot(V, Au), dot(V, Av), abs(dot(V, An)));
                vec2 du = -Vt.xy / max(Vt.z, 0.25) * 0.04 * s * (1 - ss(d, 30, 50)) / 8;
                float dep = 0, dm = 1 - hgt(ht, uv);
                for (int i = 0; i < 8 && dep < dm; i++) { uv += du; dep += 0.125; dm = 1 - hgt(ht, uv); }
            }
            if (alb > 3) {
                c *= mix(vec3(1), texB(tex[alb], uv).rgb / max(textureLod(tex[alb], vec2(0.5), 14).rgb, vec3(0.02)), dk);
                if (dnm > 3) { vec2 t = texB(tex[dnm], uv).xy * 2 - 1; N = normalize(N + (Au * t.x + Av * t.y) * dk); }
            } else {
                float e = 1.0 / 256, h0 = hgt(ht, uv);
                vec2 g = vec2(hgt(ht, uv + vec2(e, 0)) - h0, hgt(ht, uv + vec2(0, e)) - h0) * 2.5;
                N = normalize(N - (Au * g.x + Av * g.y) * dk);
            }
        }
    }
    vec3 wp = inW + MV(20).xyz;
    c *= 0.86 + 0.18 * vnoise(wp / 250) + 0.1 * vnoise(wp / 1300);   // macro variation against visible tiling
    int ramp = tid(fg.z);
    if (!atmOn() && cs.z > 0 && ramp >= 0) {
        float sd = dot(MV(19).xyz, normalize(vec3(inUv.xy, inUv.z)));
        c = mix(c, texB(tex[ramp], vec2(sd, fg.w)).rgb, max(0, 1 - exp(d * fg.x * fg.y)));
    }
    vec3 dc = toLin(c) * 0.96; const float pr = 0.85, r = pr * pr;
    GBUF(N, pr, 0);
    gAlb = vec4(sqrt(dc), 1);
    bool ibl = (int(F.ibl.x) & 2) != 0;
    vec3 sun = pbrLit(F.lightDir.xyz, toLin(F.lightCol.rgb) * sunAtm(inW) * sunShadow(inW, d, N0), N, V, dc, vec3(0.04), pr, r);
    vec3 irr = toLin(F.ambient.rgb) * (ibl ? F.ibl.y : 1) + (ibl ? shIrr(N) * F.ibl.w : vec3(0));
    vec3 R = reflect(-V, N), pre = ibl ? textureLod(iblCube, R, pr * F.ibl.z).rgb * F.ibl.w : irr;
    vec2 eb = envBrdf(pr, clamp(abs(dot(N, V)), 1e-4, 1));
    vec3 amb = dc * irr + pre * (0.04 * eb.x + eb.y) * sat(1 + dot(R, N0));
    oCol = vec4(finish(sun + amb, sun, amb, inW), pw.w);
    oDepth = d;
}
#endif
void main() {
#ifdef ENH
    if (ef(EF_TERRAIN) && MV(21).w >= 0 && !underwater()) { terrain(); return; }
#endif
    float d = inVz;
    if (d > F.misc.x) discard;
    vec4 a = MV(14), pw = MV(15), cs = MV(16), tint = MV(17), fg = MV(18);
    float lum = dot(inCol.rgb, vec3(0.299, 0.587, 0.114));
    vec3 v6 = cs.x * (mix(vec3(lum), inCol.rgb, cs.y) - tint.rgb * tint.a) + tint.rgb * tint.a;
    vec3 p = (inW + MV(20).xyz) * 1e-5, w = abs(normalize(inN));
    float h = inCol.a, sD = ss(d, a.x, a.y), nk = (1 - sD) * pw.x, fk = sD * pw.y;
    vec3 c = vec3(0), n = vec3(0);
    int gb = tid(MV(21).x);
    for (int i = 0; i < int(cs.w); i++) {
        vec4 r = MV(i), tl = MV(4 + i), id = MV(8 + i);
        float bw = ss(h, r.x, r.y) - ss(h, r.z, r.w);
        c += tri(tid(id.x), p * tl.x, w).rgb * bw * nk + tri(tid(id.y), p * tl.y, w).rgb * bw * fk;
        if (gb < 0) {
            if (tid(id.z) >= 0) n += triN(tid(id.z), p * tl.z, w) * bw * nk;
            if (tid(id.w) >= 0) n += triN(tid(id.w), p * tl.w, w) * bw * fk;
        }
    }
    if (gb >= 0) n = triN(gb, p * MV(21).y, w);
    c = mix(v6, v6 * c, nk + fk);
    vec4 st = MV(12); int stT = tid(MV(13).x), stB = tid(MV(13).y);
    float sw = (1 - ss(d, a.z, a.w)) * sat(inUv.w * pw.z), sn = st.z > 0 ? sw : (1 - sD) * sw, sf = st.z > 0 ? 0 : sD * sw;
    if (stT >= 0) { c = mix(c, tri(stT, p * st.x, w).rgb, sn); c = mix(c, tri(stT, p * st.y, w).rgb, sf); }
    if (stB >= 0) { n = mix(n, triN(stB, p * st.x, w), sn); n = mix(n, triN(stB, p * st.y, w), sf); }
    n.z += 1e-4;
    vec3 N0 = normalize(inN), Tg = normalize(inT.xyz - N0 * dot(N0, inT.xyz)), B = cross(N0, Tg) * inT.w;
    vec3 N = normalize(Tg * n.x + B * n.y + N0 * n.z);
    GBUF(N, 0.9, 0);
    int ramp = tid(fg.z);
    if (cs.z > 0 && ramp >= 0) {
        float sd = dot(MV(19).xyz, normalize(vec3(inUv.xy, inUv.z)));
        c = mix(c, texB(tex[ramp], vec2(sd, fg.w)).rgb, max(0, 1 - exp(d * fg.x * fg.y)));
    }
    float uw = MV(21).w < 0 ? 1 : 0, fo = d / MV(21).z;
    if (uw > 0 && ramp >= 0) c = mix(c, texB(tex[ramp], vec2(0, 0.25)).rgb, sat(fo));   // stock samples uv4.x, which PQS leaves 0
    vec3 lc = F.lightCol.rgb * shadowAt(inW, d, N0);
    float al = pw.w + uw * (1 - (1 - pw.w) * min(1.01 - sat(fo * 0.5), 1) - pw.w);
    oCol = vec4(lc * c * max(0, dot(N, F.lightDir.xyz)) + c * F.ambient.rgb, al);
    oDepth = d;
}
