// KSPVk Enhanced mode (Phase 8a, opt-in): HDR scene target, TAA with per-object motion vectors, auto-exposure, dual-filter bloom, tonemapping,
// G-buffer extras (octahedral normal, roughness / metal) and a Hi-Z pyramid. Phase 8b: physically based parts lit by an IBL cube that KSPVk
// renders itself (one face per frame: sky, scaled bodies, terrain, scenery), GGX-prefiltered, plus SH9 irradiance. Included by core.cpp after scene.inl.
// Phase 8c: PCSS sun shadows, contact shadows, GTAO, SSGI, shadowed spot lights. 8d: Hillaire atmosphere LUTs + aerial perspective (main body),
// FFT / Gerstner ocean. 8e: triplanar, height-blended terrain with detail normals and POM (shader side, pqs.frag).
// Off: nothing here runs and the images are freed. A feature whose pipeline or images fail is disabled on its own (gEnhFail) for the session.
#include <unordered_map>
#include "post_hiz_comp.h"
#include "post_taa_comp.h"
#include "post_hist_comp.h"
#include "post_expo_comp.h"
#include "post_bdown_comp.h"
#include "post_bup_comp.h"
#include "tonemap_frag.h"
#include "ibl_down_comp.h"
#include "ibl_pref_comp.h"
#include "ibl_sh_comp.h"
#include "post_gtao_comp.h"
#include "post_ssgi_comp.h"
#include "post_ssr_comp.h"
#include "post_den_comp.h"
#include "post_resolve_comp.h"
#include "post_zcopy_comp.h"
#include "atmo_trans_comp.h"
#include "atmo_multi_comp.h"
#include "atmo_skyv_comp.h"
#include "atmo_ap_comp.h"
#include "ocean_spec_comp.h"
#include "ocean_fft_comp.h"
#include "ocean_nrm_comp.h"
#include "ocean_mip_comp.h"
#include "shadow_enh_vert.h"
#include "shadow_skin_enh_vert.h"

enum { EF_ON = 1, EF_TAA = 2, EF_BLOOM = 4, EF_AUTOEXP = 8, EF_GBUF = 16, EF_HIZ = 32, EF_PBR = 64, EF_IBL = 128, EF_SOFTSH = 256, EF_CONTACT = 512,
       EF_GTAO = 1024, EF_SSGI = 2048, EF_SPOTSH = 4096, EF_ATMO = 8192, EF_OCEAN = 16384, EF_FFT = 32768, EF_TERRAIN = 65536, EF_POM = 131072, EF_SSR = 262144 };   // mirrored in enh.glsl
enum { NBLOOM = 5, MAXHIZ = 16, IBL_RES = 128, IBL_SRC_MIPS = 8, IBL_MIPS = 6, NSPOT = 4, SPOT_RES = 1024, FFT_N = 128, WAV_MIPS = 8 };
#pragma pack(push, 4)
struct EnhIn { int flags, tonemap; float ev, evRange, key, bloom, threshold, taaWeight, ambFill, iblScale, sunSoft, aoStrength, giStrength, skyBright, renderScale; };   // renderScale: all modes (upscale.inl)   // ambFill: stock ambient kept under IBL   // tonemap: 0 neutral, 1 AgX, 2 ACES fit, 3 clip
#pragma pack(pop)

static std::mutex gEnhM; static EnhIn gEnhReq{};
static std::atomic<int> gEnhFail{0}, gEnhActive{0};
static float gPassMs[9];   // IBL, pre-scene (spot shadows, atmosphere, ocean), main, Hi-Z, AO / GI, TAA, exposure, bloom, tonemap (smoothed)

static struct {
    int w = 0, h = 0, ow = 0, oh = 0, nHiz = 0; float jit[2]{}; bool imgs = false; uint64_t bytes = 0;   // w, h: scene size; ow, oh: output (TAA history); jit: TAA jitter in uv
    std::atomic<int> init{0}; std::thread builder;   // pipelines: 0 not started, 1 building (worker thread: seconds on Iris Xe), 2 ready, 3 failed
    VImg hdr, nrm, rm, mv, alb, hist[2], bloom, hiz, ao, gi, aoH[2], giH[2], zc, ssr; VkImageView bloomMip[NBLOOM]{}, hizMip[MAXHIZ]{};
    Buf expo, prev[K];
    VkSampler lin = VK_NULL_HANDLE, nearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout lay = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet ds[K]{}; VkImageView dsDepth[K]{};
    VkPipeline pHiz = VK_NULL_HANDLE, pTaa = VK_NULL_HANDLE, pHist = VK_NULL_HANDLE, pExpo = VK_NULL_HANDLE, pDown = VK_NULL_HANDLE, pUp = VK_NULL_HANDLE, pTone = VK_NULL_HANDLE;
    VkPipeline pGtao = VK_NULL_HANDLE, pSsgi = VK_NULL_HANDLE, pDen = VK_NULL_HANDLE, pRes = VK_NULL_HANDLE, pZc = VK_NULL_HANDLE, pSsr = VK_NULL_HANDLE; int aoCur = 0;
    std::unordered_map<uint64_t, uint32_t> prevIdx, curIdx; std::vector<float> prevRows, curRows;
    float prevVP[3][16]; bool havePrev = false; int histCur = 0, lastFlags = 0; uint32_t frame = 0; EnhIn e{};
    std::chrono::steady_clock::time_point last;
} gP;
static struct {
    VImg raw, src, live, depth; VkImageView faceView[6]{}, srcMip[IBL_SRC_MIPS]{}, liveMip[IBL_MIPS]{}, slotView[K]{};
    VkSampler smp = VK_NULL_HANDLE; VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout lay = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE; VkDescriptorSet ds = VK_NULL_HANDLE;
    VkPipeline pDown = VK_NULL_HANDLE, pPref = VK_NULL_HANDLE, pSh = VK_NULL_HANDLE; Buf sh;
    int face = 0; bool imgs = false, valid = false; uint64_t bytes = 0;
    std::vector<DrawRef> list[4];   // scenery parts, cutout scenery parts, PQS, ocean
} gI;
// 8c / 8d auxiliary resources: atmosphere LUTs, ocean spectrum / FFT / wave mips, spot-light shadow maps (fixed size, allocated with the post images)
static struct {
    VImg trans, multi, skyv, ap, fq[2], wav, spot; VkImageView wavMip[WAV_MIPS]{}, spotLayer[NSPOT]{};
    VkSampler rep = VK_NULL_HANDLE; VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout lay = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE; VkDescriptorSet ds[K]{};
    VkPipeline pTrans = VK_NULL_HANDLE, pMulti = VK_NULL_HANDLE, pSkyv = VK_NULL_HANDLE, pAp = VK_NULL_HANDLE, pSpec = VK_NULL_HANDLE, pFft = VK_NULL_HANDLE, pNrm = VK_NULL_HANDLE, pMip = VK_NULL_HANDLE;
    VkPipeline sh[2][2]{};   // spot shadows [skin][cutoff]
    bool imgs = false, atm = false, inside = false; uint64_t bytes = 0, atmWant = 0, atmDone = 0; uint32_t gen = 0, slotGen[K]{};
    int nSpot = 0; V3 spPos[NSPOT], spFwd[NSPOT]; float spTan[NSPOT], spRange[NSPOT]; std::vector<DrawRef> spList[NSPOT][2][2];
} gX;
// right, down, forward of each cube face (Vulkan face order and orientation), in the local view's world axes
static const float IBL_FB[6][3][3] = { { {0, 0, -1}, {0, -1, 0}, {1, 0, 0} }, { {0, 0, 1}, {0, -1, 0}, {-1, 0, 0} }, { {1, 0, 0}, {0, 0, 1}, {0, 1, 0} },
                                       { {1, 0, 0}, {0, 0, -1}, {0, -1, 0} }, { {1, 0, 0}, {0, -1, 0}, {0, 0, 1} }, { {-1, 0, 0}, {0, -1, 0}, {0, 0, -1} } };

EXPORT void KSPVk_SetEnhanced(const EnhIn* e) { std::lock_guard<std::mutex> l(gEnhM); gEnhReq = *e; gRScale = e->renderScale; }
// out: pass ms (IBL, pre-scene, main, Hi-Z, AO / GI, TAA, exposure, bloom, tonemap), exposure, mean log2 luminance, image MB, failed flags. Returns active flags.
EXPORT int KSPVk_GetEnhanced(float* out, int n) {
    float v[13]; memcpy(v, gPassMs, sizeof gPassMs);
    v[9] = gP.expo.p ? ((float*)gP.expo.p)[0] : 1; v[10] = gP.expo.p ? ((float*)gP.expo.p)[1] : 0; v[11] = (float)(gP.bytes / 1048576.0); v[12] = (float)gEnhFail;
    memcpy(out, v, sizeof(float) * std::min(n, 13));
    return gEnhActive;
}

