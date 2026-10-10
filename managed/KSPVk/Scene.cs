using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.SceneManagement;

namespace KSPVk
{
    /// Object extractor (Phase 2 parts, Phase 4 every scene): mirrors the opaque mesh renderers seen by the scene's local camera
    /// (flight, KSC, editors, main menu; IVA interiors mapped from internal space into world space) into the native scene renderer.
    /// A renderer is hidden from Unity (forceRenderingOff, skinned casters ShadowsOnly; IVA: only while the InternalCamera renders) only once its
    /// mesh and textures are resident on the Vulkan side; everything else keeps rendering in Unity.
    public unsafe partial class Scene
    {
        public const int FBump = 1, FSpec = 2, FEmis = 4, FMapped = 8, FUnlit = 16, FNoVcol = 32, FCutoff = 64, FStd = 128, FStdSpec = 256, FRgAg = 512,
                         FLmShader = 1024, FGround = 2048, FMul = 4096, FGlossA = 8192, FGlossMap = 16384, FDetail = 32768, FWrap = 65536, FFlag = 131072, FIntSpace = 262144, FAMix = 524288;
        const int DfCast = 1, DfSkin = 2, DfIva = 4, DfMask = 8, RsReady = 2, RsFailed = 3, TexWhite = 0, TexFlat = 1, TexBlack = 2, TexClear = 3;
        const int MaxSub = 16, MaxLight = 8, MatF = 96, InstF = 24, SceneInBytes = 920;
        const int MpbPerFrame = 256, MatPerFrame = 24, RescanFrames = 60, PumpKB = 16384;
        // layers a local camera can show that hold mesh renderers: Default, TransparentFX, Local Scenery, Internal Space / editor interior, Kerbals, Disconnected Parts, 20, AeroFXIgnore
        const int LocalLayers = (1 << 0) | (1 << 1) | (1 << 15) | (1 << 16) | (1 << 17) | (1 << 19) | (1 << 20) | (1 << 23);
        const float LmRGBM = 5;   // gamma-space RGBM lightmap range (unity_Lightmap_HDR.x)

        static readonly Dictionary<string, int> ShaderFlags = new Dictionary<string, int>
        {
            ["KSP/Diffuse"] = 0, ["KSP/Specular"] = FSpec, ["KSP/Bumped"] = FBump, ["KSP/Bumped Specular"] = FBump | FSpec,
            ["KSP/Bumped Specular (Stencil)"] = FBump | FSpec, ["KSP/Emissive/Diffuse"] = FEmis, ["KSP/Emissive/Specular"] = FEmis | FSpec,
            ["KSP/Emissive/Bumped Specular"] = FEmis | FBump | FSpec, ["KSP/Bumped Specular (Mapped)"] = FMapped | FBump,
            ["KSP/Emissive/Bumped Specular (Mapped)"] = FMapped | FBump | FEmis, ["KSP/Alpha/Cutoff"] = FCutoff, ["KSP/Alpha/Cutoff Bumped"] = FCutoff | FBump,
            ["KSP/Bumped Specular Opaque (Cutoff)"] = FCutoff | FBump | FSpec, ["KSP/Specular Opaque (Cutoff)"] = FCutoff | FSpec,
            ["KSP/Unlit"] = FUnlit, ["KSP/UnlitColor"] = FUnlit,
            ["KSP/Scenery/Diffuse"] = 0, ["KSP/Scenery/Specular"] = FSpec, ["KSP/Scenery/Bumped"] = FBump, ["KSP/Scenery/Bumped Specular"] = FBump | FSpec,
            ["KSP/Scenery/Emissive/Specular"] = FEmis | FSpec, ["KSP/Scenery/Emissive/Bumped Specular"] = FEmis | FBump | FSpec,
            ["KSP/Scenery/Alpha/Cutoff"] = FCutoff, ["KSP/Scenery/Diffuse Multiply"] = FMul, ["KSP/Scenery/Diffuse Ground KSC"] = FGround,
            ["KSP/Scenery/Emissive/Lightmapper"] = FLmShader | FEmis, ["KSP/Scenery/Emissive/Lightmapper_Specular"] = FLmShader | FEmis | FSpec,
            ["Legacy Shaders/Diffuse"] = 0, ["Legacy Shaders/Specular"] = FSpec, ["Legacy Shaders/Bumped Diffuse"] = FBump | FRgAg,
            ["Legacy Shaders/Bumped Specular"] = FBump | FSpec | FRgAg, ["Mobile/Diffuse"] = 0,
            ["Standard"] = FStd, ["Standard (Specular setup)"] = FStd | FStdSpec,
            ["Legacy Shaders/Transparent/Cutout/Bumped Diffuse"] = FCutoff | FBump | FRgAg, ["KSP/Scenery/Diffuse Detail"] = FStd | FDetail,
            ["KSP/Scenery/Diffuse Ground KSC Specular"] = FGround | FStd | FStdSpec | FGlossA,
            ["KSP/Lightwrapped/Specular"] = FWrap | FSpec, ["KSP/Scenery/Flag"] = FFlag,
            ["KSP/InternalSpace"] = FIntSpace | FBump,
            ["Parallax Specular (Alpha)"] = FAMix | FBump | FSpec,   // planted-flag banner: texture over _Color by its alpha, no ambient; parallax offset ignored
        };
        // opaque-queue shaders left to Unity: depth-only mask, shadow-only receiver, grab-pass water
        static readonly HashSet<string> StayInUnity = new HashSet<string>
            { "DepthMask", "KSP/Scenery/Invisible Shadow Receiver", "KSP/Scenery/Reflective Water" };

        // ---------- native ----------
        [StructLayout(LayoutKind.Sequential, Pack = 4)]
        public struct SceneStats { public int draws, visible, shadowDraws, meshes, meshReady, textures, texReady, pending, arenaFull; public float recordMs; public ulong uploadedBytes, texBytes; }
        public delegate int RegisterFn(IntPtr p);
        public delegate int PumpFn(int budgetKB);
        public delegate void StatePtrsFn(out IntPtr mesh, out IntPtr tex);
        public delegate void SceneStatsFn(out SceneStats s);
        readonly RegisterFn registerMesh, registerTex; readonly PumpFn pump; readonly Action reset; readonly SceneStatsFn sceneStats;
        readonly sbyte* meshState, texState;

        // SceneIn, MeshIn (612 B), TexIn (24 B) in unmanaged memory
        readonly IntPtr sceneIn = Marshal.AllocHGlobal(SceneInBytes), meshIn = Marshal.AllocHGlobal(612), texIn = Marshal.AllocHGlobal(24);
        IntPtr draws, insts, mats, bones; int drawCap, instCap, matCap, boneCap;

        class Entry
        {
            public Renderer r; public SkinnedMeshRenderer smr; public Transform t;
            public Mesh mesh; public Material[] mats; public int[] slots; public int meshId = -1, inst, lmTex = -1;
            public ShadowCastingMode shadow; public int layerBit; public bool hidden, cast, iva, mcc, mask; public Part part;
            public Transform[] bones; public Matrix4x4[] bind;
            public int xi = -1, rbx = -1; public int[] bx; public Bounds lb; public Transform rb; public bool bonesOn; public VkHidden vh;   // transform job slots, skinned local bounds
            public string reason;    // why it is not drawn by Vulkan (null = drawn, or waiting for uploads)
        }
        class MatRec { public Material m; public int flags, fam; public Renderer r; public Component c; public int[] tex = new int[5]; public bool ready; }

