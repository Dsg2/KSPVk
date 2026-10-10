#version 450
#extension GL_GOOGLE_include_directive : require
// World geometry (PQS, ocean, scaled bodies, sun, atmosphere, galaxy). pc.ids.z selects the view (0 local, 1 scaled, 2 galaxy; +3: the Enhanced IBL cube face).
#define VERT
#include "common.glsl"
layout(location = 0) in vec3 inPos; layout(location = 1) in vec3 inNrm; layout(location = 2) in vec4 inTan;
layout(location = 3) in vec2 inUv; layout(location = 4) in vec4 inCol; layout(location = 7) in vec2 inUv1;
layout(location = 0) out vec3 oW; layout(location = 1) out vec3 oN; layout(location = 2) out vec4 oT;
layout(location = 3) out vec4 oUv; layout(location = 4) out vec4 oCol; layout(location = 5) out float oVz;
layout(location = 6) out vec3 oObj; layout(location = 7) out vec3 oObjN;
void main() {
    vec4 m0, m1, m2;
    oW = xform(vec4(inPos, 1), m0, m1, m2);
    vec3 c0 = vec3(m0.x, m1.x, m2.x), c1 = vec3(m0.y, m1.y, m2.y), c2 = vec3(m0.z, m1.z, m2.z);
    float s = dot(c0, cross(c1, c2)) < 0 ? -1.0 : 1.0;
    oN = s * (inNrm.x * cross(c1, c2) + inNrm.y * cross(c2, c0) + inNrm.z * cross(c0, c1));
    oT = vec4(inTan.x * c0 + inTan.y * c1 + inTan.z * c2, inTan.w * s);
    oUv = vec4(inUv, inUv1); oCol = inCol; oObj = inPos; oObjN = inNrm;
    int v = pc.ids.z;
    oVz = dot(F.viewZV[v], vec4(oW, 1));
    gl_Position = F.vp[v] * vec4(oW, 1);
    MOTION(inPos, oW, v);
}
