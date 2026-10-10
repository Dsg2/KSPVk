#version 450
#extension GL_GOOGLE_include_directive : require
#define VERT
#include "common.glsl"
layout(location = 0) in vec3 inPos; layout(location = 1) in vec3 inNrm; layout(location = 2) in vec4 inTan;
layout(location = 3) in vec2 inUv; layout(location = 4) in vec4 inCol; layout(location = 7) in vec2 inUv1;
layout(location = 0) out vec3 oW; layout(location = 1) out vec3 oN; layout(location = 2) out vec4 oT;
layout(location = 3) out vec2 oUv; layout(location = 4) out vec4 oCol; layout(location = 5) out float oVz;
layout(location = 6) out vec2 oUv1; layout(location = 7) out vec3 oObj;
void main() {
    vec4 r0, r1, r2;
    oW = xform(vec4(inPos, 1), r0, r1, r2);
    vec3 c0 = vec3(r0.x, r1.x, r2.x), c1 = vec3(r0.y, r1.y, r2.y), c2 = vec3(r0.z, r1.z, r2.z);
    float s = dot(c0, cross(c1, c2)) < 0 ? -1.0 : 1.0;
    oN = s * (inNrm.x * cross(c1, c2) + inNrm.y * cross(c2, c0) + inNrm.z * cross(c0, c1));   // cofactor = inverse-transpose up to scale
    oT = vec4(inTan.x * c0 + inTan.y * c1 + inTan.z * c2, inTan.w * s);
    oUv = inUv; oCol = inCol; oUv1 = inUv1; oObj = inPos;
    int v = pc.ids.z;   // 0, or 3 for the Enhanced IBL cube face
    oVz = dot(F.viewZV[v], vec4(oW, 1));
    gl_Position = F.vp[v] * vec4(oW, 1);
    MOTION(inPos, oW, v);
}