        readonly List<Entry> entries = new List<Entry>();
        readonly Dictionary<Renderer, Entry> byRenderer = new Dictionary<Renderer, Entry>();
        readonly Dictionary<int, int> meshIds = new Dictionary<int, int>(), texIds = new Dictionary<int, int>();
        public Dictionary<int, int> TexIdsDiag => texIds;
        readonly Dictionary<Material, int> matSlots = new Dictionary<Material, int>();
        readonly List<MatRec> matRecs = new List<MatRec>();
        readonly MaterialPropertyBlock mpb = new MaterialPropertyBlock();
        readonly List<Light> lights = new List<Light>(); Light lastSun;
        static readonly System.Reflection.FieldInfo ScaledSunF = typeof(Sun).GetField("scaledSunLight", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Instance);   // private in KSP
        struct LightC { public float d; public Light l; public Vector3 pos, fw; }
        readonly List<LightC> cand = new List<LightC>();
        readonly Plane[] planes = new Plane[6];
        readonly List<Entry> ivaHidden = new List<Entry>();
        readonly List<int> liveMats = new List<int>();   // material instances (renderer.material): stock mutates them in place (lights, IVA indicators)
        readonly HashSet<Renderer> seen = new HashSet<Renderer>();
        int frame, mpbCursor, matCursor, liveCursor, localMask, ivaMask;
        Matrix4x4 ivaM;
        Camera ivaCam;
        public int Hidden, Draws, TexFallbacks;
        public readonly long[] Prof = new long[7];   // Stopwatch ticks: rescan, materials + MPBs, object draws, world, header; expose for off-screen cameras (outside Build); transform job wait
        static long Tick() => System.Diagnostics.Stopwatch.GetTimestamp();
        void Lap(int i, ref long t) { long n = Tick(); Prof[i] += n - t; t = n; }
        public bool Active { get; private set; }
        public Camera Cam { get; private set; }
        public Camera IvaCam => ivaCam;
        public Camera CompositeCam { get; private set; }   // where the local view is composited: the earliest of the merged cameras   // local view camera (null: scaled / galaxy views only)

        public Scene(RegisterFn regMesh, RegisterFn regTex, PumpFn pump, StatePtrsFn statePtrs, Action reset, SceneStatsFn stats)
        {
            registerMesh = regMesh; registerTex = regTex; this.pump = pump; this.reset = reset; sceneStats = stats;
            statePtrs(out IntPtr m, out IntPtr t); meshState = (sbyte*)m; texState = (sbyte*)t;
            Camera.onPreCull += c => { try { if (c == ivaCam) { foreach (var e in ivaHidden) if (e.r) e.r.forceRenderingOff = true; } else if (Foreign(c) && exposed++ == 0) Expose(true, c.cullingMask); } catch (Exception x) { Core.Fault(x); } };
            Camera.onPostRender += c => { try { if (c == ivaCam) { foreach (var e in ivaHidden) if (e.r) e.r.forceRenderingOff = false; } else if (exposed > 0 && Foreign(c) && --exposed == 0) Expose(false, exposedMask); } catch (Exception x) { Core.Fault(x); } };
        }

        public SceneStats Stats() { var s = default(SceneStats); sceneStats(out s); return s; }

        static void Grow(ref IntPtr p, ref int cap, int need, int bytes)
        {
            if (need <= cap) return;
            cap = Math.Max(need, cap * 2 + 64);
            p = p == IntPtr.Zero ? Marshal.AllocHGlobal(cap * bytes) : Marshal.ReAllocHGlobal(p, (IntPtr)(cap * bytes));
        }

        /// After a settings change: give Unity back whatever is no longer allowed; the draw loops re-take the rest.
        public void ApplyOwnership()
        {
            if (!PartsOn) foreach (var e in entries) Unhide(e);
            RestoreShadows(); TakeShadows();
            ApplyFamilies();
        }

        static bool Plain(Camera c) => c && c.isActiveAndEnabled && c.targetTexture == null && !c.orthographic && c.cameraType == CameraType.Game;

        /// The local view: the flight camera in flight, elsewhere the top-most plain camera showing local layers. Cameras sharing its pose
        /// (editor sceneryCam under Main Camera) are merged into one view. IVA: the InternalCamera's layers, mapped into world space.
        Camera PickLocal()
        {
            localMask = ivaMask = 0; ivaCam = null; owned.Clear();
            Camera best = null;
            CompositeCam = null;
            if (HighLogic.LoadedSceneIsFlight)
            {
                best = FlightCamera.fetch ? FlightCamera.fetch.mainCamera : null;
                if (!Plain(best)) return null;
                localMask = best.cullingMask & LocalLayers;
                var ic = InternalCamera.Instance;
                Camera c = ic ? ic.GetComponent<Camera>() : null;
                if (ic && ic.isActive && Plain(c) && InternalSpace.Instance && FlightGlobals.ActiveVessel)
                {
                    ivaCam = c; ivaMask = c.cullingMask & LocalLayers & ~localMask;
                    Transform v = FlightGlobals.ActiveVessel.transform;
                    ivaM = Matrix4x4.TRS(v.position, v.rotation, Vector3.one) * Matrix4x4.Translate(-InternalSpace.Instance.transform.position);
                }
                owned.Add(best);
                return CompositeCam = best;
            }
            foreach (var c in Camera.allCameras)
                if (Plain(c) && (c.cullingMask & LocalLayers) != 0 && (best == null || c.depth > best.depth)) best = c;
            if (best == null) return null;
            // composite in the first merged camera when the later ones keep its depth (editor: sceneryCam, then Main Camera with clear=Nothing),
            // so Unity's scenery decals drawn in between land on top of our surfaces instead of under the later composite
            Camera first = best; bool keep = true;
            foreach (var c in Camera.allCameras)
                if (Plain(c) && (c.transform.position - best.transform.position).sqrMagnitude < 1e-4f && Vector3.Dot(c.transform.forward, best.transform.forward) > 0.9999f)
                {
                    localMask |= c.cullingMask & LocalLayers; owned.Add(c);
                    if (c.depth < first.depth) first = c;
                }
            foreach (var c in Camera.allCameras)
                if (Plain(c) && c.depth > first.depth && c.depth <= best.depth && c.clearFlags != CameraClearFlags.Nothing) keep = false;
            CompositeCam = keep ? first : best;
            return best;
        }

        /// Right after UpdateAllRenderers: start the transform job so it overlaps Unity's own late-frame work (skinning, canvases).
        public void EarlySchedule() { if (Active) XSchedule(); }   // after UpdateAllRenderers, which would wait for it

