using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.LowLevel;
using UnityEngine.Rendering;

namespace KSPVk
{
    /// Drives the native Vulkan core once per frame and composites its output into the eligible Unity cameras
    /// (BeforeForwardOpaque) with per-camera depth re-projection. UI cameras are untouched.
    /// The Scene extractor hands opaque objects to Vulkan: the local view (flight, KSC, editors, main menu, IVA) plus the scaled and galaxy views.
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public class Core : MonoBehaviour
    {
        public const int MaxCams = 16, EvFrame = 1, EvComposite = 2, EvEnd = 3;
        public const int StateNone = 0, StateOk = 1, StateFailed = 2;
        // 3D layers Vulkan content can appear in: Default, TransparentFX, Water, Local/Scaled Scenery, Kerbals, Internal Space, Sky, PartsList, etc.
        const int Layers3D = (1 << 0) | (1 << 1) | (1 << 4) | (1 << 9) | (1 << 10) | (1 << 15) | (1 << 16) | (1 << 17) | (1 << 18) | (1 << 19) | (1 << 20) | (1 << 23) | (1 << 28) | (1 << 30);
        const int SkyLayer = 1 << 18;

        public static Core Instance;
        public bool Enabled = true, DebugPattern;
        public Scene Scene;
        public long KickTicks;   // Stopwatch ticks spent in Kick (main thread), cumulative
        public int State => native != null ? Stats().state : StateNone;
        public static string PluginData => Path.Combine(KSPUtil.ApplicationRootPath, "GameData/KSPVk/PluginData");

        [StructLayout(LayoutKind.Sequential, Pack = 4)]
        public struct NativeStats { public ulong begun, submitted, skipped, composites, compositeSkips; public float vkGpuMs; public int state, width, height, resizes; public ulong gpuWaits, emptyFrames; public float vkPrepMs; public double subWaitMs; }

        // ---------- native binding (GetProcAddress: Mono's DllImport search does not cover GameData) ----------
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr module, string name);
        delegate int InitFn(IntPtr nativeTex);
        delegate long BeginFrameFn(IntPtr frameParams, IntPtr scene);
        delegate IntPtr GetPtrFn();
        delegate void GetStatsFn(out NativeStats s);
        delegate int PopLogFn(IntPtr buf, int len);
        delegate void VoidFn();
        delegate void SetIntFn(int v);
        [StructLayout(LayoutKind.Sequential, Pack = 4)]
        struct EnhIn { public int flags, tonemap; public float ev, evRange, key, bloom, threshold, taaWeight, ambFill, iblScale, sunSoft, aoStrength, giStrength, skyBright, renderScale; }
        delegate void SetEnhFn(ref EnhIn e);
        delegate int GetEnhFn([Out] float[] v, int n);
        class Native { public BeginFrameFn BeginFrame; public GetStatsFn GetStats; public PopLogFn PopLog; public VoidFn Shutdown; public SetIntFn SetCompositeMode, SetSceneDebug; public IntPtr EventFunc;
                       public SetEnhFn SetEnhanced; public GetEnhFn GetEnhanced; }
        EnhIn enhSent; bool enhInit;
        Native native;

        static T Bind<T>(IntPtr lib, string name) where T : class
        {
            IntPtr p = GetProcAddress(lib, name);
            if (p == IntPtr.Zero) throw new Exception("missing export " + name);
            return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
        }

        class Slot { public Camera cam; public CommandBuffer cb; public bool seen; }
        readonly Slot[] slots = new Slot[MaxCams];
        readonly Dictionary<Camera, int> slotOf = new Dictionary<Camera, int>();
        Camera[] camBuf = new Camera[32];
        IntPtr frameParams, logBuf;
        const int LogLen = 8192;
        bool frameBegun;
        Texture2D initTex;

        void Awake()
        {
            if (Instance) { Destroy(this); return; }
            Instance = this;
            DontDestroyOnLoad(gameObject);
            gameObject.AddComponent<SettingsWindow>();
            frameParams = Marshal.AllocHGlobal(16 + MaxCams * 16);
            logBuf = Marshal.AllocHGlobal(LogLen);
            Log.Open();
            int unclean = Log.BeginSession();
            if (unclean > 0) Log.Line($"previous session ended without a clean quit ({unclean} in a row)", true);
            if (unclean >= 2 && !Settings.Bench) { Enabled = false; Notice("KSPVk is off: KSP closed unexpectedly twice in a row. " + (Settings.ToggleKey != "None" ? Settings.ToggleKey + " or " : "") + "the KSPVk window turns it back on (KSPVk.log has details)."); }
            try { Init(); }
            catch (Exception e) { Log.Line("init failed, running the stock renderer: " + e.Message, true); native = null; Notice("KSPVk could not start; KSP uses its stock renderer (see GameData/KSPVk/PluginData/KSPVk.log)."); }
            if (native != null) InstallPlayerLoop();
            DrainLog();
        }

