// Physically based atmosphere (Hillaire 2020; Bruneton transmittance mapping). Shared by atmo.comp and the Enhanced scene shaders, which
// provide transLut / multiLut. Metres, positions relative to the main body's centre. F.atmC.w ground radius | F.atmP: top radius, Rayleigh / Mie
// scale height, aerial-perspective far | F.atmB: Rayleigh rgb, Mie scattering (per m) | F.atmX.x Mie g | F.atmS.w sun angular radius
const float PI = 3.14159265;
const vec2 TRANS_RES = vec2(256, 64), MULTI_RES = vec2(32);
float sq(float x) { return x * x; }
vec3 sunE() { return PI * pow(max(F.lightCol.rgb, vec3(0)), vec3(2.2)) * F.atmX.y; }   // sun illuminance (lights carry Unity's 1/pi)
vec2 sphHit(vec3 o, vec3 d, float R) { float b = dot(o, d), h = b * b - (dot(o, o) - R * R); if (h < 0) return vec2(-1); h = sqrt(h); return vec2(-b - h, -b + h); }
void medium(float r, out vec3 sR, out float sM, out vec3 ext) {
    float h = max(r - F.atmC.w, 0); sR = F.atmB.rgb * exp(-h / F.atmP.y); sM = F.atmB.w * exp(-h / F.atmP.z); ext = sR + sM * 1.11;
}
float phaseR(float c) { return 3 / (16 * PI) * (1 + c * c); }
float phaseM(float c) { float g = F.atmX.x, g2 = g * g; return 3 / (8 * PI) * (1 - g2) * (1 + c * c) / ((2 + g2) * pow(max(1 + g2 - 2 * g * c, 1e-4), 1.5)); }
vec2 transUv(float r, float mu) {
    float Rg = F.atmC.w, Rt = F.atmP.x, H = sqrt(max((Rt - Rg) * (Rt + Rg), 1)), rho = sqrt(max((r - Rg) * (r + Rg), 0));
    float d = max(-r * mu + sqrt(max(Rt * Rt - r * r * (1 - mu * mu), 0)), 0), dmin = Rt - r, dmax = rho + H;
    vec2 x = clamp(vec2((d - dmin) / max(dmax - dmin, 1e-3), rho / H), 0, 1);
    return (x * (TRANS_RES - 1) + 0.5) / TRANS_RES;
}
vec3 transmittance(float r, float mu) { return textureLod(transLut, transUv(r, mu), 0).rgb; }
vec3 transToTop(vec3 p, vec3 d) {   // along d to space (the ground is not tested)
    float r = length(p);
    if (r > F.atmP.x) { vec2 t = sphHit(p, d, F.atmP.x); if (t.x < 0) return vec3(1); p += d * t.x; r = F.atmP.x; }
    return transmittance(r, dot(p, d) / r);
}
float eclipse(vec3 p) {   // other bodies in front of the sun (soft over the sun's disc)
    float s = 1; vec3 L = F.lightDir.xyz;
    for (int i = 0; i < 4; i++) {
        vec4 o = F.occ[i]; if (o.w <= 0) break;
        vec3 v = o.xyz - p; float t = dot(v, L);
        if (t > 0) { float e = t * F.atmS.w + 1; s *= smoothstep(-e, e, sqrt(max(dot(v, v) - t * t, 0)) - o.w); }
    }
    return s;
}
vec3 sunTrans(vec3 p) {   // sunlight reaching p: atmosphere, the planet's own shadow, eclipses
    float r = max(length(p), F.atmC.w + 1); vec3 L = F.lightDir.xyz;
    return transToTop(p, L) * smoothstep(-0.02, 0.02, dot(p, L) / r + sqrt(max(1 - sq(F.atmC.w / r), 0))) * eclipse(p);
}
vec3 multiScat(float r, float muS) {
    vec2 x = clamp(vec2(muS * 0.5 + 0.5, (r - F.atmC.w) / (F.atmP.x - F.atmC.w)), 0, 1);
    return textureLod(multiLut, (x * (MULTI_RES - 1) + 0.5) / MULTI_RES, 0).rgb;
}
bool atmSeg(vec3 o, vec3 d, out float t0, out float t1) {   // the part of the ray inside the atmosphere, up to the ground
    vec2 a = sphHit(o, d, F.atmP.x); t0 = max(a.x, 0); t1 = a.y;
    vec2 g = sphHit(o, d, F.atmC.w); if (g.x > 0) t1 = min(t1, g.x);
    return a.y > 0 && t1 > t0;
}
// In-scattered light per unit sun illuminance (single + multiple scattering) and transmittance over [t0, t1]; steps denser near t0
vec3 scatter(vec3 o, vec3 d, float t0, float t1, int n, out vec3 T) {
    vec3 L = F.lightDir.xyz, S = vec3(0); T = vec3(1);
    float c = dot(d, L), pr = phaseR(c), pm = phaseM(c), D = t1 - t0, tp = t0;
    for (int i = 0; i < n; i++) {
        float t = t0 + D * sq(float(i + 1) / n), dt = t - tp; vec3 p = o + d * (tp + 0.5 * dt); tp = t;
        float r = length(p); vec3 sR, ext; float sM; medium(r, sR, sM, ext);
        vec3 j = (sR * pr + sM * pm) * sunTrans(p) + (sR + sM) * multiScat(r, dot(p, L) / r), st = exp(-ext * dt);
        S += T * (j - j * st) / max(ext, vec3(1e-12));
        T *= st;
    }
    return S;
}