        /// Per frame, before BeginFrame2. Returns the SceneIn pointer (null when inactive) and the draw count.
        public IntPtr Build(bool enabled, out int drawCount)
        {
            drawCount = 0;
            Camera cam = enabled ? PickLocal() : null, view = cam ?? (ScaledCamera.Instance ? ScaledCamera.Instance.cam : null);
            if (!enabled || !Plain(view))
            {
                if (Active) RestoreAll();
                return IntPtr.Zero;
            }
            if (cam != Cam) frame = 0;   // camera switch: rescan now
            if (exposed > 0) { exposed = 0; Expose(false, exposedMask); }
            if (texMax != Settings.MaxTexture)   // Vulkan copies are sized at upload: a new cap re-uploads everything
            {
                bool had = texMax >= 0; texMax = Settings.MaxTexture; SetTexMax?.Invoke(texMax);
                Debug.Log($"[KSPVk] texture cap {texMax}, anisotropic filtering {QualitySettings.anisotropicFiltering}, master limit {QualitySettings.masterTextureLimit}");
                if (had) { ResetNative("texture size cap changed"); return IntPtr.Zero; }
            }
            Active = true; Cam = cam;
            long t0 = Tick();
            if (frame++ % RescanFrames == 0 || rootPos >= 0) Rescan(frame == 1);   // first frame on a camera: all at once
            else Verify();
            if (!xRun) XSchedule();   // normally already running since the start of PostLateUpdate
            Lap(0, ref t0);
            if (pump(PumpKB) == 0 && Stats().arenaFull != 0) { ResetNative(); return IntPtr.Zero; }

            // materials: re-read constants round-robin; textures may have become ready
            for (int i = 0, n = Math.Min(MatPerFrame, matRecs.Count); i < n; i++) { matCursor = (matCursor + 1) % matRecs.Count; WriteMat(matCursor); }
            for (int i = 0, n = Math.Min(MatPerFrame * 2, liveMats.Count); i < n; i++) { liveCursor = (liveCursor + 1) % liveMats.Count; WriteMat(liveMats[liveCursor]); }
            if (frame % 4 == 0) for (int i = 0; i < matRecs.Count; i++) if (!matRecs[i].ready && matRecs[i].fam == 0) WriteMat(i);   // waiting for textures
            // MPBs (highlight rim, temperature, emissive colour): round-robin
            for (int i = 0, n = Math.Min(MpbPerFrame, entries.Count); i < n; i++) { mpbCursor = (mpbCursor + 1) % entries.Count; ReadMpb(entries[mpbCursor]); }

            XComplete();
            Lap(1, ref t0);
            int subs = 0;
            foreach (var e in entries) subs += e.slots != null ? e.slots.Length : 0;
            Grow(ref draws, ref drawCap, subs + WorldCap(), 80);
            int nb = 0;
            Hidden = 0;
            Vessel ivaV = ivaCam ? FlightGlobals.ActiveVessel : null;   // in IVA only the interior shades itself (stock IVASun lights layers 16/20 only)
            if (cam != null) GeometryUtility.CalculateFrustumPlanes(cam, planes);
            float* D = (float*)draws;
            for (int i = 0; PartsOn && i < entries.Count; i++)
            {
                var e = entries[i];
                if (e.r == null || e.reason != null) continue;
                if (!e.hidden)
                {
                    if (meshState[e.meshId] == RsFailed) { e.reason = "mesh failed"; continue; }
                    if (meshState[e.meshId] != RsReady || !Ready(e)) continue;
                    Hide(e);
                }
                Hidden++;
                if (e.iva ? !e.r.enabled || !e.r.gameObject.activeInHierarchy : !e.vh.on || !e.r.enabled) continue;
                float* d = D + drawCount * 20;
                int* di = (int*)(d + 12);
                int bone = 0, fl = (e.cast && !e.mask && !(ivaV && e.part && e.part.vessel == ivaV) ? DfCast : 0) | (e.iva ? DfIva : 0) | (e.mask ? DfMask : 0);
                if (e.smr != null)
                {
                    // world bounds from the root bone and the local bounds (smr.bounds is a costly icall); off-screen skinned objects (VAB workers) skip their bones
                    XRows(e.rbx, e.rb, d);
                    Vector3 c = e.lb.center, x = e.lb.extents;
                    Vector3 wc = new Vector3(d[0] * c.x + d[1] * c.y + d[2] * c.z + d[3], d[4] * c.x + d[5] * c.y + d[6] * c.z + d[7], d[8] * c.x + d[9] * c.y + d[10] * c.z + d[11]);
                    Vector3 wx = new Vector3(Math.Abs(d[0]) * x.x + Math.Abs(d[1]) * x.y + Math.Abs(d[2]) * x.z, Math.Abs(d[4]) * x.x + Math.Abs(d[5]) * x.y + Math.Abs(d[6]) * x.z, Math.Abs(d[8]) * x.x + Math.Abs(d[9]) * x.y + Math.Abs(d[10]) * x.z);
                    bool vis = cam == null || e.iva || GeometryUtility.TestPlanesAABB(planes, new Bounds(wc, wx * 2));
                    XBonesOn(e, vis);
                    if (!vis) continue;
                    int nbone = e.bones.Length;
                    Grow(ref bones, ref boneCap, (nb + nbone) * 3, 16);
                    float* B = (float*)bones + nb * 12;
                    for (int b = 0; b < nbone; b++, B += 12)
                    {
                        int k = e.bx[b];
                        if (k >= 0 && xFresh[k]) *(R34*)B = X[k];
                        else Rows(e.bones[b] != null ? e.bones[b].localToWorldMatrix * e.bind[b] : Matrix4x4.zero, B);
                        if (e.iva) Rows(ivaM * Mat(B), B);
                    }
                    if (e.iva) wc = ivaM.MultiplyPoint3x4(wc);
                    d[0] = wc.x; d[1] = wc.y; d[2] = wc.z; d[3] = wx.magnitude;
                    bone = nb * 3; fl |= DfSkin;
                    nb += nbone;
                }
                else
                {
                    XRows(e.xi, e.t, d);
                    if (e.iva) Rows(ivaM * Mat(d), d);
                }
                for (int s = 0; s < e.slots.Length; s++)
                {
                    float* ds = D + (drawCount + s) * 20;
                    if (s > 0) for (int k = 0; k < 12; k++) ds[k] = d[k];
                    int* dsi = (int*)(ds + 12);
                    dsi[0] = e.meshId; dsi[1] = s; dsi[2] = e.slots[s]; dsi[3] = e.inst; dsi[4] = fl; dsi[5] = bone; dsi[6] = dsi[7] = 0;
                }
                drawCount += e.slots.Length;
            }
            Draws = drawCount;
            Lap(2, ref t0);
            if (!BuildWorld(cam, ref drawCount)) return IntPtr.Zero;
            Lap(3, ref t0);
            FillHeader(view, cam != null, drawCount, nb * 3);
            Lap(4, ref t0);
            return sceneIn;
        }

        Vector3d lastOrigin;
        /// Floating origin moved or Krakensbane is shifting the world this frame, or the scene was just (re)scanned: cached shadow cascades are stale.
        bool WorldShifted()
        {
            if (!HighLogic.LoadedSceneIsFlight) return frame <= 2;
            Vector3d o = FloatingOrigin.OffsetNonKrakensbane;
            bool moved = o != lastOrigin || Krakensbane.GetFrameVelocity().sqrMagnitude > 0;
            lastOrigin = o;
            return moved || frame <= 2;
        }

        // IVASun follows the sun at 0.9x intensity with its own tint; returned as a scale of the sun colour
        float IvaSun(Light sun)
        {
            if (!ivaCam || !sun || sun.intensity <= 0) return 1;
            foreach (var l in lights)
                if (l && l.isActiveAndEnabled && l.type == LightType.Directional && (l.cullingMask & ivaMask) != 0 && (l.cullingMask & localMask) == 0)
                    return l.intensity * l.color.grayscale / (sun.intensity * sun.color.grayscale);
            return 1;
        }

        static readonly int[] LocalLayerList = { 0, 1, 15, 16, 17, 19, 20, 23 };
        static int Mask8(int mask) { int m = 0; for (int i = 0; i < 8; i++) if ((mask >> LocalLayerList[i] & 1) != 0) m |= 1 << i; return m; }   // 32-layer mask -> bits over LocalLayers

        bool Ready(Entry e)
        {
            foreach (int s in e.slots) if (!matRecs[s].ready) return false;
            return e.lmTex < 0 || texState[e.lmTex] == RsReady;
        }

        static void Rows(Matrix4x4 m, float* o)
        {
            o[0] = m.m00; o[1] = m.m01; o[2] = m.m02; o[3] = m.m03;
            o[4] = m.m10; o[5] = m.m11; o[6] = m.m12; o[7] = m.m13;
            o[8] = m.m20; o[9] = m.m21; o[10] = m.m22; o[11] = m.m23;
        }

