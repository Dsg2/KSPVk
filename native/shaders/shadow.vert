#version 450
#extension GL_GOOGLE_include_directive : require
#define VERT
#include "common.glsl"
layout(location = 0) in vec3 inPos; layout(location = 3) in vec2 inUv;
layout(location = 0) out vec2 oUv;
void main() {
    vec4 r0, r1, r2;
    oUv = inUv;
#ifdef ENH
    gl_Position = (pc.ids.z >= 4 ? F.spotVP[pc.ids.z - 4] : F.shadowVP[pc.ids.z]) * vec4(xform(vec4(inPos, 1), r0, r1, r2), 1);   // 4..7: Enhanced spot-light layers
#else
    gl_Position = F.shadowVP[pc.ids.z] * vec4(xform(vec4(inPos, 1), r0, r1, r2), 1);
#endif
}
