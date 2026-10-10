// Frame UBO (FrameU in native/core/scene.inl). Shared by the scene shaders (binding 0) and the Enhanced compute passes (FRAME_BINDING).
#ifndef FRAME_BINDING
#define FRAME_BINDING 0
#endif
struct Light { vec4 pos; vec4 col; vec4 dir; };       // pos: xyz, range | col: rgb*intensity, type (0 point, 1 spot, 2 directional) | dir: xyz (towards light for directional), cos(half angle)
layout(set = 0, binding = FRAME_BINDING, row_major, std140) uniform Frame {
    mat4 viewProj; vec4 camPos; vec4 lightDir; vec4 lightCol; vec4 ambient;
    mat4 shadowVP[4]; vec4 splits; vec4 shadowP; vec4 viewZ; vec4 cascTexel; vec4 cascBias;   // shadowP: strength, normalBias, 1/res, enabled
    ivec4 nLights; Light lights[8];
    mat4 vp[6]; vec4 camPosV[3]; vec4 viewZV[6]; vec4 misc;   // per view (0 local, 1 scaled, 2 galaxy; 3..5: the same for the Enhanced IBL cube face); misc: local far, time, sin(time/8), time/20
    vec4 ambEq, ambGround, env;                               // trilight ambient (ambient = sky); env: ambient mode, reflection intensity, lightmap RGBM scale, cube max mip
    mat4 pvp[3]; vec4 jitter; vec4 ibl;                       // Enhanced: previous frame's unjittered view-projections, TAA jitter (NDC);
                                                              // ibl: x features (1 physically based, 2 IBL cube ready), y stock-ambient fill, z IBL max lod, w IBL intensity
#ifdef ENH   // Phase 8c-8e; stock shaders see the block without these
    ivec4 enh;                         // x active EF_* flags, y frame index, z depth copy valid (ocean pass), w -
    vec4 camR, camU, camF, proj;       // camera right / up / forward (world), Unity m00, m11, m02, m12
    mat4 spotVP[4]; ivec4 spotIdx[2];  // shadowed spot lights: light index -> shadow layer (-1: none)
    vec4 atmC, atmP, atmB, atmX, atmS; // main atmosphere, local metres: centre + ground radius | top radius, Rayleigh / Mie scale height, aerial-perspective far
                                       // | Rayleigh rgb, Mie (per m) | Mie g, sun illuminance scale, valid (2: map view), metres per scaled unit | centre (scaled space), -
    vec4 occ[4];                       // eclipse occluders (local metres): centre, radius
    vec4 sfx;                          // sun angular radius (tan), AO strength, GI strength, contact-shadow length (m)
    vec4 rs;                           // render scale: scene / output size, texture lod bias (TAAU), -, -
#endif
} F;
#ifdef ENH
#define texB(s, uv) texture(s, uv, F.rs.y)
#else
#define texB texture
#endif