        void FillHeader(Camera cam, bool local, int drawCount, int bone4)
        {
            byte* h = (byte*)sceneIn;
            *(IntPtr*)(h + 0) = draws; *(IntPtr*)(h + 8) = insts; *(IntPtr*)(h + 16) = mats; *(IntPtr*)(h + 24) = bones;
            int* hi = (int*)(h + 32);
            hi[0] = drawCount; hi[1] = instTop; hi[2] = matRecs.Count; hi[3] = bone4; hi[5] = 0;
            float* f = (float*)(h + 56);
            Matrix4x4 v = cam.worldToCameraMatrix, p = cam.projectionMatrix;
            for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) f[r * 4 + c] = v[r, c];
            f[16] = p.m00; f[17] = p.m01; f[18] = p.m02; f[19] = p.m10; f[20] = p.m11; f[21] = p.m12;
            f[22] = ivaCam ? Mathf.Min(cam.nearClipPlane, ivaCam.nearClipPlane) : cam.nearClipPlane;
            f[23] = Settings.ShadowDist > 0 ? Settings.ShadowDist : Mathf.Min(QualitySettings.shadowDistance, 2500f);
            Vector3 cp = cam.transform.position; f[24] = cp.x; f[25] = cp.y; f[26] = cp.z; f[27] = 1;
            // sun: Sun.Instance.sunLight if it lights the local layers, else the brightest directional light that does (editors)
            int mask = localMask | ivaMask;
            Light sun = Sun.Instance != null ? Sun.Instance.sunLight : null;
            if (sun != null && !(sun.isActiveAndEnabled && (sun.cullingMask & mask) != 0)) sun = null;
            if (sun == null)
                foreach (var l in lights)
                    if (l && l.type == LightType.Directional && l.isActiveAndEnabled && (l.cullingMask & localMask) != 0 && (sun == null || l.intensity > sun.intensity)) sun = l;
            if (sun == null)   // map view, tracking station: nothing lights the local layers (none scanned); scaled planets take the scaled-space sun
                if (Sun.Instance != null && ScaledSunF?.GetValue(Sun.Instance) is Light sl && sl && sl.isActiveAndEnabled) sun = sl;
            if (sun != lastSun) { lastSun = sun; Debug.Log($"[KSPVk] sun light {(sun ? $"{sun.name} intensity {sun.intensity} colour {sun.color} mask {sun.cullingMask:x}" : "none")} (local mask {localMask:x})"); }
            Vector3 ld = sun ? -sun.transform.forward : Vector3.up;
            Color lc = sun ? sun.color * (sun.intensity * Settings.SunScale) : Color.black;
            f[28] = ld.x; f[29] = ld.y; f[30] = ld.z; f[31] = 0;
            f[32] = lc.r; f[33] = lc.g; f[34] = lc.b; f[35] = 1;
            bool tri = RenderSettings.ambientMode == AmbientMode.Trilight;
            Color amb = RenderSettings.ambientMode == AmbientMode.Flat ? RenderSettings.ambientLight * RenderSettings.ambientIntensity : RenderSettings.ambientSkyColor;
            float ak = Settings.AmbientScale;
            amb *= ak;
            f[36] = amb.r; f[37] = amb.g; f[38] = amb.b; f[39] = 1;
            bool sh = local && sun && ShadowsOf(sun) != LightShadows.None && QualitySettings.shadows != ShadowQuality.Disable;
            f[40] = sun ? Mathf.Clamp01(sun.shadowStrength * Settings.ShadowStrength) : 0; f[41] = Settings.NormalBias; f[42] = sh && Settings.Shadows ? 1 : 0; f[43] = WorldShifted() ? 1 : 0;
            // other lights touching the local layers, strongest reach at the camera first (IVA lights mapped into world space)
            int nl = 0;
            float* L = f + 44;
            cand.Clear();
            foreach (var l in lights)
            {
                if (l == null || l == sun || !l.isActiveAndEnabled || l.intensity <= 0) continue;
                if (l.type != LightType.Point && l.type != LightType.Spot && l.type != LightType.Directional) continue;
                bool iva = (ivaMask >> l.gameObject.layer & 1) != 0;   // IVA lights live in internal space
                if (!iva && (l.cullingMask & localMask) == 0) continue;
                if (l.bakingOutput.isBaked && l.bakingOutput.lightmapBakeType == LightmapBakeType.Baked) continue;
                if (iva && l.type == LightType.Directional) continue;   // IVASun: the world sun lights the mapped interior
                Transform t = l.transform;
                Vector3 pos = t.position, fw = t.forward;
                if (iva) { pos = ivaM.MultiplyPoint3x4(pos); fw = ivaM.MultiplyVector(fw); }
                float reach = l.type == LightType.Directional ? 100 : Mathf.Min(1, l.range * l.range / Mathf.Max(1e-4f, (pos - cp).sqrMagnitude));
                cand.Add(new LightC { d = -l.intensity * reach, l = l, pos = pos, fw = l.type == LightType.Directional ? -fw : fw });   // strongest reach first
            }
            cand.Sort((a, b) => a.d.CompareTo(b.d));
            for (; nl < cand.Count && nl < MaxLight; nl++)
            {
                var k = cand[nl]; var l = k.l;
                float* o = L + nl * 12;
                Color c = l.color * l.intensity;
                o[0] = k.pos.x; o[1] = k.pos.y; o[2] = k.pos.z; o[3] = l.range;
                o[4] = c.r; o[5] = c.g; o[6] = c.b; o[7] = (l.type == LightType.Point ? 0 : l.type == LightType.Spot ? 1 : 2) + 4 * Mask8(l.cullingMask);
                o[8] = k.fw.x; o[9] = k.fw.y; o[10] = k.fw.z; o[11] = Mathf.Cos(l.spotAngle * 0.5f * Mathf.Deg2Rad);
            }
            hi[4] = nl;
            FillViews(f, cam);
            // f[200..]: trilight ambient, environment (ambient mode, reflection intensity, lightmap range), reflection cube id
            Color eq = RenderSettings.ambientEquatorColor * ak, gr = RenderSettings.ambientGroundColor * ak;
            f[200] = eq.r; f[201] = eq.g; f[202] = eq.b; f[203] = IvaSun(sun); f[204] = gr.r; f[205] = gr.g; f[206] = gr.b; f[207] = 1;
            f[208] = tri ? 1 : 0; f[209] = RenderSettings.reflectionIntensity; f[210] = LmRGBM; f[211] = 0;
            Texture env = RenderSettings.defaultReflectionMode == DefaultReflectionMode.Custom ? RenderSettings.customReflection : ReflectionProbe.defaultTexture;
            int* ei = (int*)(f + 212);
            ei[0] = env != null && env.dimension == TextureDimension.Cube ? TexId(env, -1) : -1; ei[1] = ei[2] = ei[3] = 0;
        }

        // ---------- scanning ----------
        // A rescan walks the scene roots (FindObjectsOfType scans every object in memory: 6.4 ms) within a time budget per frame,
        // except on a camera's first frame. Instance slots are stable (free list).
        readonly List<GameObject> roots = new List<GameObject>(), rootTmp = new List<GameObject>();
        readonly HashSet<Renderer> ignored = new HashSet<Renderer>();   // rejected by layer / type / PQS quad: skipped until every 8th scan
        int scans, rendPos;   // rendTmp holds the current root's renderers; rendPos: next one to consider
        readonly List<Renderer> rendTmp = new List<Renderer>();
        readonly List<Light> lightTmp = new List<Light>(), lightsNext = new List<Light>();
        int rootPos = -1, instTop;
        readonly List<int> freeInst = new List<int>();
        static readonly List<Material> matTmp = new List<Material>();
        const double ScanBudgetMs = 0.4;
        public double RescanMaxMs;   // worst single-frame rescan slice (diagnostics)

