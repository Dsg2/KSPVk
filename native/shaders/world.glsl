// Fragment include for world shaders. WMat mirrors the 96-float material record (texture ids as int bits, -1 = none).
#include "common.glsl"
struct WMat { vec4 v[24]; };
layout(std430, set = 0, binding = 1) readonly buffer Mats { WMat m[]; } M;
layout(set = 0, binding = 4) uniform sampler2D tex[4096];
layout(location = 0) in vec3 inW; layout(location = 1) in vec3 inN; layout(location = 2) in vec4 inT;
layout(location = 3) in vec4 inUv; layout(location = 4) in vec4 inCol; layout(location = 5) in float inVz;
layout(location = 6) in vec3 inObj; layout(location = 7) in vec3 inObjN;
layout(location = 0) out vec4 oCol; layout(location = 1) out float oDepth;
#define MV(i) M.m[pc.ids.x].v[i]
int tid(float f) { return floatBitsToInt(f); }
vec4 T(int t, vec2 uv) { return t < 0 ? vec4(0) : texB(tex[t], uv); }
float sat(float x) { return clamp(x, 0, 1); }
float ss(float x, float a, float b) { float t = b == a ? step(a, x) : sat((x - a) / (b - a)); return t * t * (3 - 2 * t); }
vec4 tri(int t, vec3 q, vec3 w) { return T(t, q.xy) * w.z + T(t, q.zy) * w.x + T(t, q.zx) * w.y; }
vec3 dn(vec4 r) { vec2 n = r.wy * 2 - 1; return vec3(n, sqrt(1 - min(1, dot(n, n)))); }
vec3 triN(int t, vec3 q, vec3 w) { return dn(T(t, q.xy)) * w.z + dn(T(t, q.zy)) * w.x + dn(T(t, q.zx)) * w.y; }
