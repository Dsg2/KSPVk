using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.LowLevel;
using Debug = UnityEngine.Debug;

namespace KSPVk.Probe
{
    /// Phase 0 profiler: per-PlayerLoop-subsystem main-thread timing, per-camera CPU time,
    /// per-camera GPU-timeline time (D3D11 timestamps via native plugin), plus scene dumps.
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public class Probe : MonoBehaviour
    {
        public static Probe Instance;
        public static string Root => KSPUtil.ApplicationRootPath;
        public static string PluginData => Path.Combine(Root, "GameData/KSPVk/PluginData");
        static readonly double TickMs = 1000.0 / Stopwatch.Frequency;
        const int GpuRing = 6, GpuSlots = 128;

        // Native exports are bound via GetProcAddress (Mono's DllImport search does not cover GameData paths).
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr module, string name);
        delegate int InitD3D11Fn(IntPtr nativeTexture);
        delegate void SetFrameFn(int ring, ulong frame);
        delegate IntPtr GetRenderEventFuncFn();
        delegate int GetGpuFrameFn(int ring, out ulong frame, [Out] double[] ms, int max);
        static SetFrameFn KSPVk_SetFrame;
        static GetGpuFrameFn KSPVk_GetGpuFrame;
        static T Bind<T>(IntPtr lib, string name) where T : class
        {
            IntPtr p = GetProcAddress(lib, name);
            if (p == IntPtr.Zero) throw new Exception("missing export " + name);
            return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
        }

        string[] sysNames;
        long[] sysBegin, sysAcc;
        int presentId = -1;
        long frameStart;
        ulong frameIdx;
        readonly Dictionary<Camera, long> camStart = new Dictionary<Camera, long>();
        readonly Dictionary<string, double> camCpuFrame = new Dictionary<string, double>();
        readonly Dictionary<ulong, List<string>> gpuLabels = new Dictionary<ulong, List<string>>();
        List<string> curLabels;
        IntPtr renderEvent;
        bool gpuOk;
        readonly double[] gpuBuf = new double[GpuSlots];

        public Capture Current;
        public string Status = "idle";
        public bool Uncapped;
        Texture2D gpuProbeTex;

        void Awake()
        {
            if (Instance) { Destroy(this); return; }
            Instance = this;
            DontDestroyOnLoad(gameObject);
            InstallPlayerLoop();
            Camera.onPreCull += OnCamPreCull;
            Camera.onPostRender += OnCamPostRender;
            InitGpu();
            string bench = Environment.GetCommandLineArgs().FirstOrDefault(a => a.StartsWith("-kspvk-bench"));
            if (bench != null) StartCoroutine(Bench.Run(this, bench.Contains("=") ? bench.Substring(bench.IndexOf('=') + 1) : "run"));
            Debug.Log($"[KSPVk.Probe] ready. gfx={SystemInfo.graphicsDeviceType} gpuTimer={gpuOk} bench={bench ?? "off"}");
        }

        void InitGpu()
        {
            try
            {
                if (SystemInfo.graphicsDeviceType != UnityEngine.Rendering.GraphicsDeviceType.Direct3D11) return;
                string dll = Path.GetFullPath(Path.Combine(PluginData, "KSPVkNative.dll")).Replace('/', '\\');
                IntPtr lib = LoadLibraryW(dll);
                if (lib == IntPtr.Zero) throw new Exception("LoadLibrary(" + dll + ") failed: " + Marshal.GetLastWin32Error());
                KSPVk_SetFrame = Bind<SetFrameFn>(lib, "KSPVk_SetFrame");
                KSPVk_GetGpuFrame = Bind<GetGpuFrameFn>(lib, "KSPVk_GetGpuFrame");
                gpuProbeTex = new Texture2D(1, 1); gpuProbeTex.Apply();
                int rc = Bind<InitD3D11Fn>(lib, "KSPVk_InitD3D11")(gpuProbeTex.GetNativeTexturePtr());
                gpuOk = rc == 1;
                if (!gpuOk) throw new Exception("InitD3D11 returned " + rc);
                renderEvent = Bind<GetRenderEventFuncFn>(lib, "KSPVk_GetRenderEventFunc")();
            }
            catch (Exception e) { Debug.LogWarning("[KSPVk.Probe] GPU timer unavailable: " + e.Message); gpuOk = false; }
        }