        void Rescan(bool full)
        {
            long t0 = Tick();
            int mask = localMask | ivaMask;
            if (rootPos < 0 || full)
            {
                seen.Clear(); lightsNext.Clear(); roots.Clear();
                if (mask != 0)
                {
                    for (int i = 0; i < SceneManager.sceneCount; i++)
                    {
                        var sc = SceneManager.GetSceneAt(i);
                        if (sc.isLoaded) { sc.GetRootGameObjects(rootTmp); roots.AddRange(rootTmp); }
                    }
                    Core.Instance.gameObject.scene.GetRootGameObjects(rootTmp); roots.AddRange(rootTmp);   // DontDestroyOnLoad (bodies, KSC under Kerbin's PQS)
                }
                rootPos = 0; rendTmp.Clear(); rendPos = 0;
                if (full || ++scans % 8 == 0) ignored.Clear();
            }
            long budget = full ? long.MaxValue : (long)(ScanBudgetMs * System.Diagnostics.Stopwatch.Frequency / 1000);
            while ((rendPos < rendTmp.Count || rootPos < roots.Count) && Tick() - t0 < budget)
            {
                if (rendPos < rendTmp.Count) { for (int n = 0; n < 8 && rendPos < rendTmp.Count; n++) Consider(rendTmp[rendPos++], mask, full); continue; }
                var go = roots[rootPos++];
                if (go == null || !go.activeInHierarchy) continue;
                go.GetComponentsInChildren(false, lightTmp); lightsNext.AddRange(lightTmp);
                go.GetComponentsInChildren(false, rendTmp); rendPos = 0;
            }
            if (rootPos >= roots.Count && rendPos >= rendTmp.Count)
            {
                for (int i = entries.Count - 1; i >= 0; i--)
                    if (entries[i].r == null || !seen.Contains(entries[i].r)) Drop(entries[i]);
                rootPos = -1; roots.Clear(); rendTmp.Clear(); rendPos = 0;
                lights.Clear(); lights.AddRange(lightsNext);
                TakeShadows();
                RescanWorld(full);
            }
            if (!full) RescanMaxMs = Math.Max(RescanMaxMs, (Tick() - t0) * 1000.0 / System.Diagnostics.Stopwatch.Frequency);
        }

        /// Discovery only checks that a known renderer is still there; its mesh and materials are verified round-robin (Verify), or here on a full scan.
        void Consider(Renderer r, int mask, bool verify)
        {
            if (r == null) return;   // destroyed since its root was listed
            if (byRenderer.TryGetValue(r, out var e))
            {
                seen.Add(r);
                if (!verify || Same(e)) return;
                Drop(e);
            }
            if (ignored.Contains(r)) return;
            if (!(r is MeshRenderer) && !(r is SkinnedMeshRenderer) || (mask >> r.gameObject.layer & 1) == 0) { ignored.Add(r); return; }
            bool iva = (ivaMask >> r.gameObject.layer & 1) != 0;
            if (r.GetComponent<PQ>() != null || Compat.Excluded(r)) { ignored.Add(r); return; }   // PQS quads belong to World; mods can keep renderers in Unity
            if (r.forceRenderingOff) return;   // hidden by someone else
            seen.Add(r);
            e = Setup(r, iva);
            e.inst = freeInst.Count > 0 ? freeInst[freeInst.Count - 1] : instTop++;
            if (freeInst.Count > 0) freeInst.RemoveAt(freeInst.Count - 1);
            Grow(ref insts, ref instCap, instTop, InstF * 4);
            entries.Add(e); byRenderer[r] = e;
            ReadMpb(e); WriteInst(e);
        }

        static bool PartsOn => Settings.Parts && !Compat.PartsOff;
        public bool Owns(Renderer r) => r != null && byRenderer.TryGetValue(r, out var e) && e.hidden;
        public void Release(Renderer r) { if (r != null && byRenderer.TryGetValue(r, out var e)) Drop(e); if (r != null) ignored.Add(r); }
        public void Unignore(Renderer r) { if (r != null) ignored.Remove(r); }

        void Drop(Entry e)
        {
            Unhide(e);
            entries.Remove(e); byRenderer.Remove(e.r); freeInst.Add(e.inst);
        }

        bool Same(Entry e)
        {
            var r = e.r;
            if (r == null) return true;   // dropped by the scan
            Mesh m = e.smr != null ? e.smr.sharedMesh : r.GetComponent<MeshFilter>()?.sharedMesh;
            return m == e.mesh && e.iva == ((ivaMask >> r.gameObject.layer & 1) != 0) && SameMats(r, e);
        }

        int verCursor;
        const int VerifyPerFrame = 64;
        void Verify()
        {
            int mask = localMask | ivaMask;
            for (int k = 0; k < VerifyPerFrame && entries.Count > 0; k++)
            {
                if (verCursor >= entries.Count) verCursor = 0;
                var e = entries[verCursor];
                if (Same(e)) { verCursor++; continue; }
                var r = e.r;
                Drop(e); Consider(r, mask, false);
            }
        }

        static bool SameMats(Renderer r, Entry e)
        {
            r.GetSharedMaterials(matTmp);
            if (matTmp.Count != e.mats.Length) return false;
            for (int i = 0; i < matTmp.Count; i++) if (matTmp[i] != e.mats[i]) return false;
            return true;
        }

        Entry Setup(Renderer r, bool iva)
        {
            var e = new Entry { r = r, t = r.transform, smr = r as SkinnedMeshRenderer, iva = iva };
            e.mesh = e.smr != null ? e.smr.sharedMesh : r.GetComponent<MeshFilter>()?.sharedMesh;
            e.mats = r.sharedMaterials;
            if (e.mesh == null) { e.reason = "no mesh"; return e; }
            if (r.isPartOfStaticBatch) { e.reason = "static batch"; return e; }
            if (r.shadowCastingMode == ShadowCastingMode.ShadowsOnly) { e.reason = "not visible (shadows only)"; return e; }
            int n = Math.Min(e.mats.Length, e.mesh.subMeshCount);
            if (n == 0) { e.reason = e.mats.Length == 0 ? "no materials" : "no submeshes"; return e; }
            if (n > MaxSub) { e.reason = "too many submeshes (" + e.mesh.subMeshCount + ")"; return e; }
            foreach (var m in e.mats)
            {
                if (m == null || m.shader == null) { e.reason = "no material"; return e; }
                if (m.renderQueue >= 2500) { e.reason = "transparent (Unity)"; return e; }
                if (iva && m.shader.name == "DepthMask") { e.mask = true; continue; }   // IVA window masks: depth only in Vulkan, and still drawn by Unity
                if (StayInUnity.Contains(m.shader.name)) { e.reason = "stays in Unity: " + m.shader.name; return e; }
                if (!ShaderFlags.ContainsKey(m.shader.name)) { e.reason = "shader " + m.shader.name; return e; }
            }
            if (e.mats.Length > e.mesh.subMeshCount) { e.reason = "multi-pass materials"; return e; }
            for (int s = 0; s < n; s++) if (e.mesh.GetTopology(s) != MeshTopology.Triangles) { e.reason = "topology"; return e; }
            if (e.smr != null)
            {
                e.bones = e.smr.bones; e.bind = e.mesh.bindposes;
                if (e.bones == null || e.bones.Length == 0 || e.bind.Length != e.bones.Length) { e.reason = "skin without bones"; return e; }
            }
            int li = r.lightmapIndex, nlm = LightmapSettings.lightmaps?.Length ?? 0;
            if (li >= 0 && li < nlm)
            {
                e.lmTex = TexId(LightmapSettings.lightmaps[li].lightmapColor, -1);
                if (e.lmTex < 0) { e.reason = "lightmap texture"; return e; }
            }
            e.meshId = MeshId(e.mesh);
            if (e.meshId < 0) { e.reason = "mesh unsupported"; return e; }
            e.slots = new int[n];
            for (int s = 0; s < n; s++) e.slots[s] = MatSlot(e.mats[s]);
            e.shadow = r.shadowCastingMode;
            e.cast = e.shadow != ShadowCastingMode.Off;
            e.part = iva ? null : r.GetComponentInParent<Part>();
            if (e.part != null) foreach (var pm in e.part.Modules) if (pm is ModuleColorChanger cc && cc.shaderProperty == "_EmissiveColor") e.mcc = true;   // its MPB colour is authoritative, (0,0,0,0) = off
            return e;
        }

