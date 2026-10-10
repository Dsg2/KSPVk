// KSPVk scene renderer: vessel parts (Phase 2) and the world (Phase 3: PQS terrain + ocean, scaled bodies, sun, atmosphere, galaxy). Included by core.cpp.
// Main thread: mesh / texture registration (D3D11 staging readback, converted in Pump), per-frame SceneIn copy in BeginFrame2.
// Render thread: uploads, descriptor catch-up, 4-cascade sun shadows, main pass into the shared colour + linear depth images.
#include <algorithm>
#include <map>
#include "part_vert.h"
#include "part_skin_vert.h"
#include "part_frag.h"
#include "shadow_vert.h"
#include "shadow_skin_vert.h"
#include "shadow_frag.h"
#include "world_vert.h"
#include "pqs_frag.h"
#include "ocean_frag.h"
#include "scaled_frag.h"
#include "sun_frag.h"
#include "atmo_frag.h"
#include "galaxy_frag.h"
#include "part_enh_vert.h"
#include "part_skin_enh_vert.h"
#include "world_enh_vert.h"
#include "part_enh_frag.h"
#include "pqs_enh_frag.h"
#include "ocean_enh_frag.h"
#include "scaled_enh_frag.h"
#include "sun_enh_frag.h"
#include "atmo_enh_frag.h"
#include "galaxy_enh_frag.h"

enum { MAXMESH = 16384, MAXTEX = 4096, MAXMAT = 8192, MAXINST = 16384, MAXDRAW = 32768, MAXBONE4 = 65536, MAXLIGHT = 8, NCASC = 4, SHADOW_RES = 2048, MAXSUB = 16 };
enum { RS_NONE, RS_PENDING, RS_READY, RS_FAILED };
enum { DF_CAST = 1, DF_SKIN = 2, DF_IVA = 4, DF_MASK = 8 };   // IVA: drawn after a depth clear like stock's InternalCamera; MASK: IVA DepthMask, depth only
enum { MF_CUTOFF = 64, MATF = 96, INSTF = 24 };
enum { TEX_WHITE, TEX_FLAT, TEX_BLACK, TEX_CLEAR, TEX_FIRST };
enum { FAM_PART, FAM_PQS, FAM_OCEAN, FAM_SCALED, FAM_SUN, FAM_ATMO, FAM_GALAXY, NFAM };   // DrawIn.pad0; DrawIn.pad1 = view (0 local, 1 scaled, 2 galaxy)
enum { MAXSHARED = 64 };
enum { DBG_FLIP_WINDING = 1, DBG_NO_SHADOWS = 2, DBG_NO_CULL = 4 };
static const VkDeviceSize VB_ARENA = 192ull << 20, IB_ARENA = 96ull << 20, SK_ARENA = 16ull << 20, STAGE_RING = 16ull << 20;
static const uint32_t VSTRIDE = 64, SSTRIDE = 20;   // packed vertex: pos3 nrm3 tan4 uv0 uv1 rgba16f | skin: weights4 joints u8x4

#pragma pack(push, 4)
struct MeshAttrIn { int attr, format, dim, stream, offset; };
struct MeshIn {
    void* vb[4]; void* ib; int stride[4]; int streams, ib32, vertexCount, attrCount; MeshAttrIn attrs[16];
    int subCount, subStart[MAXSUB], subLen[MAXSUB], subBase[MAXSUB]; float center[3], extents[3];
};
struct TexIn { void* tex; int wrapU, wrapV, filter, aniso; };
struct DrawIn { float r[12]; int mesh, sub, mat, inst, flags, bone, pad0, pad1; };   // skinned: r[0..3] = world bounding sphere
struct LightIn { float pos[4], col[4], dir[4]; };
struct ViewIn { float view[16], proj[6], nearClip, farClip, camPos[4]; };            // extra views: 0 scaled space, 1 galaxy
struct MeshCPU { const float *pos, *nrm, *tan, *col, *uv0, *uv1; const int* ib; int vertexCount, indexCount; float center[3], extents[3]; };
struct SceneIn {
    const DrawIn* draws; const float* insts; const float* mats; const float* bones;   // insts 16 floats, mats 48 floats, bones vec4
    int drawCount, instCount, matCount, bone4Count, lightCount, flags;
    float view[16], proj[6], nearClip, shadowDist;    // view: worldToCamera, row-major; proj: Unity m00 m01 m02 m10 m11 m12
    float camPos[4], lightDir[4], lightCol[4], ambient[4], shadow[4];                  // lightDir towards the sun; shadow: strength, normal bias (texels), enabled
    LightIn lights[MAXLIGHT];
    ViewIn views[2]; float misc[4];                                                     // misc: local far clip, time, -, -
    float ambEq[4], ambGround[4], env[4]; int envTex, envPad[3];                        // env: ambient mode (1 trilight), reflection intensity, lightmap RGBM scale, -
};
struct SceneStats { int draws, visible, shadowDraws, meshes, meshReady, textures, texReady, pending, arenaFull; float recordMs; uint64_t uploadedBytes, texBytes; };
#pragma pack(pop)
static_assert(sizeof(DrawIn) == 80 && sizeof(MeshIn) == 612 && sizeof(SceneIn) == 920 && sizeof(MeshCPU) == 88, "C# mirrors these layouts");

struct FrameU {
    float viewProj[16], camPos[4], lightDir[4], lightCol[4], ambient[4], shadowVP[NCASC][16], splits[4], shadowP[4], viewZ[4], cascTexel[4], cascBias[4];
    int nLights[4]; LightIn lights[MAXLIGHT];
    float vp[6][16], camPosV[3][4], viewZV[6][4], misc[4];   // views 3..5: the Enhanced IBL cube face
    float ambEq[4], ambGround[4], env[4];
    float pvp[3][16], jitter[4], ibl[4];   // Enhanced only (jitter.zw: 1 / target size)
    int enh[4]; float camR[4], camU[4], camF[4], proj[4], spotVP[4][16]; int spotIdx[8];   // Enhanced 8c-8e, mirrors the #ifdef ENH part of frame.glsl
    float atmC[4], atmP[4], atmB[4], atmX[4], atmS[4], occ[4][4], sfx[4], rs[4];
};

