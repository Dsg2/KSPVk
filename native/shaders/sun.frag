#version 450
#extension GL_GOOGLE_include_directive : require
// Sun surface (Emissive Multi Ramp Sunspots). v[0..3] emit0, emit1, sunspot, rim colors | v[4] noise offsets
// v[5] sunspotPower, rimPower, rimBlend | v[6] NoiseMap ST | v[7] SunspotTex ST | v[8] ids (noise, ramp, sunspot)
#include "world.glsl"
void main() {
    vec4 id = MV(8), k = MV(5), ns = MV(6), ss_ = MV(7);
    int rp = tid(id.y);
    vec4 n4 = T(tid(id.x), inUv.xy * ns.xy + ns.zw) + MV(4);
    float t = rp < 0 ? 0 : 0.25 * (texB(tex[rp], vec2(n4.x, 0.5)).x + texB(tex[rp], vec2(n4.y, 0.5)).y
                                 + texB(tex[rp], vec2(n4.z, 0.5)).z + texB(tex[rp], vec2(n4.w, 0.5)).w);
    vec3 c = mix(MV(0).rgb, MV(1).rgb, t);
    c = mix(c, MV(2).rgb, T(tid(id.z), inUv.zw * ss_.xy + ss_.zw).x * k.x);
    vec3 N = normalize(inN), V = normalize(F.camPosV[1].xyz - inW);
    GBUF(N, 1, 0);
    vec3 e = mix(c, MV(3).rgb, pow(sat(dot(V, N)), k.y) * k.z);
    oCol = vec4(c * F.lightCol.rgb * max(0, dot(N, F.lightDir.xyz)) + e, 1);
    oDepth = -inVz;
}