        int MeshId(Mesh m)
        {
            int key = m.GetInstanceID();
            if (meshIds.TryGetValue(key, out int id)) return id;
            byte* p = (byte*)meshIn;
            for (int i = 0; i < 612; i++) p[i] = 0;
            int streams = m.vertexBufferCount;
            if (streams < 1 || streams > 4 || m.subMeshCount > MaxSub) return meshIds[key] = -1;
            var attrs = m.GetVertexAttributes();
            int* stride = (int*)(p + 40); int* hdr = (int*)(p + 56); int* A = (int*)(p + 72);
            if (attrs.Length > 16) return meshIds[key] = -1;
            int n = 0;   // Unity packs each stream's attributes in enum order, no padding
            for (int at = 0; at <= (int)VertexAttribute.BlendIndices; at++)
                foreach (var a in attrs)
                {
                    if ((int)a.attribute != at) continue;
                    int* o = A + n * 5;
                    o[0] = at; o[1] = (int)a.format; o[2] = a.dimension; o[3] = a.stream; o[4] = stride[a.stream];
                    stride[a.stream] += FormatSize(a.format) * a.dimension;
                    n++;
                }
            for (int s = 0; s < streams; s++) ((IntPtr*)p)[s] = m.GetNativeVertexBufferPtr(s);
            ((IntPtr*)p)[4] = m.GetNativeIndexBufferPtr();
            hdr[0] = streams; hdr[1] = m.indexFormat == IndexFormat.UInt32 ? 1 : 0; hdr[2] = m.vertexCount; hdr[3] = n;
            int* sub = (int*)(p + 392);
            sub[0] = m.subMeshCount;
            for (int s = 0; s < m.subMeshCount; s++)
            {
                var d = m.GetSubMesh(s);
                sub[1 + s] = d.indexStart; sub[17 + s] = d.indexCount; sub[33 + s] = d.baseVertex;
            }
            float* b = (float*)(p + 588);
            Bounds bb = m.bounds;
            b[0] = bb.center.x; b[1] = bb.center.y; b[2] = bb.center.z; b[3] = bb.extents.x; b[4] = bb.extents.y; b[5] = bb.extents.z;
            id = registerMesh(meshIn);
            if (id < 0) { ResetNative(); return -1; }
            return meshIds[key] = id;
        }

        static int FormatSize(VertexAttributeFormat f)
        {
            switch (f)
            {
                case VertexAttributeFormat.Float32: case VertexAttributeFormat.UInt32: case VertexAttributeFormat.SInt32: return 4;
                case VertexAttributeFormat.Float16: case VertexAttributeFormat.UNorm16: case VertexAttributeFormat.SNorm16:
                case VertexAttributeFormat.UInt16: case VertexAttributeFormat.SInt16: return 2;
                default: return 1;
            }
        }

        /// 2D textures, plus cubemaps (Cubemap or cube RenderTexture: the reflection environment).
        readonly HashSet<Texture> hiResTex = new HashSet<Texture>();   // registered at twice the size cap (scaled-planet surface maps)
        static int Aniso(int a) =>   // Unity's quality override: Forced On lifts every texture to at least 9
            QualitySettings.anisotropicFiltering == AnisotropicFiltering.Disable ? 1 : QualitySettings.anisotropicFiltering == AnisotropicFiltering.ForceEnable ? Math.Max(a, 9) : a;
        int TexId(Texture t, int fallback)
        {
            if (t == null) return fallback;
            int key = t.GetInstanceID();
            if (texIds.TryGetValue(key, out int id)) return id;
            if (!(t is Texture2D) && t.dimension != TextureDimension.Cube) return texIds[key] = -1;
            IntPtr ptr = t.GetNativeTexturePtr();
            if (ptr == IntPtr.Zero) return fallback;   // render texture not created yet: retry later
            int* p = (int*)texIn;
            *(IntPtr*)p = ptr;
            p[2] = (int)t.wrapModeU; p[3] = (int)t.wrapModeV; p[4] = (int)t.filterMode | (hiResTex.Contains(t) ? 256 : 0); p[5] = Aniso(t.anisoLevel);
            id = registerTex(texIn);
            if (id < 0) { ResetNative(); return fallback; }
            return texIds[key] = id;
        }

        static readonly string[] TexPart = { "_MainTex", "_BumpMap", "_Emissive", "_SpecMap", null }, TexLm = { "_MainTex", "_BumpMap", "_Illum", "_SpecMap", null },
                                 TexGround = { "_TarmacTexture", "_BlendMaskTexture", "_NearGrassTexture", "_FarGrassTexture", null },
                                 TexStd = { "_MainTex", "_BumpMap", "_EmissionMap", "_MetallicGlossMap", "_OcclusionMap" },
                                 TexStdSpec = { "_MainTex", "_BumpMap", "_EmissionMap", "_SpecGlossMap", "_OcclusionMap" },
                                 TexDetail = { "_BaseTex", null, "_MainTex", null, null }, TexFlag = { "_DetailTex", null, "_MainTex", null, null },
                                 TexIntSpace = { "_MainTex", "_BumpMap", "_LightMap", null, null },
                                 TexGroundSpec = { "_TarmacTexture", "_BlendMaskTexture", "_NearGrassTexture", "_FarGrassTexture", "_MidGrassTexture" };
        static readonly int[] TexDefaults = { TexWhite, TexFlat, TexBlack, TexWhite, TexWhite };

        static string[] TexNames(int fl) =>
            (fl & FDetail) != 0 ? TexDetail : (fl & FFlag) != 0 ? TexFlag : (fl & FIntSpace) != 0 ? TexIntSpace : (fl & FGround) != 0 ? ((fl & FStd) != 0 ? TexGroundSpec : TexGround) : (fl & FStd) != 0 ? ((fl & FStdSpec) != 0 ? TexStdSpec : TexStd) : (fl & FLmShader) != 0 ? TexLm : TexPart;

        int MatSlot(Material m)
        {
            if (matSlots.TryGetValue(m, out int s)) return s;
            s = matRecs.Count;
            int fl = ShaderFlags.TryGetValue(m.shader.name, out int sf) ? sf : FUnlit;
            if ((fl & (FUnlit | FIntSpace)) == 0) fl |= FNoVcol;
            if (m.shader.name == "KSP/Unlit") fl |= FNoVcol;
            if ((fl & FStd) != 0)
            {
                if (m.IsKeywordEnabled("_NORMALMAP")) fl |= FBump | FRgAg;
                if (m.IsKeywordEnabled("_METALLICGLOSSMAP") || m.IsKeywordEnabled("_SPECGLOSSMAP")) fl |= FGlossMap;
                if (m.IsKeywordEnabled("_SMOOTHNESS_TEXTURE_ALBEDO_CHANNEL_A")) fl |= FGlossA;
                if (m.IsKeywordEnabled("_EMISSION")) fl |= FEmis;
                if (m.IsKeywordEnabled("_ALPHATEST_ON")) fl |= FCutoff;
            }
            var rec = new MatRec { m = m, flags = fl };
            string[] names = TexNames(fl);
            for (int i = 0; i < 5; i++)
                rec.tex[i] = TexId(names[i] != null && m.HasProperty(names[i]) ? m.GetTexture(names[i]) : null, (fl & FGround) != 0 ? TexWhite : TexDefaults[i]);
            matRecs.Add(rec); matSlots[m] = s;
            if (m.name.EndsWith("(Instance)")) liveMats.Add(s);
            Grow(ref mats, ref matCap, matRecs.Count, MatF * 4);
            WriteMat(s);
            return s;
        }

