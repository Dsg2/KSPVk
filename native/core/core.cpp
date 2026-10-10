// KSPVk native core (Phase 1 interop + Phase 2 scene renderer in scene.inl).
// A Vulkan render thread draws into D3D11-shared images (colour RGBA8 + linear view depth R32F, K slots);
// Unity's D3D11 cameras composite them from GL.IssuePluginEvent callbacks (CameraEvent.BeforeForwardOpaque).
// Sync: fenceVk (Vulkan signals frame N rendered, D3D GPU waits), fenceD3D (D3D signals frame N consumed,
// render thread waits before reusing that slot). Any failure -> state FAILED -> pure pass-through.
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define EXPORT extern "C" __declspec(dllexport)
enum { K = 3, MAXCAM = 16 };
enum { EV_FRAME = 1, EV_COMPOSITE = 2, EV_END = 3 };
enum { ST_NONE = 0, ST_OK = 1, ST_FAILED = 2 };
static bool gCamLogged[16];
enum { FLAG_TEST_PATTERN = 1 };

#pragma pack(push, 4)
struct CamParams { float m22, m23, scale; int mode; };            // mode: -1 skip, 0 sky (always in front), 1 depth slice
struct FrameParams { int width, height, flags, camCount; CamParams cams[MAXCAM]; };
struct Stats { uint64_t begun, submitted, skipped, composites, compositeSkips; float vkGpuMs; int state, width, height, resizes; uint64_t gpuWaits, emptyFrames; float vkPrepMs; double subWaitMs; };
#pragma pack(pop)

// ---------------- logging ----------------
static std::mutex gLogM;
static std::string gLog;
static void logf(const char* fmt, ...) {
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    std::lock_guard<std::mutex> l(gLogM); gLog += b; gLog += '\n';
}
EXPORT int KSPVk_PopLog(char* buf, int len) {
    std::lock_guard<std::mutex> l(gLogM);
    int n = (int)gLog.size() < len - 1 ? (int)gLog.size() : len - 1;
    memcpy(buf, gLog.data(), n); buf[n] = 0; gLog.erase(0, n); return n;
}

// ---------------- Vulkan loader ----------------
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkCreateInstance vkCreateInstance;
#define VKFN(n) static PFN_##n n;
#define VK_INSTANCE_FNS(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroyInstance) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties)
#define VK_DEVICE_FNS(X) X(vkGetDeviceQueue) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkGetMemoryWin32HandlePropertiesKHR) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkImportSemaphoreWin32HandleKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBeginRendering) \
    X(vkCmdEndRendering) X(vkCmdClearAttachments) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkCreateFence) \
    X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkDestroyDevice) X(vkFreeCommandBuffers) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) \
    X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetFrontFace) \
    X(vkCmdSetDepthBias) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCmdDraw)
VK_INSTANCE_FNS(VKFN) VK_DEVICE_FNS(VKFN)

// ---------------- state ----------------
static std::atomic<int> gState{ST_NONE};
static ID3D11Device* gDev; static ID3D11Device1* gDev1; static ID3D11Device5* gDev5;
static ID3D11DeviceContext* gCtx; static ID3D11DeviceContext1* gCtx1; static ID3D11DeviceContext4* gCtx4;
static ID3DDeviceContextState* gOwnState;
static ID3D11VertexShader *gVS, *gVST; static ID3D11ComputeShader* gCS; static ID3D11PixelShader* gPS; static ID3D11Buffer* gCB;
enum { CM_TILES, CM_SYNC_ONLY, CM_NO_WAIT, CM_FULLSCREEN, CM_SKIP };   // diagnostics; CM_TILES is the production path
static std::atomic<int> gCompositeMode{CM_TILES};
static ID3D11BlendState* gBlend; static ID3D11DepthStencilState* gDSS; static ID3D11RasterizerState* gRS;
static bool gOwnStateReady;
static LUID gLuid;

static VkInstance gInst; static VkPhysicalDevice gPd; static VkDevice gVd; static VkQueue gQ; static uint32_t gQf;
static float gTsPeriod; static VkCommandPool gPool; static VkQueryPool gQp;

struct SharedFence { ID3D11Fence* f = nullptr; HANDLE h = nullptr; VkSemaphore s = VK_NULL_HANDLE; };
static SharedFence gFenceVk, gFenceD3D;
static HANDLE gWaitEvent;