        // ---------- PlayerLoop instrumentation ----------
        void InstallPlayerLoop()
        {
            var root = PlayerLoop.GetCurrentPlayerLoop();
            var top = root.subSystemList;
            var names = new List<string>();
            for (int i = 0; i < top.Length; i++)
            {
                var subs = top[i].subSystemList;
                if (subs == null) continue;
                var list = new List<PlayerLoopSystem>();
                if (i == 0) list.Add(Marker(-1));
                foreach (var s in subs)
                {
                    int id = names.Count;
                    names.Add(top[i].type.Name + "." + s.type.Name);
                    if (s.type.Name == "PresentAfterDraw") presentId = id;
                    list.Add(Marker(2 * id)); list.Add(s); list.Add(Marker(2 * id + 1));
                }
                top[i].subSystemList = list.ToArray();
            }
            root.subSystemList = top;
            PlayerLoop.SetPlayerLoop(root);
            sysNames = names.ToArray();
            sysBegin = new long[names.Count];
            sysAcc = new long[names.Count];
        }

        PlayerLoopSystem Marker(int code) => new PlayerLoopSystem { type = typeof(Probe), updateDelegate = () => OnMarker(code) };

        void OnMarker(int code)
        {
            long t = Stopwatch.GetTimestamp();
            if (code < 0) { OnFrameStart(t); return; }
            int id = code >> 1;
            if ((code & 1) == 0)
            {
                sysBegin[id] = t;
                if (id == presentId && gpuOk && curLabels != null)   // all rendering for this frame has been issued
                {
                    GL.IssuePluginEvent(renderEvent, (3 << 24) | ((int)(frameIdx % GpuRing) << 16) | curLabels.Count);
                    curLabels.Add("end");
                    curLabels = null;
                }
            }
            else sysAcc[id] += t - sysBegin[id];
        }

        void OnFrameStart(long t)
        {
            if (frameStart != 0 && Current != null && Current.Active)
                Current.AddFrame((t - frameStart) * TickMs, sysAcc, TickMs, camCpuFrame);
            Array.Clear(sysAcc, 0, sysAcc.Length);
            camCpuFrame.Clear();
            frameStart = t;
            frameIdx++;
            if (Uncapped) { Application.targetFrameRate = -1; QualitySettings.vSyncCount = 0; }
            if (!gpuOk) return;
            for (int r = 0; r < GpuRing; r++)
            {
                int n = KSPVk_GetGpuFrame(r, out ulong f, gpuBuf, GpuSlots);
                if (n != 0 && gpuLabels.TryGetValue(f, out var labels))
                {
                    if (n > 0 && Current != null && Current.Contains(f)) Current.AddGpu(gpuBuf, n, labels);
                    gpuLabels.Remove(f);
                }
            }
            if (gpuLabels.Count > 64) gpuLabels.Remove(gpuLabels.Keys.Min());
            int ring = (int)(frameIdx % GpuRing);
            KSPVk_SetFrame(ring, frameIdx);
            curLabels = new List<string> { "begin" };
            gpuLabels[frameIdx] = curLabels;
            GL.IssuePluginEvent(renderEvent, (1 << 24) | (ring << 16));
        }

        void GpuMark(string label)
        {
            if (!gpuOk || curLabels == null || curLabels.Count >= GpuSlots - 1) return;
            GL.IssuePluginEvent(renderEvent, (2 << 24) | ((int)(frameIdx % GpuRing) << 16) | curLabels.Count);
            curLabels.Add(label);
        }

        void OnCamPreCull(Camera c) { camStart[c] = Stopwatch.GetTimestamp(); GpuMark(">" + c.name); }

