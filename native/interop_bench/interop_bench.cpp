// Phase 0: D3D11 <-> Vulkan shared image + shared fence prototype and cost benchmark.
// D3D11 owns the shared textures/fences (as Unity would); Vulkan imports them, renders, D3D11 consumes.
// Usage: interop_bench [width] [height] [frames] [adapterIndex|-1=auto]
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

#define CHECK(x) do { auto _r = (x); if ((long long)_r < 0) { printf("FAIL %s -> 0x%llx (line %d)\n", #x, (unsigned long long)(long long)_r, __LINE__); exit(1); } } while (0)

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define VKFN(n) static PFN_##n n;
#define VK_INSTANCE_FNS(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroyInstance) X(vkGetPhysicalDeviceFeatures2) X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceMemoryProperties)
#define VK_DEVICE_FNS(X) X(vkGetDeviceQueue) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkBindImageMemory) \
    X(vkGetMemoryWin32HandlePropertiesKHR) X(vkCreateSemaphore) X(vkImportSemaphoreWin32HandleKHR) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) \
    X(vkQueueSubmit) X(vkWaitSemaphores) X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) \
    X(vkDeviceWaitIdle) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkResetCommandBuffer) X(vkDestroyDevice) \
    X(vkGetSemaphoreCounterValue)
VK_INSTANCE_FNS(VKFN) VK_DEVICE_FNS(VKFN)
static PFN_vkCreateInstance vkCreateInstance;

static double qpcMs() { static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / f.QuadPart; }

struct Fmt { const char* name; DXGI_FORMAT dx; VkFormat vk; int bpp; };
static const Fmt FMTS[] = { {"RGBA8", DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4},
                            {"RGBA16F", DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8},
                            {"R32F(depth)", DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, 4} };

struct Ctx {
    ID3D11Device5* dev; ID3D11DeviceContext4* ctx; LUID luid; char gpuName[128];
    VkInstance inst; VkPhysicalDevice pd; VkDevice vd; VkQueue q; uint32_t qf; float tsPeriod;
    VkCommandPool pool;
} C;

struct Shared {   // one shared surface (slot)
    ID3D11Texture2D* tex; ID3D11Texture2D* local; ID3D11Texture2D* staging; HANDLE h;
    VkImage img; VkDeviceMemory mem;
};

static void initD3D(int want) {
    IDXGIFactory1* fac; CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac));
    IDXGIAdapter1* best = nullptr; SIZE_T bestMem = 0;
    for (UINT i = 0;; i++) {
        IDXGIAdapter1* a; if (fac->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        printf("  adapter %u: %ls (vram %zu MB, shared %zu MB)%s\n", i, d.Description, d.DedicatedVideoMemory >> 20, d.SharedSystemMemory >> 20,
               (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " [software]" : "");
        bool pick = want >= 0 ? (int)i == want : (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory + d.SharedSystemMemory / 4 > bestMem);
        if (pick && (want >= 0 || !best || true)) { if (want < 0) bestMem = d.DedicatedVideoMemory + d.SharedSystemMemory / 4; if (best) best->Release(); best = a; } else a->Release();
    }
    DXGI_ADAPTER_DESC1 d; best->GetDesc1(&d); C.luid = d.AdapterLuid; snprintf(C.gpuName, sizeof C.gpuName, "%ls", d.Description);
    ID3D11Device* dev; ID3D11DeviceContext* ctx; D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1 };
    CHECK(D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    CHECK(dev->QueryInterface(__uuidof(ID3D11Device5), (void**)&C.dev));
    CHECK(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&C.ctx));
    printf("D3D11 on: %s\n", C.gpuName);
}

