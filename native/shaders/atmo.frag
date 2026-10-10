#version 450
#extension GL_GOOGLE_include_directive : require
// AtmosphereFromGround (scaled space); the stock per-vertex scattering loop runs per pixel. 
// v[0] offsetTransform | v[1] v4CameraPos | v[2] v4LightDir | v[3] invWaveLength | v[4] camHeight, innerRadius, KrESun, KmESun
// v[5] Kr4PI, Km4PI, scale, scaleDepth | v[6] scaleOverScaleDepth, samples, g, g2 | v[7] exposure, camHeightUnderwater, opacityAltBase, opacityAltMult
// v[8] underwaterColorStart | v[9] underwaterColorEnd | v[10].x lightDot
#include "world.glsl"
float sc(float x) { return exp(-0.00287 + x * (0.459 + x * (3.83 + x * (-6.80 + x * 5.25)))); }
#ifdef ENH
// Enhanced sky (8d), the main body only: the sky-view LUT while the camera is inside the atmosphere, a per-pixel march from outside.
// Premultiplied output: in-scattered light, 1 - mean transmittance (what shows through: stars, other bodies).
vec2 skyUv(vec3 o, vec3 d) {   // inverse of the SKYV mapping in atmo.comp (Hillaire's non-linear latitude, sun-relative azimuth)
    float Rg = F.atmC.w, r = clamp(length(o), Rg + 2, F.atmP.x - 1); vec3 up = normalize(o), L = F.lightDir.xyz;
    float vh = sqrt(max((r - Rg) * (r + Rg), 0)), beta = acos(clamp(vh / r, -1, 1)), zha = PI - beta, cz = dot(up, d), za = acos(clamp(cz, -1, 1));
    float v = cz >= -vh / r ? 0.5 * (1 - sqrt(max(1 - za / zha, 0))) : 0.5 + 0.5 * sqrt(clamp((za - zha) / beta, 0, 1));
    vec3 dh = d - up * cz, lh = L - up * dot(up, L);
    float cl = dot(dh, dh) * dot(lh, lh) > 1e-12 ? dot(dh, lh) * inversesqrt(dot(dh, dh) * dot(lh, lh)) : 1;
    return vec2(sqrt(clamp(0.5 - 0.5 * cl, 0, 1)), v);
}
#endif
void main() {
#ifdef ENH
    if (atmOn() && MV(7).y <= 0 && distance(MV(0).xyz, F.atmS.xyz) <= 1e-4 * max(1, length(F.atmS.xyz))) {
        vec3 d, o = F.camPos.xyz - F.atmC.xyz;
        bool map = F.atmX.z > 1.5;   // map view: trace from the scaled camera (outside the atmosphere)
        if (map) { o = (F.camPosV[1].xyz - F.atmS.xyz) * F.atmX.w; d = normalize(inW - F.camPosV[1].xyz); }
        else if (pc.ids.z >= 3) d = normalize(inW - F.camPosV[1].xyz);
        else {
            vec2 n = gl_FragCoord.xy * F.jitter.zw * 2 - 1 - F.jitter.xy;
            d = normalize(F.camR.xyz * ((n.x + F.proj.z) / F.proj.x) + F.camU.xyz * ((F.proj.w - n.y) / F.proj.y) + F.camF.xyz);
        }
        vec3 S, Tr; float r = length(o), mu = dot(o, d) / r;
        if (r < F.atmP.x && !map) {
            S = textureLod(skyLut, skyUv(o, d), 0).rgb;
            Tr = mu < -sqrt(max(1 - sq(F.atmC.w / r), 0)) ? vec3(0) : transmittance(r, mu);
        } else {
            float t0, t1;
            if (!atmSeg(o, d, t0, t1)) discard;
            S = scatter(o, d, t0, t1, 16, Tr) * sunE();
        }
        oCol = vec4(pow(S, vec3(1 / 2.2)), 1 - dot(Tr, vec3(1.0 / 3)));
        oDepth = -inVz;
        return;
    }
#endif
    vec3 cam = MV(1).xyz - MV(0).xyz, L = MV(2).xyz, iw = MV(3).xyz;
    vec4 a = MV(4), b = MV(5), c6 = MV(6);
    vec3 ray = inW - MV(0).xyz - cam; float far = length(ray); vec3 dir = ray / far;
    float start = -b.w * sc(1 - dot(dir, cam) / length(cam)), depth = exp(c6.x * abs(a.y - a.x));
    float seg = far / c6.y, sl = seg * b.z;
    vec3 s = cam + dir * seg * 0.5, kk = iw * b.x + b.y, fr = vec3(0);
    for (int i = 0; i < 3; i++) {
        float h = length(s), dep = exp(c6.x * abs(a.y - h));
        float sct = depth * start + dep * (-b.w * sc(1 - dot(L, s) / h) + b.w * sc(1 - dot(dir, s) / h));
        fr += exp(-sct * kk) * dep * sl;
        s += dir * seg;
    }
    vec3 v3 = cam - inObj;
    float cs = dot(L, v3) / length(v3), g = c6.z, g2 = c6.w, c2 = 1 + cs * cs;
    float mie = 1.5 * ((1 - g2) / (2 + g2)) * c2 / pow(1 + g2 - 2 * g * cs, 1.5);
    vec3 c = 0.75 * c2 * fr * iw * a.z + mie * fr * a.w;
    vec4 u = MV(7);
    oCol = vec4(1 - exp(-u.x * clamp(c, 0, 1)), 1 - exp(-u.x * c.b));
    if (u.y > 0) {   // stock underwater tint: opacity grows with depth and with distance from the camera
        float s = length(inW - MV(0).xyz) / max(length(inObj), 1e-6), dd = abs(s * (length(MV(1).xyz) - length(MV(0).xyz + inObj)));
        vec4 uc = mix(vec4(0, 0, 0, 1), mix(MV(8), MV(9), u.y), MV(10).x);
        oCol = mix(oCol, uc, sat(u.z * (dd * 0.03 + 1) + u.w * u.y));
    }
    oDepth = -inVz;
}