struct Img { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; HANDLE h = nullptr;
             VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
struct ImageSet {
    int w = 0, h = 0; Img color[K], depth[K];
    int tw = 0, th = 0; ID3D11Texture2D* tiles = nullptr; ID3D11ShaderResourceView* tileSrv = nullptr; ID3D11UnorderedAccessView* tileUav = nullptr;
    ~ImageSet() {
        if (tileUav) tileUav->Release(); if (tileSrv) tileSrv->Release(); if (tiles) tiles->Release();
        for (Img* i : { color, depth }) for (int k = 0; k < K; k++) {
            if (i[k].view) vkDestroyImageView(gVd, i[k].view, nullptr);
            if (i[k].img) vkDestroyImage(gVd, i[k].img, nullptr);
            if (i[k].mem) vkFreeMemory(gVd, i[k].mem, nullptr);
            if (i[k].srv) i[k].srv->Release();
            if (i[k].tex) i[k].tex->Release();
            if (i[k].h) CloseHandle(i[k].h);
        }
    }
};
static std::shared_ptr<ImageSet> gSet;

struct Slot {
    FrameParams p{}; std::shared_ptr<ImageSet> set; std::atomic<uint64_t> submitted{0};
    VkCommandBuffer cb = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; bool tsPending = false, tsEnh = false;
    bool content = false;   // set at BeginFrame: anything for Vulkan to draw this frame (empty frames skip the GPU wait and all composites)
};
static Slot gSlot[K];

static std::thread gThread;
static std::mutex gQm; static std::condition_variable gQcv, gIdleCv, gSubCv;
static std::deque<uint64_t> gRenderQ, gGfxQ;
static bool gBusy, gQuit;
static uint64_t gFrameCounter;
static uint64_t gCurFrame; static bool gWaited;            // gfx-thread only
static Stats gStats;
static int gTimeouts;

static bool gSceneCaps, gDepthClamp, gAniso, gStorageIdx; static float gMaxAniso;   // gStorageIdx: Enhanced TAA / bloom / Hi-Z index storage-image arrays
enum { QPS = 12 };   // timestamps per slot: start, after prep, IBL, pre-scene, main pass, Hi-Z, AO / GI, TAA, exposure, bloom, tonemap, end (2..10 Enhanced only)
static uint64_t gProcessed;   // last frame the render thread finished with (under gQm)
static void fail(const char* why) { if (gState.exchange(ST_FAILED) != ST_FAILED) logf("FAILED: %s", why); }
#define VKCHECK(x) do { VkResult _r = (x); if (_r < 0) { logf("%s -> %d (line %d)", #x, (int)_r, __LINE__); return false; } } while (0)
#define HRCHECK(x) do { HRESULT _h = (x); if (FAILED(_h)) { logf("%s -> 0x%08lx (line %d)", #x, (unsigned long)_h, __LINE__); return false; } } while (0)

// ---------------- init ----------------
// Layered composite. Linear depth: +inf = nothing / galaxy only, > 0 local view (m), < 0 scaled view (-depth in scaled units).
// mode 0: galaxy layer (d = inf), 1: local view slice, 2: scaled view slice, 3: everything at the camera's near plane (debug pattern sky),
// 4: IVA pixels only (local depth tagged x1e20), into the InternalCamera after its depth clear so Unity's IVA leftovers depth-test against them.
static const char* kHlsl = R"(
Texture2D<float4> Col : register(t0);
Texture2D<float> Dep : register(t1);
Texture2D<uint4> Tiles : register(t2);           // per 16x16 tile, float bits: min / max positive depth, min / max |negative depth| (empty: max < min)
cbuffer P : register(b0) { float2 ratio; float m22, m23, scale, mode; float2 vpOrigin; float2 imgSize; };
float4 VS(uint id : SV_VertexID) : SV_Position { float2 t = float2((id << 1) & 2, id & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0, 1); }
float dec(float d) { return d >= 1e19 && d < 1e38 ? d * 1e-20 : d; }
bool slice(uint lo, uint hi) { return hi >= lo && m23 / (asfloat(lo) * scale) - m22 >= 0 && m23 / (asfloat(hi) * scale) - m22 <= 1; }   // z(dc) = m23/dc - m22, reversed Z
// One quad per tile; tiles with no content for this camera collapse to zero area.
float4 VST(uint vid : SV_VertexID, uint iid : SV_InstanceID) : SV_Position {
    uint tw, th; Tiles.GetDimensions(tw, th);
    uint2 t = uint2(iid % tw, iid / tw);
    uint4 r = Tiles.Load(int3(t, 0));
    bool vis = mode < 0.5 ? r.y == 0x7f800000 : mode < 1.5 ? slice(r.x, r.y) : mode < 2.5 ? slice(r.z, r.w) : mode > 3.5 ? r.y >= r.x : r.y >= r.x || r.w >= r.z;
    float2 px = min((t + float2(vid & 1, vid >> 1)) * 16, imgSize);
    return vis ? float4(px.x / imgSize.x * 2 - 1, 1 - px.y / imgSize.y * 2, 0, 1) : float4(0, 0, 0, 1);
}
#ifdef TILE_CS
RWTexture2D<uint4> TileOut : register(u0);
groupshared uint gMin, gMax, gMinN, gMaxN;
[numthreads(16, 16, 1)]
void CS(uint3 g : SV_GroupID, uint gi : SV_GroupIndex, uint3 d : SV_DispatchThreadID) {
    if (gi == 0) { gMin = gMinN = 0xffffffff; gMax = gMaxN = 0; }
    GroupMemoryBarrierWithGroupSync();
    if (all(d.xy < (uint2)imgSize)) {
        float z = dec(Dep.Load(int3(d.xy, 0)));
        if (Col.Load(int3(d.xy, 0)).a > 0 || z < 1e38) {
            uint v = asuint(abs(z));
            if (z > 0) { InterlockedMin(gMin, v); InterlockedMax(gMax, v); } else if (z < 0) { InterlockedMin(gMinN, v); InterlockedMax(gMaxN, v); }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) TileOut[g.xy] = uint4(gMin, gMax, gMinN, gMaxN);
}
#endif
float4 PS(float4 pos : SV_Position, out float depth : SV_Depth) : SV_Target {
    int3 p = int3((pos.xy - vpOrigin) * ratio, 0);
    float4 c = Col.Load(p);
    float d = Dep.Load(p);
    if (!(c.a > 0 || d < 1e38)) discard;
    bool iva = d >= 1e19 && d < 1e38;
    d = dec(d);
    if (mode > 3.5) {
        if (!iva) discard;
        float z = m23 / (d * scale) - m22;
        if (!(z >= 0 && z <= 1)) discard;
        depth = z;
    }
    else if (mode > 2.5) depth = 1;
    else if (mode < 0.5) { if (d < 1e38) discard; depth = 0; }   // galaxy: behind everything the camera still draws
    else {
        bool sc = mode > 1.5;
        if (sc ? !(d < 0) : !(d > 0)) discard;
        float z = m23 / (abs(d) * scale) - m22;
        if (!(z >= 0 && z <= 1)) discard;        // outside this camera's near/far (also catches inf/NaN)
        depth = max(0, z * 0.999996 - 2.4e-7);   // a hair farther (reversed Z): Unity decals lying exactly on our surfaces pass LEqual as in stock
    }
    return float4(c.rgb, 1);
}
)";

static bool initD3D(void* nativeTex) {
    ((ID3D11Resource*)nativeTex)->GetDevice(&gDev);
    if (!gDev) { logf("no D3D11 device"); return false; }
    HRCHECK(gDev->QueryInterface(__uuidof(ID3D11Device1), (void**)&gDev1));
    HRCHECK(gDev->QueryInterface(__uuidof(ID3D11Device5), (void**)&gDev5));
    gDev->GetImmediateContext(&gCtx);
    // Mesh/texture registration copies and maps on the main thread while Unity's render thread drives the same immediate context.
    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(gCtx->QueryInterface(__uuidof(ID3D11Multithread), (void**)&mt))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
    else logf("ID3D11Multithread unavailable: immediate context not protected");
    HRCHECK(gCtx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&gCtx1));
    HRCHECK(gCtx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&gCtx4));
    IDXGIDevice* dd; HRCHECK(gDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd));
    IDXGIAdapter* ad; HRESULT hr = dd->GetAdapter(&ad); dd->Release(); HRCHECK(hr);
    DXGI_ADAPTER_DESC desc; ad->GetDesc(&desc); ad->Release(); gLuid = desc.AdapterLuid;
    logf("D3D11 adapter: %ls (FL %x, flags %x)", desc.Description, gDev->GetFeatureLevel(), gDev->GetCreationFlags());

    D3D_FEATURE_LEVEL fl = gDev->GetFeatureLevel();
    UINT sf = (gDev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    HRCHECK(gDev1->CreateDeviceContextState(sf, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &gOwnState));

    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll"); if (!dc) { logf("d3dcompiler_47.dll missing"); return false; }
    auto compile = (pD3DCompile)GetProcAddress(dc, "D3DCompile");
    ID3DBlob *vs, *err = nullptr;
    if (FAILED(compile(kHlsl, strlen(kHlsl), "kspvk_composite", nullptr, nullptr, "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &err))) {
        logf("composite VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false;
    }
    HRCHECK(gDev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &gVS));
    vs->Release();
    static const D3D_SHADER_MACRO csDefs[2] = { { "TILE_CS", "1" }, {} };
    ID3DBlob *vst, *cs;
    if (FAILED(compile(kHlsl, strlen(kHlsl), "kspvk_composite", nullptr, nullptr, "VST", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vst, &err)) ||
        FAILED(compile(kHlsl, strlen(kHlsl), "kspvk_composite", csDefs, nullptr, "CS", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err))) {
        logf("tile shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false;
    }
    HRCHECK(gDev->CreateVertexShader(vst->GetBufferPointer(), vst->GetBufferSize(), nullptr, &gVST)); vst->Release();
    HRCHECK(gDev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &gCS)); cs->Release();
    ID3DBlob* ps;
    if (FAILED(compile(kHlsl, strlen(kHlsl), "kspvk_composite", nullptr, nullptr, "PS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &err))) {
        logf("composite PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false;
    }
    HRCHECK(gDev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &gPS));
    ps->Release();

    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRCHECK(gDev->CreateBuffer(&bd, nullptr, &gCB));
    D3D11_BLEND_DESC bl{}; auto& rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE; rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    HRCHECK(gDev->CreateBlendState(&bl, &gBlend));
    D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;   // reversed Z: respect content already in a non-cleared depth buffer
    HRCHECK(gDev->CreateDepthStencilState(&ds, &gDSS));
    D3D11_RASTERIZER_DESC rs{}; rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
    HRCHECK(gDev->CreateRasterizerState(&rs, &gRS));
    gWaitEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    return true;
}

static bool initVk() {
    HMODULE m = LoadLibraryA("vulkan-1.dll"); if (!m) { logf("vulkan-1.dll not found"); return false; }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(m, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_3; ai.pApplicationName = "KSPVk"; ai.pEngineName = "KSPVk";
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &ai;
    VKCHECK(vkCreateInstance(&ici, nullptr, &gInst));
#define LI(n) n = (PFN_##n)vkGetInstanceProcAddr(gInst, #n);
    VK_INSTANCE_FNS(LI)
    uint32_t n = 0; vkEnumeratePhysicalDevices(gInst, &n, nullptr); std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(gInst, &n, pds.data());
    VkPhysicalDeviceProperties2 chosen{};
    for (auto pd : pds) {
        VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext = &idp;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        if (idp.deviceLUIDValid && !memcmp(idp.deviceLUID, &gLuid, sizeof(LUID)) && p2.properties.apiVersion >= VK_API_VERSION_1_3) { gPd = pd; chosen = p2; }
    }
    if (!gPd) { logf("no Vulkan 1.3 device matching the D3D11 adapter LUID"); return false; }
    gTsPeriod = chosen.properties.limits.timestampPeriod;
    logf("Vulkan device: %s, api %u.%u.%u", chosen.properties.deviceName, VK_API_VERSION_MAJOR(chosen.properties.apiVersion),
         VK_API_VERSION_MINOR(chosen.properties.apiVersion), VK_API_VERSION_PATCH(chosen.properties.apiVersion));
    uint32_t qn; vkGetPhysicalDeviceQueueFamilyProperties(gPd, &qn, nullptr); std::vector<VkQueueFamilyProperties> qp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(gPd, &qn, qp.data());
    for (gQf = 0; gQf < qn && !(qp[gQf].queueFlags & VK_QUEUE_GRAPHICS_BIT); gQf++) {}
    const char* exts[] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
    float prio = 1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = gQf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features h12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; have.pNext = &h12;
    vkGetPhysicalDeviceFeatures2(gPd, &have);
    gSceneCaps = have.features.textureCompressionBC && have.features.shaderSampledImageArrayDynamicIndexing && have.features.independentBlend &&
                 h12.descriptorBindingSampledImageUpdateAfterBind && h12.descriptorBindingPartiallyBound;
    gDepthClamp = have.features.depthClamp; gAniso = have.features.samplerAnisotropy; gStorageIdx = have.features.shaderStorageImageArrayDynamicIndexing; gMaxAniso = chosen.properties.limits.maxSamplerAnisotropy;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; f12.pNext = &f13; f12.timelineSemaphore = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = h12.descriptorBindingSampledImageUpdateAfterBind; f12.descriptorBindingPartiallyBound = h12.descriptorBindingPartiallyBound;
    VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; want.pNext = &f12;
    want.features.textureCompressionBC = have.features.textureCompressionBC; want.features.depthClamp = gDepthClamp; want.features.samplerAnisotropy = gAniso;
    want.features.shaderSampledImageArrayDynamicIndexing = have.features.shaderSampledImageArrayDynamicIndexing; want.features.independentBlend = have.features.independentBlend;
    want.features.shaderStorageImageArrayDynamicIndexing = gStorageIdx;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &want; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = exts;
    VKCHECK(vkCreateDevice(gPd, &dci, nullptr, &gVd));
#define LD(n) n = (PFN_##n)vkGetDeviceProcAddr(gVd, #n);
    VK_DEVICE_FNS(LD)
    vkGetDeviceQueue(gVd, gQf, 0, &gQ);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = gQf;
    VKCHECK(vkCreateCommandPool(gVd, &pci, nullptr, &gPool));
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = QPS * K;
    VKCHECK(vkCreateQueryPool(gVd, &qpi, nullptr, &gQp));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = gPool; cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (Slot& s : gSlot) { VKCHECK(vkAllocateCommandBuffers(gVd, &cai, &s.cb)); VKCHECK(vkCreateFence(gVd, &fci, nullptr, &s.fence)); }
    return true;
}

static bool makeFence(SharedFence& sf) {
    HRCHECK(gDev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&sf.f));
    HRCHECK(sf.f->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sf.h));
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sci.pNext = &tci;
    VKCHECK(vkCreateSemaphore(gVd, &sci, nullptr, &sf.s));
    VkImportSemaphoreWin32HandleInfoKHR isi{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    isi.semaphore = sf.s; isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT; isi.handle = sf.h;
    VKCHECK(vkImportSemaphoreWin32HandleKHR(gVd, &isi));
    return true;
}

static bool makeImg(Img& im, DXGI_FORMAT dxf, VkFormat vkf, int w, int h) {
    D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = dxf; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    HRCHECK(gDev->CreateTexture2D(&td, nullptr, &im.tex));
    HRCHECK(gDev->CreateShaderResourceView(im.tex, nullptr, &im.srv));
    IDXGIResource1* r; HRCHECK(im.tex->QueryInterface(__uuidof(IDXGIResource1), (void**)&r));
    HRESULT hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &im.h); r->Release(); HRCHECK(hr);
    VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ici.pNext = &emi; ici.imageType = VK_IMAGE_TYPE_2D; ici.format = vkf;
    ici.extent = { (uint32_t)w, (uint32_t)h, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VKCHECK(vkCreateImage(gVd, &ici, nullptr, &im.img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(gVd, im.img, &mr);
    VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    VKCHECK(vkGetMemoryWin32HandlePropertiesKHR(gVd, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, im.h, &hp));
    uint32_t bits = mr.memoryTypeBits & hp.memoryTypeBits, mt = 0; while (mt < 32 && !(bits & (1u << mt))) mt++;
    if (mt == 32) { logf("no memory type for shared image"); return false; }
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; ded.image = im.img;
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR}; imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT; imp.handle = im.h;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.pNext = &imp; mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    VKCHECK(vkAllocateMemory(gVd, &mai, nullptr, &im.mem));
    VKCHECK(vkBindImageMemory(gVd, im.img, im.mem, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vci.image = im.img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = vkf;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VKCHECK(vkCreateImageView(gVd, &vci, nullptr, &im.view));
    return true;
}

static std::shared_ptr<ImageSet> makeSet(int w, int h) {
    auto s = std::make_shared<ImageSet>(); s->w = w; s->h = h;
    for (int k = 0; k < K; k++)
        if (!makeImg(s->color[k], DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, w, h) ||
            !makeImg(s->depth[k], DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, w, h)) return nullptr;
    s->tw = (w + 15) / 16; s->th = (h + 15) / 16;
    D3D11_TEXTURE2D_DESC td{}; td.Width = s->tw; td.Height = s->th; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_UINT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(gDev->CreateTexture2D(&td, nullptr, &s->tiles)) || FAILED(gDev->CreateShaderResourceView(s->tiles, nullptr, &s->tileSrv)) ||
        FAILED(gDev->CreateUnorderedAccessView(s->tiles, nullptr, &s->tileUav))) return nullptr;
    return s;
}

#include "scene.inl"
#include "upscale.inl"
#include "enhanced.inl"

// ---------------- render thread ----------------
static bool waitD3D(uint64_t v, DWORD ms) {
    if (gFenceD3D.f->GetCompletedValue() >= v) return true;
    gFenceD3D.f->SetEventOnCompletion(v, gWaitEvent);
    return WaitForSingleObject(gWaitEvent, ms) == WAIT_OBJECT_0 || gFenceD3D.f->GetCompletedValue() >= v;
}

static void barrier(VkCommandBuffer cb, VkImage* imgs, int n, VkImageLayout from, VkImageLayout to, uint32_t srcQf, uint32_t dstQf,
                    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, VkAccessFlags srcA, VkAccessFlags dstA) {
    VkImageMemoryBarrier b[2]{};
    for (int i = 0; i < n; i++) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b[i].image = imgs[i]; b[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b[i].oldLayout = from; b[i].newLayout = to; b[i].srcQueueFamilyIndex = srcQf; b[i].dstQueueFamilyIndex = dstQf;
        b[i].srcAccessMask = srcA; b[i].dstAccessMask = dstA;
    }
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, n, b);
}

static bool renderFrame(uint64_t N, Slot& s) {
    if (gState != ST_OK || !s.set) return false;
    if (N > K && !waitD3D(N - K, 1000)) {
        logf("timeout waiting for D3D to release frame %llu", (unsigned long long)(N - K));
        if (++gTimeouts >= 3) fail("repeated D3D fence timeouts");
        return false;
    }
    gTimeouts = 0;
    VKCHECK(vkWaitForFences(gVd, 1, &s.fence, VK_TRUE, 1000000000ull));
    VKCHECK(vkResetFences(gVd, 1, &s.fence));
    int si = (int)(&s - gSlot);
    if (s.tsPending) {
        uint64_t ts[QPS];
        bool ok = s.tsEnh ? vkGetQueryPoolResults(gVd, gQp, QPS * si, QPS, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS
                          : vkGetQueryPoolResults(gVd, gQp, QPS * si, 2, 16, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                            vkGetQueryPoolResults(gVd, gQp, QPS * si + QPS - 1, 1, 8, ts + QPS - 1, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
        if (ok) {
            gStats.vkGpuMs = (float)((ts[QPS - 1] - ts[0]) * gTsPeriod / 1e6); gStats.vkPrepMs = (float)((ts[1] - ts[0]) * gTsPeriod / 1e6);
            if (s.tsEnh) enhTimes(ts + 1);
        }
    }
    SlotScene& ss = gSS[si];
    bool uploads = gSceneOK && (sceneHasUploads() || ss.texApplied < gTexReady.size());
    if (!s.content && !uploads) {   // nothing to draw: only advance the timeline so fence bookkeeping stays uniform
        VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO}; ts.signalSemaphoreValueCount = 1; ts.pSignalSemaphoreValues = &N;
        VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO}; sub.pNext = &ts; sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &gFenceVk.s;
        VKCHECK(vkQueueSubmit(gQ, 1, &sub, s.fence));
        s.tsPending = false;
        return true;
    }
    ImageSet& set = *s.set;
    VkImage imgs[2] = { set.color[si].img, set.depth[si].img };
    VkCommandBuffer cb = s.cb;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(cb, &bi));
    const uint32_t q = QPS * si;
    vkCmdResetQueryPool(cb, gQp, q, QPS);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gQp, q);
    sceneUploads(cb, ss);
    if (!s.content) {   // uploads only: the shared images are untouched and D3D skips this frame's composites
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gQp, q + 1);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gQp, q + QPS - 1);
        VKCHECK(vkEndCommandBuffer(cb));
        VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO}; ts.signalSemaphoreValueCount = 1; ts.pSignalSemaphoreValues = &N;
        VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO}; sub.pNext = &ts; sub.commandBufferCount = 1; sub.pCommandBuffers = &cb;
        sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &gFenceVk.s;
        VKCHECK(vkQueueSubmit(gQ, 1, &sub, s.fence));
        s.tsPending = true; s.tsEnh = false;
        return true;
    }
    upScale(set);
    gEF = enhBegin(si, set);
    bool scene = scenePrepare(cb, ss, gRW, gRH);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gQp, q + 1);
    if (gEF) {
        if (scene && (gEF & EF_IBL)) iblRecord(cb, ss);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gQp, q + 2);
        if (scene) enhPreScene(cb, ss);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gQp, q + 3);
    }
    barrier(cb, imgs, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (gUp) upBegin(cb);
    if (gEF) enhPreMain(cb);
    VkRenderingAttachmentInfo att[6]{};
    for (int i = 0; i < 2; i++) {
        att[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; att[i].imageView = i ? (gUp ? gU.lz.view : set.depth[si].view) : (gUp ? gU.col.view : set.color[si].view);
        att[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; att[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    att[1].clearValue.color.float32[0] = INFINITY;                  // empty = infinitely far
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = { {0, 0}, {(uint32_t)gRW, (uint32_t)gRH} }; ri.layerCount = 1;
    ri.colorAttachmentCount = 2; ri.pColorAttachments = att;
    if (gEF) { enhAttachments(att); ri.colorAttachmentCount = 6; }
    VkRenderingAttachmentInfo za{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (scene) {   // reversed-Z hardware depth, cleared to 0 (infinitely far)
        za.imageView = gZ.view; za.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; za.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        za.storeOp = gEF ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;   // Enhanced: the pass may be split around the ocean
        ri.pDepthAttachment = &za;
    }
    vkCmdBeginRendering(cb, &ri);
    gMainRI = &ri; gMainDepth = gUp ? gU.lz.img : set.depth[si].img;
    if (scene) sceneDraw(cb, ss, gRW, gRH);
    gMainRI = nullptr;
    if (s.p.flags & FLAG_TEST_PATTERN) {
        // Depth-hand-off test: four opaque screen rects at 3 m, 30 m, 300 m and 3000 km linear view depth.
        static const float depthM[4] = { 3, 30, 300, 3e6f }, col[4][3] = { {1, .15f, .15f}, {.15f, 1, .15f}, {.2f, .4f, 1}, {1, 1, .15f} };
        for (int i = 0; i < 4; i++) {
            VkClearAttachment ca[2]{}; ca[0].aspectMask = ca[1].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[1].colorAttachment = 1;
            memcpy(ca[0].clearValue.color.float32, col[i], 12); ca[0].clearValue.color.float32[3] = 1;
            ca[1].clearValue.color.float32[0] = depthM[i];
            VkClearRect r{}; r.layerCount = 1;
            r.rect = { { (int32_t)(gRW * (0.06f + 0.235f * i)), (int32_t)(gRH * 0.30f) }, { (uint32_t)(gRW * 0.18f), (uint32_t)(gRH * 0.30f) } };
            vkCmdClearAttachments(cb, 2, ca, 1, &r);
        }
    }
    vkCmdEndRendering(cb);
    if (gEF) {   // post passes leave the colour as an attachment and the depth in GENERAL
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gQp, q + 4);
        enhPost(cb, si, set, q + 5);
    }
    if (gUp) upDraw(cb, si, set, (gEF & EF_TAA) != 0, gEF ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);   // leaves both shared images as attachments
    if (gEF && !gUp) {
        barrier(cb, imgs, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, gQf, VK_QUEUE_FAMILY_EXTERNAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
        barrier(cb, imgs + 1, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, gQf, VK_QUEUE_FAMILY_EXTERNAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0);
    } else
        barrier(cb, imgs, 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, gQf, VK_QUEUE_FAMILY_EXTERNAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gQp, q + QPS - 1);
    VKCHECK(vkEndCommandBuffer(cb));
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO}; ts.signalSemaphoreValueCount = 1; ts.pSignalSemaphoreValues = &N;
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO}; sub.pNext = &ts; sub.commandBufferCount = 1; sub.pCommandBuffers = &cb;
    sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &gFenceVk.s;
    VKCHECK(vkQueueSubmit(gQ, 1, &sub, s.fence));
    s.tsPending = true; s.tsEnh = gEF != 0;
    gEF = 0;
    return true;
}

static void renderLoop() {
    for (;;) {
        uint64_t N;
        {
            std::unique_lock<std::mutex> l(gQm);
            gQcv.wait(l, [] { return gQuit || !gRenderQ.empty(); });
            if (gQuit) return;
            N = gRenderQ.front(); gRenderQ.pop_front(); gBusy = true;
        }
        Slot& s = gSlot[N % K];
        bool ok = renderFrame(N, s);
        if (!ok && gState == ST_OK && gTimeouts == 0) fail("Vulkan frame recording/submission failed");
        {
            std::lock_guard<std::mutex> l(gQm);
            if (ok) { s.submitted = N; gStats.submitted++; } else gStats.skipped++;
            gBusy = false; gProcessed = N;
        }
        gIdleCv.notify_all(); gSubCv.notify_all();
    }
}

static void drainRenderThread() {
    std::unique_lock<std::mutex> l(gQm);
    gIdleCv.wait(l, [] { return gRenderQ.empty() && !gBusy; });
}

// ---------------- exports: main thread ----------------
EXPORT int KSPVk_Init(void* nativeTex) {
    if (gState == ST_OK) return 1;
    if (gState == ST_FAILED || !nativeTex) return -1;
    if (!initD3D(nativeTex) || !initVk() || !makeFence(gFenceVk) || !makeFence(gFenceD3D)) { fail("init"); return -2; }
    gState = ST_OK;
    gSceneOK = initScene(gSceneCaps);
    gThread = std::thread(renderLoop);
    logf("init OK (K=%d slots)", K);
    return 1;
}

// Returns the frame number (>0) if a KSPVk frame was started; the caller then issues EV_FRAME. 0 = skipped, <0 = failed.
EXPORT long long KSPVk_BeginFrame2(const FrameParams* p, const SceneIn* sc) {
    if (gState != ST_OK) return -1;
    if (p->width <= 0 || p->height <= 0) return 0;
    if (!gSet || gSet->w != p->width || gSet->h != p->height) {
        drainRenderThread();
        if (gVd) vkQueueWaitIdle(gQ);
        gSet = makeSet(p->width, p->height);
        if (!gSet) { fail("shared image allocation"); return -1; }
        gStats.resizes++; memset(gCamLogged, 0, sizeof gCamLogged); logf("shared images %dx%d", p->width, p->height);
    }
    uint64_t N = gFrameCounter + 1;
    if (N > K) {   // the render thread must be done with this slot's previous frame (N-K) before its inputs are overwritten
        std::unique_lock<std::mutex> l(gQm);
        if (!gIdleCv.wait_for(l, std::chrono::seconds(2), [&] { return gProcessed >= N - K || gState != ST_OK; }) || gState != ST_OK) {
            fail("render thread stalled"); return -1;
        }
    }
    gFrameCounter = N;
    Slot& s = gSlot[N % K];
    s.p = *p; if (s.p.camCount > MAXCAM) s.p.camCount = MAXCAM;
    s.set = gSet;
    sceneCopy((int)(N % K), sc);
    s.content = (s.p.flags & FLAG_TEST_PATTERN) != 0 || gSS[N % K].h.drawCount > 0;
    if (!s.content) gStats.emptyFrames++;
    {
        std::lock_guard<std::mutex> l(gQm);
        gRenderQ.push_back(N); gGfxQ.push_back(N); gStats.begun++;
        while (gGfxQ.size() > 8) gGfxQ.pop_front();      // events lost (should not happen); never grow unbounded
    }
    gQcv.notify_one();
    return (long long)N;
}
EXPORT long long KSPVk_BeginFrame(const FrameParams* p) { return KSPVk_BeginFrame2(p, nullptr); }

// ---------------- gfx-thread events ----------------
static void ensureOwnState() {
    if (gOwnStateReady) return;
    ID3DDeviceContextState* prev; gCtx1->SwapDeviceContextState(gOwnState, &prev);
    gCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gCtx->VSSetShader(gVS, nullptr, 0); gCtx->PSSetShader(gPS, nullptr, 0); gCtx->PSSetConstantBuffers(0, 1, &gCB);
    float bf[4] = {}; gCtx->OMSetBlendState(gBlend, bf, 0xffffffff); gCtx->OMSetDepthStencilState(gDSS, 0); gCtx->RSSetState(gRS);
    gCtx1->SwapDeviceContextState(prev, nullptr); prev->Release();
    gOwnStateReady = true;
}

// Once per frame, after the fence wait: reduce the shared colour/depth to per-tile depth ranges (own context state).
static void classifyTiles(Slot& s) {
    ensureOwnState();
    int si = (int)(gCurFrame % K);
    float cb[12] = {}; cb[8] = (float)s.set->w; cb[9] = (float)s.set->h;
    ID3DDeviceContextState* prev; gCtx1->SwapDeviceContextState(gOwnState, &prev);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(gCtx->Map(gCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, cb, sizeof cb); gCtx->Unmap(gCB, 0); }
    ID3D11ShaderResourceView* srv[2] = { s.set->color[si].srv, s.set->depth[si].srv };
    gCtx->CSSetShader(gCS, nullptr, 0); gCtx->CSSetConstantBuffers(0, 1, &gCB); gCtx->CSSetShaderResources(0, 2, srv);
    gCtx->CSSetUnorderedAccessViews(0, 1, &s.set->tileUav, nullptr);
    gCtx->Dispatch(s.set->tw, s.set->th, 1);
    ID3D11ShaderResourceView* nul[2] = {}; ID3D11UnorderedAccessView* nuav = nullptr;
    gCtx->CSSetShaderResources(0, 2, nul); gCtx->CSSetUnorderedAccessViews(0, 1, &nuav, nullptr);
    gCtx1->SwapDeviceContextState(prev, nullptr); prev->Release();
}

static void composite(int cam) {
    if (!gCurFrame || gState != ST_OK) return;
    Slot& s = gSlot[gCurFrame % K];
    if (cam >= s.p.camCount || s.p.cams[cam].mode < 0 || !s.set || !s.content) return;
    if (s.submitted.load() != gCurFrame) {
        std::unique_lock<std::mutex> l(gQm);
        uint64_t want = gCurFrame;
        auto t0 = std::chrono::steady_clock::now();
        struct W { std::chrono::steady_clock::time_point t; ~W() { gStats.subWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); } } w{t0};
        if (!gSubCv.wait_for(l, std::chrono::milliseconds(30), [&] { return s.submitted.load() == want || gState != ST_OK; }) || gState != ST_OK) {
            gStats.compositeSkips++; return;
        }
    }
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    gCtx->OMGetRenderTargets(1, &rtv, &dsv);
    D3D11_VIEWPORT vp{}; UINT nvp = 1; gCtx->RSGetViewports(&nvp, &vp);
    if (!rtv || !nvp || vp.Width <= 0) { if (rtv) rtv->Release(); if (dsv) dsv->Release(); gStats.compositeSkips++; return; }
    if (!gCamLogged[cam]) {
        gCamLogged[cam] = true;
        ID3D11Resource* r; rtv->GetResource(&r); D3D11_TEXTURE2D_DESC d{}; ((ID3D11Texture2D*)r)->GetDesc(&d); r->Release();
        logf("cam %d composite target %ux%u fmt %d bind %x misc %x, dsv %d, viewport %.0f,%.0f %.0fx%.0f", cam, d.Width, d.Height, d.Format,
             d.BindFlags, d.MiscFlags, dsv != nullptr, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
    }
    int cm = gCompositeMode;
    if (cm == CM_SKIP) { rtv->Release(); if (dsv) dsv->Release(); return; }
    if (!gWaited) {
        gWaited = true;
        if (cm != CM_NO_WAIT) { gCtx4->Wait(gFenceVk.f, gCurFrame); gStats.gpuWaits++; }
        if (cm == CM_TILES || cm == CM_NO_WAIT) classifyTiles(s);
    }
    if (cm == CM_SYNC_ONLY) { rtv->Release(); if (dsv) dsv->Release(); gStats.composites++; return; }
    ensureOwnState();
    const CamParams& c = s.p.cams[cam];
    int si = (int)(gCurFrame % K);
    float cb[12] = { s.set->w / vp.Width, s.set->h / vp.Height, c.m22, c.m23, c.scale, (float)c.mode, vp.TopLeftX, vp.TopLeftY, (float)s.set->w, (float)s.set->h };
    ID3DDeviceContextState* prev; gCtx1->SwapDeviceContextState(gOwnState, &prev);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(gCtx->Map(gCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, cb, sizeof cb); gCtx->Unmap(gCB, 0); }
    ID3D11ShaderResourceView* srv[2] = { s.set->color[si].srv, s.set->depth[si].srv };
    gCtx->OMSetRenderTargets(1, &rtv, dsv);
    gCtx->RSSetViewports(1, &vp);
    gCtx->PSSetShaderResources(0, 2, srv);
    if (cm != CM_FULLSCREEN) {
        gCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP); gCtx->VSSetShader(gVST, nullptr, 0);
        gCtx->VSSetConstantBuffers(0, 1, &gCB); gCtx->VSSetShaderResources(2, 1, &s.set->tileSrv);
        gCtx->DrawInstanced(4, s.set->tw * s.set->th, 0, 0);
        ID3D11ShaderResourceView* nul = nullptr; gCtx->VSSetShaderResources(2, 1, &nul);
        gCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); gCtx->VSSetShader(gVS, nullptr, 0);
    } else gCtx->Draw(3, 0);
    ID3D11ShaderResourceView* nul[2] = {}; gCtx->PSSetShaderResources(0, 2, nul);
    gCtx->OMSetRenderTargets(0, nullptr, nullptr);
    gCtx1->SwapDeviceContextState(prev, nullptr); prev->Release();
    rtv->Release(); if (dsv) dsv->Release();
    gStats.composites++;
}

static void __stdcall OnEvent(int id) {
    int kind = (id >> 24) & 0xff, arg = id & 0xffff;
    if (kind == EV_FRAME) {
        std::lock_guard<std::mutex> l(gQm);
        gCurFrame = gGfxQ.empty() ? 0 : gGfxQ.front(); if (!gGfxQ.empty()) gGfxQ.pop_front();
        gWaited = false;
        if (gState == ST_OK && gDev->GetDeviceRemovedReason() != S_OK) fail("D3D11 device removed");
    } else if (kind == EV_COMPOSITE) composite(arg);
    else if (kind == EV_END && gCurFrame) {
        if (gCtx4 && gFenceD3D.f) gCtx4->Signal(gFenceD3D.f, gCurFrame);
        gCurFrame = 0;
    }
}

EXPORT void KSPVk_SetCompositeMode(int m) { gCompositeMode = m; logf("composite mode %d", m); }

EXPORT void* KSPVk_GetEventFunc() { return (void*)&OnEvent; }

EXPORT void KSPVk_GetStats(Stats* out) {
    std::lock_guard<std::mutex> l(gQm);
    *out = gStats; out->state = gState; out->width = gSet ? gSet->w : 0; out->height = gSet ? gSet->h : 0;
}

EXPORT void KSPVk_Shutdown() {
    if (gThread.joinable()) { { std::lock_guard<std::mutex> l(gQm); gQuit = true; } gQcv.notify_all(); gThread.join(); }
    if (gVd) {
        vkDeviceWaitIdle(gVd);
        enhShutdown(); upShutdown(); sceneShutdown();
        for (Slot& s : gSlot) { s.set.reset(); if (s.fence) vkDestroyFence(gVd, s.fence, nullptr); }
        gSet.reset();
        for (SharedFence* f : { &gFenceVk, &gFenceD3D }) if (f->s) vkDestroySemaphore(gVd, f->s, nullptr);
        vkDestroyQueryPool(gVd, gQp, nullptr); vkDestroyCommandPool(gVd, gPool, nullptr); vkDestroyDevice(gVd, nullptr); gVd = VK_NULL_HANDLE;
    }
    if (gInst) { vkDestroyInstance(gInst, nullptr); gInst = VK_NULL_HANDLE; }
    if (gState == ST_OK) gState = ST_NONE;
}