// ---------------- motion: previous rows / view-projections (same frame of reference, so floating-origin and Krakensbane shifts cancel) ----------------
static uint64_t drawKey(const DrawIn& d) {   // PQS quads: the sub index is the edge-stitch variant, which changes with neighbouring LOD
    return (uint64_t)(d.mesh & 0x3fff) | (uint64_t)(d.pad0 == FAM_PQS ? 0 : d.sub & 15) << 14 | (uint64_t)(d.mat & 0x1fff) << 18 |
           (uint64_t)(d.inst & 0x3fff) << 31 | (uint64_t)(d.pad0 & 7) << 45 | (uint64_t)((d.flags & DF_IVA) != 0) << 48;
}
static float halton(uint32_t i, uint32_t b) { float f = 1, r = 0; for (; i; i /= b) { f /= b; r += f * (i % b); } return r; }
// 8c-8e constants: camera basis, main-body atmosphere (from the C# FamAtmo material: 44 Rg, 45 Rt, 46 / 47 Rayleigh / Mie scale height, 48 / 49 densities,
// 50 valid, 51 Mie g, 52..54 local centre; 0..2 scaled centre, 12..14 1 / wavelength^4), eclipse occluders, sun size, shadowed spot lights.
static void enhFrame(FrameU& F, SlotScene& ss) {
    const SceneIn& H = ss.h; const float* V = H.view; const EnhIn& e = gP.e; const V3 eye = v3(H.camPos);
    F.enh[0] = gEF; F.enh[1] = (int)gP.frame; F.enh[2] = (gEF & EF_OCEAN) && gP.pZc ? 1 : 0;
    for (int i = 0; i < 3; i++) { F.camR[i] = V[i]; F.camU[i] = V[4 + i]; F.camF[i] = -V[8 + i]; }
    F.proj[0] = H.proj[0]; F.proj[1] = H.proj[4]; F.proj[2] = H.proj[2]; F.proj[3] = H.proj[5];
    F.jitter[2] = 1.f / gZw; F.jitter[3] = 1.f / gZh;
    const float* mats = ss.mats.data(); int am = -1; float best = 20, sunA = 1;
    for (const DrawIn& d : ss.draws) {
        if (d.pad0 == FAM_ATMO && d.mat >= 0 && d.mat < H.matCount) {
            const float* f = mats + (size_t)d.mat * MATF;
            if (f[50] <= 0 || f[45] <= f[44]) continue;
            V3 c{ f[52], f[53], f[54] }; float r = sqrtf(dot(c - eye, c - eye)) / f[45];
            if (r < best) best = r, am = d.mat;
        } else if (d.pad0 == FAM_SUN && drawValid(d, H)) {
            V3 c; float rad; drawBounds(d, gMesh[d.mesh], false, c, rad);
            V3 o = c - v3(F.camPosV[1]); float l = sqrtf(dot(o, o));
            if (l > rad) sunA = std::min(sunA, rad / l);
        }
    }
    if (sunA >= 1) sunA = 0.01f;
    sunA = std::min(std::max(sunA, 0.002f), 0.05f);
    gX.atm = am >= 0 && (gEF & EF_ATMO) && gX.imgs;
    if (gX.atm) {
        const float* f = mats + (size_t)am * MATF;
        // Rayleigh: Earth's coefficients (5.8 / 13.6 / 33.1 per Mm), tinted by the body's 1 / wavelength^4 relative to Kerbin's (stock's ratios are too flat: a grey sky)
        float tr = f[12] / 5.602f * 5.802f, tg = f[13] / 9.4733f * 13.558f, tb = f[14] / 19.644f * 33.1f, mx = std::max(std::max(tr, tg), std::max(tb, 1e-12f));
        float Rg = f[44], Rt = f[45], ch = sqrtf(Rt * Rt - Rg * Rg);
        V3 cam = eye - V3{ f[52], f[53], f[54] }; float cl = sqrtf(dot(cam, cam));
        V3 dd = (v3(F.camPosV[1]) - V3{ f[0], f[1], f[2] }) * 6000.f - cam;
        bool mapCam = H.views[0].nearClip > 0 && sqrtf(dot(dd, dd)) > std::max(500.f, 0.01f * cl);   // map view: the scaled camera is not at the local camera
        // aerial-perspective far: the whole atmosphere chord, or from outside past the horizon through it
        float c4[4] = { f[52], f[53], f[54], Rg }, p4[4] = { Rt, f[46], f[47], cl > Rt ? sqrtf(cl * cl - Rg * Rg) + ch : 2 * ch };
        float b4[4] = { tr / mx * 33.1e-6f * f[48], tg / mx * 33.1e-6f * f[48], tb / mx * 33.1e-6f * f[48], 4e-6f * f[49] };
        float x4[4] = { f[51], e.skyBright > 0 ? e.skyBright : 3, mapCam ? 2.f : 1.f, 6000 }, s4[4] = { f[0], f[1], f[2], sunA };
        memcpy(F.atmC, c4, 16); memcpy(F.atmP, p4, 16); memcpy(F.atmB, b4, 16); memcpy(F.atmX, x4, 16); memcpy(F.atmS, s4, 16);
        uint64_t h = 1469598103934665603ull;   // LUT parameters (FNV-1a): recompute the transmittance / multiple-scattering LUTs on change
        for (float v : { Rg, Rt, p4[1], p4[2], b4[0], b4[1], b4[2], b4[3], f[51] }) { uint32_t u; memcpy(&u, &v, 4); h = (h ^ u) * 1099511628211ull; }
        gX.atmWant = h;
        gX.inside = cl < Rt;
        if (f[29] <= 0)   // the stock shell is ~1.025 x the radius (Kerbin: 15 km): scale the mesh out to the atmosphere top so the sky fades out instead of ending
            for (DrawIn& d : ss.draws) {
                if (d.pad0 != FAM_ATMO || d.mat != am || !drawValid(d, H)) continue;
                V3 c; float rad; drawBounds(d, gMesh[d.mesh], false, c, rad);
                float k = Rt * 1.02f / 6000.f / std::max(rad * 0.57735f, 1e-6f);   // rad is the box half-diagonal: sqrt(3) x the sphere's
                if (k <= 1) continue;
                const float cc[3] = { c.x, c.y, c.z };
                for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) d.r[i * 4 + j] *= k; d.r[i * 4 + 3] = cc[i] + k * (d.r[i * 4 + 3] - cc[i]); }
            }
        V3 L = v3(F.lightDir), as = v3(s4); int no = 0;
        for (const DrawIn& d : ss.draws) {   // eclipse occluders: other scaled bodies between the planet and the sun
            if (d.pad0 != FAM_SCALED || no == 4 || !drawValid(d, H)) continue;
            V3 c; float rad; drawBounds(d, gMesh[d.mesh], false, c, rad);
            V3 o = c - as; if (dot(o, o) < rad * rad * 0.25f) continue;
            o = o * 6000.f; rad *= 6000.f;
            float t = dot(o, L), pp = sqrtf(std::max(dot(o, o) - t * t, 0.f));
            if (t <= 0 || pp > rad + Rt + t * sunA * 2) continue;
            F.occ[no][0] = o.x; F.occ[no][1] = o.y; F.occ[no][2] = o.z; F.occ[no][3] = rad; no++;
        }
    }
    F.sfx[0] = sunA * (e.sunSoft > 0 ? e.sunSoft : 1); F.sfx[1] = e.aoStrength; F.sfx[2] = e.giStrength; F.sfx[3] = 0.6f;

    for (int& i : F.spotIdx) i = -1;
    gX.nSpot = 0;
    if ((gEF & EF_SPOTSH) && gX.imgs) {   // the four strongest spot lights at the camera get a shadow layer
        std::pair<float, int> c[MAXLIGHT]; int n = 0;
        for (int i = 0; i < H.lightCount && i < 8; i++) {
            const LightIn& l = H.lights[i];
            if (((int)l.col[3] & 3) != 1 || l.pos[3] <= 0) continue;
            V3 o = v3(l.pos) - eye; float r = l.pos[3], lum = 0.2126f * l.col[0] + 0.7152f * l.col[1] + 0.0722f * l.col[2];
            c[n++] = { -lum * r * r / (dot(o, o) + r * r), i };
        }
        std::sort(c, c + n);
        for (int k = 0; k < n && k < NSPOT; k++) {
            const LightIn& l = H.lights[c[k].second];
            V3 P = v3(l.pos), fw = norm(v3(l.dir)), a = fabsf(fw.y) < 0.99f ? V3{ 0, 1, 0 } : V3{ 1, 0, 0 }, r = norm(cross(a, fw)), u = cross(fw, r);
            float t = tanf(std::min(acosf(std::min(std::max(l.dir[3], -1.f), 1.f)) + 0.05f, 1.45f)), fr = l.pos[3], nr = 0.05f, q = fr / (fr - nr);
            float* M = F.spotVP[k];
            M[0] = r.x / t; M[1] = r.y / t; M[2] = r.z / t; M[3] = -dot(r, P) / t;
            M[4] = u.x / t; M[5] = u.y / t; M[6] = u.z / t; M[7] = -dot(u, P) / t;
            M[8] = fw.x * q; M[9] = fw.y * q; M[10] = fw.z * q; M[11] = -dot(fw, P) * q - nr * q;
            M[12] = fw.x; M[13] = fw.y; M[14] = fw.z; M[15] = -dot(fw, P);
            F.spotIdx[c[k].second] = k;
            gX.spPos[k] = P; gX.spFwd[k] = fw; gX.spTan[k] = t; gX.spRange[k] = fr; gX.nSpot = k + 1;
        }
    }
}
static void enhPrepare(FrameU& F, SlotScene& ss) {
    int si = (int)(&ss - gSS);
    enhFrame(F, ss);
    if (gEF & EF_TAA) {
        uint32_t i = gP.frame % 8 + 1;
        F.jitter[0] = (halton(i, 2) - 0.5f) * 2 / gZw; F.jitter[1] = (halton(i, 3) - 0.5f) * 2 / gZh;
        gP.jit[0] = F.jitter[0] * 0.5f; gP.jit[1] = F.jitter[1] * 0.5f;
    }
    F.rs[0] = (float)gP.w / gP.ow; F.rs[1] = (gEF & EF_TAA) && gUp ? log2f(F.rs[0]) : 0; F.rs[2] = F.rs[3] = 0;   // TAAU: textures sharpened to the output size
    for (int v = 0; v < 3; v++) { memcpy(F.pvp[v], gP.havePrev ? gP.prevVP[v] : F.vp[v], 64); memcpy(gP.prevVP[v], F.vp[v], 64); }
    F.ibl[0] = (float)(((gEF & EF_PBR) ? 1 : 0) | ((gEF & EF_IBL) && gI.valid ? 2 : 0)); F.ibl[1] = gP.e.ambFill; F.ibl[2] = IBL_MIPS - 1; F.ibl[3] = gP.e.iblScale;
    if (gEF & EF_IBL) {   // this frame's cube face, at each view's camera: axes mapped through the local view's rotation into the view's own world
        const SceneIn& H = ss.h;
        for (int v = 0; v < 3; v++) {
            const float* Vv = v ? H.views[v - 1].view : H.view; float nc = v ? H.views[v - 1].nearClip : H.nearClip;
            if (nc <= 0) continue;
            float ax[3][3]; const float* cp = F.camPosV[v]; float* M = F.vp[3 + v];
            for (int a = 0; a < 3; a++) {
                const float* e = IBL_FB[gI.face][a]; float c[3];
                for (int i = 0; i < 3; i++) c[i] = H.view[i * 4] * e[0] + H.view[i * 4 + 1] * e[1] + H.view[i * 4 + 2] * e[2];
                for (int j = 0; j < 3; j++) ax[a][j] = Vv[j] * c[0] + Vv[4 + j] * c[1] + Vv[8 + j] * c[2];
            }
            memset(M, 0, 64);
            for (int r = 0; r < 4; r++) if (r != 2) { const float* x = ax[r == 3 ? 2 : r]; memcpy(M + r * 4, x, 12); M[r * 4 + 3] = -(x[0] * cp[0] + x[1] * cp[1] + x[2] * cp[2]); }
            M[11] = nc;   // infinite reversed Z, like viewProj()
            memcpy(F.viewZV[3 + v], M + 12, 16);
        }
    }
    if (!(gEF & EF_TAA) || !gP.prev[si].p) return;
    float* o = (float*)gP.prev[si].p;
    gP.curIdx.clear(); gP.curRows.clear();
    for (size_t i = 0; i < ss.draws.size(); i++) {
        const DrawIn& d = ss.draws[i];
        uint64_t k = drawKey(d);
        auto it = gP.prevIdx.find(k);
        const float* pr = it != gP.prevIdx.end() ? &gP.prevRows[it->second * 12] : nullptr;
        float* r = o + i * 12;
        if (d.pad0 == FAM_PART && (d.flags & DF_SKIN)) { for (int j = 0; j < 3; j++) r[j] = pr ? d.r[j] - pr[j] : 0; r[3] = 0; }   // bounds-centre delta
        else memcpy(r, pr ? pr : d.r, 48);
        if (gP.curIdx.emplace(k, (uint32_t)(gP.curRows.size() / 12)).second) gP.curRows.insert(gP.curRows.end(), d.r, d.r + 12);
    }
    std::swap(gP.prevIdx, gP.curIdx); std::swap(gP.prevRows, gP.curRows);
}