static void initVk() {
    HMODULE m = LoadLibraryA("vulkan-1.dll"); if (!m) { printf("no vulkan-1.dll\n"); exit(1); }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(m, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_3; ai.pApplicationName = "KSPVk interop bench";
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &ai;
    CHECK(vkCreateInstance(&ici, nullptr, &C.inst));
#define LI(n) n = (PFN_##n)vkGetInstanceProcAddr(C.inst, #n);
    VK_INSTANCE_FNS(LI)
    uint32_t n = 0; vkEnumeratePhysicalDevices(C.inst, &n, nullptr); std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(C.inst, &n, pds.data());
    for (auto pd : pds) {
        VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext = &idp;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        bool match = idp.deviceLUIDValid && !memcmp(idp.deviceLUID, &C.luid, sizeof(LUID));
        printf("  vk device: %s api %u.%u.%u luid-match=%d\n", p2.properties.deviceName, VK_API_VERSION_MAJOR(p2.properties.apiVersion),
               VK_API_VERSION_MINOR(p2.properties.apiVersion), VK_API_VERSION_PATCH(p2.properties.apiVersion), match);
        if (match) { C.pd = pd; C.tsPeriod = p2.properties.limits.timestampPeriod; }
    }
    if (!C.pd) { printf("FAIL: no Vulkan device with matching LUID\n"); exit(1); }
    uint32_t qn; vkGetPhysicalDeviceQueueFamilyProperties(C.pd, &qn, nullptr); std::vector<VkQueueFamilyProperties> qp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(C.pd, &qn, qp.data());
    for (C.qf = 0; C.qf < qn && !(qp[C.qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); C.qf++) {}
    const char* exts[] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
    float prio = 1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = C.qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; f12.timelineSemaphore = VK_TRUE; f12.hostQueryReset = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &f12; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = exts;
    CHECK(vkCreateDevice(C.pd, &dci, nullptr, &C.vd));
#define LD(n) n = (PFN_##n)vkGetDeviceProcAddr(C.vd, #n);
    VK_DEVICE_FNS(LD)
    vkGetDeviceQueue(C.vd, C.qf, 0, &C.q);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = C.qf;
    CHECK(vkCreateCommandPool(C.vd, &pci, nullptr, &C.pool));
}

static Shared makeShared(const Fmt& f, int w, int h) {
    Shared s{};
    D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f.dx; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    CHECK(C.dev->CreateTexture2D(&td, nullptr, &s.tex));
    IDXGIResource1* r; CHECK(s.tex->QueryInterface(__uuidof(IDXGIResource1), (void**)&r));
    CHECK(r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &s.h)); r->Release();
    td.MiscFlags = 0; CHECK(C.dev->CreateTexture2D(&td, nullptr, &s.local));                       // "backbuffer" stand-in
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.Width = td.Height = 1;
    CHECK(C.dev->CreateTexture2D(&td, nullptr, &s.staging));

    VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ici.pNext = &emi; ici.imageType = VK_IMAGE_TYPE_2D; ici.format = f.vk;
    ici.extent = { (uint32_t)w, (uint32_t)h, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL; ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    CHECK(vkCreateImage(C.vd, &ici, nullptr, &s.img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(C.vd, s.img, &mr);
    VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    CHECK(vkGetMemoryWin32HandlePropertiesKHR(C.vd, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, s.h, &hp));
    uint32_t bits = mr.memoryTypeBits & hp.memoryTypeBits; uint32_t mt = 0; while (mt < 32 && !(bits & (1u << mt))) mt++;
    if (mt == 32) { printf("FAIL: no compatible memory type (img %x, handle %x)\n", mr.memoryTypeBits, hp.memoryTypeBits); exit(1); }
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; ded.image = s.img;
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR}; imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT; imp.handle = s.h;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.pNext = &imp; mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    CHECK(vkAllocateMemory(C.vd, &mai, nullptr, &s.mem));
    CHECK(vkBindImageMemory(C.vd, s.img, s.mem, 0));
    return s;
}

struct SharedFence { ID3D11Fence* f; HANDLE h; VkSemaphore s; };
static SharedFence makeFence() {
    SharedFence sf{};
    CHECK(C.dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&sf.f));
    CHECK(sf.f->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sf.h));
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sci.pNext = &tci;
    CHECK(vkCreateSemaphore(C.vd, &sci, nullptr, &sf.s));
    VkImportSemaphoreWin32HandleInfoKHR isi{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    isi.semaphore = sf.s; isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT; isi.handle = sf.h;
    CHECK(vkImportSemaphoreWin32HandleKHR(C.vd, &isi));
    return sf;
}

static void colorFor(uint32_t i, float c[4]) { c[0] = (i % 7) / 6.f; c[1] = (i % 5) / 4.f; c[2] = (i % 3) / 2.f; c[3] = 1; }

// Record: discard-acquire, clear to frame colour, release to external. Timestamps around the work.
static void recordVk(VkCommandBuffer cb, VkImage img, VkQueryPool qp, uint32_t qbase, uint32_t frame, int reps) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    vkCmdResetQueryPool(cb, qp, qbase, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, qbase);
    VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = img; b.subresourceRange = rng;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkClearColorValue cv; colorFor(frame, cv.float32);
    for (int r = 0; r < reps; r++) vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rng);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL; b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
    b.srcQueueFamilyIndex = C.qf; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, qbase + 1);
    vkEndCommandBuffer(cb);
}

static void submitVk(VkCommandBuffer cb, VkSemaphore waitS, uint64_t waitV, VkSemaphore sigS, uint64_t sigV) {
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = waitS ? 1 : 0; ts.pWaitSemaphoreValues = &waitV; ts.signalSemaphoreValueCount = 1; ts.pSignalSemaphoreValues = &sigV;
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.pNext = &ts; si.waitSemaphoreCount = waitS ? 1 : 0; si.pWaitSemaphores = &waitS; si.pWaitDstStageMask = &ws;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sigS;
    CHECK(vkQueueSubmit(C.q, 1, &si, VK_NULL_HANDLE));
}