        void WriteMat(int s)
        {
            var rec = matRecs[s]; var m = rec.m;
            float* f = (float*)mats + s * MatF;
            if (rec.fam != 0) { WriteWorldMat(rec, f); return; }
            int* fi = (int*)f;
            if (m == null) { rec.ready = false; return; }
            int fl = rec.flags;
            bool std = (fl & FStd) != 0, ground = (fl & FGround) != 0;
            Col(f + 0, m, ground ? "_GrassColor" : "_Color", Color.white); Col(f + 4, m, ground ? "_SpecularColor" : "_SpecColor", Color.gray);
            Col(f + 8, m, std ? "_EmissionColor" : "_EmissiveColor", Color.black); Col(f + 12, m, "_BurnColor", Color.white);
            if (std || (fl & FLmShader) != 0) f[11] = 1;   // Standard / Lightmapper emission ignores the colour's alpha
            ST(f + 16, m, ground ? "_TarmacTexture" : "_MainTex");
            if ((fl & (FDetail | FFlag)) != 0) { ST(f + 16, m, (fl & FFlag) != 0 ? "_DetailTex" : "_BaseTex"); ST(f + 24, m, "_MainTex"); Col(f + 4, m, "_DetailColor", Color.white); }   // base + detail, Standard lighting
            else if (std && !ground) { for (int i = 20; i < 32; i++) f[i] = f[16 + (i & 3)]; }   // Standard samples every map with _MainTex_ST
            else { ST(f + 20, m, ground ? "_BlendMaskTexture" : "_BumpMap"); ST(f + 24, m, (fl & FLmShader) != 0 ? "_Illum" : "_Emissive"); ST(f + 28, m, "_SpecMap"); }
            f[32] = Flt(m, (fl & FMapped) != 0 && !m.HasProperty(P("_Shininess")) ? "_Glossiness" : "_Shininess", 0.078125f);
            f[33] = Flt(m, "_Cutoff", 0.5f); f[34] = Flt(m, "_RimFalloff", 0.1f); f[35] = Flt(m, "_LightBoost", 1);
            f[36] = Flt(m, (fl & FIntSpace) != 0 ? "_LightAmbient" : "_AmbientMultiplier", 0); f[37] = Flt(m, "_SpecTint", 0); f[38] = Flt(m, "_Occlusion", 1); f[39] = 0;
            if ((fl & FIntSpace) != 0) { Col(f + 0, m, "_LightColor1", Color.white); Col(f + 8, m, "_LightColor2", Color.black); ST(f + 24, m, "_LightMap"); }
            if ((fl & FDetail) != 0) f[48] = f[49] = f[50] = f[51] = 0;
            else if (ground) { f[48] = Flt(m, "_NearGrassTiling", 1); f[49] = Flt(m, "_FarGrassTiling", 1); f[50] = Mathf.Max(1e-3f, Flt(m, "_FarGrassBlendDistance", 100)); f[51] = 0; }
            else if (std) { f[48] = Flt(m, "_Metallic", 0); f[49] = Flt(m, "_Glossiness", 0.5f); f[50] = Flt(m, "_GlossMapScale", 1); f[51] = Flt(m, "_OcclusionStrength", 1); }
            else f[48] = f[49] = f[50] = f[51] = 0;
            f[52] = Flt(m, "_BumpScale", 1); f[53] = Flt(m, "_MidGrassTiling", 1); f[54] = Mathf.Max(1e-3f, Flt(m, "_MidGrassBlendDistance", 50)); f[55] = 0;
            bool ready = true;
            for (int i = 0; i < 5; i++)
            {
                int id = rec.tex[i];
                int def = ground ? TexWhite : TexDefaults[i];
                if (id >= 0 && texState[id] == RsFailed) { rec.tex[i] = def; TexFallbacks++; id = def; }
                if (id < 0) id = def;
                fi[i < 4 ? 40 + i : 56] = id;
                ready &= texState[id] == RsReady;
            }
            fi[44] = fl; fi[45] = fi[46] = fi[47] = 0; fi[57] = fi[58] = fi[59] = 0;
            rec.ready = ready;
        }

        // property names -> ids once: the string overloads marshal and hash the name on every call (~40 per material)
        static readonly Dictionary<string, int> propIds = new Dictionary<string, int>();
        static int P(string n) { if (!propIds.TryGetValue(n, out int id)) propIds[n] = id = Shader.PropertyToID(n); return id; }

        static void Col(float* o, Material m, string n, Color def)
        {
            int id = P(n);
            Color c = m.HasProperty(id) ? m.GetColor(id) : def;
            o[0] = c.r; o[1] = c.g; o[2] = c.b; o[3] = c.a;
        }
        static void ST(float* o, Material m, string n)
        {
            Vector2 s = Vector2.one, t = Vector2.zero;
            int id = P(n);
            if (m.HasProperty(id)) { s = m.GetTextureScale(id); t = m.GetTextureOffset(id); }
            o[0] = s.x; o[1] = s.y; o[2] = t.x; o[3] = t.y;
        }
        static float Flt(Material m, string n, float def) { int id = P(n); return m.HasProperty(id) ? m.GetFloat(id) : def; }

        static readonly int idRim = Shader.PropertyToID("_RimColor"), idTemp = Shader.PropertyToID("_TemperatureColor"), idFall = Shader.PropertyToID("_RimFalloff"),
                            idOpa = Shader.PropertyToID("_Opacity"), idEmis = Shader.PropertyToID("_EmissiveColor");

        void ReadMpb(Entry e)
        {
            if (e.inst >= instCap) return;
            float* f = (float*)insts + e.inst * InstF;
            for (int i = 0; i < 16; i++) f[i] = 0;
            f[9] = 1;
            if (e.r == null || !e.r.HasPropertyBlock()) return;
            e.r.GetPropertyBlock(mpb);
            Color rim = mpb.GetColor(idRim), temp = mpb.GetColor(idTemp), em = mpb.GetColor(idEmis);
            float opa = mpb.GetFloat(idOpa);
            f[0] = rim.r; f[1] = rim.g; f[2] = rim.b; f[3] = rim.a;
            f[4] = temp.r; f[5] = temp.g; f[6] = temp.b; f[7] = temp.a;
            f[8] = mpb.GetFloat(idFall); f[9] = opa > 0 ? opa : 1;
            if (e.mcc || em.r != 0 || em.g != 0 || em.b != 0) { f[10] = 1; f[12] = em.r; f[13] = em.g; f[14] = em.b; f[15] = em.a; }
        }

        /// Static per-renderer data: lightmap scale-offset and texture, flags (1: no shadows received, 2: IVA interior, lit by IVASun).
        void WriteInst(Entry e)
        {
            if (e.inst >= instCap) return;
            float* f = (float*)insts + e.inst * InstF;
            Vector4 st = e.r != null ? e.r.lightmapScaleOffset : Vector4.zero;
            f[16] = st.x; f[17] = st.y; f[18] = st.z; f[19] = st.w;
            int* fi = (int*)(f + 20);
            fi[0] = e.lmTex; fi[1] = e.iva ? 2 : 0; fi[2] = Mask8(e.r ? 1 << e.r.gameObject.layer : 1); fi[3] = Compat.PbrOverride(e.part);
        }

