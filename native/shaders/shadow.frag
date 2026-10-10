#version 450
#extension GL_GOOGLE_include_directive : require
// Alpha-tested shadow casters only (opaque casters have no fragment shader).
#include "common.glsl"
struct Mat { vec4 color, specColor, emisColor, burn, mainST, bumpST, emisST, specST, p, q; ivec4 tex; ivec4 info; vec4 pad[12]; };
layout(std430, set = 0, binding = 1) readonly buffer Mats { Mat m[]; } M;
layout(set = 0, binding = 4) uniform sampler2D tex[4096];
layout(location = 0) in vec2 inUv;
void main() {
    Mat mt = M.m[pc.ids.x];
    if (texB(tex[mt.tex.x], inUv * mt.mainST.xy + mt.mainST.zw).a * mt.color.a < mt.p.y) discard;
}