struct D3DTimer { ID3D11Query *dj, *t0, *t1, *ev; };
static D3DTimer makeTimer() {
    D3DTimer t; D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; C.dev->CreateQuery(&qd, &t.dj);
    qd.Query = D3D11_QUERY_TIMESTAMP; C.dev->CreateQuery(&qd, &t.t0); C.dev->CreateQuery(&qd, &t.t1);
    qd.Query = D3D11_QUERY_EVENT; C.dev->CreateQuery(&qd, &t.ev); return t;
}
static double readTimer(D3DTimer& t) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
    while (C.ctx->GetData(t.dj, &dj, sizeof dj, 0) == S_FALSE) {}
    C.ctx->GetData(t.t0, &a, sizeof a, 0); C.ctx->GetData(t.t1, &b, sizeof b, 0);
    return dj.Disjoint ? -1 : (b - a) * 1000.0 / dj.Frequency;
}

struct Stats { std::vector<double> v; void add(double x) { if (x >= 0) v.push_back(x); }
    double pct(double p) { if (v.empty()) return -1; auto w = v; std::sort(w.begin(), w.end()); return w[std::min(w.size() - 1, (size_t)(p * w.size()))]; }
    double mean() { double s = 0; for (double x : v) s += x; return v.empty() ? -1 : s / v.size(); } };