        // ---------- hiding ----------
        // Cameras we don't composite into (craft thumbnails, the aero-FX camera and its velocity depth map) must still see the parts:
        // un-hide them for that camera's render. Internal-space cameras (portraits) are skipped.
        int exposed, exposedMask, hiddenLayers;
        readonly List<Camera> owned = new List<Camera>();
        const int InternalLayers = (1 << 16) | (1 << 20);
        bool Foreign(Camera c) => Active && (c.cullingMask & hiddenLayers) != 0 && (c.cullingMask & InternalLayers) == 0 && c.cameraType == CameraType.Game && !owned.Contains(c);
        void Expose(bool on, int mask)
        {
            long t0 = Tick();
            exposedMask = mask;
            foreach (var e in entries)
                if (e.hidden && !e.iva && (e.layerBit & mask) != 0 && e.r)
                    e.r.forceRenderingOff = !on;
            Prof[5] += Tick() - t0;
        }

        void Hide(Entry e)
        {
            XTrack(e);
            if (e.iva) { if (!e.mask) ivaHidden.Add(e); }
            else
            {
                // forceRenderingOff also drops the renderer from Unity's skinning and shadow maps; animators culled by visibility must keep running
                var h = e.r.GetComponent<VkHidden>();
                if (!h) h = e.r.gameObject.AddComponent<VkHidden>();
                h.owner = e.r.GetInstanceID(); h.mode = e.shadow; e.vh = h;
                if (e.smr != null) KeepAnimating(e.r);
                e.r.forceRenderingOff = true;
                hiddenLayers |= e.layerBit = 1 << e.r.gameObject.layer;
            }
            e.hidden = true;
        }

        readonly Dictionary<Component, int> animCull = new Dictionary<Component, int>();   // original culling of animators we set to AlwaysAnimate
        void KeepAnimating(Renderer r)
        {
            foreach (var a in r.GetComponentsInParent<Animator>(true))
                if (a.cullingMode != AnimatorCullingMode.AlwaysAnimate) { animCull[a] = (int)a.cullingMode; a.cullingMode = AnimatorCullingMode.AlwaysAnimate; }
            foreach (var a in r.GetComponentsInParent<Animation>(true))
                if (a.cullingType != AnimationCullingType.AlwaysAnimate) { animCull[a] = (int)a.cullingType; a.cullingType = AnimationCullingType.AlwaysAnimate; }
        }

        // Unity's shadow maps now only hold its leftovers (transparents and particles receive none): switch off the shadows of the lights on
        // the local layers so Unity skips shadow culling, cascades and the screen-space shadow pass. Vulkan keeps reading the original setting.
        static readonly bool KeepUnityShadows = Environment.GetEnvironmentVariable("KSPVK_UNITYSHADOWS") == "1";
        readonly Dictionary<Light, LightShadows> unityShadows = new Dictionary<Light, LightShadows>();
        LightShadows ShadowsOf(Light l) => unityShadows.TryGetValue(l, out var s) ? s : l.shadows;
        void RestoreShadows() { foreach (var kv in unityShadows) if (kv.Key) kv.Key.shadows = kv.Value; unityShadows.Clear(); }
        void TakeShadows()
        {
            if (KeepUnityShadows || !PartsOn || (FamMask >> FamPqs & 1) == 0) return;   // Unity still draws parts or terrain: keep its shadows
            foreach (var l in lights)
                if (l && l.shadows != LightShadows.None && (l.cullingMask & (localMask | ivaMask)) != 0 && !unityShadows.ContainsKey(l)) { unityShadows[l] = l.shadows; l.shadows = LightShadows.None; }
        }

        void Unhide(Entry e)
        {
            if (!e.hidden) return;
            e.hidden = false;
            XUntrack(e);
            if (e.iva) { ivaHidden.Remove(e); return; }
            if (e.r == null) return;
            e.r.forceRenderingOff = false;
        }

        public void RestoreAll()
        {
            if (exposed > 0) { exposed = 0; Expose(false, exposedMask); }
            foreach (var e in entries) Unhide(e);
            ivaHidden.Clear(); hiddenLayers = 0;
            RestoreWorld();
            XReset();
            RestoreShadows();
            foreach (var kv in animCull)
                if (kv.Key is Animator a && a) a.cullingMode = (AnimatorCullingMode)kv.Value;
                else if (kv.Key is Animation an && an) an.cullingType = (AnimationCullingType)kv.Value;
            animCull.Clear();
            entries.Clear(); byRenderer.Clear(); matSlots.Clear(); matRecs.Clear(); liveMats.Clear();
            Active = false; Cam = null; ivaCam = null; frame = 0; Hidden = Draws = 0;
            rootPos = -1; roots.Clear(); ignored.Clear(); instTop = 0; freeInst.Clear();
        }

        public Action<int> SetTexMax;
        int texMax = -1;

        void ResetNative(string why = "scene arena/table full")
        {
            Log.Line(why + ": resetting native scene");
            RestoreAll();
            reset();
            meshIds.Clear(); texIds.Clear(); ForgetWorldMeshes();
        }

        // ---------- diagnostics ----------
        /// Mesh renderers on the local camera's layers that Unity still draws, by reason (opaque ones are the residual).
        public Dictionary<string, int> Residuals(out int residual)
        {
            var res = new Dictionary<string, int>();
            residual = 0;
            int mask = localMask | ivaMask;
            if (mask == 0) return res;
            foreach (var r in UnityEngine.Object.FindObjectsOfType<Renderer>())
            {
                if ((mask >> r.gameObject.layer & 1) == 0 || !r.enabled || r.forceRenderingOff || r.shadowCastingMode == ShadowCastingMode.ShadowsOnly) continue;
                bool mesh = r is MeshRenderer || r is SkinnedMeshRenderer;
                if (mesh && r.GetComponent<PQ>() != null) continue;   // counted by WorldResiduals
                string why;
                byRenderer.TryGetValue(r, out var e);
                if (e != null && e.hidden) continue;   // IVA: hidden only while the InternalCamera renders
                if (!mesh) why = "fx: " + r.GetType().Name;
                else if (e != null) why = e.reason ?? "pending upload";
                else if (Compat.Excluded(r)) why = "stays in Unity: mod or API exclusion";
                else if (!PartsOn) why = "stays in Unity: objects family off";
                else why = "not scanned";
                bool opaque = mesh && !why.StartsWith("stays in Unity") && Array.TrueForAll(r.sharedMaterials, m => m != null && m.renderQueue < 2500);
                if (opaque) residual++;
                string k = (opaque ? "RESIDUAL " : "by design ") + why + " [L" + r.gameObject.layer + "]";
                res[k] = res.TryGetValue(k, out int c) ? c + 1 : 1;
            }
            return res;
        }
    }

    /// Marks a renderer KSPVk hid. Instantiate copies the hidden state into clones (editor Alt-copy, symmetry): the clone's Awake restores it.
    public class VkHidden : MonoBehaviour
    {
        public int owner; public ShadowCastingMode mode;
        [NonSerialized] public bool on;   // active in hierarchy (OnEnable fires inside AddComponent when it is)
        void OnEnable() => on = true;
        void OnDisable() => on = false;
        void Awake()
        {
            if (owner == 0) return;   // AddComponent on the original
            var r = GetComponent<Renderer>();
            if (r && r.GetInstanceID() != owner) { r.forceRenderingOff = false; r.shadowCastingMode = mode; Destroy(this); }
        }
    }
}