        void OnCamPostRender(Camera c)
        {
            if (camStart.TryGetValue(c, out long s))
            {
                camCpuFrame.TryGetValue(c.name, out double v);
                camCpuFrame[c.name] = v + (Stopwatch.GetTimestamp() - s) * TickMs;
            }
            GpuMark("<" + c.name);
        }

        // ---------- capture ----------
        public IEnumerator Measure(Capture cap, float seconds)
        {
            cap.SysNames = sysNames;
            cap.FirstFrame = frameIdx + 1;
            Current = cap;
            cap.Active = true;
            yield return new WaitForSecondsRealtime(seconds);
            cap.Active = false;
            cap.LastFrame = frameIdx;
            for (int i = 0; i < GpuRing + 4; i++) yield return null;   // let GPU results drain
            Current = null;
            cap.GpuTimer = gpuOk;
        }

        void Update()
        {
            // Manual capture in any scene: Ctrl+Alt+F9 (10 s)
            if (Input.GetKey(KeyCode.LeftControl) && Input.GetKey(KeyCode.LeftAlt) && Input.GetKeyDown(KeyCode.F9) && Current == null)
                StartCoroutine(Manual());
        }

        IEnumerator Manual()
        {
            string dir = Path.Combine(PluginData, "bench/manual_" + DateTime.Now.ToString("yyyyMMdd_HHmmss"));
            Directory.CreateDirectory(dir);
            var cap = new Capture(HighLogic.LoadedScene.ToString());
            ScreenMessages.PostScreenMessage("[KSPVk] capturing 10 s…", 3f);
            yield return Measure(cap, 10f);
            Report.WriteStep(dir, cap);
            ScreenMessages.PostScreenMessage("[KSPVk] capture written to " + dir, 5f);
        }

        void OnGUI()
        {
            if (Status != "idle") GUI.Label(new Rect(8, 8, 900, 24), "[KSPVk bench] " + Status);
        }
    }

    public class Capture
    {
        public readonly string Name;
        public string[] SysNames;
        public bool Active, GpuTimer;
        public ulong FirstFrame, LastFrame = ulong.MaxValue;
        public readonly List<double> FrameMs = new List<double>();
        public double[] SysSumMs;
        public readonly Dictionary<string, List<double>> CamCpu = new Dictionary<string, List<double>>();
        public readonly Dictionary<string, double> CamGpuSum = new Dictionary<string, double>();
        public readonly Dictionary<string, int> CamGpuCalls = new Dictionary<string, int>();
        public readonly List<double> GpuSpan = new List<double>();
        public int GpuFrames;

        public Capture(string name) { Name = name; }
        public bool Contains(ulong f) => f >= FirstFrame && f <= LastFrame;

        public void AddFrame(double ms, long[] acc, double tickMs, Dictionary<string, double> cams)
        {
            if (SysSumMs == null) SysSumMs = new double[acc.Length];
            FrameMs.Add(ms);
            for (int i = 0; i < acc.Length; i++) SysSumMs[i] += acc[i] * tickMs;
            foreach (var kv in cams)
            {
                if (!CamCpu.TryGetValue(kv.Key, out var l)) CamCpu[kv.Key] = l = new List<double>();
                l.Add(kv.Value);
            }
        }

        public void AddGpu(double[] ms, int n, List<string> labels)
        {
            int m = Math.Min(n, labels.Count);
            if (m < 2 || labels[m - 1] != "end" || ms[m - 1] < 0) return;
            GpuFrames++;
            GpuSpan.Add(ms[m - 1]);
            var open = new Dictionary<string, double>();
            for (int i = 1; i < m - 1; i++)
            {
                if (ms[i] < 0) continue;
                string cam = labels[i].Substring(1);
                if (labels[i][0] == '>') open[cam] = ms[i];
                else if (open.TryGetValue(cam, out double s))
                {
                    CamGpuSum.TryGetValue(cam, out double v); CamGpuSum[cam] = v + (ms[i] - s);
                    CamGpuCalls.TryGetValue(cam, out int c); CamGpuCalls[cam] = c + 1;
                    open.Remove(cam);
                }
            }
        }
    }
}