struct Buf { VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; uint8_t* p = nullptr; VkDeviceSize size = 0; };
struct VImg { VkImage img = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
struct MeshRec { MeshIn in{}; ID3D11Buffer* stg[5]{}; uint32_t vbOff = 0, ibOff = 0, skOff = 0, nv = 0, ni = 0, nsk = 0; bool skin = false, shared = false, alloc = false; float c[3]{}, rad = 0; };
struct TexRec { TexIn in{}; ID3D11Texture2D* stg = nullptr; D3D11_TEXTURE2D_DESC d{}; VkFormat fmt{}; VkComponentMapping sw{}; int blk = 0, bpe = 0; bool cube = false; VImg im; uint64_t bytes = 0; uint32_t skip = 0, mips = 0; };
struct Upload { int kind, id; uint32_t gen, vbOff, ibOff, skOff; std::vector<uint8_t> data; VkDeviceSize nv, ni, nsk; std::vector<VkBufferImageCopy> reg; };   // kind 0 mesh, 1 texture, 2 shared indices
struct Arena {   // first-fit free list, in elements
    std::map<uint32_t, uint32_t> fr; uint32_t cap = 0;
    void init(uint32_t n) { fr.clear(); fr[0] = cap = n; }
    int64_t alloc(uint32_t n) {
        if (!n) return 0;
        for (auto it = fr.begin(); it != fr.end(); ++it) if (it->second >= n) { uint32_t o = it->first, l = it->second; fr.erase(it); if (l > n) fr[o + n] = l - n; return o; }
        return -1;
    }
    void release(uint32_t o, uint32_t n) {
        if (!n) return;
        auto it = fr.emplace(o, n).first, nx = std::next(it);
        if (nx != fr.end() && o + n == nx->first) { it->second += nx->second; fr.erase(nx); }
        if (it != fr.begin()) { auto pv = std::prev(it); if (pv->first + pv->second == it->first) { pv->second += it->second; fr.erase(it); } }
    }
};
struct SlotScene {
    std::vector<DrawIn> draws; std::vector<float> insts, mats, bones; SceneIn h{};
    Buf ubo, mat, inst, bone, stage; VkDescriptorSet ds = VK_NULL_HANDLE; size_t texApplied = 0; int envApplied = -2; std::vector<Buf> garbage;
};

static bool gSceneOK;
static VkPhysicalDeviceMemoryProperties gMemProps;
static MeshRec* gMesh; static TexRec* gTex;
static volatile int8_t gMeshState[MAXMESH], gTexState[MAXTEX];
static int gMeshN, gTexN = TEX_FIRST;
static std::vector<int> gPendMesh, gPendTex;                  // main thread
static Arena gVBA, gIBA, gSKA;                               // main thread
static volatile uint32_t gMeshGen[MAXMESH];                   // bumped when an id is recycled; stale uploads are skipped
static std::vector<int> gFreeIds; static std::deque<std::pair<int, uint64_t>> gDead;
struct Shared { uint32_t off, n; }; static Shared gShared[MAXSHARED]; static int gSharedN; static std::vector<Shared> gSharedOld;
static bool gArenaFull;
static int gTexMax;   // largest side kept in Vulkan copies (0: full size); top mips above it are skipped
static std::mutex gUpM; static std::deque<Upload> gUploads; static std::atomic<uint64_t> gUploadedBytes{0};
static SlotScene gSS[K];
static std::vector<int> gTexReady;                            // render thread: ids in the order they became ready
static std::map<int, VkSampler> gSmp;                         // render thread
static Buf gVB, gIB, gSK;
static VkDescriptorSetLayout gDSL; static VkDescriptorPool gDPool; static VkPipelineLayout gPLay;
static VkPipeline gPipe[2][2], gShPipe[2][2], gWPipe[NFAM], gMaskPipe;   // [skin][cutoff]; world families
static VkSampler gShadowSmp;
static VImg gShadow, gDefCube; static VkImageView gShadowLayer[NCASC];
static VImg gZ; static int gZw, gZh;
static std::atomic<int> gSceneDebug{0};
static SceneStats gSStats;
static int gEF;   // render thread: Enhanced features active for the frame being recorded (0: stock path)
static VkRenderingInfo* gMainRI; static VkImage gMainDepth;   // the main pass while it is recorded (Enhanced ocean: split around a depth copy)
static VkSampler gShadowRawSmp;
static void enhPrepare(FrameU& F, SlotScene& ss);
static void enhOceanSplit(VkCommandBuffer cb, SlotScene& ss);

// ---------------- Vulkan helpers ----------------
static int memType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < gMemProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (gMemProps.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
static bool allocMem(const VkMemoryRequirements& mr, VkMemoryPropertyFlags want, VkDeviceMemory& m) {
    int t = memType(mr.memoryTypeBits, want);
    if (t < 0) { logf("no memory type with flags %x", want); return false; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)t;
    VKCHECK(vkAllocateMemory(gVd, &ai, nullptr, &m));
    return true;
}
static bool makeBuf(Buf& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = usage;
    VKCHECK(vkCreateBuffer(gVd, &ci, nullptr, &b.b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(gVd, b.b, &mr);
    if (!allocMem(mr, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b.m)) return false;
    VKCHECK(vkBindBufferMemory(gVd, b.b, b.m, 0));
    if (host) VKCHECK(vkMapMemory(gVd, b.m, 0, VK_WHOLE_SIZE, 0, (void**)&b.p));
    b.size = size;
    return true;
}
static void freeBuf(Buf& b) { if (b.b) vkDestroyBuffer(gVd, b.b, nullptr); if (b.m) vkFreeMemory(gVd, b.m, nullptr); b = Buf(); }
static bool makeVImg(VImg& im, VkFormat f, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                     VkComponentMapping sw = {}, bool cube = false, uint32_t depth = 1) {   // depth > 1: 3D
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0; ci.imageType = depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = f; ci.extent = { w, h, depth };
    ci.mipLevels = mips; ci.arrayLayers = layers; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    VKCHECK(vkCreateImage(gVd, &ci, nullptr, &im.img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(gVd, im.img, &mr);
    if (!allocMem(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, im.m)) return false;
    VKCHECK(vkBindImageMemory(gVd, im.img, im.m, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = im.img; vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f; vi.components = sw; vi.subresourceRange = { aspect, 0, mips, 0, layers };
    VKCHECK(vkCreateImageView(gVd, &vi, nullptr, &im.view));
    return true;
}
static void freeVImg(VImg& i) {
    if (i.view) vkDestroyImageView(gVd, i.view, nullptr);
    if (i.img) vkDestroyImage(gVd, i.img, nullptr);
    if (i.m) vkFreeMemory(gVd, i.m, nullptr);
    i = VImg();
}
static void imgBarrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, uint32_t mips, uint32_t layers, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkAccessFlags sa, VkAccessFlags da) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = img; b.subresourceRange = { aspect, 0, mips, 0, layers };
    b.oldLayout = from; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.srcAccessMask = sa; b.dstAccessMask = da;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}
template <class F> static bool oneShot(F f) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = gPool; ai.commandBufferCount = 1;
    VkCommandBuffer cb; VKCHECK(vkAllocateCommandBuffers(gVd, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi); f(cb); vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VKCHECK(vkQueueSubmit(gQ, 1, &si, VK_NULL_HANDLE));
    vkQueueWaitIdle(gQ); vkFreeCommandBuffers(gVd, gPool, 1, &cb);
    return true;
}
static VkSampler sampler(int wu, int wv, int filter, int aniso) {
    aniso = std::max(1, std::min(aniso, 16));
    int key = (wu & 3) | (wv & 3) << 2 | (filter & 3) << 4 | aniso << 6;
    auto it = gSmp.find(key); if (it != gSmp.end()) return it->second;
    static const VkSamplerAddressMode am[4] = { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT };
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = ci.minFilter = filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = filter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;   // Unity: Bilinear = point mips
    ci.addressModeU = am[wu & 3]; ci.addressModeV = am[wv & 3]; ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; ci.maxLod = VK_LOD_CLAMP_NONE;
    ci.anisotropyEnable = gAniso && aniso > 1; ci.maxAnisotropy = std::min((float)aniso, gMaxAniso);
    VkSampler s = VK_NULL_HANDLE; vkCreateSampler(gVd, &ci, nullptr, &s);
    return gSmp[key] = s;
}

// ---------------- format conversion ----------------
static bool mapFormat(DXGI_FORMAT f, VkFormat& vf, int& blk, int& bpe, VkComponentMapping& sw) {
    sw = {}; blk = 0;
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: vf = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; blk = 1; bpe = 8; return true;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: vf = VK_FORMAT_BC2_UNORM_BLOCK; blk = 1; bpe = 16; return true;
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: vf = VK_FORMAT_BC3_UNORM_BLOCK; blk = 1; bpe = 16; return true;
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: vf = VK_FORMAT_BC4_UNORM_BLOCK; blk = 1; bpe = 8; return true;
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: vf = VK_FORMAT_BC5_UNORM_BLOCK; blk = 1; bpe = 16; return true;
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: vf = VK_FORMAT_BC7_UNORM_BLOCK; blk = 1; bpe = 16; return true;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: vf = VK_FORMAT_R8G8B8A8_UNORM; bpe = 4; return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: vf = VK_FORMAT_B8G8R8A8_UNORM; bpe = 4; return true;
    case DXGI_FORMAT_B8G8R8X8_UNORM: vf = VK_FORMAT_B8G8R8A8_UNORM; bpe = 4; sw.a = VK_COMPONENT_SWIZZLE_ONE; return true;
    case DXGI_FORMAT_A8_UNORM: vf = VK_FORMAT_R8_UNORM; bpe = 1; sw = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R }; return true;
    case DXGI_FORMAT_R8_UNORM: vf = VK_FORMAT_R8_UNORM; bpe = 1; return true;
    case DXGI_FORMAT_R8G8_UNORM: vf = VK_FORMAT_R8G8_UNORM; bpe = 2; return true;
    case DXGI_FORMAT_B5G6R5_UNORM: vf = VK_FORMAT_R5G6B5_UNORM_PACK16; bpe = 2; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: vf = VK_FORMAT_R16G16B16A16_SFLOAT; bpe = 8; return true;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: vf = VK_FORMAT_R32G32B32A32_SFLOAT; bpe = 16; return true;
    default: return false;
    }
}
static float h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 0x3ffu, u;
    if (!e) { float f = m / 16777216.0f; return s ? -f : f; }
    u = e == 31 ? s | 0x7f800000u | (m << 13) : s | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4); return f;
}
static int fmtSize(int f) { static const int s[12] = { 4, 2, 1, 1, 2, 2, 1, 1, 2, 2, 4, 4 }; return f >= 0 && f < 12 ? s[f] : 0; }
static void readAttr(const uint8_t* p, int f, int dim, float* o) {   // Unity VertexAttributeFormat -> float
    for (int i = 0; i < dim && i < 4; i++) switch (f) {
        case 0: memcpy(&o[i], p + 4 * i, 4); break;
        case 1: { uint16_t v; memcpy(&v, p + 2 * i, 2); o[i] = h2f(v); } break;
        case 2: o[i] = p[i] / 255.f; break;
        case 3: o[i] = std::max(-1.f, (int8_t)p[i] / 127.f); break;
        case 4: { uint16_t v; memcpy(&v, p + 2 * i, 2); o[i] = v / 65535.f; } break;
        case 5: { int16_t v; memcpy(&v, p + 2 * i, 2); o[i] = std::max(-1.f, v / 32767.f); } break;
        case 6: o[i] = p[i]; break;
        case 7: o[i] = (int8_t)p[i]; break;
        case 8: { uint16_t v; memcpy(&v, p + 2 * i, 2); o[i] = v; } break;
        case 9: { int16_t v; memcpy(&v, p + 2 * i, 2); o[i] = v; } break;
        case 10: { uint32_t v; memcpy(&v, p + 4 * i, 4); o[i] = (float)v; } break;
        case 11: { int32_t v; memcpy(&v, p + 4 * i, 4); o[i] = (float)v; } break;
    }
}

// ---------------- init ----------------
static VkShaderModule shmod(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = bytes; ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(gVd, &ci, nullptr, &m); return m;
}
static bool makePipe(VkPipeline& out, VkShaderModule vs, VkShaderModule fs, bool shadow, bool skin, bool cutoff, int fam = FAM_PART, bool depthOnly = false, bool enh = false) {
    VkBool32 cut = cutoff;
    VkSpecializationMapEntry me{ 0, 0, sizeof(VkBool32) }; VkSpecializationInfo spec{ 1, &me, sizeof cut, &cut };
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main"; st[1].pSpecializationInfo = &spec;
    VkVertexInputBindingDescription vb[2] = { { 0, VSTRIDE, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, SSTRIDE, VK_VERTEX_INPUT_RATE_VERTEX } };
    VkVertexInputAttributeDescription all[8] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 }, { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 24 },
        { 3, 0, VK_FORMAT_R32G32_SFLOAT, 40 }, { 4, 0, VK_FORMAT_R16G16B16A16_SFLOAT, 56 }, { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, { 6, 1, VK_FORMAT_R8G8B8A8_UINT, 16 },
        { 7, 0, VK_FORMAT_R32G32_SFLOAT, 48 } };
    VkVertexInputAttributeDescription va[8]; uint32_t na = 0;
    bool world = fam != FAM_PART;
    for (auto& a : all)
        if (a.location == 7 ? !shadow : (a.location < 5 || skin) && (!shadow || a.location == 0 || a.location == 3 || a.location >= 5)) va[na++] = a;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = skin ? 2 : 1; vi.pVertexBindingDescriptions = vb; vi.vertexAttributeDescriptionCount = na; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.cullMode = shadow || fam == FAM_GALAXY ? VK_CULL_MODE_NONE : fam == FAM_ATMO ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    rs.depthClampEnable = shadow && gDepthClamp; rs.depthBiasEnable = shadow; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = ds.depthWriteEnable = fam != FAM_GALAXY;
    ds.depthCompareOp = shadow ? VK_COMPARE_OP_LESS : fam == FAM_ATMO || fam == FAM_PART ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_GREATER;   // parts: like Unity LEqual, so coplanar cutout layers (drawn after opaque) win
    // attachment 0: stock blend per family on rgb, alpha = max coverage; attachment 1 (linear depth) never blends
    // Enhanced: + normal, roughness / metal / light fractions, motion, albedo (none for the IVA mask; no surface attributes from the sky layers)
    VkPipelineColorBlendAttachmentState ba[6]{}; ba[0].colorWriteMask = depthOnly ? 0 : 0xf; ba[1].colorWriteMask = fam == FAM_GALAXY || depthOnly ? 0 : 0xf;
    ba[2].colorWriteMask = ba[3].colorWriteMask = ba[5].colorWriteMask = fam == FAM_GALAXY || fam == FAM_ATMO || depthOnly ? 0 : 0xf; ba[4].colorWriteMask = depthOnly ? 0 : 0xf;
    if (world) {
        VkBlendFactor sf = VK_BLEND_FACTOR_ONE, df = VK_BLEND_FACTOR_ZERO;
        if (fam == FAM_PQS) sf = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, df = VK_BLEND_FACTOR_SRC_ALPHA;
        else if (fam == FAM_OCEAN) sf = VK_BLEND_FACTOR_SRC_ALPHA, df = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        else if (fam == FAM_ATMO) df = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba[0].blendEnable = VK_TRUE; ba[0].srcColorBlendFactor = sf; ba[0].dstColorBlendFactor = df; ba[0].colorBlendOp = VK_BLEND_OP_ADD;
        ba[0].srcAlphaBlendFactor = ba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba[0].alphaBlendOp = VK_BLEND_OP_MAX;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = shadow ? 0 : enh ? 6 : 2; cb.pAttachments = ba;
    VkDynamicState dyn[4] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = shadow ? 4 : 3; dy.pDynamicStates = dyn;
    VkFormat cf[6] = { enh ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM };
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = shadow ? 0 : enh ? 6 : 2; ri.pColorAttachmentFormats = cf;
    ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pi.pNext = &ri;
    pi.stageCount = fs ? 2 : 1; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds; pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = gPLay;
    VKCHECK(vkCreateGraphicsPipelines(gVd, VK_NULL_HANDLE, 1, &pi, nullptr, &out));
    return true;
}

static void writeTexDescriptors(VkDescriptorSet ds, int first, int count, const VkDescriptorImageInfo* ii) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = ds; w.dstBinding = 4; w.dstArrayElement = (uint32_t)first;
    w.descriptorCount = (uint32_t)count; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = ii;
    vkUpdateDescriptorSets(gVd, 1, &w, 0, nullptr);
}
static void setEnv(SlotScene& ss, int id) {   // binding 6: the scene's reflection cube (id -1: black)
    VkDescriptorImageInfo ii{ sampler(1, 1, 2, 1), id >= 0 ? gTex[id].im.view : gDefCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = ss.ds; w.dstBinding = 6; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(gVd, 1, &w, 0, nullptr);
    ss.envApplied = id;
}
static void resetTexDescriptors(int from) {   // every unused bindless slot points at the white texture
    std::vector<VkDescriptorImageInfo> ii(MAXTEX - from, { gSmp.begin()->second, gTex[TEX_WHITE].im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
    for (SlotScene& ss : gSS) writeTexDescriptors(ss.ds, from, MAXTEX - from, ii.data());
}

static bool initScene(bool bcOk) {
    if (!bcOk) { logf("scene renderer disabled: missing BC textures / descriptor indexing support"); return false; }
    vkGetPhysicalDeviceMemoryProperties(gPd, &gMemProps);
    gMesh = new MeshRec[MAXMESH]; gTex = new TexRec[MAXTEX];
    const VkBufferUsageFlags dst = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!makeBuf(gVB, VB_ARENA, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | dst, false) || !makeBuf(gIB, IB_ARENA, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | dst, false) ||
        !makeBuf(gSK, SK_ARENA, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | dst, false)) return false;
    for (SlotScene& ss : gSS)
        if (!makeBuf(ss.ubo, sizeof(FrameU), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true) || !makeBuf(ss.mat, MAXMAT * MATF * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
            !makeBuf(ss.inst, MAXINST * INSTF * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) || !makeBuf(ss.bone, MAXBONE4 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
            !makeBuf(ss.stage, STAGE_RING, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;

    // descriptor layout: 0 frame UBO, 1 materials, 2 instances, 3 shadow map, 4 bindless textures (update-after-bind: Iris Xe allows 64 samplers otherwise), 5 bones, 6 reflection cube,
    // 7 previous-frame rows, 8 IBL cube, 9 IBL SH; 10 raw shadow depth (PCSS), 11 depth copy (ocean), 12 aerial perspective, 13 sky-view, 14 transmittance,
    // 15 spot shadows, 16 ocean waves, 17 multiple scattering (7.. Enhanced only, bound when it first runs)
    const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, FS = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorBindingFlags PB = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    enum { NB = 18 };
    VkDescriptorSetLayoutBinding b[NB] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VF, nullptr }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, FS, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, FS, nullptr }, { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FS, nullptr },
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAXTEX, FS, nullptr }, { 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
        { 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FS, nullptr }, { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
        { 8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FS, nullptr }, { 9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, FS, nullptr } };
    VkDescriptorBindingFlags bf[NB] = { 0, 0, 0, 0, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | PB, 0, 0, PB, PB, PB };
    for (int i = 10; i < NB; i++) { b[i] = { (uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FS, nullptr }; bf[i] = PB; }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO}; bfi.bindingCount = NB; bfi.pBindingFlags = bf;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dli.pNext = &bfi;
    dli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT; dli.bindingCount = NB; dli.pBindings = b;
    VKCHECK(vkCreateDescriptorSetLayout(gVd, &dli, nullptr, &gDSL));
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, K }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * K }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (MAXTEX + 3 + NB - 10) * K } };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dpi.maxSets = K; dpi.poolSizeCount = 3; dpi.pPoolSizes = ps;
    VKCHECK(vkCreateDescriptorPool(gVd, &dpi, nullptr, &gDPool));
    VkPushConstantRange pcr{ VF, 0, 64 };
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &gDSL; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKCHECK(vkCreatePipelineLayout(gVd, &pli, nullptr, &gPLay));

    // default textures, shadow map
    static const uint32_t px[4] = { 0xffffffffu, 0x80ff8080u, 0xff000000u, 0u };   // white, flat normal (0.5, 0.5, 1, 0.5 as DXT5nm .wy), black, clear
    Buf up; if (!makeBuf(up, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    memcpy(up.p, px, sizeof px);
    for (int i = 0; i < 6; i++) ((uint32_t*)up.p)[4 + i] = 0xff000000u;   // default reflection cube: black
    for (int i = 0; i < TEX_FIRST; i++)
        if (!makeVImg(gTex[i].im, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!makeVImg(gDefCube, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 6, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, {}, true)) return false;
    if (!makeVImg(gShadow, VK_FORMAT_D32_SFLOAT, SHADOW_RES, SHADOW_RES, 1, NCASC, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
    for (int c = 0; c < NCASC; c++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = gShadow.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_D32_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, (uint32_t)c, 1 };
        VKCHECK(vkCreateImageView(gVd, &vi, nullptr, &gShadowLayer[c]));
    }
    oneShot([&](VkCommandBuffer cb) {
        for (int i = 0; i < TEX_FIRST; i++) {
            imgBarrier(cb, gTex[i].im.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy r{}; r.bufferOffset = 4 * i; r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; r.imageExtent = { 1, 1, 1 };
            vkCmdCopyBufferToImage(cb, up.b, gTex[i].im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            imgBarrier(cb, gTex[i].im.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        imgBarrier(cb, gShadow.img, VK_IMAGE_ASPECT_DEPTH_BIT, 1, NCASC, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT);
        imgBarrier(cb, gDefCube.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r{}; r.bufferOffset = 16; r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6 }; r.imageExtent = { 1, 1, 1 };
        vkCmdCopyBufferToImage(cb, up.b, gDefCube.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        imgBarrier(cb, gDefCube.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    });
    freeBuf(up);
    for (int i = 0; i < TEX_FIRST; i++) gTexState[i] = RS_READY;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sci.compareEnable = VK_TRUE; sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VKCHECK(vkCreateSampler(gVd, &sci, nullptr, &gShadowSmp));
    sci.compareEnable = VK_FALSE; sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    VKCHECK(vkCreateSampler(gVd, &sci, nullptr, &gShadowRawSmp));
    sampler(0, 0, 1, 1);   // default sampler, also used for the white placeholder descriptors

    VkDescriptorSetLayout ls[K]; for (auto& l : ls) l = gDSL;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = gDPool; dai.descriptorSetCount = K; dai.pSetLayouts = ls;
    VkDescriptorSet sets[K]; VKCHECK(vkAllocateDescriptorSets(gVd, &dai, sets));
    for (int k = 0; k < K; k++) {
        SlotScene& ss = gSS[k]; ss.ds = sets[k];
        VkDescriptorBufferInfo bi[4] = { { ss.ubo.b, 0, VK_WHOLE_SIZE }, { ss.mat.b, 0, VK_WHOLE_SIZE }, { ss.inst.b, 0, VK_WHOLE_SIZE }, { ss.bone.b, 0, VK_WHOLE_SIZE } };
        VkDescriptorImageInfo si[2] = { { gShadowSmp, gShadow.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL }, { gShadowRawSmp, gShadow.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL } };
        VkWriteDescriptorSet w[6]{};
        const int bind[6] = { 0, 1, 2, 5, 3, 10 };
        for (int i = 0; i < 6; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = ss.ds; w[i].dstBinding = bind[i]; w[i].descriptorCount = 1;
            w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : i < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if (i < 4) w[i].pBufferInfo = &bi[i]; else w[i].pImageInfo = &si[i - 4];
        }
        vkUpdateDescriptorSets(gVd, 6, w, 0, nullptr);
        VkDescriptorImageInfo d[TEX_FIRST];
        for (int i = 0; i < TEX_FIRST; i++) d[i] = { gSmp.begin()->second, gTex[i].im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        writeTexDescriptors(ss.ds, 0, TEX_FIRST, d);
        setEnv(ss, -1);
    }
    resetTexDescriptors(TEX_FIRST);

    VkShaderModule pv = shmod(part_vert, sizeof part_vert), psv = shmod(part_skin_vert, sizeof part_skin_vert), pf = shmod(part_frag, sizeof part_frag),
                   sv = shmod(shadow_vert, sizeof shadow_vert), ssv = shmod(shadow_skin_vert, sizeof shadow_skin_vert), sf = shmod(shadow_frag, sizeof shadow_frag);
    bool ok = pv && psv && pf && sv && ssv && sf;
    for (int s = 0; s < 2 && ok; s++)
        for (int c = 0; c < 2 && ok; c++)
            ok = makePipe(gPipe[s][c], s ? psv : pv, pf, false, s, c) && makePipe(gShPipe[s][c], s ? ssv : sv, c ? sf : VK_NULL_HANDLE, true, s, c);
    ok = ok && makePipe(gMaskPipe, pv, pf, false, false, false, FAM_PART, true);
    VkShaderModule wv = shmod(world_vert, sizeof world_vert), wf[NFAM] = { VK_NULL_HANDLE, shmod(pqs_frag, sizeof pqs_frag), shmod(ocean_frag, sizeof ocean_frag),
        shmod(scaled_frag, sizeof scaled_frag), shmod(sun_frag, sizeof sun_frag), shmod(atmo_frag, sizeof atmo_frag), shmod(galaxy_frag, sizeof galaxy_frag) };
    for (int f = 1; f < NFAM && ok; f++) ok = wv && wf[f] && makePipe(gWPipe[f], wv, wf[f], false, false, false, f);
    for (VkShaderModule m : { pv, psv, pf, sv, ssv, sf, wv }) if (m) vkDestroyShaderModule(gVd, m, nullptr);
    for (VkShaderModule m : wf) if (m) vkDestroyShaderModule(gVd, m, nullptr);
    if (!ok) { logf("pipeline creation failed"); return false; }
    gVBA.init((uint32_t)(VB_ARENA / VSTRIDE)); gIBA.init((uint32_t)(IB_ARENA / 4)); gSKA.init((uint32_t)(SK_ARENA / SSTRIDE));
    logf("scene renderer ready (arenas VB %llu MB, IB %llu MB; depth clamp %d, aniso %d)", (unsigned long long)(VB_ARENA >> 20), (unsigned long long)(IB_ARENA >> 20), gDepthClamp, gAniso);
    return true;
}

// ---------------- registration (main thread) ----------------
static void releaseMeshStaging(MeshRec& m) { for (auto& b : m.stg) if (b) { b->Release(); b = nullptr; } }
static int newMeshId() {
    if (!gFreeIds.empty()) { int id = gFreeIds.back(); gFreeIds.pop_back(); return id; }
    return gMeshN < MAXMESH ? gMeshN++ : -1;
}
static uint16_t f2h(float f) {
    uint32_t x; memcpy(&x, &f, 4); uint32_t s = (x >> 16) & 0x8000; int e = (int)((x >> 23) & 0xff) - 112; uint32_t m = x & 0x7fffff;
    if (e <= 0) return (uint16_t)s;
    if (e >= 31) return (uint16_t)(s | 0x7c00);
    return (uint16_t)(s | (((uint32_t)e << 10) + ((m + 0x1000) >> 13)));
}
static void packVertex(uint8_t* o, const float* pos, const float* nrm, const float* tan, const float* uv0, const float* uv1, const float* col) {
    memcpy(o, pos, 12); memcpy(o + 12, nrm, 12); memcpy(o + 24, tan, 16); memcpy(o + 40, uv0, 8); memcpy(o + 48, uv1, 8);
    uint16_t h[4]; for (int c = 0; c < 4; c++) h[c] = f2h(col[c]);
    memcpy(o + 56, h, 8);
}
// Reserves arena ranges for a mesh; false (and arena-full flag) if any arena is exhausted.
static bool allocMesh(MeshRec& m, uint32_t nv, uint32_t ni, uint32_t nsk) {
    int64_t v = gVBA.alloc(nv), i = gIBA.alloc(ni), k = gSKA.alloc(nsk);
    if (v < 0 || i < 0 || k < 0) {
        if (v >= 0) gVBA.release((uint32_t)v, nv);
        if (i >= 0) gIBA.release((uint32_t)i, ni);
        if (k >= 0) gSKA.release((uint32_t)k, nsk);
        gArenaFull = true; return false;
    }
    m.vbOff = (uint32_t)v; m.ibOff = (uint32_t)i; m.skOff = (uint32_t)k; m.nv = nv; m.ni = ni; m.nsk = nsk; m.alloc = true;
    return true;
}
static Upload meshUpload(int id, const MeshRec& m) {
    Upload u; u.kind = 0; u.id = id; u.gen = gMeshGen[id]; u.vbOff = m.vbOff; u.ibOff = m.ibOff; u.skOff = m.skOff; u.nv = m.nv; u.ni = m.ni; u.nsk = m.nsk;
    u.data.resize((size_t)m.nv * VSTRIDE + (size_t)m.ni * 4 + (size_t)m.nsk * SSTRIDE);
    return u;
}

EXPORT int KSPVk_RegisterMesh(const MeshIn* in) {
    if (gState != ST_OK || !gSceneOK) return -1;
    int id = newMeshId();
    if (id < 0) return -1;
    MeshRec& m = gMesh[id]; m = MeshRec(); m.in = *in;
    gMeshState[id] = RS_FAILED;
    if (in->streams < 1 || in->streams > 4 || in->vertexCount <= 0 || in->subCount < 1 || in->subCount > MAXSUB || in->attrCount < 1 || in->attrCount > 16 || !in->ib) {
        logf("mesh %d: unsupported layout (streams %d, verts %d, subs %d, attrs %d)", id, in->streams, in->vertexCount, in->subCount, in->attrCount); return id;
    }
    for (int s = 0; s <= in->streams; s++) {
        ID3D11Buffer* src = (ID3D11Buffer*)(s < in->streams ? in->vb[s] : in->ib);
        if (!src) continue;   // a stream without attributes may have no buffer
        D3D11_BUFFER_DESC d; src->GetDesc(&d);
        if (s < in->streams && d.ByteWidth < (UINT)in->stride[s] * (UINT)in->vertexCount) {
            logf("mesh %d: stream %d is %u bytes, layout needs %d", id, s, d.ByteWidth, in->stride[s] * in->vertexCount); releaseMeshStaging(m); return id;
        }
        D3D11_BUFFER_DESC sd{}; sd.ByteWidth = d.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(gDev->CreateBuffer(&sd, nullptr, &m.stg[s]))) { logf("mesh %d: staging alloc failed", id); releaseMeshStaging(m); return id; }
        gCtx->CopyResource(m.stg[s], src);
    }
    for (int a = 0; a < in->attrCount; a++)
        if (!m.stg[in->attrs[a].stream]) { logf("mesh %d: attribute %d in stream %d without buffer", id, in->attrs[a].attr, in->attrs[a].stream); releaseMeshStaging(m); return id; }
    gMeshState[id] = RS_PENDING; gPendMesh.push_back(id);
    return id;
}

static void pushUpload(Upload&& u);
// Mesh from CPU arrays (PQS quads), converted immediately. ib == null: vertex-only, drawn with a shared index set (DrawIn.sub).
EXPORT int KSPVk_RegisterMeshCPU(const MeshCPU* in) {
    if (gState != ST_OK || !gSceneOK || !in->pos || in->vertexCount <= 0 || (in->ib && in->indexCount <= 0)) return -1;
    int id = newMeshId();
    if (id < 0) return -1;
    MeshRec& m = gMesh[id]; m = MeshRec();
    uint32_t nv = (uint32_t)in->vertexCount, ni = in->ib ? (uint32_t)in->indexCount : 0;
    if (!allocMesh(m, nv, ni, 0)) { gMeshState[id] = RS_FAILED; return id; }
    m.shared = !in->ib; m.in.vertexCount = in->vertexCount; m.in.subCount = 1; m.in.subLen[0] = (int)ni;
    memcpy(m.in.center, in->center, 12); memcpy(m.in.extents, in->extents, 12); memcpy(m.c, in->center, 12);
    m.rad = sqrtf(in->extents[0] * in->extents[0] + in->extents[1] * in->extents[1] + in->extents[2] * in->extents[2]);
    Upload u = meshUpload(id, m);
    static const float z4[4] = {}, n4[4] = { 0, 0, 1, 0 }, t4[4] = { 1, 0, 0, 1 }, c4[4] = { 1, 1, 1, 1 };
    for (uint32_t v = 0; v < nv; v++)
        packVertex(u.data.data() + (size_t)v * VSTRIDE, in->pos + 3 * v, in->nrm ? in->nrm + 3 * v : n4, in->tan ? in->tan + 4 * v : t4,
                   in->uv0 ? in->uv0 + 2 * v : z4, in->uv1 ? in->uv1 + 2 * v : z4, in->col ? in->col + 4 * v : c4);
    if (ni) {
        uint32_t* I = (uint32_t*)(u.data.data() + (size_t)nv * VSTRIDE);
        for (uint32_t i = 0; i < ni; i++) I[i] = std::min((uint32_t)in->ib[i], nv - 1);
    }
    gMeshState[id] = RS_PENDING;
    pushUpload(std::move(u));
    return id;
}

// Index sets shared by vertex-only meshes (the 17 PQS edge-stitch variants). Replaces any previous sets.
EXPORT int KSPVk_SetSharedIndices(const int* idx, const int* counts, int sets) {
    if (gState != ST_OK || !gSceneOK || sets < 0 || sets > MAXSHARED) return 0;
    uint32_t total = 0; for (int i = 0; i < sets; i++) total += (uint32_t)std::max(0, counts[i]);
    int64_t off = gIBA.alloc(total);
    if (off < 0) { gArenaFull = true; return 0; }
    for (int i = 0; i < gSharedN; i++) gSharedOld.push_back(gShared[i]);   // kept until the next reset: in-flight frames may still use them
    Upload u; u.kind = 2; u.id = 0; u.gen = 0; u.ibOff = (uint32_t)off; u.ni = total; u.nv = u.nsk = 0; u.vbOff = u.skOff = 0;
    u.data.resize((size_t)total * 4);
    memcpy(u.data.data(), idx, (size_t)total * 4);
    uint32_t o = (uint32_t)off;
    for (int i = 0; i < sets; i++) { gShared[i] = { o, (uint32_t)std::max(0, counts[i]) }; o += gShared[i].n; }
    gSharedN = sets;
    pushUpload(std::move(u));
    return 1;
}

// The caller stops drawing the id now; its storage is recycled once every in-flight frame is past it.
EXPORT void KSPVk_ReleaseMesh(int id) {
    if (gState != ST_OK || !gSceneOK || id < 0 || id >= gMeshN) return;
    gDead.push_back({ id, gFrameCounter });
}
static void reapMeshes() {
    while (!gDead.empty() && gFrameCounter >= gDead.front().second + 2 * K + 1) {
        int id = gDead.front().first; gDead.pop_front();
        MeshRec& m = gMesh[id];
        auto it = std::find(gPendMesh.begin(), gPendMesh.end(), id);
        if (it != gPendMesh.end()) gPendMesh.erase(it);
        releaseMeshStaging(m);
        if (m.alloc) { gVBA.release(m.vbOff, m.nv); gIBA.release(m.ibOff, m.ni); gSKA.release(m.skOff, m.nsk); }
        gMeshGen[id] = gMeshGen[id] + 1;
        gMeshState[id] = RS_NONE;
        m = MeshRec();
        gFreeIds.push_back(id);
    }
}

EXPORT int KSPVk_RegisterTexture(const TexIn* in) {
    if (gState != ST_OK || !gSceneOK || gTexN >= MAXTEX) return -1;
    int id = gTexN++;
    TexRec& t = gTex[id]; t = TexRec(); t.in = *in;
    gTexState[id] = RS_FAILED;
    ID3D11Resource* r = (ID3D11Resource*)in->tex;
    if (!r) return id;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { logf("texture %d: not 2D (%d)", id, dim); return id; }
    ID3D11Texture2D* src = (ID3D11Texture2D*)r; src->GetDesc(&t.d);
    t.cube = (t.d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) && t.d.ArraySize == 6;
    if ((t.d.ArraySize != 1 && !t.cube) || t.d.SampleDesc.Count != 1 || !mapFormat(t.d.Format, t.fmt, t.blk, t.bpe, t.sw)) {
        logf("texture %d: unsupported (format %d, %ux%u, array %u)", id, t.d.Format, t.d.Width, t.d.Height, t.d.ArraySize); return id;
    }
    D3D11_TEXTURE2D_DESC sd = t.d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (FAILED(gDev->CreateTexture2D(&sd, nullptr, &t.stg))) { logf("texture %d: staging alloc failed", id); return id; }
    gCtx->CopyResource(t.stg, src);
    gTexState[id] = RS_PENDING; gPendTex.push_back(id);
    return id;
}

static void pushUpload(Upload&& u) { std::lock_guard<std::mutex> l(gUpM); gUploads.push_back(std::move(u)); }

// Meshes serialized by older Unity versions keep the old channel order (position, normal, color, texcoords, tangent last), and Unity 2019 has no
// API for the real offsets. Score both orders by how many tangents look like tangents (|w| = 1) and switch to the legacy offsets if they win.
static bool legacyLayout(const MeshIn& in, MeshAttrIn* at, const D3D11_MAPPED_SUBRESOURCE* ms, uint32_t nv) {
    int ti = -1;
    for (int a = 0; a < in.attrCount; a++) if (at[a].attr == 2) ti = a;
    if (ti < 0 || at[ti].format != 0 || at[ti].dim != 4) return false;
    MeshAttrIn lg[16]; memcpy(lg, at, sizeof lg);
    auto rank = [](int a) { return a == 2 ? 11 : a == 3 ? 2 : a >= 4 && a <= 11 ? a - 1 : a; };
    int off[4] = {};
    for (int r = 0; r <= 13; r++)
        for (int a = 0; a < in.attrCount; a++)
            if (rank(lg[a].attr) == r) { lg[a].offset = off[lg[a].stream]; off[lg[a].stream] += fmtSize(lg[a].format) * lg[a].dim; }
    if (lg[ti].offset == at[ti].offset) return false;
    auto score = [&](const MeshAttrIn& A) {
        int n = 0;
        for (uint32_t v = 0; v < std::min(nv, 256u); v++) {
            float t[4]; memcpy(t, (const uint8_t*)ms[A.stream].pData + (size_t)v * in.stride[A.stream] + A.offset, 16);
            n += std::fabs(std::fabs(t[3]) - 1) < 1e-3f && std::fabs(t[0] * t[0] + t[1] * t[1] + t[2] * t[2] - 1) < 0.05f;
        }
        return n;
    };
    if (score(lg[ti]) <= score(at[ti])) return false;
    memcpy(at, lg, sizeof lg);
    return true;
}

// 0 = GPU copy not finished yet, 1 = done (converted and queued, or failed)
static int pumpMesh(int id, size_t& used) {
    MeshRec& m = gMesh[id]; const MeshIn& in = m.in;
    D3D11_MAPPED_SUBRESOURCE ms[5]{}; bool mapped[5]{};
    HRESULT hr = S_OK;
    for (int k = 0; k <= in.streams && hr == S_OK; k++)
        if (m.stg[k]) { hr = gCtx->Map(m.stg[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms[k]); mapped[k] = hr == S_OK; }
    auto unmap = [&] { for (int k = 0; k <= in.streams; k++) if (mapped[k]) gCtx->Unmap(m.stg[k], 0); };
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { unmap(); return 0; }
    if (hr != S_OK) { unmap(); releaseMeshStaging(m); gMeshState[id] = RS_FAILED; logf("mesh %d: map failed %08lx", id, (unsigned long)hr); return 1; }

    D3D11_BUFFER_DESC ibd; m.stg[in.streams]->GetDesc(&ibd);
    uint32_t nv = (uint32_t)in.vertexCount, ni = ibd.ByteWidth / (in.ib32 ? 4 : 2);
    MeshAttrIn at[16]; memcpy(at, in.attrs, sizeof at);
    if (legacyLayout(in, at, ms, nv)) logf("mesh %d: legacy vertex layout (tangent after texcoords)", id);
    bool skin = false;
    for (int a = 0; a < in.attrCount; a++) skin |= in.attrs[a].attr == 13;
    if (!allocMesh(m, nv, ni, skin ? nv : 0)) { unmap(); releaseMeshStaging(m); gMeshState[id] = RS_FAILED; logf("mesh %d: arena full", id); return 1; }
    Upload u;
    try { u = meshUpload(id, m); }
    catch (const std::bad_alloc&) { unmap(); releaseMeshStaging(m); gMeshState[id] = RS_FAILED; logf("mesh %d: out of memory", id); return 1; }
    uint8_t* V = u.data.data(); uint32_t* I = (uint32_t*)(V + (size_t)nv * VSTRIDE); uint8_t* S = (uint8_t*)(I + ni);
    bool bad = false;
    for (uint32_t v = 0; v < nv; v++) {
        float pos[4] = {}, nrm[4] = { 0, 0, 1, 0 }, tan[4] = { 1, 0, 0, 1 }, uv[4] = {}, uv1[4] = {}, col[4] = { 1, 1, 1, 1 }, w[4] = { 1, 0, 0, 0 }, j[4] = {};
        bool haveW = false;
        for (int a = 0; a < in.attrCount; a++) {
            const MeshAttrIn& A = at[a];
            float* dst = A.attr == 0 ? pos : A.attr == 1 ? nrm : A.attr == 2 ? tan : A.attr == 3 ? col : A.attr == 4 ? uv : A.attr == 5 ? uv1 : A.attr == 12 ? w : A.attr == 13 ? j : nullptr;
            if (!dst) continue;
            if (A.attr == 12) { haveW = true; w[0] = 0; }
            readAttr((const uint8_t*)ms[A.stream].pData + (size_t)v * in.stride[A.stream] + A.offset, A.format, A.dim, dst);
        }
        packVertex(V + (size_t)v * VSTRIDE, pos, nrm, tan, uv, uv1, col);
        if (skin) {
            if (!haveW) { w[0] = 1; w[1] = w[2] = w[3] = 0; }
            float sum = w[0] + w[1] + w[2] + w[3]; if (sum > 0) for (float& x : w) x /= sum;
            uint8_t* s = S + (size_t)v * SSTRIDE; memcpy(s, w, 16);
            for (int c = 0; c < 4; c++) { if (j[c] > 255) bad = true; s[16 + c] = (uint8_t)j[c]; }
        }
    }
    if (in.ib32) memcpy(I, ms[in.streams].pData, (size_t)ni * 4);
    else { const uint16_t* src = (const uint16_t*)ms[in.streams].pData; for (uint32_t i = 0; i < ni; i++) I[i] = src[i]; }
    unmap(); releaseMeshStaging(m);
    if (bad) { gMeshState[id] = RS_FAILED; logf("mesh %d: more than 256 bones", id); return 1; }
    for (int s = 0; s < in.subCount; s++) {
        int st = std::max(0, in.subStart[s]); m.in.subStart[s] = std::min(st, (int)ni); m.in.subLen[s] = std::max(0, std::min(in.subLen[s], (int)ni - m.in.subStart[s]));
    }
    m.skin = skin;
    memcpy(m.c, in.center, 12);
    m.rad = sqrtf(in.extents[0] * in.extents[0] + in.extents[1] * in.extents[1] + in.extents[2] * in.extents[2]);
    used += u.data.size();
    pushUpload(std::move(u));
    return 1;
}

static int pumpTex(int id, size_t& used) {
    TexRec& t = gTex[id];
    uint32_t mips = t.d.MipLevels, layers = t.cube ? 6 : 1, nsub = mips * layers;   // subresource = layer * mips + mip
    uint32_t skip = 0;
    const uint32_t cap = (uint32_t)gTexMax << ((t.in.filter >> 8) & 1);   // filter bit 8: scaled-planet surface map, twice the cap
    while (gTexMax > 0 && skip + 1 < mips && std::max(t.d.Width >> skip, t.d.Height >> skip) > cap) skip++;
    std::vector<D3D11_MAPPED_SUBRESOURCE> ms(nsub);
    HRESULT hr = S_OK; uint32_t k = 0;
    for (; k < nsub && (hr = gCtx->Map(t.stg, k, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms[k])) == S_OK; k++) {}
    auto unmap = [&] { for (uint32_t i = 0; i < k; i++) gCtx->Unmap(t.stg, i); };
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { unmap(); return 0; }
    if (hr != S_OK) { unmap(); t.stg->Release(); t.stg = nullptr; gTexState[id] = RS_FAILED; logf("texture %d: map failed %08lx", id, (unsigned long)hr); return 1; }
    Upload u; u.kind = 1; u.id = id;
    size_t total = 0;
    std::vector<uint32_t> src;   // D3D11 subresource of each copied region
    for (uint32_t j = 0; j < nsub; j++) {
        uint32_t i = j % mips, w = std::max(1u, t.d.Width >> i), h = std::max(1u, t.d.Height >> i);
        if (i < skip) continue;
        size_t rb = t.blk ? ((w + 3) / 4) * t.bpe : (size_t)w * t.bpe, rows = t.blk ? (h + 3) / 4 : h;
        VkBufferImageCopy r{}; r.bufferOffset = total; r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - skip, j / mips, 1 }; r.imageExtent = { w, h, 1 };
        u.reg.push_back(r); src.push_back(j);
        total = (total + rb * rows + 15) & ~(size_t)15;
    }
    t.skip = skip; t.mips = mips - skip;
    try { u.data.resize(total); }
    catch (const std::bad_alloc&) { unmap(); logf("texture %d: out of memory (%zu bytes), retrying", id, total); return 0; }   // keep the staging copy, retry later
    for (size_t k2 = 0; k2 < src.size(); k2++) {
        uint32_t j = src[k2], i = j % mips, w = std::max(1u, t.d.Width >> i), h = std::max(1u, t.d.Height >> i);
        size_t rb = t.blk ? ((w + 3) / 4) * t.bpe : (size_t)w * t.bpe, rows = t.blk ? (h + 3) / 4 : h;
        for (size_t y = 0; y < rows; y++) memcpy(u.data.data() + u.reg[k2].bufferOffset + y * rb, (const uint8_t*)ms[j].pData + y * ms[j].RowPitch, rb);
    }
    unmap(); t.stg->Release(); t.stg = nullptr;
    used += total;
    pushUpload(std::move(u));
    return 1;
}

// Converts finished staging copies into upload jobs; budgetKB caps the bytes converted per call. Returns the number still pending.
EXPORT int KSPVk_Pump(int budgetKB) {
    if (gState != ST_OK || !gSceneOK) return 0;
    try {   // never let a C++ exception unwind into Mono
        reapMeshes();
        size_t budget = (size_t)std::max(1, budgetKB) << 10, used = 0;
        for (auto* q : { &gPendMesh, &gPendTex })
            for (size_t i = 0; i < q->size() && used < budget;) {
                int id = (*q)[i];
                if (q == &gPendMesh ? pumpMesh(id, used) : pumpTex(id, used)) q->erase(q->begin() + i); else i++;
            }
    } catch (const std::exception& e) { logf("pump: %s", e.what()); }
    return (int)(gPendMesh.size() + gPendTex.size());
}

EXPORT void KSPVk_GetStatePtrs(volatile int8_t** mesh, volatile int8_t** tex) { *mesh = gMeshState; *tex = gTexState; }
EXPORT void KSPVk_SetTexMax(int v) { gTexMax = v; }
EXPORT void KSPVk_SetSceneDebug(int flags) { gSceneDebug = flags; logf("scene debug flags %d", flags); }

static void drainRenderThread();
// Drops every registered mesh / texture (arena full, or explicit reset). The caller must re-register.
EXPORT void KSPVk_ResetScene() {
    if (!gSceneOK) return;
    drainRenderThread(); vkQueueWaitIdle(gQ);
    { std::lock_guard<std::mutex> l(gUpM); gUploads.clear(); }
    for (int i = 0; i < gMeshN; i++) { releaseMeshStaging(gMesh[i]); gMeshState[i] = RS_NONE; gMesh[i] = MeshRec(); }
    for (int i = TEX_FIRST; i < gTexN; i++) { if (gTex[i].stg) gTex[i].stg->Release(); freeVImg(gTex[i].im); gTexState[i] = RS_NONE; }
    resetTexDescriptors(TEX_FIRST);
    gPendMesh.clear(); gPendTex.clear(); gTexReady.clear(); for (SlotScene& ss : gSS) { ss.texApplied = 0; setEnv(ss, -1); }
    gMeshN = 0; gTexN = TEX_FIRST; gArenaFull = false;
    gVBA.init(gVBA.cap); gIBA.init(gIBA.cap); gSKA.init(gSKA.cap);
    for (auto& g : gMeshGen) g = g + 1;
    gFreeIds.clear(); gDead.clear(); gSharedN = 0; gSharedOld.clear();
    logf("scene reset");
}

EXPORT void KSPVk_GetSceneStats(SceneStats* out) {
    std::lock_guard<std::mutex> l(gQm);
    *out = gSStats;
    out->meshes = gMeshN; out->textures = gTexN - TEX_FIRST; out->meshReady = out->texReady = 0;
    for (int i = 0; i < gMeshN; i++) out->meshReady += gMeshState[i] == RS_READY;
    out->texBytes = 0;
    for (int i = TEX_FIRST; i < gTexN; i++) if (gTexState[i] == RS_READY) { out->texReady++; out->texBytes += gTex[i].bytes; }
    out->pending = (int)(gPendMesh.size() + gPendTex.size()); out->arenaFull = gArenaFull; out->uploadedBytes = gUploadedBytes;
}

// Main thread, BeginFrame2: copy the scene into slot si (the render thread has finished with that slot's previous frame).
static void sceneCopy(int si, const SceneIn* sc) {
    SlotScene& ss = gSS[si];
    if (!sc || !gSceneOK) { ss.h = SceneIn(); ss.draws.clear(); return; }
    ss.h = *sc;
    ss.h.drawCount = std::min(std::max(0, sc->drawCount), (int)MAXDRAW); ss.h.instCount = std::min(std::max(0, sc->instCount), (int)MAXINST);
    ss.h.matCount = std::min(std::max(0, sc->matCount), (int)MAXMAT); ss.h.bone4Count = std::min(std::max(0, sc->bone4Count), (int)MAXBONE4);
    ss.h.lightCount = std::min(std::max(0, sc->lightCount), (int)MAXLIGHT);
    ss.draws.assign(sc->draws, sc->draws + ss.h.drawCount);
    ss.insts.assign(sc->insts, sc->insts + ss.h.instCount * INSTF);
    ss.mats.assign(sc->mats, sc->mats + ss.h.matCount * MATF);
    ss.bones.assign(sc->bones, sc->bones + ss.h.bone4Count * 4);
}

// ---------------- render thread ----------------
static bool sceneHasUploads() { std::lock_guard<std::mutex> l(gUpM); return !gUploads.empty(); }

static void sceneUploads(VkCommandBuffer cb, SlotScene& ss) {
    if (!gSceneOK) return;
    for (Buf& b : ss.garbage) freeBuf(b);
    ss.garbage.clear();
    std::vector<Upload> work;
    {
        std::lock_guard<std::mutex> l(gUpM);
        VkDeviceSize top = 0;
        while (!gUploads.empty()) {
            VkDeviceSize sz = (gUploads.front().data.size() + 15) & ~15ull;
            if (top + sz > STAGE_RING && !(work.empty() && sz > STAGE_RING)) break;   // oversize jobs go alone, through a temporary buffer
            top += sz; work.push_back(std::move(gUploads.front())); gUploads.pop_front();
            if (sz > STAGE_RING) break;
        }
    }
    bool bufCopies = false; VkDeviceSize top = 0;
    for (Upload& u : work) {
        if (u.kind == 0 && u.gen != gMeshGen[u.id]) continue;   // released (or reset) before it was uploaded
        VkBuffer src = ss.stage.b; VkDeviceSize off = top;
        if (u.data.size() > STAGE_RING) {
            Buf t; if (!makeBuf(t, u.data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) { if (u.kind < 2) (u.kind ? gTexState : gMeshState)[u.id] = RS_FAILED; continue; }
            memcpy(t.p, u.data.data(), u.data.size()); src = t.b; off = 0; ss.garbage.push_back(t);
        } else { memcpy(ss.stage.p + top, u.data.data(), u.data.size()); top += (u.data.size() + 15) & ~15ull; }
        gUploadedBytes += u.data.size();
        if (u.kind == 2) {
            VkBufferCopy c{ off, u.ibOff * 4ull, u.ni * 4 };
            if (u.ni) vkCmdCopyBuffer(cb, src, gIB.b, 1, &c);
            bufCopies = true;
        } else if (u.kind == 0) {
            VkBufferCopy c[3] = { { off, u.vbOff * (VkDeviceSize)VSTRIDE, u.nv * VSTRIDE }, { off + u.nv * VSTRIDE, u.ibOff * 4ull, u.ni * 4 },
                                  { off + u.nv * VSTRIDE + u.ni * 4, u.skOff * (VkDeviceSize)SSTRIDE, u.nsk * SSTRIDE } };
            vkCmdCopyBuffer(cb, src, gVB.b, 1, &c[0]);
            if (u.ni) vkCmdCopyBuffer(cb, src, gIB.b, 1, &c[1]);
            if (u.nsk) vkCmdCopyBuffer(cb, src, gSK.b, 1, &c[2]);
            bufCopies = true;
            gMeshState[u.id] = RS_READY;
        } else {
            TexRec& t = gTex[u.id];
            uint32_t layers = t.cube ? 6 : 1, mips = (uint32_t)u.reg.size() / layers;
            if (!makeVImg(t.im, t.fmt, std::max(1u, t.d.Width >> t.skip), std::max(1u, t.d.Height >> t.skip), mips, layers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, t.sw, t.cube)) {
                freeVImg(t.im); gTexState[u.id] = RS_FAILED; logf("texture %d: image creation failed", u.id); continue;
            }
            for (auto& r : u.reg) r.bufferOffset += off;
            imgBarrier(cb, t.im.img, VK_IMAGE_ASPECT_COLOR_BIT, mips, layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(cb, src, t.im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)u.reg.size(), u.reg.data());
            imgBarrier(cb, t.im.img, VK_IMAGE_ASPECT_COLOR_BIT, mips, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            gTexReady.push_back(u.id); t.bytes = u.data.size();
            gTexState[u.id] = RS_READY;
        }
    }
    if (bufCopies) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    // this slot's descriptor set catches up with textures that became ready since it was last recorded
    if (ss.texApplied < gTexReady.size()) {
        std::vector<VkDescriptorImageInfo> ii; std::vector<VkWriteDescriptorSet> w;
        ii.reserve(gTexReady.size() - ss.texApplied);
        for (; ss.texApplied < gTexReady.size(); ss.texApplied++) {
            TexRec& t = gTex[gTexReady[ss.texApplied]];
            if (!t.im.view || t.cube) continue;
            ii.push_back({ sampler(t.in.wrapU, t.in.wrapV, t.in.filter & 0xff, t.in.aniso), t.im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = ss.ds; x.dstBinding = 4; x.dstArrayElement = (uint32_t)gTexReady[ss.texApplied];
            x.descriptorCount = 1; x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; x.pImageInfo = &ii.back();
            w.push_back(x);
        }
        if (!w.empty()) vkUpdateDescriptorSets(gVd, (uint32_t)w.size(), w.data(), 0, nullptr);
    }
}

static bool sceneEnsureDepth(int w, int h) {
    if (gZ.img && gZw == w && gZh == h) return true;
    if (gZ.img) { vkDeviceWaitIdle(gVd); freeVImg(gZ); }
    gZw = w; gZh = h;
    return makeVImg(gZ, VK_FORMAT_D32_SFLOAT, w, h, 1, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
}

struct V3 { float x, y, z; };
static V3 v3(const float* p) { return { p[0], p[1], p[2] }; }
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static V3 norm(V3 a) { float l = sqrtf(dot(a, a)); return l > 0 ? a * (1 / l) : V3{ 0, 1, 0 }; }

// far cascades are re-rendered every 2nd / 4th frame and reused in between, with the matrices they were rendered with
static const int CASC_EVERY[NCASC] = { 1, 1, 2, 4 };
static struct { bool ok; float R, cx, cy, z0, M[16], texel, bias; V3 L; } gCascC[NCASC];
static bool gShadowInit, gShLast; static uint32_t gShFrame;
struct DrawRef { const DrawIn* d; const MeshRec* m; float k; };   // k: front-to-back sort key (nearest bounding-sphere point)
static std::vector<DrawRef> gBucket[2][2], gIva[2][2], gIvaMask, gShBucket[NCASC][2][2], gWorld[NFAM];
static bool gLocal;   // render thread: the current frame has local-view draws

// Infinite reversed-Z projection (Vulkan y-down) times view; P: Unity m00 m01 m02 m10 m11 m12.
static void viewProj(const float* V, const float* P, float nearClip, float* out) {
    float Pv[16] = { P[0], P[1], P[2], 0, -P[3], -P[4], -P[5], 0, 0, 0, 0, nearClip, 0, 0, -1, 0 };
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += Pv[i * 4 + k] * V[k * 4 + j];
        out[i * 4 + j] = s;
    }
}
static void drawMesh(VkCommandBuffer cb, const DrawIn& d, const MeshRec& m, bool skin, uint32_t fi = 0) {   // fi: draw index (Enhanced motion rows)
    if (skin) {
        VkBuffer bs[2] = { gVB.b, gSK.b }; VkDeviceSize os[2] = { m.vbOff * (VkDeviceSize)VSTRIDE, m.skOff * (VkDeviceSize)SSTRIDE };
        vkCmdBindVertexBuffers(cb, 0, 2, bs, os);
        vkCmdDrawIndexed(cb, m.in.subLen[d.sub], 1, m.ibOff + m.in.subStart[d.sub], m.in.subBase[d.sub], fi);
    } else if (m.shared) vkCmdDrawIndexed(cb, gShared[d.sub].n, 1, gShared[d.sub].off, (int32_t)m.vbOff, fi);
    else vkCmdDrawIndexed(cb, m.in.subLen[d.sub], 1, m.ibOff + m.in.subStart[d.sub], (int32_t)m.vbOff + m.in.subBase[d.sub], fi);
}
struct Casc { V3 r, u, f; float cx, cy, z0, R, depth; bool skip; };

static bool drawValid(const DrawIn& d, const SceneIn& H) {
    int fam = d.pad0;
    if (fam < 0 || fam >= NFAM || d.mesh < 0 || d.mesh >= gMeshN || gMeshState[d.mesh] != RS_READY || d.mat < 0 || d.mat >= H.matCount) return false;
    if (fam == FAM_PART && (d.inst < 0 || d.inst >= H.instCount)) return false;
    const MeshRec& m = gMesh[d.mesh];
    return m.shared ? d.sub >= 0 && d.sub < gSharedN && gShared[d.sub].n : d.sub >= 0 && d.sub < m.in.subCount && m.in.subLen[d.sub];
}
static void drawBounds(const DrawIn& d, const MeshRec& m, bool skin, V3& c, float& rad) {   // world bounding sphere
    if (skin) { c = v3(d.r); rad = d.r[3]; return; }
    const float* r = d.r;
    c = { r[0] * m.c[0] + r[1] * m.c[1] + r[2] * m.c[2] + r[3], r[4] * m.c[0] + r[5] * m.c[1] + r[6] * m.c[2] + r[7], r[8] * m.c[0] + r[9] * m.c[1] + r[10] * m.c[2] + r[11] };
    float s = 0; for (int j = 0; j < 3; j++) s = std::max(s, r[j] * r[j] + r[4 + j] * r[4 + j] + r[8 + j] * r[8 + j]);
    rad = m.rad * sqrtf(s);
}

// Fills the frame buffers, culls, records the shadow passes. Returns true if the main pass has draws.
static bool scenePrepare(VkCommandBuffer cb, SlotScene& ss, int w, int h) {
    auto t0 = std::chrono::steady_clock::now();
    const SceneIn& H = ss.h;
    gSStats.draws = H.drawCount; gSStats.visible = gSStats.shadowDraws = 0;
    if (!gSceneOK || H.drawCount == 0 || !sceneEnsureDepth(w, h)) return false;
    int dbg = gSceneDebug;
    memcpy(ss.mat.p, ss.mats.data(), ss.mats.size() * 4);
    memcpy(ss.inst.p, ss.insts.data(), ss.insts.size() * 4);
    memcpy(ss.bone.p, ss.bones.data(), ss.bones.size() * 4);

    FrameU F{};
    const float* V = H.view; const float* P = H.proj;
    viewProj(V, P, H.nearClip, F.viewProj);
    memcpy(F.camPos, H.camPos, 16); memcpy(F.lightCol, H.lightCol, 16); memcpy(F.ambient, H.ambient, 16);
    V3 L = norm(v3(H.lightDir)); F.lightDir[0] = L.x; F.lightDir[1] = L.y; F.lightDir[2] = L.z;
    for (int i = 0; i < 4; i++) F.viewZ[i] = -V[8 + i];
    memcpy(F.vp[0], F.viewProj, 64); memcpy(F.camPosV[0], F.camPos, 16); memcpy(F.viewZV[0], F.viewZ, 16);
    for (int v = 0; v < 2; v++) {
        const ViewIn& X = H.views[v];
        if (X.nearClip <= 0) continue;
        viewProj(X.view, X.proj, X.nearClip, F.vp[v + 1]); memcpy(F.camPosV[v + 1], X.camPos, 16);
        for (int i = 0; i < 4; i++) F.viewZV[v + 1][i] = -X.view[8 + i];
    }
    F.misc[0] = H.misc[0] > 0 ? H.misc[0] : 1e30f; F.misc[1] = H.misc[1]; F.misc[2] = sinf(H.misc[1] / 8); F.misc[3] = H.misc[1] / 20;
    F.nLights[0] = H.lightCount; memcpy(F.lights, H.lights, sizeof(LightIn) * H.lightCount);
    memcpy(F.ambEq, H.ambEq, 16); memcpy(F.ambGround, H.ambGround, 16); memcpy(F.env, H.env, 16);
    int env = H.envTex >= TEX_FIRST && H.envTex < gTexN && gTexState[H.envTex] == RS_READY && gTex[H.envTex].cube ? H.envTex : -1;
    if (env != ss.envApplied) setEnv(ss, env);
    F.env[3] = env >= 0 ? (float)(gTex[env].mips - 1) : 0;

    // cascades: bounding sphere of each view-frustum slice, light-space ortho box, texel-snapped
    bool shadows = H.shadow[2] > 0 && !(dbg & DBG_NO_SHADOWS);
    Casc cs[NCASC];
    if (shadows) {
        static const float frac[NCASC] = { 0.012f, 0.06f, 0.24f, 1.f };
        V3 cam = v3(H.camPos), fwd = norm(V3{ -V[8], -V[9], -V[10] });
        float tx = 1 / P[0], ty = 1 / P[4], k2 = tx * tx + ty * ty;
        V3 lf = L * -1.f, a = fabsf(lf.y) < 0.99f ? V3{ 0, 1, 0 } : V3{ 1, 0, 0 };
        V3 lr = norm(cross(a, lf)), lu = cross(lf, lr);
        float n = H.nearClip;
        gShFrame++;
        bool keep = gShadowInit && gShLast && H.shadow[3] == 0;   // shadow[3]: world shifted / scene changed
        for (int c = 0; c < NCASC; c++) {
            float f = H.shadowDist * frac[c];
            float z = std::min(f, (f + n) * (1 + k2) * 0.5f);
            float R = ceilf(sqrtf((f - z) * (f - z) + f * f * k2));
            V3 ctr = cam + fwd * z;
            float texel = 2 * R / SHADOW_RES;
            Casc& k = cs[c];
            k.r = lr; k.u = lu; k.f = lf; k.R = R;
            k.cx = floorf(dot(lr, ctr) / texel) * texel; k.cy = floorf(dot(lu, ctr) / texel) * texel;
            float back = gDepthClamp ? R : R + 2000;   // casters between the sun and the slice; depth clamp keeps the ones in front of the box
            k.z0 = dot(lf, ctr) - R - back; k.depth = 2 * R + back;
            float* M = F.shadowVP[c];
            auto& C = gCascC[c];
            // reuse while the cached box still covers the slice (camera moved < 10% of its radius) and the sun has not turned
            float tol = 0.1f * R;
            if (keep && C.ok && (gShFrame + c) % CASC_EVERY[c] && C.R == R && dot(C.L, L) > 0.99999f &&
                fabsf(C.cx - k.cx) < tol && fabsf(C.cy - k.cy) < tol && fabsf(C.z0 - k.z0) < tol) {
                k.skip = true; memcpy(M, C.M, 64);
                F.splits[c] = f; F.cascTexel[c] = C.texel; F.cascBias[c] = C.bias;
                n = f; continue;
            }
            k.skip = false;
            M[0] = lr.x / R; M[1] = lr.y / R; M[2] = lr.z / R; M[3] = -k.cx / R;
            M[4] = lu.x / R; M[5] = lu.y / R; M[6] = lu.z / R; M[7] = -k.cy / R;
            M[8] = lf.x / k.depth; M[9] = lf.y / k.depth; M[10] = lf.z / k.depth; M[11] = -k.z0 / k.depth;
            M[15] = 1;
            F.splits[c] = f; F.cascTexel[c] = texel; F.cascBias[c] = 1.0f * texel / k.depth;
            C.ok = true; C.R = R; C.cx = k.cx; C.cy = k.cy; C.z0 = k.z0; C.L = L; C.texel = texel; C.bias = F.cascBias[c]; memcpy(C.M, M, 64);
            n = f;
        }
        F.shadowP[0] = H.shadow[0]; F.shadowP[1] = H.shadow[1]; F.shadowP[2] = 1.f / SHADOW_RES; F.shadowP[3] = 1;
    }
    if (gEF) enhPrepare(F, ss);
    memcpy(ss.ubo.p, &F, sizeof F);

    gShLast = shadows;

    // culling: frustum planes from viewProj (left, right, bottom, top, near)
    float pl[5][4]; const float* M = F.viewProj;
    for (int i = 0; i < 4; i++) {
        pl[0][i] = M[12 + i] + M[i]; pl[1][i] = M[12 + i] - M[i]; pl[2][i] = M[12 + i] + M[4 + i]; pl[3][i] = M[12 + i] - M[4 + i]; pl[4][i] = M[12 + i] - M[8 + i];
    }
    for (auto& p : pl) { float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]); for (float& x : p) x /= l; }
    for (auto& b : gBucket) for (auto& v : b) v.clear();
    for (auto& b : gIva) for (auto& v : b) v.clear();
    gIvaMask.clear();
    for (auto& c : gShBucket) for (auto& b : c) for (auto& v : b) v.clear();
    for (auto& v : gWorld) v.clear();
    gLocal = false;
    const int* matInfo = (const int*)ss.mats.data();
    const V3 eye = v3(H.camPos);
    for (const DrawIn& d : ss.draws) {
        int fam = d.pad0;
        if (!drawValid(d, H)) continue;
        const MeshRec& m = gMesh[d.mesh];
        bool skin = fam == FAM_PART && (d.flags & DF_SKIN) != 0;
        if (fam != FAM_PART && fam != FAM_PQS && fam != FAM_OCEAN) { gWorld[fam].push_back({ &d, &m }); gSStats.visible++; continue; }   // scaled / galaxy views: a handful of draws, unculled
        if (skin && (!m.skin || d.bone < 0 || d.bone >= H.bone4Count)) continue;
        V3 c; float rad;
        drawBounds(d, m, skin, c, rad);
        int cut = fam == FAM_PART && (matInfo[d.mat * MATF + 44] & MF_CUTOFF) ? 1 : 0;
        bool vis = true;
        if (!(dbg & DBG_NO_CULL)) for (auto& p : pl) if (p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3] < -rad) { vis = false; break; }
        if (vis) { (fam != FAM_PART ? gWorld[fam] : d.flags & DF_MASK ? gIvaMask : d.flags & DF_IVA ? gIva[skin][cut] : gBucket[skin][cut]).push_back({ &d, &m, sqrtf(dot(c - eye, c - eye)) - rad }); gSStats.visible++; gLocal = true; }
        if (shadows && (d.flags & DF_CAST) && !(d.flags & DF_MASK))
            for (int k = 0; k < NCASC; k++) {
                const Casc& q = cs[k];
                if (q.skip) continue;
                float x = dot(q.r, c) - q.cx, y = dot(q.u, c) - q.cy, z = dot(q.f, c) - q.z0;
                if (fabsf(x) > q.R + rad || fabsf(y) > q.R + rad || z - rad > q.depth || (!gDepthClamp && z + rad < 0)) continue;
                gShBucket[k][skin][cut].push_back({ &d, &m, z - rad }); gSStats.shadowDraws++;
            }
    }

    // front to back: early depth rejection instead of overdraw
    auto byK = [](std::vector<DrawRef>& v) { std::sort(v.begin(), v.end(), [](const DrawRef& a, const DrawRef& b) { return a.k < b.k; }); };
    for (auto& b : gBucket) for (auto& v : b) byK(v);
    for (auto& b : gIva) for (auto& v : b) byK(v);
    for (auto& c : gShBucket) for (auto& b : c) for (auto& v : b) byK(v);
    byK(gWorld[FAM_PQS]); byK(gWorld[FAM_OCEAN]);

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gPLay, 0, 1, &ss.ds, 0, nullptr);
    vkCmdBindIndexBuffer(cb, gIB.b, 0, VK_INDEX_TYPE_UINT32);
    if (shadows) {
        imgBarrier(cb, gShadow.img, VK_IMAGE_ASPECT_DEPTH_BIT, 1, NCASC, gShadowInit ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        gShadowInit = true;
        for (int k = 0; k < NCASC; k++) {
            if (cs[k].skip) continue;
            VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; da.imageView = gShadowLayer[k]; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; da.clearValue.depthStencil.depth = 1;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = { { 0, 0 }, { SHADOW_RES, SHADOW_RES } }; ri.layerCount = 1; ri.pDepthAttachment = &da;
            vkCmdBeginRendering(cb, &ri);
            VkViewport vp{ 0, 0, (float)SHADOW_RES, (float)SHADOW_RES, 0, 1 }; VkRect2D sc{ { 0, 0 }, { SHADOW_RES, SHADOW_RES } };
            vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
            for (int s = 0; s < 2; s++) for (int c = 0; c < 2; c++) {
                auto& list = gShBucket[k][s][c];
                if (list.empty()) continue;
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gShPipe[s][c]);
                vkCmdSetDepthBias(cb, 1.0f, 0.0f, 1.5f);
                vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
                VkDeviceSize zero = 0; if (!s) vkCmdBindVertexBuffers(cb, 0, 1, &gVB.b, &zero);
                for (const DrawRef& r : list) {
                    const DrawIn& d = *r.d;
                    float pc[16]; memcpy(pc, d.r, 48); int* ids = (int*)(pc + 12); ids[0] = d.mat; ids[1] = d.inst; ids[2] = k; ids[3] = d.bone;
                    vkCmdPushConstants(cb, gPLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
                    drawMesh(cb, d, *r.m, s);
                }
            }
            vkCmdEndRendering(cb);
        }
        imgBarrier(cb, gShadow.img, VK_IMAGE_ASPECT_DEPTH_BIT, 1, NCASC, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    imgBarrier(cb, gZ.img, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    gSStats.recordMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

// Inside the main dynamic-rendering pass (colour, linear depth, gZ). Views back to front: galaxy, scaled space, then (after a depth reset) the local view.
static VkPipeline gEPipe[2][2], gEWPipe[NFAM], gEMaskPipe;   // Enhanced variants (created on first use)
static void runList(VkCommandBuffer cb, SlotScene& ss, const std::vector<DrawRef>& list, VkPipeline pipe, bool skin, int view) {
    if (list.empty()) return;
    bool flipAll = (gSceneDebug & DBG_FLIP_WINDING) != 0;
    VkDeviceSize zero = 0;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (!skin) vkCmdBindVertexBuffers(cb, 0, 1, &gVB.b, &zero);
    int face = -1;
    for (const DrawRef& r : list) {
        const DrawIn& d = *r.d;
        float pc[16]; memcpy(pc, d.r, 48); int* ids = (int*)(pc + 12); ids[0] = d.mat; ids[1] = d.inst; ids[2] = view; ids[3] = d.bone;
        vkCmdPushConstants(cb, gPLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
        const float* q = d.r;
        float det = skin ? 1 : q[0] * (q[5] * q[10] - q[6] * q[9]) - q[1] * (q[4] * q[10] - q[6] * q[8]) + q[2] * (q[4] * q[9] - q[5] * q[8]);
        int f = (det < 0) != flipAll;   // mirrored transforms flip the winding
        if (f != face) { vkCmdSetFrontFace(cb, f ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE); face = f; }
        drawMesh(cb, d, *r.m, skin, gEF ? (uint32_t)(r.d - ss.draws.data()) : 0);
    }
}
static void sceneDraw(VkCommandBuffer cb, SlotScene& ss, int w, int h) {
    auto t0 = std::chrono::steady_clock::now();
    VkViewport vp{ 0, 0, (float)w, (float)h, 0, 1 }; VkRect2D sc{ { 0, 0 }, { (uint32_t)w, (uint32_t)h } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    auto run = [&](const std::vector<DrawRef>& list, VkPipeline pipe, bool skin, int view) { runList(cb, ss, list, pipe, skin, view); };
    auto P = [&](int s, int c) { return gEF ? gEPipe[s][c] : gPipe[s][c]; };
    auto W = [&](int f) { return gEF ? gEWPipe[f] : gWPipe[f]; };
    run(gWorld[FAM_GALAXY], W(FAM_GALAXY), false, 2);
    for (int f : { FAM_SCALED, FAM_SUN, FAM_ATMO }) run(gWorld[f], W(f), false, 1);
    if (!gWorld[FAM_SCALED].empty() || !gWorld[FAM_SUN].empty() || !gWorld[FAM_ATMO].empty()) {
        VkClearAttachment ca{}; ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; ca.clearValue.depthStencil.depth = 0;
        VkClearRect r{ sc, 0, 1 };
        vkCmdClearAttachments(cb, 1, &ca, 1, &r);
    }
    for (int s = 0; s < 2; s++) for (int c = 0; c < 2; c++) run(gBucket[s][c], P(s, c), s, 0);
    run(gWorld[FAM_PQS], W(FAM_PQS), false, 0);
    if (gEF && !gWorld[FAM_OCEAN].empty()) enhOceanSplit(cb, ss);
    run(gWorld[FAM_OCEAN], W(FAM_OCEAN), false, 0);
    bool iva = !gIvaMask.empty();
    for (auto& b : gIva) for (auto& v : b) iva |= !v.empty();
    if (iva) {   // like stock: the interior over everything, minus its DepthMask windows
        VkClearAttachment ca{}; ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; ca.clearValue.depthStencil.depth = 0;
        VkClearRect r{ sc, 0, 1 };
        vkCmdClearAttachments(cb, 1, &ca, 1, &r);
        run(gIvaMask, gEF ? gEMaskPipe : gMaskPipe, false, 0);
        for (int s = 0; s < 2; s++) for (int c = 0; c < 2; c++) run(gIva[s][c], P(s, c), s, 0);
    }
    gSStats.recordMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static void sceneShutdown() {
    if (!gVd) return;
    for (SlotScene& ss : gSS) { for (Buf* b : { &ss.ubo, &ss.mat, &ss.inst, &ss.bone, &ss.stage }) freeBuf(*b); for (Buf& b : ss.garbage) freeBuf(b); ss.garbage.clear(); }
    for (Buf* b : { &gVB, &gIB, &gSK }) freeBuf(*b);
    if (gTex) for (int i = 0; i < gTexN; i++) { if (gTex[i].stg) gTex[i].stg->Release(); freeVImg(gTex[i].im); }
    if (gMesh) for (int i = 0; i < gMeshN; i++) releaseMeshStaging(gMesh[i]);
    for (auto& kv : gSmp) vkDestroySampler(gVd, kv.second, nullptr);
    gSmp.clear();
    for (VkSampler sm : { gShadowSmp, gShadowRawSmp }) if (sm) vkDestroySampler(gVd, sm, nullptr);
    for (VkImageView v : gShadowLayer) if (v) vkDestroyImageView(gVd, v, nullptr);
    freeVImg(gShadow); freeVImg(gZ); freeVImg(gDefCube); gShadowInit = gShLast = false;
    for (auto& a : gPipe) for (VkPipeline p : a) if (p) vkDestroyPipeline(gVd, p, nullptr);
    for (auto& a : gShPipe) for (VkPipeline p : a) if (p) vkDestroyPipeline(gVd, p, nullptr);
    for (VkPipeline p : gWPipe) if (p) vkDestroyPipeline(gVd, p, nullptr);
    if (gMaskPipe) vkDestroyPipeline(gVd, gMaskPipe, nullptr);
    if (gPLay) vkDestroyPipelineLayout(gVd, gPLay, nullptr);
    if (gDPool) vkDestroyDescriptorPool(gVd, gDPool, nullptr);
    if (gDSL) vkDestroyDescriptorSetLayout(gVd, gDSL, nullptr);
    gSceneOK = false;
}
