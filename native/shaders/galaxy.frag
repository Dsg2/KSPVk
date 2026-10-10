#version 450
#extension GL_GOOGLE_include_directive : require
// Galaxy skybox faces. v[0] color | v[1] MainTex ST | v[2].x texture id
#include "world.glsl"
void main() {
    vec4 st = MV(1);
    oCol = vec4(T(tid(MV(2).x), inUv.xy * st.xy + st.zw).rgb * MV(0).rgb, 1);
    oDepth = 0;
}
