// Phase 0 native helper: D3D11 timestamp queries driven from Unity's render thread via GL.IssuePluginEvent.
// Event id layout: kind<<24 | ring<<16 | slot.  kinds: 1 = frame begin (slot 0), 2 = mark(slot), 3 = frame end(slot).
// Results for a ring entry are harvested on the render thread when that entry is reused (RING frames later).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <cstring>

#define EXPORT extern "C" __declspec(dllexport)
enum { RING = 6, SLOTS = 128 };

struct Entry { ID3D11Query* dj; ID3D11Query* ts[SLOTS]; int used; bool pending; uint64_t frame; };
static ID3D11Device* gDev;
static ID3D11DeviceContext* gCtx;
static Entry gRing[RING];
static uint64_t gFrameOf[RING];
static CRITICAL_SECTION gLock;
// Published results (copied under lock): per ring entry, ms offsets from slot 0 (-1 = missing).
static struct { uint64_t frame; int count; double ms[SLOTS]; bool valid; } gOut[RING];

EXPORT int KSPVk_InitD3D11(void* nativeTexture) {
    if (gDev) return 1;
    if (!nativeTexture) return -1;
    InitializeCriticalSection(&gLock);
    ((ID3D11Resource*)nativeTexture)->GetDevice(&gDev);
    if (!gDev) return -2;
    gDev->GetImmediateContext(&gCtx);
    D3D11_QUERY_DESC qd{};
    for (auto& e : gRing) {
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; if (FAILED(gDev->CreateQuery(&qd, &e.dj))) return -3;
        qd.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& q : e.ts) if (FAILED(gDev->CreateQuery(&qd, &q))) return -4;
    }
    return 1;
}

EXPORT void KSPVk_SetFrame(int ring, uint64_t frame) { gFrameOf[ring % RING] = frame; }

static void harvest(int r) {
    Entry& e = gRing[r];
    if (!e.pending) return;
    e.pending = false;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    bool ok = gCtx->GetData(e.dj, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint;
    double ms[SLOTS]; UINT64 t0 = 0;
    for (int i = 0; ok && i < e.used; i++) {
        UINT64 t; if (gCtx->GetData(e.ts[i], &t, sizeof t, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { ms[i] = -1; continue; }
        if (i == 0) t0 = t;
        ms[i] = (double)(t - t0) * 1000.0 / dj.Frequency;
    }
    EnterCriticalSection(&gLock);
    gOut[r].frame = e.frame; gOut[r].count = e.used; gOut[r].valid = ok;
    if (ok) memcpy(gOut[r].ms, ms, sizeof(double) * e.used);
    LeaveCriticalSection(&gLock);
}

static void __stdcall OnRenderEvent(int id) {
    if (!gCtx) return;
    int kind = (id >> 24) & 0xff, r = ((id >> 16) & 0xff) % RING, slot = id & 0xffff;
    if (slot >= SLOTS) return;
    Entry& e = gRing[r];
    if (kind == 1) {
        harvest(r);
        e.frame = gFrameOf[r]; e.used = 1;
        gCtx->Begin(e.dj); gCtx->End(e.ts[0]);
    } else if (kind == 2 || kind == 3) {
        if (e.used == 0) return;               // begin not seen (e.g. plugin attached mid-frame)
        gCtx->End(e.ts[slot]); if (slot + 1 > e.used) e.used = slot + 1;
        if (kind == 3) { gCtx->End(e.dj); e.pending = true; }
    }
}

EXPORT void* KSPVk_GetRenderEventFunc() { return (void*)&OnRenderEvent; }

// Copies the newest harvested result for ring entry r. Returns slot count, 0 if none, -1 if disjoint/invalid.
EXPORT int KSPVk_GetGpuFrame(int r, uint64_t* frame, double* ms, int max) {
    if (!gDev) return 0;
    r %= RING; int n;
    EnterCriticalSection(&gLock);
    *frame = gOut[r].frame; n = gOut[r].valid ? (gOut[r].count < max ? gOut[r].count : max) : -1;
    if (n > 0) memcpy(ms, gOut[r].ms, sizeof(double) * n);
    if (gOut[r].count == 0) n = 0;
    gOut[r].count = 0;
    LeaveCriticalSection(&gLock);
    return n;
}