// ---------------- resources ----------------
static VkPipeline computePipe(const uint32_t* code, size_t bytes, VkPipelineLayout lay = VK_NULL_HANDLE) {
    VkShaderModule m = shmod(code, bytes);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr }; ci.layout = lay ? lay : gP.lay;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(gVd, VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(gVd, m, nullptr);
    return p;
}
// Once, on a worker thread: Enhanced scene pipelines, post layout and pipelines. False: Enhanced unavailable this session.
static bool enhInit() {
    VkShaderModule pv = shmod(part_enh_vert, sizeof part_enh_vert), psv = shmod(part_skin_enh_vert, sizeof part_skin_enh_vert), pf = shmod(part_enh_frag, sizeof part_enh_frag),
                   wv = shmod(world_enh_vert, sizeof world_enh_vert);
    VkShaderModule wf[NFAM] = { VK_NULL_HANDLE, shmod(pqs_enh_frag, sizeof pqs_enh_frag), shmod(ocean_enh_frag, sizeof ocean_enh_frag), shmod(scaled_enh_frag, sizeof scaled_enh_frag),
                                shmod(sun_enh_frag, sizeof sun_enh_frag), shmod(atmo_enh_frag, sizeof atmo_enh_frag), shmod(galaxy_enh_frag, sizeof galaxy_enh_frag) };
    bool ok = pv && psv && pf && wv;
    for (int s = 0; s < 2 && ok; s++) for (int c = 0; c < 2 && ok; c++) ok = makePipe(gEPipe[s][c], s ? psv : pv, pf, false, s, c, FAM_PART, false, true);
    ok = ok && makePipe(gEMaskPipe, pv, pf, false, false, false, FAM_PART, true, true);
    for (int f = 1; f < NFAM && ok; f++) ok = wf[f] && makePipe(gEWPipe[f], wv, wf[f], false, false, false, f, false, true);
    for (VkShaderModule m : { pv, psv, pf, wv }) if (m) vkDestroyShaderModule(gVd, m, nullptr);
    for (VkShaderModule m : wf) if (m) vkDestroyShaderModule(gVd, m, nullptr);
    if (!ok) { logf("Enhanced: scene pipeline creation failed"); return false; }

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sci.maxLod = VK_LOD_CLAMP_NONE;
    VKCHECK(vkCreateSampler(gVd, &sci, nullptr, &gP.lin));
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    VKCHECK(vkCreateSampler(gVd, &sci, nullptr, &gP.nearest));
    const VkShaderStageFlags CF = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorBindingFlags PB = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBinding b[5] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16, CF, nullptr }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16, CF, nullptr },
                                          { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAXHIZ, CF, nullptr }, { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, CF, nullptr },
                                          { 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, CF, nullptr } };
    VkDescriptorBindingFlags bf[5] = { PB, PB, PB, 0, 0 };
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO}; bfi.bindingCount = 5; bfi.pBindingFlags = bf;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dli.pNext = &bfi; dli.bindingCount = 5; dli.pBindings = b;
    VKCHECK(vkCreateDescriptorSetLayout(gVd, &dli, nullptr, &gP.dsl));
    VkPushConstantRange pcr{ CF, 0, 48 };
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &gP.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKCHECK(vkCreatePipelineLayout(gVd, &pli, nullptr, &gP.lay));
    VkDescriptorPoolSize ps[4] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * K }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (16 + MAXHIZ) * K }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, K },
                                   { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, K } };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = K; dpi.poolSizeCount = 4; dpi.pPoolSizes = ps;
    VKCHECK(vkCreateDescriptorPool(gVd, &dpi, nullptr, &gP.pool));
    VkDescriptorSetLayout ls[K]; for (auto& l : ls) l = gP.dsl;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = gP.pool; dai.descriptorSetCount = K; dai.pSetLayouts = ls;
    VKCHECK(vkAllocateDescriptorSets(gVd, &dai, gP.ds));
    auto writeUbo = [](VkDescriptorSet ds, int k, uint32_t bind) {
        VkDescriptorBufferInfo ub{ gSS[k].ubo.b, 0, sizeof(FrameU) };
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = ds; w.dstBinding = bind; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &ub;
        vkUpdateDescriptorSets(gVd, 1, &w, 0, nullptr);
    };
    for (int k = 0; k < K; k++) writeUbo(gP.ds[k], k, 4);
    if (!makeBuf(gP.expo, 16 + 256 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
    memset(gP.expo.p, 0, 16 + 256 * 4); ((float*)gP.expo.p)[0] = 1;
    if (!(gP.pTone = fsPipe(tonemap_frag, sizeof tonemap_frag, gP.lay))) { logf("Enhanced: tonemap pipeline failed"); return false; }
    gP.pHist = computePipe(post_hist_comp, sizeof post_hist_comp); gP.pExpo = computePipe(post_expo_comp, sizeof post_expo_comp);
    if (gStorageIdx) {
        gP.pHiz = computePipe(post_hiz_comp, sizeof post_hiz_comp); gP.pTaa = computePipe(post_taa_comp, sizeof post_taa_comp);
        gP.pDown = computePipe(post_bdown_comp, sizeof post_bdown_comp); gP.pUp = computePipe(post_bup_comp, sizeof post_bup_comp);
        gP.pGtao = computePipe(post_gtao_comp, sizeof post_gtao_comp); gP.pSsgi = computePipe(post_ssgi_comp, sizeof post_ssgi_comp); gP.pSsr = computePipe(post_ssr_comp, sizeof post_ssr_comp);
        gP.pDen = computePipe(post_den_comp, sizeof post_den_comp); gP.pRes = computePipe(post_resolve_comp, sizeof post_resolve_comp);
        gP.pZc = computePipe(post_zcopy_comp, sizeof post_zcopy_comp);
    } else logf("Enhanced: no dynamic storage-image indexing; TAA, bloom and Hi-Z unavailable");
    if (gStorageIdx) {   // IBL: raw face array, source cube, prefiltered mips, SH
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        VKCHECK(vkCreateSampler(gVd, &sci, nullptr, &gI.smp));
        const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutBinding ib[5] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, nullptr }, { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, nullptr },
                                               { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8, C, nullptr }, { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8, C, nullptr }, { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, nullptr } };
        VkDescriptorBindingFlags ibf[5] = { 0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0 };
        bfi.bindingCount = 5; bfi.pBindingFlags = ibf; dli.bindingCount = 5; dli.pBindings = ib;
        VKCHECK(vkCreateDescriptorSetLayout(gVd, &dli, nullptr, &gI.dsl));
        VkPushConstantRange ipr{ C, 0, 32 };
        pli.pSetLayouts = &gI.dsl; pli.pPushConstantRanges = &ipr;
        VKCHECK(vkCreatePipelineLayout(gVd, &pli, nullptr, &gI.lay));
        VkDescriptorPoolSize ips[3] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
        dpi.maxSets = 1; dpi.poolSizeCount = 3; dpi.pPoolSizes = ips;
        VKCHECK(vkCreateDescriptorPool(gVd, &dpi, nullptr, &gI.pool));
        dai.descriptorPool = gI.pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &gI.dsl;
        VKCHECK(vkAllocateDescriptorSets(gVd, &dai, &gI.ds));
        if (makeBuf(gI.sh, 9 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) {
            memset(gI.sh.p, 0, 9 * 16);
            gI.pDown = computePipe(ibl_down_comp, sizeof ibl_down_comp, gI.lay); gI.pPref = computePipe(ibl_pref_comp, sizeof ibl_pref_comp, gI.lay);
            gI.pSh = computePipe(ibl_sh_comp, sizeof ibl_sh_comp, gI.lay);
        }
    }
    if (gStorageIdx) {   // 8c / 8d auxiliary compute: 0 frame, 1 LUT samplers, 2 LUT / sky-view images, 3 aerial perspective, 4 FFT ping-pong, 5 wave mips
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VKCHECK(vkCreateSampler(gVd, &sci, nullptr, &gX.rep));
        const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutBinding xb[6] = { { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, C, nullptr }, { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, C, nullptr },
                                               { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4, C, nullptr }, { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, nullptr },
                                               { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2, C, nullptr }, { 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, WAV_MIPS, C, nullptr } };
        VkDescriptorBindingFlags xf[6] = { 0, PB, PB, 0, 0, PB };
        bfi.bindingCount = 6; bfi.pBindingFlags = xf; dli.bindingCount = 6; dli.pBindings = xb;
        VKCHECK(vkCreateDescriptorSetLayout(gVd, &dli, nullptr, &gX.dsl));
        VkPushConstantRange xr{ C, 0, 48 };
        pli.pSetLayouts = &gX.dsl; pli.pPushConstantRanges = &xr;
        VKCHECK(vkCreatePipelineLayout(gVd, &pli, nullptr, &gX.lay));
        VkDescriptorPoolSize xs[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, K }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * K }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (7 + WAV_MIPS) * K } };
        dpi.maxSets = K; dpi.poolSizeCount = 3; dpi.pPoolSizes = xs;
        VKCHECK(vkCreateDescriptorPool(gVd, &dpi, nullptr, &gX.pool));
        for (auto& l : ls) l = gX.dsl;
        dai.descriptorPool = gX.pool; dai.descriptorSetCount = K; dai.pSetLayouts = ls;
        VKCHECK(vkAllocateDescriptorSets(gVd, &dai, gX.ds));
        for (int k = 0; k < K; k++) writeUbo(gX.ds[k], k, 0);
        gX.pTrans = computePipe(atmo_trans_comp, sizeof atmo_trans_comp, gX.lay); gX.pMulti = computePipe(atmo_multi_comp, sizeof atmo_multi_comp, gX.lay);
        gX.pSkyv = computePipe(atmo_skyv_comp, sizeof atmo_skyv_comp, gX.lay); gX.pAp = computePipe(atmo_ap_comp, sizeof atmo_ap_comp, gX.lay);
        gX.pSpec = computePipe(ocean_spec_comp, sizeof ocean_spec_comp, gX.lay); gX.pFft = computePipe(ocean_fft_comp, sizeof ocean_fft_comp, gX.lay);
        gX.pNrm = computePipe(ocean_nrm_comp, sizeof ocean_nrm_comp, gX.lay); gX.pMip = computePipe(ocean_mip_comp, sizeof ocean_mip_comp, gX.lay);
    }
    {   // spot-light shadow casters: the cascade pipelines with the Enhanced vertex shader (spot view-projections)
        VkShaderModule sv = shmod(shadow_enh_vert, sizeof shadow_enh_vert), ssv = shmod(shadow_skin_enh_vert, sizeof shadow_skin_enh_vert), sf = shmod(shadow_frag, sizeof shadow_frag);
        bool sok = sv && ssv && sf;
        for (int s = 0; s < 2 && sok; s++) for (int c = 0; c < 2 && sok; c++) sok = makePipe(gX.sh[s][c], s ? ssv : sv, c ? sf : VK_NULL_HANDLE, true, s, c);
        for (VkShaderModule m : { sv, ssv, sf }) if (m) vkDestroyShaderModule(gVd, m, nullptr);
        if (!sok) for (auto& a : gX.sh) for (VkPipeline& q : a) { if (q) vkDestroyPipeline(gVd, q, nullptr); q = VK_NULL_HANDLE; }
    }
    int fail = (!gP.pHist || !gP.pExpo ? EF_AUTOEXP : 0) | (!gP.pHiz ? EF_HIZ : 0) | (!gP.pTaa ? EF_TAA : 0) | (!gP.pDown || !gP.pUp ? EF_BLOOM : 0) |
               (!gI.pDown || !gI.pPref || !gI.pSh ? EF_IBL : 0) | (!gP.pGtao || !gP.pDen || !gP.pRes ? EF_GTAO | EF_CONTACT | EF_SSGI : 0) | (!gP.pSsgi ? EF_SSGI : 0) | (!gP.pSsr || !gP.pRes ? EF_SSR : 0) |
               (!gX.pTrans || !gX.pMulti || !gX.pSkyv || !gX.pAp ? EF_ATMO : 0) | (!gX.pSpec || !gX.pFft || !gX.pNrm || !gX.pMip ? EF_FFT : 0) | (!gX.sh[0][0] ? EF_SPOTSH : 0);
    if (fail) { gEnhFail |= fail; logf("Enhanced: features unavailable (flags %x)", fail); }
    logf("Enhanced ready");
    return true;
}