        void Init()
        {
            if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11) throw new Exception("needs Direct3D11, got " + SystemInfo.graphicsDeviceType);
            string dll = Path.GetFullPath(Path.Combine(PluginData, "KSPVkNative.dll")).Replace('/', '\\');
            IntPtr lib = LoadLibraryW(dll);
            if (lib == IntPtr.Zero) throw new Exception("LoadLibrary(" + dll + ") failed: " + Marshal.GetLastWin32Error());
            var n = new Native
            {
                BeginFrame = Bind<BeginFrameFn>(lib, "KSPVk_BeginFrame2"),
                SetSceneDebug = Bind<SetIntFn>(lib, "KSPVk_SetSceneDebug"),
                GetStats = Bind<GetStatsFn>(lib, "KSPVk_GetStats"),
                PopLog = Bind<PopLogFn>(lib, "KSPVk_PopLog"),
                Shutdown = Bind<VoidFn>(lib, "KSPVk_Shutdown"),
                SetCompositeMode = Bind<SetIntFn>(lib, "KSPVk_SetCompositeMode"),
                EventFunc = Bind<GetPtrFn>(lib, "KSPVk_GetEventFunc")(),
                SetEnhanced = Bind<SetEnhFn>(lib, "KSPVk_SetEnhanced"),
                GetEnhanced = Bind<GetEnhFn>(lib, "KSPVk_GetEnhanced"),
            };
            initTex = new Texture2D(1, 1); initTex.Apply();
            native = n;   // assigned first so DrainLog can report native init errors
            int rc = Bind<InitFn>(lib, "KSPVk_Init")(initTex.GetNativeTexturePtr());
            if (rc != 1) { DrainLog(); throw new Exception("KSPVk_Init returned " + rc); }
            Scene = new Scene(Bind<Scene.RegisterFn>(lib, "KSPVk_RegisterMesh"), Bind<Scene.RegisterFn>(lib, "KSPVk_RegisterTexture"), Bind<Scene.PumpFn>(lib, "KSPVk_Pump"),
                              Bind<Scene.StatePtrsFn>(lib, "KSPVk_GetStatePtrs"), Bind<VoidFn>(lib, "KSPVk_ResetScene").Invoke, Bind<Scene.SceneStatsFn>(lib, "KSPVk_GetSceneStats"));
            Scene.BindWorld(Bind<Scene.RegisterFn>(lib, "KSPVk_RegisterMeshCPU"), Bind<Scene.SharedFn>(lib, "KSPVk_SetSharedIndices"), Bind<Scene.ReleaseFn>(lib, "KSPVk_ReleaseMesh"));
            Scene.SetTexMax = Bind<Scene.ReleaseFn>(lib, "KSPVk_SetTexMax").Invoke;
            Log.Line("core ready");
        }

        /// Diagnostics: 0 tiled composite (default), 1 fence wait only, 2 tiled without fence wait, 3 full-screen composite, 4 skip.
        public void SetCompositeMode(int m) => native?.SetCompositeMode(m);

        public NativeStats Stats() { var s = default(NativeStats); native?.GetStats(out s); return s; }

        /// Enhanced mode: v (13) = pass GPU ms (IBL, pre-scene, main, Hi-Z, AO / GI, TAA, exposure, bloom, tonemap), exposure, mean log2 luminance, image MB,
        /// failed flags. Returns the active flags.
        public int Enhanced(float[] v) => native != null ? native.GetEnhanced(v, v.Length) : 0;

        void SendEnhanced()
        {
            var e = new EnhIn { flags = Settings.Enh, tonemap = Settings.Tonemap, ev = Settings.Ev, evRange = Settings.EvRange, key = Settings.ExpKey,
                                bloom = Settings.Bloom, threshold = Settings.BloomThreshold, taaWeight = Settings.TaaWeight, ambFill = Settings.AmbFill, iblScale = Settings.IblScale,
                                sunSoft = Settings.SunSoft, aoStrength = Settings.AoStrength, giStrength = Settings.GiStrength, skyBright = Settings.SkyBright, renderScale = Settings.RenderScale };
            if (enhInit && e.Equals(enhSent)) return;
            native.SetEnhanced(ref e); enhSent = e; enhInit = true;
        }

        /// Diagnostics for the scene renderer: 1 flip winding, 2 no shadows, 4 no frustum culling.
        public void SetSceneDebug(int f) => native?.SetSceneDebug(f);

        // ---------- PlayerLoop: transform job after UpdateAllRenderers, Kick before FinishFrameRendering (cameras render inside it), End after ----------
        void InstallPlayerLoop()
        {
            var root = PlayerLoop.GetCurrentPlayerLoop();
            var top = root.subSystemList;
            for (int i = 0; i < top.Length; i++)
            {
                if (top[i].type?.Name != "PostLateUpdate") continue;
                var list = new List<PlayerLoopSystem>(top[i].subSystemList);
                int f = list.FindIndex(s => s.type?.Name == "FinishFrameRendering");
                if (f < 0) throw new Exception("FinishFrameRendering not found");
                list.Insert(f + 1, new PlayerLoopSystem { type = typeof(Core), updateDelegate = End });
                list.Insert(f, new PlayerLoopSystem { type = typeof(Core), updateDelegate = Kick });
                int r = list.FindIndex(s => s.type?.Name == "UpdateAllRenderers");   // renderer updates read the hierarchies the job locks
                list.Insert(r + 1, new PlayerLoopSystem { type = typeof(Scene), updateDelegate = () => { try { if (Enabled) Scene?.EarlySchedule(); } catch (Exception e) { Fault(e); } } });
                top[i].subSystemList = list.ToArray();
            }
            root.subSystemList = top;
            PlayerLoop.SetPlayerLoop(root);
        }

        static bool Eligible(Camera c) =>
            c && c.targetTexture == null && !c.orthographic && (c.cullingMask & Layers3D) != 0 && c.cameraType == CameraType.Game;

        void Kick()
        {
            long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
            try { KickInner(); }
            catch (Exception e) { frameBegun = false; Fault(e); }
            KickTicks += System.Diagnostics.Stopwatch.GetTimestamp() - t0;
        }

        void KickInner()
        {
            frameBegun = false;
            if (native == null) return;
            if (!Compat.Applied) { Compat.Apply(); if (Compat.AllOff && Compat.Applied) { Enabled = false; Notice("KSPVk is off: an installed mod is not compatible (" + string.Join("; ", Compat.Notes) + ")."); } }
            API.Tick();
            SendEnhanced();
            if (faultAt > 0 && Time.frameCount >= faultAt && faultLeft-- > 0) throw new Exception("fault self-test (KSPVK_FAULTTEST)");
            bool active = Enabled && State == StateOk && Settings.SceneOn();
            for (int i = 0; i < MaxCams; i++) if (slots[i] != null) slots[i].seen = false;
            if (active)
            {
                if (Camera.allCamerasCount > camBuf.Length) camBuf = new Camera[Camera.allCamerasCount * 2];
                int n = Camera.GetAllCameras(camBuf);
                for (int i = 0; i < n; i++)
                {
                    Camera c = camBuf[i];
                    if (!Eligible(c)) continue;
                    if (!slotOf.TryGetValue(c, out int s)) s = Attach(c);
                    if (s >= 0) slots[s].seen = true;
                }
                Array.Clear(camBuf, 0, n);
            }
            for (int i = 0; i < MaxCams; i++) if (slots[i] != null && !slots[i].seen) Detach(i);
            int draws = 0;
            IntPtr scene = Scene != null ? Scene.Build(active, out draws) : IntPtr.Zero;
            if (!active) return;
            Camera near = scene != IntPtr.Zero && draws > 0 ? Scene.CompositeCam : null;
            bool world = scene != IntPtr.Zero && Scene.WorldDraws > 0;
            Camera scaled = world ? Scene.ScaledCam : null, galaxy = world ? Scene.GalaxyCam : null;

            unsafe
            {
                int* hdr = (int*)frameParams;
                hdr[0] = Screen.width; hdr[1] = Screen.height; hdr[2] = DebugPattern ? 1 : 0; hdr[3] = MaxCams;
                float* cp = (float*)(frameParams + 16);
                for (int i = 0; i < MaxCams; i++, cp += 4)
                {
                    Camera c = slots[i]?.cam;
                    if (c == null) { ((int*)cp)[3] = -1; continue; }
                    Matrix4x4 p = GL.GetGPUProjectionMatrix(c.projectionMatrix, true);
                    bool sky = (c.cullingMask & ~SkyLayer) == 0;
                    cp[0] = p.m22; cp[1] = p.m23;
                    // modes: 0 galaxy layer, 1 local slice, 2 scaled slice, 3 everything (debug pattern behind the sky camera), 4 IVA only (InternalCamera)
                    if (DebugPattern) { cp[2] = sky ? 0f : ScaledCamera.Instance && ScaledCamera.Instance.cam == c ? ScaledSpace.InverseScaleFactor : 1f; ((int*)cp)[3] = sky ? 3 : 1; }
                    else { cp[2] = 1f; ((int*)cp)[3] = c == near ? 1 : c == scaled ? 2 : c == galaxy ? 0 : near && c == Scene.IvaCam ? 4 : -1; }
                }
            }
            if (native.BeginFrame(frameParams, scene) > 0)
            {
                GL.IssuePluginEvent(native.EventFunc, EvFrame << 24);
                frameBegun = true;
            }
        }

        void End()
        {
            if (frameBegun) GL.IssuePluginEvent(native.EventFunc, EvEnd << 24);
            frameBegun = false;
        }

        int Attach(Camera c)
        {
            int s = Array.FindIndex(slots, x => x == null);
            if (s < 0) return -1;
            var cb = new CommandBuffer { name = "KSPVk composite" };
            cb.IssuePluginEvent(native.EventFunc, (EvComposite << 24) | s);
            c.AddCommandBuffer(CameraEvent.BeforeForwardOpaque, cb);
            slots[s] = new Slot { cam = c, cb = cb, seen = true };
            slotOf[c] = s;
            Log.Line($"camera '{c.name}' -> slot {s} (depth {c.depth}, mask {c.cullingMask:x}, clear {c.clearFlags}, near {c.nearClipPlane}, far {c.farClipPlane})");
            return s;
        }

        void Detach(int s)
        {
            var sl = slots[s];
            if (sl.cam) sl.cam.RemoveCommandBuffer(CameraEvent.BeforeForwardOpaque, sl.cb);
            sl.cb.Release();
            foreach (var kv in slotOf) if (kv.Value == s) { slotOf.Remove(kv.Key); break; }
            slots[s] = null;
        }

        void Update()
        {
            if (pendingNotice != null && ScreenMessages.Instance != null) { ScreenMessages.PostScreenMessage(pendingNotice, 12f, ScreenMessageStyle.UPPER_CENTER); pendingNotice = null; }
            if (native == null) return;
            if (!failNoticed && State == StateFailed) { failNoticed = true; Log.Line("native renderer stopped; KSP is back on its stock renderer", true); Notice("KSPVk stopped after a Vulkan error; KSP uses its stock renderer (see KSPVk.log)."); }
            if (Input.GetKey(KeyCode.LeftControl) && Input.GetKey(KeyCode.LeftAlt) && Input.GetKeyDown(KeyCode.F10)) { DebugPattern = !DebugPattern; ScreenMessages.PostScreenMessage("[KSPVk] depth test pattern " + (DebugPattern ? "on" : "off"), 2f); }
            if (!SettingsWindow.Capturing && Settings.ToggleKeyDown()) { Enabled = !Enabled; ScreenMessages.PostScreenMessage("[KSPVk] " + (Enabled ? "enabled" : "disabled"), 2f); }
            DrainLog();
        }

        void DrainLog()
        {
            if (native == null) return;
            if (native.PopLog(logBuf, LogLen) <= 0) return;
            foreach (var line in Marshal.PtrToStringAnsi(logBuf).Split('\n'))
                if (line.Length > 0) Log.Native(line);
        }

        void OnApplicationQuit()
        {
            Log.EndSession();
            if (native == null) return;
            Scene?.RestoreAll();
            for (int i = 0; i < MaxCams; i++) if (slots[i] != null) Detach(i);
            native.Shutdown();
            DrainLog();
            native = null;
        }

        // ---------- crash safety ----------
        string pendingNotice; bool failNoticed; int faults;
        // KSPVK_FAULTTEST=<frame>,<count>: throw from Kick on <count> frames starting at <frame> (tests the fallback path)
        static readonly int[] faultTest = (Environment.GetEnvironmentVariable("KSPVK_FAULTTEST") ?? "").Split(',').Select(x => int.TryParse(x, out int v) ? v : 0).ToArray();
        int faultAt = faultTest.Length == 2 ? faultTest[0] : 0, faultLeft = faultTest.Length == 2 ? faultTest[1] : 0;
        void Notice(string s) { pendingNotice = s; Log.Line("notice: " + s, true); }

        /// Any exception on a KSPVk code path: hand everything back to Unity for this frame (the next frame rebuilds from scratch); off after 3.
        public static void Fault(Exception e)
        {
            var c = Instance;
            Log.Line("exception: " + e, true);
            if (c == null) return;
            try { c.Scene?.RestoreAll(); } catch (Exception e2) { Log.Line("restore failed: " + e2.Message, true); }
            if (++c.faults >= 3 && c.Enabled) { c.Enabled = false; c.Notice("KSPVk turned itself off after repeated errors; KSP uses its stock renderer (see KSPVk.log)."); }
        }
    }
}