int main(int argc, char** argv) {
    int W = argc > 1 ? atoi(argv[1]) : 2560, H = argc > 2 ? atoi(argv[2]) : 1440, N = argc > 3 ? atoi(argv[3]) : 600, A = argc > 4 ? atoi(argv[4]) : -1;
    printf("== KSPVk interop bench %dx%d, %d frames\n", W, H, N);
    initD3D(A); initVk();
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; vkGetPhysicalDeviceProperties2(C.pd, &p2);
    printf("Vulkan on: %s (timestampPeriod %.2f ns)\n", p2.properties.deviceName, C.tsPeriod);

    VkCommandBuffer cbs[4]; VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = C.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 4; CHECK(vkAllocateCommandBuffers(C.vd, &cai, cbs));
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = 8;
    VkQueryPool qp; CHECK(vkCreateQueryPool(C.vd, &qpi, nullptr, &qp));
    VkFence vf[2]; VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(C.vd, &fci, nullptr, &vf[0]); vkCreateFence(C.vd, &fci, nullptr, &vf[1]);

    bool allOk = true;
    for (const Fmt& f : FMTS) {
        printf("\n-- format %s (%.1f MB/surface)\n", f.name, (double)W * H * f.bpp / 1048576.0);
        Shared s[2] = { makeShared(f, W, H), makeShared(f, W, H) };
        SharedFence ready = makeFence(), done = makeFence();   // ready: VK->D3D, done: D3D->VK (both monotonic per frame)
        D3DTimer tm = makeTimer();

        // 1) Correctness: serialized frames, D3D reads back the pixel Vulkan cleared.
        int bad = 0;
        for (uint32_t i = 1; i <= 16; i++) {
            Shared& x = s[i & 1];
            vkWaitForFences(C.vd, 1, &vf[0], VK_TRUE, UINT64_MAX); vkResetFences(C.vd, 1, &vf[0]);
            recordVk(cbs[0], x.img, qp, 0, i, 1);
            submitVk(cbs[0], done.s, i - 1, ready.s, i);
            VkSubmitInfo e{VK_STRUCTURE_TYPE_SUBMIT_INFO}; vkQueueSubmit(C.q, 1, &e, vf[0]);
            C.ctx->Wait(ready.f, i);
            D3D11_BOX box{ (UINT)W / 2, (UINT)H / 2, 0, (UINT)W / 2 + 1, (UINT)H / 2 + 1, 1 };
            C.ctx->CopySubresourceRegion(x.staging, 0, 0, 0, 0, x.tex, 0, &box);
            C.ctx->Signal(done.f, i);
            D3D11_MAPPED_SUBRESOURCE ms; CHECK(C.ctx->Map(x.staging, 0, D3D11_MAP_READ, 0, &ms));
            float c[4]; colorFor(i, c); bool ok;
            if (f.dx == DXGI_FORMAT_R8G8B8A8_UNORM) { auto p = (uint8_t*)ms.pData; ok = abs(p[0] - (int)(c[0] * 255 + .5f)) <= 1 && abs(p[1] - (int)(c[1] * 255 + .5f)) <= 1; }
            else if (f.dx == DXGI_FORMAT_R32_FLOAT) ok = fabsf(*(float*)ms.pData - c[0]) < 1e-6f;
            else { auto p = (uint16_t*)ms.pData; ok = p[0] != 0 || c[0] == 0; }  // half: non-zero check is enough here
            C.ctx->Unmap(x.staging, 0); if (!ok) bad++;
        }
        printf("   correctness: %s (%d/16 frames mismatched)\n", bad ? "FAIL" : "OK", bad); allOk &= !bad;
        uint64_t v = 16;

        // 2) Serialized hand-off latency: VK work -> D3D wait -> D3D copy -> CPU sees D3D completion.
        Stats lat, vkGpu, dxGpu, base;
        for (int i = 0; i < N / 3; i++) {   // baseline: D3D copy alone (what compositing costs without interop)
            double t0 = qpcMs(); C.ctx->Begin(tm.dj); C.ctx->End(tm.t0); C.ctx->CopyResource(s[0].local, s[0].tex); C.ctx->End(tm.t1); C.ctx->End(tm.dj); C.ctx->End(tm.ev);
            C.ctx->Flush(); while (C.ctx->GetData(tm.ev, nullptr, 0, 0) == S_FALSE) {} base.add(qpcMs() - t0); dxGpu.add(readTimer(tm));
        }
        for (int i = 0; i < N / 3; i++) {
            v++; Shared& x = s[v & 1];
            double t0 = qpcMs();
            recordVk(cbs[0], x.img, qp, 0, (uint32_t)v, 1);
            submitVk(cbs[0], done.s, v - 1, ready.s, v);
            C.ctx->Wait(ready.f, v); C.ctx->CopyResource(x.local, x.tex); C.ctx->Signal(done.f, v); C.ctx->End(tm.ev); C.ctx->Flush();
            while (C.ctx->GetData(tm.ev, nullptr, 0, 0) == S_FALSE) {}
            lat.add(qpcMs() - t0);
            uint64_t ts[2]; if (vkGetQueryPoolResults(C.vd, qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
                vkGpu.add((ts[1] - ts[0]) * C.tsPeriod / 1e6);
        }
        printf("   D3D11 copy alone (CPU round trip): mean %.3f ms  p95 %.3f ms | GPU copy %.3f ms\n", base.mean(), base.pct(.95), dxGpu.mean());
        printf("   VK clear+release GPU:              mean %.3f ms\n", vkGpu.mean());
        printf("   VK->D3D serialized hand-off:       mean %.3f ms  p95 %.3f ms  (overhead vs copy+clear ~ %.3f ms)\n",
               lat.mean(), lat.pct(.95), lat.mean() - base.mean() - vkGpu.mean());

        // 3) Pipelined throughput (2 slots, CPU never blocks on D3D): heavier VK frame (8 full clears ~ a light render pass).
        auto runPipelined = [&](bool interop, int frames) {
            double t0 = qpcMs();
            for (int i = 0; i < frames; i++) {
                v++; int sl = (int)(v & 1); Shared& x = s[sl];
                vkWaitForFences(C.vd, 1, &vf[sl], VK_TRUE, UINT64_MAX); vkResetFences(C.vd, 1, &vf[sl]);
                recordVk(cbs[1 + sl], x.img, qp, 2 + 2 * sl, (uint32_t)v, 8);
                if (interop) submitVk(cbs[1 + sl], done.s, v - 2 > 0 ? v - 2 : 0, ready.s, v);
                else { VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cbs[1 + sl]; vkQueueSubmit(C.q, 1, &si, VK_NULL_HANDLE); }
                VkSubmitInfo e{VK_STRUCTURE_TYPE_SUBMIT_INFO}; vkQueueSubmit(C.q, 1, &e, vf[sl]);
                if (interop) C.ctx->Wait(ready.f, v);
                C.ctx->CopyResource(x.local, x.tex);
                if (interop) C.ctx->Signal(done.f, v);
                C.ctx->End(tm.ev); C.ctx->Flush();
                if (i % 2) while (C.ctx->GetData(tm.ev, nullptr, 0, 0) == S_FALSE) {}  // keep D3D at most ~2 frames behind (like a swapchain)
            }
            vkDeviceWaitIdle(C.vd); while (C.ctx->GetData(tm.ev, nullptr, 0, 0) == S_FALSE) {}
            return (qpcMs() - t0) / frames;
        };
        // non-interop run must not leave the "done" fence behind the values interop waits on
        double noSync = runPipelined(false, N / 3); C.ctx->Signal(done.f, v); C.ctx->Signal(ready.f, v);
        double withSync = runPipelined(true, N / 3);
        printf("   pipelined frame time: unsynchronised %.3f ms | with shared-fence sync %.3f ms | sync cost %.3f ms/frame\n",
               noSync, withSync, withSync - noSync);
        vkDeviceWaitIdle(C.vd);
    }
    printf("\nRESULT: %s on %s\n", allOk ? "INTEROP OK" : "INTEROP FAILED", C.gpuName);
    return allOk ? 0 : 2;
}