static VkImageView arrView(VkImage img, uint32_t mip, uint32_t layer, uint32_t n) {   // rgba16f: one layer (2D) or all six (2D array)
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = img; vi.viewType = n > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT; vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, n };
    VkImageView v = VK_NULL_HANDLE; vkCreateImageView(gVd, &vi, nullptr, &v); return v;
}
static void freeIblImgs() {
    for (VkImageView& v : gI.faceView) { if (v) vkDestroyImageView(gVd, v, nullptr); v = VK_NULL_HANDLE; }
    for (VkImageView& v : gI.srcMip) { if (v) vkDestroyImageView(gVd, v, nullptr); v = VK_NULL_HANDLE; }
    for (VkImageView& v : gI.liveMip) { if (v) vkDestroyImageView(gVd, v, nullptr); v = VK_NULL_HANDLE; }
    for (VImg* i : { &gI.raw, &gI.src, &gI.live, &gI.depth }) freeVImg(*i);
    for (auto& v : gI.slotView) v = VK_NULL_HANDLE;
    gI.imgs = gI.valid = false; gI.bytes = 0;
}
static bool makeIblImgs() {
    const VkFormat HF = VK_FORMAT_R16G16B16A16_SFLOAT; const VkImageUsageFlags sto = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!makeVImg(gI.raw, HF, IBL_RES, IBL_RES, 1, 6, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !makeVImg(gI.src, HF, IBL_RES, IBL_RES, IBL_SRC_MIPS, 6, sto, VK_IMAGE_ASPECT_COLOR_BIT, {}, true) || !makeVImg(gI.live, HF, IBL_RES, IBL_RES, IBL_MIPS, 6, sto, VK_IMAGE_ASPECT_COLOR_BIT, {}, true) ||
        !makeVImg(gI.depth, VK_FORMAT_D32_SFLOAT, IBL_RES, IBL_RES, 1, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) { freeIblImgs(); return false; }
    for (uint32_t i = 0; i < 6; i++) gI.faceView[i] = arrView(gI.raw.img, 0, i, 1);
    for (uint32_t i = 0; i < IBL_SRC_MIPS; i++) gI.srcMip[i] = arrView(gI.src.img, i, 0, 6);
    for (uint32_t i = 0; i < IBL_MIPS; i++) gI.liveMip[i] = arrView(gI.live.img, i, 0, 6);
    for (VImg* i : { &gI.raw, &gI.src, &gI.live, &gI.depth }) { VkMemoryRequirements mr; vkGetImageMemoryRequirements(gVd, i->img, &mr); gI.bytes += mr.size; }
    oneShot([&](VkCommandBuffer cb) {
        for (VImg* i : { &gI.raw, &gI.src, &gI.live })
            imgBarrier(cb, i->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    });
    VkDescriptorImageInfo smp[2] = { { gP.nearest, gI.raw.view, VK_IMAGE_LAYOUT_GENERAL }, { gI.smp, gI.src.view, VK_IMAGE_LAYOUT_GENERAL } }, ws[IBL_SRC_MIPS], wl[IBL_MIPS];
    for (int i = 0; i < IBL_SRC_MIPS; i++) ws[i] = { VK_NULL_HANDLE, gI.srcMip[i], VK_IMAGE_LAYOUT_GENERAL };
    for (int i = 0; i < IBL_MIPS; i++) wl[i] = { VK_NULL_HANDLE, gI.liveMip[i], VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorBufferInfo sb{ gI.sh.b, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wr[4]{};
    for (auto& x : wr) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = gI.ds; }
    wr[0].dstBinding = 0; wr[0].descriptorCount = 2; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = smp;
    wr[1].dstBinding = 2; wr[1].descriptorCount = IBL_SRC_MIPS; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[1].pImageInfo = ws;
    wr[2].dstBinding = 3; wr[2].descriptorCount = IBL_MIPS; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[2].pImageInfo = wl;
    wr[3].dstBinding = 4; wr[3].descriptorCount = 1; wr[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[3].pBufferInfo = &sb;
    vkUpdateDescriptorSets(gVd, 4, wr, 0, nullptr);
    gI.imgs = true; gI.valid = false; gI.face = 0;
    return true;
}
static void freeAuxImgs() {
    for (VkImageView& v : gX.wavMip) { if (v) vkDestroyImageView(gVd, v, nullptr); v = VK_NULL_HANDLE; }
    for (VkImageView& v : gX.spotLayer) { if (v) vkDestroyImageView(gVd, v, nullptr); v = VK_NULL_HANDLE; }
    for (VImg* i : { &gX.trans, &gX.multi, &gX.skyv, &gX.ap, &gX.fq[0], &gX.fq[1], &gX.wav, &gX.spot }) freeVImg(*i);
    gX.imgs = false; gX.atmDone = 0; gX.bytes = 0;
}
static VkImageView mipView(VkImage img, VkFormat f, uint32_t mip);
static bool makeAuxImgs() {
    const VkFormat HF = VK_FORMAT_R16G16B16A16_SFLOAT; const VkImageUsageFlags sto = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; const VkImageAspectFlags CA = VK_IMAGE_ASPECT_COLOR_BIT;
    if (!makeVImg(gX.trans, HF, 256, 64, 1, 1, sto, CA) || !makeVImg(gX.multi, HF, 32, 32, 1, 1, sto, CA) || !makeVImg(gX.skyv, HF, 192, 108, 1, 1, sto, CA) ||
        !makeVImg(gX.ap, HF, 32, 32, 1, 1, sto, CA, {}, false, 32) || !makeVImg(gX.fq[0], VK_FORMAT_R32G32B32A32_SFLOAT, FFT_N, FFT_N, 1, 1, sto, CA) ||
        !makeVImg(gX.fq[1], VK_FORMAT_R32G32B32A32_SFLOAT, FFT_N, FFT_N, 1, 1, sto, CA) || !makeVImg(gX.wav, HF, FFT_N, FFT_N, WAV_MIPS, 1, sto, CA) ||
        !makeVImg(gX.spot, VK_FORMAT_D32_SFLOAT, SPOT_RES, SPOT_RES, 1, NSPOT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) {
        freeAuxImgs(); return false;
    }
    for (uint32_t i = 0; i < WAV_MIPS; i++) gX.wavMip[i] = mipView(gX.wav.img, HF, i);
    for (uint32_t i = 0; i < NSPOT; i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = gX.spot.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_D32_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1 }; vkCreateImageView(gVd, &vi, nullptr, &gX.spotLayer[i]);
    }
    for (VImg* i : { &gX.trans, &gX.multi, &gX.skyv, &gX.ap, &gX.fq[0], &gX.fq[1], &gX.wav, &gX.spot }) { VkMemoryRequirements mr; vkGetImageMemoryRequirements(gVd, i->img, &mr); gX.bytes += mr.size; }
    oneShot([&](VkCommandBuffer cb) {
        for (VImg* i : { &gX.trans, &gX.multi, &gX.skyv, &gX.ap, &gX.fq[0], &gX.fq[1], &gX.wav })
            imgBarrier(cb, i->img, CA, VK_REMAINING_MIP_LEVELS, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        imgBarrier(cb, gX.spot.img, VK_IMAGE_ASPECT_DEPTH_BIT, 1, NSPOT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT);
    });
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo sx[2] = { { gP.lin, gX.trans.view, G }, { gP.lin, gX.multi.view, G } }, wx[3] = { { VK_NULL_HANDLE, gX.trans.view, G }, { VK_NULL_HANDLE, gX.multi.view, G }, { VK_NULL_HANDLE, gX.skyv.view, G } },
                          wv{ VK_NULL_HANDLE, gX.ap.view, G }, wq[2] = { { VK_NULL_HANDLE, gX.fq[0].view, G }, { VK_NULL_HANDLE, gX.fq[1].view, G } }, wn[WAV_MIPS];
    for (int i = 0; i < WAV_MIPS; i++) wn[i] = { VK_NULL_HANDLE, gX.wavMip[i], G };
    for (VkDescriptorSet ds : gX.ds) {
        VkWriteDescriptorSet wr[5]{};
        const uint32_t bind[5] = { 1, 2, 3, 4, 5 }, cnt[5] = { 2, 3, 1, 2, WAV_MIPS }; const VkDescriptorImageInfo* src[5] = { sx, wx, &wv, wq, wn };
        for (int i = 0; i < 5; i++) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = ds; wr[i].dstBinding = bind[i]; wr[i].descriptorCount = cnt[i]; wr[i].pImageInfo = src[i];
            wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        vkUpdateDescriptorSets(gVd, 5, wr, 0, nullptr);
    }
    gX.imgs = true; gX.atmDone = 0;
    return true;
}
static void freePostImgs() {
    freeIblImgs(); freeAuxImgs();
    for (VkImageView& v : gP.bloomMip) { if (v) vkDestroyImageView(gVd, v, nullptr); v = VK_NULL_HANDLE; }
    for (VkImageView& v : gP.hizMip) { if (v) vkDestroyImageView(gVd, v, nullptr); v = VK_NULL_HANDLE; }
    for (VImg* i : { &gP.hdr, &gP.nrm, &gP.rm, &gP.mv, &gP.alb, &gP.hist[0], &gP.hist[1], &gP.bloom, &gP.hiz, &gP.ao, &gP.gi, &gP.aoH[0], &gP.aoH[1], &gP.giH[0], &gP.giH[1], &gP.zc, &gP.ssr }) freeVImg(*i);
    for (auto& v : gP.dsDepth) v = VK_NULL_HANDLE;
    gP.imgs = false; gP.bytes = 0; gP.havePrev = false;
}
static VkImageView mipView(VkImage img, VkFormat f, uint32_t mip) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1 };
    VkImageView v = VK_NULL_HANDLE; vkCreateImageView(gVd, &vi, nullptr, &v); return v;
}
static bool makePostImgs(int w, int h, int ow, int oh) {   // w, h: scene size; the TAA history is output size
    const VkImageUsageFlags att = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, sto = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkFormat HF = VK_FORMAT_R16G16B16A16_SFLOAT;
    int bw = std::max(1, w / 2), bh = std::max(1, h / 2), hw = (w + 1) / 2, hh = (h + 1) / 2;
    gP.nHiz = 1; while (gP.nHiz < MAXHIZ - 1 && std::max(bw, bh) >> gP.nHiz) gP.nHiz++;   // Wz[15]: the ocean's depth copy
    gP.w = w; gP.h = h; gP.ow = ow; gP.oh = oh;
    const VkImageAspectFlags CA = VK_IMAGE_ASPECT_COLOR_BIT;
    if (!makeVImg(gP.hdr, HF, w, h, 1, 1, att | VK_IMAGE_USAGE_STORAGE_BIT, CA) || !makeVImg(gP.nrm, HF, w, h, 1, 1, att, CA) ||
        !makeVImg(gP.rm, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, att, CA) || !makeVImg(gP.mv, VK_FORMAT_R16G16_SFLOAT, w, h, 1, 1, att, CA) ||
        !makeVImg(gP.alb, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, att, CA) || !makeVImg(gP.ao, HF, hw, hh, 1, 1, sto, CA) || !makeVImg(gP.gi, HF, hw, hh, 1, 1, sto, CA) ||
        !makeVImg(gP.aoH[0], HF, hw, hh, 1, 1, sto, CA) || !makeVImg(gP.aoH[1], HF, hw, hh, 1, 1, sto, CA) || !makeVImg(gP.giH[0], HF, hw, hh, 1, 1, sto, CA) ||
        !makeVImg(gP.giH[1], HF, hw, hh, 1, 1, sto, CA) || !makeVImg(gP.zc, VK_FORMAT_R32_SFLOAT, w, h, 1, 1, sto, CA) || !makeVImg(gP.ssr, HF, hw, hh, 1, 1, sto, CA) ||
        !makeVImg(gP.hist[0], HF, ow, oh, 1, 1, sto, VK_IMAGE_ASPECT_COLOR_BIT) || !makeVImg(gP.hist[1], HF, ow, oh, 1, 1, sto, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !makeVImg(gP.bloom, HF, bw, bh, NBLOOM, 1, sto, VK_IMAGE_ASPECT_COLOR_BIT) || !makeVImg(gP.hiz, VK_FORMAT_R32_SFLOAT, bw, bh, gP.nHiz, 1, sto, VK_IMAGE_ASPECT_COLOR_BIT)) {
        freePostImgs(); return false;
    }
    for (uint32_t i = 0; i < NBLOOM; i++) gP.bloomMip[i] = mipView(gP.bloom.img, HF, i);
    for (int i = 0; i < gP.nHiz; i++) gP.hizMip[i] = mipView(gP.hiz.img, VK_FORMAT_R32_SFLOAT, i);
    gP.bytes = 0;
    for (VImg* i : { &gP.hdr, &gP.nrm, &gP.rm, &gP.mv, &gP.alb, &gP.hist[0], &gP.hist[1], &gP.bloom, &gP.hiz, &gP.ao, &gP.gi, &gP.aoH[0], &gP.aoH[1], &gP.giH[0], &gP.giH[1], &gP.zc, &gP.ssr })
        { VkMemoryRequirements mr; vkGetImageMemoryRequirements(gVd, i->img, &mr); gP.bytes += mr.size; }
    oneShot([&](VkCommandBuffer cb) {
        for (VImg* i : { &gP.hist[0], &gP.hist[1], &gP.bloom, &gP.hiz, &gP.ao, &gP.gi, &gP.aoH[0], &gP.aoH[1], &gP.giH[0], &gP.giH[1], &gP.zc, &gP.ssr })
            imgBarrier(cb, i->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    });
    // descriptors: everything but the per-slot shared depth (S[4], written per frame)
    for (VkDescriptorSet ds : gP.ds) {
        const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo s[16] = { { gP.lin, gP.hdr.view, G }, { gP.lin, gP.hist[0].view, G }, { gP.lin, gP.hist[1].view, G }, { gP.nearest, gP.mv.view, G }, {},
            { gP.lin, gP.bloom.view, G }, { gP.nearest, gP.nrm.view, G }, { gP.nearest, gP.hiz.view, G }, { gP.nearest, gP.rm.view, G }, { gP.nearest, gP.alb.view, G },
            { gP.nearest, gP.ao.view, G }, { gP.nearest, gP.gi.view, G }, { gP.lin, gP.aoH[0].view, G }, { gP.lin, gP.aoH[1].view, G }, { gP.lin, gP.giH[0].view, G }, { gP.lin, gP.giH[1].view, G } };
        VkDescriptorImageInfo c[15], z[MAXHIZ];
        c[0] = { VK_NULL_HANDLE, gP.hist[0].view, G }; c[1] = { VK_NULL_HANDLE, gP.hist[1].view, G };
        for (int i = 0; i < NBLOOM; i++) c[2 + i] = { VK_NULL_HANDLE, gP.bloomMip[i], G };
        VkImageView cv[7] = { gP.hdr.view, gP.ao.view, gP.gi.view, gP.aoH[0].view, gP.aoH[1].view, gP.giH[0].view, gP.giH[1].view };
        for (int i = 0; i < 7; i++) c[7 + i] = { VK_NULL_HANDLE, cv[i], G };
        c[14] = { VK_NULL_HANDLE, gP.ssr.view, G };
        for (int i = 0; i < gP.nHiz; i++) z[i] = { VK_NULL_HANDLE, gP.hizMip[i], G };
        VkDescriptorImageInfo zc{ VK_NULL_HANDLE, gP.zc.view, G };
        VkDescriptorBufferInfo eb{ gP.expo.b, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet wr[6]{};
        for (auto& x : wr) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = ds; }
        wr[0].dstBinding = 0; wr[0].descriptorCount = 4; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = s;
        wr[1].dstBinding = 0; wr[1].dstArrayElement = 5; wr[1].descriptorCount = 11; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[1].pImageInfo = s + 5;
        wr[2].dstBinding = 1; wr[2].descriptorCount = 15; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[2].pImageInfo = c;
        wr[3].dstBinding = 2; wr[3].descriptorCount = gP.nHiz; wr[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[3].pImageInfo = z;
        wr[4].dstBinding = 3; wr[4].descriptorCount = 1; wr[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[4].pBufferInfo = &eb;
        wr[5].dstBinding = 2; wr[5].dstArrayElement = 15; wr[5].descriptorCount = 1; wr[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[5].pImageInfo = &zc;
        vkUpdateDescriptorSets(gVd, 6, wr, 0, nullptr);
    }
    gP.imgs = true; gP.havePrev = false;
    if (gI.pDown) { if (makeIblImgs()) gP.bytes += gI.bytes; else { gEnhFail |= EF_IBL; logf("Enhanced: IBL image allocation failed"); } }
    if (gX.lay) { if (makeAuxImgs()) gP.bytes += gX.bytes; else { gEnhFail |= EF_ATMO | EF_FFT | EF_SPOTSH; logf("Enhanced: atmosphere / ocean / spot image allocation failed"); } }
    gX.gen++;
    logf("Enhanced images %dx%d (output %dx%d): %.1f MB", w, h, ow, oh, gP.bytes / 1048576.0);
    return true;
}

// Render thread, before scenePrepare: the features this frame records (0: stock path).
static int enhBegin(int si, ImageSet& set) {
    EnhIn e; { std::lock_guard<std::mutex> l(gEnhM); e = gEnhReq; }
    int f = (e.flags & EF_ON) && !(gEnhFail & EF_ON) && gSceneOK ? e.flags : 0;
    if (!f) {
        if (gP.imgs) { vkQueueWaitIdle(gQ); freePostImgs(); logf("Enhanced off: images freed"); }
        gEnhActive = 0; return 0;
    }
    int st = gP.init;
    if (st == 0) { gP.init = 1; gP.builder = std::thread([] { auto t = std::chrono::steady_clock::now(); bool ok = enhInit(); if (!ok) gEnhFail |= EF_ON;
        logf("Enhanced pipelines built in %.0f ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count()); gP.init = ok ? 2 : 3; }); }
    if (st != 2) { gEnhActive = 0; return 0; }   // stock frames until the pipelines are built
    if (!gP.imgs || gP.w != gRW || gP.h != gRH || gP.ow != set.w || gP.oh != set.h) {
        vkQueueWaitIdle(gQ); freePostImgs();
        if (!makePostImgs(gRW, gRH, set.w, set.h)) { logf("Enhanced: image allocation failed"); gEnhFail |= EF_ON; gEnhActive = 0; return 0; }
    }
    if (!gP.prev[si].b) {
        if (!makeBuf(gP.prev[si], (VkDeviceSize)MAXDRAW * 48, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) { gEnhFail |= EF_ON; gEnhActive = 0; return 0; }
        memset(gP.prev[si].p, 0, (size_t)MAXDRAW * 48);
        VkDescriptorBufferInfo bi{ gP.prev[si].b, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = gSS[si].ds; w.dstBinding = 7; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(gVd, 1, &w, 0, nullptr);
    }
    const VkImageView dv = gUp ? gU.lz.view : set.depth[si].view;
    if (gP.dsDepth[si] != dv) {
        VkDescriptorImageInfo ii{ gP.nearest, dv, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = gP.ds[si]; w.dstBinding = 0; w.dstArrayElement = 4; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
        vkUpdateDescriptorSets(gVd, 1, &w, 0, nullptr);
        gP.dsDepth[si] = dv;
    }
    if (gI.imgs && gI.slotView[si] != gI.live.view) {   // binding 8 / 9: IBL cube and SH
        VkDescriptorImageInfo ii{ gI.smp, gI.live.view, VK_IMAGE_LAYOUT_GENERAL }; VkDescriptorBufferInfo bi{ gI.sh.b, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w[2]{};
        for (int i = 0; i < 2; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = gSS[si].ds; w[i].dstBinding = 8 + i; w[i].descriptorCount = 1; }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &bi;
        vkUpdateDescriptorSets(gVd, 2, w, 0, nullptr);
        gI.slotView[si] = gI.live.view;
    }
    if (gX.slotGen[si] != gX.gen) {   // scene bindings 11..17 (8c / 8d)
        const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo ii[7] = { { gP.nearest, gP.zc.view, G }, { gP.lin, gX.ap.view, G }, { gP.lin, gX.skyv.view, G }, { gP.lin, gX.trans.view, G },
                                        { gShadowSmp, gX.spot.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL }, { gX.rep, gX.wav.view, G }, { gP.lin, gX.multi.view, G } };
        VkWriteDescriptorSet w[7]{}; uint32_t n = 0;
        for (int i = 0; i < 7; i++) if (ii[i].imageView) {
            VkWriteDescriptorSet& x = w[n++]; x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = gSS[si].ds; x.dstBinding = 11 + i; x.descriptorCount = 1;
            x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; x.pImageInfo = &ii[i];
        }
        if (n) vkUpdateDescriptorSets(gVd, n, w, 0, nullptr);
        gX.slotGen[si] = gX.gen;
    }
    f &= ~gEnhFail;
    if (!gI.imgs) f &= ~EF_IBL;
    if (!gX.imgs) f &= ~(EF_ATMO | EF_FFT | EF_SPOTSH);
    if (!(f & EF_GBUF)) f &= ~(EF_GTAO | EF_SSGI | EF_CONTACT | EF_SSR);
    if (!(f & EF_OCEAN)) f &= ~EF_FFT;
    if ((f ^ gP.lastFlags) & EF_TAA) gP.havePrev = false;   // TAA (re)enabled: no valid history or motion rows
    if ((f ^ gP.lastFlags) & EF_IBL) gI.valid = false, gI.face = 0;
    gP.e = e; gEnhActive = f;
    return f;
}

static void memBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
static const VkPipelineStageFlags PS_ALL = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

// After scenePrepare (shadows recorded, scene set bound): one cube face per frame from the camera position, then after the sixth face
// linearise + mip the source cube, GGX-prefilter it and project SH9. Faces see the sky, scaled bodies, terrain and Local Scenery
// (KSC, editor buildings), not the vessel, so parts never reflect themselves.
static void iblRecord(VkCommandBuffer cb, SlotScene& ss) {
    const SceneIn& H = ss.h; const int f = gI.face;
    const V3 R = v3(IBL_FB[f][0]), D = v3(IBL_FB[f][1]), Fw = v3(IBL_FB[f][2]), eye = v3(H.camPos), pl[4] = { Fw + R, Fw - R, Fw + D, Fw - D };
    const int *ins = (const int*)ss.insts.data(), *matInfo = (const int*)ss.mats.data();
    for (auto& l : gI.list) l.clear();
    for (const DrawIn& d : ss.draws) {
        int fam = d.pad0;
        if ((fam != FAM_PART && fam != FAM_PQS && fam != FAM_OCEAN) || !drawValid(d, H)) continue;
        if (fam == FAM_PART && ((d.flags & (DF_SKIN | DF_IVA | DF_MASK)) || !(ins[d.inst * INSTF + 22] & 4))) continue;   // layer bit 2: Local Scenery
        const MeshRec& m = gMesh[d.mesh];
        V3 c; float rad; drawBounds(d, m, false, c, rad);
        V3 o = c - eye; float dist = sqrtf(dot(o, o));
        bool vis = rad >= dist * 0.004f && dot(Fw, o) >= -rad;   // smaller than a face texel: skipped
        for (int k = 0; k < 4 && vis; k++) vis = dot(pl[k], o) >= -rad * 1.4143f;
        if (vis) gI.list[fam == FAM_PQS ? 2 : fam == FAM_OCEAN ? 3 : (matInfo[d.mat * MATF + 44] & MF_CUTOFF) ? 1 : 0].push_back({ &d, &m, dist - rad });
    }
    for (auto& l : gI.list) std::sort(l.begin(), l.end(), [](const DrawRef& a, const DrawRef& b) { return a.k < b.k; });

    memBarrier(cb, PS_ALL, PS_ALL);   // the previous cycle's filtering is done with the raw faces; earlier frames are done reading the cube
    imgBarrier(cb, gI.depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att[6]{};
    for (auto& a : att) { a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; a.imageLayout = VK_IMAGE_LAYOUT_GENERAL; a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; }
    att[0].imageView = gI.faceView[f];
    VkRenderingAttachmentInfo za{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; za.imageView = gI.depth.view; za.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    za.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; za.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = { { 0, 0 }, { IBL_RES, IBL_RES } }; ri.layerCount = 1; ri.colorAttachmentCount = 6; ri.pColorAttachments = att; ri.pDepthAttachment = &za;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp{ 0, 0, (float)IBL_RES, (float)IBL_RES, 0, 1 }; VkRect2D sc{ { 0, 0 }, { IBL_RES, IBL_RES } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    runList(cb, ss, gWorld[FAM_GALAXY], gEWPipe[FAM_GALAXY], false, 5);
    for (int w : { FAM_SCALED, FAM_ATMO }) runList(cb, ss, gWorld[w], gEWPipe[w], false, 4);   // no sun disc: direct light already carries it
    if (!gWorld[FAM_SCALED].empty() || !gWorld[FAM_ATMO].empty()) {
        VkClearAttachment ca{}; ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; VkClearRect r{ sc, 0, 1 };
        vkCmdClearAttachments(cb, 1, &ca, 1, &r);
    }
    runList(cb, ss, gI.list[0], gEPipe[0][0], false, 3); runList(cb, ss, gI.list[1], gEPipe[0][1], false, 3);
    runList(cb, ss, gI.list[2], gEWPipe[FAM_PQS], false, 3); runList(cb, ss, gI.list[3], gEWPipe[FAM_OCEAN], false, 3);
    vkCmdEndRendering(cb);
    gI.face = (f + 1) % 6;
    if (f != 5) return;

    memBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gI.lay, 0, 1, &gI.ds, 0, nullptr);
    struct { int a[4]; float b[4]; } pc{};
    auto push = [&] { vkCmdPushConstants(cb, gI.lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc); };
    auto groups = [](int m) { return (uint32_t)((std::max(1, IBL_RES >> m) + 7) / 8); };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gI.pDown);
    for (int m = 0; m < IBL_SRC_MIPS; m++) {
        pc.a[0] = m; push(); vkCmdDispatch(cb, groups(m), groups(m), 6);
        memBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gI.pPref);
    for (int m = 0; m < IBL_MIPS; m++) {
        pc.a[0] = m; pc.b[0] = (float)m / (IBL_MIPS - 1); pc.b[1] = 4 * 3.14159265f / (6.f * IBL_RES * IBL_RES); push();
        vkCmdDispatch(cb, groups(m), groups(m), 6);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gI.pSh);
    pc.a[0] = 3; push(); vkCmdDispatch(cb, 1, 1, 1);   // SH from the 16x16 source mip
    memBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    gI.valid = true;
}

// After the IBL face, before the main pass: spot-light shadow layers, atmosphere LUTs / sky-view / aerial perspective, ocean spectrum -> FFT -> wave mips.
static void enhPreScene(VkCommandBuffer cb, SlotScene& ss) {
    const SceneIn& H = ss.h; const int si = (int)(&ss - gSS);
    if (gX.nSpot) {
        for (auto& a : gX.spList) for (auto& b : a) for (auto& v : b) v.clear();
        const int* matInfo = (const int*)ss.mats.data();
        for (const DrawIn& d : ss.draws) {
            if (d.pad0 != FAM_PART || !(d.flags & DF_CAST) || (d.flags & (DF_MASK | DF_IVA)) || !drawValid(d, H)) continue;
            const MeshRec& m = gMesh[d.mesh]; bool skin = (d.flags & DF_SKIN) != 0;
            if (skin && (!m.skin || d.bone < 0 || d.bone >= H.bone4Count)) continue;
            V3 c; float rad; drawBounds(d, m, skin, c, rad);
            int cut = (matInfo[d.mat * MATF + 44] & MF_CUTOFF) ? 1 : 0;
            for (int k = 0; k < gX.nSpot; k++) {
                V3 o = c - gX.spPos[k]; float z = dot(o, gX.spFwd[k]), t = gX.spTan[k];
                if (z + rad < 0.05f || z - rad > gX.spRange[k] || sqrtf(std::max(dot(o, o) - z * z, 0.f)) > z * t + rad * sqrtf(1 + t * t)) continue;
                gX.spList[k][skin][cut].push_back({ &d, &m, z - rad });
            }
        }
        imgBarrier(cb, gX.spot.img, VK_IMAGE_ASPECT_DEPTH_BIT, 1, NSPOT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        for (int k = 0; k < gX.nSpot; k++) {
            VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; da.imageView = gX.spotLayer[k]; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; da.clearValue.depthStencil.depth = 1;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = { { 0, 0 }, { SPOT_RES, SPOT_RES } }; ri.layerCount = 1; ri.pDepthAttachment = &da;
            vkCmdBeginRendering(cb, &ri);
            VkViewport vp{ 0, 0, (float)SPOT_RES, (float)SPOT_RES, 0, 1 }; VkRect2D sc{ { 0, 0 }, { SPOT_RES, SPOT_RES } };
            vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
            for (int s = 0; s < 2; s++) for (int c = 0; c < 2; c++) {
                auto& list = gX.spList[k][s][c];
                if (list.empty()) continue;
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gX.sh[s][c]);
                vkCmdSetDepthBias(cb, 1.0f, 0.0f, 1.5f); vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
                VkDeviceSize zero = 0; if (!s) vkCmdBindVertexBuffers(cb, 0, 1, &gVB.b, &zero);
                for (const DrawRef& r : list) {
                    const DrawIn& d = *r.d;
                    float pc[16]; memcpy(pc, d.r, 48); int* ids = (int*)(pc + 12); ids[0] = d.mat; ids[1] = d.inst; ids[2] = 4 + k; ids[3] = d.bone;
                    vkCmdPushConstants(cb, gPLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
                    drawMesh(cb, d, *r.m, s);
                }
            }
            vkCmdEndRendering(cb);
        }
        imgBarrier(cb, gX.spot.img, VK_IMAGE_ASPECT_DEPTH_BIT, 1, NSPOT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    bool ocean = (gEF & EF_FFT) && !gWorld[FAM_OCEAN].empty();
    if (!gX.atm && !ocean) return;
    memBarrier(cb, PS_ALL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);   // earlier frames are done sampling the LUTs / waves
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gX.lay, 0, 1, &gX.ds[si], 0, nullptr);
    struct { int a[4]; float b[4], c[4]; } pc{};
    auto push = [&] { vkCmdPushConstants(cb, gX.lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc); };
    auto run = [&](VkPipeline p, uint32_t x, uint32_t y) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p); push(); vkCmdDispatch(cb, x, y, 1); };
    auto cbar = [&] { memBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); };
    if (gX.atm) {
        if (gX.atmDone != gX.atmWant) { run(gX.pTrans, 32, 8); cbar(); run(gX.pMulti, 32, 32); cbar(); gX.atmDone = gX.atmWant; }
        if (gX.inside) run(gX.pSkyv, 24, 14);
        run(gX.pAp, 4, 4);
    }
    if (ocean) {
        run(gX.pSpec, FFT_N / 8, FFT_N / 8); cbar();
        pc.a[0] = 0; pc.a[1] = 0; pc.a[2] = 1; run(gX.pFft, FFT_N, 1); cbar();
        pc.a[0] = 1; pc.a[1] = 1; pc.a[2] = 0; run(gX.pFft, FFT_N, 1); cbar();
        run(gX.pNrm, FFT_N / 8, FFT_N / 8); cbar();
        for (int m = 1; m < WAV_MIPS; m++) { pc.a[0] = m; uint32_t g = (uint32_t)std::max(1, (FFT_N >> m) / 8); run(gX.pMip, g, g); cbar(); }
    }
    memBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// Inside the main pass, before the ocean: end it, copy the linear depth so far (terrain, parts) for the water's depth, resume with LOAD.
static void enhOceanSplit(VkCommandBuffer cb, SlotScene& ss) {
    if (!(gEF & EF_OCEAN) || !gP.pZc || !gMainRI) return;
    const int si = (int)(&ss - gSS);
    vkCmdEndRendering(cb);
    imgBarrier(cb, gMainDepth, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.lay, 0, 1, &gP.ds[si], 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pZc);
    vkCmdDispatch(cb, (uint32_t)(gP.w + 7) / 8, (uint32_t)(gP.h + 7) / 8, 1);
    imgBarrier(cb, gMainDepth, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    memBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    auto* a = const_cast<VkRenderingAttachmentInfo*>(gMainRI->pColorAttachments);
    for (uint32_t i = 0; i < gMainRI->colorAttachmentCount; i++) a[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    if (gMainRI->pDepthAttachment) const_cast<VkRenderingAttachmentInfo*>(gMainRI->pDepthAttachment)->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    vkCmdBeginRendering(cb, gMainRI);
    VkViewport vp{ 0, 0, (float)gP.w, (float)gP.h, 0, 1 }; VkRect2D sc{ { 0, 0 }, { (uint32_t)gP.w, (uint32_t)gP.h } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
}

// Before the main pass: earlier frames' post passes are done with the scene targets, which start undefined.
static void enhPreMain(VkCommandBuffer cb) {
    memBarrier(cb, PS_ALL, PS_ALL);
    for (VImg* i : { &gP.hdr, &gP.nrm, &gP.rm, &gP.mv, &gP.alb })
        imgBarrier(cb, i->img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}
// Main-pass attachments 0 and 2..5 for Enhanced; a disabled feature gets no image, so its writes are discarded.
static void enhAttachments(VkRenderingAttachmentInfo* att) {
    att[0].imageView = gP.hdr.view;
    VkImageView v[4] = { gEF & EF_GBUF ? gP.nrm.view : VK_NULL_HANDLE, gEF & EF_GBUF ? gP.rm.view : VK_NULL_HANDLE, gEF & EF_TAA ? gP.mv.view : VK_NULL_HANDLE,
                         gEF & EF_GBUF ? gP.alb.view : VK_NULL_HANDLE };
    for (int i = 0; i < 4; i++) {
        VkRenderingAttachmentInfo& a = att[2 + i];
        a = VkRenderingAttachmentInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; a.imageView = v[i]; a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
}

// After the main pass: Hi-Z, AO / contact shadows / GI, TAA, exposure, bloom, tonemap into the shared colour (left in COLOR_ATTACHMENT_OPTIMAL;
// the shared depth is left GENERAL). Timestamps q0 + 0..5 after each stage.
static void enhPost(VkCommandBuffer cb, int si, ImageSet& set, uint32_t q0) {
    const int f = gEF; const EnhIn& e = gP.e;
    for (VImg* i : { &gP.hdr, &gP.nrm, &gP.rm, &gP.mv, &gP.alb })
        imgBarrier(cb, i->img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    imgBarrier(cb, gMainDepth, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.lay, 0, 1, &gP.ds[si], 0, nullptr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gP.lay, 0, 1, &gP.ds[si], 0, nullptr);
    struct { int a[4]; float b[4], c[4]; } pc{};
    auto push = [&] { vkCmdPushConstants(cb, gP.lay, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc); };
    auto groups = [](int n, int g) { return (uint32_t)((n + g - 1) / g); };
    auto cbar = [&] { memBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT); };
    const int w = gP.w, h = gP.h, ow = gP.ow, oh = gP.oh, bw = std::max(1, w / 2), bh = std::max(1, h / 2);
    auto ts = [&](int i) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gQp, q0 + i); };

    if (f & EF_HIZ) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pHiz);
        for (int i = 0; i < gP.nHiz; i++) {
            pc.a[0] = i; pc.a[1] = i - 1; push();
            vkCmdDispatch(cb, groups(std::max(1, bw >> i), 8), groups(std::max(1, bh >> i), 8), 1);
            cbar();
        }
    }
    ts(0);
    if (f & (EF_GTAO | EF_CONTACT | EF_SSGI | EF_SSR)) {   // half-res AO + contact / GI / reflections, temporal + bilateral denoise (AO, GI), depth-aware resolve into the hdr target
        const int hw = (w + 1) / 2, hh = (h + 1) / 2, ao = (f & EF_GTAO ? 1 : 0) | (f & EF_CONTACT ? 2 : 0), gi = (f & EF_SSGI) != 0, sr = (f & EF_SSR) != 0;
        const int prev = gP.aoCur, cur = prev ^ 1, reset = !gP.havePrev || ((f ^ gP.lastFlags) & (EF_GTAO | EF_CONTACT | EF_SSGI)) != 0;
        if (ao) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pGtao); pc = {}; pc.a[0] = ao; pc.b[0] = 1.5f; pc.b[1] = 0.6f; push(); vkCmdDispatch(cb, groups(hw, 8), groups(hh, 8), 1); }
        if (gi) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pSsgi); pc = {}; pc.b[0] = 6; push(); vkCmdDispatch(cb, groups(hw, 8), groups(hh, 8), 1); }
        if (sr) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pSsr); pc = {}; push(); vkCmdDispatch(cb, groups(hw, 8), groups(hh, 8), 1); }
        cbar();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pDen);
        for (int k = 0; k < 2; k++) {
            if (k ? !gi : !ao) continue;
            pc = {}; pc.a[0] = k; pc.a[1] = (k ? 14 : 12) + prev; pc.a[2] = (k ? 12 : 10) + cur; pc.a[3] = reset; pc.b[0] = k ? 0.08f : 0.1f; pc.b[1] = (f & EF_TAA) ? 1.f : 0.f; push();
            vkCmdDispatch(cb, groups(hw, 8), groups(hh, 8), 1);
        }
        cbar();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pRes);
        pc = {}; pc.a[0] = ao | (gi ? 4 : 0) | (sr ? 8 : 0); pc.a[1] = 12 + cur; pc.a[2] = 14 + cur; pc.b[0] = e.aoStrength; pc.b[1] = e.giStrength; push();
        vkCmdDispatch(cb, groups(w, 8), groups(h, 8), 1);
        cbar();
        gP.aoCur = cur;
    }
    ts(1);
    int src = 0;
    if (f & EF_TAA) {
        int prev = gP.histCur, cur = prev ^ 1;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pTaa);
        pc = {}; pc.a[0] = 1 + prev; pc.a[1] = cur; pc.a[2] = !gP.havePrev; pc.b[0] = e.taaWeight > 0 ? e.taaWeight : 0.1f; pc.b[1] = gP.jit[0]; pc.b[2] = gP.jit[1]; push();
        vkCmdDispatch(cb, groups(ow, 8), groups(oh, 8), 1);
        cbar();
        gP.histCur = cur; src = 1 + cur;
    }
    ts(2);
    const int sw0 = src ? ow : w, sh0 = src ? oh : h;   // exposure / bloom source size
    auto now = std::chrono::steady_clock::now();
    float dt = gP.havePrev ? std::min(0.25f, std::chrono::duration<float>(now - gP.last).count()) : 1e3f;
    gP.last = now;
    float fixedExpo = (f & EF_AUTOEXP) ? 0 : exp2f(e.ev);
    if (f & EF_AUTOEXP) {
        pc = {}; pc.a[0] = src; push();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pHist);
        vkCmdDispatch(cb, groups(std::max(1, sw0 / 2), 16), groups(std::max(1, sh0 / 2), 16), 1);
        cbar();
        pc.b[0] = e.key > 0 ? e.key : 0.18f; pc.b[1] = e.evRange; pc.b[2] = 1 - expf(-dt * 2); pc.b[3] = e.ev; push();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pExpo);
        vkCmdDispatch(cb, 1, 1, 1);
        cbar();
    }
    ts(3);
    if (f & EF_BLOOM) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pDown);
        for (int i = 0; i < NBLOOM; i++) {
            int sw = i ? std::max(1, bw >> (i - 1)) : sw0, sh = i ? std::max(1, bh >> (i - 1)) : sh0;
            pc = {}; pc.a[0] = 2 + i; pc.a[1] = i; pc.a[2] = src; pc.b[0] = 1.f / sw; pc.b[1] = 1.f / sh; pc.b[2] = e.threshold; pc.b[3] = 0.5f; pc.c[3] = fixedExpo; push();
            vkCmdDispatch(cb, groups(std::max(1, bw >> i), 8), groups(std::max(1, bh >> i), 8), 1);
            cbar();
        }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gP.pUp);
        for (int i = NBLOOM - 1; i >= 1; i--) {
            pc = {}; pc.a[0] = 2 + i - 1; pc.a[1] = i; pc.b[0] = 1.f / std::max(1, bw >> i); pc.b[1] = 1.f / std::max(1, bh >> i); push();
            vkCmdDispatch(cb, groups(std::max(1, bw >> (i - 1)), 8), groups(std::max(1, bh >> (i - 1)), 8), 1);
            cbar();
        }
    }
    ts(4);
    const bool full = !gUp || (f & EF_TAA); const int tw = full ? ow : w, th = full ? oh : h;   // render scale without TAA: tonemap at scene size, upscale.inl takes it from there
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView = full ? set.color[si].view : gU.col.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = { { 0, 0 }, { (uint32_t)tw, (uint32_t)th } }; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp{ 0, 0, (float)tw, (float)th, 0, 1 }; VkRect2D sc{ { 0, 0 }, { (uint32_t)tw, (uint32_t)th } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gP.pTone);
    pc = {}; pc.a[0] = src; pc.a[1] = std::min(3, std::max(0, e.tonemap)); pc.a[2] = (f & EF_BLOOM) != 0; pc.b[0] = e.bloom; pc.c[3] = fixedExpo; push();
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    ts(5);
    gP.havePrev = true; gP.lastFlags = f; gP.frame++;
}

static void enhTimes(const uint64_t* t) {   // t: timestamps after prep, IBL, pre-scene, main pass, Hi-Z, AO / GI, TAA, exposure, bloom, tonemap
    for (int i = 0; i < 9; i++) { float ms = (float)((t[i + 1] - t[i]) * gTsPeriod / 1e6); gPassMs[i] += (ms - gPassMs[i]) * 0.1f; }
}

static void enhShutdown() {
    if (gP.builder.joinable()) gP.builder.join();
    if (!gVd) return;
    freePostImgs();
    for (Buf& b : gP.prev) freeBuf(b);
    freeBuf(gP.expo); freeBuf(gI.sh);
    for (VkPipeline* p : { &gI.pDown, &gI.pPref, &gI.pSh }) if (*p) { vkDestroyPipeline(gVd, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (gI.lay) vkDestroyPipelineLayout(gVd, gI.lay, nullptr);
    if (gI.pool) vkDestroyDescriptorPool(gVd, gI.pool, nullptr);
    if (gI.dsl) vkDestroyDescriptorSetLayout(gVd, gI.dsl, nullptr);
    if (gI.smp) vkDestroySampler(gVd, gI.smp, nullptr);
    gI.lay = VK_NULL_HANDLE; gI.pool = VK_NULL_HANDLE; gI.dsl = VK_NULL_HANDLE; gI.smp = VK_NULL_HANDLE;
    for (auto& a : gEPipe) for (VkPipeline& p : a) if (p) { vkDestroyPipeline(gVd, p, nullptr); p = VK_NULL_HANDLE; }
    for (VkPipeline* p : { &gEMaskPipe, &gP.pHiz, &gP.pTaa, &gP.pHist, &gP.pExpo, &gP.pDown, &gP.pUp, &gP.pTone, &gP.pGtao, &gP.pSsgi, &gP.pDen, &gP.pRes, &gP.pZc, &gP.pSsr,
                           &gX.pTrans, &gX.pMulti, &gX.pSkyv, &gX.pAp, &gX.pSpec, &gX.pFft, &gX.pNrm, &gX.pMip, &gX.sh[0][0], &gX.sh[0][1], &gX.sh[1][0], &gX.sh[1][1] })
        if (*p) { vkDestroyPipeline(gVd, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (gX.lay) vkDestroyPipelineLayout(gVd, gX.lay, nullptr);
    if (gX.pool) vkDestroyDescriptorPool(gVd, gX.pool, nullptr);
    if (gX.dsl) vkDestroyDescriptorSetLayout(gVd, gX.dsl, nullptr);
    if (gX.rep) vkDestroySampler(gVd, gX.rep, nullptr);
    gX.lay = VK_NULL_HANDLE; gX.pool = VK_NULL_HANDLE; gX.dsl = VK_NULL_HANDLE; gX.rep = VK_NULL_HANDLE;
    for (VkPipeline& p : gEWPipe) if (p) { vkDestroyPipeline(gVd, p, nullptr); p = VK_NULL_HANDLE; }
    if (gP.lay) vkDestroyPipelineLayout(gVd, gP.lay, nullptr);
    if (gP.pool) vkDestroyDescriptorPool(gVd, gP.pool, nullptr);
    if (gP.dsl) vkDestroyDescriptorSetLayout(gVd, gP.dsl, nullptr);
    for (VkSampler s : { gP.lin, gP.nearest }) if (s) vkDestroySampler(gVd, s, nullptr);
}
