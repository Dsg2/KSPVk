#version 450
#extension GL_GOOGLE_include_directive : require
// Scaled-space planets (Scaled Planet Simple / RimAerial / RimAerial Standard). v[0] color | v[1] specColor
// v[2] shininess, opacity, rimPower, rimBlend | v[3..5] MainTex/BumpMap/ResourceMap ST | v[6] localLightDir | v[7] ids (main, bump, rimRamp, resource) | v[8].x kind
#include "world.glsl"
void main() {
    vec4 p = MV(2), st = MV(3), bs = MV(4), rs = MV(5), id = MV(7);
    int kind = int(MV(8).x), bump = tid(id.y);
    vec4 tx = T(tid(id.x), inUv.xy * st.xy + st.zw);
    vec3 alb = tx.rgb * MV(0).rgb, n = bump >= 0 ? dn(T(bump, inUv.xy * bs.xy + bs.zw)) : vec3(0, 0, 1);
    vec3 N0 = normalize(inN), Tg = normalize(inT.xyz - N0 * dot(N0, inT.xyz)), B = cross(N0, Tg) * inT.w;
    vec3 N = normalize(Tg * n.x + B * n.y + N0 * n.z), V = normalize(F.camPosV[1].xyz - inW), L = F.lightDir.xyz, H = normalize(L + V);
    GBUF(N, kind == 2 ? 1 - tx.a * p.x : sqrt(2 / (p.x * 128 + 2)), 0);
    float ndl = max(0, dot(N, L)), ndh = max(0, dot(N, H));
    vec3 lc = F.lightCol.rgb, c;
    if (kind == 2) {   // Unity BRDF1 (gamma); ambient from the reflection probe only, as stock
        vec3 sp = MV(1).rgb; float omr = 1 - max(sp.r, max(sp.g, sp.b)), sm = tx.a * p.x, pr = 1 - sm, a = max(pr * pr, 0.002), a2 = a * a;
        float nv = abs(dot(N, V)), lh = sat(dot(L, H)), fd = 2 * lh * lh * pr - 0.5, q = 1 - ndl, q5 = q * q * q * q * q, fv = pow(1 - nv, 5);
        float dif = (1 + fd * q5) * (1 + fd * fv) * ndl;
        float vis = 0.5 / (ndl * (nv * (1 - a) + a) + nv * (ndl * (1 - a) + a) + 1e-5), dd = (ndh * a2 - ndh) * ndh + 1;
        float spec = sqrt(max(1e-4, vis * a2 * 0.31831 / (dd * dd + 1e-7) * 3.14159265)) * ndl * (dot(sp, sp) > 0 ? 1 : 0);
        float l5 = pow(1 - lh, 5);
        c = lc * (alb * omr * dif + spec * ((1 - sp) * l5 + sp));
    } else c = lc * alb * ndl + lc * MV(1).rgb * pow(ndh, p.x * 128) * tx.a;
    if (kind >= 1) {
        vec4 res = T(tid(id.w), inUv.xy * rs.xy + rs.zw);
        int rr = tid(id.z);
        vec3 rim = rr >= 0 ? texB(tex[rr], vec2(dot(MV(6).xyz, normalize(inObjN)), 1)).rgb : vec3(0);
        c += rim * p.w * pow(1 - sat(dot(N0, V)), p.z) * p.y * (1 - res.a) + res.rgb * res.a;
    }
    oCol = vec4(c, 1);
    oDepth = -inVz;
}
